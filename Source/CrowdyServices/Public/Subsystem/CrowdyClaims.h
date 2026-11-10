#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
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
	BadReply
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

#if WITH_DEV_AUTOMATION_TESTS
	/** Stands in for Initialize on a subsystem built outside a collection, sending every call to Client. */
	void InitializeForTest(const TSharedPtr<FCrowdyCppClient>& Client, int64 AppId, int64 UserId);
#endif

private:
	struct FSearch;

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
