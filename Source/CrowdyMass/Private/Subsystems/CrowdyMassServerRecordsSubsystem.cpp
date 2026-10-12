#include "Subsystems/CrowdyMassServerRecordsSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "CrowdyMassLog.h"
#include "CrowdyServerObject.h"
#include "CrowdyServerObjectComponent.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectLibrary.h"
#include "CrowdyServerObjectSubsystem.h"
#include "Derivation/CrowdyMassServerRecordsDerivation.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Subsystem/CrowdySDKSubsystem.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace CrowdyMassServerRecords
{
	/** Whether Function takes the row's entity target and then the changed names, and nothing else. */
	bool ResolveHandlerParams(const UFunction& Function, const FStructProperty*& OutTarget, const FArrayProperty*& OutChanged)
	{
		OutTarget = nullptr;
		OutChanged = nullptr;
		int32 Count = 0;
		for (TFieldIterator<FProperty> It(&Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (FCrowdyRPC::IsTrueOutputParam(*It))
			{
				return false;
			}
			const FStructProperty* AsTarget = CastField<FStructProperty>(*It);
			const FArrayProperty* AsNames = CastField<FArrayProperty>(*It);
			OutTarget = Count == 0 && AsTarget && AsTarget->Struct == FCrowdyMassEntityTarget::StaticStruct() ? AsTarget : OutTarget;
			OutChanged = Count == 1 && AsNames && AsNames->Inner->IsA<FStrProperty>() ? AsNames : OutChanged;
			++Count;
		}
		return Count == 2 && OutTarget && OutChanged;
	}

	/** Server record ids are case-sensitive, so only the spelling an entity's id is written in names its row. */
	bool ParseRecordId(const FString& RecordId, FGuid& OutUUID)
	{
		for (const TCHAR Char : RecordId)
		{
			if (FChar::IsLower(Char))
			{
				return false;
			}
		}
		return FGuid::ParseExact(RecordId, EGuidFormats::Digits, OutUUID);
	}
}

void UCrowdyMassServerRecordsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UCrowdyMassEntitySubsystem* Entities = Collection.InitializeDependency<UCrowdyMassEntitySubsystem>();

#if WITH_EDITOR
	ObjectsReplacedHandle = FCoreUObjectDelegates::OnObjectsReplaced.AddUObject(this, &UCrowdyMassServerRecordsSubsystem::HandleObjectsReplaced);
	ObjectPropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddUObject(this, &UCrowdyMassServerRecordsSubsystem::HandleObjectPropertyChanged);
#endif

	UWorld* World = GetWorld();
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	if (!GameInstance || !Entities)
	{
		return;
	}
	Rows = Entities;
	ServerObjects = GameInstance->GetSubsystem<UCrowdyServerObjectSubsystem>();
	SDK = GameInstance->GetSubsystem<UCrowdySDKSubsystem>();
	if (UCrowdySDKSubsystem* Login = SDK.Get())
	{
		Login->OnLogin.AddUniqueDynamic(this, &UCrowdyMassServerRecordsSubsystem::HandleLogin);
		Login->OnRegister.AddUniqueDynamic(this, &UCrowdyMassServerRecordsSubsystem::HandleLogin);
	}
	RowClassHandle = Entities->OnRowClassRecorded.AddUObject(this, &UCrowdyMassServerRecordsSubsystem::HandleRowClassRecorded);
	RowRemovedHandle = Entities->OnRowRemoved.AddUObject(this, &UCrowdyMassServerRecordsSubsystem::HandleRowRemoved);
	World->GetTimerManager().SetTimer(FollowTimer, FTimerDelegate::CreateUObject(this, &UCrowdyMassServerRecordsSubsystem::Refresh), FollowSeconds, true);
}

void UCrowdyMassServerRecordsSubsystem::Deinitialize()
{
#if WITH_EDITOR
	FCoreUObjectDelegates::OnObjectsReplaced.Remove(ObjectsReplacedHandle);
	FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(ObjectPropertyChangedHandle);
#endif
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(FollowTimer);
		World->GetTimerManager().ClearTimer(FlushTimer);
	}
	if (UCrowdyMassEntitySubsystem* Entities = Rows.Get())
	{
		Entities->OnRowClassRecorded.Remove(RowClassHandle);
		Entities->OnRowRemoved.Remove(RowRemovedHandle);
	}
	if (UCrowdySDKSubsystem* Login = SDK.Get())
	{
		Login->OnLogin.RemoveDynamic(this, &UCrowdyMassServerRecordsSubsystem::HandleLogin);
		Login->OnRegister.RemoveDynamic(this, &UCrowdyMassServerRecordsSubsystem::HandleLogin);
	}
	ReleaseAll();
	FollowedClasses.Empty();
	ClassRecords.Empty();
	RowRecords.Empty();
	FieldNamesByDefinition.Empty();
	AppearedRows.Empty();
	WatchersToCatchUp.Empty();
	HandlersByClass.Empty();
	EntityWatchers.Empty();
	Super::Deinitialize();
}

bool UCrowdyMassServerRecordsSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	return Super::ShouldCreateSubsystem(Outer) && GetDefault<UCrowdyMassEntitySubsystem>()->ShouldCreateSubsystem(Outer);
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyMassServerRecordsSubsystem::InitializeForTest(UCrowdyMassEntitySubsystem* InRows, UCrowdyServerObjectSubsystem* InServerObjects)
{
	Rows = InRows;
	ServerObjects = InServerObjects;
	RowClassHandle = InRows->OnRowClassRecorded.AddUObject(this, &UCrowdyMassServerRecordsSubsystem::HandleRowClassRecorded);
	RowRemovedHandle = InRows->OnRowRemoved.AddUObject(this, &UCrowdyMassServerRecordsSubsystem::HandleRowRemoved);
}

UCrowdyServerObject* UCrowdyMassServerRecordsSubsystem::FindHeldForTest(UCrowdyServerObjectDefinition* Definition, const FString& InstanceId) const
{
	FCrowdyServerObjectKey Key;
	Key.Definition = Definition;
	Key.InstanceId = InstanceId;
	const FCrowdyMassHeldServerObject* Found = Held.Find(Key);
	return Found ? Found->Object.Get() : nullptr;
}
#endif

const FCrowdyMassClassServerRecords& UCrowdyMassServerRecordsSubsystem::FindClassRecords(const UClass* Class)
{
	if (const FCrowdyMassClassServerRecords* Known = ClassRecords.Find(Class))
	{
		return *Known;
	}
	const FCrowdyMassDerivedServerRecords Derived = FCrowdyMassServerRecordsDerivation::FromClass(Class);
	UE_CLOG(!Derived.Reason.IsEmpty(), LogCrowdyMass, Warning, TEXT("[CrowdyMassServerRecords] %s"), *Derived.Reason);
	FCrowdyMassClassServerRecords Records;
	Records.Definition = Derived.bSupported ? Derived.Definition : nullptr;
	Records.InstanceId = Derived.InstanceId;
	Records.bArea = Derived.bArea;
	return ClassRecords.Add(Class, Records);
}

const FCrowdyMassFieldNames& UCrowdyMassServerRecordsSubsystem::FindFieldNames(const UCrowdyServerObject& Object)
{
	const FObjectKey Key(Object.GetDefinition());
	if (const FCrowdyMassFieldNames* Known = FieldNamesByDefinition.Find(Key))
	{
		return *Known;
	}
	FCrowdyMassFieldNames& Names = FieldNamesByDefinition.Add(Key);
	Object.GetFieldVariableNames(Names.ByField);
	for (const FString& Name : Names.ByField)
	{
		if (!Name.IsEmpty())
		{
			Names.All.Add(Name);
		}
	}
	return Names;
}

void UCrowdyMassServerRecordsSubsystem::HandleRowClassRecorded(const FGuid& UUID, const UClass* Class)
{
	// Recorded while a row spawns, so only the flush acquires anything. A class with no records component is left alone.
	if (!Class || !FindClassRecords(Class).Definition)
	{
		return;
	}
	if (!FollowedClasses.Contains(Class))
	{
		FollowedClasses.Add(Class);
		bFollowDirty = true;
	}
	FCrowdyMassRowRecord& Row = RowRecords.FindOrAdd(UUID);
	if (Row.RecordId.IsEmpty())
	{
		Row.RecordId = UUID.ToString(EGuidFormats::Digits);
	}
	AppearedRows.Add(UUID);
	ArmFlush();
}

void UCrowdyMassServerRecordsSubsystem::HandleRowRemoved(const FGuid& UUID)
{
	RowRecords.Remove(UUID);
}

void UCrowdyMassServerRecordsSubsystem::ArmFlush()
{
	UWorld* World = GetWorld();
	if (World && !World->GetTimerManager().TimerExists(FlushTimer))
	{
		FlushTimer = World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &UCrowdyMassServerRecordsSubsystem::FlushAppearedRows));
	}
}

void UCrowdyMassServerRecordsSubsystem::FlushAppearedRows()
{
	// The timer still exists while it runs, so a row recorded by a handler below must arm a new one.
	FlushTimer.Invalidate();
	if (bFollowDirty)
	{
		bFollowDirty = false;
		Refresh();
	}
	const TSet<FGuid> Appeared = MoveTemp(AppearedRows);
	AppearedRows.Reset();
	const TSet<FMassEntityHandle> CatchUps = MoveTemp(WatchersToCatchUp);
	WatchersToCatchUp.Reset();
	const UCrowdyMassEntitySubsystem* Entities = Rows.Get();
	if (!Entities || !HasListeners())
	{
		return;
	}
	FCrowdyMassHandlerBatch Batch;
	for (const FGuid& UUID : Appeared)
	{
		FMassEntityHandle Entity;
		const UCrowdyServerObject* Object = nullptr;
		if (!Entities->FindEntityByUUID(UUID, Entity) || !FindRecord(Entity, Object) || Object->GetStatus() != ECrowdyServerObjectStatus::Ready)
		{
			continue;
		}
		const UClass* Class = Entities->GetEntityClass(UUID);
		if (FindHandlers(Batch, Class).IsEmpty() && !HasWatcher(Entity))
		{
			continue;
		}
		const TArray<FString> Names = FindFieldNames(*Object).All;
		Dispatch(Entity, Class, Names, Batch);
	}
	for (const FMassEntityHandle Entity : CatchUps)
	{
		const UCrowdyServerObject* Object = nullptr;
		const FCrowdyMassServerValuesWatchersRef* Watchers = EntityWatchers.Find(Entity);
		if (!Watchers || !FindRecord(Entity, Object) || Object->GetStatus() != ECrowdyServerObjectStatus::Ready)
		{
			continue;
		}
		FCrowdyMassEntityTarget Target;
		Target.Entity = Entity;
		Target.World = GetWorld();
		const FCrowdyMassServerValuesWatchersRef Watching = *Watchers;
		const TArray<FString> Names = FindFieldNames(*Object).All;
		Watching->Broadcast(Target, Names);
	}
}

bool UCrowdyMassServerRecordsSubsystem::FindViewLocation(FVector& OutLocation) const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (ViewLocationForTest.IsSet())
	{
		OutLocation = *ViewLocationForTest;
		return true;
	}
#endif
	const UWorld* World = GetWorld();
	const APlayerController* Player = World ? World->GetFirstPlayerController() : nullptr;
	if (!Player)
	{
		return false;
	}
	if (const APawn* Pawn = Player->GetPawn())
	{
		OutLocation = Pawn->GetActorLocation();
		return true;
	}
	if (!Player->PlayerCameraManager)
	{
		return false;
	}
	OutLocation = Player->PlayerCameraManager->GetCameraLocation();
	return true;
}

void UCrowdyMassServerRecordsSubsystem::ForEachClassKey(const FCrowdyMassClassServerRecords& Records, bool bPlaced, int64 ChunkX, int64 ChunkY, int32 Reach,
	TFunctionRef<void(const FCrowdyServerObjectKey&)> Visit) const
{
	FCrowdyServerObjectKey Key;
	Key.Definition = Records.Definition;
	Key.InstanceId = Records.InstanceId;
	if (!Records.Definition)
	{
		return;
	}
	if (!Records.bArea)
	{
		Visit(Key);
		return;
	}
	if (!bPlaced)
	{
		return;
	}
	const int32 Size = FMath::Max(Records.Definition->AreaSizeChunks, 1);
	const int32 Radius = FMath::Clamp(Records.Definition->InterestRadiusAreas, 0, 3) + Reach;
	for (int32 Dx = -Radius; Dx <= Radius; ++Dx)
	{
		for (int32 Dy = -Radius; Dy <= Radius; ++Dy)
		{
			Key.InstanceId = UCrowdyServerObjectDefinition::MakeAreaKey(ChunkX + Dx * Size, ChunkY + Dy * Size, Size);
			Visit(Key);
		}
	}
}

void UCrowdyMassServerRecordsSubsystem::HandleLogin(bool bSuccess, FString Message)
{
	if (!bSuccess)
	{
		return;
	}
	bRetryFailedHeld = true;
	Refresh();
}

void UCrowdyMassServerRecordsSubsystem::Refresh()
{
	const bool bRetryFailed = bRetryFailedHeld;
	bRetryFailedHeld = false;
	UCrowdyServerObjectSubsystem* Objects = ServerObjects.Get();
	if (!Objects || (FollowedClasses.IsEmpty() && Held.IsEmpty()))
	{
		return;
	}

	FVector View;
	int64 ChunkX = 0;
	int64 ChunkY = 0;
	const bool bPlaced = FindViewLocation(View) && Objects->FindGroundChunk(View, ChunkX, ChunkY);
	// Areas are acquired within the radius and kept one area further out, so a view moving along a border does not flap.
	TArray<FCrowdyServerObjectKey, TInlineAllocator<16>> Wanted;
	TArray<FCrowdyServerObjectKey, TInlineAllocator<32>> Kept;
	for (int32 Index = 0; Index < FollowedClasses.Num(); ++Index)
	{
		const FCrowdyMassClassServerRecords& Records = FindClassRecords(FollowedClasses[Index]);
		ForEachClassKey(Records, bPlaced, ChunkX, ChunkY, 0, [&Wanted](const FCrowdyServerObjectKey& Key) { Wanted.AddUnique(Key); });
		ForEachClassKey(Records, bPlaced, ChunkX, ChunkY, 1, [&Kept](const FCrowdyServerObjectKey& Key) { Kept.AddUnique(Key); });
	}

	bool bChanged = false;
	TArray<UCrowdyServerObject*, TInlineAllocator<16>> Released;
	for (auto It = Held.CreateIterator(); It; ++It)
	{
		UCrowdyServerObject* Object = It.Value().Object;
		const ECrowdyServerObjectStatus Status = Object ? Object->GetStatus() : ECrowdyServerObjectStatus::Released;
		const bool bRetry = bRetryFailed && Status == ECrowdyServerObjectStatus::Failed;
		if (Object && Status != ECrowdyServerObjectStatus::Released && !bRetry && Kept.Contains(It.Key()))
		{
			continue;
		}
		if (Object)
		{
			Object->OnRecordsChanged.Remove(It.Value().RecordsHandle);
			Objects->Release(Object, this);
			Released.Add(Object);
		}
		It.RemoveCurrent();
		bChanged = true;
	}

	TArray<UCrowdyServerObject*, TInlineAllocator<16>> AlreadyReady;
	for (const FCrowdyServerObjectKey& Key : Wanted)
	{
		if (Held.Contains(Key))
		{
			continue;
		}
		FString Error;
		UCrowdyServerObject* Object = Objects->Acquire(Key.Definition, Key.InstanceId, this, Error);
		if (!Object)
		{
			bool bRefused = false;
			RefusedDefinitions.Add(FObjectKey(Key.Definition), &bRefused);
			UE_CLOG(!bRefused, LogCrowdyMass, Warning, TEXT("[CrowdyMassServerRecords] Crowd rows cannot read %s/%s: %s"), *Key.Definition->TypeName, *Key.InstanceId, *Error);
			continue;
		}
		FCrowdyMassHeldServerObject& Entry = Held.Add(Key);
		Entry.Object = Object;
		Entry.RecordsHandle = Object->OnRecordsChanged.AddUObject(this, &UCrowdyMassServerRecordsSubsystem::HandleRecordsChanged);
		bChanged = true;
		if (Object->GetStatus() == ECrowdyServerObjectStatus::Ready)
		{
			AlreadyReady.Add(Object);
		}
	}
	if (!bChanged)
	{
		return;
	}

	const TMap<TObjectPtr<const UClass>, FCrowdyMassHeldServerObjects> PreviousSources = MoveTemp(Sources);
	Sources.Reset();
	for (int32 Index = 0; Index < FollowedClasses.Num(); ++Index)
	{
		const FCrowdyMassClassServerRecords& Records = FindClassRecords(FollowedClasses[Index]);
		if (!Records.Definition)
		{
			continue;
		}
		FCrowdyMassHeldServerObjects& Source = Sources.FindOrAdd(FollowedClasses[Index]);
		ForEachClassKey(Records, bPlaced, ChunkX, ChunkY, 1, [this, &Source](const FCrowdyServerObjectKey& Key)
		{
			if (const FCrowdyMassHeldServerObject* Found = Held.Find(Key))
			{
				Source.Objects.AddUnique(Found->Object);
			}
		});
	}
	for (UCrowdyServerObject* Object : Released)
	{
		AnnounceReleased(*Object, PreviousSources);
	}
	// An object another owner already holds was read before this subsystem listened, so its records are announced now.
	for (UCrowdyServerObject* Object : AlreadyReady)
	{
		AnnounceAll(*Object);
	}
}

FCrowdyMassRowRecord* UCrowdyMassServerRecordsSubsystem::ResolveRow(const FString& RecordId, FMassEntityHandle& OutEntity, const UClass*& OutClass)
{
	const UCrowdyMassEntitySubsystem* Entities = Rows.Get();
	FGuid UUID;
	if (!Entities || !CrowdyMassServerRecords::ParseRecordId(RecordId, UUID) || !Entities->FindEntityByUUID(UUID, OutEntity))
	{
		return nullptr;
	}
	OutClass = Entities->GetEntityClass(UUID);
	return OutClass ? RowRecords.Find(UUID) : nullptr;
}

namespace CrowdyMassServerRecords
{
	bool Holds(const FCrowdyMassHeldServerObjects* Source, const UCrowdyServerObject* Object)
	{
		return Source && Object && Source->Objects.ContainsByPredicate([Object](const TObjectPtr<UCrowdyServerObject>& Each) { return Each.Get() == Object; });
	}
}

bool UCrowdyMassServerRecordsSubsystem::IsSourceOf(const UClass* Class, const UCrowdyServerObject* Object) const
{
	return CrowdyMassServerRecords::Holds(Sources.Find(Class), Object);
}

void UCrowdyMassServerRecordsSubsystem::AnnounceAll(UCrowdyServerObject& Object)
{
	if (!HasListeners())
	{
		return;
	}
	const TArray<FString> Names = FindFieldNames(Object).All;
	FCrowdyMassHandlerBatch Batch;
	for (const FString& RecordId : Object.GetRecordIds())
	{
		FMassEntityHandle Entity;
		const UClass* Class = nullptr;
		FCrowdyMassRowRecord* Row = ResolveRow(RecordId, Entity, Class);
		if (!Row || !IsSourceOf(Class, &Object))
		{
			continue;
		}
		Row->Announcer = &Object;
		FGuid UUID;
		FGuid::ParseExact(RecordId, EGuidFormats::Digits, UUID);
		// Its row has just heard every value, so the flush does not announce it again.
		AppearedRows.Remove(UUID);
		Dispatch(Entity, Class, Names, Batch);
	}
}

void UCrowdyMassServerRecordsSubsystem::AnnounceReleased(UCrowdyServerObject& Object, const TMap<TObjectPtr<const UClass>, FCrowdyMassHeldServerObjects>& PreviousSources)
{
	if (!HasListeners())
	{
		return;
	}
	FCrowdyMassHandlerBatch Batch;
	for (const FString& RecordId : Object.GetRecordIds())
	{
		FMassEntityHandle Entity;
		const UClass* Class = nullptr;
		FCrowdyMassRowRecord* Row = ResolveRow(RecordId, Entity, Class);
		const FCrowdyMassHeldServerObjects* Previous = Row ? PreviousSources.Find(Class) : nullptr;
		if (!CrowdyMassServerRecords::Holds(Previous, &Object) || IsRecordHeld(Class, RecordId))
		{
			continue;
		}
		Row->Announcer.Reset();
		Dispatch(Entity, Class, TConstArrayView<FString>(), Batch);
	}
}

void UCrowdyMassServerRecordsSubsystem::ReleaseAll()
{
	UCrowdyServerObjectSubsystem* Objects = ServerObjects.Get();
	for (const TPair<FCrowdyServerObjectKey, FCrowdyMassHeldServerObject>& Pair : Held)
	{
		UCrowdyServerObject* Object = Pair.Value.Object;
		if (!Object)
		{
			continue;
		}
		Object->OnRecordsChanged.Remove(Pair.Value.RecordsHandle);
		if (Objects)
		{
			Objects->Release(Object, this);
		}
	}
	Held.Empty();
	Sources.Empty();
}

void UCrowdyMassServerRecordsSubsystem::HandleRecordsChanged(UCrowdyServerObject* Object, TConstArrayView<FString> ChangedIds, TConstArrayView<FString> RemovedIds,
	TConstArrayView<TBitArray<>> ChangedFields)
{
	if (!Object)
	{
		return;
	}
	const bool bListened = HasListeners();
	FCrowdyMassHandlerBatch Batch;
	for (int32 Index = 0; Index < ChangedIds.Num(); ++Index)
	{
		FMassEntityHandle Entity;
		const UClass* Class = nullptr;
		FCrowdyMassRowRecord* Row = ResolveRow(ChangedIds[Index], Entity, Class);
		if (!Row || !IsSourceOf(Class, Object))
		{
			continue;
		}
		Row->Announcer = Object;
		if (!bListened || !ChangedFields.IsValidIndex(Index) || (FindHandlers(Batch, Class).IsEmpty() && !HasWatcher(Entity)))
		{
			continue;
		}
		const TArray<FString>& ByField = FindFieldNames(*Object).ByField;
		TArray<FString> Names;
		for (TConstSetBitIterator<> It(ChangedFields[Index]); It; ++It)
		{
			if (ByField.IsValidIndex(It.GetIndex()) && !ByField[It.GetIndex()].IsEmpty())
			{
				Names.Add(ByField[It.GetIndex()]);
			}
		}
#if WITH_DEV_AUTOMATION_TESTS
		++NamesBuiltForTest;
#endif
		if (!Names.IsEmpty())
		{
			Dispatch(Entity, Class, Names, Batch);
		}
	}
	for (const FString& RecordId : RemovedIds)
	{
		FMassEntityHandle Entity;
		const UClass* Class = nullptr;
		FCrowdyMassRowRecord* Row = ResolveRow(RecordId, Entity, Class);
		if (!Row || !IsSourceOf(Class, Object))
		{
			continue;
		}
		if (Row->Announcer.Get() == Object)
		{
			Row->Announcer.Reset();
		}
		// A record removed from one area as its entity moves into another is still held, so its row has not lost it.
		if (bListened && !IsRecordHeld(Class, RecordId))
		{
			Dispatch(Entity, Class, TConstArrayView<FString>(), Batch);
		}
	}
}

bool UCrowdyMassServerRecordsSubsystem::IsRecordHeld(const UClass* Class, const FString& RecordId) const
{
	const FCrowdyMassHeldServerObjects* Source = Sources.Find(Class);
	if (!Source)
	{
		return false;
	}
	return Source->Objects.ContainsByPredicate([&RecordId](const TObjectPtr<UCrowdyServerObject>& Object) { return Object && Object->GetRecord(RecordId); });
}

const FCrowdyMassHandlerList& UCrowdyMassServerRecordsSubsystem::FindHandlers(FCrowdyMassHandlerBatch& Batch, const UClass* Class)
{
	if (Batch.Version != HandlersVersion)
	{
		Batch.Entries.Reset();
		Batch.Version = HandlersVersion;
	}
	for (const FCrowdyMassHandlerBatch::FEntry& Known : Batch.Entries)
	{
		if (Known.Class == Class)
		{
			return Known.Handlers;
		}
	}
#if WITH_DEV_AUTOMATION_TESTS
	++HandlerListsBuiltForTest;
#endif
	FCrowdyMassHandlerBatch::FEntry& Entry = Batch.Entries.AddDefaulted_GetRef();
	Entry.Class = Class;
	for (const UClass* Each = Class; Each; Each = Each->GetSuperClass())
	{
		if (const TArray<FCrowdyMassServerValuesHandlerRef>* Found = HandlersByClass.Find(FObjectKey(Each)))
		{
			Entry.Handlers.Append(*Found);
		}
	}
	return Entry.Handlers;
}

void UCrowdyMassServerRecordsSubsystem::Dispatch(FMassEntityHandle Entity, const UClass* Class, TConstArrayView<FString> Changed, FCrowdyMassHandlerBatch& Batch)
{
	FCrowdyMassEntityTarget Target;
	Target.Entity = Entity;
	Target.World = GetWorld();

	// The batch holds the list until the next lookup, so a handler that adds or removes handlers does not change the list being run.
	const FCrowdyMassHandlerList& Running = FindHandlers(Batch, Class);
	for (const FCrowdyMassServerValuesHandlerRef& Handler : Running)
	{
		if (!RunHandler(*Handler, Target, Changed))
		{
			RemoveClassHandler(Handler->Handle);
		}
	}

	const FCrowdyMassServerValuesWatchersRef* Watchers = EntityWatchers.Find(Entity);
	if (!Watchers)
	{
		return;
	}
	const FCrowdyMassServerValuesWatchersRef Watching = *Watchers;
	Watching->Broadcast(Target, Changed);
}

bool UCrowdyMassServerRecordsSubsystem::RunHandler(FCrowdyMassServerValuesHandler& Handler, const FCrowdyMassEntityTarget& Target, TConstArrayView<FString> Changed)
{
	if (Handler.Native.IsBound())
	{
		Handler.Native.Execute(Target, Changed);
		return true;
	}
	UObject* Object = Handler.Object.Get();
	if (!::IsValid(Object))
	{
		return false;
	}
	// A recompiled Blueprint mints a new function under the same name, so a stale one is looked up again.
	UFunction* Function = Handler.Function.Get();
	if (!Function)
	{
		Function = Object->FindFunction(Handler.FunctionName);
		if (!Function || !CrowdyMassServerRecords::ResolveHandlerParams(*Function, Handler.TargetParam, Handler.ChangedParam))
		{
			return false;
		}
		Handler.Function = Function;
	}

	const int32 FrameSize = FMath::Max<int32>(Function->ParmsSize, 1);
	void* Frame = FMemory_Alloca(FrameSize);
	FMemory::Memzero(Frame, FrameSize);
	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		It->InitializeValue_InContainer(Frame);
	}
	Handler.TargetParam->CopyCompleteValue(Handler.TargetParam->ContainerPtrToValuePtr<void>(Frame), &Target);
	*Handler.ChangedParam->ContainerPtrToValuePtr<TArray<FString>>(Frame) = TArray<FString>(Changed);
	Object->ProcessEvent(Function, Frame);
	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		It->DestroyValue_InContainer(Frame);
	}
	return true;
}

FDelegateHandle UCrowdyMassServerRecordsSubsystem::AddClassHandler(const UClass* Class, FOnCrowdyMassServerValuesChanged::FDelegate Listener)
{
	if (!Class || !Listener.IsBound())
	{
		return FDelegateHandle();
	}
	FCrowdyMassServerValuesHandlerRef Handler = MakeShared<FCrowdyMassServerValuesHandler, ESPMode::NotThreadSafe>();
	Handler->Handle = FDelegateHandle(FDelegateHandle::GenerateNewHandle);
	Handler->Class = Class;
	Handler->Native = MoveTemp(Listener);
	HandlersByClass.FindOrAdd(FObjectKey(Class)).Add(Handler);
	++HandlersVersion;
	return Handler->Handle;
}

bool UCrowdyMassServerRecordsSubsystem::BindObjectHandler(const UClass* Class, UObject* Object, FName FunctionName)
{
	UFunction* Function = ::IsValid(Object) ? Object->FindFunction(FunctionName) : nullptr;
	const FStructProperty* TargetParam = nullptr;
	const FArrayProperty* ChangedParam = nullptr;
	if (!Class || !Function || !CrowdyMassServerRecords::ResolveHandlerParams(*Function, TargetParam, ChangedParam))
	{
		UE_LOG(LogCrowdyMass, Warning, TEXT("[CrowdyMassServerRecords] Cannot bind %s::%s to the server values of %s: it needs an entity class and a function taking a Crowdy Mass Entity Target then an array of names. Recompile the Blueprint that binds it."),
			*GetNameSafe(Object), *FunctionName.ToString(), *GetNameSafe(Class));
		return false;
	}
	TArray<FCrowdyMassServerValuesHandlerRef>& Handlers = HandlersByClass.FindOrAdd(FObjectKey(Class));
	const bool bBound = Handlers.ContainsByPredicate([Object, FunctionName](const FCrowdyMassServerValuesHandlerRef& Each)
	{
		return Each->Object.Get() == Object && Each->FunctionName == FunctionName;
	});
	if (bBound)
	{
		return true;
	}
	FCrowdyMassServerValuesHandlerRef Handler = MakeShared<FCrowdyMassServerValuesHandler, ESPMode::NotThreadSafe>();
	Handler->Handle = FDelegateHandle(FDelegateHandle::GenerateNewHandle);
	Handler->Class = Class;
	Handler->Object = Object;
	Handler->Function = Function;
	Handler->FunctionName = FunctionName;
	Handler->TargetParam = TargetParam;
	Handler->ChangedParam = ChangedParam;
	Handlers.Add(Handler);
	++HandlersVersion;
	return true;
}

bool UCrowdyMassServerRecordsSubsystem::UnbindObjectHandler(const UClass* Class, const UObject* Object, FName FunctionName)
{
	const FObjectKey Key(Class);
	TArray<FCrowdyMassServerValuesHandlerRef>* Handlers = Class && Object ? HandlersByClass.Find(Key) : nullptr;
	if (!Handlers)
	{
		return false;
	}
	const int32 Removed = Handlers->RemoveAll([Object, FunctionName](const FCrowdyMassServerValuesHandlerRef& Each)
	{
		return Each->Object.GetEvenIfUnreachable() == Object && Each->FunctionName == FunctionName;
	});
	// Emptied lists are dropped, so a world with no handlers left skips the per-record work entirely.
	if (Handlers->IsEmpty())
	{
		HandlersByClass.Remove(Key);
	}
	HandlersVersion += Removed > 0 ? 1 : 0;
	return Removed > 0;
}

bool UCrowdyMassServerRecordsSubsystem::RemoveClassHandler(FDelegateHandle Handle)
{
	for (auto It = HandlersByClass.CreateIterator(); It; ++It)
	{
		if (It.Value().RemoveAll([Handle](const FCrowdyMassServerValuesHandlerRef& Each) { return Each->Handle == Handle; }) == 0)
		{
			continue;
		}
		if (It.Value().IsEmpty())
		{
			It.RemoveCurrent();
		}
		++HandlersVersion;
		return true;
	}
	return false;
}

FDelegateHandle UCrowdyMassServerRecordsSubsystem::WatchEntity(FMassEntityHandle Entity, FOnCrowdyMassServerValuesChanged::FDelegate Listener)
{
	FCrowdyMassServerValuesWatchersRef* Watchers = EntityWatchers.Find(Entity);
	if (!Watchers)
	{
		Watchers = &EntityWatchers.Add(Entity, MakeShared<FOnCrowdyMassServerValuesChanged, ESPMode::NotThreadSafe>());
	}
	const FDelegateHandle Handle = (*Watchers)->Add(MoveTemp(Listener));
	// A watcher may start on a row whose record arrived long ago, so it hears the current values at the next flush, never inline.
	if (FindServerValues(Entity))
	{
		WatchersToCatchUp.Add(Entity);
		ArmFlush();
	}
	return Handle;
}

void UCrowdyMassServerRecordsSubsystem::UnwatchEntity(FMassEntityHandle Entity, FDelegateHandle Handle)
{
	FCrowdyMassServerValuesWatchersRef* Watchers = EntityWatchers.Find(Entity);
	if (!Watchers)
	{
		return;
	}
	(*Watchers)->Remove(Handle);
	if (!(*Watchers)->IsBound())
	{
		EntityWatchers.Remove(Entity);
	}
}

const FCrowdyServerRecord* UCrowdyMassServerRecordsSubsystem::FindRecord(FMassEntityHandle Entity, const UCrowdyServerObject*& OutObject) const
{
	OutObject = nullptr;
	const UCrowdyMassEntitySubsystem* Entities = Rows.Get();
	FGuid UUID;
	if (!Entities || !Entities->FindUUIDByEntity(Entity, UUID))
	{
		return nullptr;
	}
	const FCrowdyMassRowRecord* Row = RowRecords.Find(UUID);
	const UClass* Class = Row ? Entities->GetEntityClass(UUID) : nullptr;
	const FCrowdyMassHeldServerObjects* Source = Class ? Sources.Find(Class) : nullptr;
	if (!Source)
	{
		return nullptr;
	}
	// The object that announced the row's record last is current while an entity crosses between areas.
	const UCrowdyServerObject* Announcer = Row->Announcer.Get();
	const FCrowdyServerRecord* Record = CrowdyMassServerRecords::Holds(Source, Announcer) ? Announcer->GetRecord(Row->RecordId) : nullptr;
	if (Record)
	{
		OutObject = Announcer;
		return Record;
	}
	for (const TObjectPtr<UCrowdyServerObject>& Object : Source->Objects)
	{
		Record = Object ? Object->GetRecord(Row->RecordId) : nullptr;
		if (Record)
		{
			OutObject = Object;
			return Record;
		}
	}
	return nullptr;
}

const FInstancedStruct* UCrowdyMassServerRecordsSubsystem::FindServerValues(FMassEntityHandle Entity) const
{
	const UCrowdyServerObject* Object = nullptr;
	const FCrowdyServerRecord* Record = FindRecord(Entity, Object);
	return Record ? &Record->Values : nullptr;
}

bool UCrowdyMassServerRecordsSubsystem::ReadServerValueInto(FMassEntityHandle Entity, FName Name, const FProperty* DestProperty, void* Dest) const
{
	const UCrowdyServerObject* Object = nullptr;
	const FCrowdyServerRecord* Record = FindRecord(Entity, Object);
	const bool bReady = Object && Object->GetStatus() == ECrowdyServerObjectStatus::Ready;
	return UCrowdyServerObjectLibrary::ReadReadyValueInto(Record ? &Record->Values : nullptr, bReady, Name, DestProperty, Dest);
}

#if WITH_EDITOR
void UCrowdyMassServerRecordsSubsystem::HandleObjectsReplaced(const TMap<UObject*, UObject*>& ReplacementMap)
{
	auto Remap = [&ReplacementMap](UObject* Old) -> UObject*
	{
		UObject* const* New = Old ? ReplacementMap.Find(Old) : nullptr;
		return New && *New ? *New : Old;
	};
	for (TObjectPtr<const UClass>& Class : FollowedClasses)
	{
		Class = Cast<UClass>(Remap(const_cast<UClass*>(Class.Get())));
	}
	FollowedClasses.Remove(nullptr);

	TMap<FObjectKey, TArray<FCrowdyMassServerValuesHandlerRef>> Remapped;
	for (const TPair<FObjectKey, TArray<FCrowdyMassServerValuesHandlerRef>>& Pair : HandlersByClass)
	{
		for (const FCrowdyMassServerValuesHandlerRef& Handler : Pair.Value)
		{
			const UClass* Class = Cast<UClass>(Remap(const_cast<UClass*>(Handler->Class.GetEvenIfUnreachable())));
			UObject* Object = Remap(Handler->Object.GetEvenIfUnreachable());
			if (!Class)
			{
				continue;
			}
			Handler->Class = Class;
			Handler->Object = Object;
			Handler->Function.Reset();
			Remapped.FindOrAdd(FObjectKey(Class)).Add(Handler);
		}
	}
	HandlersByClass = MoveTemp(Remapped);
	++HandlersVersion;
	ClassRecords.Empty();
	FieldNamesByDefinition.Empty();
}

void UCrowdyMassServerRecordsSubsystem::HandleObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent)
{
	const bool bRecordsSource = Object && (Object->IsA<UCrowdyServerObjectComponent>() || Object->IsA<UCrowdyServerObjectDefinition>());
	if (bRecordsSource && PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive)
	{
		ClassRecords.Empty();
		FieldNamesByDefinition.Empty();
	}
}
#endif
