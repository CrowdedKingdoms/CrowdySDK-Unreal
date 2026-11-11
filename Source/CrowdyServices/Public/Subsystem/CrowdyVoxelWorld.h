#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Core/UDP/Subscription/FCrowdySubscription.h"
#include "Serialization/CrowdyActorId.h"
#include "Subsystem/CrowdyVoxels.h"
#include "CrowdyVoxelWorld.generated.h"

class UCrowdyCppReplicationSubsystem;
class UCrowdyGameSession;
class UCrowdyUDPSubsystem;
struct FCrowdyTrackedSend;

/** One edit to send: a voxel of one chunk. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyVoxelWrite
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Voxels")
	int64 ChunkX = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Voxels")
	int64 ChunkY = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Voxels")
	int64 ChunkZ = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Voxels")
	FCrowdyVoxel Voxel;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnCrowdyVoxelChunk, int64, ChunkX, int64, ChunkY, int64, ChunkZ);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnCrowdyVoxelChunkChanged, int64, ChunkX, int64, ChunkY, int64, ChunkZ, const TArray<FIntVector>&, Positions);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnCrowdyVoxelEditDone, int64, EditId, const FCrowdyVoxelWrite&, Write, ECrowdyVoxelResult, Result, const FString&, Message);

/**
 * The live voxel world around followed actors: their chunks loaded, everyone's edits merged as they happen, and this
 * player's own edits shown at once, sent, and confirmed or taken back. Rendering is the game's: it reads cells and
 * listens for changes. Edits from other players arrive only for chunks within 8 of this player's realtime actor.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdyVoxelWorld : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/** Edits sent a second at most, so voice, movement and video keep room under the platform's 500. */
	static constexpr double EditsPerSecond = 100.0;

	/** How long a sent edit waits for its answer before the chunk is fetched again to settle it. */
	static constexpr double AnswerTimeoutSeconds = 1.0;

	/** How long after connecting the loaded chunks are fetched once more, so edits other servers routed late are seen. */
	static constexpr double SettleSeconds = 5.0;

	/** The platform's reach for live edits, in chunks on every axis. */
	static constexpr int32 LiveReach = 8;

	/** How long Reload Chunk waits for its fetch before it ends Failed. */
	static constexpr double ReloadTimeoutSeconds = 30.0;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Keep every chunk within Range (1 to 7) of the actor's chunk loaded, and unload what falls past Range + 1. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Voxels")
	void Follow(AActor* Actor, int32 Range);

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Voxels")
	void StopFollowing(AActor* Actor);

	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Voxels")
	bool IsChunkLoaded(int64 ChunkX, int64 ChunkY, int64 ChunkZ) const;

	/** Loaded, within 8 chunks of this player's realtime actor, and fetched since the connection settled. */
	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Voxels")
	bool IsChunkLive(int64 ChunkX, int64 ChunkY, int64 ChunkZ) const;

	/** Within 8 chunks of this player's own chunk, loaded or not: an edit there reaches the players near it live. */
	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Voxels")
	bool IsChunkInReach(int64 ChunkX, int64 ChunkY, int64 ChunkZ) const;

	/** What this player sees at the cell: their own edit still waiting, else the confirmed voxel. */
	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Voxels")
	FCrowdyVoxel GetVoxel(int64 ChunkX, int64 ChunkY, int64 ChunkZ, FIntVector Position) const;

	/** A copy of a loaded chunk as this player sees it. Copies the whole chunk, so read single cells with Get Voxel. */
	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Voxels")
	bool FindChunk(int64 ChunkX, int64 ChunkY, int64 ChunkZ, FCrowdyVoxelChunk& Chunk) const;

	/** Every chunk loaded now, so a renderer that starts late can draw what is already there; later loads arrive as On Chunk Loaded. */
	void ForEachLoadedChunk(TFunctionRef<void(int64 ChunkX, int64 ChunkY, int64 ChunkZ)> Visit) const;

	/** Every cell of a loaded chunk with a type of 1 to 255 as this player sees it, in index order, with no copy. */
	void ForEachFilledVoxel(int64 ChunkX, int64 ChunkY, int64 ChunkZ, TFunctionRef<void(FIntVector Position, int32 Type)> Visit) const;

	/** Show the edit at once and send it. Returns its edit id; On Edit Done reports it exactly once, never inside this call. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Voxels")
	int64 SetVoxel(const FCrowdyVoxelWrite& Write);

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Voxels")
	TArray<int64> SetVoxels(const TArray<FCrowdyVoxelWrite>& Writes);

	/**
	 * Fetches a loaded chunk again and merges it as any fetch is, under this player's waiting edits. OnDone runs once, on
	 * a later frame: Ok once a fetch begun after this call has merged and On Chunk Changed has named every cell it changed.
	 * A chunk not loaded, a lost connection, a failed fetch or 30 seconds without one ends it otherwise, and a chunk not
	 * loaded is not loaded by this. Dropped without running if the world ends first.
	 */
	void ReloadChunk(int64 ChunkX, int64 ChunkY, int64 ChunkZ, TFunction<void(ECrowdyVoxelResult Result, const FString& Message)> OnDone);

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Voxels")
	FOnCrowdyVoxelChunk OnChunkLoaded;

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Voxels")
	FOnCrowdyVoxelChunk OnChunkUnloaded;

	/** At most once a frame per chunk, naming every cell whose seen value changed, whoever changed it. */
	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Voxels")
	FOnCrowdyVoxelChunkChanged OnChunkChanged;

	/** Once per edit id. Comes after the On Chunk Changed that showed the edit, and before any that takes it back. */
	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Voxels")
	FOnCrowdyVoxelEditDone OnEditDone;

	/** A live edit as the realtime notification carries it. Public for the notification handler and tests. */
	struct FLiveEdit
	{
		FInt64Vector Chunk = FInt64Vector::ZeroValue;
		FCrowdyVoxel Voxel;
		int64 EpochMillis = 0;
		uint8 Sequence = 0;
		bool bOwnActor = false;
	};

	void HandleLiveEdit(const FLiveEdit& Edit);
	void HandleTrackedSend(const FCrowdyTrackedSend& Event);

#if WITH_DEV_AUTOMATION_TESTS
	using FSendFn = TFunction<uint64(const FCrowdyVoxelWrite& Write, FString& OutError)>;
	using FFetchFn = TFunction<void(int64 X, int64 Y, int64 Z, int32 Range, TFunction<void(const FCrowdyChunkFetch&)> OnDone)>;

	/** Replaces the platform: Range 0 asks for one chunk. Connection, clock and local chunk come from the setters below. */
	void InitializeForTest(FSendFn Send, FFetchFn Fetch);
	void SetConnectedForTest(bool bConnected) { bConnectedForTest = bConnected; }
	void SetNowForTest(double Now) { NowForTest = Now; }
	void SetLocalChunkForTest(const FInt64Vector& Chunk) { LocalChunkForTest = Chunk; bHasLocalChunkForTest = true; }
	void ClearLocalChunkForTest() { bHasLocalChunkForTest = false; }

	/** One frame's work, as Tick runs it, for a subsystem built outside a world. */
	void TickForTest() { Step(); }

	/** What the world's end does to this subsystem's state, for one built outside a world. */
	void EndWorldForTest() { ReleaseWorld(); }
#endif

private:
	struct FChunkState
	{
		FCrowdyVoxelChunk Confirmed;
		TMap<uint64, int64> CellTimes;
		TArray<FLiveEdit> HeldDuringFetch;
		TArray<FIntVector> SettlePositions;
		TArray<FIntVector> SettleInFlight;
		TArray<FIntVector> Changed;
		TSet<uint64> ChangedKeys;
		double FetchedAt = -1.0;
		double FetchIssuedAt = -1.0;
		int32 FetchRequest = 0;
		bool bLoaded = false;
		bool bFetchFull = false;
		bool bFetchAgain = false;
		bool bFetchAgainFull = false;
		bool bHeldOverflow = false;
		bool bInReach = false;
	};

	struct FPendingEdit
	{
		int64 EditId = 0;
		uint64 Ticket = 0;
		FCrowdyVoxelWrite Write;
		FInt64Vector Chunk = FInt64Vector::ZeroValue;
		uint64 Key = 0;
		double SentAt = 0.0;
		FString Message;
		ECrowdyVoxelResult Result = ECrowdyVoxelResult::Ok;
		uint8 Sequence = 0;
		bool bSent = false;
		bool bSequenceKnown = false;
		bool bShown = false;
		bool bAnswered = false;
	};

	struct FFollower
	{
		TWeakObjectPtr<AActor> Actor;
		int32 Range = 1;
		FInt64Vector Chunk = FInt64Vector::ZeroValue;
		bool bHasChunk = false;
	};

	struct FReload
	{
		TFunction<void(ECrowdyVoxelResult, const FString&)> OnDone;
		FString Message;
		FInt64Vector Chunk = FInt64Vector::ZeroValue;
		double Deadline = 0.0;
		/** Only a fetch of this request number or later, so begun after the reload was asked for, satisfies it. */
		int32 FirstRequest = 0;
		ECrowdyVoxelResult Result = ECrowdyVoxelResult::Ok;
		bool bDone = false;
	};

	double Now() const;
	bool IsConnected() const;
	bool HasActorId() const;
	bool HasLocalChunk(FInt64Vector& OutChunk) const;
	uint64 Send(const FCrowdyVoxelWrite& Write, FString& OutError);
	void Fetch(int64 X, int64 Y, int64 Z, int32 Range, TFunction<void(const FCrowdyChunkFetch&)> OnDone);

	void Step();
	void UpdateConnection();
	void UpdateActorId();
	void UpdateFollowers();
	void UpdateReach();
	void Reload(bool bWholeBand);
	void IssueSettles();
	void SendDue();
	void ResolveAnswers();
	void BroadcastChanges();

	void RequestChunks(const TArray<FInt64Vector>& Coords, bool bFull);
	void RequestCube(const FInt64Vector& Centre, int32 Range, bool bReload, const TSet<FInt64Vector>* Only = nullptr);
	void OnFetched(const FInt64Vector& Coord, int32 Request, const FCrowdyChunkFetch& Fetch);
	void OnCubeFetched(const FInt64Vector& Centre, int32 Range, int32 Request, const FCrowdyChunkFetch& Fetch);
	void Adopt(const FInt64Vector& Coord, const FCrowdyVoxelChunk* Fetched);
	bool ApplyLive(const FInt64Vector& Coord, FChunkState& State, const FLiveEdit& Edit);
	void RequestSettle(const FInt64Vector& Coord, const FIntVector& Position);
	FCrowdyVoxel SeenVoxel(const FInt64Vector& Coord, const FChunkState& State, const FIntVector& Position) const;
	const FPendingEdit* NewestPending(const FInt64Vector& Coord, uint64 Key) const;
	void MarkChanged(const FInt64Vector& Coord, FChunkState& State, const FIntVector& Position);
	void TakeOff(const TArray<int64>& EditIds);
	void EndReloads(const FInt64Vector& Coord, int32 SatisfyingRequest, ECrowdyVoxelResult Result, const FString& Message);
	TArray<FReload> TakeEndedReloads();
	void ReleaseWorld();

	TMap<FInt64Vector, FChunkState> Chunks;
	TArray<FPendingEdit> Pending;
	TArray<FPendingEdit> EarlyDone;
	TArray<FFollower> Followers;
	TArray<FReload> Reloads;
	TArray<FInt64Vector> DirtyChunks;
	TArray<FInt64Vector> LoadedEvents;
	TArray<FInt64Vector> UnloadedEvents;
	TMap<FInt64Vector, int32> PendingPerChunk;
	TArray<FInt64Vector> SettleQueue;

	FCrowdySubscription NotificationSubscription;
	FDelegateHandle TrackedSendHandle;
	TWeakObjectPtr<UCrowdyGameSession> Session;
	TWeakObjectPtr<UCrowdyUDPSubsystem> Udp;
	TWeakObjectPtr<UCrowdyCppReplicationSubsystem> Replication;
	TWeakObjectPtr<UCrowdyVoxels> Voxels;
	FCrowdyActorId OwnActorId;
	FString OwnActorUuid;
	FInt64Vector LocalChunk = FInt64Vector::ZeroValue;

	double SettleAt = -1.0;
	double RetryAt = 0.0;
	double SendAllowance = 0.0;
	double LastTickAt = -1.0;
	int64 NextEditId = 1;
	int32 NextFetchRequest = 1;
	bool bWasConnected = false;
	bool bSettleDone = false;
	bool bFollowDirty = false;
	bool bLocalKnown = false;

#if WITH_DEV_AUTOMATION_TESTS
	FSendFn SendForTest;
	FFetchFn FetchForTest;
	double NowForTest = 0.0;
	FInt64Vector LocalChunkForTest = FInt64Vector::ZeroValue;
	bool bConnectedForTest = false;
	bool bHasLocalChunkForTest = false;
	bool bUseTestSeams = false;
#endif
};
