#include "Replication/GameModel/CrowdyGameModelSubsystem.h"

#include "CrowdyCppClient.h"
#include "CrowdyGameModelLog.h"
#include "Data/CrowdyContainerManifest.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h"
#include "TimerManager.h"
#include "UObject/Package.h"

namespace
{
	// The client hands back the raw `data` object; the codec parsers read Envelope->data.
	TSharedPtr<FJsonObject> ManifestWrapDataEnvelope(const TSharedPtr<FJsonObject>& DataObject)
	{
		TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
		if (DataObject.IsValid())
		{
			Envelope->SetObjectField(TEXT("data"), DataObject);
		}
		return Envelope;
	}

	FCrowdyManifestApplyOutcome ManifestFailure(const TCHAR* Code, const FString& Message, bool bRetryable)
	{
		FCrowdyManifestApplyOutcome Outcome;
		Outcome.Code = Code;
		Outcome.Message = Message;
		Outcome.bRetryable = bRetryable;
		return Outcome;
	}

	// The allowance refusal is decided at the server's gate before the ensure runs, so repeating it is safe; a
	// transport loss never reached the server either. A policy refusal is final.
	bool ManifestOutcomeIsRetryable(const FCrowdyCppJsonResult& R)
	{
		return R.ErrorCode.Equals(TEXT("RATE_LIMITED"), ESearchCase::IgnoreCase) || R.ErrorCode.IsEmpty();
	}
}

void UCrowdyGameModelSubsystem::ApplyContainerManifest(const UCrowdyContainerManifest* Manifest, const FString& SessionId,
	bool bIgnoreActiveSession, TFunction<void(const FCrowdyApplyManifestResult&)> OnDone)
{
	auto FailNow = [&OnDone](const TCHAR* Code, const FString& Message, bool bRetryable)
	{
		FCrowdyApplyManifestResult Result;
		Result.Failed = 1;
		FCrowdyManifestRowFailure& Failure = Result.Failures.AddDefaulted_GetRef();
		Failure.Code = Code;
		Failure.Message = Message;
		Failure.bRetryable = bRetryable;
		if (OnDone) { OnDone(Result); }
	};

	if (!IsValid(Manifest))
	{
		FailNow(TEXT("no_manifest"), TEXT("No container manifest was given."), false);
		return;
	}
	if (bShuttingDown)
	{
		FailNow(TEXT("shutdown"), TEXT("The world is tearing down."), false);
		return;
	}
	FString Endpoint, Token;
	int64 AppId = 0;
	if (!ResolveApiContext(Endpoint, Token, AppId))
	{
		FailNow(TEXT("no_token"), TEXT("Not signed in to the Game API yet."), true);
		return;
	}
	const FString ResolvedSession = bIgnoreActiveSession ? SessionId : ResolveSessionId(SessionId, ActiveSessionId);
	const TWeakObjectPtr<UCrowdyGameModelSubsystem> WeakThis(this);

	// Scope is resolved per type up front: a pre-seed usually runs before anything of that type has registered.
	int32 AppScopedRows = 0;
	for (const FCrowdyContainerManifestRow& Row : Manifest->Rows)
	{
		EnsureContainerScopeKnown(Row.TypeName);
		AppScopedRows += IsContainerTypeAppScoped(Row.TypeName) ? 1 : 0;
	}

	if (const UWorld* World = GetWorld())
	{
		const FString WorldPackage = World->GetPackage() ? UWorld::RemovePIEPrefix(World->GetPackage()->GetName()) : FString();
		UE_CLOG(!WorldPackage.IsEmpty() && !Manifest->MapPackage.IsEmpty() && WorldPackage != Manifest->MapPackage,
			LogCrowdyGameModel, Warning,
			TEXT("[GameModel] manifest %s was scanned from %s but this world is %s; its rows will be created under another map's placement keys."),
			*Manifest->GetName(), *Manifest->MapPackage, *WorldPackage);
	}

	// One ensure per row, the same call the entity's own bind makes, so a pre-created row is exactly the row that
	// bind will find. The endpoint and bearer are resolved per row: a long apply outlives a token refresh or a
	// datacenter move, and a captured pair would replay the stale one on every row.
	FCrowdyManifestApplyRunner::FSend Send = [WeakThis, ResolvedSession]
		(const FCrowdyContainerManifestRow& Row, FCrowdyManifestApplyRunner::FOnRowDone RowDone)
	{
		UCrowdyGameModelSubsystem* S = ResolveLiveSelf(WeakThis);
		if (!S)
		{
			RowDone(ManifestFailure(TEXT("shutdown"), TEXT("The world tore down before the row was sent."), false));
			return;
		}
		FString RowEndpoint, RowToken;
		int64 RowAppId = 0;
		if (!S->ResolveApiContext(RowEndpoint, RowToken, RowAppId))
		{
			RowDone(ManifestFailure(TEXT("no_token"), TEXT("The Game API sign-in went away mid-apply."), true));
			return;
		}
		FCrowdyCppClient* Client = S->EnsureCppClient(RowEndpoint, RowToken);
		if (!Client)
		{
			RowDone(ManifestFailure(TEXT("no_client"), TEXT("No Game API client is available."), true));
			return;
		}
		const FString RowSession = S->IsContainerTypeAppScoped(Row.TypeName) ? FString() : ResolvedSession;
		const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildEnsureContainerVariables(
			RowAppId, Row.TypeName, Row.BindingKey, Row.DisplayName, RowSession, FString());
		const FString TypeName = Row.TypeName;
		Client->RunRuntimeOp(TEXT("GameModelEnsureContainer"), Vars,
			[WeakThis, TypeName, RowDone = MoveTemp(RowDone)](FCrowdyCppJsonResult R) mutable
		{
			if (UCrowdyGameModelSubsystem* Live = ResolveLiveSelf(WeakThis))
			{
				Live->ReportContainerRefusal(R.ErrorCode, TypeName, R.ErrorMessage);
			}
			FCrowdyManifestApplyOutcome Outcome;
			FString ContainerId;
			int64 OwnerUserId = 0;
			Outcome.bOk = FCrowdyGameApiCodec::ParseEnsureContainerEnvelope(ManifestWrapDataEnvelope(R.Data),
				R.bTransportOk, TArray<FString>(), ContainerId, OwnerUserId, Outcome.bCreated);
			if (!Outcome.bOk)
			{
				Outcome.Code = R.ErrorCode.IsEmpty() ? TEXT("transport") : R.ErrorCode;
				Outcome.Message = R.ErrorMessage;
				Outcome.bRetryable = ManifestOutcomeIsRetryable(R);
			}
			RowDone(Outcome);
		});
	};

	TSharedPtr<FCrowdyManifestApplyRunner> Runner = MakeShared<FCrowdyManifestApplyRunner>(Manifest->Rows,
		FCrowdyManifestApplyRunner::DefaultMaxInFlight, MoveTemp(Send),
		[WeakThis, AppScopedRows, OnDone = MoveTemp(OnDone)](const FCrowdyApplyManifestResult& RunnerResult)
	{
		// Runs once. The runner leaves the live list first; the caller's callback is last so it may start another.
		if (UCrowdyGameModelSubsystem* S = ResolveLiveSelf(WeakThis))
		{
			S->ManifestApplies.RemoveAll([](const TSharedPtr<FCrowdyManifestApplyRunner>& Entry) { return !Entry.IsValid() || Entry->IsDone(); });
		}
		FCrowdyApplyManifestResult Result = RunnerResult;
		Result.AppScoped = AppScopedRows;
		UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Log,
			TEXT("[GameModel] manifest apply done: created %d, existing %d, failed %d, unanswered %d, %d app-scoped"),
			Result.Created, Result.Existing, Result.Failed, Result.Unanswered, Result.AppScoped);
		if (OnDone) { OnDone(Result); }
	});

	// Pacing against the shared allowance: the runner parks while the window is near full and resumes on this
	// world's timer, and a RATE_LIMITED row is retried with backoff instead of ending the apply.
	Runner->SetPacing(
		[WeakThis]()
		{
			const UCrowdyGameModelSubsystem* S = ResolveLiveSelf(WeakThis);
			return !S || S->GetRecentInvokeCount() < InvokeBudgetLimitPerWindow - FCrowdyManifestApplyRunner::DefaultMaxInFlight;
		},
		[WeakThis](float Seconds, TFunction<void()> Fn)
		{
			UCrowdyGameModelSubsystem* S = ResolveLiveSelf(WeakThis);
			UWorld* World = S ? S->GetWorld() : nullptr;
			if (!World)
			{
				Fn();
				return;
			}
			FTimerHandle Handle;
			World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda(MoveTemp(Fn)), Seconds, false);
		});

	ManifestApplies.Add(Runner);
	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Log,
		TEXT("[GameModel] manifest apply: %d row(s) of %s into session %s, %d of them app-scoped"), Manifest->Rows.Num(),
		*Manifest->MapPackage, ResolvedSession.IsEmpty() ? TEXT("<app-global>") : *ResolvedSession, AppScopedRows);
	Runner->Start();
}

void UCrowdyGameModelSubsystem::FailPendingManifestApplies()
{
	// Each Abort completes its runner, whose completion prunes the list; iterate a copy.
	TArray<TSharedPtr<FCrowdyManifestApplyRunner>> Pending = ManifestApplies;
	ManifestApplies.Empty();
	for (const TSharedPtr<FCrowdyManifestApplyRunner>& Runner : Pending)
	{
		if (Runner.IsValid() && !Runner->IsDone())
		{
			Runner->Abort(TEXT("shutdown"), TEXT("The world tore down before every row was answered."));
		}
	}
}
