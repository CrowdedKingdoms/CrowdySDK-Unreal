#include "CrowdyServerObject.h"

#include "CrowdyExecCodec.h"
#include "CrowdyExecInternal.h"
#include "CrowdyExecLog.h"
#include "CrowdyNativeExec.h"
#include "CrowdyNativeKit.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectLibrary.h"
#include "CrowdyServerObjectSubsystem.h"
#include "Engine/GameInstance.h"
#include "HAL/PlatformTime.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UObject/PropertyOptional.h"
#include "UObject/UnrealType.h"

namespace CrowdyServerObjectDetail
{
	constexpr int32 MaxBufferedPushes = 32;
	constexpr int32 MaxCallAttempts = 3;
	constexpr int32 MaxReasonChars = 256;
	constexpr float MaxRetryHintSeconds = 30.f;
	constexpr float MaxUnsentSeconds = 30.f;
	/** A paged read of records takes several round trips, so it buffers more pushes, bounded by the memory MaxBufferedPushes full ones take. */
	constexpr int32 MaxBufferedRecordPushes = 1024;
	constexpr int64 MaxBufferedRecordBytes = static_cast<int64>(MaxBufferedPushes) * CrowdyExec::MaxPayloadBytes;
	constexpr float ChangesIntervalSeconds = 1.f;
	constexpr float MaxMessageRetrySeconds = 30.f;
	constexpr int32 MaxPendingMessages = 32;

	const TCHAR* const ReleasedReason = TEXT("This Server Object was given back");

	FCrowdyNativeExecConnection* GetConnection(const TWeakObjectPtr<UCrowdyServerObjectSubsystem>& Subsystem)
	{
		const UCrowdyServerObjectSubsystem* Owner = Subsystem.Get();
		return Owner ? Owner->GetConnection() : nullptr;
	}

	/** Where a type's subscribes, reads and calls go: the Game Kit for a Game Kit starter, the connection for every other type. */
	struct FRoute
	{
		FCrowdyNativeExecConnection* Connection = nullptr;
		FCrowdyNativeKit* Kit = nullptr;

		explicit operator bool() const { return Connection || Kit; }

		uint64 Subscribe(const FString& NodeType, const FString& Key, const FString& Topic, TFunction<void(const FCrowdyNativeExecPush&)> OnPush,
			TFunction<void(const FCrowdyNativeExecReply&)> OnDone) const
		{
			if (Kit)
			{
				return Kit->Subscribe(NodeType, Key, Topic, MoveTemp(OnPush), [OnDone = MoveTemp(OnDone)](const FCrowdyNativeKitReply& Reply) { OnDone(Reply); });
			}
			return Connection ? Connection->Subscribe(NodeType, Key, Topic, MoveTemp(OnPush), MoveTemp(OnDone)) : 0;
		}

		void Unsubscribe(uint64 Handle) const
		{
			if (Kit)
			{
				Kit->Unsubscribe(Handle);
			}
			else if (Connection)
			{
				Connection->Unsubscribe(Handle);
			}
		}

		/** OnDone gets a Game Kit starter's refusal code too, empty for every other type. */
		void Call(const FString& NodeType, const FString& Key, const FString& Method, TArray<uint8> Payload, bool bOnce,
			TFunction<void(const FCrowdyNativeExecReply&, const FString&)> OnDone) const
		{
			if (Kit)
			{
				Kit->Call(NodeType, Key, Method, Payload, bOnce, 0, [OnDone = MoveTemp(OnDone)](const FCrowdyNativeKitReply& Reply) { OnDone(Reply, Reply.Code); });
				return;
			}
			if (Connection)
			{
				Connection->Call(NodeType, Key, Method, MoveTemp(Payload), [OnDone = MoveTemp(OnDone)](const FCrowdyNativeExecReply& Reply) { OnDone(Reply, FString()); });
			}
		}
	};

	FRoute GetRoute(const TWeakObjectPtr<UCrowdyServerObjectSubsystem>& Subsystem, const UCrowdyServerObjectDefinition& Definition)
	{
		UCrowdyServerObjectSubsystem* Owner = Subsystem.Get();
		if (!Owner)
		{
			return FRoute();
		}
		return Definition.IsGameKitStarter() ? FRoute{nullptr, Owner->GetKit()} : FRoute{Owner->GetConnection(), nullptr};
	}

	/** The topic a type's watched values arrive on. */
	FString StateTopic(const UCrowdyServerObjectDefinition& Definition)
	{
		return Definition.IsGameKitStarter() ? Definition.KitStateTopic : FString(TEXT("state"));
	}

	FCrowdyServerCallResult MakeResult(ECrowdyServerCallOutcome Outcome, const FString& Reason, bool bRetryable = false)
	{
		FCrowdyServerCallResult Result;
		Result.Outcome = Outcome;
		Result.Reason = Reason;
		Result.bRetryable = bRetryable;
		return Result;
	}

	FString PlainReason(const FCrowdyNativeExecReply& Reply)
	{
		if (!Reply.Message.IsEmpty())
		{
			return CrowdyExec::SafeText(Reply.Message, MaxReasonChars);
		}
		return FString::Printf(TEXT("The server refused the request (status %d)"), static_cast<int32>(Reply.Status));
	}

	/** The platform says why it refused, such as a type switched off or an app paused by billing, so its message follows the sentence. */
	FString WithServerMessage(const FString& Sentence, const FCrowdyNativeExecReply& Reply)
	{
		return Reply.Message.IsEmpty() ? Sentence : Sentence + TEXT(": ") + CrowdyExec::SafeText(Reply.Message, MaxReasonChars);
	}

	float RetryHintSeconds(const FCrowdyNativeExecReply& Reply)
	{
		return Reply.RetryAfterMs >= 0 ? FMath::Min(Reply.RetryAfterMs / 1000.f, MaxRetryHintSeconds) : 0.f;
	}

	/** Seconds stretched by up to half again, so many clients do not retry in lockstep; Jitter is in [0, 1) and never shortens the wait. */
	float Jittered(float Seconds, float Jitter)
	{
		return Seconds * (1.f + 0.5f * Jitter);
	}

	float NextJitter(const TWeakObjectPtr<UCrowdyServerObjectSubsystem>& Subsystem)
	{
		const UCrowdyServerObjectSubsystem* Owner = Subsystem.Get();
		return Owner ? Owner->NextJitter() : 0.f;
	}

	/** The wait before retrying after Failures refusals in a row: doubling from 1 s up to 10 s, or longer when the reply asks. */
	float RetryInSeconds(int32 Failures, const FCrowdyNativeExecReply& Reply, float Jitter)
	{
		const float Backoff = FMath::Min(static_cast<float>(1 << FMath::Min(Failures - 1, 4)), 10.f);
		return Jittered(FMath::Max(Backoff, RetryHintSeconds(Reply)), Jitter);
	}

	bool IsSwitchedOffText(const FString& Message)
	{
		// A refused connect ends with the Game API's code; older hosts said it only in words.
		return Message.EndsWith(TEXT("(APP_PAUSED)"), ESearchCase::CaseSensitive) || Message.EndsWith(TEXT("(APP_DISABLED)"), ESearchCase::CaseSensitive)
			|| Message.EndsWith(TEXT("(TYPE_DISABLED)"), ESearchCase::CaseSensitive)
			|| Message.Contains(TEXT(" is switched off in app "), ESearchCase::CaseSensitive)
			|| (Message.StartsWith(TEXT("app "), ESearchCase::CaseSensitive)
				&& (Message.Contains(TEXT(" is switched off"), ESearchCase::CaseSensitive) || Message.Contains(TEXT(" is paused"), ESearchCase::CaseSensitive)));
	}

	/** Why the socket cannot redial when the app or type is switched off, or empty: a call waiting meanwhile got no answer, but the pause is the cause. */
	FString RedialRefusal(const TWeakObjectPtr<UCrowdyServerObjectSubsystem>& Subsystem)
	{
		const FCrowdyNativeExecConnection* Connection = GetConnection(Subsystem);
		if (!Connection || Connection->IsConnected())
		{
			return FString();
		}
		const FString Refusal = Connection->GetRefusal();
		return IsSwitchedOffText(Refusal) ? Refusal : FString();
	}

	FString NotDeployedReason(const UCrowdyServerObjectDefinition& Definition)
	{
		return FString::Printf(TEXT("%s is not deployed on the server"), *Definition.TypeName);
	}

	/** Every deploy from this SDK sends the root, so an app answering NotFound for it was deployed without one: it is asked again only this often. */
	constexpr float MissingRootRetrySeconds = 60.f;

	bool IsKitWorld(const UCrowdyServerObjectDefinition& Definition)
	{
		return Definition.IsGameKitStarter() && &Definition == UCrowdyServerObjectLibrary::GetGameKitWorldDefinition();
	}

	/** The platform refuses a watch of a type the active version lacks this way, not with NotFound; this SDK deploys every type players may reach. */
	bool IsAbsentTypeDenial(const FCrowdyNativeExecReply& Reply)
	{
		return Reply.Status == ECrowdyNativeExecStatus::Denied && Reply.Message.Equals(TEXT("clients may not subscribe to that type"), ESearchCase::CaseSensitive);
	}

	bool IsMissingRoot(const UCrowdyServerObjectDefinition& Definition, const FCrowdyNativeExecReply& Reply)
	{
		return (Reply.Status == ECrowdyNativeExecStatus::NotFound || IsAbsentTypeDenial(Reply))
			&& (Definition.TypeName.Equals(UCrowdyServerObjectDefinition::RootTypeName, ESearchCase::CaseSensitive) || IsKitWorld(Definition));
	}

	FString NotDeployedReason(const UCrowdyServerObjectDefinition& Definition, const FCrowdyNativeExecReply& Reply)
	{
		if (!IsMissingRoot(Definition, Reply))
		{
			return NotDeployedReason(Definition);
		}
		return IsKitWorld(Definition)
			? FString(TEXT("This app has no Game Kit world, so Players Online has no count: deploy the app from Server Compute, or clear the Game Kit World setting if the project no longer has the world starter. It is asked again every minute."))
			: FString(TEXT("This app has no Crowdy SDK root, so Players Online has no count: deploy the app from Server Compute (an app last deployed with an older Crowdy SDK, or by another tool, has none). It is asked again every minute."));
	}

	bool IsConnectionClosed(const FCrowdyNativeExecReply& Reply)
	{
		return Reply.Status == ECrowdyNativeExecStatus::Unavailable
			&& (Reply.Message.Equals(FCrowdyNativeExecConnection::CanceledMessage(), ESearchCase::CaseSensitive)
				|| Reply.Message.Equals(FCrowdyNativeKit::CanceledMessage(), ESearchCase::CaseSensitive));
	}

	/** A timeout the client gave up on: nothing came back, so the server's Time per Call is not the cause. */
	const TCHAR* const NoAnswerReason = TEXT("The server sent no answer in time; the connection may be slow or lost");

	/** Why a retried read or subscribe refusal was given, or empty when saying so would not help. */
	FString WaitingReason(const UCrowdyServerObjectDefinition& Definition, const FCrowdyNativeExecReply& Reply, bool bRead)
	{
		const TCHAR* const Verb = bRead ? TEXT("read") : TEXT("watch");
		switch (Reply.Status)
		{
		case ECrowdyNativeExecStatus::NotFound:
			return NotDeployedReason(Definition, Reply);
		case ECrowdyNativeExecStatus::Denied:
			if (IsAbsentTypeDenial(Reply))
			{
				return NotDeployedReason(Definition, Reply);
			}
			if (IsSwitchedOffText(Reply.Message))
			{
				return WithServerMessage(TEXT("The server is switched off or paused, so this Server Object waits"), Reply);
			}
			return WithServerMessage(FString::Printf(TEXT("The server refused to let this player %s this Server Object"), Verb), Reply);
		case ECrowdyNativeExecStatus::AppPaused:
		case ECrowdyNativeExecStatus::AppDisabled:
		case ECrowdyNativeExecStatus::TypeDisabled:
			return WithServerMessage(TEXT("The server is switched off or paused, so this Server Object waits"), Reply);
		case ECrowdyNativeExecStatus::Unavailable:
			// Such as an app at its instance limit, which would otherwise leave the object connecting with no reason. Reads
			// only: a good read clears what a read set, while a subscribe's reason would outlast the recovery.
			return !bRead || Reply.Message.IsEmpty() || IsConnectionClosed(Reply) ? FString() : WithServerMessage(TEXT("The server could not be reached"), Reply);
		case ECrowdyNativeExecStatus::Trapped:
			return TEXT("The server code crashed; check its log");
		case ECrowdyNativeExecStatus::DeadlineExceeded:
			return Reply.bNoReply ? FString(NoAnswerReason) : FString::Printf(TEXT("The server took longer than this type's Time per Call to %s this Server Object"), Verb);
		case ECrowdyNativeExecStatus::Internal:
			return TEXT("The server failed with an internal error");
		case ECrowdyNativeExecStatus::AppError:
			return PlainReason(Reply);
		default:
			return FString();
		}
	}

	/** Only these say the server did not run the call; anything else may already have run it. */
	bool IsCallRetried(ECrowdyNativeExecStatus Status)
	{
		return Status == ECrowdyNativeExecStatus::Busy || Status == ECrowdyNativeExecStatus::RateLimited;
	}

	/** Server code refuses with "denied: <why>" or "cooldown: <why>": both are Denied, and a cooldown can be retried later. */
	void ApplyAppRefusal(FCrowdyServerCallResult& Result, const FString& Function)
	{
		const TCHAR* const DeniedToken = TEXT("denied:");
		const TCHAR* const CooldownToken = TEXT("cooldown:");
		const bool bCooldown = Result.Reason.StartsWith(CooldownToken, ESearchCase::CaseSensitive);
		if (!bCooldown && !Result.Reason.StartsWith(DeniedToken, ESearchCase::CaseSensitive))
		{
			return;
		}
		const FString Rest = Result.Reason.RightChop(FCString::Strlen(bCooldown ? CooldownToken : DeniedToken)).TrimStartAndEnd();
		Result.Outcome = ECrowdyServerCallOutcome::Denied;
		Result.bRetryable = bCooldown;
		Result.Reason = Rest.IsEmpty() ? FString::Printf(TEXT("This player may not call %s on this Server Object"), *Function) : Rest;
	}

	/** A switched-off or paused app or type did not run the call; it can be tried again once it is back on. */
	void ApplyPaused(FCrowdyServerCallResult& Result, const FCrowdyNativeExecReply& Reply, const FString& Function)
	{
		Result.Outcome = ECrowdyServerCallOutcome::Paused;
		Result.bRetryable = true;
		Result.Reason = WithServerMessage(FString::Printf(TEXT("The server is switched off or paused, so it refused the call to %s"), *Function), Reply);
	}

	/** Whether a Game Kit refusal code says asking again may help. */
	bool IsRetryableKitCode(const FString& Code)
	{
		return Code.Equals(TEXT("cooldown"), ESearchCase::CaseSensitive) || Code.Equals(TEXT("unavailable"), ESearchCase::CaseSensitive);
	}

	/** A Game Kit starter refuses with a code: Refused, but for the codes that mean an outcome a call already has. */
	void ApplyKitRefusal(FCrowdyServerCallResult& Result, const FCrowdyNativeExecReply& Reply, const FString& Code, const FString& Function)
	{
		Result.Code = FName(*Code);
		const bool bCooldown = Code.Equals(TEXT("cooldown"), ESearchCase::CaseSensitive);
		const bool bUnavailable = Code.Equals(TEXT("unavailable"), ESearchCase::CaseSensitive);
		Result.Outcome = bCooldown || Code.Equals(TEXT("forbidden"), ESearchCase::CaseSensitive) ? ECrowdyServerCallOutcome::Denied
			: bUnavailable ? ECrowdyServerCallOutcome::Unavailable
			: ECrowdyServerCallOutcome::Refused;
		Result.bRetryable = IsRetryableKitCode(Code);
		Result.Reason = Reply.Message.IsEmpty() ? FString::Printf(TEXT("The server refused the call to %s: %s"), *Function, *Code) : PlainReason(Reply);
	}

	/** Whether a call was the built-in member function with BuiltInMethod, not a function of the definition's own. */
	bool IsBuiltInMemberCall(const UCrowdyServerObjectDefinition& Definition, FName Function, const FString& Method, const TCHAR* BuiltInMethod)
	{
		return !Definition.IsFunctionLibrary() && Definition.MembersFrom == ECrowdyServerMembersSource::ThisObject && UCrowdyServerObjectDefinition::IsMemberFunction(Function)
			&& Method.Equals(BuiltInMethod, ESearchCase::CaseSensitive);
	}

	/** A late watcher's first call names the member values too, when the object keeps its own members. */
	void AppendMemberNames(const UCrowdyServerObjectDefinition& Definition, TArray<FString>& OutNames)
	{
		if (Definition.MembersFrom != ECrowdyServerMembersSource::ThisObject)
		{
			return;
		}
		if (Definition.bShowMembers)
		{
			OutNames.Add(TEXT("Members"));
		}
		OutNames.Append({TEXT("MemberCount"), TEXT("Leader"), TEXT("OpenForJoining")});
	}

	/** Names for a trace line: comma-separated, or "-" when there are none. */
	FString TraceNames(const TArray<FString>& Names)
	{
		return Names.IsEmpty() ? FString(TEXT("-")) : FString::Join(Names, TEXT(","));
	}

	template <typename TEnum>
	FString TraceEnum(TEnum Value)
	{
		return StaticEnum<TEnum>()->GetNameStringByValue(static_cast<int64>(Value));
	}

	/** The instance a trace line names: an Owner Only instance id is the player's user id, so it prints as "owner", and the root has none, so it prints as "-". */
	const TCHAR* TraceId(const UCrowdyServerObjectDefinition& Definition, const FString& InstanceId)
	{
		if (!Definition.IsFunctionLibrary() && Definition.Visibility == ECrowdyServerObjectVisibility::OwnerOnly)
		{
			return TEXT("owner");
		}
		return InstanceId.IsEmpty() ? TEXT("-") : *InstanceId;
	}

	/** Whether the field's value or its presence flag differs between two values of its struct. */
	bool FieldDiffers(const FCrowdyExecLayout& Layout, const FCrowdyExecField& Field, const void* Before, const void* After)
	{
		if (!Layout.Types[Field.Type].Property->Identical_InContainer(Before, After))
		{
			return true;
		}
		return Field.Presence && Field.Presence->GetPropertyValue_InContainer(Before) != Field.Presence->GetPropertyValue_InContainer(After);
	}

	/** The State field of Variable, or null when the layout has none. */
	const FCrowdyExecField* FindStateField(const FCrowdyExecLayout& Layout, const FProperty& Variable)
	{
		if (Layout.StateStruct == INDEX_NONE)
		{
			return nullptr;
		}
		return Layout.Structs[Layout.StateStruct].Fields.FindByPredicate([&Layout, &Variable](const FCrowdyExecField& Each) { return Layout.Types[Each.Type].Property == &Variable; });
	}

	/** A copy of one State field's value and presence, to tell after the State is rewritten whether that field changed. */
	struct FHeldField
	{
		const FProperty& Variable;
		const FBoolProperty* Presence;
		void* Value;
		bool bPresent;

		FHeldField(const FProperty& InVariable, const FCrowdyExecField& Field, const void* State)
			: Variable(InVariable)
			, Presence(Field.Presence)
			, Value(FMemory::Malloc(InVariable.GetSize(), InVariable.GetMinAlignment()))
			, bPresent(Field.Presence && Field.Presence->GetPropertyValue_InContainer(State))
		{
			Variable.InitializeValue(Value);
			Variable.CopyCompleteValue(Value, Variable.ContainerPtrToValuePtr<void>(State));
		}

		FHeldField(const FHeldField&) = delete;
		FHeldField& operator=(const FHeldField&) = delete;

		~FHeldField()
		{
			Variable.DestroyValue(Value);
			FMemory::Free(Value);
		}

		bool Differs(const void* State) const
		{
			return !Variable.Identical(Value, Variable.ContainerPtrToValuePtr<void>(State)) || (Presence && Presence->GetPropertyValue_InContainer(State) != bPresent);
		}
	};

	int64 FindSignedInUserId(const TWeakObjectPtr<UCrowdyServerObjectSubsystem>& Subsystem)
	{
		const UCrowdyServerObjectSubsystem* Owner = Subsystem.Get();
		return Owner ? Owner->GetSignedInUserId() : 0;
	}

	void WriteKey(FCrowdyExecWriter& Writer, const ANSICHAR* Key)
	{
		Writer.String(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Key), FCStringAnsi::Strlen(Key)));
	}

	/** A read's or changes' input: the page after the id After, and for changes the restricted seq Since. */
	TArray<uint8> PageRequest(const FString& After, const uint64* Since = nullptr)
	{
		TArray<uint8> Bytes;
		FCrowdyExecWriter Writer(Bytes);
		const FTCHARToUTF8 AfterText(*After, After.Len());
		Writer.MapHeader(Since ? 2 : 1);
		if (Since)
		{
			WriteKey(Writer, "since");
			Writer.UInt(*Since);
		}
		WriteKey(Writer, "after");
		Writer.String(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(AfterText.Get()), AfterText.Length()));
		return Bytes;
	}

	/** A Game Kit starter's resync: the snapshot of Topic, answered for this caller. */
	TArray<uint8> KitSnapshotRequest(const FString& Topic)
	{
		TArray<uint8> Bytes;
		FCrowdyExecWriter Writer(Bytes);
		const FTCHARToUTF8 TopicText(*Topic, Topic.Len());
		Writer.MapHeader(1);
		WriteKey(Writer, "topic");
		Writer.String(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(TopicText.Get()), TopicText.Length()));
		return Bytes;
	}

	/** The object's member values, so a read and a push apply them through one path. */
	struct FMemberFields
	{
		TArray<int64>& Members;
		int32& MemberCount;
		int64& Leader;
		bool& bOpenForJoining;
		bool& bIsMember;
		bool& bIsLeader;
	};

	/** Stores a key a read or push carried, or a read's default for one it lacks; names it when carried (bNameCarried) or changed. */
	template <typename T>
	void TakeMemberKey(const T& Value, bool bCarried, bool bRead, bool bNameCarried, T& Field, const TCHAR* Name, TArray<FString>& OutChanged)
	{
		if (!bCarried && !bRead)
		{
			return;
		}
		if (bNameCarried ? bCarried : !(Field == Value))
		{
			OutChanged.Add(Name);
		}
		Field = Value;
	}

	/** Keeps only the members keys the definition has the server send, so extra keys from a mismatched server change nothing. */
	FCrowdyExecMembersView KeepDefinedMemberKeys(const UCrowdyServerObjectDefinition& Definition, FCrowdyExecMembersView&& View)
	{
		const bool bOwnMembers = Definition.MembersFrom == ECrowdyServerMembersSource::ThisObject;
		FCrowdyExecMembersView Kept;
		if (bOwnMembers && Definition.bShowMembers)
		{
			Kept.Members = MoveTemp(View.Members);
			Kept.bHasMembers = View.bHasMembers;
		}
		if (bOwnMembers)
		{
			Kept.MemberCount = View.MemberCount;
			Kept.Leader = View.Leader;
			Kept.bOpen = View.bOpen;
			Kept.bHasMemberCount = View.bHasMemberCount;
			Kept.bHasLeader = View.bHasLeader;
			Kept.bHasOpen = View.bHasOpen;
		}
		if (Definition.MembersFrom != ECrowdyServerMembersSource::None)
		{
			Kept.bIsMember = View.bIsMember;
			Kept.bIsLeader = View.bIsLeader;
			Kept.bHasIsMember = View.bHasIsMember;
			Kept.bHasIsLeader = View.bHasIsLeader;
		}
		return Kept;
	}

	/** Whether a pushed members list is what Is Member follows, so a Join or Leave must not set it. */
	bool IsMembershipPushed(const UCrowdyServerObjectDefinition& Definition)
	{
		return Definition.MembersFrom == ECrowdyServerMembersSource::ThisObject && Definition.bShowMembers
			&& Definition.Visibility == ECrowdyServerObjectVisibility::Public;
	}

	/** A read sets Is Member and Is Leader as the server says; a push derives them from a carried list or leader and the signed-in user. */
	void ApplyMemberView(const FMemberFields& Out, const FCrowdyExecMembersView& View, bool bRead, bool bNameCarried, int64 UserId, TArray<FString>& OutChanged)
	{
		TakeMemberKey(View.Members, View.bHasMembers, bRead, bNameCarried, Out.Members, TEXT("Members"), OutChanged);
		TakeMemberKey(View.MemberCount, View.bHasMemberCount, bRead, bNameCarried, Out.MemberCount, TEXT("MemberCount"), OutChanged);
		TakeMemberKey(View.Leader, View.bHasLeader, bRead, bNameCarried, Out.Leader, TEXT("Leader"), OutChanged);
		TakeMemberKey(View.bOpen, View.bHasOpen, bRead, bNameCarried, Out.bOpenForJoining, TEXT("OpenForJoining"), OutChanged);
		if (bRead)
		{
			Out.bIsMember = View.bIsMember;
			Out.bIsLeader = View.bIsLeader;
			return;
		}
		if (UserId == 0)
		{
			return;
		}
		if (View.bHasMembers)
		{
			Out.bIsMember = Out.Members.Contains(UserId);
		}
		if (View.bHasLeader)
		{
			Out.bIsLeader = Out.Leader == UserId;
		}
	}
}

void UCrowdyServerObject::Setup(UCrowdyServerObjectSubsystem* InSubsystem, UCrowdyServerObjectDefinition* InDefinition, const FString& InInstanceId)
{
	Subsystem = InSubsystem;
	Definition = InDefinition;
	InstanceId = InInstanceId;
	Definition->InitializeValues(Definition->GetStateStruct(), State);
	Status = ECrowdyServerObjectStatus::Connecting;
	MessageBindings.Follow(this, FOnCrowdyServerMessage::FDelegate::CreateUObject(this, &UCrowdyServerObject::HandleBoundMessage));
}

FInstancedPropertyBag UCrowdyServerObject::GetStateList() const
{
	return CrowdyExec::ToList(State);
}

int64 UCrowdyServerObject::GetSignedInUserId() const
{
	const UCrowdyServerObjectSubsystem* Owner = Subsystem.Get();
	return Owner ? Owner->GetSignedInUserId() : 0;
}

bool UCrowdyServerObject::HoldsRecords() const
{
	return Definition && !Definition->IsFunctionLibrary() && (Definition->Holds == ECrowdyServerObjectHolds::RecordPerEntity || Definition->HasKitCollection());
}

const FInstancedStruct* UCrowdyServerObject::FindValues(const FString* RecordId) const
{
	if (!RecordId)
	{
		return &State;
	}
	const FCrowdyServerRecord* Record = Records.Find(*RecordId);
	return Record ? &Record->Values : nullptr;
}

FDelegateHandle UCrowdyServerObject::WatchRecord(const FString& Id, FOnCrowdyServerValuesChanged::FDelegate Listener)
{
	TSharedRef<FOnCrowdyServerValuesChanged>* Watchers = RecordWatchers.Find(Id);
	if (!Watchers)
	{
		Watchers = &RecordWatchers.Add(Id, MakeShared<FOnCrowdyServerValuesChanged>());
	}
	const uint32 JoinedDuring = ActiveNotify;
	const FDelegateHandle Handle = (*Watchers)->Add(JoinedDuring == 0 ? Listener
		: FOnCrowdyServerValuesChanged::FDelegate::CreateWeakLambda(this, [this, Listener, JoinedDuring](UCrowdyServerObject* Object, TConstArrayView<FString> Fields)
		{
			if (ActiveNotify != JoinedDuring)
			{
				Listener.ExecuteIfBound(Object, Fields);
			}
		}));
	if (Status != ECrowdyServerObjectStatus::Ready || !Records.Contains(Id))
	{
		return Handle;
	}
	TArray<FString> Fields;
	CollectWatchedFields(nullptr, nullptr, Fields, true);
	if (Fields.Num() > 0)
	{
		TGuardValue<uint64> Announcing(AnnouncedSerial, FCrowdyServerVariableBindings::LatestSerial());
		Listener.ExecuteIfBound(this, Fields);
	}
	return Handle;
}

bool UCrowdyServerObject::IsRecordWatched(const FString& Id) const
{
	const TSharedRef<FOnCrowdyServerValuesChanged>* Watchers = RecordWatchers.Find(Id);
	return Watchers && (*Watchers)->IsBound();
}

void UCrowdyServerObject::UnwatchRecord(const FString& Id, FDelegateHandle Handle)
{
	TSharedRef<FOnCrowdyServerValuesChanged>* Watchers = RecordWatchers.Find(Id);
	if (!Watchers)
	{
		return;
	}
	(*Watchers)->Remove(Handle);
	if (!(*Watchers)->IsBound())
	{
		RecordWatchers.Remove(Id);
	}
}

void UCrowdyServerObject::PruneRecordEventWatchers()
{
	for (int32 Index = RecordEventWatchers.Num() - 1; Index >= 0; --Index)
	{
		if (RecordEventWatchers[Index].Event.IsBound())
		{
			continue;
		}
		const FRecordEventWatcher Dead = RecordEventWatchers[Index];
		RecordEventWatchers.RemoveAt(Index);
		UnwatchRecord(Dead.RecordId, Dead.Handle);
	}
}

void UCrowdyServerObject::K2_WatchRecord(const FString& RecordId, FCrowdyServerRecordEvent Event)
{
	if (!Event.IsBound())
	{
		return;
	}
	PruneRecordEventWatchers();
	const bool bWatching = RecordEventWatchers.ContainsByPredicate([&RecordId, &Event](const FRecordEventWatcher& Each) { return Each.Watches(RecordId, Event); });
	if (bWatching)
	{
		return;
	}
	RecordEventWatchers.Add({RecordId, Event, FDelegateHandle()});
	const FDelegateHandle Handle = WatchRecord(RecordId, FOnCrowdyServerValuesChanged::FDelegate::CreateWeakLambda(this,
		[this, RecordId, Event](UCrowdyServerObject*, TConstArrayView<FString> Fields)
		{
			if (!Event.IsBound())
			{
				PruneRecordEventWatchers();
				return;
			}
			Event.ExecuteIfBound(this, RecordId, ToVariableNames(Fields, true));
		}));
	// The catch-up run may have stopped this watcher already.
	FRecordEventWatcher* Added = RecordEventWatchers.FindByPredicate([&RecordId, &Event](const FRecordEventWatcher& Each) { return Each.Watches(RecordId, Event); });
	if (!Added)
	{
		UnwatchRecord(RecordId, Handle);
		return;
	}
	Added->Handle = Handle;
}

void UCrowdyServerObject::K2_UnwatchRecord(const FString& RecordId, FCrowdyServerRecordEvent Event)
{
	const int32 Index = RecordEventWatchers.IndexOfByPredicate([&RecordId, &Event](const FRecordEventWatcher& Each) { return Each.Watches(RecordId, Event); });
	if (Index == INDEX_NONE)
	{
		return;
	}
	const FRecordEventWatcher Stopped = RecordEventWatchers[Index];
	RecordEventWatchers.RemoveAt(Index);
	UnwatchRecord(Stopped.RecordId, Stopped.Handle);
}

bool UCrowdyServerObject::K2_FindRecord(const FString& Id, FInstancedStruct& Values, int64& Owner, FString& Team) const
{
	const FCrowdyServerRecord* Record = Status == ECrowdyServerObjectStatus::Ready ? Records.Find(Id) : nullptr;
	Values = Record ? Record->Values : FInstancedStruct();
	Owner = Record ? Record->Owner : 0;
	Team = Record ? Record->Team : FString();
	return Record != nullptr;
}

TArray<FString> UCrowdyServerObject::GetRecordIds() const
{
	TArray<FString> Ids;
	Records.GetKeys(Ids);
	return Ids;
}

FDelegateHandle UCrowdyServerObject::WatchValues(FOnCrowdyServerValuesChanged::FDelegate Listener)
{
	const FDelegateHandle Handle = ValuesChanged.Add(Listener);
	if (Status != ECrowdyServerObjectStatus::Ready)
	{
		return Handle;
	}
	TArray<FString> Fields;
	CollectWatchedFields(nullptr, nullptr, Fields);
	CrowdyServerObjectDetail::AppendMemberNames(*Definition, Fields);
	if (Fields.Num() > 0)
	{
		TGuardValue<uint64> Announcing(AnnouncedSerial, FCrowdyServerVariableBindings::LatestSerial());
		Listener.ExecuteIfBound(this, Fields);
	}
	return Handle;
}

void UCrowdyServerObject::UnwatchValues(FDelegateHandle Handle)
{
	ValuesChanged.Remove(Handle);
}

void UCrowdyServerObject::K2_WatchVariables(FCrowdyServerVariablesEvent Event)
{
	if (!Event.IsBound())
	{
		return;
	}
	VariableWatchers.RemoveAll([](const FCrowdyServerVariablesEvent& Watcher) { return !Watcher.IsBound(); });
	VariableWatchers.AddUnique(Event);
	if (Status != ECrowdyServerObjectStatus::Ready)
	{
		return;
	}
	TArray<FString> Fields;
	CollectWatchedFields(nullptr, nullptr, Fields);
	CrowdyServerObjectDetail::AppendMemberNames(*Definition, Fields);
	if (Fields.Num() > 0)
	{
		Event.ExecuteIfBound(this, ToVariableNames(Fields));
	}
}

TArray<FString> UCrowdyServerObject::ToVariableNames(TConstArrayView<FString> Fields, bool bRecord) const
{
	TArray<FString> Names(Fields);
	FString Error;
	const FCrowdyExecLayout* Layout = Definition ? Definition->ResolveLayout(Error) : nullptr;
	const int32 StructIndex = !Layout ? INDEX_NONE : bRecord ? Layout->RecordStruct : Layout->StateStruct;
	if (StructIndex == INDEX_NONE)
	{
		return Names;
	}
	const TArray<FCrowdyExecField>& StateFields = Layout->Structs[StructIndex].Fields;
	for (FString& Name : Names)
	{
		const FCrowdyExecField* Field = StateFields.FindByPredicate([&Name](const FCrowdyExecField& Each) { return Each.Name.Equals(Name, ESearchCase::CaseSensitive); });
		if (Field)
		{
			Name = CrowdyExec::KeepNameCharacters(Layout->Types[Field->Type].Property->GetAuthoredName());
		}
	}
	return Names;
}

void UCrowdyServerObject::K2_UnwatchVariables(FCrowdyServerVariablesEvent Event)
{
	VariableWatchers.Remove(Event);
}

UCrowdyServerObject::FMessageTopic* UCrowdyServerObject::FindOrAddMessageTopic(FName Message)
{
	if (Status == ECrowdyServerObjectStatus::Released)
	{
		return nullptr;
	}
	if (const TSharedRef<FMessageTopic>* Found = MessageTopics.Find(Message))
	{
		return &Found->Get();
	}
	const FCrowdyServerMessage* Declared = Definition->FindMessage(Message);
	if (!Declared)
	{
		UE_LOG(LogCrowdyExec, Warning, TEXT("%s has no message named %s, so nothing can listen to it"), *Definition->TypeName, *Message.ToString());
		return nullptr;
	}
	if (!Declared->bPlayersCanListen)
	{
		bool bWarnedBefore = false;
		ServerOnlyWarned.Add(Message, &bWarnedBefore);
		UE_CLOG(!bWarnedBefore, LogCrowdyExec, Warning, TEXT("%s is heard only by other Server Object types; turn on Players Can Listen to hear it in a game"), *Message.ToString());
		return nullptr;
	}
	return &MessageTopics.Add(Message, MakeShared<FMessageTopic>()).Get();
}

FDelegateHandle UCrowdyServerObject::WatchMessage(FName Message, FOnCrowdyServerMessage::FDelegate Listener)
{
	FMessageTopic* Topic = Listener.IsBound() ? FindOrAddMessageTopic(Message) : nullptr;
	if (!Topic)
	{
		return FDelegateHandle();
	}
	const FDelegateHandle Handle = Listener.GetHandle();
	Topic->bRefused = false;
	Topic->Listeners.Add(MoveTemp(Listener));
	SyncMessageSubscriptions();
	return Handle;
}

void UCrowdyServerObject::UnwatchMessage(FName Message, FDelegateHandle Handle)
{
	const TSharedRef<FMessageTopic>* Found = MessageTopics.Find(Message);
	if (!Found || !Handle.IsValid() || (*Found)->Listeners.RemoveAll([Handle](const FOnCrowdyServerMessage::FDelegate& Each) { return Each.GetHandle() == Handle; }) == 0)
	{
		return;
	}
	SyncMessageSubscriptions();
}

void UCrowdyServerObject::K2_WatchMessage(FName Message, FCrowdyServerMessageEvent Event)
{
	FMessageTopic* Topic = Event.IsBound() ? FindOrAddMessageTopic(Message) : nullptr;
	if (!Topic)
	{
		return;
	}
	Topic->Events.RemoveAll([](const FCrowdyServerMessageEvent& Each) { return !Each.IsBound(); });
	Topic->Events.AddUnique(Event);
	Topic->bRefused = false;
	SyncMessageSubscriptions();
}

void UCrowdyServerObject::K2_UnwatchMessage(FName Message, FCrowdyServerMessageEvent Event)
{
	const TSharedRef<FMessageTopic>* Found = MessageTopics.Find(Message);
	if (!Found)
	{
		return;
	}
	(*Found)->Events.Remove(Event);
	SyncMessageSubscriptions();
}

void UCrowdyServerObject::TickMessages(float DeltaSeconds)
{
	bool bDue = false;
	for (TPair<FName, TSharedRef<FMessageTopic>>& Pair : MessageTopics)
	{
		FMessageTopic& Topic = Pair.Value.Get();
		if (!Topic.bRefused || Topic.RetryIn <= 0.f)
		{
			continue;
		}
		Topic.RetryIn -= DeltaSeconds;
		if (Topic.RetryIn <= 0.f)
		{
			Topic.bRefused = false;
			bDue = true;
		}
	}
	if (bDue)
	{
		SyncMessageSubscriptions();
	}
}

void UCrowdyServerObject::SyncMessageSubscriptions(bool bRetryRefused)
{
	if (Status != ECrowdyServerObjectStatus::Connecting && Status != ECrowdyServerObjectStatus::Ready)
	{
		return;
	}
	const CrowdyServerObjectDetail::FRoute Route = CrowdyServerObjectDetail::GetRoute(Subsystem, *Definition);
	TArray<FName, TInlineAllocator<8>> Wanted;
	for (auto It = MessageTopics.CreateIterator(); It; ++It)
	{
		const FMessageTopic& Topic = It.Value().Get();
		if (!Topic.IsWanted())
		{
			Route.Unsubscribe(Topic.Handle);
			It.RemoveCurrent();
			continue;
		}
		if (Route && Topic.Handle == 0 && (!Topic.bRefused || bRetryRefused))
		{
			Wanted.Add(It.Key());
		}
	}
	for (const FName Message : Wanted)
	{
		SubscribeMessage(Message);
	}
}

void UCrowdyServerObject::SubscribeMessage(FName Message)
{
	const CrowdyServerObjectDetail::FRoute Route = CrowdyServerObjectDetail::GetRoute(Subsystem, *Definition);
	const TSharedRef<FMessageTopic>* Found = MessageTopics.Find(Message);
	const FCrowdyServerMessage* Declared = Definition->FindMessage(Message);
	if (!Route || !Found || !Declared)
	{
		return;
	}
	// A Game Kit starter's events on its State Topic arrive with the values, so a second subscription would hear each twice.
	if (Definition->IsGameKitStarter() && Declared->GetTopic().Equals(Definition->KitStateTopic, ESearchCase::CaseSensitive))
	{
		return;
	}
	FMessageTopic& Topic = Found->Get();
	const uint32 Attempt = ++MessageAttempt;
	Topic.Attempt = Attempt;
	Topic.bRefused = false;
	Topic.RetryIn = 0.f;
	TWeakObjectPtr<UCrowdyServerObject> WeakThis(this);
	const uint32 SubscribeGeneration = Generation;
	const uint64 Handle = Route.Subscribe(Definition->TypeName, InstanceId, Declared->GetTopic(),
		[WeakThis, SubscribeGeneration](const FCrowdyNativeExecPush& Push)
		{
			UCrowdyServerObject* Self = WeakThis.Get();
			if (Self && Self->Generation == SubscribeGeneration)
			{
				Self->HandlePush(Push);
			}
		},
		[WeakThis, SubscribeGeneration, Message, Attempt](const FCrowdyNativeExecReply& Reply)
		{
			UCrowdyServerObject* Self = WeakThis.Get();
			if (Self && Self->Generation == SubscribeGeneration)
			{
				Self->HandleMessageSubscribeDone(Message, Attempt, Reply);
			}
		});
	if (Generation != SubscribeGeneration)
	{
		Route.Unsubscribe(Handle);
		return;
	}
	if (Topic.Attempt == Attempt)
	{
		Topic.Handle = Handle;
	}
}

void UCrowdyServerObject::HandleMessageSubscribeDone(FName Message, uint32 Attempt, const FCrowdyNativeExecReply& Reply)
{
	const TSharedRef<FMessageTopic>* Found = MessageTopics.Find(Message);
	if (!Found || (*Found)->Attempt != Attempt)
	{
		return;
	}
	FMessageTopic& Topic = Found->Get();
	if (Reply.IsOk())
	{
		Topic.bWarned = false;
		Topic.Failures = 0;
		return;
	}
	Topic.Handle = 0;
	Topic.bRefused = true;
	if (CrowdyServerObjectDetail::IsConnectionClosed(Reply))
	{
		return;
	}
	// Asked again after a growing wait; a closed connection waits for the reconnect instead.
	const float Backoff = FMath::Min(static_cast<float>(1 << FMath::Min(Topic.Failures++, 5)), CrowdyServerObjectDetail::MaxMessageRetrySeconds);
	Topic.RetryIn = CrowdyServerObjectDetail::Jittered(FMath::Max(Backoff, CrowdyServerObjectDetail::RetryHintSeconds(Reply)), CrowdyServerObjectDetail::NextJitter(Subsystem));
	if (Topic.bWarned)
	{
		return;
	}
	Topic.bWarned = true;
	UE_LOG(LogCrowdyExec, Warning, TEXT("%s could not listen to its %s message, so it is asked again after a wait: %s"), *Definition->TypeName, *Message.ToString(),
		*CrowdyServerObjectDetail::PlainReason(Reply));
}

void UCrowdyServerObject::DropMessageSubscriptions(bool bUnsubscribe)
{
	const CrowdyServerObjectDetail::FRoute Route = bUnsubscribe ? CrowdyServerObjectDetail::GetRoute(Subsystem, *Definition) : CrowdyServerObjectDetail::FRoute();
	for (TPair<FName, TSharedRef<FMessageTopic>>& Pair : MessageTopics)
	{
		FMessageTopic& Topic = Pair.Value.Get();
		Route.Unsubscribe(Topic.Handle);
		Topic.Handle = 0;
		Topic.Attempt = 0;
		Topic.bRefused = false;
		Topic.Failures = 0;
		Topic.RetryIn = 0.f;
	}
}

void UCrowdyServerObject::HandlePush(const FCrowdyNativeExecPush& Push)
{
	if (Definition->IsGameKitStarter())
	{
		HandleKitPush(Push);
		return;
	}
	if (Push.Topic.Equals(TEXT("state"), ESearchCase::CaseSensitive))
	{
		ReceivePush(Push.Payload);
		return;
	}
	ReceiveMessage(Push);
}

void UCrowdyServerObject::HandleKitPush(const FCrowdyNativeExecPush& Push)
{
	const bool bStateTopic = Push.Topic.Equals(Definition->KitStateTopic, ESearchCase::CaseSensitive);
	const bool bCollectionTopic = Definition->HasKitCollection() && Push.Topic.Equals(Definition->KitCollectionTopic, ESearchCase::CaseSensitive);
	FCrowdyExecKitHeader Header;
	FString Error;
	if (!CrowdyExec::ReadKitHeader(Push.Payload, Header, Error))
	{
		UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: %s/%s push dropped topic=%s: %s"), *Definition->TypeName,
			CrowdyServerObjectDetail::TraceId(*Definition, InstanceId), *Push.Topic, *CrowdyExec::SafeText(Error, CrowdyServerObjectDetail::MaxReasonChars));
		if (bStateTopic)
		{
			RequestRead();
		}
		if (bCollectionTopic)
		{
			RequestCollectionRead();
		}
		return;
	}
	if (Header.Kind == ECrowdyExecKitKind::Event)
	{
		ReceiveMessage(Push);
		return;
	}
	if (Header.Kind == ECrowdyExecKitKind::Changed)
	{
		if (bStateTopic)
		{
			ReceiveKitChanged(Push.Payload);
		}
		return;
	}
	if (bCollectionTopic)
	{
		ReceiveCollectionPush(Push.Payload);
		return;
	}
	if (!bStateTopic)
	{
		return;
	}
	// The State Topic carries whole snapshots, so a change sent as a delta is read whole instead.
	if (Header.Kind == ECrowdyExecKitKind::Delta)
	{
		TraceReread(TEXT("delta"));
		RequestRead();
		return;
	}
	ReceivePush(Push.Payload);
}

void UCrowdyServerObject::ReceiveMessage(const FCrowdyNativeExecPush& Push)
{
	const bool bListening = Status == ECrowdyServerObjectStatus::Connecting || Status == ECrowdyServerObjectStatus::Ready;
	const FCrowdyServerMessage* Declared = bListening ? Definition->FindMessageByTopic(Push.Topic) : nullptr;
	const TSharedRef<FMessageTopic>* Found = Declared ? MessageTopics.Find(Declared->Name) : nullptr;
	if (!Found || !(*Found)->IsWanted())
	{
		return;
	}
	FInstancedStruct Payload;
	Declared->InitializePayload(Payload);
	FString Error;
	if (Payload.IsValid() && !CrowdyExec::Decode(*Definition, *Payload.GetScriptStruct(), Push.Payload, Payload.GetMutableMemory(), Error))
	{
		UE_LOG(LogCrowdyExec, Warning, TEXT("%s sent a %s message that does not fit its payload, so it was dropped: %s"), *Definition->TypeName, *Declared->Name.ToString(),
			*CrowdyExec::SafeText(Error, CrowdyServerObjectDetail::MaxReasonChars));
		return;
	}
	// A message may not overtake the state it describes, so one received while the values are read waits for the reads then out, not for those started after it.
	if (Status == ECrowdyServerObjectStatus::Connecting || OldestReadInFlight() != MAX_uint32)
	{
		if (PendingMessages.Num() >= CrowdyServerObjectDetail::MaxPendingMessages)
		{
			PendingMessages.RemoveAt(0);
			UE_LOG(LogCrowdyExec, Warning, TEXT("%s held back %d messages while reading its values, so the oldest was dropped"), *Definition->TypeName, CrowdyServerObjectDetail::MaxPendingMessages);
		}
		FHeldMessage& Held = PendingMessages.AddDefaulted_GetRef();
		Held.Name = Declared->Name;
		Held.Payload = MoveTemp(Payload);
		Held.Barrier = ReadTickets;
		return;
	}
	const uint32 ReceiveGeneration = Generation;
	FlushPendingMessages();
	if (Generation == ReceiveGeneration)
	{
		AnnounceMessage(Declared->Name, Payload);
	}
}

void UCrowdyServerObject::FlushPendingMessages()
{
	const uint32 FlushGeneration = Generation;
	while (!PendingMessages.IsEmpty() && Generation == FlushGeneration && Status == ECrowdyServerObjectStatus::Ready && PendingMessages[0].Barrier <= OldestReadInFlight())
	{
		const FHeldMessage Next = MoveTemp(PendingMessages[0]);
		PendingMessages.RemoveAt(0);
		AnnounceMessage(Next.Name, Next.Payload);
	}
}

uint32 UCrowdyServerObject::OldestReadInFlight() const
{
	uint32 Oldest = bReadInFlight ? StateReadTicket : MAX_uint32;
	for (TConstSetBitIterator<> It(OwnerReadsInFlight); It; ++It)
	{
		if (OwnerReadTickets.IsValidIndex(It.GetIndex()))
		{
			Oldest = FMath::Min(Oldest, OwnerReadTickets[It.GetIndex()]);
		}
	}
	return Oldest;
}

void UCrowdyServerObject::AnnounceMessage(FName Message, const FInstancedStruct& Payload)
{
	const TSharedRef<FMessageTopic>* Found = MessageTopics.Find(Message);
	if (!Found)
	{
		return;
	}
	const TSharedRef<FMessageTopic> Topic = *Found;
	const uint32 AnnounceGeneration = Generation;
	// A listener may release the object, which ends the announcement.
	auto Continues = [this, AnnounceGeneration]() { return Generation == AnnounceGeneration && Status == ECrowdyServerObjectStatus::Ready; };
	// A listener may watch or stop watching during the call; one stopped by an earlier listener is skipped.
	const TArray<FOnCrowdyServerMessage::FDelegate> Listeners = Topic->Listeners;
	for (const FOnCrowdyServerMessage::FDelegate& Listener : Listeners)
	{
		const FDelegateHandle Handle = Listener.GetHandle();
		if (!Continues())
		{
			return;
		}
		if (Topic->Listeners.ContainsByPredicate([Handle](const FOnCrowdyServerMessage::FDelegate& Each) { return Each.GetHandle() == Handle; }))
		{
			Listener.ExecuteIfBound(this, Message, Payload);
		}
	}
	const TArray<FCrowdyServerMessageEvent> Watchers = Topic->Events;
	for (const FCrowdyServerMessageEvent& Watcher : Watchers)
	{
		if (!Continues())
		{
			return;
		}
		if (Topic->Events.Contains(Watcher))
		{
			Watcher.ExecuteIfBound(this, Message, Payload);
		}
	}
	const int32 Held = Topic->Listeners.Num() + Topic->Events.Num();
	Topic->Listeners.RemoveAll([](const FOnCrowdyServerMessage::FDelegate& Each) { return !Each.IsBound(); });
	Topic->Events.RemoveAll([](const FCrowdyServerMessageEvent& Each) { return !Each.IsBound(); });
	if (Held != Topic->Listeners.Num() + Topic->Events.Num())
	{
		SyncMessageSubscriptions();
	}
}

void UCrowdyServerObject::HandleBoundMessage(UCrowdyServerObject* Announcer, FName Message, const FInstancedStruct& Payload)
{
	const TObjectPtr<UCrowdyServerObject> Self(this);
	MessageBindings.Notify(Self, Message, Payload);
}

void UCrowdyServerObject::NotifyStatus()
{
	// A native listener may move the status on (a rejoin restarts a Failed object); Blueprint then hears only the newer one.
	const ECrowdyServerObjectStatus Announced = Status;
	OnStatusChanged.Broadcast(this, Announced);
	if (Status != Announced)
	{
		return;
	}
	StatusChanged.Broadcast(this, Announced);
}

void UCrowdyServerObject::SetStatus(ECrowdyServerObjectStatus NewStatus)
{
	UE_CLOG(CrowdyExecTrace::Exec() && NewStatus != Status, LogCrowdyExec, Log, TEXT("exec: %s/%s status %s -> %s"), *Definition->TypeName, CrowdyServerObjectDetail::TraceId(*Definition, InstanceId),
		*CrowdyServerObjectDetail::TraceEnum(Status), *CrowdyServerObjectDetail::TraceEnum(NewStatus));
	Status = NewStatus;
}

void UCrowdyServerObject::TraceReread(const TCHAR* Reason) const
{
	UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: %s/%s reread reason=%s"), *Definition->TypeName,
		CrowdyServerObjectDetail::TraceId(*Definition, InstanceId), Reason);
}

void UCrowdyServerObject::NotifyValues(const TArray<FString>& Fields)
{
	TGuardValue<uint64> Announcing(AnnouncedSerial, FCrowdyServerVariableBindings::LatestSerial());
	ValuesChanged.Broadcast(this, Fields);
	const TObjectPtr<UCrowdyServerObject> Self(this);
	VariableBindings.Notify(Self, Fields);
	if (VariableWatchers.IsEmpty())
	{
		return;
	}
	// A watcher may watch or stop watching during the call; one stopped by an earlier watcher is skipped.
	const TArray<FString> Names = ToVariableNames(Fields);
	const TArray<FCrowdyServerVariablesEvent> Watchers = VariableWatchers;
	for (const FCrowdyServerVariablesEvent& Watcher : Watchers)
	{
		if (VariableWatchers.Contains(Watcher))
		{
			Watcher.ExecuteIfBound(this, Names);
		}
	}
	VariableWatchers.RemoveAll([](const FCrowdyServerVariablesEvent& Watcher) { return !Watcher.IsBound(); });
}

namespace CrowdyServerObjectDetail
{
	uint64 LatestBindingSerial = 0;

	void InitializeParams(const UFunction& Function, uint8* Params)
	{
		FMemory::Memzero(Params, Function.ParmsSize);
		for (TFieldIterator<FProperty> It(&Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->InitializeValue_InContainer(Params);
		}
	}

	void DestroyParams(const UFunction& Function, uint8* Params)
	{
		for (TFieldIterator<FProperty> It(&Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->DestroyValue_InContainer(Params);
		}
	}
}

uint64 FCrowdyServerVariableBindings::LatestSerial()
{
	return CrowdyServerObjectDetail::LatestBindingSerial;
}

bool FCrowdyServerVariableBindings::HasLiveHandler() const
{
	return Bindings.ContainsByPredicate([](const FBinding& Each) { return Each.Handler.IsValid(); });
}

void FCrowdyServerVariableBindings::Bind(UCrowdyServerObject* Object, FName Variable, UObject* Handler, FName Function)
{
	if (!Handler || Function.IsNone())
	{
		return;
	}
	Bindings.RemoveAll([Handler, Function](const FBinding& Each) { return !Each.Handler.IsValid() || (Each.Handler == Handler && Each.Function == Function); });
	const FName KeptName(*CrowdyExec::KeepNameCharacters(Variable.ToString()));
	const FBinding& Added = Bindings.Add_GetRef({Variable, KeptName, Handler, Function, ++CrowdyServerObjectDetail::LatestBindingSerial});
	if (Object && Object->GetStatus() == ECrowdyServerObjectStatus::Ready && FindValues(*Object))
	{
		Run(*Object, FBinding(Added));
	}
}

void FCrowdyServerVariableBindings::ReadRecord(bool bRecord, const FString& Id)
{
	bReadsRecord = bRecord;
	RecordId = bRecord ? Id : FString();
}

const FInstancedStruct* FCrowdyServerVariableBindings::FindValues(const UCrowdyServerObject& Object) const
{
	return Object.FindValues(bReadsRecord ? &RecordId : nullptr);
}

void FCrowdyServerVariableBindings::Unbind(FName Variable, const UObject* Handler, FName Function)
{
	Bindings.RemoveAll([Variable, Handler, Function](const FBinding& Each)
	{
		return Each.Variable == Variable && Each.Handler.Get() == Handler && Each.Function == Function;
	});
}

void FCrowdyServerVariableBindings::UnbindHandler(const UObject* Handler, FName Function)
{
	Bindings.RemoveAll([Handler, Function](const FBinding& Each) { return Each.Handler.Get() == Handler && Each.Function == Function; });
}

void FCrowdyServerVariableBindings::Notify(const TObjectPtr<UCrowdyServerObject>& Held, TConstArrayView<FString> Fields)
{
	UCrowdyServerObject* Object = Held;
	if (!Object || Bindings.IsEmpty())
	{
		return;
	}
	TArray<FName, TInlineAllocator<16>> Changed;
	for (const FString& Name : Object->ToVariableNames(Fields))
	{
		Changed.Add(FName(*Name, FNAME_Find));
	}
	const uint64 Announced = Object->GetAnnouncedSerial();
	// A handler may bind, unbind or move the holder to another object; a binding made or dropped meanwhile is skipped.
	const TArray<FBinding> Snapshot = Bindings;
	for (const FBinding& Binding : Snapshot)
	{
		if (Held != Object)
		{
			break;
		}
		const bool bRuns = Binding.Serial <= Announced && Changed.Contains(Binding.KeptName) && Binding.Handler.IsValid();
		if (bRuns && Bindings.Contains(Binding))
		{
			Run(*Object, Binding);
		}
	}
	Bindings.RemoveAll([](const FBinding& Each) { return !Each.Handler.IsValid(); });
}

void FCrowdyServerVariableBindings::Run(const UCrowdyServerObject& Object, const FBinding& Binding)
{
	const TPair<const UObject*, FName> Key(Binding.Handler.Get(), Binding.Function);
	if (Running.Contains(Key))
	{
		return;
	}
	Running.Add(Key);
	Call(Object, Binding);
	Running.RemoveSingleSwap(Key);
}

void FCrowdyServerVariableBindings::Call(const UCrowdyServerObject& Object, const FBinding& Binding) const
{
	const FInstancedStruct* Values = FindValues(Object);
	if (!Values)
	{
		return;
	}
	UObject* Handler = Binding.Handler.Get();
	UFunction* Function = Handler ? Handler->FindFunction(Binding.Function) : nullptr;
	TFieldIterator<FProperty> Param(Function);
	if (!Function || !Param || !Param->HasAnyPropertyFlags(CPF_Parm))
	{
		UE_LOG(LogCrowdyExec, Warning, TEXT("%s has no function %s taking a value, so %s's changes cannot reach it"), *GetNameSafe(Handler), *Binding.Function.ToString(), *Binding.Variable.ToString());
		return;
	}
	const FProperty* ValueParam = *Param;
	++Param;
	const FBoolProperty* HasValueParam = Param && Param->HasAnyPropertyFlags(CPF_Parm) ? CastField<FBoolProperty>(*Param) : nullptr;
	const FProperty* Field = UCrowdyServerObjectLibrary::FindServerValue(*Values, Binding.Variable);
	const FOptionalProperty* Optional = CastField<FOptionalProperty>(Field);
	const bool bEmpty = Optional && !Optional->IsSet(Optional->ContainerPtrToValuePtr<void>(Values->GetMemory()));

	// Only the parameters: a Blueprint function's locals lie past ParmsSize, and ProcessEvent sets them up itself.
	uint8* Params = static_cast<uint8*>(FMemory_Alloca_Aligned(Function->ParmsSize, Function->GetMinAlignment()));
	CrowdyServerObjectDetail::InitializeParams(*Function, Params);
	const bool bCopied = UCrowdyServerObjectLibrary::GetServerValueInto(*Values, Binding.Variable, ValueParam, ValueParam->ContainerPtrToValuePtr<void>(Params));
	if (!bCopied && !bEmpty)
	{
		UE_LOG(LogCrowdyExec, Warning, TEXT("%s.%s cannot take %s's value; check the variable and its type"), *GetNameSafe(Handler), *Binding.Function.ToString(), *Binding.Variable.ToString());
		CrowdyServerObjectDetail::DestroyParams(*Function, Params);
		return;
	}
	if (HasValueParam)
	{
		HasValueParam->SetPropertyValue_InContainer(Params, bCopied && (bReadsRecord || !Object.IsUnreadOwnerValue(Binding.Variable)));
	}
	Handler->ProcessEvent(Function, Params);
	CrowdyServerObjectDetail::DestroyParams(*Function, Params);
}

FCrowdyServerMessageBindings::~FCrowdyServerMessageBindings()
{
	Unfollow();
}

bool FCrowdyServerMessageBindings::HasLiveHandler() const
{
	return Bindings.ContainsByPredicate([](const FBinding& Each) { return Each.Handler.IsValid(); });
}

void FCrowdyServerMessageBindings::Follow(UCrowdyServerObject* Object, const FOnCrowdyServerMessage::FDelegate& InRelay)
{
	if (Object && Followed.Get() == Object)
	{
		return;
	}
	if (UCrowdyServerObject* Left = Followed.Get())
	{
		for (const TPair<FName, FDelegateHandle>& Pair : Watching)
		{
			Left->UnwatchMessage(Pair.Key, Pair.Value);
		}
	}
	Watching.Reset();
	Followed = Object;
	if (!Object)
	{
		return;
	}
	Relay = InRelay;
	for (const FBinding& Each : Bindings)
	{
		Watch(Each.Message);
	}
}

void FCrowdyServerMessageBindings::Watch(FName Message)
{
	UCrowdyServerObject* Object = Followed.Get();
	if (!Object || Watching.Contains(Message))
	{
		return;
	}
	const FDelegateHandle Handle = Object->WatchMessage(Message, Relay);
	Watching.Add(Message, Handle);
}

void FCrowdyServerMessageBindings::Unwatch(FName Message)
{
	FDelegateHandle Handle;
	UCrowdyServerObject* Object = Followed.Get();
	if (Watching.RemoveAndCopyValue(Message, Handle) && Object)
	{
		Object->UnwatchMessage(Message, Handle);
	}
}

void FCrowdyServerMessageBindings::Prune()
{
	TArray<FName, TInlineAllocator<4>> Unwanted;
	for (const TPair<FName, FDelegateHandle>& Pair : Watching)
	{
		const FName Message = Pair.Key;
		if (!Bindings.ContainsByPredicate([Message](const FBinding& Each) { return Each.Message == Message && Each.Handler.IsValid(); }))
		{
			Unwanted.Add(Message);
		}
	}
	for (const FName Message : Unwanted)
	{
		Unwatch(Message);
	}
}

void FCrowdyServerMessageBindings::Bind(FName Message, UObject* Handler, FName Function)
{
	if (!Handler || Function.IsNone())
	{
		return;
	}
	Bindings.RemoveAll([Handler, Function](const FBinding& Each) { return !Each.Handler.IsValid() || (Each.Handler == Handler && Each.Function == Function); });
	Bindings.Add({Message, Handler, Function});
	Watch(Message);
	Prune();
}

void FCrowdyServerMessageBindings::Unbind(FName Message, const UObject* Handler, FName Function)
{
	Bindings.RemoveAll([Message, Handler, Function](const FBinding& Each) { return Each.Message == Message && Each.Handler.Get() == Handler && Each.Function == Function; });
	Prune();
}

void FCrowdyServerMessageBindings::UnbindHandler(const UObject* Handler, FName Function)
{
	Bindings.RemoveAll([Handler, Function](const FBinding& Each) { return Each.Handler.Get() == Handler && Each.Function == Function; });
	Prune();
}

void FCrowdyServerMessageBindings::Reset()
{
	Bindings.Reset();
	Prune();
}

void FCrowdyServerMessageBindings::Notify(const TObjectPtr<UCrowdyServerObject>& Held, FName Message, const FInstancedStruct& Payload)
{
	UCrowdyServerObject* Object = Held;
	if (!Object || Bindings.IsEmpty())
	{
		return;
	}
	// A handler may bind, unbind or move the holder to another object; a binding made or dropped meanwhile is skipped.
	const TArray<FBinding> Snapshot = Bindings;
	for (const FBinding& Binding : Snapshot)
	{
		if (Held != Object)
		{
			break;
		}
		if (Binding.Message == Message && Binding.Handler.IsValid() && Bindings.Contains(Binding))
		{
			Run(Binding, Payload);
		}
	}
	Bindings.RemoveAll([](const FBinding& Each) { return !Each.Handler.IsValid(); });
	Prune();
}

void FCrowdyServerMessageBindings::Run(const FBinding& Binding, const FInstancedStruct& Payload)
{
	UObject* Handler = Binding.Handler.Get();
	const TPair<const UObject*, FName> Key(Handler, Binding.Function);
	UFunction* Function = Handler ? Handler->FindFunction(Binding.Function) : nullptr;
	TFieldIterator<FProperty> Param(Function);
	const FStructProperty* PayloadParam = Param && Param->HasAnyPropertyFlags(CPF_Parm) ? CastField<FStructProperty>(*Param) : nullptr;
	if (!Function || !PayloadParam || PayloadParam->Struct != TBaseStructure<FInstancedStruct>::Get())
	{
		UE_LOG(LogCrowdyExec, Warning, TEXT("%s has no function %s taking a payload, so %s's messages cannot reach it"), *GetNameSafe(Handler), *Binding.Function.ToString(), *Binding.Message.ToString());
		return;
	}
	if (Running.Contains(Key))
	{
		return;
	}
	Running.Add(Key);
	uint8* Params = static_cast<uint8*>(FMemory_Alloca_Aligned(Function->ParmsSize, Function->GetMinAlignment()));
	CrowdyServerObjectDetail::InitializeParams(*Function, Params);
	*PayloadParam->ContainerPtrToValuePtr<FInstancedStruct>(Params) = Payload;
	Handler->ProcessEvent(Function, Params);
	CrowdyServerObjectDetail::DestroyParams(*Function, Params);
	Running.RemoveSingleSwap(Key);
}

void UCrowdyServerObject::Call(FName Function, const FInstancedStruct& Params, TFunction<void(const FCrowdyServerCallResult&)> OnDone, float TimeoutSeconds)
{
	CallWith(Function, Params.GetScriptStruct(), Params.GetMemory(), MoveTemp(OnDone), TimeoutSeconds);
}

void UCrowdyServerObject::Call(FName Function, const FInstancedPropertyBag& Params, TFunction<void(const FCrowdyServerCallResult&)> OnDone, float TimeoutSeconds)
{
	CallWith(Function, Params.GetPropertyBagStruct(), Params.GetValue().GetMemory(), MoveTemp(OnDone), TimeoutSeconds);
}

FInstancedPropertyBag UCrowdyServerObject::MakeParams(FName Function) const
{
	const FCrowdyServerFunction* Found = Definition->FindFunction(Function);
	// A List emptied in the editor keeps a struct with no values, which sends nothing.
	const bool bSendsList = Found && Found->ParamsForm == ECrowdyServerValuesForm::List && Found->GetParamsStruct();
	return bSendsList ? Found->ParamsList : FInstancedPropertyBag();
}

FInstancedStruct UCrowdyServerObject::MakeInputs(FName Function) const
{
	return UCrowdyServerObjectLibrary::MakeFunctionInputs(Definition, Function);
}

void UCrowdyServerObject::CallWith(FName Function, const UScriptStruct* GivenStruct, const void* Params, TFunction<void(const FCrowdyServerCallResult&)> OnDone, float TimeoutSeconds)
{
	if (!OnDone)
	{
		OnDone = [](const FCrowdyServerCallResult&) {};
	}
	auto Refuse = [&OnDone](ECrowdyServerCallOutcome Outcome, const FString& Reason)
	{
		OnDone(CrowdyServerObjectDetail::MakeResult(Outcome, Reason));
	};
	if (Status == ECrowdyServerObjectStatus::Released)
	{
		Refuse(ECrowdyServerCallOutcome::Canceled, CrowdyServerObjectDetail::ReleasedReason);
		return;
	}
	if (Status == ECrowdyServerObjectStatus::Failed)
	{
		Refuse(CrowdyExec::IsAccessEndedReason(FailureReason) ? ECrowdyServerCallOutcome::AccessEnded : ECrowdyServerCallOutcome::Unavailable, FailureReason);
		return;
	}
	const FCrowdyServerFunction* Found = Definition->FindFunction(Function);
	if (!Found)
	{
		Refuse(ECrowdyServerCallOutcome::BadRequest, FString::Printf(TEXT("%s has no function named %s"), *Definition->TypeName, *Function.ToString()));
		return;
	}
	const UScriptStruct* ParamsStruct = Found->GetParamsStruct();
	if (GivenStruct != ParamsStruct)
	{
		const FString List = Found->GetParamsListName();
		Refuse(ECrowdyServerCallOutcome::BadRequest, ParamsStruct
			? FString::Printf(TEXT("%s takes %s params"), *Function.ToString(), List.IsEmpty() ? *CrowdyExec::DisplayName(ParamsStruct) : *List)
			: FString::Printf(TEXT("%s takes no params"), *Function.ToString()));
		return;
	}
	TArray<uint8> Payload;
	FString Error;
	if (!CrowdyExec::Encode(*Definition, ParamsStruct, Params, Payload, Error))
	{
		Refuse(ECrowdyServerCallOutcome::BadRequest, Error);
		return;
	}

	const uint32 CallId = ++NextCallId;
	FPendingCall& Pending = PendingCalls.Add(CallId);
	Pending.Function = Function;
	Pending.Method = Found->GetMethodName();
	Pending.Payload = MoveTemp(Payload);
	Pending.ReplyStruct = Found->GetReplyStruct();
	Pending.ReplyName = Found->ReplyForm == ECrowdyServerValuesForm::List ? Found->GetReplyListName() : CrowdyExec::DisplayName(Pending.ReplyStruct);
	Pending.OnDone = MoveTemp(OnDone);
	Pending.bSendOnce = Found->bSendOnce && Definition->IsGameKitStarter();
	Pending.MadeAt = FPlatformTime::Seconds();
	Pending.TimeoutSeconds = TimeoutSeconds;
	SendCall(CallId);
}

void UCrowdyServerObject::AddOwner(const UObject* Owner)
{
	if (!Owner)
	{
		return;
	}
	Owners.AddUnique(TWeakObjectPtr<const UObject>(Owner));
}

void UCrowdyServerObject::RemoveOwner(const UObject* Owner)
{
	if (!Owner)
	{
		return;
	}
	Owners.Remove(TWeakObjectPtr<const UObject>(Owner));
}

bool UCrowdyServerObject::PruneOwners()
{
	Owners.RemoveAll([](const TWeakObjectPtr<const UObject>& Owner) { return !Owner.IsValid(); });
	return Owners.Num() > 0;
}

void UCrowdyServerObject::HandleConnected()
{
	SyncMessageSubscriptions();
	if (Status != ECrowdyServerObjectStatus::Connecting || bReadInFlight || bRefreshScheduled)
	{
		return;
	}
	const uint32 ConnectGeneration = Generation;
	TArray<FString> Watched;
	CollectWatchedFields(nullptr, nullptr, Watched);
	// The members travel in the read, so an object with members reads even when it watches no variables, as does one of records.
	// A library has nothing to watch, whatever its asset still holds from when it was a Server Object.
	const bool bReads = Definition->IsGameKitStarter() ? !Definition->KitStateTopic.IsEmpty() || Definition->HasKitCollection()
		: Watched.Num() > 0 || Definition->MembersFrom != ECrowdyServerMembersSource::None || HoldsRecords();
	if (!Definition->IsFunctionLibrary() && bReads)
	{
		Refresh();
	}
	else
	{
		SetStatus(ECrowdyServerObjectStatus::Ready);
		NotifyStatus();
	}
	if (Generation == ConnectGeneration)
	{
		SendDueCalls();
	}
}

void UCrowdyServerObject::HandleReconnected()
{
	const CrowdyServerObjectDetail::FRoute Route = CrowdyServerObjectDetail::GetRoute(Subsystem, *Definition);
	if (!Route)
	{
		return;
	}
	DropMessageSubscriptions(true);
	SyncMessageSubscriptions(true);
	// Nothing to refresh before a subscription exists; a Failed or Released object never holds one.
	if (SubscribeHandle == 0 && CollectionHandle == 0)
	{
		return;
	}
	// A renewed subscription is never confirmed, so subscribe afresh for a real answer, then read what was missed; the message topics are subscribed afresh above.
	if (SubscribeHandle != 0)
	{
		Route.Unsubscribe(SubscribeHandle);
	}
	if (CollectionHandle != 0)
	{
		Route.Unsubscribe(CollectionHandle);
	}
	SubscribeHandle = 0;
	CollectionHandle = 0;
	bSubscribeConfirmed = false;
	TraceReread(TEXT("reconnect"));
	Refresh();
}

void UCrowdyServerObject::HandleCrowdyTeamsChanged()
{
	// Teammates' unchanged Team Only values reach a new member of their Crowdy Team only through a read.
	if (Status != ECrowdyServerObjectStatus::Ready || !bHasRead || !Definition->HasCrowdyTeamRecords())
	{
		return;
	}
	TraceReread(TEXT("team"));
	RequestRead();
}

void UCrowdyServerObject::SetConnectProblem(const FString& Reason)
{
	if (Status != ECrowdyServerObjectStatus::Connecting || (Reason.IsEmpty() && !bConnectSetReason))
	{
		return;
	}
	FailureReason = Reason;
	bConnectSetReason = !Reason.IsEmpty();
}

void UCrowdyServerObject::Restart()
{
	if (Status != ECrowdyServerObjectStatus::Failed)
	{
		return;
	}
	StopListening();
	SetStatus(ECrowdyServerObjectStatus::Connecting);
	FailureReason.Reset();
	bReadSetReason = false;
	bConnectSetReason = false;
	bHasRead = false;
	AppliedEpoch = 0;
	AppliedSeq = 0;

	// A fresh instance: re-initializing the same one clears it instead, which loses a Blueprint struct's defaults.
	FInstancedStruct Fresh;
	Definition->InitializeValues(Definition->GetStateStruct(), Fresh);
	State = MoveTemp(Fresh);
	Members.Reset();
	MemberCount = 0;
	Leader = 0;
	bOpenForJoining = true;
	bIsMember = false;
	bIsLeader = false;
	Records.Reset();
	AppliedRSeq = 0;
	OwnTeam.Reset();
	CollectionEpoch = 0;
	CollectionSeq = 0;
	bCollectionRead = false;
	OwnerReadsDone.Reset();

	const uint32 RestartGeneration = Generation;
	NotifyStatus();
	if (Generation != RestartGeneration || !CrowdyServerObjectDetail::GetConnection(Subsystem))
	{
		return;
	}
	HandleConnected();
}

void UCrowdyServerObject::Fail(const FString& Reason, bool bNotify)
{
	if (Status == ECrowdyServerObjectStatus::Failed || Status == ECrowdyServerObjectStatus::Released)
	{
		return;
	}
	SetStatus(ECrowdyServerObjectStatus::Failed);
	FailureReason = Reason;
	bReadSetReason = false;
	bConnectSetReason = false;
	StopListening();
	// Records hold what this player was allowed to see, which a sign-out or refusal must not leave behind.
	Records.Reset();
	AppliedRSeq = 0;
	OwnTeam.Reset();
	CollectionEpoch = 0;
	CollectionSeq = 0;
	bCollectionRead = false;
	ResetOwnerValues();
	const uint32 FailGeneration = Generation;

	TMap<uint32, FPendingCall> Calls = MoveTemp(PendingCalls);
	PendingCalls.Reset();
	const FCrowdyServerCallResult Canceled = CrowdyServerObjectDetail::MakeResult(
		CrowdyExec::IsAccessEndedReason(FailureReason) ? ECrowdyServerCallOutcome::AccessEnded : ECrowdyServerCallOutcome::Canceled, FailureReason);
	for (TPair<uint32, FPendingCall>& Pair : Calls)
	{
		TraceAnswered(Pair.Value, Canceled.Outcome);
		Pair.Value.OnDone(Canceled);
	}
	if (bNotify && Generation == FailGeneration)
	{
		NotifyStatus();
	}
}

void UCrowdyServerObject::MarkReleased(bool bNotify)
{
	if (Status == ECrowdyServerObjectStatus::Released)
	{
		return;
	}
	SetStatus(ECrowdyServerObjectStatus::Released);
	FailureReason = CrowdyServerObjectDetail::ReleasedReason;
	bReadSetReason = false;
	StopListening();

	TMap<uint32, FPendingCall> Calls = MoveTemp(PendingCalls);
	PendingCalls.Reset();
	const FCrowdyServerCallResult Canceled = CrowdyServerObjectDetail::MakeResult(ECrowdyServerCallOutcome::Canceled, FailureReason);
	for (TPair<uint32, FPendingCall>& Pair : Calls)
	{
		TraceAnswered(Pair.Value, Canceled.Outcome);
		if (bNotify)
		{
			Pair.Value.OnDone(Canceled);
		}
	}
	if (bNotify)
	{
		NotifyStatus();
	}
	ValuesChanged.Clear();
	VariableWatchers.Reset();
	VariableBindings.Reset();
	MessageBindings.Reset();
	MessageTopics.Reset();
	OnRecordsChanged.Clear();
	RecordWatchers.Reset();
	RecordEventWatchers.Reset();
}

void UCrowdyServerObject::StopListening()
{
	++Generation;
	if (SubscribeHandle != 0)
	{
		CrowdyServerObjectDetail::GetRoute(Subsystem, *Definition).Unsubscribe(SubscribeHandle);
	}
	SubscribeHandle = 0;
	bSubscribeConfirmed = false;
	if (CollectionHandle != 0)
	{
		CrowdyServerObjectDetail::GetRoute(Subsystem, *Definition).Unsubscribe(CollectionHandle);
	}
	CollectionHandle = 0;
	CollectionPushes.Reset();
	CollectionBytes = 0;
	bCollectionReadInFlight = false;
	bCollectionReadAgain = false;
	bKitStateFailing = false;
	bCollectionFailing = false;
	OwnerReadsInFlight.Reset();
	OwnerReadsAgain.Reset();
	OwnerReadsFailing.Reset();
	OwnerReadsWanted.Reset();
	DropMessageSubscriptions(true);
	PendingMessages.Reset();
	BufferedPushes.Reset();
	BufferedBytes = 0;
	bReadInFlight = false;
	bReadAgain = false;
	bRefreshScheduled = false;
	RefreshRetryIn = 0.f;
	RefreshFailures = 0;
	PagedRecords.Reset();
	bChangesInFlight = false;
	bChangesWanted = false;
	LateCalls.Reset();
}

void UCrowdyServerObject::Tick(float DeltaSeconds)
{
	if (Status != ECrowdyServerObjectStatus::Connecting && Status != ECrowdyServerObjectStatus::Ready)
	{
		return;
	}
	const uint32 TickGeneration = Generation;
	if (bRefreshScheduled)
	{
		RefreshRetryIn -= DeltaSeconds;
	}
	ChangesWait = FMath::Max(ChangesWait - DeltaSeconds, 0.f);
	TickMessages(DeltaSeconds);
	TickCalls(DeltaSeconds);
	if (Generation != TickGeneration || !CrowdyServerObjectDetail::GetConnection(Subsystem))
	{
		return;
	}
	if (bRefreshScheduled && RefreshRetryIn <= 0.f)
	{
		bRefreshScheduled = false;
		Refresh();
		return;
	}
	if (Status == ECrowdyServerObjectStatus::Connecting && SubscribeHandle == 0 && CollectionHandle == 0 && !bReadInFlight && !bCollectionReadInFlight && !bRefreshScheduled)
	{
		HandleConnected();
		return;
	}
	SendWantedOwnerReads();
	if (Generation == TickGeneration)
	{
		SendDueChanges();
	}
}

void UCrowdyServerObject::TickCalls(float DeltaSeconds)
{
	if (PendingCalls.IsEmpty())
	{
		return;
	}
	// A Game Kit starter's calls wait for the kit, which a connection may fail to start.
	const bool bConnected = static_cast<bool>(CrowdyServerObjectDetail::GetRoute(Subsystem, *Definition));
	TArray<uint32, TInlineAllocator<8>> Closed;
	TArray<uint32, TInlineAllocator<8>> TimedOut;
	TArray<uint32, TInlineAllocator<8>> Unsent;
	for (TPair<uint32, FPendingCall>& Pair : PendingCalls)
	{
		FPendingCall& Pending = Pair.Value;
		Pending.RetryIn -= Pending.bInFlight ? 0.f : DeltaSeconds;
		Pending.UnsentSeconds += (bConnected || Pending.bInFlight) ? 0.f : DeltaSeconds;
		// A long hitch counts as one second, so an answer waiting unread is not timed out by the frame that delayed it.
		Pending.ElapsedSeconds += FMath::Min(DeltaSeconds, 1.f);
		if (Pending.bConnectionClosed)
		{
			Closed.Add(Pair.Key);
			continue;
		}
		if (Pending.TimeoutSeconds > 0.f && Pending.ElapsedSeconds >= Pending.TimeoutSeconds)
		{
			TimedOut.Add(Pair.Key);
			continue;
		}
		if (Pending.UnsentSeconds > CrowdyServerObjectDetail::MaxUnsentSeconds)
		{
			Unsent.Add(Pair.Key);
		}
	}
	const uint32 TickGeneration = Generation;
	const FCrowdyServerCallResult ClosedResult = CrowdyServerObjectDetail::MakeResult(ECrowdyServerCallOutcome::Unavailable,
		TEXT("The connection to the server was closed"), true);
	const FCrowdyServerCallResult UnsentResult = CrowdyServerObjectDetail::MakeResult(ECrowdyServerCallOutcome::Unavailable,
		TEXT("The server could not be reached"), true);
	for (const uint32 CallId : Closed)
	{
		if (Generation != TickGeneration)
		{
			return;
		}
		CompleteCall(CallId, ClosedResult);
	}
	for (const uint32 CallId : TimedOut)
	{
		if (Generation != TickGeneration)
		{
			return;
		}
		const FPendingCall* Pending = PendingCalls.Find(CallId);
		if (!Pending)
		{
			continue;
		}
		const FString Reason = FString::Printf(TEXT("No answer within %s seconds; %s may still have run on the server"),
			*FString::SanitizeFloat(Pending->TimeoutSeconds, 0), *Pending->Function.ToString());
		if (Pending->bInFlight)
		{
			LateCalls.Add(CallId, ReadsMembershipAfter(*Pending));
		}
		CompleteCall(CallId, CrowdyServerObjectDetail::MakeResult(ECrowdyServerCallOutcome::Timeout, Reason, false));
	}
	for (const uint32 CallId : Unsent)
	{
		if (Generation != TickGeneration)
		{
			return;
		}
		CompleteCall(CallId, UnsentResult);
	}
	if (Generation == TickGeneration)
	{
		SendDueCalls();
	}
}

void UCrowdyServerObject::Refresh()
{
	const CrowdyServerObjectDetail::FRoute Route = CrowdyServerObjectDetail::GetRoute(Subsystem, *Definition);
	if (!Route)
	{
		if (Definition->IsGameKitStarter() && CrowdyServerObjectDetail::GetConnection(Subsystem))
		{
			SetConnectProblem(TEXT("The Game Kit could not be started on this connection, so this Game Kit starter waits for the next one"));
		}
		bRefreshScheduled = true;
		RefreshRetryIn = 0.f;
		return;
	}
	SyncMessageSubscriptions(true);
	const uint32 RefreshGeneration = Generation;
	if (Definition->HasKitCollection())
	{
		RefreshCollection();
	}
	if (Generation != RefreshGeneration || (Definition->IsGameKitStarter() && Definition->KitStateTopic.IsEmpty()))
	{
		return;
	}
	if (SubscribeHandle == 0)
	{
		TWeakObjectPtr<UCrowdyServerObject> WeakThis(this);
		const uint32 SubscribeGeneration = Generation;
		const uint32 Attempt = ++SubscribeAttempt;
		bSubscribeConfirmed = false;
		const uint64 Handle = Route.Subscribe(Definition->TypeName, InstanceId, CrowdyServerObjectDetail::StateTopic(*Definition),
			[WeakThis, SubscribeGeneration](const FCrowdyNativeExecPush& Push)
			{
				UCrowdyServerObject* Self = WeakThis.Get();
				if (Self && Self->Generation == SubscribeGeneration)
				{
					Self->HandlePush(Push);
				}
			},
			[WeakThis, SubscribeGeneration, Attempt](const FCrowdyNativeExecReply& Reply)
			{
				UCrowdyServerObject* Self = WeakThis.Get();
				if (Self && Self->Generation == SubscribeGeneration)
				{
					Self->HandleSubscribeDone(Attempt, Reply);
				}
			});
		if (Generation != SubscribeGeneration)
		{
			Route.Unsubscribe(Handle);
			return;
		}
		SubscribeHandle = Handle;
	}
	RequestRead();
}

void UCrowdyServerObject::RequestRead()
{
	if (bReadInFlight)
	{
		bReadAgain = true;
		return;
	}
	if (bRefreshScheduled)
	{
		return;
	}
	if (!CrowdyServerObjectDetail::GetConnection(Subsystem))
	{
		bRefreshScheduled = true;
		RefreshRetryIn = 0.f;
		return;
	}
	bReadAgain = false;
	PagedRecords.Reset();
	PageAfter.Reset();
	bFirstPage = true;
	SendRead();
}

void UCrowdyServerObject::SendRead()
{
	const CrowdyServerObjectDetail::FRoute Route = CrowdyServerObjectDetail::GetRoute(Subsystem, *Definition);
	if (!Route)
	{
		bReadInFlight = false;
		bRefreshScheduled = true;
		RefreshRetryIn = 0.f;
		return;
	}
	bReadInFlight = true;
	StateReadTicket = ReadTickets++;
	TWeakObjectPtr<UCrowdyServerObject> WeakThis(this);
	const uint32 ReadGeneration = Generation;
	const bool bKit = Definition->IsGameKitStarter();
	TArray<uint8> Request = bKit ? CrowdyServerObjectDetail::KitSnapshotRequest(Definition->KitStateTopic)
		: HoldsRecords() ? CrowdyServerObjectDetail::PageRequest(PageAfter) : TArray<uint8>{ 0x80 };
	Route.Call(Definition->TypeName, InstanceId, bKit ? TEXT("snapshot") : TEXT("read"), MoveTemp(Request), false,
		[WeakThis, ReadGeneration](const FCrowdyNativeExecReply& Reply, const FString& Code)
		{
			UCrowdyServerObject* Self = WeakThis.Get();
			if (Self && Self->Generation == ReadGeneration)
			{
				Self->HandleReadDone(Reply, Code);
			}
		});
}

void UCrowdyServerObject::HandleSubscribeDone(uint32 Attempt, const FCrowdyNativeExecReply& Reply)
{
	// An answer to a subscription since replaced says nothing about the current one.
	if (Attempt != SubscribeAttempt)
	{
		return;
	}
	// The reason names the instance only as the line's head does.
	const TCHAR* const ShownId = CrowdyServerObjectDetail::TraceId(*Definition, InstanceId);
	if (CrowdyExecTrace::Exec())
	{
		FString Refusal = Reply.IsOk() ? FString() : CrowdyServerObjectDetail::PlainReason(Reply);
		if (!InstanceId.IsEmpty())
		{
			Refusal = Refusal.Replace(*InstanceId, ShownId, ESearchCase::CaseSensitive);
		}
		UE_LOG(LogCrowdyExec, Log, TEXT("exec: %s/%s subscribe %s%s"), *Definition->TypeName, ShownId, Reply.IsOk() ? TEXT("ok") : TEXT("failed "), *Refusal);
	}
	if (Reply.IsOk())
	{
		bSubscribeConfirmed = true;
		return;
	}
	SubscribeHandle = 0;
	bSubscribeConfirmed = false;
	bKitStateFailing = Definition->IsGameKitStarter();
	HandleRefreshFailure(Reply, false);
}

void UCrowdyServerObject::HandleReadDone(const FCrowdyNativeExecReply& Reply, const FString& Code)
{
	bReadInFlight = false;
	if (!Reply.IsOk())
	{
		bKitStateFailing = Definition->IsGameKitStarter();
		const uint32 FailedGeneration = Generation;
		HandleRefreshFailure(Reply, true, Code);
		if (Generation == FailedGeneration)
		{
			FlushPendingMessages();
		}
		return;
	}
	// A Game Kit starter's records are its collection's, read apart from its State.
	if (HoldsRecords() && !Definition->IsGameKitStarter())
	{
		HandleRecordsPage(Reply);
		return;
	}
	FInstancedStruct Read;
	Definition->InitializeValues(Definition->GetStateStruct(), Read);
	FCrowdyExecStateHeader Header;
	FCrowdyExecMembersView View;
	TArray<FString> Carried;
	TBitArray<> CarriedFields;
	FString Error;
	const bool bDecoded = Definition->IsGameKitStarter()
		? CrowdyExec::DecodeKitSnapshot(*Definition, Reply.Payload, Read.GetMutableMemory(), Header, Error, &CarriedFields)
		: CrowdyExec::DecodeState(*Definition, ECrowdyExecStateMessage::Read, Reply.Payload, Read.GetMutableMemory(), Header, Carried, Error, &View);
	if (!bDecoded)
	{
		Fail(CrowdyExec::SafeText(Error, CrowdyServerObjectDetail::MaxReasonChars), true);
		return;
	}

	// A State edited since the last read has another shape, so every watched value counts as changed.
	const bool bSameShape = State.GetScriptStruct() == Read.GetScriptStruct();
	// Values read for a player who has since signed out are left at their defaults, and announced as changed.
	const bool bSameOwner = OwnerValuesUserId == GetSignedInUserId();
	const bool bKeptOwnerValues = Definition->IsGameKitStarter() && bSameShape && bSameOwner;
	if (bKeptOwnerValues)
	{
		KeepOwnerValues(State.GetMemory(), Read.GetMutableMemory(), &CarriedFields);
	}
	if (!bSameOwner)
	{
		OwnerValuesUserId = GetSignedInUserId();
		OwnerReadsDone.Init(false, Definition->KitOwnerValues.Num());
	}
	TArray<FString> Changed;
	CollectWatchedFields(bHasRead && bSameShape ? State.GetMemory() : nullptr, Read.GetMemory(), Changed);
	const CrowdyServerObjectDetail::FMemberFields MemberFields{Members, MemberCount, Leader, bOpenForJoining, bIsMember, bIsLeader};
	View = CrowdyServerObjectDetail::KeepDefinedMemberKeys(*Definition, MoveTemp(View));
	CrowdyServerObjectDetail::ApplyMemberView(MemberFields, View, true, !bHasRead, 0, Changed);
	// A Game Kit starter with a collection is Ready once its members are read too, and one with Owner Values once they are.
	const bool bBecameReady = Status == ECrowdyServerObjectStatus::Connecting && (!Definition->HasKitCollection() || bCollectionRead) && HasReadOwnerValues();
	State = MoveTemp(Read);
	AppliedEpoch = Header.Epoch;
	AppliedSeq = Header.Seq;
	bHasRead = true;
	bKitStateFailing = false;
	NoteGoodRead();
	// Every read of the State reads them again: a reconnect, a new epoch or a gap may have missed a changed message.
	RequestOwnerValues();
	UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: %s/%s read applied epoch=%llu seq=%llu changed=%s"), *Definition->TypeName,
		CrowdyServerObjectDetail::TraceId(*Definition, InstanceId),
		AppliedEpoch, AppliedSeq, *CrowdyServerObjectDetail::TraceNames(ToVariableNames(Changed)));
	if (bBecameReady)
	{
		SetStatus(ECrowdyServerObjectStatus::Ready);
	}

	// Values before status, so a listener bound from the Ready handler hears only its own catch-up call.
	const uint32 ReadGeneration = Generation;
	// A starter whose members are not read yet announces its values once they are, as it becomes Ready.
	if (Changed.Num() > 0 && Status != ECrowdyServerObjectStatus::Connecting)
	{
		NotifyValues(Changed);
	}
	if (bBecameReady && Definition->HasKitCollection() && Generation == ReadGeneration)
	{
		AnnounceHeldRecords();
	}
	if (bBecameReady && Generation == ReadGeneration)
	{
		NotifyStatus();
	}
	if (Generation != ReadGeneration)
	{
		return;
	}
	ReplayBufferedPushes();
	if (Generation == ReadGeneration && bReadAgain && !bReadInFlight)
	{
		RequestRead();
	}
	if (Generation == ReadGeneration)
	{
		FlushPendingMessages();
	}
}

void UCrowdyServerObject::NoteGoodRead()
{
	// The other half of a Game Kit starter still failing keeps its own backoff and reason.
	if (bKitStateFailing || bCollectionFailing || OwnerReadsFailing.Contains(true))
	{
		return;
	}
	// Until a subscribe is accepted, a working read must not undo its backoff or its reason, only a read's.
	if (bSubscribeConfirmed)
	{
		RefreshFailures = 0;
	}
	if (bSubscribeConfirmed || bReadSetReason)
	{
		FailureReason.Reset();
		bReadSetReason = false;
		bConnectSetReason = false;
	}
}

void UCrowdyServerObject::HandleRecordsPage(const FCrowdyNativeExecReply& Reply)
{
	FCrowdyExecRecordsMessage Page;
	FString Error;
	if (!CrowdyExec::DecodeRecords(*Definition, ECrowdyExecRecordsMessage::Read, Reply.Payload, [](const FString&) -> const void* { return nullptr; }, Page, Error))
	{
		Fail(CrowdyExec::SafeText(Error, CrowdyServerObjectDetail::MaxReasonChars), true);
		return;
	}
	// A server that restarted between pages started a new epoch, so the read starts over; the new read covers every push received so far.
	if (!bFirstPage && Page.Epoch != PageEpoch)
	{
		TraceReread(TEXT("epoch"));
		PagedRecords.Reset();
		PageAfter.Reset();
		BufferedPushes.Reset();
		BufferedBytes = 0;
		bFirstPage = true;
		SendRead();
		return;
	}
	if (bFirstPage)
	{
		PageEpoch = Page.Epoch;
		PageSeq = Page.Seq;
		PageRSeq = Page.RSeq;
		bFirstPage = false;
	}
	const int32 PagedBefore = PagedRecords.Num();
	for (FCrowdyExecRecordUpdate& Update : Page.Records)
	{
		PageAfter = Update.Id;
		FCrowdyServerRecord& Record = PagedRecords.FindOrAdd(MoveTemp(Update.Id));
		Record.Values = MoveTemp(Update.Values);
		Record.Owner = Update.Owner;
		Record.Team = MoveTemp(Update.Team);
	}
	if (PagedRecords.Num() > CrowdyExec::MaxElements)
	{
		Fail(FString::Printf(TEXT("The server sent more than the %d records an instance holds"), CrowdyExec::MaxElements), true);
		return;
	}
	// Each page before the last must bring a record not held yet, so a read ends within MaxElements pages.
	if (Page.bMore && PagedRecords.Num() == PagedBefore)
	{
		Fail(TEXT("The server sent a page of records with nothing new before the last"), true);
		return;
	}
	if (Page.bMore)
	{
		SendRead();
		return;
	}
	CommitRecordsRead();
}

void UCrowdyServerObject::CommitRecordsRead()
{
	// A State edited since the last read has another shape, so every record counts as new.
	const bool bSameShape = State.GetScriptStruct() == Definition->GetStateStruct();
	if (!bSameShape)
	{
		FInstancedStruct Fresh;
		Definition->InitializeValues(Definition->GetStateStruct(), Fresh);
		State = MoveTemp(Fresh);
	}
	TArray<FString> Changed;
	TArray<TBitArray<>> ChangedFields;
	TArray<FString> Removed;
	TArray<TPair<FString, TArray<FString>>> Watched;
	for (const TPair<FString, FCrowdyServerRecord>& Pair : PagedRecords)
	{
		const FCrowdyServerRecord* Old = bHasRead && bSameShape ? Records.Find(Pair.Key) : nullptr;
		TBitArray<> Fields;
		const bool bValues = CollectWatchedFieldBits(Old ? Old->Values.GetMemory() : nullptr, Pair.Value.Values.GetMemory(), Fields);
		const bool bOwnership = !Old || Old->Owner != Pair.Value.Owner || !Old->Team.Equals(Pair.Value.Team, ESearchCase::CaseSensitive);
		if (!bValues && !bOwnership)
		{
			continue;
		}
		Changed.Add(Pair.Key);
		if (bValues && IsRecordWatched(Pair.Key))
		{
			CollectWrittenFields(Fields, Watched.Emplace_GetRef(Pair.Key, TArray<FString>()).Value);
		}
		ChangedFields.Add(MoveTemp(Fields));
	}
	for (const TPair<FString, FCrowdyServerRecord>& Pair : Records)
	{
		if (PagedRecords.Contains(Pair.Key))
		{
			continue;
		}
		Removed.Add(Pair.Key);
		if (IsRecordWatched(Pair.Key))
		{
			CollectWatchedFields(nullptr, nullptr, Watched.Emplace_GetRef(Pair.Key, TArray<FString>()).Value, true);
		}
	}
	Records = MoveTemp(PagedRecords);
	PagedRecords.Reset();
	AppliedEpoch = PageEpoch;
	AppliedSeq = PageSeq;
	AppliedRSeq = PageRSeq;
	bHasRead = true;
	OwnTeam = Definition->HasCrowdyTeamRecords() ? FString() : FindOwnTeam();
	// A read carries every restricted value this player may see, so changes asked for before it are not needed.
	++ChangesTicket;
	bChangesWanted = false;
	NoteGoodRead();
	UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: %s/%s records read applied epoch=%llu seq=%llu rseq=%llu records=%d changed=%d removed=%d"),
		*Definition->TypeName, CrowdyServerObjectDetail::TraceId(*Definition, InstanceId), AppliedEpoch, AppliedSeq, AppliedRSeq, Records.Num(), Changed.Num(), Removed.Num());
	const bool bBecameReady = Status == ECrowdyServerObjectStatus::Connecting;
	if (bBecameReady)
	{
		SetStatus(ECrowdyServerObjectStatus::Ready);
	}

	// Records before status, so a listener bound from the Ready handler hears only its own catch-up call.
	const uint32 ReadGeneration = Generation;
	NotifyRecords(Changed, ChangedFields, Removed, Watched);
	if (bBecameReady && Generation == ReadGeneration)
	{
		NotifyStatus();
	}
	if (Generation != ReadGeneration)
	{
		return;
	}
	ReplayBufferedPushes();
	if (Generation == ReadGeneration && bReadAgain && !bReadInFlight)
	{
		RequestRead();
	}
	if (Generation == ReadGeneration)
	{
		FlushPendingMessages();
	}
}

void UCrowdyServerObject::HandleRefreshFailure(const FCrowdyNativeExecReply& Reply, bool bRead, const FString& Code)
{
	const TCHAR* const Verb = bRead ? TEXT("read") : TEXT("watch");
	const FString Permanent = PermanentRefusal(Reply, Verb, Code);
	if (!Permanent.IsEmpty())
	{
		Fail(Permanent, true);
		return;
	}
	// A refused subscribe's reason outlasts a read's, since a good read clears only the read's own.
	FCrowdyNativeExecReply Shown = Reply;
	const FString Refusal = Reply.Status == ECrowdyNativeExecStatus::DeadlineExceeded && Reply.bNoReply && !Definition->IsGameKitStarter()
		? CrowdyServerObjectDetail::RedialRefusal(Subsystem) : FString();
	if (!Refusal.IsEmpty())
	{
		Shown.Status = ECrowdyNativeExecStatus::AppPaused;
		Shown.Message = Refusal;
	}
	const FString Waiting = CrowdyServerObjectDetail::WaitingReason(*Definition, Shown, bRead);
	const bool bKeepWatchReason = bRead && !bSubscribeConfirmed && !bReadSetReason && !FailureReason.IsEmpty();
	if (!Waiting.IsEmpty() && !bKeepWatchReason)
	{
		FailureReason = Waiting;
		bReadSetReason = bRead;
		bConnectSetReason = false;
	}
	ScheduleRefresh(Reply);
}

FString UCrowdyServerObject::PermanentRefusal(const FCrowdyNativeExecReply& Reply, const TCHAR* Verb, const FString& Code) const
{
	const bool bAppError = Reply.Status == ECrowdyNativeExecStatus::AppError;
	// A starter's coded refusal of its own snapshot, such as forbidden or a topic it does not have, answers the same next time.
	if (bAppError && !Code.IsEmpty() && !CrowdyServerObjectDetail::IsRetryableKitCode(Code))
	{
		return FString::Printf(TEXT("The server code for %s refused to let this player %s it (%s)%s%s"), *Definition->TypeName, Verb, *Code,
			Reply.Message.IsEmpty() ? TEXT("") : TEXT(": "), *CrowdyExec::SafeText(Reply.Message, CrowdyServerObjectDetail::MaxReasonChars));
	}
	if (Reply.Status == ECrowdyNativeExecStatus::Denied && CrowdyExec::IsAccessEndedText(Reply.Message))
	{
		return CrowdyExec::AccessEndedReason(Reply.Message);
	}
	if (bAppError && Reply.Message.StartsWith(TEXT("denied"), ESearchCase::CaseSensitive))
	{
		return FString::Printf(TEXT("This player may not %s this Server Object"), Verb);
	}
	if (bAppError && Reply.Message.StartsWith(TEXT("unknown_method"), ESearchCase::CaseSensitive))
	{
		return FString::Printf(TEXT("The server code for %s has no watched values; regenerate and deploy it"), *Definition->TypeName);
	}
	return Reply.Status == ECrowdyNativeExecStatus::BadRequest ? CrowdyServerObjectDetail::PlainReason(Reply) : FString();
}

void UCrowdyServerObject::ScheduleRefresh(const FCrowdyNativeExecReply& Reply)
{
	++RefreshFailures;
	const float Jitter = CrowdyServerObjectDetail::NextJitter(Subsystem);
	RefreshRetryIn = CrowdyServerObjectDetail::IsMissingRoot(*Definition, Reply) ? CrowdyServerObjectDetail::Jittered(CrowdyServerObjectDetail::MissingRootRetrySeconds, Jitter)
		: CrowdyServerObjectDetail::RetryInSeconds(RefreshFailures, Reply, Jitter);
	bRefreshScheduled = true;
}

void UCrowdyServerObject::ReceivePush(const TArray<uint8>& Payload)
{
	if (Status != ECrowdyServerObjectStatus::Connecting && Status != ECrowdyServerObjectStatus::Ready)
	{
		return;
	}
	if (bHasRead && !bReadInFlight)
	{
		ApplyPush(Payload);
		return;
	}
	// Each Game Kit snapshot is a whole State, so only the newest is worth holding.
	if (Definition->IsGameKitStarter())
	{
		BufferedPushes.Reset();
		BufferedBytes = 0;
	}
	const bool bFull = HoldsRecords()
		? BufferedPushes.Num() >= CrowdyServerObjectDetail::MaxBufferedRecordPushes || BufferedBytes + Payload.Num() > CrowdyServerObjectDetail::MaxBufferedRecordBytes
		: BufferedPushes.Num() >= CrowdyServerObjectDetail::MaxBufferedPushes;
	if (bFull)
	{
		BufferedPushes.Reset();
		BufferedBytes = 0;
		bReadAgain = true;
		TraceReread(TEXT("overflow"));
		return;
	}
	BufferedPushes.Add(Payload);
	BufferedBytes += Payload.Num();
}

void UCrowdyServerObject::ApplyPush(const TArray<uint8>& Payload)
{
	if (Definition->IsGameKitStarter())
	{
		ApplyKitSnapshot(Payload);
		return;
	}
	// A records push nests deeper than a header read skips, so it is decoded whole and its header checked after.
	if (HoldsRecords())
	{
		ApplyRecordsPush(Payload);
		return;
	}
	FCrowdyExecStateHeader Header;
	FString Error;
	if (!CrowdyExec::ReadStateHeader(Payload, Header, Error))
	{
		RequestRead();
		return;
	}
	if (!AcceptsPush(Header))
	{
		return;
	}
	FCrowdyExecMembersView View;
	TArray<FString> Carried;
	if (!CrowdyExec::DecodeState(*Definition, ECrowdyExecStateMessage::Push, Payload, State.GetMutableMemory(), Header, Carried, Error, &View))
	{
		RequestRead();
		return;
	}
	AppliedSeq = Header.Seq;
	const CrowdyServerObjectDetail::FMemberFields MemberFields{Members, MemberCount, Leader, bOpenForJoining, bIsMember, bIsLeader};
	View = CrowdyServerObjectDetail::KeepDefinedMemberKeys(*Definition, MoveTemp(View));
	const int64 UserId = View.bHasMembers || View.bHasLeader ? CrowdyServerObjectDetail::FindSignedInUserId(Subsystem) : 0;
	CrowdyServerObjectDetail::ApplyMemberView(MemberFields, View, false, false, UserId, Carried);
	UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: %s/%s push applied epoch=%llu seq=%llu changed=%s"), *Definition->TypeName,
		CrowdyServerObjectDetail::TraceId(*Definition, InstanceId),
		AppliedEpoch, AppliedSeq, *CrowdyServerObjectDetail::TraceNames(ToVariableNames(Carried)));
	if (Carried.Num() > 0)
	{
		NotifyValues(Carried);
	}
}

void UCrowdyServerObject::ApplyKitSnapshot(const TArray<uint8>& Payload)
{
	FCrowdyExecKitHeader Header;
	FString Error;
	if (!CrowdyExec::ReadKitHeader(Payload, Header, Error))
	{
		RequestRead();
		return;
	}
	// A whole snapshot needs no gap check, and a new epoch's first one is taken as it is.
	if (Header.Epoch == AppliedEpoch && Header.Seq <= AppliedSeq)
	{
		UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: %s/%s push stale epoch=%llu seq=%llu"), *Definition->TypeName,
			CrowdyServerObjectDetail::TraceId(*Definition, InstanceId), AppliedEpoch, AppliedSeq);
		return;
	}
	// Only snapshots are let through to here, so the header read above is the one check of it.
	const UScriptStruct* StateStruct = Definition->GetStateStruct();
	FInstancedStruct Next;
	Definition->InitializeValues(StateStruct, Next);
	TBitArray<> CarriedFields;
	if (!StateStruct || State.GetScriptStruct() != StateStruct || !CrowdyExec::DecodeWritten(*Definition, *StateStruct, Payload, Next.GetMutableMemory(), &CarriedFields, Error))
	{
		RequestRead();
		return;
	}
	// Another player signed in since the Owner Values were read: a read of the State reads theirs.
	if (OwnerValuesUserId != GetSignedInUserId() && !Definition->KitOwnerValues.IsEmpty())
	{
		RequestRead();
		return;
	}
	KeepOwnerValues(State.GetMemory(), Next.GetMutableMemory(), &CarriedFields);
	TArray<FString> Changed;
	CollectWatchedFields(State.GetMemory(), Next.GetMemory(), Changed);
	const bool bNewEpoch = Header.Epoch != AppliedEpoch;
	State = MoveTemp(Next);
	AppliedEpoch = Header.Epoch;
	AppliedSeq = Header.Seq;
	UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: %s/%s push applied epoch=%llu seq=%llu changed=%s"), *Definition->TypeName,
		CrowdyServerObjectDetail::TraceId(*Definition, InstanceId), AppliedEpoch, AppliedSeq, *CrowdyServerObjectDetail::TraceNames(ToVariableNames(Changed)));
	// A restarted hub sends no changed message for the Owner Values it holds.
	if (bNewEpoch)
	{
		RequestOwnerValues();
	}
	// A starter still waiting for its records or Owner Values announces every value as it becomes Ready.
	if (Changed.Num() > 0 && Status != ECrowdyServerObjectStatus::Connecting)
	{
		NotifyValues(Changed);
	}
}

bool UCrowdyServerObject::AcceptsPush(const FCrowdyExecStateHeader& Header)
{
	if (Header.Epoch != AppliedEpoch)
	{
		TraceReread(TEXT("epoch"));
		RequestRead();
		return false;
	}
	if (Header.Seq <= AppliedSeq)
	{
		UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: %s/%s push stale epoch=%llu seq=%llu"), *Definition->TypeName,
			CrowdyServerObjectDetail::TraceId(*Definition, InstanceId),
			AppliedEpoch, AppliedSeq);
		return false;
	}
	// Only Every Player pushes carry values, a gap missed some, and a State reshaped in the editor cannot take one: read instead.
	const bool bOwnerOnly = Definition->Visibility != ECrowdyServerObjectVisibility::Public;
	const bool bGap = Header.Seq > AppliedSeq + 1;
	if (bOwnerOnly || bGap || State.GetScriptStruct() != Definition->GetStateStruct())
	{
		if (bOwnerOnly || bGap)
		{
			TraceReread(bOwnerOnly ? TEXT("ownerOnly") : TEXT("gap"));
		}
		RequestRead();
		return false;
	}
	return true;
}

void UCrowdyServerObject::ReplayBufferedPushes()
{
	TArray<TArray<uint8>> Pending = MoveTemp(BufferedPushes);
	BufferedPushes.Reset();
	BufferedBytes = 0;
	const uint32 ReplayGeneration = Generation;
	for (const TArray<uint8>& Payload : Pending)
	{
		// A read requested by an earlier replayed push covers every push still buffered.
		if (Generation != ReplayGeneration || bReadInFlight)
		{
			return;
		}
		ReceivePush(Payload);
	}
}

void UCrowdyServerObject::RefreshCollection()
{
	const CrowdyServerObjectDetail::FRoute Route = CrowdyServerObjectDetail::GetRoute(Subsystem, *Definition);
	if (!Route)
	{
		return;
	}
	if (CollectionHandle == 0)
	{
		TWeakObjectPtr<UCrowdyServerObject> WeakThis(this);
		const uint32 SubscribeGeneration = Generation;
		const uint32 Attempt = ++CollectionAttempt;
		const uint64 Handle = Route.Subscribe(Definition->TypeName, InstanceId, Definition->KitCollectionTopic,
			[WeakThis, SubscribeGeneration](const FCrowdyNativeExecPush& Push)
			{
				UCrowdyServerObject* Self = WeakThis.Get();
				if (Self && Self->Generation == SubscribeGeneration)
				{
					Self->HandlePush(Push);
				}
			},
			[WeakThis, SubscribeGeneration, Attempt](const FCrowdyNativeExecReply& Reply)
			{
				UCrowdyServerObject* Self = WeakThis.Get();
				if (Self && Self->Generation == SubscribeGeneration)
				{
					Self->HandleCollectionSubscribeDone(Attempt, Reply);
				}
			});
		if (Generation != SubscribeGeneration)
		{
			Route.Unsubscribe(Handle);
			return;
		}
		CollectionHandle = Handle;
	}
	RequestCollectionRead();
}

void UCrowdyServerObject::RequestCollectionRead()
{
	if (bCollectionReadInFlight)
	{
		bCollectionReadAgain = true;
		return;
	}
	// A scheduled refresh reads the members again itself.
	if (bRefreshScheduled)
	{
		return;
	}
	const CrowdyServerObjectDetail::FRoute Route = CrowdyServerObjectDetail::GetRoute(Subsystem, *Definition);
	if (!Route)
	{
		bRefreshScheduled = true;
		RefreshRetryIn = 0.f;
		return;
	}
	bCollectionReadInFlight = true;
	bCollectionReadAgain = false;
	TWeakObjectPtr<UCrowdyServerObject> WeakThis(this);
	const uint32 ReadGeneration = Generation;
	Route.Call(Definition->TypeName, InstanceId, TEXT("snapshot"), CrowdyServerObjectDetail::KitSnapshotRequest(Definition->KitCollectionTopic), false,
		[WeakThis, ReadGeneration](const FCrowdyNativeExecReply& Reply, const FString& Code)
		{
			UCrowdyServerObject* Self = WeakThis.Get();
			if (Self && Self->Generation == ReadGeneration)
			{
				Self->HandleCollectionReadDone(Reply, Code);
			}
		});
}

void UCrowdyServerObject::HandleCollectionSubscribeDone(uint32 Attempt, const FCrowdyNativeExecReply& Reply)
{
	if (Attempt != CollectionAttempt || Reply.IsOk())
	{
		return;
	}
	UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: %s/%s collection subscribe failed %s"), *Definition->TypeName,
		CrowdyServerObjectDetail::TraceId(*Definition, InstanceId), *CrowdyServerObjectDetail::PlainReason(Reply));
	CollectionHandle = 0;
	bCollectionFailing = true;
	HandleRefreshFailure(Reply, false);
}

void UCrowdyServerObject::HandleCollectionReadDone(const FCrowdyNativeExecReply& Reply, const FString& Code)
{
	bCollectionReadInFlight = false;
	if (!Reply.IsOk())
	{
		bCollectionFailing = true;
		HandleRefreshFailure(Reply, true, Code);
		return;
	}
	FCrowdyExecKitHeader Header;
	TArray<FCrowdyExecKitMember> Read;
	FString Error;
	if (!CrowdyExec::DecodeKitCollection(*Definition, Reply.Payload, Header, Read, Error) || Header.Kind != ECrowdyExecKitKind::Snapshot)
	{
		Fail(CrowdyExec::SafeText(Error.IsEmpty() ? TEXT("The server answered a read of the collection with a delta") : Error, CrowdyServerObjectDetail::MaxReasonChars), true);
		return;
	}
	CollectionEpoch = Header.Epoch;
	CollectionSeq = Header.Seq;
	bCollectionRead = true;
	bCollectionFailing = false;
	NoteGoodRead();
	// Values and records before status, so a listener bound from the Ready handler hears only its own catch-up call.
	const bool bBecameReady = Status == ECrowdyServerObjectStatus::Connecting && (Definition->KitStateTopic.IsEmpty() || bHasRead) && HasReadOwnerValues();
	if (bBecameReady)
	{
		SetStatus(ECrowdyServerObjectStatus::Ready);
	}
	const uint32 ReadGeneration = Generation;
	// The State read before it held its announcement for this.
	TArray<FString> Values;
	if (bBecameReady && bHasRead)
	{
		CollectWatchedFields(nullptr, nullptr, Values);
	}
	if (Values.Num() > 0)
	{
		NotifyValues(Values);
	}
	if (Generation != ReadGeneration)
	{
		return;
	}
	ApplyCollection(Read, true, Status == ECrowdyServerObjectStatus::Ready);
	if (bBecameReady && Generation == ReadGeneration)
	{
		NotifyStatus();
	}
	if (Generation != ReadGeneration)
	{
		return;
	}
	TArray<TArray<uint8>> Held = MoveTemp(CollectionPushes);
	CollectionPushes.Reset();
	CollectionBytes = 0;
	for (const TArray<uint8>& Payload : Held)
	{
		// A read requested by an earlier held push covers every push still held.
		if (Generation != ReadGeneration || bCollectionReadInFlight)
		{
			return;
		}
		ReceiveCollectionPush(Payload);
	}
	if (Generation == ReadGeneration && bCollectionReadAgain && !bCollectionReadInFlight)
	{
		RequestCollectionRead();
	}
	if (Generation == ReadGeneration)
	{
		FlushPendingMessages();
	}
}

void UCrowdyServerObject::ReceiveCollectionPush(const TArray<uint8>& Payload)
{
	if (Status != ECrowdyServerObjectStatus::Connecting && Status != ECrowdyServerObjectStatus::Ready)
	{
		return;
	}
	if (bCollectionRead && !bCollectionReadInFlight)
	{
		ApplyCollectionPush(Payload);
		return;
	}
	const bool bFull = CollectionPushes.Num() >= CrowdyServerObjectDetail::MaxBufferedRecordPushes
		|| CollectionBytes + Payload.Num() > CrowdyServerObjectDetail::MaxBufferedRecordBytes;
	if (bFull)
	{
		CollectionPushes.Reset();
		CollectionBytes = 0;
		bCollectionReadAgain = true;
		TraceReread(TEXT("collectionOverflow"));
		return;
	}
	CollectionPushes.Add(Payload);
	CollectionBytes += Payload.Num();
}

void UCrowdyServerObject::ApplyCollectionPush(const TArray<uint8>& Payload)
{
	FCrowdyExecKitHeader Header;
	TArray<FCrowdyExecKitMember> Pushed;
	FString Error;
	if (!CrowdyExec::DecodeKitCollection(*Definition, Payload, Header, Pushed, Error))
	{
		UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: %s/%s collection push dropped: %s"), *Definition->TypeName,
			CrowdyServerObjectDetail::TraceId(*Definition, InstanceId), *CrowdyExec::SafeText(Error, CrowdyServerObjectDetail::MaxReasonChars));
		RequestCollectionRead();
		return;
	}
	// A new epoch's snapshot is taken as it is; a delta of another epoch, or after a missed seq, means the members must be read again.
	const bool bSnapshot = Header.Kind == ECrowdyExecKitKind::Snapshot;
	const bool bSameEpoch = Header.Epoch == CollectionEpoch;
	if (bSameEpoch && Header.Seq <= CollectionSeq)
	{
		UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: %s/%s collection push stale epoch=%llu seq=%llu"), *Definition->TypeName,
			CrowdyServerObjectDetail::TraceId(*Definition, InstanceId), CollectionEpoch, CollectionSeq);
		return;
	}
	if (!bSnapshot && (!bSameEpoch || Header.Seq != CollectionSeq + 1))
	{
		TraceReread(bSameEpoch ? TEXT("collectionGap") : TEXT("collectionEpoch"));
		RequestCollectionRead();
		return;
	}
	CollectionEpoch = Header.Epoch;
	CollectionSeq = Header.Seq;
	ApplyCollection(Pushed, bSnapshot, Status == ECrowdyServerObjectStatus::Ready);
}

void UCrowdyServerObject::ApplyCollection(TArray<FCrowdyExecKitMember>& Changes, bool bWhole, bool bAnnounce)
{
	TArray<FString> Changed;
	TArray<TBitArray<>> ChangedFields;
	TArray<FString> Removed;
	TArray<TPair<FString, TArray<FString>>> Watched;
	TCrowdyRecordMap<FCrowdyServerRecord> Next;
	TCrowdyRecordMap<FCrowdyServerRecord>& Into = bWhole ? Next : Records;
	for (FCrowdyExecKitMember& Member : Changes)
	{
		if (Member.bRemoved)
		{
			if (Records.Remove(Member.Id) > 0)
			{
				Removed.Add(MoveTemp(Member.Id));
			}
			continue;
		}
		const FCrowdyServerRecord* Old = Records.Find(Member.Id);
		TBitArray<> Fields;
		const bool bValues = CollectWatchedFieldBits(Old ? Old->Values.GetMemory() : nullptr, Member.Values.GetMemory(), Fields);
		if (Old && !bValues)
		{
			if (bWhole)
			{
				Next.Add(Member.Id, *Old);
			}
			continue;
		}
		if (bValues && IsRecordWatched(Member.Id))
		{
			CollectWrittenFields(Fields, Watched.Emplace_GetRef(Member.Id, TArray<FString>()).Value);
		}
		Changed.Add(Member.Id);
		ChangedFields.Add(MoveTemp(Fields));
		Into.FindOrAdd(MoveTemp(Member.Id)).Values = MoveTemp(Member.Values);
	}
	if (bWhole)
	{
		// A snapshot leaves out the members it removed.
		for (const TPair<FString, FCrowdyServerRecord>& Pair : Records)
		{
			if (!Next.Contains(Pair.Key))
			{
				Removed.Add(Pair.Key);
			}
		}
		Records = MoveTemp(Next);
	}
	for (const FString& Id : Removed)
	{
		if (IsRecordWatched(Id))
		{
			CollectWatchedFields(nullptr, nullptr, Watched.Emplace_GetRef(Id, TArray<FString>()).Value, true);
		}
	}
	if (Records.Num() > CrowdyExec::MaxElements)
	{
		Fail(FString::Printf(TEXT("The server sent more than the %d records an instance holds"), CrowdyExec::MaxElements), true);
		return;
	}
	UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: %s/%s collection applied epoch=%llu seq=%llu records=%d changed=%d removed=%d"),
		*Definition->TypeName, CrowdyServerObjectDetail::TraceId(*Definition, InstanceId), CollectionEpoch, CollectionSeq, Records.Num(), Changed.Num(), Removed.Num());
	if (bAnnounce)
	{
		NotifyRecords(Changed, ChangedFields, Removed, Watched);
	}
}

void UCrowdyServerObject::AnnounceHeldRecords()
{
	TArray<FString> Changed;
	TArray<TBitArray<>> ChangedFields;
	TArray<TPair<FString, TArray<FString>>> Watched;
	for (const TPair<FString, FCrowdyServerRecord>& Pair : Records)
	{
		Changed.Add(Pair.Key);
		CollectWatchedFieldBits(nullptr, Pair.Value.Values.GetMemory(), ChangedFields.AddDefaulted_GetRef());
		if (IsRecordWatched(Pair.Key))
		{
			CollectWatchedFields(nullptr, nullptr, Watched.Emplace_GetRef(Pair.Key, TArray<FString>()).Value, true);
		}
	}
	NotifyRecords(Changed, ChangedFields, TArray<FString>(), Watched);
}

bool UCrowdyServerObject::ReadsOwnerValues() const
{
	if (!Definition->IsGameKitStarter() || Definition->KitOwnerValues.IsEmpty())
	{
		return false;
	}
	const int64 UserId = GetSignedInUserId();
	return !Definition->IsKeyedByPlayer() || (UserId > 0 && InstanceId.Equals(LexToString(UserId), ESearchCase::CaseSensitive));
}

bool UCrowdyServerObject::HasReadOwnerValues() const
{
	return !ReadsOwnerValues() || (OwnerReadsDone.Num() == Definition->KitOwnerValues.Num() && OwnerReadsDone.Find(false) == INDEX_NONE);
}

bool UCrowdyServerObject::IsUnreadOwnerValue(FName Variable) const
{
	if (!Definition || Definition->KitOwnerValues.IsEmpty() || ReadsOwnerValues())
	{
		return false;
	}
	const FProperty* Field = UCrowdyServerObjectLibrary::FindServerValue(State, Variable);
	return Field && Definition->KitOwnerValues.ContainsByPredicate([this, Field](const FCrowdyKitOwnerValue& Owned) { return Definition->FindVariable(Owned.Variable) == Field; });
}

void UCrowdyServerObject::RequestOwnerValues()
{
	if (!ReadsOwnerValues())
	{
		return;
	}
	for (int32 Index = 0; Index < Definition->KitOwnerValues.Num(); ++Index)
	{
		RequestOwnerValue(Index);
	}
}

void UCrowdyServerObject::RequestOwnerValue(int32 Index)
{
	const int32 Count = Definition->KitOwnerValues.Num();
	if (!Definition->KitOwnerValues.IsValidIndex(Index))
	{
		return;
	}
	for (TBitArray<>* Bits : {&OwnerReadsInFlight, &OwnerReadsAgain, &OwnerReadsDone, &OwnerReadsFailing})
	{
		if (Bits->Num() != Count)
		{
			Bits->Init(false, Count);
		}
	}
	if (OwnerReadTickets.Num() != Count)
	{
		OwnerReadTickets.Init(0, Count);
	}
	if (OwnerReadsInFlight[Index])
	{
		OwnerReadsAgain[Index] = true;
		return;
	}
	const CrowdyServerObjectDetail::FRoute Route = CrowdyServerObjectDetail::GetRoute(Subsystem, *Definition);
	// A refresh reads the State, and the Owner Values after it.
	if (!Route)
	{
		bRefreshScheduled = true;
		RefreshRetryIn = 0.f;
		return;
	}
	OwnerReadsInFlight[Index] = true;
	OwnerReadsAgain[Index] = false;
	OwnerReadTickets[Index] = ReadTickets++;
	TWeakObjectPtr<UCrowdyServerObject> WeakThis(this);
	const uint32 ReadGeneration = Generation;
	const uint32 ReadEra = OwnerReadEra;
	Route.Call(Definition->TypeName, InstanceId, Definition->KitOwnerValues[Index].Method, TArray<uint8>{0x80}, false,
		[WeakThis, ReadGeneration, ReadEra, Index](const FCrowdyNativeExecReply& Reply, const FString& Code)
		{
			UCrowdyServerObject* Self = WeakThis.Get();
			if (Self && Self->Generation == ReadGeneration && Self->OwnerReadEra == ReadEra)
			{
				Self->HandleOwnerValueDone(Index, Reply, Code);
			}
		});
}

bool UCrowdyServerObject::HoldsOwnerRead(int32 Index) const
{
	const int32 Count = Definition->KitOwnerValues.Num();
	return Definition->KitOwnerValues.IsValidIndex(Index) && OwnerReadsInFlight.Num() == Count && OwnerReadsAgain.Num() == Count && OwnerReadsDone.Num() == Count
		&& OwnerReadsFailing.Num() == Count && OwnerReadTickets.Num() == Count;
}

void UCrowdyServerObject::HandleOwnerValueDone(int32 Index, const FCrowdyNativeExecReply& Reply, const FString& Code)
{
	if (!HoldsOwnerRead(Index))
	{
		RequestRead();
		return;
	}
	OwnerReadsInFlight[Index] = false;
	if (!Reply.IsOk())
	{
		OwnerReadsFailing[Index] = true;
		const uint32 FailedGeneration = Generation;
		HandleRefreshFailure(Reply, true, Code);
		if (Generation == FailedGeneration)
		{
			FlushPendingMessages();
		}
		return;
	}
	const FCrowdyKitOwnerValue& Owned = Definition->KitOwnerValues[Index];
	const FProperty* Variable = Definition->FindVariable(Owned.Variable);
	FString Error;
	const FCrowdyExecLayout* Layout = Variable ? Definition->ResolveLayout(Error) : nullptr;
	const FCrowdyExecField* Field = Layout ? CrowdyServerObjectDetail::FindStateField(*Layout, *Variable) : nullptr;
	bool bDecoded = false;
	bool bFieldChanged = false;
	if (Field && State.GetScriptStruct() == Definition->GetStateStruct())
	{
		const CrowdyServerObjectDetail::FHeldField Held(*Variable, *Field, State.GetMemory());
		bDecoded = CrowdyExec::DecodeKitVariable(*Definition, *Variable, Reply.Payload, State.GetMutableMemory(), Error);
		bFieldChanged = bDecoded && Field->bWatched && Held.Differs(State.GetMemory());
	}
	if (!bDecoded)
	{
		Fail(CrowdyExec::SafeText(Error.IsEmpty() ? FString::Printf(TEXT("%s's Owner Value %s is not a variable of its State"), *Definition->TypeName, *Owned.Variable.ToString()) : Error,
			CrowdyServerObjectDetail::MaxReasonChars), true);
		return;
	}
	OwnerReadsDone[Index] = true;
	OwnerReadsFailing[Index] = false;
	NoteGoodRead();
	TArray<FString> Changed;
	if (bFieldChanged)
	{
		Changed.Add(Field->Name);
	}
	UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: %s/%s owner value %s applied changed=%s"), *Definition->TypeName,
		CrowdyServerObjectDetail::TraceId(*Definition, InstanceId), *Owned.Method, *CrowdyServerObjectDetail::TraceNames(ToVariableNames(Changed)));
	const bool bBecameReady = Status == ECrowdyServerObjectStatus::Connecting && bHasRead && (!Definition->HasKitCollection() || bCollectionRead) && HasReadOwnerValues();
	if (bBecameReady)
	{
		SetStatus(ECrowdyServerObjectStatus::Ready);
		// The reads before this one held their announcement for it.
		Changed.Reset();
		CollectWatchedFields(nullptr, nullptr, Changed);
	}
	const uint32 ReadGeneration = Generation;
	if (Changed.Num() > 0 && Status != ECrowdyServerObjectStatus::Connecting)
	{
		NotifyValues(Changed);
	}
	if (bBecameReady && Definition->HasKitCollection() && Generation == ReadGeneration)
	{
		AnnounceHeldRecords();
	}
	if (bBecameReady && Generation == ReadGeneration)
	{
		NotifyStatus();
	}
	if (Generation != ReadGeneration)
	{
		return;
	}
	if (OwnerReadsAgain[Index])
	{
		RequestOwnerValue(Index);
	}
	if (Generation == ReadGeneration)
	{
		FlushPendingMessages();
	}
}

void UCrowdyServerObject::ReceiveKitChanged(const TArray<uint8>& Payload)
{
	// The read the State still waits for reads every Owner Value after it.
	if (!bHasRead || !ReadsOwnerValues())
	{
		return;
	}
	TArray<int64> Who;
	FString What;
	FString Error;
	const bool bRead = CrowdyExec::ReadKitChanged(Payload, Who, What, Error);
	if (bRead && !Who.Contains(GetSignedInUserId()))
	{
		return;
	}
	UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: %s/%s changed %s%s"), *Definition->TypeName, CrowdyServerObjectDetail::TraceId(*Definition, InstanceId),
		bRead ? *What : TEXT("unreadable, every owner value read again: "), bRead ? TEXT("") : *CrowdyExec::SafeText(Error, CrowdyServerObjectDetail::MaxReasonChars));
	const int32 Count = Definition->KitOwnerValues.Num();
	if (OwnerReadsWanted.Num() != Count)
	{
		OwnerReadsWanted.Init(false, Count);
	}
	for (int32 Index = 0; Index < Count; ++Index)
	{
		if (!bRead || Definition->KitOwnerValues[Index].What.Equals(What, ESearchCase::CaseSensitive))
		{
			OwnerReadsWanted[Index] = true;
		}
	}
}

void UCrowdyServerObject::SendWantedOwnerReads()
{
	if (!OwnerReadsWanted.Contains(true))
	{
		return;
	}
	const TBitArray<> Wanted = MoveTemp(OwnerReadsWanted);
	OwnerReadsWanted.Reset();
	const uint32 SendGeneration = Generation;
	for (TConstSetBitIterator<> It(Wanted); It && Generation == SendGeneration; ++It)
	{
		RequestOwnerValue(It.GetIndex());
	}
}

void UCrowdyServerObject::KeepOwnerValues(const void* From, void* To, const TBitArray<>* Carried) const
{
	FString Error;
	const FCrowdyExecLayout* Layout = Definition->ResolveLayout(Error);
	const TArray<FCrowdyExecField>* StateFields = Layout && Layout->StateStruct != INDEX_NONE ? &Layout->Structs[Layout->StateStruct].Fields : nullptr;
	for (const FCrowdyKitOwnerValue& Owned : Definition->KitOwnerValues)
	{
		const FProperty* Variable = Definition->FindVariable(Owned.Variable);
		if (!Variable)
		{
			continue;
		}
		const FCrowdyExecField* Field = StateFields ? StateFields->FindByPredicate([Layout, Variable](const FCrowdyExecField& Each) { return Layout->Types[Each.Type].Property == Variable; }) : nullptr;
		// A hub whose snapshot carries the variable too sends a value newer than the one kept.
		const int32 FieldIndex = Field ? static_cast<int32>(Field - StateFields->GetData()) : INDEX_NONE;
		if (Carried && Carried->IsValidIndex(FieldIndex) && (*Carried)[FieldIndex])
		{
			continue;
		}
		Variable->CopyCompleteValue_InContainer(To, From);
		if (Field && Field->Presence)
		{
			Field->Presence->SetPropertyValue_InContainer(To, Field->Presence->GetPropertyValue_InContainer(From));
		}
	}
}

void UCrowdyServerObject::ResetOwnerValues()
{
	OwnerReadsDone.Reset();
	const UScriptStruct* StateStruct = Definition->GetStateStruct();
	if (Definition->KitOwnerValues.IsEmpty() || !StateStruct || State.GetScriptStruct() != StateStruct)
	{
		return;
	}
	FInstancedStruct Fresh;
	Definition->InitializeValues(StateStruct, Fresh);
	KeepOwnerValues(Fresh.GetMemory(), State.GetMutableMemory());
}

void UCrowdyServerObject::HandleSignedInPlayerChanged()
{
	if (Definition->KitOwnerValues.IsEmpty() || (Status != ECrowdyServerObjectStatus::Connecting && Status != ECrowdyServerObjectStatus::Ready))
	{
		return;
	}
	++OwnerReadEra;
	OwnerReadsInFlight.Reset();
	OwnerReadsAgain.Reset();
	OwnerReadsFailing.Reset();
	OwnerReadsWanted.Reset();
	OwnerValuesUserId = GetSignedInUserId();
	const FInstancedStruct Before = State;
	ResetOwnerValues();
	TArray<FString> Changed;
	if (bHasRead && Before.GetScriptStruct() == State.GetScriptStruct())
	{
		CollectWatchedFields(Before.GetMemory(), State.GetMemory(), Changed);
	}
	if (Changed.Num() > 0 && Status == ECrowdyServerObjectStatus::Ready)
	{
		NotifyValues(Changed);
	}
}

void UCrowdyServerObject::CollectWatchedFields(const void* Before, const void* After, TArray<FString>& OutFields, bool bRecord) const
{
	FString Error;
	const FCrowdyExecLayout* Layout = Definition->ResolveLayout(Error);
	const int32 StructIndex = !Layout ? INDEX_NONE : bRecord ? Layout->RecordStruct : Layout->StateStruct;
	if (StructIndex == INDEX_NONE)
	{
		return;
	}
	for (const FCrowdyExecField& Field : Layout->Structs[StructIndex].Fields)
	{
		if (!Field.bWatched)
		{
			continue;
		}
		if (Before && !CrowdyServerObjectDetail::FieldDiffers(*Layout, Field, Before, After))
		{
			continue;
		}
		OutFields.Add(Field.Name);
	}
}

bool UCrowdyServerObject::CollectWatchedFieldBits(const void* Before, const void* After, TBitArray<>& OutFields) const
{
	FString Error;
	const FCrowdyExecLayout* Layout = Definition->ResolveLayout(Error);
	if (!Layout || Layout->RecordStruct == INDEX_NONE)
	{
		return false;
	}
	const TArray<FCrowdyExecField>& Fields = Layout->Structs[Layout->RecordStruct].Fields;
	OutFields.Init(false, Fields.Num());
	bool bAny = false;
	for (int32 Index = 0; Index < Fields.Num(); ++Index)
	{
		const bool bChanged = Fields[Index].bWatched && (!Before || CrowdyServerObjectDetail::FieldDiffers(*Layout, Fields[Index], Before, After));
		OutFields[Index] = bChanged;
		bAny |= bChanged;
	}
	return bAny;
}

void UCrowdyServerObject::GetFieldVariableNames(TArray<FString>& OutNames) const
{
	OutNames.Reset();
	FString Error;
	const FCrowdyExecLayout* Layout = Definition ? Definition->ResolveLayout(Error) : nullptr;
	if (!Layout || Layout->RecordStruct == INDEX_NONE)
	{
		return;
	}
	for (const FCrowdyExecField& Field : Layout->Structs[Layout->RecordStruct].Fields)
	{
		OutNames.Add(Field.bWatched ? CrowdyExec::KeepNameCharacters(Layout->Types[Field.Type].Property->GetAuthoredName()) : FString());
	}
}

void UCrowdyServerObject::CollectWrittenFields(const TBitArray<>& Written, TArray<FString>& OutFields) const
{
	FString Error;
	const FCrowdyExecLayout* Layout = Definition->ResolveLayout(Error);
	if (!Layout || Layout->RecordStruct == INDEX_NONE)
	{
		return;
	}
	const TArray<FCrowdyExecField>& Fields = Layout->Structs[Layout->RecordStruct].Fields;
	for (TConstSetBitIterator<> It(Written); It; ++It)
	{
		if (Fields.IsValidIndex(It.GetIndex()) && Fields[It.GetIndex()].bWatched)
		{
			OutFields.Add(Fields[It.GetIndex()].Name);
		}
	}
}

void UCrowdyServerObject::ResetRestrictedFields(FCrowdyExecRecordUpdate& Update, FInstancedStruct& Defaults) const
{
	FString Error;
	const FCrowdyExecLayout* Layout = Definition->ResolveLayout(Error);
	if (!Layout || Layout->StateStruct == INDEX_NONE || Update.Values.GetScriptStruct() != Definition->GetStateStruct())
	{
		return;
	}
	if (!Defaults.IsValid())
	{
		Definition->InitializeValues(Definition->GetStateStruct(), Defaults);
	}
	const TArray<FCrowdyExecField>& Fields = Layout->Structs[Layout->StateStruct].Fields;
	for (int32 Index = 0; Index < Fields.Num() && Index < Update.Fields.Num(); ++Index)
	{
		const FProperty* Property = Layout->Types[Fields[Index].Type].Property;
		if (!Fields[Index].bRestricted || Property->Identical_InContainer(Update.Values.GetMemory(), Defaults.GetMemory()))
		{
			continue;
		}
		Property->CopyCompleteValue_InContainer(Update.Values.GetMutableMemory(), Defaults.GetMemory());
		Update.Fields[Index] = true;
	}
}

void UCrowdyServerObject::ApplyRecordsPush(const TArray<uint8>& Payload)
{
	FCrowdyExecRecordsMessage Push;
	FString Error;
	auto FindStored = [this](const FString& Id) -> const void*
	{
		const FCrowdyServerRecord* Stored = Records.Find(Id);
		return Stored ? Stored->Values.GetMemory() : nullptr;
	};
	// Records of a State reshaped in the editor cannot take a push decoded over them: read instead.
	if (State.GetScriptStruct() != Definition->GetStateStruct()
		|| !CrowdyExec::DecodeRecords(*Definition, ECrowdyExecRecordsMessage::Push, Payload, FindStored, Push, Error))
	{
		RequestRead();
		return;
	}
	if (!AcceptsPush(FCrowdyExecStateHeader{Push.Epoch, Push.Seq}))
	{
		return;
	}
	AppliedSeq = Push.Seq;
	const int64 UserId = CrowdyServerObjectDetail::FindSignedInUserId(Subsystem);
	// Only an owner or team change of a record the player owned or owns can change the player's own team.
	bool bOwnTouched = false;
	TArray<FString> Changed;
	TArray<TBitArray<>> ChangedFields;
	TArray<FString> Removed;
	TArray<TPair<FString, TArray<FString>>> Watched;
	FInstancedStruct Defaults;
	bool bReset = false;
	for (FCrowdyExecRecordUpdate& Update : Push.Records)
	{
		// A new owner or team may no longer see the restricted values held; changes asked from before this push's restricted seq bring back what it may.
		if (Update.bHasOwner || Update.bHasTeam)
		{
			ResetRestrictedFields(Update, Defaults);
			bReset = true;
		}
		FCrowdyServerRecord& Record = Records.FindOrAdd(Update.Id);
		const bool bWasOwn = UserId != 0 && Record.Owner == UserId;
		Record.Values = MoveTemp(Update.Values);
		if (Update.bHasOwner)
		{
			Record.Owner = Update.Owner;
		}
		if (Update.bHasTeam)
		{
			Record.Team = MoveTemp(Update.Team);
		}
		bOwnTouched |= (Update.bHasOwner || Update.bHasTeam) && (bWasOwn || (UserId != 0 && Record.Owner == UserId));
		if (IsRecordWatched(Update.Id))
		{
			CollectWrittenFields(Update.Fields, Watched.Emplace_GetRef(Update.Id, TArray<FString>()).Value);
		}
		ChangedFields.Add(MoveTemp(Update.Fields));
		Changed.Add(MoveTemp(Update.Id));
	}
	for (FString& Id : Push.Removed)
	{
		const FCrowdyServerRecord* Gone = Records.Find(Id);
		if (!Gone)
		{
			continue;
		}
		bOwnTouched |= UserId != 0 && Gone->Owner == UserId;
		Records.Remove(Id);
		if (IsRecordWatched(Id))
		{
			CollectWatchedFields(nullptr, nullptr, Watched.Emplace_GetRef(Id, TArray<FString>()).Value, true);
		}
		Removed.Add(MoveTemp(Id));
	}
	if (Records.Num() > CrowdyExec::MaxElements)
	{
		Fail(FString::Printf(TEXT("The server sent more than the %d records an instance holds"), CrowdyExec::MaxElements), true);
		return;
	}
	// A changes reply that overtook this push already moved past the values just reset, so the next one asks from before it.
	if (bReset)
	{
		const uint64 Before = Push.RSeq > 0 ? Push.RSeq - 1 : 0;
		AppliedRSeq = FMath::Min(AppliedRSeq, Before);
		ChangesFirstRSeq = FMath::Min(ChangesFirstRSeq, Before);
	}
	// A Crowdy Team's membership is the platform's, so only a team taken from this object's records is followed here.
	const FString Team = bOwnTouched && !Definition->HasCrowdyTeamRecords() ? FindOwnTeam() : OwnTeam;
	const bool bTeamChanged = !Team.Equals(OwnTeam, ESearchCase::CaseSensitive);
	OwnTeam = Team;
	UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: %s/%s records push applied epoch=%llu seq=%llu changed=%d removed=%d restricted=%d"),
		*Definition->TypeName, CrowdyServerObjectDetail::TraceId(*Definition, InstanceId), AppliedEpoch, AppliedSeq, Changed.Num(), Removed.Num(), Push.bRestricted ? 1 : 0);

	const uint32 PushGeneration = Generation;
	NotifyRecords(Changed, ChangedFields, Removed, Watched);
	if (Generation != PushGeneration)
	{
		return;
	}
	// Teammates' unchanged Team Only values reach a new team member only through a read.
	if (bTeamChanged)
	{
		TraceReread(TEXT("team"));
		RequestRead();
		return;
	}
	if (Push.bRestricted)
	{
		RequestChanges();
	}
}

void UCrowdyServerObject::RequestChanges()
{
	bChangesWanted = true;
	SendDueChanges();
}

void UCrowdyServerObject::SendDueChanges()
{
	if (!bChangesWanted || bChangesInFlight || ChangesWait > 0.f || !bHasRead || bReadInFlight || !CrowdyServerObjectDetail::GetConnection(Subsystem))
	{
		return;
	}
	bChangesWanted = false;
	ChangesWait = CrowdyServerObjectDetail::ChangesIntervalSeconds;
	ChangesSince = AppliedRSeq;
	ChangesAfter.Reset();
	ChangesPages = 0;
	bChangesFirstPage = true;
	bChangesMissed = false;
	SendChangesPage();
}

void UCrowdyServerObject::HandleChangesFailure(const FCrowdyNativeExecReply& Reply)
{
	const FString Permanent = PermanentRefusal(Reply, TEXT("read"));
	if (!Permanent.IsEmpty())
	{
		Fail(Permanent, true);
		return;
	}
	++ChangesFailures;
	ChangesWait = CrowdyServerObjectDetail::RetryInSeconds(ChangesFailures, Reply, CrowdyServerObjectDetail::NextJitter(Subsystem));
	bChangesWanted = true;
}

void UCrowdyServerObject::SendChangesPage()
{
	FCrowdyNativeExecConnection* Connection = CrowdyServerObjectDetail::GetConnection(Subsystem);
	if (!Connection)
	{
		bChangesWanted = true;
		return;
	}
	bChangesInFlight = true;
	TWeakObjectPtr<UCrowdyServerObject> WeakThis(this);
	const uint32 ChangesGeneration = Generation;
	const uint32 Ticket = ChangesTicket;
	Connection->Call(Definition->TypeName, InstanceId, TEXT("changes"), CrowdyServerObjectDetail::PageRequest(ChangesAfter, &ChangesSince),
		[WeakThis, ChangesGeneration, Ticket](const FCrowdyNativeExecReply& Reply)
		{
			UCrowdyServerObject* Self = WeakThis.Get();
			if (Self && Self->Generation == ChangesGeneration)
			{
				Self->HandleChangesDone(Ticket, Reply);
			}
		});
}

void UCrowdyServerObject::HandleChangesDone(uint32 Ticket, const FCrowdyNativeExecReply& Reply)
{
	bChangesInFlight = false;
	// A read since this was sent already carries what it would bring.
	if (Ticket != ChangesTicket)
	{
		return;
	}
	if (!Reply.IsOk())
	{
		HandleChangesFailure(Reply);
		return;
	}
	ChangesFailures = 0;
	FCrowdyExecRecordsMessage Page;
	FString Error;
	auto FindStored = [this](const FString& Id) -> const void*
	{
		const FCrowdyServerRecord* Stored = Records.Find(Id);
		return Stored ? Stored->Values.GetMemory() : nullptr;
	};
	// Records of a State reshaped in the editor cannot take values decoded over them: read instead.
	if (State.GetScriptStruct() != Definition->GetStateStruct()
		|| !CrowdyExec::DecodeRecords(*Definition, ECrowdyExecRecordsMessage::Changes, Reply.Payload, FindStored, Page, Error))
	{
		TraceReread(TEXT("changes"));
		RequestRead();
		return;
	}
	if (bChangesFirstPage)
	{
		ChangesFirstRSeq = Page.RSeq;
		bChangesFirstPage = false;
	}
	TArray<FString> Changed;
	TArray<TBitArray<>> ChangedFields;
	TArray<TPair<FString, TArray<FString>>> Watched;
	for (FCrowdyExecRecordUpdate& Update : Page.Records)
	{
		ChangesAfter = Update.Id;
		// A record whose creating push has not arrived yet is asked for again; one removed since has left the server's changes.
		FCrowdyServerRecord* Record = Records.Find(Update.Id);
		if (!Record)
		{
			bChangesMissed = true;
			continue;
		}
		Record->Values = MoveTemp(Update.Values);
		if (IsRecordWatched(Update.Id))
		{
			CollectWrittenFields(Update.Fields, Watched.Emplace_GetRef(Update.Id, TArray<FString>()).Value);
		}
		ChangedFields.Add(MoveTemp(Update.Fields));
		Changed.Add(MoveTemp(Update.Id));
	}
	// Each page brings a record and an instance holds at most MaxElements, so paging stops after that many pages.
	const bool bMore = Page.bMore && !Page.Records.IsEmpty() && ++ChangesPages < CrowdyExec::MaxElements;
	// The first page's, so a change made while the pages were read is asked for again.
	if (!bMore && !bChangesMissed)
	{
		AppliedRSeq = ChangesFirstRSeq;
	}
	bChangesWanted |= !bMore && bChangesMissed;
	UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: %s/%s records changes applied since=%llu rseq=%llu changed=%d more=%d"),
		*Definition->TypeName, CrowdyServerObjectDetail::TraceId(*Definition, InstanceId), ChangesSince, Page.RSeq, Changed.Num(), bMore ? 1 : 0);
	if (bMore)
	{
		SendChangesPage();
	}
	NotifyRecords(Changed, ChangedFields, TArray<FString>(), Watched);
}

FString UCrowdyServerObject::FindOwnTeam() const
{
	const int64 UserId = CrowdyServerObjectDetail::FindSignedInUserId(Subsystem);
	const TPair<FString, FCrowdyServerRecord>* First = nullptr;
	for (const TPair<FString, FCrowdyServerRecord>& Pair : Records)
	{
		const bool bEarlier = !First || FCString::Strcmp(*Pair.Key, *First->Key) < 0;
		if (UserId != 0 && Pair.Value.Owner == UserId && bEarlier)
		{
			First = &Pair;
		}
	}
	return First ? First->Value.Team : FString();
}

void UCrowdyServerObject::NotifyRecords(const TArray<FString>& Changed, const TArray<TBitArray<>>& ChangedFields, const TArray<FString>& Removed,
	const TArray<TPair<FString, TArray<FString>>>& Watched)
{
	if (Changed.IsEmpty() && Removed.IsEmpty())
	{
		return;
	}
	const uint32 NotifyGeneration = Generation;
	TGuardValue<uint32> Notifying(ActiveNotify, ++NotifyCount);
	OnRecordsChanged.Broadcast(this, Changed, Removed, ChangedFields);
	if (Generation != NotifyGeneration)
	{
		return;
	}
	// A Blueprint broadcast copies both lists, so it waits for a listener.
	if (RecordsChanged.IsBound())
	{
		RecordsChanged.Broadcast(this, Changed, Removed);
	}
	if (Generation != NotifyGeneration)
	{
		return;
	}
	BroadcastRecordsUpdated(Changed, ChangedFields, Removed);
	if (Generation != NotifyGeneration)
	{
		return;
	}
	TGuardValue<uint64> Announcing(AnnouncedSerial, FCrowdyServerVariableBindings::LatestSerial());
	for (const TPair<FString, TArray<FString>>& Each : Watched)
	{
		if (Generation != NotifyGeneration)
		{
			return;
		}
		const TSharedRef<FOnCrowdyServerValuesChanged>* Watchers = RecordWatchers.Find(Each.Key);
		if (!Watchers || Each.Value.IsEmpty())
		{
			continue;
		}
		// Held here, so a watcher that stops watching does not destroy the delegate it runs in.
		const TSharedRef<FOnCrowdyServerValuesChanged> Running = *Watchers;
		Running->Broadcast(this, Each.Value);
	}
}

void UCrowdyServerObject::BroadcastRecordsUpdated(const TArray<FString>& Changed, const TArray<TBitArray<>>& ChangedFields, const TArray<FString>& Removed)
{
	if (!RecordsUpdated.IsBound())
	{
		return;
	}
	TArray<FString> VariableNames;
	GetFieldVariableNames(VariableNames);
	TArray<FCrowdyServerRecordChange> Changes;
	Changes.Reserve(Changed.Num());
	for (int32 Index = 0; Index < Changed.Num(); ++Index)
	{
		FCrowdyServerRecordChange& Change = Changes.AddDefaulted_GetRef();
		Change.RecordId = Changed[Index];
		if (!ChangedFields.IsValidIndex(Index))
		{
			continue;
		}
		for (TConstSetBitIterator<> It(ChangedFields[Index]); It; ++It)
		{
			const FString* Name = VariableNames.IsValidIndex(It.GetIndex()) ? &VariableNames[It.GetIndex()] : nullptr;
			if (Name && !Name->IsEmpty())
			{
				Change.ChangedVariables.Add(*Name);
			}
		}
	}
	RecordsUpdated.Broadcast(this, Changes, Removed);
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyServerObject::ApplyRecordsForTest(TConstArrayView<TPair<FString, FInstancedStruct>> Changed, TConstArrayView<FString> Removed)
{
	TArray<FString> ChangedIds;
	TArray<TBitArray<>> ChangedFields;
	TArray<FString> RemovedIds;
	TArray<TPair<FString, TArray<FString>>> Watched;
	for (const TPair<FString, FInstancedStruct>& Each : Changed)
	{
		FCrowdyServerRecord& Record = Records.FindOrAdd(Each.Key);
		TBitArray<>& Fields = ChangedFields.AddDefaulted_GetRef();
		CollectWatchedFieldBits(Record.Values.IsValid() ? Record.Values.GetMemory() : nullptr, Each.Value.GetMemory(), Fields);
		Record.Values = Each.Value;
		if (IsRecordWatched(Each.Key))
		{
			CollectWrittenFields(Fields, Watched.Emplace_GetRef(Each.Key, TArray<FString>()).Value);
		}
		ChangedIds.Add(Each.Key);
	}
	for (const FString& Id : Removed)
	{
		if (Records.Remove(Id) == 0)
		{
			continue;
		}
		if (IsRecordWatched(Id))
		{
			CollectWatchedFields(nullptr, nullptr, Watched.Emplace_GetRef(Id, TArray<FString>()).Value, true);
		}
		RemovedIds.Add(Id);
	}
	bHasRead = true;
	SetStatus(ECrowdyServerObjectStatus::Ready);
	NotifyRecords(ChangedIds, ChangedFields, RemovedIds, Watched);
}

FCrowdyServerCallResult UCrowdyServerObject::MakeCallResultForTest(FName Function, const FCrowdyNativeExecReply& Reply, const FString& Code) const
{
	FPendingCall Pending;
	Pending.Function = Function;
	return MakeCallResult(Pending, Reply, Code);
}

FString UCrowdyServerObject::WaitingReasonForTest(const FCrowdyNativeExecReply& Reply, bool bRead) const
{
	return CrowdyServerObjectDetail::WaitingReason(*Definition, Reply, bRead);
}
#endif

void UCrowdyServerObject::SendDueCalls()
{
	if (!CrowdyServerObjectDetail::GetConnection(Subsystem))
	{
		return;
	}
	TArray<uint32, TInlineAllocator<8>> Due;
	for (const TPair<uint32, FPendingCall>& Pair : PendingCalls)
	{
		if (!Pair.Value.bInFlight && !Pair.Value.bConnectionClosed && Pair.Value.RetryIn <= 0.f)
		{
			Due.Add(Pair.Key);
		}
	}
	const uint32 SendGeneration = Generation;
	for (const uint32 CallId : Due)
	{
		if (Generation != SendGeneration)
		{
			return;
		}
		SendCall(CallId);
	}
}

void UCrowdyServerObject::SendCall(uint32 CallId)
{
	FPendingCall* Pending = PendingCalls.Find(CallId);
	const CrowdyServerObjectDetail::FRoute Route = CrowdyServerObjectDetail::GetRoute(Subsystem, *Definition);
	if (!Pending || !Route || Pending->bInFlight || Pending->bConnectionClosed || Pending->RetryIn > 0.f)
	{
		return;
	}
	Pending->bInFlight = true;
	++Pending->Attempts;
	// A retry of a Busy call is the same call, so only its first send prints.
	UE_CLOG(CrowdyExecTrace::Exec() && Pending->Attempts == 1, LogCrowdyExec, Log, TEXT("exec: %s/%s call %s sent"), *Definition->TypeName,
		CrowdyServerObjectDetail::TraceId(*Definition, InstanceId), *Pending->Function.ToString());
	TWeakObjectPtr<UCrowdyServerObject> WeakThis(this);
	const uint32 CallGeneration = Generation;
	// The reply can arrive before Call returns, removing the entry, so nothing below may touch Pending.
	Route.Call(Definition->TypeName, InstanceId, Pending->Method, Pending->Payload, Pending->bSendOnce,
		[WeakThis, CallGeneration, CallId](const FCrowdyNativeExecReply& Reply, const FString& Code)
		{
			UCrowdyServerObject* Self = WeakThis.Get();
			if (Self && Self->Generation == CallGeneration)
			{
				Self->HandleCallDone(CallId, Reply, Code);
			}
		});
}

void UCrowdyServerObject::HandleCallDone(uint32 CallId, const FCrowdyNativeExecReply& Reply, const FString& Code)
{
	FPendingCall* Pending = PendingCalls.Find(CallId);
	if (!Pending)
	{
		HandleLateReply(CallId, Reply);
		return;
	}
	Pending->bInFlight = false;
	// Closed elsewhere, perhaps mid-teardown: reported on the next Tick so no caller code runs inside the close.
	if (CrowdyServerObjectDetail::IsConnectionClosed(Reply))
	{
		Pending->bConnectionClosed = true;
		return;
	}
	// The Game Kit already sent a Busy call again before answering.
	const bool bRetried = !Definition->IsGameKitStarter() && CrowdyServerObjectDetail::IsCallRetried(Reply.Status);
	if (bRetried && Pending->Attempts < CrowdyServerObjectDetail::MaxCallAttempts)
	{
		const float Backoff = FMath::Min(0.25f * static_cast<float>(1 << FMath::Min(Pending->Attempts - 1, 3)), 2.f);
		Pending->RetryIn = CrowdyServerObjectDetail::Jittered(FMath::Max(Backoff, CrowdyServerObjectDetail::RetryHintSeconds(Reply)), CrowdyServerObjectDetail::NextJitter(Subsystem));
		return;
	}
	const FCrowdyServerCallResult Result = MakeCallResult(*Pending, Reply, Code);
	ReadAfterAnswer(Result.IsSuccess() && ReadsMembershipAfter(*Pending), Reply);
	CompleteCall(CallId, Result);
}

bool UCrowdyServerObject::ReadsMembershipAfter(const FPendingCall& Pending) const
{
	const bool bJoinOrLeave = CrowdyServerObjectDetail::IsBuiltInMemberCall(*Definition, Pending.Function, Pending.Method, TEXT("join"))
		|| CrowdyServerObjectDetail::IsBuiltInMemberCall(*Definition, Pending.Function, Pending.Method, TEXT("leave"));
	// A pushed list brings the new membership itself; otherwise a read asks the server for Is Member and Is Leader.
	return bJoinOrLeave && !CrowdyServerObjectDetail::IsMembershipPushed(*Definition);
}

void UCrowdyServerObject::ReadAfterAnswer(bool bReadMembership, const FCrowdyNativeExecReply& Reply)
{
	// A crashed instance restarts from its last snapshot, so the watched values may have rolled back.
	if ((bReadMembership || Reply.Status == ECrowdyNativeExecStatus::Trapped) && SubscribeHandle != 0)
	{
		RequestRead();
	}
}

void UCrowdyServerObject::HandleLateReply(uint32 CallId, const FCrowdyNativeExecReply& Reply)
{
	bool bMembership = false;
	if (!LateCalls.RemoveAndCopyValue(CallId, bMembership) || CrowdyServerObjectDetail::IsConnectionClosed(Reply))
	{
		return;
	}
	ReadAfterAnswer(bMembership && Reply.IsOk(), Reply);
}

void UCrowdyServerObject::CompleteCall(uint32 CallId, const FCrowdyServerCallResult& Result)
{
	FPendingCall* Pending = PendingCalls.Find(CallId);
	if (!Pending)
	{
		return;
	}
	TraceAnswered(*Pending, Result.Outcome);
	TFunction<void(const FCrowdyServerCallResult&)> OnDone = MoveTemp(Pending->OnDone);
	PendingCalls.Remove(CallId);
	OnDone(Result);
}

void UCrowdyServerObject::TraceAnswered(const FPendingCall& Pending, ECrowdyServerCallOutcome Outcome) const
{
	UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: %s/%s call %s answered %s ms=%d"), *Definition->TypeName,
		CrowdyServerObjectDetail::TraceId(*Definition, InstanceId),
		*Pending.Function.ToString(), *CrowdyServerObjectDetail::TraceEnum(Outcome), static_cast<int32>(FMath::RoundToInt((FPlatformTime::Seconds() - Pending.MadeAt) * 1000.0)));
}

FCrowdyServerCallResult UCrowdyServerObject::MakeCallResult(const FPendingCall& Pending, const FCrowdyNativeExecReply& Reply, const FString& Code) const
{
	FCrowdyServerCallResult Result;
	Result.Outcome = ECrowdyServerCallOutcome::Success;
	if (Reply.IsOk() && !Pending.ReplyStruct)
	{
		return Result;
	}
	if (Reply.IsOk())
	{
		Result.Reply.InitializeAs(Pending.ReplyStruct);
		// What the reply leaves out takes the called function's own values, not those of another List of the same shape.
		const FCrowdyServerFunction* Called = Definition->FindFunction(Pending.Function);
		FInstancedStruct Start;
		if (Called && Called->GetReplyStruct() == Pending.ReplyStruct)
		{
			Called->InitializeReply(Start);
		}
		FString Error;
		if (CrowdyExec::Decode(*Definition, *Pending.ReplyStruct, Reply.Payload, Result.Reply.GetMutableMemory(), Error, Start.GetMemory()))
		{
			return Result;
		}
		Result.Reply.Reset();
		Result.Outcome = ECrowdyServerCallOutcome::ServerError;
		Result.Reason = FString::Printf(TEXT("The reply did not match %s: %s"), *Pending.ReplyName,
			*CrowdyExec::SafeText(Error, CrowdyServerObjectDetail::MaxReasonChars));
		return Result;
	}

	const FString Function = Pending.Function.ToString();
	Result.Outcome = ECrowdyServerCallOutcome::ServerError;
	Result.Reason = CrowdyServerObjectDetail::PlainReason(Reply);
	Result.bRetryable = Reply.bRetryable;
	switch (Reply.Status)
	{
	case ECrowdyNativeExecStatus::AppError:
		if (!Code.IsEmpty())
		{
			CrowdyServerObjectDetail::ApplyKitRefusal(Result, Reply, Code, Function);
			break;
		}
		CrowdyServerObjectDetail::ApplyAppRefusal(Result, Function);
		break;
	case ECrowdyNativeExecStatus::Busy:
	case ECrowdyNativeExecStatus::RateLimited:
		Result.Outcome = ECrowdyServerCallOutcome::Busy;
		Result.Reason = TEXT("The server is busy; try again shortly");
		break;
	case ECrowdyNativeExecStatus::Unavailable:
	case ECrowdyNativeExecStatus::Moved:
		Result.Outcome = ECrowdyServerCallOutcome::Unavailable;
		Result.Reason = CrowdyServerObjectDetail::IsConnectionClosed(Reply) ? FString(TEXT("The server could not be reached"))
			: CrowdyServerObjectDetail::WithServerMessage(TEXT("The server could not be reached"), Reply);
		Result.bRetryable = true;
		break;
	case ECrowdyNativeExecStatus::NotFound:
		Result.Outcome = ECrowdyServerCallOutcome::NotDeployed;
		Result.Reason = CrowdyServerObjectDetail::NotDeployedReason(*Definition);
		break;
	case ECrowdyNativeExecStatus::DeadlineExceeded:
		if (const FString Refusal = Reply.bNoReply && !Definition->IsGameKitStarter() ? CrowdyServerObjectDetail::RedialRefusal(Subsystem) : FString(); !Refusal.IsEmpty())
		{
			FCrowdyNativeExecReply Refused = Reply;
			Refused.Message = Refusal;
			CrowdyServerObjectDetail::ApplyPaused(Result, Refused, Function);
			break;
		}
		Result.Outcome = ECrowdyServerCallOutcome::Timeout;
		Result.Reason = Reply.bNoReply ? FString(CrowdyServerObjectDetail::NoAnswerReason)
			: CrowdyServerObjectDetail::WithServerMessage(FString::Printf(TEXT("%s ran past its Time per Call on the server"), *Function), Reply);
		break;
	case ECrowdyNativeExecStatus::Denied:
		if (CrowdyExec::IsAccessEndedText(Reply.Message))
		{
			Result.Outcome = ECrowdyServerCallOutcome::AccessEnded;
			Result.bRetryable = false;
			Result.Reason = CrowdyExec::AccessEndedReason(Reply.Message);
			break;
		}
		if (CrowdyServerObjectDetail::IsSwitchedOffText(Reply.Message))
		{
			CrowdyServerObjectDetail::ApplyPaused(Result, Reply, Function);
			break;
		}
		Result.Outcome = ECrowdyServerCallOutcome::Denied;
		Result.Reason = CrowdyServerObjectDetail::WithServerMessage(FString::Printf(TEXT("The server refused the call to %s"), *Function), Reply);
		break;
	case ECrowdyNativeExecStatus::AppPaused:
	case ECrowdyNativeExecStatus::AppDisabled:
	case ECrowdyNativeExecStatus::TypeDisabled:
		CrowdyServerObjectDetail::ApplyPaused(Result, Reply, Function);
		break;
	case ECrowdyNativeExecStatus::Internal:
		Result.Reason = FString::Printf(TEXT("The server failed handling %s"), *Function);
		break;
	case ECrowdyNativeExecStatus::Trapped:
		Result.Outcome = ECrowdyServerCallOutcome::ServerCrashed;
		Result.Reason = FString::Printf(TEXT("The server code crashed handling %s; check its log"), *Function);
		break;
	case ECrowdyNativeExecStatus::BadRequest:
		Result.Outcome = ECrowdyServerCallOutcome::BadRequest;
		break;
	default:
		break;
	}
	return Result;
}
