#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Queries/Data/Channels/Types/FCrowdyChannel.h"
#include "CrowdyClaims.generated.h"

class FCrowdyCppClient;
struct FCrowdyCppJsonResult;

/** How a claim call ended. Only Success carries data. Values are only ever added at the end. */
UENUM(BlueprintType)
enum class ECrowdyClaimResult : uint8
{
	Success,
	/** The player has no app token yet. */
	NotSignedIn,
	/** The chunk is inside a grid other than the app's world grid, a claim among them. */
	NotClaimable,
	/** The chunk is already claimed. */
	AlreadyClaimed,
	/** The platform answered with another error, such as the app's claim policy; the message carries its code. */
	Refused,
	/** The request did not complete. */
	Failed,
	/** The answer was malformed, so nothing from it is returned. */
	BadReply,
	/** This SDK version does not carry the call yet, so nothing was sent. */
	NotBuilt
};

/** The answer to Claim Chunk. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyChunkClaim
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	ECrowdyClaimResult Result = ECrowdyClaimResult::Failed;

	/** The new claim's grid, 0 unless Result is Success. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	int64 GridId = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	int64 ChunkX = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	int64 ChunkY = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	int64 ChunkZ = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	FString Message;
};

/** The answer to Release Claim. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyClaimRelease
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	ECrowdyClaimResult Result = ECrowdyClaimResult::Failed;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	int64 GridId = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	FString Message;
};

/** One player's claim on one chunk. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyChunkClaimInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	int64 GridId = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	int64 ChunkX = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	int64 ChunkY = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	int64 ChunkZ = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	int64 OwnerUserId = 0;

	/** The signed-in player owns it. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	bool bOwnedByMe = false;
};

/** The answer to Find Claims. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyClaimSearch
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	ECrowdyClaimResult Result = ECrowdyClaimResult::Failed;

	/** Every claim inside the box, in grid id order; empty unless Result is Success. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	TArray<FCrowdyChunkClaimInfo> Claims;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	FString Message;
};

/**
 * The answer to Can Edit Chunk. It is advice: the platform still decides each edit. It does not see the player's access
 * tier, which must also grant update_voxel_data, or a closed wilderness, which refuses a chunk only the world grid covers.
 */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyChunkEditCheck
{
	GENERATED_BODY()

	/** Success means the platform answered; bCanEdit is only meaningful then. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	ECrowdyClaimResult Result = ECrowdyClaimResult::Failed;

	/** The signed-in player holds update_voxel_data on the deciding grid. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	bool bCanEdit = false;

	/** The grid that decides edits in the chunk, 0 when no grid covers it. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	int64 GridId = 0;

	/** Why the player cannot edit, or what went wrong. Empty when bCanEdit is true. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	FString Message;
};

/** One grid covering a chunk, with the signed-in player's own permission keys on it. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyGridPermissionRow
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	int64 GridId = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	int64 LowChunkX = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	int64 LowChunkY = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	int64 LowChunkZ = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	int64 HighChunkX = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	int64 HighChunkY = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	int64 HighChunkZ = 0;

	/** The player's own unexpired keys on this grid; nobody else's are ever returned. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	TArray<FString> PermissionKeys;

	/** This is the grid whose keys decide an edit in the chunk. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	bool bDecidesEdits = false;
};

/** The answer to Get Grid Permissions. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyGridPermissions
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	ECrowdyClaimResult Result = ECrowdyClaimResult::Failed;

	/** Every grid covering the chunk, the deciding grid first, then by smallest box and lowest id; empty unless Result is Success. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	TArray<FCrowdyGridPermissionRow> Grids;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	FString Message;
};

/** The answer to List Grid Channels. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyGridChannels
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	ECrowdyClaimResult Result = ECrowdyClaimResult::Failed;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	int64 GridId = 0;

	/** The grid's own channels, in the platform's order; empty unless Result is Success. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	TArray<FCrowdyChannel> Channels;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Claims")
	FString Message;
};

/**
 * Claims chunks for the signed-in player in the current app, gives them back, and finds who has claimed what. A claim is
 * a grid over exactly one wilderness chunk that only its owner builds in; the app's claim policy must be Self Claim.
 * Nothing is cached: each call asks the platform.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdyClaims : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** The most chunks one Find Claims box may hold. */
	static constexpr int64 MaxSearchChunks = 4096;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/**
	 * Claims one chunk. OnDone runs once on the game thread, unless the game instance ends first, and never inside this
	 * call.
	 */
	void ClaimChunk(int64 ChunkX, int64 ChunkY, int64 ChunkZ, TFunction<void(const FCrowdyChunkClaim&)> OnDone);

	/** Gives back one of this player's claims. OnDone runs as for ClaimChunk. */
	void ReleaseClaim(int64 GridId, TFunction<void(const FCrowdyClaimRelease&)> OnDone);

	/**
	 * Every claim inside a box of chunks, corners included. The world grid, plots and grids with no owner are not claims.
	 * A failed read fails the whole search. OnDone runs as for ClaimChunk.
	 */
	void FindClaims(int64 LowX, int64 LowY, int64 LowZ, int64 HighX, int64 HighY, int64 HighZ, TFunction<void(const FCrowdyClaimSearch&)> OnDone);

	/**
	 * Whether the player may edit voxels in one chunk: they must hold update_voxel_data on the smallest grid covering it
	 * (the lower grid id on a tie). When no grid covers the chunk the platform refuses every edit there, so the answer
	 * is Success with bCanEdit false and GridId 0. OnDone runs as for ClaimChunk.
	 */
	void CanEditChunk(int64 ChunkX, int64 ChunkY, int64 ChunkZ, TFunction<void(const FCrowdyChunkEditCheck&)> OnDone);

	/**
	 * Every grid covering one chunk with the player's own permission keys on each, the deciding grid first. For callers
	 * that want more than Can Edit Chunk's yes or no. OnDone runs as for ClaimChunk.
	 */
	void GetGridPermissions(int64 ChunkX, int64 ChunkY, int64 ChunkZ, TFunction<void(const FCrowdyGridPermissions&)> OnDone);

	/**
	 * The channels a grid owns, oldest first. A grid id of 0 or less is Refused without sending, and a row that is not one
	 * of this grid's fails the whole list as BadReply. OnDone runs as for ClaimChunk.
	 */
	void ListGridChannels(int64 GridId, TFunction<void(const FCrowdyGridChannels&)> OnDone);

#if WITH_DEV_AUTOMATION_TESTS
	/** Stands in for Initialize on a subsystem built outside a collection, sending every call to Client. */
	void InitializeForTest(const TSharedPtr<FCrowdyCppClient>& Client, int64 AppId, int64 UserId);
#endif

private:
	struct FSearch;
	using FCoveringRead = TFunction<void(ECrowdyClaimResult Result, const FString& Message, const FString& Code, TArray<FCrowdyGridPermissionRow> Grids)>;

	void ReadCoveringGrids(const FInt64Vector& Chunk, FCoveringRead OnRead);
	void ReadOwners(const TSharedRef<FSearch>& Search);
	bool ReadOwner(FSearch& Search, int32 Index, int64 GridId, const FCrowdyCppJsonResult& Result, FString& OutProblem) const;
	FCrowdyCppClient* ResolveClient() const;
	int64 GetAppId() const;
	int64 GetUserId() const;
	bool HasAppToken() const;

	TSharedPtr<uint8> LiveSessionToken;

#if WITH_DEV_AUTOMATION_TESTS
	TWeakPtr<FCrowdyCppClient> ClientForTest;
	int64 AppIdForTest = 0;
	int64 UserIdForTest = 0;
#endif
};
