// Fill out your copyright notice in the Description page of Project Settings.

#include "CrowdyStudioSyncService.h"

#include "Auth/FCrowdyTokenVault.h"
#include "CrowdyStudioModule.h" // LogCrowdyStudio
#include "GameModel/CrowdySchemaSync.h"
#include "Gql/CrowdyStudioQueries.h"
#include "Model/CrowdyStudioTypes.h"

#include "CrowdyCppClient.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameModel/CrowdyStudioFunctionMarshalling.h"
#include "Network/CrowdyCpp/CrowdyCppAdminClientHost.h"

#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Replication/GameModel/Effect/CrowdyGameModelFunctionMarshaller.h"
#include "Replication/GameModel/Effect/CrowdyEffectAst.h"
#include "Replication/GameModel/Effect/CrowdyEffectLowering.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

using namespace CrowdyStudioMarshalling;

namespace
{
	// The last computed status per effect, and the effects with a status read in flight (so a duplicate request
	// coalesces onto the one round-trip). Both are game-thread only: every GraphQL callback lands on the game thread.
	TMap<TWeakObjectPtr<UCrowdyEffect>, ECrowdyEffectSyncStatus> GStatusCache;
	TSet<TWeakObjectPtr<UCrowdyEffect>> GStatusInFlight;
	// A per-effect generation counter bumped whenever a sync writes a fresh Synced status or an edit invalidates the
	// cache. A status read captures it at kickoff and declines to overwrite the cache if it changed while in flight, so a
	// read that captured the pre-sync server snapshot can never clobber the newer value with a stale one.
	TMap<TWeakObjectPtr<UCrowdyEffect>, uint64> GStatusEpoch;

	void SetCached(const UCrowdyEffect* Effect, ECrowdyEffectSyncStatus Status)
	{
		if (Effect)
		{
			GStatusCache.Add(TWeakObjectPtr<UCrowdyEffect>(const_cast<UCrowdyEffect*>(Effect)), Status);
		}
	}

	void BumpStatusEpoch(const UCrowdyEffect* Effect)
	{
		if (Effect)
		{
			const TWeakObjectPtr<UCrowdyEffect> Key(const_cast<UCrowdyEffect*>(Effect));
			GStatusEpoch.Add(Key, GStatusEpoch.FindRef(Key) + 1);
		}
	}

	// Drop every cached verdict a schema write has just made stale. A status describes the server schema as it stood
	// when that read ran, and a write moves it: a per-effect sync writes the effect's container type and property defs,
	// which every other effect on that type was judged against, and the console's apply/prune/delete writes the lot.
	// Dropped to Unknown rather than left in place because nothing re-reads a cached Drifted, so one left here outlives
	// the drift it describes and goes on raising the pre-play prompt forever.
	//
	// KeepFresh is the effect whose own fresh verdict the caller is about to record (null when the write came from
	// somewhere else and no effect is exempt). It is compared only when non-null: a garbage-collected key also reads
	// back as null, and treating that as a match would silently exempt every dead entry from a console-wide drop.
	void InvalidateCachedStatusesAfterSchemaWrite(const UCrowdyEffect* KeepFresh)
	{
		for (TPair<TWeakObjectPtr<UCrowdyEffect>, ECrowdyEffectSyncStatus>& Entry : GStatusCache)
		{
			if (KeepFresh && Entry.Key.Get() == KeepFresh)
			{
				continue;
			}
			Entry.Value = ECrowdyEffectSyncStatus::Unknown;
			// By key rather than through BumpStatusEpoch: the effect may have been garbage-collected, and a read that
			// captured the old epoch still has to be refused when it lands.
			GStatusEpoch.Add(Entry.Key, GStatusEpoch.FindRef(Entry.Key) + 1);
		}
	}

	// Compile one effect to its desired container type + function, applying the same shippability guards the console's
	// GatherDesiredFunctions uses. Returns false (with a human-readable reason) for an unmigrated / non-compiling /
	// reserved-name effect that must not be synced.
	bool CompileEffectForSync(const UCrowdyEffect* Effect, FCrowdyDesiredContainerType& OutType,
		FCrowdyGameModelFunctionInput& OutFunction, FString& OutGuardReason)
	{
		if (!Effect)
		{
			OutGuardReason = TEXT("No effect asset.");
			return false;
		}

		// Only Error diagnostics are read below, and the fn-callee catalog contributes warnings alone, so it is
		// skipped: a catalog cache miss would synchronously force-load every Crowdy Effect asset in the project,
		// which is far too much to pay inside a single-effect sync.
		const FCrowdyEffectLoweringResult Result = Effect->Compile(ECrowdyEffectFnCatalog::None);
		if (Result.HasErrors())
		{
			FString FirstError = TEXT("a compile error");
			for (const FCrowdyEffectDiagnostic& Diag : Result.Diagnostics)
			{
				if (Diag.Severity == ECrowdyEffectSeverity::Error)
				{
					FirstError = Diag.ToString();
					break;
				}
			}
			OutGuardReason = FString::Printf(TEXT("This effect has %s; fix it before syncing."), *FirstError);
			return false;
		}

		OutFunction = Result.Function;
		if (OutFunction.Name.IsEmpty())
		{
			OutGuardReason = TEXT("This effect compiled to an empty function name.");
			return false;
		}

		if (CrowdyGameModelMetaKeys::IsReservedCollectionFunctionName(OutFunction.Name))
		{
			OutGuardReason = FString::Printf(
				TEXT("This effect uses the reserved '%s' function-name prefix; rename it before syncing."),
				CrowdyGameModelMetaKeys::CollectionTouchFunctionPrefix);
			return false;
		}

		if (UClass* Class = Effect->ContainerClass.LoadSynchronous())
		{
			OutType = FCrowdySchemaSync::BuildDesiredForClass(Class);
		}
		return true;
	}

	// One schema-authoring operation this run still has to issue: which generated operation set to look it up in,
	// its name, and its variables. Replaces raw query text now that the transport resolves the document by name.
	struct FSyncOp
	{
		ECrowdyCppApiDomain Domain = ECrowdyCppApiDomain::GameModel;
		FString OperationName;
		TSharedPtr<FJsonObject> Variables;
	};

	// Everything one status/sync run reads back from the server before it can plan.
	struct FSyncRun : public TSharedFromThis<FSyncRun>
	{
		TWeakObjectPtr<UCrowdyEffect> Effect;
		int64 AppId = 0;
		// The client this run issues every request on, scoped to exactly this run's lifetime: built against the
		// shared origin, then rebuilt for the app's own datacenter endpoint once the mint answers.
		TSharedPtr<FCrowdyCppAdminClientHost> ClientHost;
		FString SessionToken;
		FString DiscoveryUrl;
		bool bForApply = false;
		// A status read captures the effect's status epoch at kickoff; if a sync or an edit bumps it while the read is in
		// flight, the read's computed status is stale and must not overwrite the fresher cached value.
		uint64 StartEpoch = 0;

		FCrowdyDesiredContainerType DesiredType;
		FCrowdyGameModelFunctionInput DesiredFunction;

		TArray<FStudioContainerType> CurrentTypes;
		TArray<FStudioPropertyDef> CurrentProps;
		TArray<FStudioFunction> CurrentFunctions;
		int64 SessionChannelId = 0;

		int32 Pending = 0;
		bool bReadFailed = false;
		// True once a read fails for a reason other than the run's own client being torn down (a real transport or
		// server error). If every failed read was a plain cancellation, FinishReads reports that honestly instead.
		bool bReadFailedGenuine = false;
		// Single-shot latch: on a sync whose effect declares a channel notification but the app's session channel does
		// not exist, create it once and re-enter the apply. SessionChannelId is NOT refreshed by that create, so this
		// latch, not the id, is what stops a second one.
		bool bAutoCreatedChannel = false;

		TFunction<void(ECrowdyEffectSyncStatus, const FString&)> OnStatus;
		TFunction<void(bool, const FString&)> OnDone;
	};

	void FinishStatus(const TSharedRef<FSyncRun>& Run);
	void FinishSync(const TSharedRef<FSyncRun>& Run);

	void FailRun(const TSharedRef<FSyncRun>& Run, const FString& Message)
	{
		UCrowdyEffect* Effect = Run->Effect.Get();
		if (Run->bForApply)
		{
			// The apply path never registered an in-flight status token, so it must not remove one: a genuine status
			// read for the same effect may be in flight and own that token.
			if (Run->OnDone)
			{
				Run->OnDone(false, Message);
			}
		}
		else
		{
			// The status path always drops its own in-flight token, even for an effect garbage-collected mid-read, so a
			// stale weak-ptr entry never lingers to block a later read from coalescing.
			GStatusInFlight.Remove(Run->Effect);
			SetCached(Effect, ECrowdyEffectSyncStatus::Unknown);
			if (Run->OnStatus)
			{
				Run->OnStatus(ECrowdyEffectSyncStatus::Unknown, Message);
			}
		}
	}

	// Join point for the read fan-out: called after every read has returned. Reports a read failure once, else plans.
	void FinishReads(const TSharedRef<FSyncRun>& Run)
	{
		if (Run->bReadFailed)
		{
			FailRun(Run, Run->bReadFailedGenuine
				? TEXT("Could not read the current server schema (see the log). Try again when the server is reachable.")
				: TEXT("The read was cancelled before it finished. Try again."));
			return;
		}
		if (Run->bForApply)
		{
			FinishSync(Run);
		}
		else
		{
			FinishStatus(Run);
		}
	}

	// Issues one game-plane read on the run's client and joins it into the Pending count. Pending is decremented
	// exactly once here on every outcome (parsed, transport failure, or a cancellation delivered because the run's
	// client was torn down) -- the join in FinishReads only ever fires once all of them have.
	void PostGameRead(const TSharedRef<FSyncRun>& Run, ECrowdyCppApiDomain Domain, const TCHAR* OperationName,
		const TSharedPtr<FJsonObject>& Variables, TFunction<void(const TSharedPtr<FJsonObject>& /*Envelope*/)> OnParse)
	{
		Run->ClientHost->GetClient()->RunOp(Domain, OperationName, Variables,
			[Run, OnParse](FCrowdyCppJsonResult Result)
			{
				if (Result.bTransportOk)
				{
					OnParse(CrowdyStudioGql::WrapDataEnvelope(Result.Data));
				}
				else
				{
					Run->bReadFailed = true;
					if (Result.ErrorMessage != FCrowdyCppClient::CanceledErrorMessage())
					{
						Run->bReadFailedGenuine = true;
					}
				}

				if (--Run->Pending <= 0)
				{
					FinishReads(Run);
				}
			},
			ECrowdyCppTokenPlane::Game);
	}

	// Read the current server schema relevant to this effect: its container type + that type's props, every function,
	// and whether the app's session channel exists (an effect's channel notification names it, and a name the app
	// does not have reaches nobody).
	void ReadServerSnapshot(const TSharedRef<FSyncRun>& Run)
	{
		// Fix the outstanding-read count up front (types + functions + channels, and props only when a type is known),
		// so the join fires exactly once after every read returns -- never early, even if a read were to complete
		// synchronously before its siblings are issued.
		Run->Pending = Run->DesiredType.TypeName.IsEmpty() ? 3 : 4;

		{
			const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
			SetBigIntField(Variables, TEXT("appId"), Run->AppId);
			PostGameRead(Run, ECrowdyCppApiDomain::GameModel, TEXT("GameModelContainerTypes"), Variables,
				[Run](const TSharedPtr<FJsonObject>& Envelope)
				{
					TArray<TSharedPtr<FStudioContainerType>> Types;
					CrowdyStudioGql::ParseContainerTypes(Envelope, TEXT("gameModelContainerTypes"), Types);
					for (const TSharedPtr<FStudioContainerType>& T : Types)
					{
						if (T.IsValid())
						{
							Run->CurrentTypes.Add(*T);
						}
					}
				});
		}

		if (!Run->DesiredType.TypeName.IsEmpty())
		{
			const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
			SetBigIntField(Variables, TEXT("appId"), Run->AppId);
			Variables->SetStringField(TEXT("typeName"), Run->DesiredType.TypeName);
			PostGameRead(Run, ECrowdyCppApiDomain::GameModel, TEXT("GameModelPropertyDefs"), Variables,
				[Run](const TSharedPtr<FJsonObject>& Envelope)
				{
					TArray<TSharedPtr<FStudioPropertyDef>> Defs;
					CrowdyStudioGql::ParsePropertyDefs(Envelope, TEXT("gameModelPropertyDefs"), Defs);
					for (const TSharedPtr<FStudioPropertyDef>& P : Defs)
					{
						if (P.IsValid())
						{
							Run->CurrentProps.Add(*P);
						}
					}
				});
		}

		{
			const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
			SetBigIntField(Variables, TEXT("appId"), Run->AppId);
			PostGameRead(Run, ECrowdyCppApiDomain::GameModel, TEXT("GameModelFunctions"), Variables,
				[Run](const TSharedPtr<FJsonObject>& Envelope)
				{
					TArray<TSharedPtr<FStudioFunction>> Fns;
					CrowdyStudioGql::ParseFunctions(Envelope, TEXT("gameModelFunctions"), Fns);
					for (const TSharedPtr<FStudioFunction>& F : Fns)
					{
						if (F.IsValid())
						{
							Run->CurrentFunctions.Add(*F);
						}
					}
				});
		}

		{
			const FString SessionName = FString::Printf(TEXT("__crowdy_session_%lld"), Run->AppId);
			const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
			SetBigIntField(Variables, TEXT("appId"), Run->AppId);
			PostGameRead(Run, ECrowdyCppApiDomain::Channels, TEXT("Channels"), Variables,
				[Run, SessionName](const TSharedPtr<FJsonObject>& Envelope)
				{
					TArray<TSharedPtr<FStudioGroup>> Channels;
					CrowdyStudioGql::ParseGroups(Envelope, TEXT("channels"), Channels);
					for (const TSharedPtr<FStudioGroup>& Channel : Channels)
					{
						if (Channel.IsValid() && Channel->Name == SessionName)
						{
							Run->SessionChannelId = Channel->GroupId;
							break;
						}
					}
				});
		}
	}

	// Mint the app-scoped game token from the signed-in session token via the typed facade, rebuild the run's client
	// for the endpoint the mint returns, then read the server snapshot. Mirrors CrowdyStudioAuth::FetchAppChannelNames:
	// the Game API rejects the session token, so an app token is minted first.
	void MintThenRead(const TSharedRef<FSyncRun>& Run, const FString& SessionToken, const FString& DiscoveryUrl,
		const FString& FallbackGameUrl)
	{
		Run->SessionToken = SessionToken;
		Run->DiscoveryUrl = DiscoveryUrl;

		// The app's own endpoint is not known until the mint answers, so the first client talks to the shared
		// origin, which every datacenter responds to.
		FCrowdyCppClientConfig MintConfig;
		MintConfig.ApiUrl = DiscoveryUrl;
		MintConfig.DiscoveryUrl = DiscoveryUrl;

		Run->ClientHost = FCrowdyCppAdminClientHost::Create(MintConfig, FString());
		if (!Run->ClientHost.IsValid())
		{
			FailRun(Run, TEXT("Could not construct the API client; the operation was not sent."));
			return;
		}
		Run->ClientHost->GetClient()->SetManagementToken(SessionToken);

		Run->ClientHost->GetClient()->MintAppToken(Run->AppId,
			[Run, FallbackGameUrl](FCrowdyCppAppTokenResult Result)
			{
				if (!Result.bOk || Result.AppToken.IsEmpty())
				{
					UE_LOG(LogCrowdyStudio, Warning,
						TEXT("Effect sync: could not mint an app token for app %lld (%s). Sign in to Crowdy Studio with a session account."),
						Run->AppId, *Result.ErrorMessage);
					FailRun(Run, TEXT("Could not authorize with the Game API. Sign in to Crowdy Studio with a session account (not an org token)."));
					return;
				}

				FString GameEndpoint = Result.GameApiUrl;
				if (!GameEndpoint.IsEmpty())
				{
					if (!GameEndpoint.EndsWith(TEXT("/graphql")))
					{
						GameEndpoint.RemoveFromEnd(TEXT("/"));
						GameEndpoint += TEXT("/graphql");
					}
				}
				else
				{
					GameEndpoint = FallbackGameUrl;
				}

				if (GameEndpoint.IsEmpty())
				{
					FailRun(Run, TEXT("No Game API endpoint is configured. Pick and sync an app on the Crowdy Studio Project page."));
					return;
				}

				// The first client talked to the shared origin; rebuild for the app's own datacenter endpoint the
				// mint returned before issuing any game-plane read, and reinstall both bearers on the fresh client.
				FCrowdyCppClientConfig ReadConfig;
				ReadConfig.ApiUrl = GameEndpoint;
				// The mint's own discoveryUrl when the server gave one, so a client that loses this endpoint can be
				// told where to go instead of retrying it; the configured origin otherwise.
				ReadConfig.DiscoveryUrl = Result.DiscoveryUrl.IsEmpty() ? Run->DiscoveryUrl : Result.DiscoveryUrl;

				Run->ClientHost = FCrowdyCppAdminClientHost::Create(ReadConfig, FString());
				if (!Run->ClientHost.IsValid())
				{
					FailRun(Run, TEXT("Could not construct the API client; the operation was not sent."));
					return;
				}
				const TSharedRef<FCrowdyCppClient> Client = Run->ClientHost->GetClient();
				Client->SetManagementToken(Run->SessionToken);
				Client->SetGameToken(Result.AppToken);

				ReadServerSnapshot(Run);
			});
	}

	// Address the desired function's channel notification at the app's session channel, then compute the single-effect plan.
	FCrowdySchemaSyncReport PlanRun(const TSharedRef<FSyncRun>& Run, FCrowdySchemaDelta& OutDelta)
	{
		TArray<FCrowdyGameModelFunctionInput> Functions;
		Functions.Add(Run->DesiredFunction);
		FCrowdySchemaSync::InjectSessionChannelTarget(Functions);
		Run->DesiredFunction = Functions[0];

		TMap<FString, TArray<FStudioPropertyDef>> PropsByType;
		if (!Run->DesiredType.TypeName.IsEmpty())
		{
			PropsByType.Add(Run->DesiredType.TypeName, Run->CurrentProps);
		}

		return FCrowdySchemaSync::PlanForSingleEffect(
			Run->DesiredType, Run->DesiredFunction, Run->CurrentTypes, PropsByType, Run->CurrentFunctions,
			TArray<FString>(), OutDelta);
	}

	void FinishStatus(const TSharedRef<FSyncRun>& Run)
	{
		UCrowdyEffect* Effect = Run->Effect.Get();
		GStatusInFlight.Remove(Run->Effect);

		FCrowdySchemaDelta Delta;
		const FCrowdySchemaSyncReport Report = PlanRun(Run, Delta);

		const bool bOnServer = Run->CurrentFunctions.ContainsByPredicate(
			[Run](const FStudioFunction& F) { return F.Name == Run->DesiredFunction.Name; });

		ECrowdyEffectSyncStatus Status;
		FString Message;
		if (!bOnServer)
		{
			Status = ECrowdyEffectSyncStatus::NotOnServer;
			Message = FString::Printf(TEXT("Not on the server yet: %d change(s) to create."), Report.UpsertCount());
		}
		else if (Delta.IsEmpty())
		{
			// The status read is read-only and never provisions the app's session channel, but a sync does when the
			// effect's model-changed notification needs one and the app has none. So an effect already on the server
			// still has real sync work while its session channel is missing: report drift, not "in sync".
			TArray<FCrowdyGameModelFunctionInput> Fns;
			Fns.Add(Run->DesiredFunction);
			const bool bNeedsChannel = FCrowdySchemaSync::AnyFunctionNeedsSessionChannel(Fns);
			if (CrowdyStudioSyncService::ShouldReportDriftForPendingChannel(true, bNeedsChannel, Run->SessionChannelId))
			{
				Status = ECrowdyEffectSyncStatus::Drifted;
				Message = TEXT("The session channel is not provisioned; syncing will create it.");
			}
			else
			{
				// An empty delta is not the same as agreement, so the message is built from the warnings rather than
				// asserted. The status stays Synced because no sync can clear such a warning: calling it drift would
				// ask for a press that changes nothing, before every Play, forever. The log is what carries it to a
				// reader, since the asset toolbar reads the status and never the message.
				Status = ECrowdyEffectSyncStatus::Synced;
				Message = CrowdyStudioSyncService::BuildSyncedStatusMessage(Report.Warnings);
				UE_CLOG(Report.Warnings.Num() > 0, LogCrowdyStudio, Warning, TEXT("Effect '%s': %s"),
					Effect ? *Effect->GetName() : TEXT("?"), *Message);
			}
		}
		else
		{
			Status = ECrowdyEffectSyncStatus::Drifted;
			Message = FString::Printf(TEXT("Out of sync: %d change(s) to apply."), Report.UpsertCount());
			// Real drift and a difference a sync cannot express can stand at the same time, and applying the first
			// leaves the second exactly where it was, so the notes travel with the count rather than being dropped.
			if (Report.Warnings.Num() > 0)
			{
				Message += FString::Printf(TEXT(" %d note(s): %s"),
					Report.Warnings.Num(), *FString::Join(Report.Warnings, TEXT(" ")));
			}
		}

		// If a sync or an edit bumped the effect's epoch while this read was in flight, its result reflects the pre-change
		// server snapshot: fire the callback with the freshly computed value but do not overwrite the newer cached status.
		if (Effect && CrowdyStudioSyncService::ShouldCacheStatusResult(Run->StartEpoch, GStatusEpoch.FindRef(Run->Effect)))
		{
			SetCached(Effect, Status);
		}
		if (Run->OnStatus)
		{
			Run->OnStatus(Status, Message);
		}
	}

	void RunSyncOps(const TSharedRef<FSyncRun>& Run, const TSharedRef<TArray<FSyncOp>>& Ops, int32 Index)
	{
		const int32 Total = Ops->Num();
		if (Index >= Total)
		{
			UCrowdyEffect* Effect = Run->Effect.Get();
			if (Total > 0)
			{
				InvalidateCachedStatusesAfterSchemaWrite(Effect);
			}
			SetCached(Effect, ECrowdyEffectSyncStatus::Synced);
			BumpStatusEpoch(Effect);
			if (Run->OnDone)
			{
				Run->OnDone(true, FString::Printf(TEXT("Synced to the server (%d change(s))."), Total));
			}
			return;
		}

		const FSyncOp& Op = (*Ops)[Index];

		Run->ClientHost->GetClient()->RunOp(Op.Domain, Op.OperationName, Op.Variables,
			[Run, Ops, Index, Total](FCrowdyCppJsonResult Result)
			{
				if (Result.bTransportOk)
				{
					RunSyncOps(Run, Ops, Index + 1);
				}
				else
				{
					UCrowdyEffect* Effect = Run->Effect.Get();
					// The applied ops are committed and idempotent; leave the cache Unknown so the next check re-reads.
					if (Index > 0)
					{
						InvalidateCachedStatusesAfterSchemaWrite(Effect);
					}
					SetCached(Effect, ECrowdyEffectSyncStatus::Unknown);
					const bool bCanceled = Result.ErrorMessage == FCrowdyCppClient::CanceledErrorMessage();
					if (Run->OnDone)
					{
						Run->OnDone(false, bCanceled
							? FString::Printf(
								TEXT("Applied %d of %d change(s), then the operation was cancelled. Try Sync again to finish."),
								Index, Total)
							: FString::Printf(
								TEXT("Applied %d of %d change(s), then stopped on a server error (see the log). Try Sync again to finish."),
								Index, Total));
					}
				}
			},
			ECrowdyCppTokenPlane::Game);
	}

	// Create the app's session channel (its deterministic __crowdy_session_<appId> name) and re-run the apply, so the
	// effect's channel model-changed notification reaches somebody. Mirrors the console's Apply auto-create. The
	// notification names the channel rather than carrying its id, so a successful create is all the apply needs.
	void AutoCreateSessionChannelThenApply(const TSharedRef<FSyncRun>& Run)
	{
		// CreateChannelInput! is a single nested object, unlike the other Studio mutations' flat variables.
		const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
		SetBigIntField(Input, TEXT("appId"), Run->AppId);
		Input->SetStringField(TEXT("name"), FString::Printf(TEXT("__crowdy_session_%lld"), Run->AppId));
		Input->SetStringField(TEXT("description"), TEXT("Reliable-RPC session channel (auto)"));
		Input->SetBoolField(TEXT("membersCanSend"), true);
		Input->SetStringField(TEXT("membershipPolicy"), TEXT("open"));

		const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
		Variables->SetObjectField(TEXT("input"), Input);

		Run->ClientHost->GetClient()->RunOp(ECrowdyCppApiDomain::Channels, TEXT("CreateChannel"), Variables,
			[Run](FCrowdyCppJsonResult Result)
			{
				if (!Result.bTransportOk)
				{
					const bool bCanceled = Result.ErrorMessage == FCrowdyCppClient::CanceledErrorMessage();
					FailRun(Run, bCanceled
						? TEXT("Creating the app's session channel was cancelled. Sync again to finish wiring the effect's notification.")
						: TEXT("Could not create the app's session channel for the effect's notification. Create it in Crowdy Studio (Channels view), then sync again."));
					return;
				}

				FinishSync(Run); // bAutoCreatedChannel now latched, so this pass does not create again
			},
			ECrowdyCppTokenPlane::Game);
	}

	void FinishSync(const TSharedRef<FSyncRun>& Run)
	{
		// If the effect's channel model-changed notification names a session channel the app does not have, create it
		// once and re-apply, so the notification reaches somebody rather than being dropped for want of a recipient.
		if (!Run->bAutoCreatedChannel && Run->SessionChannelId == 0)
		{
			TArray<FCrowdyGameModelFunctionInput> Fns;
			Fns.Add(Run->DesiredFunction);
			if (FCrowdySchemaSync::AnyFunctionNeedsSessionChannel(Fns))
			{
				Run->bAutoCreatedChannel = true;
				AutoCreateSessionChannelThenApply(Run);
				return;
			}
		}

		FCrowdySchemaDelta Delta;
		PlanRun(Run, Delta);

		if (Delta.IsEmpty())
		{
			UCrowdyEffect* Effect = Run->Effect.Get();
			// An empty delta means this run wrote no schema, with one exception: it may have created the app's session
			// channel on the way in. Every other effect whose only drift was that missing channel is now in sync, and
			// their cached verdicts still say otherwise.
			if (Run->bAutoCreatedChannel)
			{
				InvalidateCachedStatusesAfterSchemaWrite(Effect);
			}
			SetCached(Effect, ECrowdyEffectSyncStatus::Synced);
			BumpStatusEpoch(Effect);
			if (Run->OnDone)
			{
				Run->OnDone(true, TEXT("Already in sync; nothing to apply."));
			}
			return;
		}

		const TSharedRef<TArray<FSyncOp>> Ops = MakeShared<TArray<FSyncOp>>();

		// Container type first (a new property needs its type to exist), then its property defs, then the function.
		for (const FCrowdySchemaTypeUpsert& TypeUpsert : Delta.TypeUpserts)
		{
			const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
			SetBigIntField(Input, TEXT("appId"), Run->AppId);
			Input->SetStringField(TEXT("typeName"), TypeUpsert.Type.TypeName);
			Input->SetStringField(TEXT("displayName"), TypeUpsert.Type.DisplayName);
			Input->SetStringField(TEXT("instantiableBy"), TypeUpsert.Type.InstantiableBy);
			Input->SetStringField(TEXT("defaultPropertyVisibility"), TypeUpsert.Type.DefaultVisibility);

			const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
			Variables->SetObjectField(TEXT("input"), Input);
			Ops->Add(FSyncOp{ECrowdyCppApiDomain::GameModel, TEXT("GameModelUpsertContainerType"), Variables});
		}
		for (const FCrowdySchemaPropUpsert& PropUpsert : Delta.PropUpserts)
		{
			const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
			SetBigIntField(Input, TEXT("appId"), Run->AppId);
			Input->SetStringField(TEXT("containerTypeName"), PropUpsert.ContainerTypeName);
			Input->SetStringField(TEXT("key"), PropUpsert.Prop.Key);
			Input->SetStringField(TEXT("valueType"), PropUpsert.Prop.ValueType);
			SetOptionalStringField(Input, TEXT("defaultValueJson"), PropUpsert.Prop.DefaultValueJson);
			Input->SetStringField(TEXT("visibility"), PropUpsert.Prop.Visibility);
			Input->SetStringField(TEXT("writable"), PropUpsert.Prop.Writable);

			const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
			Variables->SetObjectField(TEXT("input"), Input);
			Ops->Add(FSyncOp{ECrowdyCppApiDomain::GameModel, TEXT("GameModelUpsertPropertyDef"), Variables});
		}
		for (const FCrowdySchemaFunctionUpsert& FnUpsert : Delta.FunctionUpserts)
		{
			const TSharedPtr<FJsonObject> Input = CrowdyGameModelMarshalling::BuildFunctionUpsertInput(FnUpsert.Function, Run->AppId);
			const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
			Variables->SetObjectField(TEXT("input"), Input);
			Ops->Add(FSyncOp{ECrowdyCppApiDomain::GameModel, TEXT("GameModelUpsertFunction"), Variables});
		}

		RunSyncOps(Run, Ops, 0);
	}

	// Shared entry for both status and sync: guard + compile the effect, resolve the target, then mint + read.
	void BeginRun(UCrowdyEffect* Effect, bool bForApply,
		TFunction<void(ECrowdyEffectSyncStatus, const FString&)> OnStatus,
		TFunction<void(bool, const FString&)> OnDone)
	{
		auto ReportGuard = [bForApply, &OnStatus, &OnDone, Effect](ECrowdyEffectSyncStatus Status, const FString& Message)
		{
			if (bForApply)
			{
				if (OnDone)
				{
					OnDone(false, Message);
				}
			}
			else
			{
				SetCached(Effect, Status);
				if (OnStatus)
				{
					OnStatus(Status, Message);
				}
			}
		};

		if (!Effect)
		{
			ReportGuard(ECrowdyEffectSyncStatus::Unknown, TEXT("No effect asset."));
			return;
		}

		// Coalesce: a status read already in flight for this effect updates the cache when it lands, so do not fan out
		// a second identical read. A sync is user-initiated and rare, so it is not coalesced.
		if (!bForApply && GStatusInFlight.Contains(Effect))
		{
			if (OnStatus)
			{
				OnStatus(CrowdyStudioSyncService::GetCachedStatus(Effect), TEXT("Checking..."));
			}
			return;
		}

		FCrowdyDesiredContainerType DesiredType;
		FCrowdyGameModelFunctionInput DesiredFunction;
		FString GuardReason;
		if (!CompileEffectForSync(Effect, DesiredType, DesiredFunction, GuardReason))
		{
			ReportGuard(ECrowdyEffectSyncStatus::Unknown, GuardReason);
			return;
		}

		FString SessionToken;
		if (!FCrowdyTokenVault::Load(SessionToken) || SessionToken.IsEmpty())
		{
			ReportGuard(ECrowdyEffectSyncStatus::Unknown,
				TEXT("Not signed in to Crowdy Studio. Open Crowdy Studio and sign in, then sync."));
			return;
		}

		const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
		const int64 AppId = Settings ? Settings->AppID : 0;
		if (AppId <= 0)
		{
			ReportGuard(ECrowdyEffectSyncStatus::Unknown,
				TEXT("No app is configured. Pick an app on the Crowdy Studio Project page first."));
			return;
		}

		FString DiscoveryUrl = Settings->GetDiscoveryUrl();
		DiscoveryUrl.RemoveFromEnd(TEXT("/"));
		DiscoveryUrl += TEXT("/graphql");

		const TSharedRef<FSyncRun> Run = MakeShared<FSyncRun>();
		Run->Effect = Effect;
		Run->AppId = AppId;
		Run->bForApply = bForApply;
		Run->DesiredType = MoveTemp(DesiredType);
		Run->DesiredFunction = MoveTemp(DesiredFunction);
		Run->OnStatus = MoveTemp(OnStatus);
		Run->OnDone = MoveTemp(OnDone);

		if (!bForApply)
		{
			Run->StartEpoch = GStatusEpoch.FindRef(Run->Effect);
			GStatusInFlight.Add(Effect);
		}

		MintThenRead(Run, SessionToken, DiscoveryUrl, Settings->GetGameApiHttpUrl());
	}
}

void CrowdyStudioSyncService::RequestEffectSyncStatus(
	UCrowdyEffect* Effect, TFunction<void(ECrowdyEffectSyncStatus, const FString&)> OnStatus)
{
	BeginRun(Effect, /*bForApply*/ false, MoveTemp(OnStatus), nullptr);
}

void CrowdyStudioSyncService::SyncEffect(
	UCrowdyEffect* Effect, TFunction<void(bool, const FString&)> OnDone)
{
	BeginRun(Effect, /*bForApply*/ true, nullptr, MoveTemp(OnDone));
}

ECrowdyEffectSyncStatus CrowdyStudioSyncService::GetCachedStatus(const UCrowdyEffect* Effect)
{
	if (!Effect)
	{
		return ECrowdyEffectSyncStatus::Unknown;
	}
	const ECrowdyEffectSyncStatus* Found =
		GStatusCache.Find(TWeakObjectPtr<UCrowdyEffect>(const_cast<UCrowdyEffect*>(Effect)));
	return Found ? *Found : ECrowdyEffectSyncStatus::Unknown;
}

TArray<TWeakObjectPtr<UCrowdyEffect>> CrowdyStudioSyncService::GetCachedOutOfSyncEffects()
{
	TArray<TWeakObjectPtr<UCrowdyEffect>> Out;
	for (auto It = GStatusCache.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent(); // drop a garbage-collected effect
			continue;
		}
		if (It.Value() == ECrowdyEffectSyncStatus::Drifted || It.Value() == ECrowdyEffectSyncStatus::NotOnServer)
		{
			Out.Add(It.Key());
		}
	}
	return Out;
}

void CrowdyStudioSyncService::InvalidateCachedStatus(const UCrowdyEffect* Effect)
{
	if (Effect)
	{
		GStatusCache.Add(TWeakObjectPtr<UCrowdyEffect>(const_cast<UCrowdyEffect*>(Effect)), ECrowdyEffectSyncStatus::Unknown);
		// Bump the epoch so a status read still in flight (which captured the pre-edit snapshot) declines to overwrite
		// this fresh Unknown with its stale result.
		BumpStatusEpoch(Effect);
	}
}

void CrowdyStudioSyncService::InvalidateAllCachedStatuses()
{
	InvalidateCachedStatusesAfterSchemaWrite(/*KeepFresh*/ nullptr);
}
