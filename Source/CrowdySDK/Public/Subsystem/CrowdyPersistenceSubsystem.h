#pragma once

#include "CoreMinimal.h"
#include "Engine/LatentActionManager.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryReceptionLayer.h"
#include "Core/UDP/Interfaces/ICrowdyReceptionLayer.h"
#include "Core/Persistence/EPullStateResult.h"
#include "Core/Persistence/ICrowdyPersistentOwner.h"
#include "CrowdyPersistenceSubsystem.generated.h"

class UCrowdySDKSubsystem;
class UCrowdyGameSession;
struct FPersistencePullResponse;
class FCrowdyPullStateLatentAction;

// ─── Wildcard struct placeholder ──────────────────────────────────────────────
// Used only as a compile-time pin-type marker for CustomThunk functions.
// Never instantiated directly.
USTRUCT(BlueprintInternalUseOnly)
struct CROWDYSDK_API FCrowdyWildcardStruct { GENERATED_BODY() };


/**
 * UCrowdyPersistenceSubsystem
 *
 * Generic persistent state storage using the server's voxel grid.
 *
 * ── Storage layout ────────────────────────────────────────────────────────────
 *   One chunk per struct TYPE:  ChunkX = TypeID,  ChunkY = 0,  ChunkZ = 0
 *   One voxel per INSTANCE:     Vx = CRC32(InstanceKey) % 32767 + 1
 *   Singleton (no instance):    Vx = 0
 *
 * ── Push / Pull ───────────────────────────────────────────────────────────────
 *   Push:  UDP (FVoxelStateUpdateRequest) — fast, low-latency.
 *          If UDP is not yet connected the push is queued and flushed on connect.
 *   Pull:  GraphQL getVoxelList — reads all instances for a struct type at once.
 *
 * ── Reliability ───────────────────────────────────────────────────────────────
 *   After each push the subsystem schedules a verification pull.
 *   If the voxel is not found on the server, the push is retried automatically
 *   (up to MaxPushRetries times, spaced PushVerifyDelaySeconds apart).
 */
UCLASS(meta=(DisplayName="Crowdy Persistence Subsystem"))
class CROWDYSDK_API UCrowdyPersistenceSubsystem
	: public UGameInstanceSubsystem
	, public ICrowdyQueryReceptionLayer
	, public ICrowdyReceptionLayer
{
	GENERATED_BODY()

public:

	// ── UGameInstanceSubsystem ────────────────────────────────────────────

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ── Blueprint API — Custom thunks ─────────────────────────────────────

	/**
	 * Push the struct's state to the server via UDP.
	 * Owner is used to derive the instance key (see ICrowdyPersistentOwner).
	 * Pass nullptr for singleton types.
	 */
	UFUNCTION(BlueprintCallable, CustomThunk, Category="Crowdy|Persistence",
		meta=(CustomStructureParam="State", AutoCreateRefTerm="State",
		      DisplayName="Push State"))
	void K2_PushState(const FCrowdyWildcardStruct& State, UObject* Owner);
	DECLARE_FUNCTION(execK2_PushState);

	/**
	 * Fetch a struct's state from the server asynchronously.
	 * OnSuccess fires with OutState populated; OnFailure fires if the slot is
	 * empty or a network error occurred.
	 */
	UFUNCTION(BlueprintCallable, CustomThunk, Category="Crowdy|Persistence",
		meta=(CustomStructureParam="OutState",
		      Latent, LatentInfo="LatentInfo",
		      ExpandEnumAsExecs="Result",
		      DisplayName="Pull State"))
	void K2_PullState(FLatentActionInfo LatentInfo, UObject* Owner,
	                  FCrowdyWildcardStruct& OutState, EPullStateResult& Result);
	DECLARE_FUNCTION(execK2_PullState);

	// ── C++ template API ──────────────────────────────────────────────────

	/** Push a typed struct. Owner = nullptr for singleton types. */
	template<typename T>
	void PushState(const T& Data, UObject* Owner = nullptr)
	{
		InternalPushState(T::StaticStruct(), &Data, Owner);
	}

	/** Pull a typed struct asynchronously. Callback fires on the game thread. */
	template<typename T>
	void PullState(UObject* Owner, TFunction<void(bool bSuccess, const T&)> Callback)
	{
		InternalPullStateCallback(T::StaticStruct(), Owner,
			[Callback = MoveTemp(Callback)](bool bOk, const TArray<uint8>& Bytes, UScriptStruct* Struct)
			{
				T Result{};
				if (bOk && Bytes.Num() > 0)
				{
					Struct->InitializeStruct(&Result);
					FMemoryReader Reader(Bytes, true);
					Struct->SerializeTaggedProperties(Reader, (uint8*)&Result, nullptr, nullptr);
				}
				Callback(bOk, Result);
			});
	}

	// ── State management ──────────────────────────────────────────────────

	/**
	 * Zero out every voxel slot written during this session.
	 * Sends a VoxelType=0 UDP message for each tracked address so the server
	 * marks those slots as empty.
	 *
	 * Also cancels any pending pushes and verifications.
	 *
	 * ⚠ UDP must be connected for the messages to reach the server.
	 *    Call this before logout/disconnect for guaranteed delivery.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy|Persistence")
	void ClearAllState();

	// ── ICrowdyQueryReceptionLayer (GraphQL pulls) ────────────────────────

	virtual void OnResponseReceived(TSharedPtr<ICrowdyQueryResponse> Response) override;
	virtual TArray<EQueryResponseType> GetSupportedResponseType() const override;

	// ── ICrowdyReceptionLayer (UDP push confirmations) ────────────────────

	/**
	 * Called when a VOXEL_UPDATE_NOTIFICATION arrives via UDP.
	 * If it matches a pending push verification, the push is considered
	 * confirmed and removed from the retry queue.
	 */
	virtual void OnMessageReceived(TSharedRef<ICrowdyMessage> Message) override;
	virtual TArray<ECrowdyMessageType> GetSupportedResponseTypes() const override;

private:

	// ── Timing constants ──────────────────────────────────────────────────

	/** Seconds after a push before the first verification pull fires. */
	static constexpr float PushVerifyDelaySeconds = 5.f;

	/** Maximum number of UDP retries before giving up on a push. */
	static constexpr int32 MaxPushRetries = 3;

	// ── Type registry ─────────────────────────────────────────────────────

	struct FPersistenceTypeInfo
	{
		/** ChunkX = TypeID. ChunkY and ChunkZ are always 0. */
		int64 ChunkX     = 0;
		/** True when the struct is tagged meta=(CrowdyPersistent, CrowdyInstanced). */
		bool  bInstanced = false;
	};

	/** Struct path → type info. Populated during Initialize(). */
	TMap<FString, FPersistenceTypeInfo> TypeRegistry;

	// ── Pending pull operations ───────────────────────────────────────────

	struct FPendingPullOp
	{
		FString          InstanceKey;
		int16            TargetVx       = 0;   // Vx to look for in the response
		int32            LatentUUID     = 0;
		FWeakObjectPtr   CallbackTarget;
	};

	/** ChunkX → pending latent pull ops. */
	TMap<int64, TArray<FPendingPullOp>> PendingPulls;

	/** ChunkXs for which a getVoxelList query is already in flight. */
	TSet<int64> InFlightPulls;

	// ── Callback-based pull ops (C++ API) ─────────────────────────────────

	struct FCallbackPullOp
	{
		FString        InstanceKey;
		int16          TargetVx  = 0;
		UScriptStruct* StructType = nullptr;
		TFunction<void(bool, const TArray<uint8>&, UScriptStruct*)> Callback;
	};

	TMap<int64, TArray<FCallbackPullOp>> CallbackPulls;

	// ── Queued UDP pushes (drained when UDP connects) ─────────────────────

	struct FQueuedPush
	{
		UScriptStruct* StructType      = nullptr;
		TArray<uint8>  SerializedBytes;
		int64          ChunkX          = 0;
		int16          Vx              = 0;
	};

	TArray<FQueuedPush> PushQueue;

	// ── Push verify-retry ─────────────────────────────────────────────────

	struct FPendingPushVerification
	{
		int64          ChunkX       = 0;
		int16          Vx           = 0;
		int16          VoxelType    = 0;
		TArray<uint8>  PushedBytes;
		UScriptStruct* StructType   = nullptr;
		int32          RetryCount   = 0;
		/** FPlatformTime::Seconds() at the last push attempt — for timeout detection. */
		double         LastPushTime = 0.0;
	};

	/**
	 * Key = MakeVerifyKey(ChunkX, Vx) — identifies the voxel slot being verified.
	 * After each push a record is added here; removed once confirmed or exhausted.
	 */
	TMap<int64, FPendingPushVerification> PendingVerifications;

	/** Periodic timer that fires pull queries for chunks with unconfirmed pushes. */
	FTimerHandle VerificationTickHandle;

	// ── Tracked push addresses ────────────────────────────────────────────

	/**
	 * Every voxel slot written this session, keyed by MakeVerifyKey(ChunkX, Vx).
	 * Used by ClearAllState() to know which slots to zero out.
	 * Populated on every DispatchUDPPush call (successful or not — clearing an
	 * unwritten slot is harmless).
	 */
	TSet<int64> TrackedPushAddresses;

	// ── Cached subsystem references ───────────────────────────────────────

	UPROPERTY()
	UCrowdySDKSubsystem* CachedSDK = nullptr;

	UPROPERTY()
	UCrowdyGameSession* CachedSession = nullptr;

	// ── Internal helpers ──────────────────────────────────────────────────

	void ScanAndRegisterStructs();
	void RegisterPersistentStruct(UScriptStruct* Struct);

	static FString DeriveInstanceKey(const UObject* Owner);

	/** CRC32 hash of Key → Vx slot (1..32767). Returns 0 for empty keys (singletons). */
	static int16 HashInstanceKey(const FString& Key);

	static TArray<uint8> SerializeStruct(UScriptStruct* Struct, const void* SrcData);

	/** Pack ChunkX (uint16) + Vx (int16) into a single int64 map key. */
	static int64 MakeVerifyKey(int64 ChunkX, int16 Vx);

	// ── Push ──────────────────────────────────────────────────────────────

	void InternalPushState(UScriptStruct* Struct, const void* SrcData, UObject* Owner);

	/** Try sending via UDP. Returns false if UDP is not connected. */
	bool DispatchUDPPush(UScriptStruct* StructType, const TArray<uint8>& Bytes,
	                     int64 ChunkX, int16 Vx);

	/** Send VoxelType=0 for a slot to signal the server to clear it. */
	void SendClearMessage(int64 ChunkX, int16 Vx) const;

	void FlushPushQueue();

	UFUNCTION()
	void OnUDPConnected();

	// ── Verify-retry ──────────────────────────────────────────────────────

	/** Register a voxel slot for post-push verification and start the tick. */
	void RegisterVerification(UScriptStruct* StructType, const TArray<uint8>& Bytes,
	                          int64 ChunkX, int16 Vx);

	/**
	 * Periodic tick — checks elapsed time on each pending verification.
	 * If PushVerifyDelaySeconds has passed without a notification, the push
	 * is retried (up to MaxPushRetries).
	 */
	UFUNCTION()
	void VerificationTick();

	// ── Pull ──────────────────────────────────────────────────────────────

	void InternalPullState(const FLatentActionInfo& LatentInfo, UObject* Owner,
	                       UScriptStruct* StructType, void* OutAddr,
	                       EPullStateResult& Result);

	void InternalPullStateCallback(UScriptStruct* StructType, UObject* Owner,
	                               TFunction<void(bool, const TArray<uint8>&, UScriptStruct*)> Callback);

	void FirePullQuery(int64 ChunkX);

	void HandlePullResponse(const FPersistencePullResponse& Response);
};
