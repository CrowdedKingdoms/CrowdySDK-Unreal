#include "Subsystem/CrowdyPersistenceSubsystem.h"

#include "CrowdyServicesLog.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Engine/LatentActionManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Async/Async.h"
#include "Core/CrowdySDKBridgeSubsystem.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Network/UDP/CrowdyUDPSubsystem.h"
#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"
#include "Core/Persistence/FCrowdyPullStateLatentAction.h"
#include "Core/GraphQL/Enums/EQueryResponseType.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Messages/Voxel/FVoxelStateUpdateRequest.h"
#include "Messages/Voxel/FVoxelUpdateNotificationMessage.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Utils/CrowdyBakedRegistry.h"
#include "Queries/Data/Persistence/FPersistencePullRequest.h"
#include "Queries/Data/Persistence/FPersistencePullResponse.h"

// ─── ICrowdyPersistentOwner default implementation ────────────────────────────

FString ICrowdyPersistentOwner::GetPersistentKey_Implementation() const
{
	if (const UObject* Self = Cast<UObject>(this))
		return Self->GetPathName();
	return TEXT("Unknown");
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

void UCrowdyPersistenceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void UCrowdyPersistenceSubsystem::Deinitialize()
{
	// Auto-clear must run before timers are cleared and before subsystem refs
	// are nulled, so it's the very first thing in deinit.
	const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
	if (Settings && Settings->bAutoClearPersistentStateOnShutdown)
		ClearAllState();

	if (UWorld* World = GetWorld())
		World->GetTimerManager().ClearTimer(VerificationTickHandle);

	PendingPulls.Empty();
	InFlightPulls.Empty();
	CallbackPulls.Empty();
	PushQueue.Empty();
	PendingVerifications.Empty();
	TrackedPushAddresses.Empty();
	TypeRegistry.Empty();
	Super::Deinitialize();
}

void UCrowdyPersistenceSubsystem::InitialSetup(UCrowdySDKBridgeSubsystem* Bridge, UCrowdyGameSession* Session)
{
	CachedBridge  = Bridge;
	CachedSession = Session;

	if (!IsValid(CachedBridge))
	{
		UE_LOG(LogCrowdyServices, Error, TEXT("[CrowdyPersistence]: Bridge Reference is null. "))
		return;
	}

	// Register for GraphQL pull responses
	if (CachedBridge->DataRegistry)
		CachedBridge->DataRegistry->RegisterLayer(this);

	// Register for UDP VOXEL_UPDATE_NOTIFICATION (push confirmation)
	if (CachedBridge->ServiceRegistry)
		CachedBridge->ServiceRegistry->RegisterReceptionLayer(this);
	
	if (UCrowdyUDPSubsystem* UDP = GetGameInstance()->GetSubsystem<UCrowdyUDPSubsystem>();
		IsValid(UDP))
	{
		UDP->OnUDPConnectionSuccessful.AddDynamic(this, &UCrowdyPersistenceSubsystem::OnUDPConnected);
	}

	ScanAndRegisterStructs();

	UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log, TEXT("[CrowdyPersistence] Initialised — %d type(s) registered."),
		TypeRegistry.Num());
}

// ─── Auto-registration ────────────────────────────────────────────────────────

void UCrowdyPersistenceSubsystem::ScanAndRegisterStructs()
{
	for (TObjectIterator<UScriptStruct> It; It; ++It)
		RegisterPersistentStruct(*It);

	const IAssetRegistry& AR =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FAssetData> StructAssets;
	AR.GetAssetsByClass(FTopLevelAssetPath(UUserDefinedStruct::StaticClass()), StructAssets);

	for (const FAssetData& Asset : StructAssets)
	{
		if (!Asset.TagsAndValues.FindTag(FName("CrowdyPersistent")).IsSet())
			continue;
		if (UScriptStruct* S = Cast<UScriptStruct>(Asset.GetAsset()))
			RegisterPersistentStruct(S);
	}
}

void UCrowdyPersistenceSubsystem::RegisterPersistentStruct(UScriptStruct* Struct)
{
	if (!IsValid(Struct) || !UCrowdyBakedRegistry::IsPersistentStruct(Struct))
		return;

	const FString PathName = Struct->GetPathName();
	if (TypeRegistry.Contains(PathName)) return;

	const FCrowdyTypeID TypeID = FCrowdyTypeIDGenerator::GenerateFromStruct(Struct);
	if (TypeID == CROWDY_INVALID_TYPE_ID)
	{
		UE_LOG(LogCrowdyServices, Warning,
			TEXT("[CrowdyPersistence] Could not generate TypeID for '%s' — skipped."),
			*Struct->GetName());
		return;
	}

	FPersistenceTypeInfo Info;
	Info.ChunkX     = static_cast<int64>(TypeID);
	Info.bInstanced = !UCrowdyBakedRegistry::IsSingletonStruct(Struct);
	TypeRegistry.Add(PathName, Info);

	UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log,
		TEXT("[CrowdyPersistence] Registered '%s'  ChunkX=%lld  Instanced=%s"),
		*Struct->GetName(), Info.ChunkX, Info.bInstanced ? TEXT("yes") : TEXT("no"));
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

FString UCrowdyPersistenceSubsystem::DeriveInstanceKey(const UObject* Subject)
{
	if (!IsValid(Subject)) return FString();

	// ── Priority 1: explicit override ─────────────────────────────────────
	// ICrowdyPersistentOwner::GetPersistentKey() gives full control when needed.
	if (Subject->GetClass()->ImplementsInterface(UCrowdyPersistentOwner::StaticClass()))
		return ICrowdyPersistentOwner::Execute_GetPersistentKey(const_cast<UObject*>(Subject));

	if (const AActor* Actor = Cast<const AActor>(Subject))
	{
		// ── Priority 2: locally controlled / locally spawned actor ────────
		// CrowdyEntityComponent is present on locally controlled players and
		// locally spawned characters (bots, etc.). Its NetID is derived from
		// the session UUID so it matches the pool ID on all other clients.
		if (const UCrowdyEntityComponent* Comp =
			Actor->FindComponentByClass<UCrowdyEntityComponent>())
		{
			const FGuid ID = Comp->GetNetID();
			if (ID.IsValid())
				return ID.ToString(EGuidFormats::Digits);
		}

		if (const UWorld* World = Actor->GetWorld())
		{
			// ── Priority 3: entity / replicated proxy (actor pool) ───────────
			// Pool actors are registered in CrowdyEntitySubsystem since Phase 8.
			if (UCrowdyEntitySubsystem* EntitySubsystem = World->GetSubsystem<UCrowdyEntitySubsystem>())
			{
				const FGuid ID = EntitySubsystem->FindEntityID(Actor);
				if (ID.IsValid())
					return ID.ToString(EGuidFormats::Digits);
			}
		}
	}

	// ── Priority 5: stable path name fallback ─────────────────────────────
	// Works for level-placed actors not registered with any of the above systems.
	// Breaks if the actor is renamed in the editor between sessions.
	return Subject->GetPathName();
}

int16 UCrowdyPersistenceSubsystem::HashInstanceKey(const FString& Key)
{
	if (Key.IsEmpty()) return 0; // singleton slot

	const uint32 Hash = FCrc::MemCrc32(*Key, Key.Len() * sizeof(TCHAR));
	return static_cast<int16>((Hash % 32767u) + 1u); // 1..32767; 0 reserved for singletons
}

TArray<uint8> UCrowdyPersistenceSubsystem::SerializeStruct(UScriptStruct* Struct, const void* SrcData)
{
	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes, true);
	Struct->SerializeTaggedProperties(Writer,
		const_cast<uint8*>(static_cast<const uint8*>(SrcData)), nullptr, nullptr);
	return Bytes;
}

int64 UCrowdyPersistenceSubsystem::MakeVerifyKey(int64 ChunkX, int16 Vx)
{
	return (static_cast<int64>(static_cast<uint16>(ChunkX)) << 16) |
	        static_cast<int64>(static_cast<uint16>(Vx));
}

// ─── Push ─────────────────────────────────────────────────────────────────────

void UCrowdyPersistenceSubsystem::InternalPushState(UScriptStruct* Struct, const void* SrcData, UObject* Subject)
{
	if (!Struct || !SrcData) return;

	const FPersistenceTypeInfo* TypeInfo = TypeRegistry.Find(Struct->GetPathName());
	if (!TypeInfo)
	{
		UE_LOG(LogCrowdyServices, Warning,
			TEXT("[CrowdyPersistence] PushState: '%s' is not registered. "
			     "Add meta=(CrowdyPersistent) to the struct."), *Struct->GetName());
		return;
	}

	const FString InstanceKey = TypeInfo->bInstanced ? DeriveInstanceKey(Subject) : FString();
	const int16   Vx          = TypeInfo->bInstanced ? HashInstanceKey(InstanceKey) : 0;
	TArray<uint8> Bytes       = SerializeStruct(Struct, SrcData);

	if (Bytes.IsEmpty())
	{
		UE_LOG(LogCrowdyServices, Warning,
			TEXT("[CrowdyPersistence] PushState: Serialization of '%s' produced empty bytes."),
			*Struct->GetName());
		return;
	}

	if (Bytes.Num() > MAX_uint16)
	{
		UE_LOG(LogCrowdyServices, Error,
			TEXT("[CrowdyPersistence] PushState: '%s' is %d bytes — exceeds UDP limit (%d). Not pushed."),
			*Struct->GetName(), Bytes.Num(), static_cast<int32>(MAX_uint16));
		return;
	}

	if (!DispatchUDPPush(Struct, Bytes, TypeInfo->ChunkX, Vx))
	{
		FQueuedPush Q;
		Q.StructType      = Struct;
		Q.SerializedBytes = Bytes;
		Q.ChunkX          = TypeInfo->ChunkX;
		Q.Vx              = Vx;
		PushQueue.Add(MoveTemp(Q));

		UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log,
			TEXT("[CrowdyPersistence] PushState: UDP not ready — queued '%s' (queue=%d)."),
			*Struct->GetName(), PushQueue.Num());
		return;
	}

	// Push sent — register for post-push verification
	RegisterVerification(Struct, Bytes, TypeInfo->ChunkX, Vx);

	UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log,
		TEXT("[CrowdyPersistence] PushState: Sent '%s' via UDP (ChunkX=%lld, Vx=%d, %d bytes)."),
		*Struct->GetName(), TypeInfo->ChunkX, Vx, Bytes.Num());
}

bool UCrowdyPersistenceSubsystem::DispatchUDPPush(UScriptStruct* StructType,
                                                    const TArray<uint8>& Bytes,
                                                    int64 ChunkX, int16 Vx)
{
	if (!IsValid(CachedBridge))
		return false;
	if (!IsValid(CachedSession))
		return false;

	const FCrowdyTypeID TypeID = FCrowdyTypeIDGenerator::GenerateFromStruct(StructType);

	FVoxelStateUpdateRequest Msg;
	Msg.UUID                = CachedSession->GetUUID();
	Msg.AppID               = CachedSession->GetAppID();
	Msg.ChunkX              = ChunkX;
	Msg.ChunkY              = 0;
	Msg.ChunkZ              = 0;
	Msg.Vx                  = Vx;
	Msg.Vy                  = 0;
	Msg.Vz                  = 0;
	Msg.VoxelType           = static_cast<int16>(TypeID);
	Msg.bContainsState      = true;
	Msg.StateBytes          = Bytes;
	Msg.StateSize           = static_cast<uint16>(Bytes.Num());
	Msg.bContainsAuth       = true;
	Msg.DecayRate           = ECrowdyDecayRate::Exponential_Decay;
	Msg.ReplicationDistance = ECrowdyReplicationDistance::One_Chunk;

	if (CachedBridge->SendMessageFn) CachedBridge->SendMessageFn(Msg);

	// Track so ClearAllState() knows which slots to zero out later.
	TrackedPushAddresses.Add(MakeVerifyKey(ChunkX, Vx));

	return true;
}

void UCrowdyPersistenceSubsystem::FlushPushQueue()
{
	if (PushQueue.IsEmpty()) return;

	UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log, TEXT("[CrowdyPersistence] Flushing %d queued push(es)."), PushQueue.Num());

	TArray<FQueuedPush> Remaining;
	for (FQueuedPush& Item : PushQueue)
	{
		if (DispatchUDPPush(Item.StructType, Item.SerializedBytes, Item.ChunkX, Item.Vx))
			RegisterVerification(Item.StructType, Item.SerializedBytes, Item.ChunkX, Item.Vx);
		else
			Remaining.Add(MoveTemp(Item));
	}
	PushQueue = MoveTemp(Remaining);
}

void UCrowdyPersistenceSubsystem::OnUDPConnected()
{
	FlushPushQueue();
}

// ─── Clear ────────────────────────────────────────────────────────────────────

void UCrowdyPersistenceSubsystem::ClearAllState()
{
	if (!IsValid(CachedBridge) || !IsValid(CachedSession))
	{
		UE_LOG(LogCrowdyServices, Warning,
			TEXT("[CrowdyPersistence] ClearAllState: Bridge or session not ready — cannot send clear messages."));
		return;
	}

	if (const UCrowdyUDPSubsystem* UDP = GetGameInstance() ? GetGameInstance()->GetSubsystem<UCrowdyUDPSubsystem>() : nullptr;
		!UDP || UDP->GetConnectionState() != EUDPConnectionState::Connected)
	{
		UE_LOG(LogCrowdyServices, Warning,
			TEXT("[CrowdyPersistence] ClearAllState: UDP not connected — "
			     "clear messages will not be delivered. Call this before disconnecting."));
	}

	UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log,
		TEXT("[CrowdyPersistence] ClearAllState: Zeroing %d tracked slot(s)."),
		TrackedPushAddresses.Num());

	for (const int64 Key : TrackedPushAddresses)
	{
		const int64 ChunkX = (Key >> 16) & 0xFFFF;
		const int16 Vx     = static_cast<int16>(Key & 0xFFFF);
		SendClearMessage(ChunkX, Vx);
	}

	// Cancel everything in-flight — state is being wiped.
	TrackedPushAddresses.Empty();
	PendingVerifications.Empty();
	PushQueue.Empty();

	if (UWorld* World = GetWorld())
		World->GetTimerManager().ClearTimer(VerificationTickHandle);

	UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log, TEXT("[CrowdyPersistence] ClearAllState: Done."));
}

void UCrowdyPersistenceSubsystem::SendClearMessage(int64 ChunkX, int16 Vx) const
{
	// VoxelType = 0 signals the server to treat this slot as empty.
	FVoxelStateUpdateRequest Msg;
	Msg.UUID                = CachedSession->GetUUID();
	Msg.AppID               = CachedSession->GetAppID();
	Msg.ChunkX              = ChunkX;
	Msg.ChunkY              = 0;
	Msg.ChunkZ              = 0;
	Msg.Vx                  = Vx;
	Msg.Vy                  = 0;
	Msg.Vz                  = 0;
	Msg.VoxelType           = 0;     // ← zero = clear this slot
	Msg.bContainsState      = false;
	Msg.StateSize           = 0;
	Msg.bContainsAuth       = true;
	Msg.DecayRate           = ECrowdyDecayRate::No_Decay;
	Msg.ReplicationDistance = ECrowdyReplicationDistance::Eight_Chunks;

	if (CachedBridge && CachedBridge->SendMessageFn) CachedBridge->SendMessageFn(Msg);
}

// ─── Verify-retry ─────────────────────────────────────────────────────────────

void UCrowdyPersistenceSubsystem::RegisterVerification(UScriptStruct* StructType,
                                                        const TArray<uint8>& Bytes,
                                                        int64 ChunkX, int16 Vx)
{
	const FCrowdyTypeID TypeID = FCrowdyTypeIDGenerator::GenerateFromStruct(StructType);
	const int64 Key = MakeVerifyKey(ChunkX, Vx);

	FPendingPushVerification V;
	V.ChunkX       = ChunkX;
	V.Vx           = Vx;
	V.VoxelType    = static_cast<int16>(TypeID);
	V.PushedBytes  = Bytes;
	V.StructType   = StructType;
	V.RetryCount   = 0;
	V.LastPushTime = FPlatformTime::Seconds();
	PendingVerifications.Add(Key, MoveTemp(V));

	// Ensure the verification tick is running
	if (!VerificationTickHandle.IsValid())
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(
				VerificationTickHandle, this,
				&UCrowdyPersistenceSubsystem::VerificationTick,
				PushVerifyDelaySeconds, /*bLoop=*/true);
		}
	}
}

void UCrowdyPersistenceSubsystem::VerificationTick()
{
	if (PendingVerifications.IsEmpty())
	{
		if (UWorld* World = GetWorld())
			World->GetTimerManager().ClearTimer(VerificationTickHandle);
		return;
	}

	// Notification-based confirmation: VOXEL_UPDATE_NOTIFICATION removes entries
	// immediately when received. This tick only handles the timeout case — pushes
	// that did not receive a notification within PushVerifyDelaySeconds.
	const double Now = FPlatformTime::Seconds();
	TArray<int64> ToRemove;

	for (auto& Pair : PendingVerifications)
	{
		FPendingPushVerification& V = Pair.Value;

		if (Now - V.LastPushTime < PushVerifyDelaySeconds)
			continue; // still within the confirmation window — wait

		if (V.RetryCount < MaxPushRetries)
		{
			++V.RetryCount;
			V.LastPushTime = Now; // reset window for this retry
			UE_LOG(LogCrowdyServices, Warning,
				TEXT("[CrowdyPersistence] No confirmation received — retrying '%s' "
				     "(attempt %d/%d, ChunkX=%lld, Vx=%d)."),
				V.StructType ? *V.StructType->GetName() : TEXT("?"),
				V.RetryCount, MaxPushRetries, V.ChunkX, V.Vx);
			DispatchUDPPush(V.StructType, V.PushedBytes, V.ChunkX, V.Vx);
		}
		else
		{
			UE_LOG(LogCrowdyServices, Error,
				TEXT("[CrowdyPersistence] Push failed after %d retries — '%s' (Vx=%d). Giving up."),
				MaxPushRetries,
				V.StructType ? *V.StructType->GetName() : TEXT("?"), V.Vx);
			ToRemove.Add(Pair.Key);
		}
	}

	for (int64 Key : ToRemove)
		PendingVerifications.Remove(Key);
}

// ─── Pull (latent action / Blueprint path) ────────────────────────────────────

void UCrowdyPersistenceSubsystem::InternalPullState(const FLatentActionInfo& LatentInfo,
                                                     UObject* Subject,
                                                     UScriptStruct* StructType,
                                                     void* OutAddr,
                                                     EPullStateResult& Result)
{
	if (!StructType || !OutAddr)
	{
		Result = EPullStateResult::OnFailure;
		return;
	}

	const FPersistenceTypeInfo* TypeInfo = TypeRegistry.Find(StructType->GetPathName());
	if (!TypeInfo)
	{
		UE_LOG(LogCrowdyServices, Warning,
			TEXT("[CrowdyPersistence] PullState: '%s' is not registered."), *StructType->GetName());
		Result = EPullStateResult::OnFailure;
		return;
	}

	UWorld* World = GetWorld();
	if (!World) { Result = EPullStateResult::OnFailure; return; }

	FLatentActionManager& LAM = World->GetLatentActionManager();
	if (LAM.FindExistingAction<FCrowdyPullStateLatentAction>(
		LatentInfo.CallbackTarget, LatentInfo.UUID))
	{
		return; // already in flight for this BP node
	}

	const FString InstanceKey = TypeInfo->bInstanced ? DeriveInstanceKey(Subject) : FString();
	const int16   TargetVx    = TypeInfo->bInstanced ? HashInstanceKey(InstanceKey) : 0;

	auto* Action = new FCrowdyPullStateLatentAction(
		LatentInfo, StructType, OutAddr, Result, Subject, InstanceKey);
	LAM.AddNewAction(LatentInfo.CallbackTarget, LatentInfo.UUID, Action);

	FPendingPullOp Op;
	Op.InstanceKey    = InstanceKey;
	Op.TargetVx       = TargetVx;
	Op.LatentUUID     = LatentInfo.UUID;
	Op.CallbackTarget = LatentInfo.CallbackTarget;
	PendingPulls.FindOrAdd(TypeInfo->ChunkX).Add(MoveTemp(Op));

	FirePullQuery(TypeInfo->ChunkX);
}

// ─── Pull (callback / C++ path) ───────────────────────────────────────────────

void UCrowdyPersistenceSubsystem::InternalPullStateCallback(
	UScriptStruct* StructType, UObject* Subject,
	TFunction<void(bool, const TArray<uint8>&, UScriptStruct*)> Callback)
{
	if (!StructType || !Callback) return;

	const FPersistenceTypeInfo* TypeInfo = TypeRegistry.Find(StructType->GetPathName());
	if (!TypeInfo)
	{
		UE_LOG(LogCrowdyServices, Warning,
			TEXT("[CrowdyPersistence] PullState (callback): '%s' is not registered."),
			*StructType->GetName());
		Callback(false, {}, StructType);
		return;
	}

	const FString InstanceKey = TypeInfo->bInstanced ? DeriveInstanceKey(Subject) : FString();
	const int16   TargetVx    = TypeInfo->bInstanced ? HashInstanceKey(InstanceKey) : 0;

	FCallbackPullOp Op;
	Op.InstanceKey = InstanceKey;
	Op.TargetVx    = TargetVx;
	Op.StructType  = StructType;
	Op.Callback    = MoveTemp(Callback);
	CallbackPulls.FindOrAdd(TypeInfo->ChunkX).Add(MoveTemp(Op));

	FirePullQuery(TypeInfo->ChunkX);
}

// ─── Query dispatch ───────────────────────────────────────────────────────────

void UCrowdyPersistenceSubsystem::FirePullQuery(int64 ChunkX)
{
	if (InFlightPulls.Contains(ChunkX)) return;
	if (!IsValid(CachedBridge) || !CachedBridge->ExecuteQueryFn || !IsValid(CachedSession))
	{
		UE_LOG(LogCrowdyServices, Warning, TEXT("[CrowdyPersistence] Cannot fire pull query — SDK not ready."));
		return;
	}

	InFlightPulls.Add(ChunkX);

	FPersistencePullRequest Req;
	Req.AppID  = CachedSession->GetAppID();
	Req.ChunkX = ChunkX;
	Req.ChunkY = 0;
	Req.ChunkZ = 0;
	Req.PrepareQuery();
	CachedBridge->ExecuteQueryFn(Req);

	UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log, TEXT("[CrowdyPersistence] Fired pull query for ChunkX=%lld."), ChunkX);
}

// ─── ICrowdyQueryReceptionLayer ───────────────────────────────────────────────

void UCrowdyPersistenceSubsystem::OnResponseReceived(TSharedPtr<ICrowdyQueryResponse> Response)
{
	TWeakObjectPtr<UCrowdyPersistenceSubsystem> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread, [WeakThis, Response]()
	{
		UCrowdyPersistenceSubsystem* Self = WeakThis.Get();
		if (!IsValid(Self) || !Response.IsValid()) return;

		if (Response->GetResponseType() == EQueryResponseType::PersistencePull)
			Self->HandlePullResponse(static_cast<FPersistencePullResponse&>(*Response));
	});
}

TArray<EQueryResponseType> UCrowdyPersistenceSubsystem::GetSupportedResponseType() const
{
	return { EQueryResponseType::PersistencePull };
}

// ── ICrowdyReceptionLayer (UDP push confirmations) ────────────────────────────

void UCrowdyPersistenceSubsystem::OnMessageReceived(TSharedRef<ICrowdyMessage> Message)
{
	if (Message->GetType() != ECrowdyMessageType::VOXEL_UPDATE_NOTIFICATION)
		return;

	const FVoxelUpdateNotificationMessage& Notif =
		static_cast<const FVoxelUpdateNotificationMessage&>(Message.Get());

	const int64 Key = MakeVerifyKey(Notif.ChunkX, Notif.Vx);

	AsyncTask(ENamedThreads::GameThread, [this, Key, Notif]()
	{
		// No authority check here — PendingVerifications only ever contains entries
		// this client registered after its own pushes, so any matching notification
		// is implicitly ours. Every client verifies its own state independently.
		if (FPendingPushVerification* V = PendingVerifications.Find(Key))
		{
			// Confirm only if VoxelType matches (guards against unrelated voxel updates
			// in the same chunk from other systems).
			if (V->VoxelType == Notif.VoxelType)
			{
				UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log,
					TEXT("[CrowdyPersistence] Push confirmed via VOXEL_UPDATE_NOTIFICATION "
						 "— '%s' (ChunkX=%lld, Vx=%d)."),
					V->StructType ? *V->StructType->GetName() : TEXT("?"),
					Notif.ChunkX, Notif.Vx);
				PendingVerifications.Remove(Key);
			}
		}
	});
	
}

TArray<ECrowdyMessageType> UCrowdyPersistenceSubsystem::GetSupportedResponseTypes() const
{
	return { ECrowdyMessageType::VOXEL_UPDATE_NOTIFICATION };
}

// ─── Response handling ────────────────────────────────────────────────────────

void UCrowdyPersistenceSubsystem::HandlePullResponse(const FPersistencePullResponse& Response)
{
	const int64 ChunkX = Response.ChunkX;
	InFlightPulls.Remove(ChunkX);

	// ── Latent-action (Blueprint) pending ops ─────────────────────────────
	if (TArray<FPendingPullOp>* Ops = PendingPulls.Find(ChunkX))
	{
		UWorld* World = GetWorld();
		FLatentActionManager* LAM = World ? &World->GetLatentActionManager() : nullptr;

		for (const FPendingPullOp& Op : *Ops)
		{
			if (!LAM) continue;

			FCrowdyPullStateLatentAction* Action =
				LAM->FindExistingAction<FCrowdyPullStateLatentAction>(
					Op.CallbackTarget.Get(), Op.LatentUUID);
			if (!Action) continue;

			if (!Response.IsValid())
			{
				Action->bSuccess = false;
			}
			else if (const TArray<uint8>* Bytes = Response.VoxelStateBytes.Find(Op.TargetVx))
			{
				Action->ResultBytes = *Bytes;
				Action->bSuccess    = true;
			}
			else
			{
				Action->bSuccess = false;
			}
			Action->bCompleted = true;
		}
		PendingPulls.Remove(ChunkX);
	}

	// ── Callback (C++) pending ops ────────────────────────────────────────
	if (TArray<FCallbackPullOp>* Ops = CallbackPulls.Find(ChunkX))
	{
		for (FCallbackPullOp& Op : *Ops)
		{
			if (!Response.IsValid())
			{
				Op.Callback(false, {}, Op.StructType);
				continue;
			}
			if (const TArray<uint8>* Bytes = Response.VoxelStateBytes.Find(Op.TargetVx))
				Op.Callback(true, *Bytes, Op.StructType);
			else
				Op.Callback(false, {}, Op.StructType);
		}
		CallbackPulls.Remove(ChunkX);
	}

}

// ─── Blueprint custom thunks ──────────────────────────────────────────────────

DEFINE_FUNCTION(UCrowdyPersistenceSubsystem::execK2_PushState)
{
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.MostRecentProperty        = nullptr;
	Stack.StepCompiledIn<FStructProperty>(nullptr);
	const FStructProperty* StructProp = CastField<FStructProperty>(Stack.MostRecentProperty);
	const void*            SrcAddr    = Stack.MostRecentPropertyAddress;

	P_GET_OBJECT(UObject, Subject);
	P_FINISH;

	P_NATIVE_BEGIN;
	if (StructProp && SrcAddr)
		P_THIS->InternalPushState(StructProp->Struct, SrcAddr, Subject);
	P_NATIVE_END;
}

DEFINE_FUNCTION(UCrowdyPersistenceSubsystem::execK2_PullState)
{
	P_GET_STRUCT(FLatentActionInfo, LatentInfo);
	P_GET_OBJECT(UObject, Subject);

	Stack.MostRecentPropertyAddress = nullptr;
	Stack.MostRecentProperty        = nullptr;
	Stack.StepCompiledIn<FStructProperty>(nullptr);
	FStructProperty* OutStructProp = CastField<FStructProperty>(Stack.MostRecentProperty);
	void*            OutAddr       = Stack.MostRecentPropertyAddress;

	P_GET_ENUM_REF(EPullStateResult, Result);
	P_FINISH;

	P_NATIVE_BEGIN;
	if (OutStructProp && OutAddr)
		P_THIS->InternalPullState(LatentInfo, Subject, OutStructProp->Struct, OutAddr, Result);
	else
		Result = EPullStateResult::OnFailure;
	P_NATIVE_END;
}
