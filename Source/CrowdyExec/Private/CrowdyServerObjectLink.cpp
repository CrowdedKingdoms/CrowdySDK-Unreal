#include "CrowdyServerObjectLink.h"

#include "CrowdyExecLog.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectShared.h"
#include "CrowdyServerObjectSubsystem.h"
#include "Engine/GameInstance.h"
#include "Subsystem/CrowdySDKSubsystem.h"
#include "Subsystem/CrowdyTeams.h"

FString UCrowdyServerObjectLink::GetFailureReason() const
{
	return Object ? Object->GetFailureReason() : JoinError;
}

UCrowdyServerObjectSubsystem* UCrowdyServerObjectLink::GetSubsystem() const
{
	return Cast<UCrowdyServerObjectSubsystem>(GetOuter());
}

UCrowdyTeams* UCrowdyServerObjectLink::FindTeams() const
{
	const UCrowdyServerObjectSubsystem* Subsystem = GetSubsystem();
	const UGameInstance* GameInstance = Subsystem ? Subsystem->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetSubsystem<UCrowdyTeams>() : nullptr;
}

void UCrowdyServerObjectLink::Start(UObject* InOwner, UCrowdyServerObjectDefinition* InDefinition, ECrowdyServerObjectFind InFind, const FString& InInstanceId, int64 InTeamId)
{
	Owner = InOwner;
	Definition = InDefinition;
	Find = InFind;
	InstanceId = InInstanceId;
	TeamId = InTeamId;

	const UCrowdyServerObjectSubsystem* Subsystem = GetSubsystem();
	const UGameInstance* GameInstance = Subsystem ? Subsystem->GetGameInstance() : nullptr;
	if (UCrowdySDKSubsystem* SDK = GameInstance ? GameInstance->GetSubsystem<UCrowdySDKSubsystem>() : nullptr)
	{
		SDK->OnLogin.AddUniqueDynamic(this, &UCrowdyServerObjectLink::HandleLogin);
		SDK->OnRegister.AddUniqueDynamic(this, &UCrowdyServerObjectLink::HandleLogin);
		bLoginBound = true;
	}
	UCrowdyTeams* Teams = Find == ECrowdyServerObjectFind::PlayersTeam ? FindTeams() : nullptr;
	if (Teams)
	{
		Teams->OnMyTeamsCacheChanged.AddUniqueDynamic(this, &UCrowdyServerObjectLink::HandleMyTeamsChanged);
		bTeamsBound = true;
	}
	Join();
}

void UCrowdyServerObjectLink::Stop()
{
	const UCrowdyServerObjectSubsystem* Subsystem = GetSubsystem();
	const UGameInstance* GameInstance = Subsystem ? Subsystem->GetGameInstance() : nullptr;
	UCrowdySDKSubsystem* SDK = bLoginBound && GameInstance ? GameInstance->GetSubsystem<UCrowdySDKSubsystem>() : nullptr;
	if (SDK)
	{
		SDK->OnLogin.RemoveDynamic(this, &UCrowdyServerObjectLink::HandleLogin);
		SDK->OnRegister.RemoveDynamic(this, &UCrowdyServerObjectLink::HandleLogin);
	}
	UCrowdyTeams* Teams = bTeamsBound ? FindTeams() : nullptr;
	if (Teams)
	{
		Teams->OnMyTeamsCacheChanged.RemoveDynamic(this, &UCrowdyServerObjectLink::HandleMyTeamsChanged);
	}
	bLoginBound = false;
	bTeamsBound = false;
	Leave();
	VariableBindings.Reset();
}

void UCrowdyServerObjectLink::Tick(float DeltaSeconds)
{
	if (Object || !JoinError.Equals(CrowdyServerObjectShared::WaitingForTeams, ESearchCase::CaseSensitive))
	{
		return;
	}
	TeamsRetryIn -= DeltaSeconds;
	if (TeamsRetryIn <= 0.f)
	{
		RequestTeams();
	}
}

void UCrowdyServerObjectLink::RequestTeams()
{
	TeamsRetryIn = TeamsRetrySeconds;
	++TeamsRequests;
	CrowdyServerObjectShared::RequestMyTeams(FindTeams());
}

FString UCrowdyServerObjectLink::ResolveInstanceId(FString& OutError, const TArray<FCrowdyTeamMembership>* Arrived) const
{
	OutError.Reset();
	if (Find == ECrowdyServerObjectFind::PlayersTeam && Arrived)
	{
		return CrowdyServerObjectShared::ResolvePlayersTeam(Arrived, TeamId, OutError);
	}
	if (Find == ECrowdyServerObjectFind::PlayersTeam)
	{
		return CrowdyServerObjectShared::ResolvePlayersTeam(TeamId != 0 ? nullptr : FindTeams(), TeamId, OutError);
	}
	if (Find == ECrowdyServerObjectFind::InstanceId)
	{
		if (InstanceId.IsEmpty())
		{
			OutError = TEXT("Set an Instance Id to find the Server Object by");
		}
		return InstanceId;
	}
	const UCrowdyServerObjectSubsystem* Subsystem = GetSubsystem();
	const int64 UserId = Subsystem ? Subsystem->GetSignedInUserId() : 0;
	if (UserId <= 0)
	{
		OutError = CrowdyServerObjectShared::WaitingForSignIn;
		return FString();
	}
	return LexToString(UserId);
}

void UCrowdyServerObjectLink::Join(const TArray<FCrowdyTeamMembership>* Arrived)
{
	Leave();
	FString Error;
	const FString Id = ResolveInstanceId(Error, Arrived);
	const bool bTeamsWait = Error.Equals(CrowdyServerObjectShared::WaitingForTeams, ESearchCase::CaseSensitive);
	if (bTeamsWait && TeamsRetryIn <= 0.f)
	{
		RequestTeams();
	}
	// Waiting for sign-in or the teams is not a failure: a sign-in or the teams arriving joins.
	UCrowdyServerObjectSubsystem* Subsystem = GetSubsystem();
	if (!Error.IsEmpty() || !Subsystem || !Owner.IsValid())
	{
		JoinError = Error;
		return;
	}
	// Held as its own owner, so a sibling Link or the owner releasing the object leaves this Link's hold alone.
	UCrowdyServerObject* Acquired = Subsystem->Acquire(Definition, Id, this, Error);
	if (!Acquired)
	{
		JoinError = Error;
		UE_LOG(LogCrowdyExec, Warning, TEXT("A Server Object Link for %s could not join: %s"), *GetNameSafe(Owner.Get()), *Error);
		return;
	}
	Object = Acquired;
	JoinError.Reset();
	ValuesHandle = Acquired->WatchValues(FOnCrowdyServerValuesChanged::FDelegate::CreateUObject(this, &UCrowdyServerObjectLink::HandleVariables));
}

void UCrowdyServerObjectLink::Leave()
{
	UCrowdyServerObject* Held = Object;
	if (!Held)
	{
		return;
	}
	Object = nullptr;
	Held->UnwatchValues(ValuesHandle);
	ValuesHandle.Reset();
	if (UCrowdyServerObjectSubsystem* Subsystem = GetSubsystem())
	{
		Subsystem->Release(Held, this);
	}
}

void UCrowdyServerObjectLink::HandleLogin(bool bSuccess, FString Message)
{
	if (bSuccess)
	{
		HandleSignedIn();
	}
}

void UCrowdyServerObjectLink::HandleSignedIn()
{
	// The teams cache may still hold the last account's teams, so this account's are asked for and joined when they arrive.
	if (Find == ECrowdyServerObjectFind::PlayersTeam && TeamId == 0)
	{
		Leave();
		JoinError = CrowdyServerObjectShared::WaitingForTeams;
		RequestTeams();
		return;
	}
	// A different account may have signed in, so the signed-in player's object is always joined again.
	const ECrowdyServerObjectStatus Status = Object ? Object->GetStatus() : ECrowdyServerObjectStatus::Failed;
	if (Find == ECrowdyServerObjectFind::SignedInPlayer || Status == ECrowdyServerObjectStatus::Failed || Status == ECrowdyServerObjectStatus::Released)
	{
		Join();
	}
}

void UCrowdyServerObjectLink::HandleMyTeamsChanged(const TArray<FCrowdyTeamMembership>& Memberships)
{
	// A cleared cache, as on sign-out, is no answer: the next sign-in asks for the teams again.
	const UCrowdyTeams* Teams = FindTeams();
	if (Teams && !Teams->HasCachedTeams())
	{
		return;
	}
	FString Error;
	const FString Id = ResolveInstanceId(Error, &Memberships);
	if (Object && Id.Equals(Object->GetInstanceId(), ESearchCase::CaseSensitive))
	{
		return;
	}
	Join(&Memberships);
}

void UCrowdyServerObjectLink::HandleVariables(UCrowdyServerObject* InObject, TConstArrayView<FString> Fields)
{
	if (InObject == Object)
	{
		VariableBindings.Notify(Object, Fields);
	}
}
