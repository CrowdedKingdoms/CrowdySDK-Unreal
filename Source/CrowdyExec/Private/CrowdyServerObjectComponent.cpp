#include "CrowdyServerObjectComponent.h"

#include "CrowdyExecLog.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectLibrary.h"
#include "CrowdyServerObjectShared.h"
#include "CrowdyServerObjectSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdySDKSubsystem.h"
#include "Subsystem/CrowdyTeams.h"
#include "UObject/UnrealType.h"

namespace CrowdyServerObjectComponentDetail
{
	using CrowdyServerObjectShared::WaitingForSignIn;
	using CrowdyServerObjectShared::WaitingForTeams;

	const TCHAR* const PickDefinition = TEXT("Pick a Server Object definition on the Crowdy Server Object component");
	const TCHAR* const SetSourceVariable = TEXT("Set a Source Variable on the Crowdy Server Object component");

	template <typename TSubsystem>
	TSubsystem* FindGameInstanceSubsystem(const UWorld* World)
	{
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance ? GameInstance->GetSubsystem<TSubsystem>() : nullptr;
	}

	int64 FindSignedInUserId(const UWorld* World)
	{
		const UCrowdyGameSession* Session = FindGameInstanceSubsystem<UCrowdyGameSession>(World);
		return Session ? Session->GetUserID() : 0;
	}

	FString WaitingForVariable(FName Variable)
	{
		return FString::Printf(TEXT("Waiting for %s"), *Variable.ToString());
	}

	void RequestMyTeams(const UWorld* World)
	{
		CrowdyServerObjectShared::RequestMyTeams(FindGameInstanceSubsystem<UCrowdyTeams>(World));
	}

	/** Values' Name as an Instance Id: a String or Name as is, an integer in decimal, empty for 0. False for any other type. */
	bool ReadInstanceIdValue(const FInstancedStruct& Values, FName Name, FString& OutValue)
	{
		const FProperty* Field = UCrowdyServerObjectLibrary::FindServerValue(Values, Name);
		const uint8* Memory = Values.GetMemory();
		if (!Field || !Memory)
		{
			return false;
		}
		const void* Value = Field->ContainerPtrToValuePtr<void>(Memory);
		if (const FStrProperty* String = CastField<FStrProperty>(Field))
		{
			OutValue = String->GetPropertyValue(Value);
			return true;
		}
		if (const FNameProperty* NameField = CastField<FNameProperty>(Field))
		{
			const FName Read = NameField->GetPropertyValue(Value);
			OutValue = Read.IsNone() ? FString() : Read.ToString();
			return true;
		}
		const FNumericProperty* Numeric = CastField<FNumericProperty>(Field);
		if (!Numeric || !Numeric->IsInteger() || Numeric->IsEnum())
		{
			return false;
		}
		OutValue = Numeric->IsA<FUInt64Property>() ? LexToString(Numeric->GetUnsignedIntPropertyValue(Value)) : LexToString(Numeric->GetSignedIntPropertyValue(Value));
		if (OutValue.Equals(TEXT("0"), ESearchCase::CaseSensitive))
		{
			OutValue.Reset();
		}
		return true;
	}
}

UCrowdyServerObjectComponent::UCrowdyServerObjectComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UCrowdyServerObjectComponent::OnRegister()
{
	Super::OnRegister();

	// A cooked actor drops its placement guid once all its components are registered, so only a component registered with it reads one, in every build.
	const AActor* Owner = GetOwner();
	if (!PlacementGuid.IsValid() && Owner && !Owner->HasActorRegisteredAllComponents())
	{
		PlacementGuid = UCrowdyEntityComponent::ResolveActorInstanceGuid(Owner);
	}
}

FString UCrowdyServerObjectComponent::ResolveInstanceId(FString& OutError) const
{
	OutError.Reset();
	if (Definition && Definition->bOnlyOneInstance)
	{
		return UCrowdyServerObjectDefinition::OnlyInstanceId;
	}
	switch (InstanceMode)
	{
	case ECrowdyServerObjectInstanceMode::InstanceId:
	{
		if (InstanceId.IsEmpty())
		{
			OutError = TEXT("Set an Instance Id on the Crowdy Server Object component");
		}
		return InstanceId;
	}
	case ECrowdyServerObjectInstanceMode::SignedInPlayer:
	{
		const int64 UserId = CrowdyServerObjectComponentDetail::FindSignedInUserId(GetWorld());
		if (UserId <= 0)
		{
			OutError = TEXT("Nobody is signed in yet");
			return FString();
		}
		return LexToString(UserId);
	}
	case ECrowdyServerObjectInstanceMode::ThisActor:
	{
		const AActor* Owner = GetOwner();
		if (!Owner || !Owner->IsNetStartupActor() || !PlacementGuid.IsValid())
		{
			OutError = TEXT("This Actor needs an actor placed in the level with the component on it; otherwise give it an Instance Id");
			return FString();
		}
		return PlacementGuid.ToString(EGuidFormats::Digits);
	}
	case ECrowdyServerObjectInstanceMode::PlayersTeam:
	{
		const UCrowdyTeams* Teams = TeamId != 0 ? nullptr : CrowdyServerObjectComponentDetail::FindGameInstanceSubsystem<UCrowdyTeams>(GetWorld());
		return CrowdyServerObjectShared::ResolvePlayersTeam(Teams, TeamId, OutError);
	}
	case ECrowdyServerObjectInstanceMode::FromServerValue:
	{
		if (SourceVariable.IsNone())
		{
			OutError = CrowdyServerObjectComponentDetail::SetSourceVariable;
			return FString();
		}
		if (SourceValue.IsEmpty())
		{
			OutError = CrowdyServerObjectComponentDetail::WaitingForVariable(SourceVariable);
		}
		return SourceValue;
	}
	}
	OutError = TEXT("Unknown Instance Mode on the Crowdy Server Object component");
	return FString();
}

void UCrowdyServerObjectComponent::BeginPlay()
{
	Super::BeginPlay();

	if (UCrowdySDKSubsystem* SDK = CrowdyServerObjectComponentDetail::FindGameInstanceSubsystem<UCrowdySDKSubsystem>(GetWorld()))
	{
		SDK->OnLogin.AddUniqueDynamic(this, &UCrowdyServerObjectComponent::HandleLogin);
		SDK->OnRegister.AddUniqueDynamic(this, &UCrowdyServerObjectComponent::HandleLogin);
		bLoginBound = true;
	}

	UCrowdyTeams* Teams = InstanceMode == ECrowdyServerObjectInstanceMode::PlayersTeam ? CrowdyServerObjectComponentDetail::FindGameInstanceSubsystem<UCrowdyTeams>(GetWorld()) : nullptr;
	if (Teams)
	{
		Teams->OnMyTeamsCacheChanged.AddUniqueDynamic(this, &UCrowdyServerObjectComponent::HandleMyTeamsChanged);
		bTeamsBound = true;
	}

	if (FollowsSourceValue())
	{
		WatchSource();
		return;
	}
	Join();
}

bool UCrowdyServerObjectComponent::FollowsSourceValue() const
{
	return InstanceMode == ECrowdyServerObjectInstanceMode::FromServerValue && !(Definition && Definition->bOnlyOneInstance);
}

void UCrowdyServerObjectComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UCrowdySDKSubsystem* SDK = bLoginBound ? CrowdyServerObjectComponentDetail::FindGameInstanceSubsystem<UCrowdySDKSubsystem>(GetWorld()) : nullptr;
	if (SDK)
	{
		SDK->OnLogin.RemoveDynamic(this, &UCrowdyServerObjectComponent::HandleLogin);
		SDK->OnRegister.RemoveDynamic(this, &UCrowdyServerObjectComponent::HandleLogin);
	}
	bLoginBound = false;

	UCrowdyTeams* Teams = bTeamsBound ? CrowdyServerObjectComponentDetail::FindGameInstanceSubsystem<UCrowdyTeams>(GetWorld()) : nullptr;
	if (Teams)
	{
		Teams->OnMyTeamsCacheChanged.RemoveDynamic(this, &UCrowdyServerObjectComponent::HandleMyTeamsChanged);
	}
	bTeamsBound = false;
	bTeamsRequested = false;

	StopWatchingSource();
	Leave();
	Super::EndPlay(EndPlayReason);
}

void UCrowdyServerObjectComponent::Join()
{
	if (bJoining)
	{
		return;
	}
	TGuardValue<bool> JoiningGuard(bJoining, true);
	Leave();

	if (!Definition)
	{
		Refuse(CrowdyServerObjectComponentDetail::PickDefinition);
		return;
	}

	FString Error;
	const FString Id = ResolveInstanceId(Error);
	if (!bTeamsRequested && Error.Equals(CrowdyServerObjectComponentDetail::WaitingForTeams, ESearchCase::CaseSensitive))
	{
		bTeamsRequested = true;
		CrowdyServerObjectComponentDetail::RequestMyTeams(GetWorld());
	}

	// Not a failure: it joins when the player signs in, their teams arrive or the source variable is set.
	const bool bSignInWait = InstanceMode == ECrowdyServerObjectInstanceMode::SignedInPlayer;
	const bool bTeamsWait = Error.Equals(CrowdyServerObjectComponentDetail::WaitingForTeams, ESearchCase::CaseSensitive);
	const bool bSourceWait = FollowsSourceValue() && !SourceVariable.IsNone() && SourceValue.IsEmpty();
	if (!Error.IsEmpty() && (bSignInWait || bTeamsWait || bSourceWait))
	{
		JoinError = bSignInWait ? FString(CrowdyServerObjectComponentDetail::WaitingForSignIn) : Error;
		return;
	}
	if (!Error.IsEmpty())
	{
		Refuse(Error);
		return;
	}

	UCrowdyServerObjectSubsystem* Subsystem = FindServerObjects();
	if (!Subsystem)
	{
		Refuse(TEXT("Server Objects are not available: there is no game instance"));
		return;
	}

	UCrowdyServerObject* Acquired = Subsystem->Acquire(Definition, Id, this, Error);
	if (!Acquired)
	{
		Refuse(Error);
		return;
	}

	Object = Acquired;
	JoinError.Reset();
	StatusHandle = Acquired->OnStatusChanged.AddUObject(this, &UCrowdyServerObjectComponent::HandleStatus);
	const FDelegateHandle Watch = Acquired->WatchValues(FOnCrowdyServerValuesChanged::FDelegate::CreateUObject(this, &UCrowdyServerObjectComponent::HandleVariables));
	if (Object != Acquired)
	{
		// A handler of the first values let go of it.
		Acquired->UnwatchValues(Watch);
		return;
	}
	ValuesHandle = Watch;

	// A shared object that is already Ready sends no status change of its own.
	if (Acquired->GetStatus() == ECrowdyServerObjectStatus::Ready)
	{
		OnStatusChanged.Broadcast(Acquired, ECrowdyServerObjectStatus::Ready);
	}
}

void UCrowdyServerObjectComponent::Leave()
{
	UCrowdyServerObject* Held = Object;
	if (!Held)
	{
		return;
	}

	Object = nullptr;
	Held->OnStatusChanged.Remove(StatusHandle);
	Held->UnwatchValues(ValuesHandle);
	StatusHandle.Reset();
	ValuesHandle.Reset();

	// A source that is also the held object is given back once, when it stops being watched; at shutdown the subsystem may be gone already.
	UCrowdyServerObjectSubsystem* Subsystem = FindServerObjects();
	if (Held == SourceObject || !Subsystem)
	{
		return;
	}
	Subsystem->Release(Held, this);
}

UCrowdyServerObjectSubsystem* UCrowdyServerObjectComponent::FindServerObjects() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (UCrowdyServerObjectSubsystem* ForTest = ServerObjectsForTest.Get())
	{
		return ForTest;
	}
#endif
	return CrowdyServerObjectComponentDetail::FindGameInstanceSubsystem<UCrowdyServerObjectSubsystem>(GetWorld());
}

void UCrowdyServerObjectComponent::Refuse(const FString& Reason)
{
	// A Failed handler that calls Rejoin would otherwise refuse again inside this broadcast, without end.
	TGuardValue<bool> JoiningGuard(bJoining, true);
	JoinError = Reason;
	UE_LOG(LogCrowdyExec, Warning, TEXT("Crowdy Server Object on %s could not join: %s"), *GetNameSafe(GetOwner()), *Reason);
	OnStatusChanged.Broadcast(nullptr, ECrowdyServerObjectStatus::Failed);
}

void UCrowdyServerObjectComponent::WatchSource()
{
	if (bJoining)
	{
		return;
	}
	StopWatchingSource();
	Leave();

	if (!Definition)
	{
		Refuse(CrowdyServerObjectComponentDetail::PickDefinition);
		return;
	}
	if (!SourceDefinition)
	{
		Refuse(TEXT("Pick a Source Definition on the Crowdy Server Object component"));
		return;
	}
	if (SourceVariable.IsNone())
	{
		Refuse(CrowdyServerObjectComponentDetail::SetSourceVariable);
		return;
	}

	const bool bPerPlayer = SourceInstance == ECrowdyServerObjectSourceInstance::SignedInPlayer;
	const int64 UserId = bPerPlayer ? CrowdyServerObjectComponentDetail::FindSignedInUserId(GetWorld()) : 0;
	if (bPerPlayer && UserId <= 0)
	{
		// Not a failure: it watches the source when the player signs in.
		JoinError = CrowdyServerObjectComponentDetail::WaitingForSignIn;
		return;
	}
	const FString SourceId = bPerPlayer ? LexToString(UserId) : SourceInstanceId;
	if (SourceId.IsEmpty())
	{
		Refuse(TEXT("Set a Source Instance Id on the Crowdy Server Object component"));
		return;
	}

	UCrowdyServerObjectSubsystem* Subsystem = FindServerObjects();
	if (!Subsystem)
	{
		Refuse(TEXT("Server Objects are not available: there is no game instance"));
		return;
	}

	FString Error;
	UCrowdyServerObject* Acquired = Subsystem->Acquire(SourceDefinition, SourceId, this, Error);
	if (!Acquired)
	{
		Refuse(Error);
		return;
	}

	SourceObject = Acquired;
	JoinError = CrowdyServerObjectComponentDetail::WaitingForVariable(SourceVariable);
	SourceStatusHandle = Acquired->OnStatusChanged.AddUObject(this, &UCrowdyServerObjectComponent::HandleSourceStatus);
	const FDelegateHandle Watch = Acquired->WatchValues(FOnCrowdyServerValuesChanged::FDelegate::CreateUObject(this, &UCrowdyServerObjectComponent::HandleSourceVariables));
	if (SourceObject != Acquired)
	{
		// A handler of the first values let go of it.
		Acquired->UnwatchValues(Watch);
		return;
	}
	SourceValuesHandle = Watch;
}

void UCrowdyServerObjectComponent::StopWatchingSource()
{
	SourceValue.Reset();
	UCrowdyServerObject* Source = SourceObject;
	if (!Source)
	{
		return;
	}

	SourceObject = nullptr;
	Source->UnwatchValues(SourceValuesHandle);
	Source->OnStatusChanged.Remove(SourceStatusHandle);
	SourceValuesHandle.Reset();
	SourceStatusHandle.Reset();

	// A source that is also the held object is given back when the component leaves it.
	UCrowdyServerObjectSubsystem* Subsystem = FindServerObjects();
	if (Source == Object || !Subsystem)
	{
		return;
	}
	Subsystem->Release(Source, this);
}

void UCrowdyServerObjectComponent::HandleSourceStatus(UCrowdyServerObject* InObject, ECrowdyServerObjectStatus Status)
{
	// A source that is also the held object passes its status on through HandleStatus.
	if (InObject != SourceObject || InObject == Object || Status != ECrowdyServerObjectStatus::Failed || Status != InObject->GetStatus())
	{
		return;
	}
	Leave();
	SourceValue.Reset();
	Refuse(InObject->GetFailureReason());
}

void UCrowdyServerObjectComponent::HandleSourceVariables(UCrowdyServerObject* InObject, TConstArrayView<FString> Fields)
{
	if (InObject != SourceObject)
	{
		return;
	}

	FString Value;
	if (!CrowdyServerObjectComponentDetail::ReadInstanceIdValue(InObject->GetState(), SourceVariable, Value))
	{
		Leave();
		SourceValue.Reset();
		const UCrowdyServerObjectDefinition* Source = InObject->GetDefinition();
		Refuse(FString::Printf(TEXT("Source Variable %s is not a String or an integer on %s"), *SourceVariable.ToString(), Source ? *Source->TypeName : TEXT("None")));
		return;
	}
	if (Value.IsEmpty())
	{
		Leave();
		SourceValue.Reset();
		JoinError = CrowdyServerObjectComponentDetail::WaitingForVariable(SourceVariable);
		return;
	}
	if (Value.Equals(SourceValue, ESearchCase::CaseSensitive))
	{
		return;
	}

	SourceValue = Value;
	Join();
}

void UCrowdyServerObjectComponent::HandleLogin(bool bSuccess, FString Message)
{
	if (!bSuccess || !HasBegunPlay())
	{
		return;
	}
	HandleSignedIn();
}

void UCrowdyServerObjectComponent::HandleSignedIn()
{
	// The teams cache may still hold the last account's teams, so this account's are asked for and joined when they arrive.
	const bool bOnlyOne = Definition && Definition->bOnlyOneInstance;
	if (InstanceMode == ECrowdyServerObjectInstanceMode::PlayersTeam && TeamId == 0 && !bOnlyOne)
	{
		Leave();
		JoinError = CrowdyServerObjectComponentDetail::WaitingForTeams;
		bTeamsRequested = true;
		CrowdyServerObjectComponentDetail::RequestMyTeams(GetWorld());
		return;
	}

	const bool bFromSource = FollowsSourceValue();
	const ECrowdyServerObjectStatus SourceStatus = SourceObject ? SourceObject->GetStatus() : ECrowdyServerObjectStatus::Failed;
	const bool bSourceStale = SourceInstance == ECrowdyServerObjectSourceInstance::SignedInPlayer || SourceStatus == ECrowdyServerObjectStatus::Failed || SourceStatus == ECrowdyServerObjectStatus::Released;
	if (bFromSource && bSourceStale)
	{
		WatchSource();
		return;
	}

	// A different account may have signed in, so the signed-in player's object is always joined again.
	const bool bPerPlayer = InstanceMode == ECrowdyServerObjectInstanceMode::SignedInPlayer && !bOnlyOne;
	const ECrowdyServerObjectStatus Status = Object ? Object->GetStatus() : ECrowdyServerObjectStatus::Failed;
	if (bPerPlayer || Status == ECrowdyServerObjectStatus::Failed || Status == ECrowdyServerObjectStatus::Released)
	{
		Join();
	}
}

void UCrowdyServerObjectComponent::HandleMyTeamsChanged(const TArray<FCrowdyTeamMembership>& Memberships)
{
	if (!HasBegunPlay() || InstanceMode != ECrowdyServerObjectInstanceMode::PlayersTeam)
	{
		return;
	}

	// A cleared cache, as on sign-out, is no answer: the next sign-in asks for the teams again.
	FString Error;
	const FString Id = ResolveInstanceId(Error);
	if (Error.Equals(CrowdyServerObjectComponentDetail::WaitingForTeams, ESearchCase::CaseSensitive))
	{
		return;
	}
	if (Object && Id.Equals(Object->GetInstanceId(), ESearchCase::CaseSensitive))
	{
		return;
	}
	Join();
}

void UCrowdyServerObjectComponent::HandleStatus(UCrowdyServerObject* InObject, ECrowdyServerObjectStatus Status)
{
	// A handler that ran first may have moved the object on already; only its current status is passed on.
	if (InObject != Object || Status != InObject->GetStatus())
	{
		return;
	}
	OnStatusChanged.Broadcast(InObject, Status);
}

void UCrowdyServerObjectComponent::HandleVariables(UCrowdyServerObject* InObject, TConstArrayView<FString> Fields)
{
	if (InObject != Object)
	{
		return;
	}
	OnVariablesChanged.Broadcast(InObject, InObject->ToVariableNames(Fields));
	if (InObject == Object)
	{
		VariableBindings.Notify(Object, Fields);
	}
}

FString UCrowdyServerObjectComponent::GetFailureReason() const
{
	if (Object)
	{
		return Object->GetFailureReason();
	}
	if (SourceObject && SourceObject->GetStatus() == ECrowdyServerObjectStatus::Failed)
	{
		return SourceObject->GetFailureReason();
	}
	return JoinError;
}

void UCrowdyServerObjectComponent::Rejoin()
{
	if (!HasBegunPlay() || bJoining)
	{
		return;
	}

	// A retry asks for the teams again, in case the last request failed.
	bTeamsRequested = false;
	StopWatchingSource();
	if (FollowsSourceValue())
	{
		WatchSource();
		return;
	}
	Join();
}
