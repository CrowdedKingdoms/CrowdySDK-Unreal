#include "CrowdyServerObject.h"

#include "CrowdyExecCodec.h"
#include "CrowdyExecInternal.h"
#include "CrowdyExecLog.h"
#include "CrowdyNativeExec.h"
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

	const TCHAR* const ReleasedReason = TEXT("This Server Object was given back");

	FCrowdyNativeExecConnection* GetConnection(const TWeakObjectPtr<UCrowdyServerObjectSubsystem>& Subsystem)
	{
		const UCrowdyServerObjectSubsystem* Owner = Subsystem.Get();
		return Owner ? Owner->GetConnection() : nullptr;
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

	float RetryHintSeconds(const FCrowdyNativeExecReply& Reply)
	{
		return Reply.RetryAfterMs >= 0 ? FMath::Min(Reply.RetryAfterMs / 1000.f, MaxRetryHintSeconds) : 0.f;
	}

	FString NotDeployedReason(const UCrowdyServerObjectDefinition& Definition)
	{
		return FString::Printf(TEXT("%s is not deployed on the server"), *Definition.TypeName);
	}

	/** Why a retried read or subscribe refusal was given, or empty when saying so would not help. */
	FString WaitingReason(const UCrowdyServerObjectDefinition& Definition, const FCrowdyNativeExecReply& Reply, const TCHAR* Verb)
	{
		switch (Reply.Status)
		{
		case ECrowdyNativeExecStatus::NotFound:
			return NotDeployedReason(Definition);
		case ECrowdyNativeExecStatus::Denied:
			return FString::Printf(TEXT("This player may not %s this Server Object"), Verb);
		case ECrowdyNativeExecStatus::Trapped:
			return TEXT("The server code crashed; check its log");
		case ECrowdyNativeExecStatus::DeadlineExceeded:
			return TEXT("The server did not answer in time");
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

	bool IsConnectionClosed(const FCrowdyNativeExecReply& Reply)
	{
		return Reply.Status == ECrowdyNativeExecStatus::Unavailable
			&& Reply.Message.Equals(FCrowdyNativeExecConnection::CanceledMessage(), ESearchCase::CaseSensitive);
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

	/** Whether a call was the built-in member function with BuiltInMethod, not a function of the definition's own. */
	bool IsBuiltInMemberCall(const UCrowdyServerObjectDefinition& Definition, FName Function, const FString& Method, const TCHAR* BuiltInMethod)
	{
		return Definition.MembersFrom == ECrowdyServerMembersSource::ThisObject && UCrowdyServerObjectDefinition::IsMemberFunction(Function)
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

	/** The instance a trace line names: an Owner Only instance id is the player's user id, so it prints as "owner". */
	const TCHAR* TraceId(const UCrowdyServerObjectDefinition& Definition, const FString& InstanceId)
	{
		return Definition.Visibility == ECrowdyServerObjectVisibility::OwnerOnly ? TEXT("owner") : *InstanceId;
	}

	int64 FindSignedInUserId(const TWeakObjectPtr<UCrowdyServerObjectSubsystem>& Subsystem)
	{
		const UCrowdyServerObjectSubsystem* Owner = Subsystem.Get();
		return Owner ? Owner->GetSignedInUserId() : 0;
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
}

FInstancedPropertyBag UCrowdyServerObject::GetStateList() const
{
	return CrowdyExec::ToList(State);
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

TArray<FString> UCrowdyServerObject::ToVariableNames(TConstArrayView<FString> Fields) const
{
	TArray<FString> Names(Fields);
	FString Error;
	const FCrowdyExecLayout* Layout = Definition ? Definition->ResolveLayout(Error) : nullptr;
	if (!Layout || Layout->StateStruct == INDEX_NONE)
	{
		return Names;
	}
	const TArray<FCrowdyExecField>& StateFields = Layout->Structs[Layout->StateStruct].Fields;
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
	if (Object && Object->GetStatus() == ECrowdyServerObjectStatus::Ready)
	{
		Run(*Object, FBinding(Added));
	}
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

void FCrowdyServerVariableBindings::Call(const UCrowdyServerObject& Object, const FBinding& Binding)
{
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
	const FProperty* Field = UCrowdyServerObjectLibrary::FindServerValue(Object.GetState(), Binding.Variable);
	const FOptionalProperty* Optional = CastField<FOptionalProperty>(Field);
	const bool bEmpty = Optional && !Optional->IsSet(Optional->ContainerPtrToValuePtr<void>(Object.GetState().GetMemory()));

	// Only the parameters: a Blueprint function's locals lie past ParmsSize, and ProcessEvent sets them up itself.
	uint8* Params = static_cast<uint8*>(FMemory_Alloca_Aligned(Function->ParmsSize, Function->GetMinAlignment()));
	CrowdyServerObjectDetail::InitializeParams(*Function, Params);
	const bool bCopied = UCrowdyServerObjectLibrary::GetServerValueInto(Object.GetState(), Binding.Variable, ValueParam, ValueParam->ContainerPtrToValuePtr<void>(Params));
	if (!bCopied && !bEmpty)
	{
		UE_LOG(LogCrowdyExec, Warning, TEXT("%s.%s cannot take %s's value; check the variable and its type"), *GetNameSafe(Handler), *Binding.Function.ToString(), *Binding.Variable.ToString());
		CrowdyServerObjectDetail::DestroyParams(*Function, Params);
		return;
	}
	if (HasValueParam)
	{
		HasValueParam->SetPropertyValue_InContainer(Params, bCopied);
	}
	Handler->ProcessEvent(Function, Params);
	CrowdyServerObjectDetail::DestroyParams(*Function, Params);
}

void UCrowdyServerObject::Call(FName Function, const FInstancedStruct& Params, TFunction<void(const FCrowdyServerCallResult&)> OnDone)
{
	CallWith(Function, Params.GetScriptStruct(), Params.GetMemory(), MoveTemp(OnDone));
}

void UCrowdyServerObject::Call(FName Function, const FInstancedPropertyBag& Params, TFunction<void(const FCrowdyServerCallResult&)> OnDone)
{
	CallWith(Function, Params.GetPropertyBagStruct(), Params.GetValue().GetMemory(), MoveTemp(OnDone));
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

void UCrowdyServerObject::CallWith(FName Function, const UScriptStruct* GivenStruct, const void* Params, TFunction<void(const FCrowdyServerCallResult&)> OnDone)
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
		Refuse(ECrowdyServerCallOutcome::Unavailable, FailureReason);
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
	Pending.MadeAt = FPlatformTime::Seconds();
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
	if (Status != ECrowdyServerObjectStatus::Connecting || bReadInFlight || bRefreshScheduled)
	{
		return;
	}
	const uint32 ConnectGeneration = Generation;
	TArray<FString> Watched;
	CollectWatchedFields(nullptr, nullptr, Watched);
	// The members travel in the read, so an object with members reads even when it watches no variables.
	if (Watched.Num() > 0 || Definition->MembersFrom != ECrowdyServerMembersSource::None)
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
	FCrowdyNativeExecConnection* Connection = CrowdyServerObjectDetail::GetConnection(Subsystem);
	// Nothing to refresh before the subscription exists; a Failed or Released object never holds one.
	if (SubscribeHandle == 0 || !Connection)
	{
		return;
	}
	// A renewed subscription is never confirmed, so subscribe afresh for a real answer, then read what was missed.
	Connection->Unsubscribe(SubscribeHandle);
	SubscribeHandle = 0;
	bSubscribeConfirmed = false;
	TraceReread(TEXT("reconnect"));
	Refresh();
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
	StopListening();
	const uint32 FailGeneration = Generation;

	TMap<uint32, FPendingCall> Calls = MoveTemp(PendingCalls);
	PendingCalls.Reset();
	const FCrowdyServerCallResult Canceled = CrowdyServerObjectDetail::MakeResult(ECrowdyServerCallOutcome::Canceled, FailureReason);
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
}

void UCrowdyServerObject::StopListening()
{
	++Generation;
	FCrowdyNativeExecConnection* Connection = CrowdyServerObjectDetail::GetConnection(Subsystem);
	if (Connection && SubscribeHandle != 0)
	{
		Connection->Unsubscribe(SubscribeHandle);
	}
	SubscribeHandle = 0;
	bSubscribeConfirmed = false;
	BufferedPushes.Reset();
	bReadInFlight = false;
	bReadAgain = false;
	bRefreshScheduled = false;
	RefreshRetryIn = 0.f;
	RefreshFailures = 0;
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
	if (Status == ECrowdyServerObjectStatus::Connecting && SubscribeHandle == 0 && !bReadInFlight && !bRefreshScheduled)
	{
		HandleConnected();
	}
}

void UCrowdyServerObject::TickCalls(float DeltaSeconds)
{
	if (PendingCalls.IsEmpty())
	{
		return;
	}
	const bool bConnected = CrowdyServerObjectDetail::GetConnection(Subsystem) != nullptr;
	TArray<uint32, TInlineAllocator<8>> Closed;
	TArray<uint32, TInlineAllocator<8>> Unsent;
	for (TPair<uint32, FPendingCall>& Pair : PendingCalls)
	{
		FPendingCall& Pending = Pair.Value;
		Pending.RetryIn -= Pending.bInFlight ? 0.f : DeltaSeconds;
		Pending.UnsentSeconds += (bConnected || Pending.bInFlight) ? 0.f : DeltaSeconds;
		if (Pending.bConnectionClosed)
		{
			Closed.Add(Pair.Key);
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
	FCrowdyNativeExecConnection* Connection = CrowdyServerObjectDetail::GetConnection(Subsystem);
	if (!Connection)
	{
		bRefreshScheduled = true;
		RefreshRetryIn = 0.f;
		return;
	}
	if (SubscribeHandle == 0)
	{
		TWeakObjectPtr<UCrowdyServerObject> WeakThis(this);
		const uint32 SubscribeGeneration = Generation;
		const uint32 Attempt = ++SubscribeAttempt;
		bSubscribeConfirmed = false;
		const uint64 Handle = Connection->Subscribe(Definition->TypeName, InstanceId, TEXT("state"),
			[WeakThis, SubscribeGeneration](const FCrowdyNativeExecPush& Push)
			{
				UCrowdyServerObject* Self = WeakThis.Get();
				if (Self && Self->Generation == SubscribeGeneration)
				{
					Self->ReceivePush(Push.Payload);
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
			Connection->Unsubscribe(Handle);
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
	FCrowdyNativeExecConnection* Connection = CrowdyServerObjectDetail::GetConnection(Subsystem);
	if (!Connection)
	{
		bRefreshScheduled = true;
		RefreshRetryIn = 0.f;
		return;
	}
	bReadInFlight = true;
	bReadAgain = false;
	TWeakObjectPtr<UCrowdyServerObject> WeakThis(this);
	const uint32 ReadGeneration = Generation;
	Connection->Call(Definition->TypeName, InstanceId, TEXT("read"), TArray<uint8>{ 0x80 },
		[WeakThis, ReadGeneration](const FCrowdyNativeExecReply& Reply)
		{
			UCrowdyServerObject* Self = WeakThis.Get();
			if (Self && Self->Generation == ReadGeneration)
			{
				Self->HandleReadDone(Reply);
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
	UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: %s/%s subscribe %s%s"), *Definition->TypeName, ShownId,
		Reply.IsOk() ? TEXT("ok") : TEXT("failed "), Reply.IsOk() ? TEXT("") : *CrowdyServerObjectDetail::PlainReason(Reply).Replace(*InstanceId, ShownId, ESearchCase::CaseSensitive));
	if (Reply.IsOk())
	{
		bSubscribeConfirmed = true;
		return;
	}
	SubscribeHandle = 0;
	bSubscribeConfirmed = false;
	HandleRefreshFailure(Reply, false);
}

void UCrowdyServerObject::HandleReadDone(const FCrowdyNativeExecReply& Reply)
{
	bReadInFlight = false;
	if (!Reply.IsOk())
	{
		HandleRefreshFailure(Reply, true);
		return;
	}
	FInstancedStruct Read;
	Definition->InitializeValues(Definition->GetStateStruct(), Read);
	FCrowdyExecStateHeader Header;
	FCrowdyExecMembersView View;
	TArray<FString> Carried;
	FString Error;
	if (!CrowdyExec::DecodeState(*Definition, ECrowdyExecStateMessage::Read, Reply.Payload, Read.GetMutableMemory(), Header, Carried, Error, &View))
	{
		Fail(CrowdyExec::SafeText(Error, CrowdyServerObjectDetail::MaxReasonChars), true);
		return;
	}

	// A State edited since the last read has another shape, so every watched value counts as changed.
	const bool bSameShape = State.GetScriptStruct() == Read.GetScriptStruct();
	TArray<FString> Changed;
	CollectWatchedFields(bHasRead && bSameShape ? State.GetMemory() : nullptr, Read.GetMemory(), Changed);
	const CrowdyServerObjectDetail::FMemberFields MemberFields{Members, MemberCount, Leader, bOpenForJoining, bIsMember, bIsLeader};
	View = CrowdyServerObjectDetail::KeepDefinedMemberKeys(*Definition, MoveTemp(View));
	CrowdyServerObjectDetail::ApplyMemberView(MemberFields, View, true, !bHasRead, 0, Changed);
	const bool bBecameReady = Status == ECrowdyServerObjectStatus::Connecting;
	State = MoveTemp(Read);
	AppliedEpoch = Header.Epoch;
	AppliedSeq = Header.Seq;
	bHasRead = true;
	// Until a subscribe is accepted, a working read must not undo its backoff or its reason, only a read's.
	if (bSubscribeConfirmed)
	{
		RefreshFailures = 0;
	}
	if (bSubscribeConfirmed || bReadSetReason)
	{
		FailureReason.Reset();
		bReadSetReason = false;
	}
	UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: %s/%s read applied epoch=%llu seq=%llu changed=%s"), *Definition->TypeName,
		CrowdyServerObjectDetail::TraceId(*Definition, InstanceId),
		AppliedEpoch, AppliedSeq, *CrowdyServerObjectDetail::TraceNames(ToVariableNames(Changed)));
	if (bBecameReady)
	{
		SetStatus(ECrowdyServerObjectStatus::Ready);
	}

	// Values before status, so a listener bound from the Ready handler hears only its own catch-up call.
	const uint32 ReadGeneration = Generation;
	if (Changed.Num() > 0)
	{
		NotifyValues(Changed);
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
}

void UCrowdyServerObject::HandleRefreshFailure(const FCrowdyNativeExecReply& Reply, bool bRead)
{
	const TCHAR* const Verb = bRead ? TEXT("read") : TEXT("watch");
	const bool bAppError = Reply.Status == ECrowdyNativeExecStatus::AppError;
	if (bAppError && Reply.Message.StartsWith(TEXT("denied"), ESearchCase::CaseSensitive))
	{
		Fail(FString::Printf(TEXT("This player may not %s this Server Object"), Verb), true);
		return;
	}
	if (bAppError && Reply.Message.StartsWith(TEXT("unknown_method"), ESearchCase::CaseSensitive))
	{
		Fail(FString::Printf(TEXT("The server code for %s has no watched values; regenerate and deploy it"), *Definition->TypeName), true);
		return;
	}
	if (Reply.Status == ECrowdyNativeExecStatus::BadRequest)
	{
		Fail(CrowdyServerObjectDetail::PlainReason(Reply), true);
		return;
	}
	// A refused subscribe's reason outlasts a read's, since a good read clears only the read's own.
	const FString Waiting = CrowdyServerObjectDetail::WaitingReason(*Definition, Reply, Verb);
	const bool bKeepWatchReason = bRead && !bSubscribeConfirmed && !bReadSetReason && !FailureReason.IsEmpty();
	if (!Waiting.IsEmpty() && !bKeepWatchReason)
	{
		FailureReason = Waiting;
		bReadSetReason = bRead;
	}
	ScheduleRefresh(Reply);
}

void UCrowdyServerObject::ScheduleRefresh(const FCrowdyNativeExecReply& Reply)
{
	++RefreshFailures;
	const float Backoff = FMath::Min(static_cast<float>(1 << FMath::Min(RefreshFailures - 1, 4)), 10.f);
	RefreshRetryIn = FMath::Max(Backoff, CrowdyServerObjectDetail::RetryHintSeconds(Reply));
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
	if (BufferedPushes.Num() >= CrowdyServerObjectDetail::MaxBufferedPushes)
	{
		BufferedPushes.Reset();
		bReadAgain = true;
		TraceReread(TEXT("overflow"));
		return;
	}
	BufferedPushes.Add(Payload);
}

void UCrowdyServerObject::ApplyPush(const TArray<uint8>& Payload)
{
	FCrowdyExecStateHeader Header;
	FString Error;
	const bool bHeaderRead = CrowdyExec::ReadStateHeader(Payload, Header, Error);
	if (!bHeaderRead || Header.Epoch != AppliedEpoch)
	{
		if (bHeaderRead)
		{
			TraceReread(TEXT("epoch"));
		}
		RequestRead();
		return;
	}
	if (Header.Seq <= AppliedSeq)
	{
		UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("exec: %s/%s push stale epoch=%llu seq=%llu"), *Definition->TypeName,
			CrowdyServerObjectDetail::TraceId(*Definition, InstanceId),
			AppliedEpoch, AppliedSeq);
		return;
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

void UCrowdyServerObject::ReplayBufferedPushes()
{
	TArray<TArray<uint8>> Pending = MoveTemp(BufferedPushes);
	BufferedPushes.Reset();
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

void UCrowdyServerObject::CollectWatchedFields(const void* Before, const void* After, TArray<FString>& OutFields) const
{
	FString Error;
	const FCrowdyExecLayout* Layout = Definition->ResolveLayout(Error);
	if (!Layout || Layout->StateStruct == INDEX_NONE)
	{
		return;
	}
	for (const FCrowdyExecField& Field : Layout->Structs[Layout->StateStruct].Fields)
	{
		if (!Field.bWatched)
		{
			continue;
		}
		if (Before && Layout->Types[Field.Type].Property->Identical_InContainer(Before, After))
		{
			continue;
		}
		OutFields.Add(Field.Name);
	}
}

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
	FCrowdyNativeExecConnection* Connection = CrowdyServerObjectDetail::GetConnection(Subsystem);
	if (!Pending || !Connection || Pending->bInFlight || Pending->bConnectionClosed || Pending->RetryIn > 0.f)
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
	Connection->Call(Definition->TypeName, InstanceId, Pending->Method, Pending->Payload,
		[WeakThis, CallGeneration, CallId](const FCrowdyNativeExecReply& Reply)
		{
			UCrowdyServerObject* Self = WeakThis.Get();
			if (Self && Self->Generation == CallGeneration)
			{
				Self->HandleCallDone(CallId, Reply);
			}
		});
}

void UCrowdyServerObject::HandleCallDone(uint32 CallId, const FCrowdyNativeExecReply& Reply)
{
	FPendingCall* Pending = PendingCalls.Find(CallId);
	if (!Pending)
	{
		return;
	}
	Pending->bInFlight = false;
	// Closed elsewhere, perhaps mid-teardown: reported on the next Tick so no caller code runs inside the close.
	if (CrowdyServerObjectDetail::IsConnectionClosed(Reply))
	{
		Pending->bConnectionClosed = true;
		return;
	}
	if (CrowdyServerObjectDetail::IsCallRetried(Reply.Status) && Pending->Attempts < CrowdyServerObjectDetail::MaxCallAttempts)
	{
		const float Backoff = FMath::Min(0.25f * static_cast<float>(1 << FMath::Min(Pending->Attempts - 1, 3)), 2.f);
		Pending->RetryIn = FMath::Max(Backoff, CrowdyServerObjectDetail::RetryHintSeconds(Reply));
		return;
	}
	const FCrowdyServerCallResult Result = MakeCallResult(*Pending, Reply);
	const bool bJoinOrLeave = CrowdyServerObjectDetail::IsBuiltInMemberCall(*Definition, Pending->Function, Pending->Method, TEXT("join"))
		|| CrowdyServerObjectDetail::IsBuiltInMemberCall(*Definition, Pending->Function, Pending->Method, TEXT("leave"));
	// A pushed list brings the new membership itself; otherwise a read asks the server for Is Member and Is Leader.
	const bool bReadMembership = Result.IsSuccess() && bJoinOrLeave && !CrowdyServerObjectDetail::IsMembershipPushed(*Definition);
	// A crashed instance restarts from its last snapshot, so the watched values may have rolled back.
	if ((bReadMembership || Reply.Status == ECrowdyNativeExecStatus::Trapped) && SubscribeHandle != 0)
	{
		RequestRead();
	}
	CompleteCall(CallId, Result);
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

FCrowdyServerCallResult UCrowdyServerObject::MakeCallResult(const FPendingCall& Pending, const FCrowdyNativeExecReply& Reply) const
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
		Result.Reason = TEXT("The server could not be reached");
		Result.bRetryable = true;
		break;
	case ECrowdyNativeExecStatus::NotFound:
		Result.Outcome = ECrowdyServerCallOutcome::NotDeployed;
		Result.Reason = CrowdyServerObjectDetail::NotDeployedReason(*Definition);
		break;
	case ECrowdyNativeExecStatus::DeadlineExceeded:
		Result.Outcome = ECrowdyServerCallOutcome::Timeout;
		Result.Reason = TEXT("The server did not answer in time");
		break;
	case ECrowdyNativeExecStatus::Denied:
		Result.Outcome = ECrowdyServerCallOutcome::Denied;
		Result.Reason = FString::Printf(TEXT("This player may not call %s on this Server Object"), *Function);
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
