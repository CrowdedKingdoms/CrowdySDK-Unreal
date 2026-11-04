#include "CrowdyLeaderSubsystem.h"

#include "CrowdyExecLog.h"
#include "CrowdyServerObject.h"
#include "CrowdyServerObjectLibrary.h"
#include "CrowdyServerObjectSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Network/UDP/CrowdyUDPSubsystem.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdyHostSubsystem.h"
#include "Subsystem/CrowdySDKSubsystem.h"
#include "UObject/UObjectGlobals.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

namespace CrowdyLeaderDetail
{
	constexpr float RetrySeconds = 10.f;
}

void UCrowdyLeaderSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (UCrowdySDKDeveloperSettings::UsesElectedHost())
	{
		return;
	}

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
		Udp->OnUDPConnectionSuccessful.AddDynamic(this, &UCrowdyLeaderSubsystem::HandleConnectionUp);
	}
	PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &UCrowdyLeaderSubsystem::HandlePostLoadMap);
	Acquire();
}

void UCrowdyLeaderSubsystem::Deinitialize()
{
	FTSTicker::RemoveTicker(RetryHandle);
	RetryHandle.Reset();
	FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapHandle);
	if (UCrowdySDKSubsystem* SDK = SDKSubsystem.Get())
	{
		SDK->OnLogin.RemoveDynamic(this, &UCrowdyLeaderSubsystem::HandleLogin);
		SDK->OnRegister.RemoveDynamic(this, &UCrowdyLeaderSubsystem::HandleLogin);
		SDK->OnLogout.RemoveDynamic(this, &UCrowdyLeaderSubsystem::HandleLogout);
	}
	if (UCrowdyUDPSubsystem* Udp = UdpSubsystem.Get())
	{
		Udp->OnUDPTimeout.RemoveDynamic(this, &UCrowdyLeaderSubsystem::HandleConnectionLost);
		Udp->OnUDPConnectionSuccessful.RemoveDynamic(this, &UCrowdyLeaderSubsystem::HandleConnectionUp);
	}
	Unfollow();
	Super::Deinitialize();
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyLeaderSubsystem::FollowForTest(UCrowdyServerObject* Object, TFunction<void(int64)> WriteLeader)
{
	WriteLeaderOverride = MoveTemp(WriteLeader);
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
	Publish(0);
}

void UCrowdyLeaderSubsystem::HandleStatusChanged(UCrowdyServerObject* Object, ECrowdyServerObjectStatus Status)
{
	if (Status == ECrowdyServerObjectStatus::Ready)
	{
		bWarnedFailure = false;
		Publish(Object->GetLeader());
		Join();
		return;
	}
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
	ArmRetry();
}

void UCrowdyLeaderSubsystem::ArmRetry()
{
	if (!RetryHandle.IsValid() && !WriteLeaderOverride)
	{
		RetryHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UCrowdyLeaderSubsystem::Retry), CrowdyLeaderDetail::RetrySeconds);
	}
}

void UCrowdyLeaderSubsystem::HandleValuesChanged(UCrowdyServerObject* Object, TConstArrayView<FString> Fields)
{
	if (!Fields.Contains(TEXT("Leader")))
	{
		return;
	}
	Publish(Object->GetLeader());
	// Is Member is never pushed, so a client that lost the lead, or never got in, cannot tell; joining again is a no-op for a member.
	if (Object->GetLeader() != Object->GetSignedInUserId())
	{
		Join();
	}
}

void UCrowdyLeaderSubsystem::Join()
{
	// A player not yet in the realtime world (still connecting, or the app is full) must not lead: the server cannot see it leave.
	if (!LeaderObject || LeaderObject->GetStatus() != ECrowdyServerObjectStatus::Ready || !bRealtimeUp)
	{
		return;
	}
	LeaderObject->Call(TEXT("Join"), FInstancedStruct(), [WeakThis = TWeakObjectPtr<UCrowdyLeaderSubsystem>(this)](const FCrowdyServerCallResult& Result)
	{
		UCrowdyLeaderSubsystem* Self = WeakThis.Get();
		if (!Self || Result.IsSuccess() || Result.Outcome == ECrowdyServerCallOutcome::Canceled)
		{
			return;
		}
		UE_LOG(LogCrowdyExec, Warning, TEXT("[CrowdyLeader] Joining the Leader type failed (%s); trying again in %.0f s."), *Result.Reason, CrowdyLeaderDetail::RetrySeconds);
		Self->ArmRetry();
	});
}

void UCrowdyLeaderSubsystem::HandleConnectionLost()
{
	bRealtimeUp = false;
	if (!LeaderObject || LeaderObject->GetStatus() != ECrowdyServerObjectStatus::Ready)
	{
		return;
	}
	LeaderObject->Call(TEXT("Leave"), FInstancedStruct(), [](const FCrowdyServerCallResult& Result)
	{
		UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("[CrowdyLeader] Left the Leader type after the realtime connection failed: %s"),
			Result.IsSuccess() ? TEXT("done") : *Result.Reason);
	});
}

void UCrowdyLeaderSubsystem::HandleConnectionUp()
{
	bRealtimeUp = true;
	Join();
}

bool UCrowdyLeaderSubsystem::IsLocalLeader() const
{
	return PublishedLeader != 0 && LeaderObject && PublishedLeader == LeaderObject->GetSignedInUserId();
}

void UCrowdyLeaderSubsystem::Publish(int64 NewLeader)
{
	if (NewLeader == PublishedLeader)
	{
		return;
	}
	const int64 Previous = PublishedLeader;
	PublishedLeader = NewLeader;
	UE_CLOG(CrowdyExecTrace::Exec(), LogCrowdyExec, Log, TEXT("[CrowdyLeader] The Leader is now %lld."), NewLeader);
	if (WriteLeaderOverride)
	{
		WriteLeaderOverride(NewLeader);
	}
	else
	{
		WriteHostStore();
	}
	OnLeaderChanged.Broadcast(NewLeader, Previous);
}

void UCrowdyLeaderSubsystem::WriteHostStore() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	if (!GameInstance)
	{
		return;
	}
	// The session holds the id replication reads, written here at once: the world's host front writes it a frame
	// later, and a front torn down by travel in between never does.
	if (UCrowdyGameSession* Session = GameInstance->GetSubsystem<UCrowdyGameSession>())
	{
		Session->SetHostID(UCrowdyHostSubsystem::HostIDForUser(PublishedLeader));
	}
	UWorld* World = GameInstance->GetWorld();
	if (UCrowdyHostSubsystem* Hosts = World ? World->GetSubsystem<UCrowdyHostSubsystem>() : nullptr)
	{
		Hosts->SetHostUserID(PublishedLeader);
	}
}

void UCrowdyLeaderSubsystem::HandlePostLoadMap(UWorld* World)
{
	// The host store's front is rebuilt with each world and starts empty.
	if (World && World->GetGameInstance() == GetGameInstance())
	{
		WriteHostStore();
	}
}

bool UCrowdyLeaderSubsystem::Retry(float DeltaTime)
{
	RetryHandle.Reset();
	Acquire();
	return false;
}

void UCrowdyLeaderSubsystem::HandleLogin(bool bSuccess, FString Message)
{
	if (bSuccess)
	{
		Acquire();
	}
}

void UCrowdyLeaderSubsystem::HandleLogout(bool bSuccess)
{
	// Signing out closes the realtime connection without reporting it failed.
	bRealtimeUp = false;
	Publish(0);
}
