#pragma once

#include "CoreMinimal.h"
#include "CrowdyServerObjectTypes.h"
#include "Engine/TimerHandle.h"
#include "Mass/EntityHandle.h"
#include "Subsystems/WorldSubsystem.h"
#include "Target/CrowdyMassEntityTarget.h"
#include "Templates/SharedPointer.h"
#include "UObject/ObjectKey.h"
#include "CrowdyMassServerRecordsSubsystem.generated.h"

class FArrayProperty;
class FProperty;
class FStructProperty;
class UCrowdyMassEntitySubsystem;
class UCrowdySDKSubsystem;
class UCrowdyServerObject;
class UCrowdyServerObjectDefinition;
class UCrowdyServerObjectSubsystem;
struct FCrowdyServerRecord;
struct FPropertyChangedEvent;

/** Target is the row whose server values changed; Changed names the variables as Get Server Value takes them, empty when its record was removed. */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnCrowdyMassServerValuesChanged, const FCrowdyMassEntityTarget& /*Target*/, TConstArrayView<FString> /*Changed*/);

/** Which Server Object holds the records of one entity class's rows. */
USTRUCT()
struct CROWDYMASS_API FCrowdyMassClassServerRecords
{
	GENERATED_BODY()

	/** Null when rows of the class get no server values. */
	UPROPERTY()
	TObjectPtr<UCrowdyServerObjectDefinition> Definition;

	/** The instance every row of the class reads; empty when rows follow the areas around the view. */
	UPROPERTY()
	FString InstanceId;

	UPROPERTY()
	bool bArea = false;
};

USTRUCT()
struct FCrowdyMassHeldServerObject
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UCrowdyServerObject> Object;

	FDelegateHandle RecordsHandle;
};

/** The held objects one class's rows read: its one instance, or its areas within reach of the view. */
USTRUCT()
struct FCrowdyMassHeldServerObjects
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<TObjectPtr<UCrowdyServerObject>> Objects;
};

/** A row's record id, written once, and the object that last announced its record. */
struct FCrowdyMassRowRecord
{
	FString RecordId;
	TWeakObjectPtr<UCrowdyServerObject> Announcer;
};

/** One definition's State fields by index, named as Get Server Value takes them (empty when not watched), and all the watched ones. */
struct FCrowdyMassFieldNames
{
	TArray<FString> ByField;
	TArray<FString> All;
};

/** One handler of server value changes on the rows of a class: a native delegate, or a function bound on an object. */
struct FCrowdyMassServerValuesHandler
{
	FDelegateHandle Handle;
	TWeakObjectPtr<const UClass> Class;
	FOnCrowdyMassServerValuesChanged::FDelegate Native;
	TWeakObjectPtr<UObject> Object;
	TWeakObjectPtr<UFunction> Function;
	FName FunctionName;
	const FStructProperty* TargetParam = nullptr;
	const FArrayProperty* ChangedParam = nullptr;
};

using FCrowdyMassServerValuesHandlerRef = TSharedRef<FCrowdyMassServerValuesHandler, ESPMode::NotThreadSafe>;
using FCrowdyMassServerValuesWatchersRef = TSharedRef<FOnCrowdyMassServerValuesChanged, ESPMode::NotThreadSafe>;
using FCrowdyMassHandlerList = TArray<FCrowdyMassServerValuesHandlerRef, TInlineAllocator<8>>;

/** The handlers of each class's rows, worked out once for a batch of changes and again only if a handler is added or removed meanwhile. */
struct FCrowdyMassHandlerBatch
{
	struct FEntry
	{
		const UClass* Class = nullptr;
		FCrowdyMassHandlerList Handlers;
	};

	uint32 Version = 0;
	TArray<FEntry, TInlineAllocator<4>> Entries;
};

/**
 * Gives crowd rows the record their actor class's Crowdy Server Object component reads, holding the Server Objects the
 * rows' classes use (one instance, or the areas around the local view) and reading a row's values from them on demand.
 */
UCLASS()
class CROWDYMASS_API UCrowdyMassServerRecordsSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	static constexpr float FollowSeconds = 0.5f;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	/** The record values of the row Entity, or null when no held object holds its record. */
	const FInstancedStruct* FindServerValues(FMassEntityHandle Entity) const;

	/** Reads the row's variable Name into Dest of type DestProperty; true only for a Ready object's value that fits. */
	bool ReadServerValueInto(FMassEntityHandle Entity, FName Name, const FProperty* DestProperty, void* Dest) const;

	/** Runs Listener after each change of the server values of a row of Class or a subclass. */
	FDelegateHandle AddClassHandler(const UClass* Class, FOnCrowdyMassServerValuesChanged::FDelegate Listener);

	/** Runs FunctionName on Object, taking the row's entity target then the changed names, as AddClassHandler runs a listener. Binding it again changes nothing. */
	bool BindObjectHandler(const UClass* Class, UObject* Object, FName FunctionName);

	bool UnbindObjectHandler(const UClass* Class, const UObject* Object, FName FunctionName);
	bool RemoveClassHandler(FDelegateHandle Handle);

	/** Runs Listener after each change of the server values of the row Entity, and once at the next flush when its record is already there. */
	FDelegateHandle WatchEntity(FMassEntityHandle Entity, FOnCrowdyMassServerValuesChanged::FDelegate Listener);
	void UnwatchEntity(FMassEntityHandle Entity, FDelegateHandle Handle);

	/** Which Server Object holds the records of Class's rows, worked out once per class. */
	const FCrowdyMassClassServerRecords& FindClassRecords(const UClass* Class);

#if WITH_DEV_AUTOMATION_TESTS
	/** Stands in for Initialize on a subsystem built outside a world. Arms no timer. */
	void InitializeForTest(UCrowdyMassEntitySubsystem* InRows, UCrowdyServerObjectSubsystem* InServerObjects);
	void SetViewLocationForTest(const FVector& Location) { ViewLocationForTest = Location; }
	/** What the next frame and the follow timer would do: announce appeared rows, then follow the view. */
	void FlushForTest() { FlushAppearedRows(); }
	void RefreshForTest() { Refresh(); }
	UCrowdyServerObject* FindHeldForTest(UCrowdyServerObjectDefinition* Definition, const FString& InstanceId) const;
	int32 NumHeldForTest() const { return Held.Num(); }
	int32 NumRowRecordsForTest() const { return RowRecords.Num(); }
	/** What the SDK's sign-in event does: held objects that Failed are acquired again. */
	void SignInForTest(bool bSuccess) { HandleLogin(bSuccess, FString()); }
	/** How many times changed variable names were built for a row. */
	int32 NamesBuiltForTest = 0;
	/** How many times a class's handler list was worked out. */
	int32 HandlerListsBuiltForTest = 0;
#endif

private:
	/** Acquires what the followed classes read within their reach and releases what lies beyond it, one area further out. */
	void Refresh();

	/** A sign-in may have given the player the access a Failed object lacked, so those held are acquired again. */
	UFUNCTION()
	void HandleLogin(bool bSuccess, FString Message);

	void HandleRowClassRecorded(const FGuid& UUID, const UClass* Class);
	void HandleRowRemoved(const FGuid& UUID);
	void ArmFlush();
	void FlushAppearedRows();
	void HandleRecordsChanged(UCrowdyServerObject* Object, TConstArrayView<FString> ChangedIds, TConstArrayView<FString> RemovedIds, TConstArrayView<TBitArray<>> ChangedFields);

	/** The row whose record id is RecordId, when it has a recorded class. */
	FCrowdyMassRowRecord* ResolveRow(const FString& RecordId, FMassEntityHandle& OutEntity, const UClass*& OutClass);
	/** Calls Visit with each key Records reads within Reach areas beyond its radius of the view, or its one instance. */
	void ForEachClassKey(const FCrowdyMassClassServerRecords& Records, bool bPlaced, int64 ChunkX, int64 ChunkY, int32 Reach, TFunctionRef<void(const FCrowdyServerObjectKey&)> Visit) const;
	bool IsSourceOf(const UClass* Class, const UCrowdyServerObject* Object) const;
	bool HasListeners() const { return !HandlersByClass.IsEmpty() || !EntityWatchers.IsEmpty(); }
	const FCrowdyMassFieldNames& FindFieldNames(const UCrowdyServerObject& Object);

	/** The handlers of Class and its superclasses, from Batch when it already has them. */
	const FCrowdyMassHandlerList& FindHandlers(FCrowdyMassHandlerBatch& Batch, const UClass* Class);
	bool HasWatcher(FMassEntityHandle Entity) const { return EntityWatchers.Contains(Entity); }

	void Dispatch(FMassEntityHandle Entity, const UClass* Class, TConstArrayView<FString> Changed, FCrowdyMassHandlerBatch& Batch);
	/** False when the handler is dead and should be dropped. */
	bool RunHandler(FCrowdyMassServerValuesHandler& Handler, const FCrowdyMassEntityTarget& Target, TConstArrayView<FString> Changed);

	/** Announces every record Object holds, with all its variables. */
	void AnnounceAll(UCrowdyServerObject& Object);
	/** Announces as removed the records Object gave a class's rows that no object the class still reads holds. */
	void AnnounceReleased(UCrowdyServerObject& Object, const TMap<TObjectPtr<const UClass>, FCrowdyMassHeldServerObjects>& PreviousSources);

	const FCrowdyServerRecord* FindRecord(FMassEntityHandle Entity, const UCrowdyServerObject*& OutObject) const;
	bool IsRecordHeld(const UClass* Class, const FString& RecordId) const;
	bool FindViewLocation(FVector& OutLocation) const;
	void ReleaseAll();

#if WITH_EDITOR
	void HandleObjectsReplaced(const TMap<UObject*, UObject*>& ReplacementMap);
	void HandleObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent);
	FDelegateHandle ObjectsReplacedHandle;
	FDelegateHandle ObjectPropertyChangedHandle;
#endif

	TWeakObjectPtr<UCrowdyMassEntitySubsystem> Rows;
	TWeakObjectPtr<UCrowdyServerObjectSubsystem> ServerObjects;
	TWeakObjectPtr<UCrowdySDKSubsystem> SDK;
	FDelegateHandle RowClassHandle;
	FDelegateHandle RowRemovedHandle;
	FTimerHandle FollowTimer;
	FTimerHandle FlushTimer;

	UPROPERTY(Transient)
	TMap<TObjectPtr<const UClass>, FCrowdyMassClassServerRecords> ClassRecords;

	/** The classes rows of this world were recorded as; only these are followed. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<const UClass>> FollowedClasses;

	UPROPERTY(Transient)
	TMap<FCrowdyServerObjectKey, FCrowdyMassHeldServerObject> Held;

	/** What each followed class's rows read. */
	UPROPERTY(Transient)
	TMap<TObjectPtr<const UClass>, FCrowdyMassHeldServerObjects> Sources;

	TMap<FGuid, FCrowdyMassRowRecord> RowRecords;
	TMap<FObjectKey, FCrowdyMassFieldNames> FieldNamesByDefinition;

	/** Rows recorded since the last flush, announced once with their current values. */
	TSet<FGuid> AppearedRows;

	/** Rows a watcher started watching while their record was there, caught up at the next flush. */
	TSet<FMassEntityHandle> WatchersToCatchUp;

	/** A class was first followed since the last flush, so the flush acquires what it reads. */
	bool bFollowDirty = false;

	TMap<FObjectKey, TArray<FCrowdyMassServerValuesHandlerRef>> HandlersByClass;
	TMap<FMassEntityHandle, FCrowdyMassServerValuesWatchersRef> EntityWatchers;

	/** Raised whenever HandlersByClass changes, so a batch knows its lists are out of date. */
	uint32 HandlersVersion = 0;

	/** A sign-in happened since the last refresh: held objects that Failed are let go and acquired again. */
	bool bRetryFailedHeld = false;

	/** Definitions an acquire refused, said once each. */
	TSet<FObjectKey> RefusedDefinitions;

#if WITH_DEV_AUTOMATION_TESTS
	TOptional<FVector> ViewLocationForTest;
#endif
};
