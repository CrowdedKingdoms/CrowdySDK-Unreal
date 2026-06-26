#include "Subsystem/CrowdyHostSubsystem.h"
#include "CrowdyServicesLog.h"
#include "Messages/GameObjects/FGameEventNotification.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Utils/HelperFunctions.h"

void UCrowdyHostSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	
	const UWorld* World = GetWorld();
	check(IsValid(World));

	if (!IsValid(World))
		return;

	if (!LoadConfig())
		return;

	UGameInstance* GameInstance = World->GetGameInstance();
	if (!GameInstance)
		return;

	GameSession = GameInstance->GetSubsystem<UCrowdyGameSession>();

	check(IsValid(GameSession.Get()));

	if (!IsValid(GameSession.Get()))
	{
		UE_LOG(LogCrowdyServices, Warning, TEXT("[CrowdySDK][HostSubsystem]: GameSession subsystem is invalid."));
		return;
	}

	GameSession->OnOwnerUUIDUpdated.AddDynamic(this, &UCrowdyHostSubsystem::OnOwnerPlayerIDSet);

	bIsReady = true;
}

void UCrowdyHostSubsystem::Deinitialize()
{
	Super::Deinitialize();
}

bool UCrowdyHostSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
		return false;

	const UWorld* World = Outer ? Outer->GetWorld() : nullptr;
	if (!World)
		return false;

	return World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game;
}

FGuid UCrowdyHostSubsystem::GetHostID() const
{
	const FGuid HostID = IsValid(GameSession) ? GameSession->GetHostID() : FGuid();

	if (!HostID.IsValid())
	{
		UE_LOG(LogCrowdyServices, Warning, TEXT("[CrowdySDK][HostSubsystem]: Host is not set yet."));
		return FGuid();
	}

	return HostID;
}

void UCrowdyHostSubsystem::SetHostUserID(const int64 InHostUserID)
{
	const int64 OldHostUserID = HostUserID.exchange(InHostUserID, std::memory_order_relaxed);

	if (OldHostUserID == InHostUserID) return;

	AsyncTask(ENamedThreads::GameThread, [this, InHostUserID, OldHostUserID]()
	{
		const FGuid PreviousHostID = UHelperFunctions::GetDeterministicID(OldHostUserID);
		const FGuid HostID = UHelperFunctions::GetDeterministicID(InHostUserID);

		if (IsValid(GameSession))
			GameSession->SetHostID(HostID);

		OnHostElected.Broadcast(HostID, PreviousHostID);
	});
}

bool UCrowdyHostSubsystem::IsReady() const
{
	return bIsReady;
}


void UCrowdyHostSubsystem::OnOwnerPlayerIDSet(FString OwnerID)
{
	LocalPlayerID = USerializationFunctionLibrary::ToGuid(OwnerID);
}

bool UCrowdyHostSubsystem::LoadConfig() const
{
	const UCrowdyMapProfile* Profile = UCrowdySDKDeveloperSettings::ResolveProfileForWorld(GetWorld());
	return Profile && Profile->bEnableNetworking;
}