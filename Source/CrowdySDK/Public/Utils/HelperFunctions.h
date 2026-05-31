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
class CROWDYSDK_API UHelperFunctions : public UBlueprintFunctionLibrary
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
	
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "CrowdySDK|Identifiers")
	static FGuid GetDeterministicID(const int64 Seed);
	
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "CrowdySDK|Identifiers")
	static FGuid GetNewID();
};
