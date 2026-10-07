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
struct FCrowdyNativeExecPush;
struct FCrowdyNativeExecReply;

/** Fields names the watched values that changed, by their server names. */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnCrowdyServerValuesChanged, UCrowdyServerObject* /*Object*/, TConstArrayView<FString> /*Fields*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnCrowdyServerObjectStatusChanged, UCrowdyServerObject* /*Object*/, ECrowdyServerObjectStatus /*Status*/);

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
	static void Call(const UCrowdyServerObject& Object, const FBinding& Binding);

	TArray<FBinding> Bindings;
	TArray<TPair<const UObject*, FName>, TInlineAllocator<2>> Running;
};

DECLARE_DYNAMIC_DELEGATE_TwoParams(FCrowdyServerVariablesEvent, UCrowdyServerObject*, Object, const TArray<FString>&, Changed);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCrowdyServerVariablesChanged, UCrowdyServerObject*, Object, const TArray<FString>&, Changed);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCrowdyServerStatusChanged, UCrowdyServerObject*, Object, ECrowdyServerObjectStatus, Status);

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
	const FString& GetInstanceId() const { return InstanceId; }
	ECrowdyServerObjectStatus GetStatus() const { return Status; }
	/** Why it Failed, or, while Connecting or Ready, why the last read or subscribe was refused; it is retried. */
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
#endif

	/**
	 * Calls the Server Function the definition names Function. Params is the function's Params struct, or empty when it
	 * has none. Busy and unreachable servers are retried a few times. OnDone runs exactly once, on the game thread,
	 * except when the game instance shuts down first: pending calls are then dropped without running it.
	 */
	void Call(FName Function, const FInstancedStruct& Params, TFunction<void(const FCrowdyServerCallResult&)> OnDone);

	/** Calls a function that sends a List, with Params from MakeParams and its values set by name. A List reply reads by name through CrowdyExec::ToList. */
	void Call(FName Function, const FInstancedPropertyBag& Params, TFunction<void(const FCrowdyServerCallResult&)> OnDone);

	/** The List Function sends, with its default values, to set values on by name and pass to Call. Empty when it sends a struct or nothing. */
	FInstancedPropertyBag MakeParams(FName Function) const;

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Server Objects", DisplayName = "Get Status")
	ECrowdyServerObjectStatus K2_GetStatus() const { return Status; }

	/** Why it Failed, or, while Connecting or Ready, why the last read was refused; it is retried. */
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

	/** Starts over from Connecting, for a Failed object acquired again. */
	void Restart();

	/** Fails every pending call as Canceled and moves to Failed. Broadcasts only when bNotify. */
	void Fail(const FString& Reason, bool bNotify);

	/** Terminal. Stops listening; pending calls complete as Canceled when bNotify, and are dropped silently otherwise. */
	void MarkReleased(bool bNotify);

	/** Advances retry timers by DeltaSeconds. */
	void Tick(float DeltaSeconds);

	void CallWith(FName Function, const UScriptStruct* ParamsStruct, const void* Params, TFunction<void(const FCrowdyServerCallResult&)> OnDone);

	UPROPERTY()
	TObjectPtr<UCrowdyServerObjectDefinition> Definition;

	TWeakObjectPtr<UCrowdyServerObjectSubsystem> Subsystem;
	FString InstanceId;
	ECrowdyServerObjectStatus Status = ECrowdyServerObjectStatus::Connecting;
	FString FailureReason;
	/** Whether FailureReason is a refused read's, which the next good read clears. */
	bool bReadSetReason = false;

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
		int32 Attempts = 0;
		bool bInFlight = false;
		/** Canceled by a connection closed elsewhere; completed on the next Tick, never inline. */
		bool bConnectionClosed = false;
	};

	/** Subscribes to the watched values if not yet, then reads them. */
	void Refresh();
	void RequestRead();
	void HandleSubscribeDone(uint32 Attempt, const FCrowdyNativeExecReply& Reply);
	void HandleReadDone(const FCrowdyNativeExecReply& Reply);
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
};
