#include "Subsystem/CrowdyHostSubsystem.h"
#include "CrowdyServiceApiSupport.h"
#include "CrowdyCppClient.h"
#include "CrowdyServicesLog.h"
#include "Messages/GameObjects/FGameEventNotification.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Utils/HelperFunctions.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Engine/GameInstance.h"
#include "Dom/JsonObject.h"

using namespace CrowdyServiceApi;

namespace
{
	constexpr const TCHAR* HostLogName = TEXT("CrowdyHostSubsystem");
}

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
	// The game session outlives every world, so a binding left behind here accumulates one dead entry per level
	// travel for the rest of the session.
	if (IsValid(GameSession))
	{
		GameSession->OnOwnerUUIDUpdated.RemoveDynamic(this, &UCrowdyHostSubsystem::OnOwnerPlayerIDSet);
	}

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

	// This is callable from any thread and the hop to the game thread costs at least a frame, which is long enough
	// for a level travel to destroy this world-scoped subsystem, so it is re-resolved rather than captured.
	TWeakObjectPtr<UCrowdyHostSubsystem> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread, [WeakThis, InHostUserID, OldHostUserID]()
	{
		UCrowdyHostSubsystem* Self = WeakThis.Get();
		if (!IsValid(Self)) return;

		const FGuid PreviousHostID = UHelperFunctions::GetDeterministicID(OldHostUserID);
		const FGuid HostID = UHelperFunctions::GetDeterministicID(InHostUserID);

		if (IsValid(Self->GameSession))
			Self->GameSession->SetHostID(HostID);

		Self->OnHostElected.Broadcast(HostID, PreviousHostID);
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

void UCrowdyHostSubsystem::CheckEntityIsHost(const AActor* Entity, TFunction<void(bool bSuccess, bool bIsHost)> Callback)
{
	if (!Callback) return;

	UWorld* World = GetWorld();
	UCrowdyEntitySubsystem* EntitySubsystem = World ? World->GetSubsystem<UCrowdyEntitySubsystem>() : nullptr;
	if (!IsValid(Entity) || !IsValid(EntitySubsystem))
	{
		UE_LOG(LogCrowdyServices, Warning, TEXT("[HostSubsystem]: CheckEntityIsHost — entity or entity subsystem invalid."));
		Callback(false, false);
		return;
	}

	const FGuid EntityNetID = EntitySubsystem->FindEntityID(Entity);
	if (!EntityNetID.IsValid())
	{
		UE_LOG(LogCrowdyServices, Warning, TEXT("[HostSubsystem]: CheckEntityIsHost — '%s' is not a registered Crowdy entity."),
			*GetNameSafe(Entity));
		Callback(false, false);
		return;
	}

	// A player avatar's NetID equals its owner's deterministic user GUID, so an entity whose
	// NetID matches the local player's id is our own avatar: the one case amIGameHost answers
	// authoritatively for this client.
	const FGuid LocalPlayerNetID = EntitySubsystem->GetLocalPlayerID();
	if (EntityNetID == LocalPlayerNetID)
	{
		RequestAmIGameHost([Callback = MoveTemp(Callback)](bool bSuccess, bool bAmHost)
		{
			Callback(bSuccess, bSuccess && bAmHost);
		});
		return;
	}

	// Any other actor: ask the server who owns it, then compare to the elected host userId.
	const FString Uuid = EntityNetID.ToString(EGuidFormats::Digits);
	TWeakObjectPtr<UCrowdyHostSubsystem> WeakThis(this);
	RequestActorOwner(Uuid, [WeakThis, Callback = MoveTemp(Callback)](bool bSuccess, int64 OwnerUserId)
	{
		if (!bSuccess)
		{
			Callback(false, false);
			return;
		}

		// A host id of zero is "nobody has been elected yet", and a subsystem that is gone cannot answer at all.
		// Neither is the same as "this actor is not the host", and the caller has a separate branch for the
		// undetermined case precisely so it can wait or retry rather than act on a false negative.
		UCrowdyHostSubsystem* Self = WeakThis.Get();
		const int64 HostUid = Self ? Self->GetHostUserID() : 0;
		Callback(HostUid != 0, OwnerUserId == HostUid);
	});
}

void UCrowdyHostSubsystem::RequestAmIGameHost(TFunction<void(bool bSuccess, bool bAmHost)> Callback)
{
	const UWorld* World = GetWorld();
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;

	FCrowdyCppClient* Client = ResolveApiClient(GameInstance, HostLogName);
	if (!Client || !IsValid(GameSession))
	{
		Callback(false, false);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("appId"), BigInt(GameSession->GetAppID()));

	Client->RunOp(ECrowdyCppApiDomain::Host, TEXT("AmIGameHost"), Variables,
		[Callback = MoveTemp(Callback)](FCrowdyCppJsonResult Result)
		{
			bool bAmHost = false;
			const bool bSuccess = ReadAmIGameHost(Result, bAmHost);
			Callback(bSuccess, bAmHost);
		});
}

void UCrowdyHostSubsystem::RequestActorOwner(const FString& Uuid, TFunction<void(bool bSuccess, int64 UserId)> Callback)
{
	const UWorld* World = GetWorld();
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;

	FCrowdyCppClient* Client = ResolveApiClient(GameInstance, HostLogName);
	if (!Client)
	{
		Callback(false, 0);
		return;
	}

	TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetStringField(TEXT("uuid"), Uuid);

	Client->RunOp(ECrowdyCppApiDomain::Actors, TEXT("Actor"), Variables,
		[Uuid, Callback = MoveTemp(Callback)](FCrowdyCppJsonResult Result)
		{
			int64 OwnerUserId = 0;
			const bool bSuccess = ReadActorOwner(Result, Uuid, OwnerUserId);
			Callback(bSuccess, OwnerUserId);
		});
}
