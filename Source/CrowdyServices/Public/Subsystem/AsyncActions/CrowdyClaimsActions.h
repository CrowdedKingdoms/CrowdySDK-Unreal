#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Subsystem/CrowdyClaims.h"
#include "CrowdyClaimsActions.generated.h"

// One signature serves each node's success and failure pins, because a Blueprint async node builds its pins from its delegates.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyClaimAsyncResult, int64, GridId, ECrowdyClaimResult, Result, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyClaimSearchAsyncResult, const TArray<FCrowdyChunkClaimInfo>&, Claims, ECrowdyClaimResult, Result, const FString&, Message);

/** Claims one wilderness chunk for the signed-in player. C++ callers use UCrowdyClaims::ClaimChunk. */
UCLASS()
class CROWDYSERVICES_API UCrowdyClaimChunk : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** The chunk is claimed; Grid Id is the new claim. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyClaimAsyncResult OnDone;

	/** Nothing was claimed; Result says why. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyClaimAsyncResult OnFailed;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Claim Chunk"), Category="Crowdy SDK|Claims")
	static UCrowdyClaimChunk* ClaimChunk(UObject* WorldContextObject, int64 ChunkX, int64 ChunkY, int64 ChunkZ);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 ChunkX = 0;
	int64 ChunkY = 0;
	int64 ChunkZ = 0;

	void Finish(const FCrowdyChunkClaim& Claim);
};

/** Gives back one of the signed-in player's claims. C++ callers use UCrowdyClaims::ReleaseClaim. */
UCLASS()
class CROWDYSERVICES_API UCrowdyReleaseClaim : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** The claim is gone and the chunk is claimable again; its voxels stay. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyClaimAsyncResult OnDone;

	/** Nothing was released; Result says why. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyClaimAsyncResult OnFailed;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Release Claim"), Category="Crowdy SDK|Claims")
	static UCrowdyReleaseClaim* ReleaseClaim(UObject* WorldContextObject, int64 GridId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 GridId = 0;

	void Finish(const FCrowdyClaimRelease& Release);
};

/** Finds every claim inside a box of chunks. C++ callers use UCrowdyClaims::FindClaims. */
UCLASS()
class CROWDYSERVICES_API UCrowdyFindClaims : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** Every claim in the box, in grid id order; empty when there are none. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyClaimSearchAsyncResult OnDone;

	/** The search did not complete; Result says why. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyClaimSearchAsyncResult OnFailed;

	/** The box runs from Low to High on every axis, corners included, and holds at most 4,096 chunks. */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Find Claims"), Category="Crowdy SDK|Claims")
	static UCrowdyFindClaims* FindClaims(UObject* WorldContextObject, int64 LowX, int64 LowY, int64 LowZ, int64 HighX, int64 HighY, int64 HighZ);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FInt64Vector Low = FInt64Vector::ZeroValue;
	FInt64Vector High = FInt64Vector::ZeroValue;

	void Finish(const FCrowdyClaimSearch& Search);
};
