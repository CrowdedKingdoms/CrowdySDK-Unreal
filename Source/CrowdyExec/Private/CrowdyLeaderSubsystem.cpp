#include "CrowdyLeaderSubsystem.h"

#include "CrowdyExecLog.h"
#include "CrowdyServerObject.h"
#include "CrowdyServerObjectLibrary.h"
#include "CrowdyServerObjectSubsystem.h"
#include "Engine/GameInstance.h"
#include "Network/UDP/CrowdyUDPSubsystem.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdySDKSubsystem.h"
#include "UObject/UnrealType.h"

namespace CrowdyLeaderDetail
{
	constexpr float RetrySeconds = 10.f;

	// Just past the server's one-second wait between one player's Enters.
	constexpr float CooldownRetrySeconds = 1.5f;

	// How often a client asks an app whose server code has no Enter again, in case it was deployed again since.
	constexpr float OlderServerRetrySeconds = 60.f;

	// Long enough for the pawn that normally spawns just after the realtime connection comes up.
	constexpr float PlayerIDWarningSeconds = 1.f;

	const TCHAR* const LeaderField = TEXT("Leader");
	const TCHAR* const ActorUserField = GET_MEMBER_NAME_STRING_CHECKED(FCrowdyLeaderState, LeaderActorUser);
	const TCHAR* const ActorIDField = GET_MEMBER_NAME_STRING_CHECKED(FCrowdyLeaderState, LeaderActorID);
}

void UCrowdyLeaderSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UCrowdyServerObjectSubsystem>();
	if (UCrowdySDKSubsystem* SDK = Collection.InitializeDependency<UCrowdySDKSubsystem>())
	{
		SDKSubsystem = SDK;
		SDK->OnLogin.AddDynamic(this, &UCrowdyLeaderSubsystem::HandleLogin);
		SDK->OnRegister.AddDynamic(this, &UCrowdyLeaderSubsystem::HandleLogin);
		SDK->OnLogout.AddDynamic(this, &UCrowdyLeaderSubsystem::HandleLogout);
	}
	if (UCrowdyUDPSubsystem* Udp = Collection.InitializeDependency<UCrowdyUDPSubsystem>())
	{
		UdpSubsystem = Udp;
		bRealtimeUp = Udp->GetConnectionState() == EUDPConnectionState::Connected;
		Udp->OnUDPTimeout.AddDynamic(this, &UCrowdyLeaderSubsystem::HandleConnectionLost);
		Udp->OnUDPInactive.AddDynamic(this, &UCrowdyLeaderSubsystem::HandleConnectionLost);
		Udp->OnUDPConnectionSuccessful.AddDynamic(this, &UCrowdyLeaderSubsystem::HandleConnectionUp);
	}
	WatchSession(Collection.InitializeDependency<UCrowdyGameSession>());
	Acquire();
}

void UCrowdyLeaderSubsystem::Deinitialize()
{
	CancelRetry();
	CancelPlayerIDWarning();
	if (UCrowdySDKSubsystem* SDK = SDKSubsystem.Get())
	{
		SDK->OnLogin.RemoveDynamic(this, &UCrowdyLeaderSubsystem::HandleLogin);
		SDK->OnRegister.RemoveDynamic(this, &UCrowdyLeaderSubsystem::HandleLogin);
		SDK->OnLogout.RemoveDynamic(this, &UCrowdyLeaderSubsystem::HandleLogout);
	}
	if (UCrowdyUDPSubsystem* Udp = UdpSubsystem.Get())
	{
		Udp->OnUDPTimeout.RemoveDynamic(this, &UCrowdyLeaderSubsystem::HandleConnectionLost);
		Udp->OnUDPInactive.RemoveDynamic(this, &UCrowdyLeaderSubsystem::HandleConnectionLost);
		Udp->OnUDPConnectionSuccessful.RemoveDynamic(this, &UCrowdyLeaderSubsystem::HandleConnectionUp);
	}
	if (UCrowdyGameSession* Session = GameSession.Get())
	{
		Session->OnOwnerUUIDUpdated.RemoveDynamic(this, &UCrowdyLeaderSubsystem::HandlePlayerIDSet);
	}
	Unfollow();
	Super::Deinitialize();
}

void UCrowdyLeaderSubsystem::WatchSession(UCrowdyGameSession* Session)
{
	GameSession = Session;
	if (Session)
	{
		Session->OnOwnerUUIDUpdated.AddDynamic(this, &UCrowdyLeaderSubsystem::HandlePlayerIDSet);
	}
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyLeaderSubsystem::FollowForTest(UCrowdyServerObject* Object, UCrowdyGameSession* Session, TFunction<void(int64 LeaderUserID, const FGuid& LeaderActorID)> WriteLeader)
{
	WriteLeaderOverride = MoveTemp(WriteLeader);
	WatchSession(Session);
	Follow(Object);
}
#endif

void UCrowdyLeaderSubsystem::Acquire()
{
	const UGameInstance* GameInstance = GetGameInstance();
	UCrowdyServerObjectSubsystem* Objects = GameInstance ? GameInstance->GetSubsystem<UCrowdyServerObjectSubsystem>() : nullptr;
	UCrowdyServerObjectDefinition* Definition = UCrowdyServerObjectLibrary::GetLeaderDefinition();
	if (!Objects || !Definition || Objects->GetSignedInUserId() == 0)
	{
		return;
	}
	FString Error;
	UCrowdyServerObject* Object = Objects->Acquire(Definition, FString(), this, Error);
	UE_CLOG(!Object, LogCrowdyExec, Warning, TEXT("[CrowdyLeader] The Leader type could not be acquired, so this client cannot lead: %s"), *Error);
	if (Object)
	{
		Follow(Object);
	}
}

void UCrowdyLeaderSubsystem::Follow(UCrowdyServerObject* Object)
{
	if (Object != LeaderObject)
	{
		Unfollow();
		LeaderObject = Object;
		if (LeaderObject)
		{
			LeaderObject->OnStatusChanged.AddUObject(this, &UCrowdyLeaderSubsystem::HandleStatusChanged);
			ValuesHandle = LeaderObject->WatchValues(FOnCrowdyServerValuesChanged::FDelegate::CreateUObject(this, &UCrowdyLeaderSubsystem::HandleValuesChanged));
		}
	}
	// Acquiring an object that failed again hands back the same one, so its status is handled again here.
	if (LeaderObject)
	{
		HandleStatusChanged(LeaderObject, LeaderObject->GetStatus());
	}
}

void UCrowdyLeaderSubsystem::Unfollow()
{
	if (LeaderObject)
	{
		LeaderObject->OnStatusChanged.RemoveAll(this);
		LeaderObject->UnwatchValues(ValuesHandle);
	}
	ValuesHandle.Reset();
	LeaderObject = nullptr;
	CancelRetry();
	EnteringActorID.Invalidate();
	EnteredActorID.Invalidate();
	Publish(0);
}

void UCrowdyLeaderSubsystem::HandleStatusChanged(UCrowdyServerObject* Object, ECrowdyServerObjectStatus Status)
{
	if (Status == ECrowdyServerObjectStatus::Ready)
	{
		bWarnedFailure = false;
		Publish(Object->GetLeader());
		// The server may have dropped this client while the object was away, so it enters again.
		Join(true);
		return;
	}
	EnteringActorID.Invalidate();
	if (Status == ECrowdyServerObjectStatus::Connecting)
	{
		return;
	}
	Publish(0);
	if (Status != ECrowdyServerObjectStatus::Failed)
	{
		return;
	}
	UE_CLOG(!bWarnedFailure, LogCrowdyExec, Warning, TEXT("[CrowdyLeader] The Leader type is unavailable (%s), so this client cannot lead; trying again every %.0f s while signed in."),
		*Object->GetFailureReason(), CrowdyLeaderDetail::RetrySeconds);
	bWarnedFailure = true;
	ArmRetry(CrowdyLeaderDetail::RetrySeconds);
}

void UCrowdyLeaderSubsystem::ArmRetry(const float Seconds)
{
	if (PendingRetrySeconds > 0.f && PendingRetrySeconds <= Seconds)
	{
		return;
	}
	CancelRetry();
	PendingRetrySeconds = Seconds;
	// Under FollowForTest the retry waits for RunRetryForTest instead of a ticker.
	if (!WriteLeaderOverride)
	{
		RetryHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UCrowdyLeaderSubsystem::Retry), Seconds);
	}
}

void UCrowdyLeaderSubsystem::CancelRetry()
{
	FTSTicker::RemoveTicker(RetryHandle);
	RetryHandle.Reset();
	PendingRetrySeconds = 0.f;
}

void UCrowdyLeaderSubsystem::HandleValuesChanged(UCrowdyServerObject* Object, TConstArrayView<FString> Fields)
{
	using namespace CrowdyLeaderDetail;
	const bool bLeaderChanged = Fields.Contains(LeaderField);
	if (!bLeaderChanged && !Fields.Contains(ActorUserField) && !Fields.Contains(ActorIDField))
	{
		return;
	}
	const int64 Before = PublishedLeader;
	Publish(Object->GetLeader());
	// Is Member is never pushed, so only a client that just lost the lead, or never got in, may have been removed; entering again is a no-op for a member.
	const int64 OwnUser = Object->GetSignedInUserId();
	if (bLeaderChanged && Object->GetLeader() != OwnUser && (Before == OwnUser || !EnteredActorID.IsValid()))
	{
		Join(true);
	}
}

void UCrowdyLeaderSubsystem::Join(const bool bAgain)
{
	// A player not yet in the realtime world (still connecting, or the app is full) must not lead: the server cannot see it leave.
	if (!LeaderObject || LeaderObject->GetStatus() != ECrowdyServerObjectStatus::Ready || !bRealtimeUp || bEnterStopped)
	{
		return;
	}
	// A sign-in as another player re-keys the session before the connection that carries the last player's token closes.
	const UCrowdyGameSession* Session = GameSession.Get();
	if (!Session || Session->GetUserID() != LeaderObject->GetConnectedUserId())
	{
		return;
	}
	// With no player id this client could lead but never write, and everyone else would wait on it.
	if (!HasPlayerID())
	{
		ArmPlayerIDWarning();
		return;
	}
	const FGuid ActorID = Session->GetID();
	if (ActorID == EnteringActorID || (!bAgain && ActorID == EnteredActorID))
	{
		return;
	}
	EnteringActorID = ActorID;
	FCrowdyLeaderEnterInputs Inputs;
	Inputs.ActorID = ActorID.ToString(EGuidFormats::Digits);
	const TWeakObjectPtr<UCrowdyServerObject> Called(LeaderObject);
	LeaderObject->Call(TEXT("Enter"), FInstancedStruct::Make(Inputs),
		[WeakThis = TWeakObjectPtr<UCrowdyLeaderSubsystem>(this), Called, ActorID, Era = SignInEra](const FCrowdyServerCallResult& Result)
	{
		UCrowdyLeaderSubsystem* Self = WeakThis.Get();
		// An answer from before the latest sign-in, or for an object this client no longer follows, no longer matters.
		if (!Self || Era != Self->SignInEra || !Called.IsValid() || Self->LeaderObject != Called.Get())
		{
			return;
		}
		if (Self->EnteringActorID == ActorID)
		{
			Self->EnteringActorID.Invalidate();
		}
		if (Result.IsSuccess())
		{
			Self->EnteredActorID = ActorID;
			return;
		}
		Self->HandleEnterRefused(Result);
	});
}

void UCrowdyLeaderSubsystem::HandleEnterRefused(const FCrowdyServerCallResult& Result)
{
	if (Result.Outcome == ECrowdyServerCallOutcome::Canceled || LeaderObject->GetSignedInUserId() == 0)
	{
		return;
	}
	if (Result.Reason.StartsWith(TEXT("unknown_method"), ESearchCase::CaseSensitive))
	{
		UE_CLOG(!bWarnedEnter, LogCrowdyExec, Warning,
			TEXT("[CrowdyLeader] The app's server code was deployed with an older Crowdy SDK, whose Leader type has no Enter, so no client can lead; deploy the app's server code again from Server Compute. A deploy does not move running instances, so restart them too (Restart in Server Compute, or -run=CrowdyServerCompute -op=restart -yes)."));
		bWarnedEnter = true;
		bEnterStopped = true;
		ArmRetry(CrowdyLeaderDetail::OlderServerRetrySeconds);
		return;
	}
	// The server's wait between one player's Enters is short and says nothing is wrong.
	if (Result.Outcome == ECrowdyServerCallOutcome::Denied && Result.bRetryable)
	{
		ArmRetry(CrowdyLeaderDetail::CooldownRetrySeconds);
		return;
	}
	UE_CLOG(!bWarnedEnter, LogCrowdyExec, Warning, TEXT("[CrowdyLeader] Entering the Leader type failed (%s), so this client cannot lead; trying again every %.0f s."),
		*Result.Reason, CrowdyLeaderDetail::RetrySeconds);
	bWarnedEnter = true;
	ArmRetry(CrowdyLeaderDetail::RetrySeconds);
}

FGuid UCrowdyLeaderSubsystem::FindLeaderActorID(const int64 Leader) const
{
	using namespace CrowdyLeaderDetail;
	if (Leader == 0 || !LeaderObject)
	{
		return FGuid();
	}
	const FInstancedStruct& State = LeaderObject->GetState();
	const FInt64Property* UserField = CastField<FInt64Property>(UCrowdyServerObjectLibrary::FindServerValue(State, ActorUserField));
	const FStrProperty* IDField = CastField<FStrProperty>(UCrowdyServerObjectLibrary::FindServerValue(State, ActorIDField));
	const uint8* Memory = State.GetMemory();
	// An id published for a player who no longer leads is stale.
	if (!UserField || !IDField || !Memory || UserField->GetPropertyValue_InContainer(Memory) != Leader)
	{
		return FGuid();
	}
	FGuid ActorID;
	if (!FGuid::ParseExact(IDField->GetPropertyValue_InContainer(Memory), EGuidFormats::Digits, ActorID))
	{
		return FGuid();
	}
	// Another user's Leader publishing this session's actor id must not make this client write.
	const UCrowdyGameSession* Session = GameSession.Get();
	if (Session && ActorID == Session->GetID() && Leader != LeaderObject->GetSignedInUserId())
	{
		return FGuid();
	}
	return ActorID;
}

void UCrowdyLeaderSubsystem::AnnounceLeader()
{
	if (PublishedLeader == AnnouncedLeader)
	{
		return;
	}
	// This user leads but not yet under this session's actor id: announcing now would hand handlers a Leader that cannot write.
	const bool bOwnUser = PublishedLeader != 0 && LeaderObject && PublishedLeader == LeaderObject->GetSignedInUserId();
	if (bOwnUser && !IsLocalLeader())
	{
		return;
	}
	const int64 Previous = AnnouncedLeader;
	AnnouncedLeader = PublishedLeader;
	OnLeaderChanged.Broadcast(PublishedLeader, Previous);
}

void UCrowdyLeaderSubsystem::ArmPlayerIDWarning()
{
	if (bWarnedPlayerID || PlayerIDWarningHandle.IsValid())
	{
		return;
	}
	PlayerIDWarningHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UCrowdyLeaderSubsystem::WarnIfStillNoPlayerID), CrowdyLeaderDetail::PlayerIDWarningSeconds);
}

void UCrowdyLeaderSubsystem::CancelPlayerIDWarning()
{
	if (!PlayerIDWarningHandle.IsValid())
	{
		return;
	}
	FTSTicker::RemoveTicker(PlayerIDWarningHandle);
	PlayerIDWarningHandle.Reset();
}

bool UCrowdyLeaderSubsystem::WarnIfStillNoPlayerID(float DeltaTime)
{
	PlayerIDWarningHandle.Reset();
	const bool bCouldJoin = LeaderObject && LeaderObject->GetStatus() == ECrowdyServerObjectStatus::Ready && bRealtimeUp;
	if (bWarnedPlayerID || !bCouldJoin || HasPlayerID())
	{
		return false;
	}
	UE_LOG(LogCrowdyExec, Warning,
		TEXT("[CrowdyLeader] This client holds no actor id, so it cannot lead until a Player Derived pawn spawns or something else gives the session one (a Crowdy Spectator component, a chat message)."));
	bWarnedPlayerID = true;
	return false;
}

void UCrowdyLeaderSubsystem::ResetForSignIn()
{
	CancelPlayerIDWarning();
	bWarnedPlayerID = false;
	bWarnedEnter = false;
	bEnterStopped = false;
	CancelRetry();
	++SignInEra;
	EnteringActorID.Invalidate();
	EnteredActorID.Invalidate();
}

#if WITH_DEV_AUTOMATION_TESTS
bool UCrowdyLeaderSubsystem::RunPlayerIDWarningForTest()
{
	if (!PlayerIDWarningHandle.IsValid())
	{
		return false;
	}
	CancelPlayerIDWarning();
	WarnIfStillNoPlayerID(0.f);
	return true;
}

bool UCrowdyLeaderSubsystem::RunRetryForTest()
{
	if (PendingRetrySeconds <= 0.f)
	{
		return false;
	}
	CancelRetry();
	bEnterStopped = false;
	Follow(LeaderObject);
	return true;
}
#endif

bool UCrowdyLeaderSubsystem::HasPlayerID() const
{
	const UCrowdyGameSession* Session = GameSession.Get();
	return Session && Session->GetID().IsValid();
}

void UCrowdyLeaderSubsystem::HandlePlayerIDSet(FString PlayerUUID)
{
	// Whether the published actor id is this session's depends on the session's id.
	if (LeaderObject && LeaderObject->GetStatus() == ECrowdyServerObjectStatus::Ready)
	{
		Publish(LeaderObject->GetLeader());
	}
	Join(false);
}

void UCrowdyLeaderSubsystem::HandleConnectionLost()
{
	bRealtimeUp = false;
	// An Enter still in flight is undone by the Exit, so the next connection enters again.
	EnteringActorID.Invalidate();
	const UCrowdyGameSession* Session = GameSession.Get();
	// The server takes the user out only for the actor id it entered with, so another session of the same user stays in.
	const FGuid ActorID = EnteredActorID.IsValid() ? EnteredActorID : Session ? Session->GetID() : FGuid();
	if (!LeaderObject || LeaderObject->GetStatus() != ECrowdyServerObjectStatus::Ready || !ActorID.IsValid())
	{
		return;
	}
	FCrowdyLeaderEnterInputs Inputs;
	Inputs.ActorID = ActorID.ToString(EGuidFormats::Digits);
	LeaderObject->Call(TEXT("Exit"), FInstancedStruct::Make(Inputs), [](const FCrowdyServerCallResult& Result)
	{
		UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("[CrowdyLeader] Exited the Leader type after the realtime connection failed: %s"),
			Result.IsSuccess() ? TEXT("done") : *Result.Reason);
	});
}

void UCrowdyLeaderSubsystem::HandleConnectionUp()
{
	bRealtimeUp = true;
	Join(true);
}

bool UCrowdyLeaderSubsystem::IsLocalLeader() const
{
	const UCrowdyGameSession* Session = GameSession.Get();
	return PublishedLeader != 0 && LeaderObject && PublishedLeader == LeaderObject->GetSignedInUserId() && PublishedActorID.IsValid() && Session
		&& PublishedActorID == Session->GetID();
}

void UCrowdyLeaderSubsystem::Publish(int64 NewLeader)
{
	const FGuid NewActorID = FindLeaderActorID(NewLeader);
	if (NewLeader == PublishedLeader && NewActorID == PublishedActorID)
	{
		AnnounceLeader();
		return;
	}
	PublishedLeader = NewLeader;
	PublishedActorID = NewActorID;
	UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("[CrowdyLeader] The Leader is now %lld, %s."), NewLeader,
		NewActorID.IsValid() ? TEXT("writing under its published actor id") : TEXT("with no actor id published"));
	if (WriteLeaderOverride)
	{
		WriteLeaderOverride(NewLeader, NewActorID);
	}
	else
	{
		WriteHostStore();
	}
	// The host store is written first, so a handler that writes as the Leader is let through.
	AnnounceLeader();
}

void UCrowdyLeaderSubsystem::WriteHostStore() const
{
	UCrowdyGameSession* Session = GameSession.Get();
	if (!Session)
	{
		return;
	}
	Session->SetHostID(PublishedActorID);
}

bool UCrowdyLeaderSubsystem::Retry(float DeltaTime)
{
	RetryHandle.Reset();
	PendingRetrySeconds = 0.f;
	// A retry asks a server that had no Enter again, in case it was deployed again since.
	bEnterStopped = false;
	Acquire();
	return false;
}

void UCrowdyLeaderSubsystem::HandleLogin(bool bSuccess, FString Message)
{
	if (!bSuccess)
	{
		return;
	}
	ResetForSignIn();

	// A sign-in over another closes the realtime connection without reporting it failed, and opens its own later.
	const UCrowdyUDPSubsystem* Udp = UdpSubsystem.Get();
	bRealtimeUp = Udp && Udp->GetConnectionState() == EUDPConnectionState::Connected;
	Acquire();
}

void UCrowdyLeaderSubsystem::HandleLogout(bool bSuccess)
{
	// Signing out closes the realtime connection without reporting it failed.
	bRealtimeUp = false;
	ResetForSignIn();
	Publish(0);
}
