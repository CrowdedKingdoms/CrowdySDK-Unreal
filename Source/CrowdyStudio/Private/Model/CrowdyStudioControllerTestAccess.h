// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "GameModel/CrowdyApplySelection.h"
#include "GameModel/CrowdyGameModelDelete.h"
#include "GameModel/CrowdyModelLoadState.h"
#include "GameModel/CrowdySchemaSync.h"
#include "Model/CrowdyStudioTypes.h"
#include "Model/FCrowdyStudioController.h"

// Reaches the controller's app-scoped reset, its attribute cache and the delete state, which are internal because no
// view may drive them directly. Declared a friend by the controller, so it must stay at global scope and keep this
// exact name.
//
// It lives in a header rather than in one test .cpp because two suites need it. A second copy in another translation
// unit would be merged with this one by the unity build and redefine it, so there is exactly one home.
struct FCrowdyStudioControllerTestAccess
{
	static TSharedPtr<FStudioPropertyDef> MakeDef(const FString& TypeName, const FString& Key)
	{
		const TSharedPtr<FStudioPropertyDef> Def = MakeShared<FStudioPropertyDef>();
		Def->ContainerTypeName = TypeName;
		Def->Key = Key;
		Def->ValueType = TEXT("int");
		return Def;
	}

	// Fill EVERY member the app-scoped reset touches with something recognizable, so a reset line that goes missing
	// leaves a value behind for the assertions below to catch. Anything added to ClearAppScopedState belongs here
	// too, or that line is free to be deleted with the suite still green.
	static void PopulateAppScopedState(FCrowdyStudioController& Controller)
	{
		const TSharedPtr<FStudioGroup> Team = MakeShared<FStudioGroup>();
		Team->GroupId = 11;
		Team->Name = TEXT("Founders");
		Controller.Teams.Add(Team);

		const TSharedPtr<FStudioGroup> Channel = MakeShared<FStudioGroup>();
		Channel->GroupId = 12;
		Channel->Name = TEXT("__crowdy_session_1");
		Controller.Channels.Add(Channel);

		const TSharedPtr<FStudioGroupMember> Member = MakeShared<FStudioGroupMember>();
		Member->UserId = 501;
		Controller.GroupMembers.Add(Member);

		const TSharedPtr<FStudioGroupRole> Role = MakeShared<FStudioGroupRole>();
		Role->RoleName = TEXT("leader");
		Controller.GroupRoles.Add(Role);

		Controller.SelectedGroupId = 11;

		Controller.TeamPolicy.AppId = 1;
		Controller.TeamPolicy.CreationPolicy = TEXT("admin");
		Controller.ChannelPolicy.AppId = 1;
		Controller.ChannelPolicy.CreationPolicy = TEXT("member");

		const TSharedPtr<FStudioGrid> Grid = MakeShared<FStudioGrid>();
		Grid->GridId = 21;
		Controller.NearbyGrids.Add(Grid);
		Controller.GridWhitelistKeys.Add(TEXT("voxel.place"));
		Controller.GridUserEffectiveKeys.Add(TEXT("voxel.break"));

		const TSharedPtr<FStudioGridGroupGrant> Grant = MakeShared<FStudioGridGroupGrant>();
		Grant->GridId = 21;
		Grant->GroupId = 11;
		Controller.GridGroupGrants.Add(Grant);

		const TSharedPtr<FStudioContainerType> Type = MakeShared<FStudioContainerType>();
		Type->TypeName = TEXT("Health");
		Controller.ContainerTypes.Add(Type);

		Controller.PropertyDefsByType.Add(TEXT("Health"), { MakeDef(TEXT("Health"), TEXT("Current")) });
		Controller.PropertyDefs = Controller.PropertyDefsByType[TEXT("Health")];
		Controller.PropertyDefsMirrorType = TEXT("Health");
		Controller.PropertyDefFetchesInFlight.Add(TEXT("Health"));
		Controller.PropertyDefLatestRequest.Add(TEXT("Health"), 7);

		const TSharedPtr<FStudioFunction> Function = MakeShared<FStudioFunction>();
		Function->Name = TEXT("ApplyDamage");
		Controller.Functions.Add(Function);
		Controller.UnfilteredFunctions.Add(Function);

		const TSharedPtr<FStudioAutomation> Automation = MakeShared<FStudioAutomation>();
		Automation->Name = TEXT("RegenTick");
		Automation->TargetTypeName = TEXT("Health");
		Controller.Automations.Add(Automation);

		const TSharedPtr<FStudioAutomationTrigger> Trigger = MakeShared<FStudioAutomationTrigger>();
		Trigger->AutomationName = TEXT("RegenTick");
		Trigger->OnEvent = TEXT("property_changed");
		Controller.AutomationTriggers.Add(Trigger);

		// The mark saying the three lists above belong to some app. Left set across a switch, the page would believe
		// the new app was already loaded and never fill itself.
		Controller.GameModelListsAppId = 4242;

		const TSharedPtr<FStudioAppFeature> Feature = MakeShared<FStudioAppFeature>();
		Feature->FeatureKey = TEXT("combat");
		Controller.Features.Add(Feature);

		const TSharedPtr<FStudioTierFeature> TierFeature = MakeShared<FStudioTierFeature>();
		TierFeature->TierId = 7;
		TierFeature->FeatureKey = TEXT("combat");
		Controller.TierFeatures.Add(TierFeature);

		const TSharedPtr<FStudioAccessTier> Tier = MakeShared<FStudioAccessTier>();
		Tier->TierId = 7;
		Tier->Name = TEXT("Founder");
		Controller.AccessTiers.Add(Tier);

		const TSharedPtr<FStudioContainer> Container = MakeShared<FStudioContainer>();
		Container->ContainerId = TEXT("container-1");
		Controller.Containers.Add(Container);

		Controller.ContainerState.ContainerId = TEXT("container-1");
		Controller.ContainerState.bValid = true;
		Controller.SelectedContainerId = TEXT("container-1");

		// The filters and the page the live list was last read at. All of it describes one app's read, so leaving
		// any of it behind would re-list the next app at another app's filters and page.
		Controller.LastContainerTypeFilter = TEXT("Health");
		Controller.LastContainerSessionFilter = TEXT("session-1");
		Controller.LastContainerLimit = 50;
		Controller.LastContainerOffset = 100;
		Controller.LastContainerPageSize = 50;
		Controller.bContainersMayHaveMore = true;

		Controller.GameModelPolicy.SessionCreationPolicy = TEXT("anyone");
		Controller.GameModelPolicy.bValid = true;

		// The states saying the lists above were really read for some app. Left set, an empty list under the new app
		// would read as "this app has none", which is what clears a delete pre-flight's blockers.
		for (int32 Slot = 0; Slot < static_cast<int32>(ECrowdyModelFamily::Count); ++Slot)
		{
			Controller.FamilyLoads[Slot].State = ECrowdyModelLoadState::Loaded;
			Controller.FamilyLoads[Slot].AppId = 4242;
			Controller.FamilyLoads[Slot].Serial = 9;
		}
		Controller.NextFamilyReadSerial = 9;

		// A model whose attribute read came back an error. Left behind, the new app's model of the same name would
		// open onto the previous app's failure.
		Controller.PropertyDefFailedTypes.Add(TEXT("Inventory"));

		// A live-model purge mid-drain. Its replies are dropped once the selection moves, so nothing else could end
		// it and the Live tab would report a delete running forever under the new app.
		Controller.ContainerPurgeAppId = 4242;
		Controller.ContainerPurgeTypeName = TEXT("Health");
		Controller.ContainerPurgePageIds.Add(TEXT("container-1"));
		Controller.ContainerPurgeCompleted = 3;
		Controller.ContainerPurgeAlreadyGone = 1;
		Controller.ContainerPurgePageRemoved = 2;
	}

	static void SetFamilyLoad(FCrowdyStudioController& Controller, ECrowdyModelFamily Family,
		ECrowdyModelLoadState State, int64 AppId, uint64 Serial)
	{
		FCrowdyFamilyLoad& Load = Controller.FamilyLoads[static_cast<int32>(Family)];
		Load.State = State;
		Load.AppId = AppId;
		Load.Serial = Serial;
	}

	static const FCrowdyFamilyLoad& GetFamilyLoad(const FCrowdyStudioController& Controller, ECrowdyModelFamily Family)
	{
		return Controller.FamilyLoads[static_cast<int32>(Family)];
	}

	// Claim a read the way every family fetch does, so a test can hold two of them open at once. There is no
	// transport here to leave one outstanding any other way.
	static uint64 IssueFamilyRead(FCrowdyStudioController& Controller, ECrowdyModelFamily Family)
	{
		return Controller.BeginFamilyRead(Family);
	}

	// One family reply's guard, landed with no transport. The rule it exercises lives in the controller, not here.
	static bool LandFamilyReply(FCrowdyStudioController& Controller, ECrowdyModelFamily Family, int64 AppId,
		uint64 Serial, bool bSuccess)
	{
		return Controller.AcceptFamilyReply(Family, AppId, Serial,
			bSuccess ? ECrowdyModelLoadState::Loaded : ECrowdyModelLoadState::Failed);
	}

	// A landed models read, carrying a list, through the same ingest the live reply goes through. Without this the
	// suite could observe the guard's verdict but never whether a rejected reply left the list alone, which is the
	// half of the rule that loses data.
	static void LandContainerTypesReply(FCrowdyStudioController& Controller, const TArray<FString>& TypeNames,
		int64 AppId, uint64 Serial)
	{
		TArray<TSharedPtr<FStudioContainerType>> Types;
		Types.Reserve(TypeNames.Num());
		for (const FString& TypeName : TypeNames)
		{
			const TSharedPtr<FStudioContainerType> Type = MakeShared<FStudioContainerType>();
			Type->TypeName = TypeName;
			Types.Add(Type);
		}
		Controller.IngestContainerTypes(MoveTemp(Types), AppId, Serial);
	}

	static const TArray<FString>& GetFailedAttributeTypes(const FCrowdyStudioController& Controller)
	{
		return Controller.PropertyDefFailedTypes;
	}

	// Claim a function read the way every fetch does, so a test can hold two of them open at once and land them out
	// of order. There is no transport here to leave one outstanding any other way.
	static uint64 BeginFunctionRead(FCrowdyStudioController& Controller)
	{
		return Controller.BeginFunctionRead();
	}

	// A landed functions read, carrying a list, through the same ingest the live reply goes through. bUnfiltered says
	// whether the read named a container type; a narrowed one may fill the mirror but never the app-wide list.
	static void LandFunctionsReply(FCrowdyStudioController& Controller, const TArray<FString>& FunctionNames,
		bool bUnfiltered, int64 AppId, uint64 FamilySerial, uint64 ReadSerial)
	{
		TArray<TSharedPtr<FStudioFunction>> Parsed;
		Parsed.Reserve(FunctionNames.Num());
		for (const FString& Name : FunctionNames)
		{
			const TSharedPtr<FStudioFunction> Function = MakeShared<FStudioFunction>();
			Function->Name = Name;
			Parsed.Add(Function);
		}

		Controller.IngestFunctions(MoveTemp(Parsed), bUnfiltered, AppId, FamilySerial, ReadSerial);
	}

	// A plan's five pending upsert arrays, as if a plan had just finished for this app. There is no way to produce
	// one without a server, and every selective-apply rule is about what these hold afterwards.
	//
	// The plan generation moves on exactly as a real plan moves it, so seeding twice stands in for a re-plan and a
	// walk built against the first seeding is genuinely stale.
	static void SeedPendingSchemaSync(FCrowdyStudioController& Controller, int64 PlannedAppId,
		const TArray<FCrowdySchemaTypeUpsert>& Types, const TArray<FCrowdySchemaPropUpsert>& Props,
		const TArray<FCrowdySchemaFunctionUpsert>& Functions,
		const TArray<FCrowdySchemaAutomationUpsert>& Automations,
		const TArray<FCrowdySchemaTriggerUpsert>& Triggers)
	{
		Controller.PlannedSyncAppId = PlannedAppId;
		Controller.PendingSyncTypeUpserts = Types;
		Controller.PendingSyncPropUpserts = Props;
		Controller.PendingSyncFunctionUpserts = Functions;
		Controller.PendingSyncAutomationUpserts = Automations;
		Controller.PendingSyncTriggerUpserts = Triggers;
		++Controller.SchemaPlanGeneration;

		// The report a finished plan leaves standing, counted off the very arrays it planned, so a test starts from
		// the numbers the panel would really be printing rather than from zeroes that hide a count going stale.
		FCrowdySchemaDelta Planned;
		Planned.TypeUpserts = Types;
		Planned.PropUpserts = Props;
		Planned.FunctionUpserts = Functions;
		Planned.AutomationUpserts = Automations;
		Planned.TriggerUpserts = Triggers;
		Controller.SchemaSyncReport = FCrowdySchemaSync::BuildReport(Planned, {}, /*bApplied*/ false);
	}

	static uint64 GetSchemaPlanGeneration(const FCrowdyStudioController& Controller)
	{
		return Controller.SchemaPlanGeneration;
	}

	// The functions a plan found carrying an SDK channel model-changed notification, as FinishSchemaPlan records them
	// off the DESIRED schema. Seeded straight in because the shape the diff emits when the channel is missing carries
	// no trace of that notification: the only way to produce one is a live plan against a live server.
	static void SetPlannedSessionChannelFunctions(FCrowdyStudioController& Controller,
		const TArray<TPair<FString /*OwningType*/, FString /*Name*/>>& ScopedFunctions)
	{
		Controller.PlannedSessionChannelFunctionKeys.Reset();
		for (const TPair<FString, FString>& Function : ScopedFunctions)
		{
			Controller.PlannedSessionChannelFunctionKeys.Add(
				FCrowdySchemaSync::ScopedNameKey(Function.Key, Function.Value));
		}
	}

	static const TArray<FCrowdySchemaTypeUpsert>& GetPendingTypes(const FCrowdyStudioController& Controller)
	{
		return Controller.PendingSyncTypeUpserts;
	}

	static const TArray<FCrowdySchemaPropUpsert>& GetPendingProps(const FCrowdyStudioController& Controller)
	{
		return Controller.PendingSyncPropUpserts;
	}

	static const TArray<FCrowdySchemaFunctionUpsert>& GetPendingFunctions(const FCrowdyStudioController& Controller)
	{
		return Controller.PendingSyncFunctionUpserts;
	}

	static const TArray<FCrowdySchemaAutomationUpsert>& GetPendingAutomations(const FCrowdyStudioController& Controller)
	{
		return Controller.PendingSyncAutomationUpserts;
	}

	static const TArray<FCrowdySchemaTriggerUpsert>& GetPendingTriggers(const FCrowdyStudioController& Controller)
	{
		return Controller.PendingSyncTriggerUpserts;
	}

	// The walk's completion, reached with no transport: an op list of the right length, already past its last entry.
	// Everything the completion decides - what to take out of the pending arrays, whether the app is now in sync,
	// what the banner says - runs here exactly as it does after a live walk.
	//
	// PlanGeneration is the plan the walk was built from. Pass one taken before a re-plan to land a walk that
	// outlived its plan, which is what a Preview pressed mid-send produces.
	static void FinishSchemaUpsertWalkForGeneration(FCrowdyStudioController& Controller,
		const TArray<FCrowdyApplyUnit>& Sent, uint64 PlanGeneration)
	{
		const TSharedRef<TArray<TPair<FString, TSharedPtr<FJsonObject>>>> Ops =
			MakeShared<TArray<TPair<FString, TSharedPtr<FJsonObject>>>>();
		for (int32 Index = 0; Index < Sent.Num(); ++Index)
		{
			Ops->Add(TPair<FString, TSharedPtr<FJsonObject>>(TEXT("GameModelUpsertContainerType"), nullptr));
		}

		const TSharedRef<const TArray<FCrowdyApplyUnit>> SentOps = MakeShared<TArray<FCrowdyApplyUnit>>(Sent);
		Controller.RunSchemaUpserts(Ops, SentOps, Sent.Num(), PlanGeneration);
	}

	// The ordinary case: a walk landing on the plan it was built from.
	static void FinishSchemaUpsertWalk(FCrowdyStudioController& Controller, const TArray<FCrowdyApplyUnit>& Sent)
	{
		FinishSchemaUpsertWalkForGeneration(Controller, Sent, Controller.SchemaPlanGeneration);
	}

	// A walk that WROTE Completed operations and then stopped on the one after them. RunSchemaUpserts is entered at
	// that index and its send cannot reach a server, so it fails at once. There is no transport here that could
	// answer some operations and refuse others, and what a walk leaves behind after a partial write is the question.
	static void StopSchemaUpsertWalkAfter(FCrowdyStudioController& Controller, int32 Total, int32 Completed)
	{
		const TSharedRef<TArray<TPair<FString, TSharedPtr<FJsonObject>>>> Ops =
			MakeShared<TArray<TPair<FString, TSharedPtr<FJsonObject>>>>();
		for (int32 Index = 0; Index < Total; ++Index)
		{
			Ops->Add(TPair<FString, TSharedPtr<FJsonObject>>(TEXT("GameModelUpsertContainerType"),
				MakeShared<FJsonObject>()));
		}

		const TSharedRef<const TArray<FCrowdyApplyUnit>> Sent = MakeShared<TArray<FCrowdyApplyUnit>>();
		Controller.RunSchemaUpserts(Ops, Sent, Completed, Controller.SchemaPlanGeneration);
	}

	// The verdict a finished plan retains for one app. It takes a server to produce one, and whether a path drops it
	// or leaves it standing over a server it has just written to is exactly what is under test.
	static void SeedModelSnapshot(FCrowdyStudioController& Controller, int64 AppId)
	{
		Controller.ModelSnapshotsByApp.Add(AppId, MakeShared<FCrowdyModelSnapshot>());
	}

	static void SetPlannedSyncNeedsSessionChannel(FCrowdyStudioController& Controller, bool bNeeds)
	{
		Controller.bPlannedSyncNeedsSessionChannel = bNeeds;
	}

	static bool IsAutoCreatingSessionChannel(const FCrowdyStudioController& Controller)
	{
		return Controller.bAutoCreatingSessionChannel;
	}

	static bool IsSchemaApplySelectionAll(const FCrowdyStudioController& Controller)
	{
		return Controller.bSchemaApplySelectionIsAll;
	}

	static void ClearAppScopedState(FCrowdyStudioController& Controller)
	{
		Controller.ClearAppScopedState();
	}

	static void Ingest(FCrowdyStudioController& Controller, const FString& TypeName,
		TArray<TSharedPtr<FStudioPropertyDef>> Defs)
	{
		Controller.IngestPropertyDefs(TypeName, MoveTemp(Defs));
	}

	static void SetMirrorType(FCrowdyStudioController& Controller, const FString& TypeName)
	{
		Controller.PropertyDefsMirrorType = TypeName;
	}

	static const FString& GetMirrorType(const FCrowdyStudioController& Controller)
	{
		return Controller.PropertyDefsMirrorType;
	}

	// Stands the controller up as if an app were selected, without SelectApp's settings write, token mint or reads.
	static void SetSelectedApp(FCrowdyStudioController& Controller, int64 AppId)
	{
		Controller.SelectedAppId = AppId;
	}

	static void MarkFetchInFlight(FCrowdyStudioController& Controller, const FString& TypeName)
	{
		Controller.PropertyDefFetchesInFlight.Add(TypeName);
	}

	static const TMap<FString, TArray<TSharedPtr<FStudioPropertyDef>>>& GetDefsByType(const FCrowdyStudioController& Controller)
	{
		return Controller.PropertyDefsByType;
	}

	static const TSet<FString>& GetFetchesInFlight(const FCrowdyStudioController& Controller)
	{
		return Controller.PropertyDefFetchesInFlight;
	}

	static const TMap<FString, uint64>& GetLatestRequests(const FCrowdyStudioController& Controller)
	{
		return Controller.PropertyDefLatestRequest;
	}

	static void SetLatestRequest(FCrowdyStudioController& Controller, const FString& TypeName, uint64 Serial)
	{
		Controller.PropertyDefLatestRequest.Add(TypeName, Serial);
	}

	static bool IsLatestRequest(const FCrowdyStudioController& Controller, const FString& TypeName,
		int64 RequestAppId, uint64 RequestSerial)
	{
		return Controller.IsLatestPropertyDefRequest(TypeName, RequestAppId, RequestSerial);
	}

	static int64 GetGameModelListsAppId(const FCrowdyStudioController& Controller)
	{
		return Controller.GameModelListsAppId;
	}

	static void SetSelectedAppId(FCrowdyStudioController& Controller, int64 AppId)
	{
		Controller.SelectedAppId = AppId;
	}

	static void SetSchemaPlanPhase(FCrowdyStudioController& Controller, int64 AppId, const FString& Phase)
	{
		Controller.SetSchemaPlanPhase(AppId, Phase);
	}

	static const FString& GetLastContainerTypeFilter(const FCrowdyStudioController& Controller)
	{
		return Controller.LastContainerTypeFilter;
	}

	static const FString& GetLastContainerSessionFilter(const FCrowdyStudioController& Controller)
	{
		return Controller.LastContainerSessionFilter;
	}

	static int32 GetLastContainerLimit(const FCrowdyStudioController& Controller)
	{
		return Controller.LastContainerLimit;
	}

	static int32 GetLastContainerOffset(const FCrowdyStudioController& Controller)
	{
		return Controller.LastContainerOffset;
	}

	static int32 GetLastContainerPageSize(const FCrowdyStudioController& Controller)
	{
		return Controller.LastContainerPageSize;
	}

	// Stands in for a reply having landed, so the "is there another page" rule can be exercised with no transport.
	// Goes through the same ingest the live reply does, or the rule under test would live in this helper.
	static void LandContainerPage(FCrowdyStudioController& Controller, int32 Limit, int32 PageSize,
		FCrowdyStudioController::EContainerPageEvidence Evidence)
	{
		TArray<TSharedPtr<FStudioContainer>> Page;
		Page.Reserve(PageSize);
		for (int32 Index = 0; Index < PageSize; ++Index)
		{
			Page.Add(MakeContainer(FString::Printf(TEXT("c-%d-%d"), Limit, Index)));
		}

		Controller.LastContainerLimit = Limit;
		Controller.IngestContainerPage(MoveTemp(Page), Limit, /*bAppend*/ false, Evidence);
	}

	static void SetContainerPageResult(FCrowdyStudioController& Controller, int32 Limit, int32 PageSize)
	{
		LandContainerPage(Controller, Limit, PageSize, FCrowdyStudioController::EContainerPageEvidence::Update);
	}

	// The delete handler's re-read of the window already on screen.
	static void RelistContainerWindow(FCrowdyStudioController& Controller, int32 Limit, int32 PageSize)
	{
		LandContainerPage(Controller, Limit, PageSize, FCrowdyStudioController::EContainerPageEvidence::Keep);
	}

	static uint64 BeginContainerRead(FCrowdyStudioController& Controller)
	{
		return Controller.BeginContainerRead();
	}

	static bool IsLatestContainerRead(const FCrowdyStudioController& Controller, uint64 ReadSerial)
	{
		return Controller.IsLatestContainerRead(ReadSerial);
	}

	static void SetContainers(FCrowdyStudioController& Controller, TArray<TSharedPtr<FStudioContainer>> Held)
	{
		Controller.Containers = MoveTemp(Held);
	}

	static void AppendContainers(FCrowdyStudioController& Controller,
		const TArray<TSharedPtr<FStudioContainer>>& Page)
	{
		Controller.AppendContainers(Page);
	}

	static TSharedPtr<FStudioContainer> MakeContainer(const FString& ContainerId)
	{
		const TSharedPtr<FStudioContainer> Container = MakeShared<FStudioContainer>();
		Container->ContainerId = ContainerId;
		return Container;
	}

	// The filters and page the Live tab last read at, set directly so a test can prove another read leaves them
	// alone. Going through FetchContainers instead would issue the very read whose side effects are under test.
	static void SetContainerWindow(FCrowdyStudioController& Controller, const FString& TypeFilter,
		const FString& SessionFilter, int32 Limit, int32 Offset, int32 PageSize, bool bMayHaveMore)
	{
		Controller.LastContainerTypeFilter = TypeFilter;
		Controller.LastContainerSessionFilter = SessionFilter;
		Controller.LastContainerLimit = Limit;
		Controller.LastContainerOffset = Offset;
		Controller.LastContainerPageSize = PageSize;
		Controller.bContainersMayHaveMore = bMayHaveMore;
	}

	// A live-model probe whose reads are still outstanding. There is no way to leave one outstanding through the
	// public entry point without a transport that defers its replies, and the teardown paths only exist for a probe
	// in that state.
	static void SeedOutstandingLiveCount(FCrowdyStudioController& Controller, int64 AppId,
		const TArray<FString>& Pending, TFunction<void(int64, TArray<FCrowdyDeleteLiveCount>&&)> Completion)
	{
		Controller.LiveCountAppId = AppId;
		++Controller.LiveCountSerial;
		Controller.LiveCountResults.Reset();
		Controller.LiveCountPending = Pending;
		Controller.LiveCountCompletion = MoveTemp(Completion);
	}

	static bool HasOutstandingLiveCount(const FCrowdyStudioController& Controller)
	{
		return static_cast<bool>(Controller.LiveCountCompletion);
	}

	// A commit walk that has finished Completed operations and is waiting on the one after them. Seeded for the same
	// reason as the probe above: nothing here can hold a reply open long enough to observe the walk mid-flight.
	static void SeedInFlightDeleteWalk(FCrowdyStudioController& Controller, int64 AppId,
		const TArray<FCrowdyDeleteOp>& Ops, int32 Completed, int32 AlreadyGone)
	{
		Controller.DeleteWalkOps = MakeShared<TArray<FCrowdyDeleteOp>>(Ops);
		Controller.DeleteCommitAppId = AppId;
		Controller.DeleteCommitTotal = Ops.Num();
		Controller.DeleteCommitCompleted = Completed;
		Controller.DeleteCommitAlreadyGone = AlreadyGone;
		Controller.DeleteRemainder.Reset();
		Controller.LastDeleteOutcome = FCrowdyDeleteOutcome();
	}

	// The app a walk is pinned to, whatever is selected now. IsDeleteCommitInFlight deliberately answers only for the
	// selected app, so it cannot say whether a walk pinned to another one is still running.
	static int64 GetDeleteCommitAppId(const FCrowdyStudioController& Controller)
	{
		return Controller.DeleteCommitAppId;
	}

	// One operation's reply, landed on the walk in flight. There is no transport here to produce one, and without
	// this seam every walk in the suite ends on its first operation, which cannot tell "stops at the first failure"
	// apart from "never advances at all".
	static void LandDeleteReply(FCrowdyStudioController& Controller, int32 Index, bool bDeleted)
	{
		if (!Controller.DeleteWalkOps.IsValid() || !Controller.DeleteWalkOps->IsValidIndex(Index))
		{
			return;
		}

		// The exact { "data": { <field>: bool } } envelope every delete mutation answers with.
		const TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField((*Controller.DeleteWalkOps)[Index].ResultField, bDeleted);
		const TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetObjectField(TEXT("data"), Data);

		Controller.HandleDeleteOpReply(Controller.DeleteWalkOps.ToSharedRef(), Index, Envelope);
	}

	static bool HasDeleteWalkInFlight(const FCrowdyStudioController& Controller)
	{
		return Controller.DeleteWalkOps.IsValid();
	}

	// A purge part-way through one page. Nothing here can hold a reply open long enough for the public entry point
	// to leave a purge in that state, and the page-exhausted branches only exist for a purge mid-flight.
	static void SeedInFlightContainerPurge(FCrowdyStudioController& Controller, int64 AppId,
		const FString& TypeName, const TArray<FString>& PageIds)
	{
		Controller.ContainerPurgeAppId = AppId;
		++Controller.ContainerPurgeSerial;
		Controller.ContainerPurgeTypeName = TypeName;
		Controller.ContainerPurgePageIds = PageIds;
		Controller.ContainerPurgeCompleted = 0;
		Controller.ContainerPurgeAlreadyGone = 0;
		Controller.ContainerPurgePageRemoved = 0;
		Controller.bContainerPurgeCancelRequested = false;
		Controller.LastContainerPurgeOutcome = FCrowdyDeleteOutcome();
	}

	// One live-model delete's reply, landed on the purge in flight. bDeleted false is the server saying it was not
	// there, which is the reply the page-removed-nothing guard has to tell apart from a real removal.
	static void LandContainerPurgeReply(FCrowdyStudioController& Controller, int32 Index, bool bDeleted)
	{
		if (!Controller.ContainerPurgePageIds.IsValidIndex(Index))
		{
			return;
		}

		const TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("gameModelDeleteContainer"), bDeleted);
		const TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetObjectField(TEXT("data"), Data);

		Controller.HandleContainerPurgeReply(Index, Controller.ContainerPurgePageIds[Index], Envelope);
	}

	static int64 GetContainerPurgeAppId(const FCrowdyStudioController& Controller)
	{
		return Controller.ContainerPurgeAppId;
	}

	// The prune candidates a plan left behind, as if a plan had just run for this app.
	static void SeedPruneFunctions(FCrowdyStudioController& Controller, int64 PlannedAppId,
		const TArray<FCrowdySchemaFunctionRef>& Refs)
	{
		Controller.PlannedSyncAppId = PlannedAppId;
		Controller.PendingPruneTypes.Reset();
		Controller.PendingPruneProps.Reset();
		Controller.PendingPruneAutomations.Reset();
		Controller.PendingPruneFunctions = Refs;
	}
};

#endif // WITH_DEV_AUTOMATION_TESTS
