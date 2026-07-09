#include "Subsystem/CrowdyHostSubsystem.h"
#include "CrowdyServicesLog.h"
#include "Messages/GameObjects/FGameEventNotification.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Utils/HelperFunctions.h"
#include "Network/GraphQL/CrowdyQuerySubsystem.h"
#include "Core/GraphQL/Enums/EGraphQLQuery.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Engine/GameInstance.h"
#include "Dom/JsonObject.h"

namespace HostQueries
{
	// Boolean "is the authenticated caller the host". App-scoped game token (descriptor).
	static const TCHAR* AmIGameHost =
		TEXT("query AmIGameHost($appId: BigInt!) { amIGameHost(appId: $appId) }");

	// Resolve an actor's server-side owner userId by its 32-char wire uuid. The uuid is
	// echoed back so the answer can be correlated to the request that asked for it.
	static const TCHAR* ActorOwner =
		TEXT("query ActorOwner($uuid: String!) { actor(uuid: $uuid) { uuid userId } }");
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

// ─── Server-validated host check ──────────────────────────────────────────────

UCrowdyQuerySubsystem* UCrowdyHostSubsystem::ResolveQuerySubsystem() const
{
	const UWorld* World = GetWorld();
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetSubsystem<UCrowdyQuerySubsystem>() : nullptr;
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
	// NetID matches the local player's id is our own avatar — the one case amIGameHost answers
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

		UCrowdyHostSubsystem* Self = WeakThis.Get();
		const int64 HostUid = Self ? Self->GetHostUserID() : 0;
		Callback(true, HostUid != 0 && OwnerUserId == HostUid);
	});
}

void UCrowdyHostSubsystem::RequestAmIGameHost(TFunction<void(bool bSuccess, bool bAmHost)> Callback)
{
	UCrowdyQuerySubsystem* QuerySubsystem = ResolveQuerySubsystem();
	if (!IsValid(QuerySubsystem) || !IsValid(GameSession))
	{
		Callback(false, false);
		return;
	}

	PendingAmIHostCallbacks.Add(MoveTemp(Callback));

	const TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("appId"), FString::Printf(TEXT("%lld"), GameSession->GetAppID()));
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::AmIGameHost, HostQueries::AmIGameHost, Vars);
}

void UCrowdyHostSubsystem::RequestActorOwner(const FString& Uuid, TFunction<void(bool bSuccess, int64 UserId)> Callback)
{
	UCrowdyQuerySubsystem* QuerySubsystem = ResolveQuerySubsystem();
	if (!IsValid(QuerySubsystem))
	{
		Callback(false, 0);
		return;
	}

	PendingActorOwnerCallbacks.Add({ Uuid, MoveTemp(Callback) });

	const TSharedPtr<FJsonObject> Vars = MakeShared<FJsonObject>();
	Vars->SetStringField(TEXT("uuid"), Uuid);
	QuerySubsystem->ExecuteQueryWithBodyAndJsonVars(EGraphQLQuery::ActorOwner, HostQueries::ActorOwner, Vars);
}

void UCrowdyHostSubsystem::HandleAmIGameHostResponse(bool bSuccess, bool bAmHost)
{
	if (PendingAmIHostCallbacks.Num() == 0) return;

	TFunction<void(bool, bool)> Callback = MoveTemp(PendingAmIHostCallbacks[0]);
	PendingAmIHostCallbacks.RemoveAt(0, 1, EAllowShrinking::No);
	if (Callback) Callback(bSuccess, bAmHost);
}

void UCrowdyHostSubsystem::HandleActorOwnerResponse(bool bSuccess, const FString& Uuid, int64 UserId)
{
	if (PendingActorOwnerCallbacks.Num() == 0) return;

	int32 Index;
	if (bSuccess)
	{
		// A valid actor(uuid) response always echoes the uuid we asked for, so correlate by
		// it exactly. An unmatched success (e.g. a duplicate/late response whose request was
		// already resolved) must be dropped, never applied to a different actor's callback.
		Index = Uuid.IsEmpty() ? INDEX_NONE : PendingActorOwnerCallbacks.IndexOfByPredicate(
			[&Uuid](const FPendingActorOwnerCallback& Pending){ return Pending.Uuid == Uuid; });
		if (Index == INDEX_NONE)
			return;
	}
	else
	{
		// A failed response carries no uuid (it was marked invalid before parse), so there is
		// nothing to correlate on — resolve the oldest pending request as the failure.
		Index = 0;
	}

	TFunction<void(bool, int64)> Callback = MoveTemp(PendingActorOwnerCallbacks[Index].Callback);
	PendingActorOwnerCallbacks.RemoveAt(Index, 1, EAllowShrinking::No);
	if (Callback) Callback(bSuccess, UserId);
}