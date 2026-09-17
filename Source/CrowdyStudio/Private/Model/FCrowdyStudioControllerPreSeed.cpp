#include "Model/FCrowdyStudioController.h"

#include "Data/CrowdyContainerManifest.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameModel/CrowdyContainerManifestScan.h"
#include "GameModel/CrowdyStudioFunctionMarshalling.h"
#include "Gql/CrowdyStudioQueries.h"
#include "Misc/PackageName.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h"
#include "UObject/Package.h"

namespace
{
	// One page per read (the server's maximum); a type with more pages than the ceiling is reported, not read forever.
	constexpr int32 PreSeedPageSize = 1000;
	constexpr int32 PreSeedMaxPagesPerType = 50;
	constexpr int32 PreSeedMaxInFlight = 8;
	// The server's per-call seed ceiling; a call is all-or-nothing.
	constexpr int32 PreSeedMaxSeedRowsPerCall = 1000;
	constexpr int32 PreSeedSessionListLimit = 50;

	FString PreSeedOpenMapPackage()
	{
		const UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		return World && World->GetPackage() ? World->GetPackage()->GetName() : FString();
	}
}

struct FCrowdyStudioController::FCrowdyPreSeedPlanRun
{
	int64 AppId = 0;
	uint64 Generation = 0;
	FString Scope;
	FString MapPackage;
	TArray<FCrowdyContainerManifestRow> Manifest;
	TArray<FString> Types;
	// The server's container types, read first: an app-scoped type is read with no session.
	TArray<FStudioContainerType> ServerTypes;
	TArray<FCrowdyPreSeedServerRow> Rows;
	TArray<FString> Warnings;
	// Rows of this plan's scope read so far for the current type; the ceiling warning counts these, not every scope.
	int32 InScopeRowsForType = 0;
	bool bFailed = false;

	bool IsAppScopedType(const FString& TypeName) const
	{
		const FStudioContainerType* Type = ServerTypes.FindByPredicate([&TypeName](const FStudioContainerType& Candidate)
		{
			return Candidate.TypeName.Equals(TypeName, ESearchCase::CaseSensitive);
		});
		return Type && FCrowdyPreSeedPlan::IsAppScoped(*Type);
	}
};

struct FCrowdyStudioController::FCrowdyPreSeedApplyRun
{
	int64 AppId = 0;
	uint64 Generation = 0;
	FString Scope;
	// Seed batches go first, one call at a time; the ensure rows follow, a few in flight at once.
	TArray<FCrowdyPreSeedBatch> SeedBatches;
	int32 SeedCursor = 0;
	int32 SeedCalls = 0;
	int32 Seeded = 0;
	TArray<int32> RowIndices;
	int32 Cursor = 0;
	int32 Active = 0;
	int32 Created = 0;
	int32 Existing = 0;
	int32 Failed = 0;
	// The server's message for the failure that stopped the run, when one was reported.
	FString FailureMessage;
	// Set on the first failure: the rows still in flight settle, no new row is sent. A systemic failure (no token,
	// an unreachable server) is then one message, not one per row.
	bool bStopped = false;
};

void FCrowdyStudioController::ClearPreSeedState()
{
	++PreSeedPlanGeneration;
	PreSeedReport = FCrowdyPreSeedReport();
	PreSeedScanWarnings.Reset();
	PreSeedManifest.Reset();
	PreSeedPlannedAppId = 0;
	PreSeedBusyAppId = 0;
	bPreSeedPlanInFlight = false;
	bPreSeedApplyInFlight = false;
	bPreSeedLastScanFailed = false;
	OnPreSeedReportChanged.Broadcast();
}

void FCrowdyStudioController::ScanOpenMapForPreSeed()
{
	if (IsPreSeedBusy())
	{
		SetStatus(TEXT("A pre-seed plan or apply is still running; wait for it before scanning."), false);
		return;
	}
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	++PreSeedPlanGeneration;
	PreSeedReport = FCrowdyPreSeedReport();
	PreSeedScanWarnings.Reset();
	if (!World)
	{
		PreSeedReport.StatusNote = TEXT("No map is open in the editor.");
		OnPreSeedReportChanged.Broadcast();
		return;
	}

	FCrowdyContainerManifestScanResult Result;
	FCrowdyContainerManifestScan::ScanWorld(World, Result);
	bool bWritten = false;
	UCrowdyContainerManifest* Saved = FCrowdyContainerManifestScan::SaveManifest(World, Result, FString(), &bWritten);
	PreSeedManifest.Reset(Saved);
	bPreSeedLastScanFailed = Saved == nullptr;
	PreSeedReport.MapPackage = World->GetPackage() ? World->GetPackage()->GetName() : FString();
	PreSeedReport.ManifestRowCount = Result.Rows.Num();
	for (const FCrowdyContainerManifestSkip& Skip : Result.Skips)
	{
		PreSeedScanWarnings.Add(FString::Printf(TEXT("skipped %s: %s"), *FPackageName::ObjectPathToObjectName(Skip.Actor), *Skip.Reason));
	}
	PreSeedReport.Warnings = PreSeedScanWarnings;
	PreSeedReport.StatusNote = Saved
		? FString::Printf(TEXT("Scanned %d entity actor(s): %d container row(s), %d skipped. Manifest %s %s. Click Preview."),
			Result.EntityActors, Result.Rows.Num(), Result.Skips.Num(), bWritten ? TEXT("saved as") : TEXT("unchanged:"), *Saved->GetName())
		: TEXT("The manifest could not be saved: save the map first, and scan from the editor rather than a play session.");
	OnPreSeedReportChanged.Broadcast();
}

const UCrowdyContainerManifest* FCrowdyStudioController::ResolvePreSeedManifest()
{
	// A failed scan means the disk asset, if any, describes a level the designer has since changed: refuse it.
	if (bPreSeedLastScanFailed)
	{
		return nullptr;
	}
	const FString MapPackage = PreSeedOpenMapPackage();
	if (MapPackage.IsEmpty())
	{
		return nullptr;
	}
	if (PreSeedManifest.IsValid() && PreSeedManifest->MapPackage == MapPackage)
	{
		return PreSeedManifest.Get();
	}
	PreSeedManifest.Reset();
	const FString PackageName = FCrowdyContainerManifestScan::ManifestPackageNameForMap(MapPackage);
	if (!FPackageName::DoesPackageExist(PackageName))
	{
		return nullptr;
	}
	const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
	PreSeedManifest.Reset(LoadObject<UCrowdyContainerManifest>(nullptr, *ObjectPath));
	return PreSeedManifest.Get();
}

void FCrowdyStudioController::PlanPreSeed(const FString& ScopeSessionId)
{
	if (SelectedAppId == 0)
	{
		SetStatus(TEXT("Select an app before planning a pre-seed."), true);
		return;
	}
	if (IsPreSeedBusy())
	{
		SetStatus(TEXT("A pre-seed plan or apply is still running."), false);
		return;
	}
	const UCrowdyContainerManifest* Manifest = ResolvePreSeedManifest();
	++PreSeedPlanGeneration;
	PreSeedReport = FCrowdyPreSeedReport();
	PreSeedReport.ScopeSessionId = ScopeSessionId;
	PreSeedReport.Warnings = PreSeedScanWarnings;
	PreSeedPlannedAppId = SelectedAppId;
	if (!Manifest)
	{
		PreSeedReport.StatusNote = TEXT("The open map has no container manifest. Click Scan open map first.");
		OnPreSeedReportChanged.Broadcast();
		return;
	}

	const TSharedRef<FCrowdyPreSeedPlanRun> Run = MakeShared<FCrowdyPreSeedPlanRun>();
	Run->AppId = SelectedAppId;
	Run->Generation = PreSeedPlanGeneration;
	Run->Scope = ScopeSessionId;
	Run->MapPackage = Manifest->MapPackage;
	Run->Manifest = Manifest->Rows;
	Run->Types = FCrowdyPreSeedPlan::DistinctTypes(Manifest->Rows);
	PreSeedReport.MapPackage = Run->MapPackage;
	PreSeedReport.ManifestRowCount = Run->Manifest.Num();
	if (Run->Types.Num() == 0)
	{
		FinishPreSeedPlan(Run);
		return;
	}
	bPreSeedPlanInFlight = true;
	PreSeedBusyAppId = SelectedAppId;
	PreSeedReport.StatusNote = TEXT("Reading the server's rows...");
	OnPreSeedReportChanged.Broadcast();

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	CrowdyStudioMarshalling::SetBigIntField(Variables, TEXT("appId"), Run->AppId);
	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelContainerTypes"), Variables,
		[this, Run](const TSharedPtr<FJsonObject>& Envelope)
		{
			if (Run->Generation != PreSeedPlanGeneration)
			{
				return;
			}
			TArray<TSharedPtr<FStudioContainerType>> Types;
			CrowdyStudioGql::ParseContainerTypes(Envelope, TEXT("gameModelContainerTypes"), Types);
			for (const TSharedPtr<FStudioContainerType>& Type : Types)
			{
				Run->ServerTypes.Add(*Type);
			}
			ReadPreSeedTypePage(Run, 0, 0);
		},
		[this, Run]()
		{
			FailPreSeedPlan(Run, TEXT("reading the server's container types"));
		});
}

void FCrowdyStudioController::ReadPreSeedTypePage(const TSharedRef<FCrowdyPreSeedPlanRun>& Run, int32 TypeIndex, int32 Offset)
{
	const FString& TypeName = Run->Types[TypeIndex];
	// An app-scoped type holds no session rows: it is read app-wide whatever scope was picked.
	const FString TypeScope = Run->IsAppScopedType(TypeName) ? FString() : Run->Scope;
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	CrowdyStudioMarshalling::SetBigIntField(Variables, TEXT("appId"), Run->AppId);
	Variables->SetStringField(TEXT("typeName"), TypeName);
	// A null session is no filter: the app scope reads every scope's rows and the diff keeps only the app-global
	// ones. A session scope is asked for by id, and each row's own session is still checked in the diff.
	if (TypeScope.IsEmpty()) { Variables->SetField(TEXT("sessionId"), MakeShared<FJsonValueNull>()); }
	else { Variables->SetStringField(TEXT("sessionId"), TypeScope); }
	Variables->SetNumberField(TEXT("limit"), PreSeedPageSize);
	Variables->SetNumberField(TEXT("offset"), Offset);

	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelContainers"), Variables,
		[this, Run, TypeIndex, Offset, TypeScope](const TSharedPtr<FJsonObject>& Envelope)
		{
			if (Run->Generation != PreSeedPlanGeneration)
			{
				// Superseded by a later scan, plan or app switch; that one owns the busy flag now.
				return;
			}
			TArray<TSharedPtr<FStudioContainer>> Page;
			CrowdyStudioGql::ParseContainers(Envelope, TEXT("gameModelContainers"), Page);
			for (const TSharedPtr<FStudioContainer>& Container : Page)
			{
				FCrowdyPreSeedServerRow& Row = Run->Rows.AddDefaulted_GetRef();
				Row.ContainerId = Container->ContainerId;
				Row.TypeName = Container->TypeName;
				Row.BindingKey = Container->BindingKey;
				Row.SessionId = Container->SessionId;
				Run->InScopeRowsForType += Row.SessionId == TypeScope ? 1 : 0;
			}

			// A full page may have more behind it; a short page is the end of the type.
			const int32 PagesRead = Offset / PreSeedPageSize + 1;
			if (Page.Num() >= PreSeedPageSize && PagesRead < PreSeedMaxPagesPerType)
			{
				ReadPreSeedTypePage(Run, TypeIndex, Offset + PreSeedPageSize);
				return;
			}
			if (Page.Num() >= PreSeedPageSize)
			{
				Run->Warnings.Add(FString::Printf(TEXT("%s: the server holds more rows than the %d this plan reads (%d of them in this scope); rows past that show as creates although they may exist."),
					*Run->Types[TypeIndex], PagesRead * PreSeedPageSize, Run->InScopeRowsForType));
			}
			Run->InScopeRowsForType = 0;
			if (TypeIndex + 1 < Run->Types.Num())
			{
				ReadPreSeedTypePage(Run, TypeIndex + 1, 0);
				return;
			}
			FinishPreSeedPlan(Run);
		},
		[this, Run, TypeIndex]()
		{
			FailPreSeedPlan(Run, FString::Printf(TEXT("reading the server's %s rows"), *Run->Types[TypeIndex]));
		});
}

void FCrowdyStudioController::FinishPreSeedPlan(const TSharedRef<FCrowdyPreSeedPlanRun>& Run)
{
	if (Run->Generation != PreSeedPlanGeneration)
	{
		return;
	}
	bPreSeedPlanInFlight = false;
	FCrowdyPreSeedPlan::Diff(Run->Manifest, Run->Rows, Run->Scope, PreSeedReport, Run->ServerTypes);
	PreSeedReport.MapPackage = Run->MapPackage;
	PreSeedReport.StatusNote.Reset();
	PreSeedReport.Warnings.Append(PreSeedScanWarnings);
	PreSeedReport.Warnings.Append(Run->Warnings);
	OnPreSeedReportChanged.Broadcast();
}

void FCrowdyStudioController::FailPreSeedPlan(const TSharedRef<FCrowdyPreSeedPlanRun>& Run, const FString& Context)
{
	if (Run->bFailed || Run->Generation != PreSeedPlanGeneration)
	{
		return;
	}
	Run->bFailed = true;
	bPreSeedPlanInFlight = false;
	PreSeedReport.bValid = false;
	PreSeedReport.StatusNote = FString::Printf(
		TEXT("Plan failed while %s. The server returned an error or did not respond (see the status message). Preview again when it is reachable."),
		*Context);
	OnPreSeedReportChanged.Broadcast();
}

void FCrowdyStudioController::ApplyPreSeed(uint64 GenerationSeen)
{
	if (GenerationSeen != PreSeedPlanGeneration || !PreSeedReport.bValid || PreSeedPlannedAppId != SelectedAppId)
	{
		PreSeedReport.StatusNote = TEXT("The plan changed since the review opened. Preview again and review the new plan.");
		OnPreSeedReportChanged.Broadcast();
		return;
	}
	if (SelectedAppId == 0)
	{
		return;
	}
	if (IsPreSeedBusy())
	{
		SetStatus(TEXT("A pre-seed plan or apply is still running."), false);
		return;
	}

	const TSharedRef<FCrowdyPreSeedApplyRun> Run = MakeShared<FCrowdyPreSeedApplyRun>();
	Run->AppId = SelectedAppId;
	Run->Generation = PreSeedPlanGeneration;
	Run->Scope = PreSeedReport.ScopeSessionId;
	FCrowdyPreSeedPlan::SplitCreateRows(PreSeedReport.Rows, PreSeedMaxSeedRowsPerCall, Run->SeedBatches, Run->RowIndices);
	if (Run->SeedBatches.Num() == 0 && Run->RowIndices.Num() == 0)
	{
		PreSeedReport.StatusNote = TEXT("Nothing to create: every manifest row already exists in this scope.");
		OnPreSeedReportChanged.Broadcast();
		return;
	}
	bPreSeedApplyInFlight = true;
	PreSeedBusyAppId = SelectedAppId;
	PreSeedReport.StatusNote = FString::Printf(TEXT("Creating %d row(s)..."), PreSeedReport.ToCreate);
	OnPreSeedReportChanged.Broadcast();
	SendPreSeedBatch(Run);
}

void FCrowdyStudioController::SendPreSeedBatch(const TSharedRef<FCrowdyPreSeedApplyRun>& Run)
{
	if (Run->Generation != PreSeedPlanGeneration)
	{
		Run->bStopped = true;
	}
	if (Run->bStopped || Run->SeedCursor >= Run->SeedBatches.Num())
	{
		if (Run->RowIndices.Num() == 0 || Run->bStopped)
		{
			FinishPreSeedApply(Run);
			return;
		}
		PumpPreSeedApply(Run);
		return;
	}

	// One batch in flight at a time: a batch is all-or-nothing, and a refusal stops the run before the next one.
	const FCrowdyPreSeedBatch& Batch = Run->SeedBatches[Run->SeedCursor++];
	const TSharedPtr<FJsonObject> Variables = FCrowdyPreSeedPlan::BuildSeedVariables(
		Run->AppId, Batch.bAppScoped ? FString() : Run->Scope, PreSeedReport.Rows, Batch.RowIndices);
	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelSeed"), Variables,
		[this, Run, Indices = Batch.RowIndices](const TSharedPtr<FJsonObject>& Envelope)
		{
			if (Run->Generation != PreSeedPlanGeneration)
			{
				Run->bStopped = true;
				return;
			}
			int32 Created = 0;
			FString IdMapJson;
			if (!CrowdyStudioGql::ParseSeedContainers(Envelope, Created, IdMapJson))
			{
				Run->Failed += Indices.Num();
				Run->bStopped = true;
				FinishPreSeedApply(Run);
				return;
			}
			// Only a row the reply mapped to an id counts as seeded; the server's created count is bounded by it.
			const int32 Mapped = FCrowdyPreSeedPlan::ApplySeedIdMap(IdMapJson, Indices, PreSeedReport.Rows);
			Created = FMath::Clamp(Created, 0, Mapped);
			Run->Created += Created;
			Run->Existing += Mapped - Created;
			Run->Seeded += Mapped;
			++Run->SeedCalls;
			if (Mapped < Indices.Num())
			{
				const int32 Unmapped = Indices.Num() - Mapped;
				Run->Failed += Unmapped;
				Run->FailureMessage = FString::Printf(TEXT("the seed reply mapped %d of %d row(s) to an id; %d row(s) came back unmapped"),
					Mapped, Indices.Num(), Unmapped);
				Run->bStopped = true;
				FinishPreSeedApply(Run);
				return;
			}
			SendPreSeedBatch(Run);
		},
		[this, Run, Count = Batch.RowIndices.Num()]()
		{
			Run->Failed += Count;
			Run->FailureMessage = GetStatusMessage();
			Run->bStopped = true;
			FinishPreSeedApply(Run);
		});
}

void FCrowdyStudioController::PumpPreSeedApply(const TSharedRef<FCrowdyPreSeedApplyRun>& Run)
{
	// A later scan, plan or app switch retires this run: nothing more is sent, the rows in flight settle, and the
	// report they would have written belongs to the newer generation.
	if (Run->Generation != PreSeedPlanGeneration)
	{
		Run->bStopped = true;
	}
	while (Run->Active < PreSeedMaxInFlight && Run->Cursor < Run->RowIndices.Num() && !Run->bStopped)
	{
		const int32 RowIndex = Run->RowIndices[Run->Cursor++];
		++Run->Active;
		const FCrowdyPreSeedRow& Row = PreSeedReport.Rows[RowIndex];
		const TSharedPtr<FJsonObject> Variables = FCrowdyGameApiCodec::BuildEnsureContainerVariables(
			Run->AppId, Row.TypeName, Row.BindingKey, Row.DisplayName, Row.bAppScoped ? FString() : Run->Scope, FString());

		auto Settle = [this, Run]()
		{
			--Run->Active;
			if (Run->Active == 0 && (Run->Cursor >= Run->RowIndices.Num() || Run->bStopped))
			{
				FinishPreSeedApply(Run);
				return;
			}
			PumpPreSeedApply(Run);
		};
		SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelEnsureContainer"), Variables,
			[this, Run, RowIndex, Settle](const TSharedPtr<FJsonObject>& Envelope)
			{
				if (Run->Generation != PreSeedPlanGeneration)
				{
					Run->bStopped = true;
					Settle();
					return;
				}
				FString ContainerId;
				int64 OwnerUserId = 0;
				bool bCreated = false;
				const bool bOk = FCrowdyGameApiCodec::ParseEnsureContainerEnvelope(Envelope, true, TArray<FString>(),
					ContainerId, OwnerUserId, bCreated);
				if (!bOk)
				{
					++Run->Failed;
					Run->bStopped = true;
				}
				else
				{
					FCrowdyPreSeedRow& Row = PreSeedReport.Rows[RowIndex];
					Row.Kind = ECrowdyPreSeedRowKind::Existing;
					Row.ContainerId = ContainerId;
					bCreated ? ++Run->Created : ++Run->Existing;
				}
				Settle();
			},
			[this, Run, Settle]()
			{
				++Run->Failed;
				Run->FailureMessage = GetStatusMessage();
				Run->bStopped = true;
				Settle();
			});
	}
}

void FCrowdyStudioController::FinishPreSeedApply(const TSharedRef<FCrowdyPreSeedApplyRun>& Run)
{
	if (Run->Generation != PreSeedPlanGeneration)
	{
		return;
	}
	bPreSeedApplyInFlight = false;
	PreSeedReport.bApplied = true;
	PreSeedReport.Created = Run->Created;
	PreSeedReport.Failed = Run->Failed;
	// A row this apply created is counted as created; Existing keeps the rows the server held before, plus the
	// ones another creator raced this apply to.
	PreSeedReport.Existing += Run->Existing;
	PreSeedReport.ToCreate = 0;
	for (const FCrowdyPreSeedRow& Row : PreSeedReport.Rows)
	{
		PreSeedReport.ToCreate += Row.Kind == ECrowdyPreSeedRowKind::Create ? 1 : 0;
	}
	if (Run->Failed > 0)
	{
		PreSeedReport.StatusNote = FString::Printf(
			TEXT("Created %d row(s), then stopped at the first failure: %s. %d row(s) remain to create. The created rows stand; Preview again and Apply to send the rest."),
			Run->Created, Run->FailureMessage.IsEmpty() ? TEXT("see the status message") : *Run->FailureMessage, PreSeedReport.ToCreate);
	}
	else
	{
		PreSeedReport.StatusNote = BuildPreSeedApplyNote(Run);
	}
	OnPreSeedReportChanged.Broadcast();
}

FString FCrowdyStudioController::BuildPreSeedApplyNote(const TSharedRef<FCrowdyPreSeedApplyRun>& Run) const
{
	TArray<FString> Parts;
	if (Run->SeedCalls > 0)
	{
		Parts.Add(FString::Printf(TEXT("seeded %d row(s) in %d call(s)"), Run->Seeded, Run->SeedCalls));
	}
	if (Run->RowIndices.Num() > 0)
	{
		TArray<FString> EnsuredTypes;
		for (const int32 RowIndex : Run->RowIndices)
		{
			const FString& TypeName = PreSeedReport.Rows[RowIndex].TypeName;
			if (!EnsuredTypes.ContainsByPredicate([&TypeName](const FString& Known) { return Known.Equals(TypeName, ESearchCase::CaseSensitive); }))
			{
				EnsuredTypes.Add(TypeName);
			}
		}
		Parts.Add(FString::Printf(TEXT("ensured %d row(s) one by one (types without an admin or bind-policy declaration: %s)"),
			Run->RowIndices.Num(), *FString::Join(EnsuredTypes, TEXT(", "))));
	}
	const FString Note = FString::Join(Parts, TEXT("; "));
	return FString::Printf(TEXT("%s%s. %d created, %d already existed."),
		*Note.Left(1).ToUpper(), *Note.Mid(1), Run->Created, Run->Existing);
}

void FCrowdyStudioController::FetchSessions(TFunction<void(const TArray<FStudioSession>&)> OnDone)
{
	if (SelectedAppId == 0)
	{
		OnDone(TArray<FStudioSession>());
		return;
	}
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	CrowdyStudioMarshalling::SetBigIntField(Variables, TEXT("appId"), SelectedAppId);
	Variables->SetNumberField(TEXT("limit"), PreSeedSessionListLimit);
	SendGame(ECrowdyCppApiDomain::GameModel, TEXT("GameModelSessions"), Variables,
		[OnDone](const TSharedPtr<FJsonObject>& Envelope)
		{
			TArray<FStudioSession> Sessions;
			CrowdyStudioGql::ParseSessions(Envelope, TEXT("gameModelSessions"), Sessions);
			OnDone(Sessions);
		},
		[OnDone]()
		{
			OnDone(TArray<FStudioSession>());
		});
}
