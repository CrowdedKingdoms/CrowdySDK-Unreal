#include "Subsystem/CrowdyHostSubsystem.h"
#include "Messages/GameObjects/FGameEventNotification.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdySDKSubsystem.h"
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

	CrowdySDK          = World->GetGameInstance()->GetSubsystem<UCrowdySDKSubsystem>();
	GameSession        = World->GetGameInstance()->GetSubsystem<UCrowdyGameSession>();
	
	check(IsValid(CrowdySDK.Get()));
	check(IsValid(GameSession.Get()));


	if (!IsValid(GameSession.Get()) || !IsValid(CrowdySDK.Get()))
	{
		UE_LOG(LogTemp, Warning, TEXT("[CrowdySDK][HostSubsystem]: One or more required subsystems are invalid."));
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
	if (!HostID.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[CrowdySDK][HostSubsystem]: Host is not set yet."));
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
		PreviousHostID = UHelperFunctions::GetDeterministicID(OldHostUserID);
		HostID = UHelperFunctions::GetDeterministicID(InHostUserID);
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
	const UCrowdySDKDeveloperSettings* DeveloperSettings = GetDefault<UCrowdySDKDeveloperSettings>();
	if (!DeveloperSettings)
		return false;

	const UWorld* World = GetWorld();
	if (!IsValid(World))
		return false;

	FString CurrentMap = FPackageName::GetShortName(World->GetOutermost()->GetName());

#if WITH_EDITOR
	if (World->WorldType == EWorldType::Editor || World->WorldType == EWorldType::PIE)
		CurrentMap = World->RemovePIEPrefix(CurrentMap);
#endif

	for (const auto& [WorldPtr, bAllowed] : DeveloperSettings->HostSubsystemAllowedMaps)
	{
		if (WorldPtr.IsNull())
			continue;

		if (FPackageName::GetShortName(WorldPtr.GetAssetName()) != CurrentMap)
			continue;

		return bAllowed;
	}

	return false;
}