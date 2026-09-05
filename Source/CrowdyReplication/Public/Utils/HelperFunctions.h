// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "StructUtils/InstancedStruct.h"
#include "HelperFunctions.generated.h"

/**
 * 
 */
UCLASS()
class CROWDYREPLICATION_API UHelperFunctions : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	
	UFUNCTION(BlueprintPure, Category = "CrowdySDK|Coordinates", meta = (DisplayName = "Get Chunk Coordinate At Location", WorldContext="WorldContextObject"))
	static void GetChunkCoordinateAtLocation(UObject* WorldContextObject, const FVector& WorldLocation, int64& ChunkX, int64& ChunkY, int64& ChunkZ);
	
	UE_DEPRECATED(5.8, TEXT("Use Get Chunk Coordinate At Location instead"))
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "CrowdySDK|Coordinates", meta = (DeprecatedFunction, DeprecationMessage = "Use Get Chunk Coordinate At Location instead"))
	static void GetChunkCoordinatesAtWorldLocation(const FVector& WorldLocation, int64& ChunkX, int64& ChunkY, int64& ChunkZ);
	
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "CrowdySDK|Coordinates")
	static void GetVoxelCoordinatesAtWorldLocation(const FVector& WorldLocation, int32& VoxelX, int32& VoxelY, int32& VoxelZ, const int32 ChunkSize = 1600);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "CrowdySDK|Identifiers")
	static FString GetNewUUID();
	
	// Turns Seed into a 128-bit FGuid that is the same on every client (unlike FGuid::NewGuid, which is
	// random per call). All four words are derived from one 32-bit hash of Seed, so the real entropy is
	// only 32 bits, not 128: birthday odds hit roughly a 1% collision chance around 77,000 ids. Do not
	// "improve" this by widening the hash. The output is wire-visible and cross-client-stable, and
	// FCrowdyModelIdentity::NetIDToContainerKey turns it straight into the server-persisted key a player's
	// Game Model container is filed under; changing the derivation reshuffles every existing player's key
	// and orphans every container already saved on the server. Collisions are handled at the point ids
	// enter the registry (UCrowdyEntitySubsystem::RegisterEntity), not by widening this hash.
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "CrowdySDK|Identifiers")
	static FGuid GetDeterministicID(const int64 Seed);
	
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "CrowdySDK|Identifiers")
	static FGuid GetNewID();
};
