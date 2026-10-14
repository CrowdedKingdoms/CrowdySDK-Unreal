#pragma once

#include "CoreMinimal.h"
#include "CrowdyServerObjectTypes.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/Object.h"
#include "CrowdyServerObject.generated.h"

class UCrowdyServerObject;
class UCrowdyServerObjectDefinition;
class UCrowdyServerObjectSubsystem;
struct FCrowdyExecRecordUpdate;
struct FCrowdyExecStateHeader;
struct FCrowdyNativeExecPush;
struct FCrowdyNativeExecReply;

/** Fields names the watched values that changed, by their server names. */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnCrowdyServerValuesChanged, UCrowdyServerObject* /*Object*/, TConstArrayView<FString> /*Fields*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnCrowdyServerObjectStatusChanged, UCrowdyServerObject* /*Object*/, ECrowdyServerObjectStatus /*Status*/);
/** The ids of the records that are new or changed, then of those removed, then each changed record's changed State fields by index (see GetFieldVariableNames), in ChangedIds' order. */
DECLARE_MULTICAST_DELEGATE_FourParams(FOnCrowdyServerRecordsChanged, UCrowdyServerObject* /*Object*/, TConstArrayView<FString> /*ChangedIds*/, TConstArrayView<FString> /*RemovedIds*/,
	TConstArrayView<TBitArray<>> /*ChangedFields*/);

namespace CrowdyServerObjectDetail
{
	/** Whether a platform refusal says the app or a type is switched off or paused. */
	CROWDYEXEC_API bool IsSwitchedOffText(const FString& Message);
}

/** Handlers of one variable each (the value, then Has Value for an optional) on whatever Server Object their holder follows. */
struct CROWDYEXEC_API FCrowdyServerVariableBindings
{
	/** Binds Handler's Function to Variable, replacing its earlier binding of Function; runs it at once when Object is Ready. */
	void Bind(UCrowdyServerObject* Object, FName Variable, UObject* Handler, FName Function);

	void Unbind(FName Variable, const UObject* Handler, FName Function);

	/** Drops Handler's binding of Function, whatever its variable. */
	void UnbindHandler(const UObject* Handler, FName Function);

	/** Runs the handlers of Fields (server names) while Held is still the object announcing them, and drops dead handlers. */
	void Notify(const TObjectPtr<UCrowdyServerObject>& Held, TConstArrayView<FString> Fields);

	void Reset() { Bindings.Reset(); }
	int32 Num() const { return Bindings.Num(); }
	bool HasLiveHandler() const;

	/** Has the handlers read the record Id instead of the object's variables, or the variables again when bRecord is false. */
	void ReadRecord(bool bRecord, const FString& Id);

	/** The serial of the latest binding made anywhere; an announcement skips bindings made after it began. */
	static uint64 LatestSerial();

private:
	struct FBinding
	{
		FName Variable;
		/** Variable as ToVariableNames gives it, compared ignoring case. */
		FName KeptName;
		TWeakObjectPtr<UObject> Handler;
		FName Function;
		uint64 Serial = 0;

		bool operator==(const FBinding& Other) const { return Variable == Other.Variable && Handler == Other.Handler && Function == Other.Function; }
	};

	/** Runs Binding unless it is already running, so a handler that binds itself again does not recurse. */
	void Run(const UCrowdyServerObject& Object, const FBinding& Binding);
	void Call(const UCrowdyServerObject& Object, const FBinding& Binding) const;

	/** The values the handlers read: the object's variables, or the record RecordId when bReadsRecord. */
	const FInstancedStruct* FindValues(const UCrowdyServerObject& Object) const;

	TArray<FBinding> Bindings;
	TArray<TPair<const UObject*, FName>, TInlineAllocator<2>> Running;
	FString RecordId;
	bool bReadsRecord = false;
};

DECLARE_DYNAMIC_DELEGATE_TwoParams(FCrowdyServerVariablesEvent, UCrowdyServerObject*, Object, const TArray<FString>&, Changed);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCrowdyServerVariablesChanged, UCrowdyServerObject*, Object, const TArray<FString>&, Changed);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCrowdyServerStatusChanged, UCrowdyServerObject*, Object, ECrowdyServerObjectStatus, Status);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyServerRecordsChanged, UCrowdyServerObject*, Object, const TArray<FString>&, ChangedIds, const TArray<FString>&, RemovedIds);
DECLARE_DYNAMIC_DELEGATE_ThreeParams(FCrowdyServerRecordEvent, UCrowdyServerObject*, Object, const FString&, RecordId, const TArray<FString>&, Changed);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyServerRecordsUpdated, UCrowdyServerObject*, Object, const TArray<FCrowdyServerRecordChange>&, Changed, const TArray<FString>&, RemovedIds);

/**
 * One Server Object (a hub instance on the platform) as this player sees it: its watched values, kept current, and
 * its Server Functions. Shared by every owner that acquired it from UCrowdyServerObjectSubsystem. Game thread only.
 */
UCLASS(BlueprintType)
class CROWDYEXEC_API UCrowdyServerObject : public UObject
{
	GENERATED_BODY()

public:
	const UCrowdyServerObjectDefinition* GetDefinition() const { return Definition; }

	/** The definition asset this object was made from, to pass to nodes that take one. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects", DisplayName = "Get Definition")
	UCrowdyServerObjectDefinition* K2_GetDefinition() const { return Definition; }

	const FString& GetInstanceId() const { return InstanceId; }
	ECrowdyServerObjectStatus GetStatus() const { return Status; }
	/** Why it Failed, or, while Connecting or Ready, why the connection or the last read or subscribe failed; it is retried. */
	const FString& GetFailureReason() const { return FailureReason; }

	/** The watched values the server last sent, in the definition's State struct; its defaults until the first read. */
	const FInstancedStruct& GetState() const { return State; }

	/** GetState as a List to read by name, when State is a List; empty when it is a struct. */
	FInstancedPropertyBag GetStateList() const;

	/** Runs Listener after every change. When the values are already current it also runs once now, naming them all. */
	FDelegateHandle WatchValues(FOnCrowdyServerValuesChanged::FDelegate Listener);
	void UnwatchValues(FDelegateHandle Handle);

	FOnCrowdyServerObjectStatusChanged OnStatusChanged;

	/** Fields, by server name, as the names Get Server Value finds; names that are not variables, such as Members, stay as they are. */
	TArray<FString> ToVariableNames(TConstArrayView<FString> Fields) const;

	/** Handlers bound to one variable of this object; dropped when it is given back. */
	FCrowdyServerVariableBindings& GetVariableBindings() { return VariableBindings; }

	/** While a change is announced, the latest binding serial when it began; bindings made later already ran when bound. */
	uint64 GetAnnouncedSerial() const { return AnnouncedSerial; }

#if WITH_DEV_AUTOMATION_TESTS
	int32 NumVariableWatchersForTest() const { return VariableWatchers.Num(); }
	int32 NumRecordEventWatchersForTest() const { return RecordEventWatchers.Num(); }
	bool IsRecordWatchedForTest(const FString& Id) const { return IsRecordWatched(Id); }

	/** The result a call of Function gets from Reply, as the platform's answer would give it. */
	FCrowdyServerCallResult MakeCallResultForTest(FName Function, const FCrowdyNativeExecReply& Reply) const;

	/** The reason a refused read (or watch) gives while it retries; empty when it gives none. */
	FString WaitingReasonForTest(const FCrowdyNativeExecReply& Reply, bool bRead) const;
#endif

	/**
	 * Calls the Server Function the definition names Function. Params is the function's Params struct, or empty when it
	 * has none. Busy and unreachable servers are retried a few times. OnDone runs exactly once, on the game thread,
	 * except when the game instance shuts down first: pending calls are then dropped without running it.
	 * TimeoutSeconds above 0 is a limit of its own, counted from the call and covering retries and waiting to send: the call then
	 * completes as a Timeout, and an answer arriving later is not reported. It can only shorten the wait: the project's Server
	 * Object Call Timeout still ends each attempt. 0 leaves only that timeout.
	 */
	void Call(FName Function, const FInstancedStruct& Params, TFunction<void(const FCrowdyServerCallResult&)> OnDone, float TimeoutSeconds = 0.f);

	/** Calls a function that sends a List, with Params from MakeParams and its values set by name. A List reply reads by name through CrowdyExec::ToList. */
	void Call(FName Function, const FInstancedPropertyBag& Params, TFunction<void(const FCrowdyServerCallResult&)> OnDone, float TimeoutSeconds = 0.f);

	/** The List Function sends, with its default values, to set values on by name and pass to Call. Empty when it sends a struct or nothing. */
	FInstancedPropertyBag MakeParams(FName Function) const;

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects", DisplayName = "Get Status")
	ECrowdyServerObjectStatus K2_GetStatus() const { return Status; }

	/** Why it Failed, or, while Connecting or Ready, why the connection or the last read failed; it is retried. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects", DisplayName = "Get Failure Reason")
	FString K2_GetFailureReason() const { return FailureReason; }

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects", DisplayName = "Get Instance Id")
	FString K2_GetInstanceId() const { return InstanceId; }

	/** The variables the server last sent; read one with Get Server Value. Their defaults until the object is Ready. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects", DisplayName = "Get Variables")
	FInstancedStruct K2_GetVariables() const { return State; }

	/** Function's inputs with their default values, to set with Set Server Value and pass to Call Server Function. Empty when it takes none. */
	UFUNCTION(BlueprintCallable, BlueprintPure = false, Category = "Crowdy SDK|Server Objects", DisplayName = "Make Inputs")
	FInstancedStruct MakeInputs(FName Function) const;

	/** Runs Event after every change of the variables, naming them as Get Server Value takes them. When they are already current it also runs once now, naming them all. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Server Objects", DisplayName = "Watch Variables")
	void K2_WatchVariables(FCrowdyServerVariablesEvent Event);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Server Objects", DisplayName = "Stop Watching Variables")
	void K2_UnwatchVariables(FCrowdyServerVariablesEvent Event);

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Server Objects", meta = (DisplayName = "On Status Changed"))
	FCrowdyServerStatusChanged StatusChanged;

	/** The members' user ids in the order they joined, for members kept by the object; empty otherwise. A change is named Members. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects")
	TArray<int64> GetMembers() const { return Members; }

	/** How many members there are, for members kept by the object, even when the list is not shown. A change is named MemberCount. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects")
	int32 GetMemberCount() const { return MemberCount; }

	/** The leader's user id, for members kept by the object; 0 when there is none. A change is named Leader. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects")
	int64 GetLeader() const { return Leader; }

	/** Whether Join is accepted, for members kept by the object. A change is named OpenForJoining. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects")
	bool IsOpenForJoining() const { return bOpenForJoining; }

	/** Whether the signed-in player is a member: from the last read or a shown member list. A Join or Leave reads again. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects")
	bool IsMember() const { return bIsMember; }

	/** Whether the signed-in player is the leader: from the last read, or the leader a change carried. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects")
	bool IsLeader() const { return bIsLeader; }

	/** Whether the definition keeps a record per entity rather than one set of variables. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects")
	bool HoldsRecords() const;

	/** The record Id as the server last sent it, or null when there is none. Ids compare case-sensitively. */
	const FCrowdyServerRecord* GetRecord(const FString& Id) const { return Records.Find(Id); }

	/** How many records the object holds now. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects")
	int32 NumRecords() const { return Records.Num(); }

	/** The variables, or with RecordId that record's values; null when that record is not there. */
	const FInstancedStruct* FindValues(const FString* RecordId) const;

	/** After every read, push or changes that changed or removed records. */
	FOnCrowdyServerRecordsChanged OnRecordsChanged;

	/** For each State field by index, its name as Get Server Value takes it when it is watched, else empty. */
	void GetFieldVariableNames(TArray<FString>& OutNames) const;

#if WITH_DEV_AUTOMATION_TESTS
	/** Stands in for a push of records: sets Changed's values, drops Removed, makes the object Ready and announces them. */
	void ApplyRecordsForTest(TConstArrayView<TPair<FString, FInstancedStruct>> Changed, TConstArrayView<FString> Removed);
#endif

	/** Runs Listener, naming the variables, after each change of record Id and when it is removed. When it is already current it also runs once now, naming them all. */
	FDelegateHandle WatchRecord(const FString& Id, FOnCrowdyServerValuesChanged::FDelegate Listener);
	void UnwatchRecord(const FString& Id, FDelegateHandle Handle);

	/** Runs Event after each change of the record RecordId and when it is removed (then Find Record is false), naming the changed variables as Get Server Value takes them. When the record is already current it also runs once now, naming them all. Records dropped because the object Failed are not reported; On Status Changed is. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Server Objects", DisplayName = "Watch Record")
	void K2_WatchRecord(const FString& RecordId, FCrowdyServerRecordEvent Event);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Server Objects", DisplayName = "Stop Watching Record")
	void K2_UnwatchRecord(const FString& RecordId, FCrowdyServerRecordEvent Event);

	/** For a Server Object that holds a record per entity: the record Id's variables, owner's user id and team. False when there is no such record or the object is not Ready. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects", DisplayName = "Find Record")
	bool K2_FindRecord(const FString& Id, FInstancedStruct& Values, int64& Owner, FString& Team) const;

	/** For a Server Object that holds a record per entity: the id of every record, in no particular order. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects")
	TArray<FString> GetRecordIds() const;

	/** For a Server Object that holds a record per entity, after every change: the ids of the records that are new or changed, then of those removed. */
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Server Objects", meta = (DisplayName = "On Records Changed"))
	FCrowdyServerRecordsChanged RecordsChanged;

	/** Like On Records Changed, but also naming each changed record's changed variables, as Get Server Value takes them; none when only its owner or team changed. */
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Server Objects", meta = (DisplayName = "On Records Updated"))
	FCrowdyServerRecordsUpdated RecordsUpdated;

private:
	friend class UCrowdyServerObjectSubsystem;

	void Setup(UCrowdyServerObjectSubsystem* InSubsystem, UCrowdyServerObjectDefinition* InDefinition, const FString& InInstanceId);

	void AddOwner(const UObject* Owner);
	void RemoveOwner(const UObject* Owner);

	/** Drops owners that no longer exist; true while at least one remains. */
	bool PruneOwners();

	/** The connection is open: subscribe to the watched values if not yet, then read them. */
	void HandleConnected();

	/** The connection came back after a drop: pushes sent meanwhile are lost, so read again. */
	void HandleReconnected();

	/** The signed-in player's Crowdy Teams changed: records whose teams are Crowdy Teams are read again. */
	void HandleCrowdyTeamsChanged();

	/** While Connecting, gives why the connection is not open yet as the reason; an empty Reason clears only a reason this gave. */
	void SetConnectProblem(const FString& Reason);

	/** Starts over from Connecting, for a Failed object acquired again. */
	void Restart();

	/** Fails every pending call as Canceled and moves to Failed. Broadcasts only when bNotify. */
	void Fail(const FString& Reason, bool bNotify);

	/** Terminal. Stops listening; pending calls complete as Canceled when bNotify, and are dropped silently otherwise. */
	void MarkReleased(bool bNotify);

	/** Advances retry timers by DeltaSeconds. */
	void Tick(float DeltaSeconds);

	void CallWith(FName Function, const UScriptStruct* ParamsStruct, const void* Params, TFunction<void(const FCrowdyServerCallResult&)> OnDone, float TimeoutSeconds);

	UPROPERTY()
	TObjectPtr<UCrowdyServerObjectDefinition> Definition;

	TWeakObjectPtr<UCrowdyServerObjectSubsystem> Subsystem;
	FString InstanceId;
	ECrowdyServerObjectStatus Status = ECrowdyServerObjectStatus::Connecting;
	FString FailureReason;
	/** Whether FailureReason is a refused read's, which the next good read clears. */
	bool bReadSetReason = false;
	/** Whether FailureReason is a failed connect's, which the next good connect clears. */
	bool bConnectSetReason = false;

	UPROPERTY()
	FInstancedStruct State;

	TArray<int64> Members;
	int32 MemberCount = 0;
	int64 Leader = 0;
	bool bOpenForJoining = true;
	bool bIsMember = false;
	bool bIsLeader = false;

	TArray<TWeakObjectPtr<const UObject>> Owners;

	/** Seconds left before an object without owners is given back; negative while it has owners. */
	float GraceRemaining = -1.f;

	FOnCrowdyServerValuesChanged ValuesChanged;

	/** Blueprint watchers; one whose object is gone is dropped at the next change. */
	TArray<FCrowdyServerVariablesEvent> VariableWatchers;

	struct FRecordEventWatcher
	{
		FString RecordId;
		FCrowdyServerRecordEvent Event;
		FDelegateHandle Handle;

		bool Watches(const FString& Id, const FCrowdyServerRecordEvent& Other) const { return RecordId.Equals(Id, ESearchCase::CaseSensitive) && Event == Other; }
	};

	/** Blueprint record watchers, each riding WatchRecord; one whose object is gone is dropped at the next change of its record. */
	TArray<FRecordEventWatcher> RecordEventWatchers;

	/** Stops the record watchers whose Blueprint object is gone. */
	void PruneRecordEventWatchers();

	/** Broadcasts RecordsUpdated for the records NotifyRecords announces. */
	void BroadcastRecordsUpdated(const TArray<FString>& Changed, const TArray<TBitArray<>>& ChangedFields, const TArray<FString>& Removed);

	FCrowdyServerVariableBindings VariableBindings;
	uint64 AnnouncedSerial = MAX_uint64;

	/** Sets Status without broadcasting it. */
	void SetStatus(ECrowdyServerObjectStatus NewStatus);

	/** Broadcasts OnStatusChanged and StatusChanged. */
	void NotifyStatus();

	void TraceReread(const TCHAR* Reason) const;

	/** Broadcasts ValuesChanged and runs every Blueprint watcher. */
	void NotifyValues(const TArray<FString>& Fields);

	struct FPendingCall
	{
		FName Function;
		FString Method;
		TArray<uint8> Payload;
		const UScriptStruct* ReplyStruct = nullptr;
		/** The reply's name for messages: its List's name or its struct's. */
		FString ReplyName;
		TFunction<void(const FCrowdyServerCallResult&)> OnDone;
		/** When the call was made, in FPlatformTime seconds. */
		double MadeAt = 0.0;
		float RetryIn = 0.f;
		/** Ticked time spent waiting for a connection to send it on. */
		float UnsentSeconds = 0.f;
		/** The caller's own limit in ticked seconds from the call; 0 or less for none. */
		float TimeoutSeconds = 0.f;
		float ElapsedSeconds = 0.f;
		int32 Attempts = 0;
		bool bInFlight = false;
		/** Canceled by a connection closed elsewhere; completed on the next Tick, never inline. */
		bool bConnectionClosed = false;
	};

	/** Subscribes to the watched values if not yet, then reads them. */
	void Refresh();
	void RequestRead();
	/** Sends the read, or the next page of a read of records; schedules a refresh when there is no connection. */
	void SendRead();
	void HandleSubscribeDone(uint32 Attempt, const FCrowdyNativeExecReply& Reply);
	void HandleReadDone(const FCrowdyNativeExecReply& Reply);
	/** Clears a reason and backoff the last good read makes stale. */
	void NoteGoodRead();
	void HandleRecordsPage(const FCrowdyNativeExecReply& Reply);
	/** Replaces the records with the pages read, then announces what changed. */
	void CommitRecordsRead();
	void ApplyRecordsPush(const TArray<uint8>& Payload);
	/** Whether a push at Header applies now; otherwise it is stale, or a read is asked for. */
	bool AcceptsPush(const FCrowdyExecStateHeader& Header);
	/** Puts the Owner Only and Team Only values of Update back to Defaults, marking those that change. */
	void ResetRestrictedFields(FCrowdyExecRecordUpdate& Update, FInstancedStruct& Defaults) const;
	/** Asks for the restricted values that changed, at most one call in flight and one a second; a request meanwhile waits for the next. */
	void RequestChanges();
	void SendDueChanges();
	void SendChangesPage();
	void HandleChangesDone(uint32 Ticket, const FCrowdyNativeExecReply& Reply);
	/** Fails the object for a permanent cause, as a refused read would; otherwise asks again after the refresh backoff. */
	void HandleChangesFailure(const FCrowdyNativeExecReply& Reply);
	/** Why a refusal will not change on retry, or empty when retrying may help. */
	FString PermanentRefusal(const FCrowdyNativeExecReply& Reply, const TCHAR* Verb) const;
	/** The team of the first record, by id, the signed-in player owns; empty when they own none. */
	FString FindOwnTeam() const;
	/** Whether a listener watches record Id, so its changed names are worth collecting. */
	bool IsRecordWatched(const FString& Id) const;
	/** The watched names of the fields Written marks. */
	void CollectWrittenFields(const TBitArray<>& Written, TArray<FString>& OutFields) const;
	/** Marks, by State field index, the watched fields that differ between Before and After, or every watched field when Before is null; true when any does. */
	bool CollectWatchedFieldBits(const void* Before, const void* After, TBitArray<>& OutFields) const;
	/** Announces changed and removed records, then runs the watchers of each in Watched with its changed names. ChangedFields runs parallel to Changed. */
	void NotifyRecords(const TArray<FString>& Changed, const TArray<TBitArray<>>& ChangedFields, const TArray<FString>& Removed,
		const TArray<TPair<FString, TArray<FString>>>& Watched);
	/** A refused read or subscribe: fails the object for a permanent cause, otherwise retries. */
	void HandleRefreshFailure(const FCrowdyNativeExecReply& Reply, bool bRead);
	void ScheduleRefresh(const FCrowdyNativeExecReply& Reply);
	void ReceivePush(const TArray<uint8>& Payload);
	void ApplyPush(const TArray<uint8>& Payload);
	void ReplayBufferedPushes();

	/** Names the watched fields that differ between Before and After, or every watched field when Before is null. */
	void CollectWatchedFields(const void* Before, const void* After, TArray<FString>& OutFields) const;

	void TickCalls(float DeltaSeconds);
	void CompleteCall(uint32 CallId, const FCrowdyServerCallResult& Result);
	void TraceAnswered(const FPendingCall& Pending, ECrowdyServerCallOutcome Outcome) const;
	void SendDueCalls();
	void SendCall(uint32 CallId);
	void HandleCallDone(uint32 CallId, const FCrowdyNativeExecReply& Reply);
	FCrowdyServerCallResult MakeCallResult(const FPendingCall& Pending, const FCrowdyNativeExecReply& Reply) const;
	/** Whether a successful answer to Pending must be followed by a read of Is Member and Is Leader. */
	bool ReadsMembershipAfter(const FPendingCall& Pending) const;
	/** Reads again after an answer that changed membership the server does not push, or after a crash rolled the values back. */
	void ReadAfterAnswer(bool bReadMembership, const FCrowdyNativeExecReply& Reply);
	/** The answer to a call already reported as timed out: still reads again when that answer calls for it. */
	void HandleLateReply(uint32 CallId, const FCrowdyNativeExecReply& Reply);
	/** Calls in flight when their own Timeout ran out, and whether a successful answer reads membership. */
	TMap<uint32, bool> LateCalls;

	/** Drops the subscription, reads, buffered pushes and retry timers, and turns away every callback still to come. */
	void StopListening();

	TMap<uint32, FPendingCall> PendingCalls;
	TArray<TArray<uint8>> BufferedPushes;
	uint64 SubscribeHandle = 0;
	uint64 AppliedEpoch = 0;
	uint64 AppliedSeq = 0;
	float RefreshRetryIn = 0.f;
	int32 RefreshFailures = 0;
	uint32 Generation = 0;
	uint32 NextCallId = 0;
	uint32 SubscribeAttempt = 0;
	bool bHasRead = false;
	bool bReadInFlight = false;
	bool bReadAgain = false;
	bool bRefreshScheduled = false;
	/** Whether the server accepted the current subscribe; SubscribeHandle is set as soon as it is sent. */
	bool bSubscribeConfirmed = false;

	TCrowdyRecordMap<FCrowdyServerRecord> Records;
	/** The pages of the read in progress, which replace Records once the last arrives. */
	TCrowdyRecordMap<FCrowdyServerRecord> PagedRecords;
	/** Shared, so a watcher that stops watching during an announcement does not destroy the delegate running it. */
	TCrowdyRecordMap<TSharedRef<FOnCrowdyServerValuesChanged>> RecordWatchers;
	/** The last id of the last page read; the next page starts after it. */
	FString PageAfter;
	/** The first page's, which a read keeps: pushes above its seq are replayed over the later pages. */
	uint64 PageEpoch = 0;
	uint64 PageSeq = 0;
	uint64 PageRSeq = 0;
	bool bFirstPage = true;
	/** Bytes of BufferedPushes, which a read of records bounds instead of their count. */
	int64 BufferedBytes = 0;
	/** The restricted seq the next changes asks from: the first page's of the last read or changes. */
	uint64 AppliedRSeq = 0;
	uint64 ChangesSince = 0;
	uint64 ChangesFirstRSeq = 0;
	FString ChangesAfter;
	/** Seconds before another changes call may be sent. */
	float ChangesWait = 0.f;
	int32 ChangesPages = 0;
	int32 ChangesFailures = 0;
	/** A page named a record not held yet, so the restricted seq is kept and changes asked for again. */
	bool bChangesMissed = false;
	/** Raised by every read, so the answer to a changes call sent before it is dropped. */
	uint32 ChangesTicket = 0;
	bool bChangesInFlight = false;
	bool bChangesWanted = false;
	bool bChangesFirstPage = true;
	FString OwnTeam;
};
