// Fill out your copyright notice in the Description page of Project Settings.


#include "Utils/HelperFunctions.h"
#include "Replication/Subsystems/CrowdyEntityManager.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdySDKSubsystem.h"


void UHelperFunctions::GetChunkCoordinateAtLocation(UObject* WorldContextObject, const FVector& WorldLocation,
	int64& ChunkX, int64& ChunkY, int64& ChunkZ)
{
	if (!IsValid(WorldContextObject))
	{
		UE_LOG(LogTemp, Error, TEXT("[HelperFunctions]: WorldContextObject is null."));
		return;
	}
	
	const UWorld* World = WorldContextObject->GetWorld();
	
	if (!IsValid(World))
	{
		UE_LOG(LogTemp, Error, TEXT("[HelperFunctions]: World is null."));
		return;
	}

	const TObjectPtr<UCrowdyGameSession> GameSession = World->GetGameInstance()->GetSubsystem<UCrowdyGameSession>();
	
	const double ChunkSize = GameSession->GetChunkSize();
	
	ChunkX = static_cast<int64>(FMath::FloorToDouble(WorldLocation.X / ChunkSize));
	ChunkY = static_cast<int64>(FMath::FloorToDouble(WorldLocation.Y / ChunkSize));
	ChunkZ = static_cast<int64>(FMath::FloorToDouble(WorldLocation.Z / ChunkSize));
	
}

void UHelperFunctions::GetChunkCoordinatesAtWorldLocation(const FVector& WorldLocation, int64& ChunkX, int64& ChunkY,
                                                          int64& ChunkZ)
{
	
	constexpr double ChunkSize = 1600.0f;
	
	ChunkX = static_cast<int64>(FMath::FloorToDouble(WorldLocation.X / ChunkSize));
	ChunkY = static_cast<int64>(FMath::FloorToDouble(WorldLocation.Y / ChunkSize));
	ChunkZ = static_cast<int64>(FMath::FloorToDouble(WorldLocation.Z / ChunkSize));
}

void UHelperFunctions::GetVoxelCoordinatesAtWorldLocation(const FVector& WorldLocation, int32& VoxelX, int32& VoxelY,
	int32& VoxelZ, const int32 ChunkSize)
{
	int32 X = static_cast<int32>(WorldLocation.X);
	int32 Y = static_cast<int32>(WorldLocation.Y);
	int32 Z = static_cast<int32>(WorldLocation.Z);
	
	X = X % ChunkSize;
	Y = Y % ChunkSize;
	Z = Z % ChunkSize;
	
	if (X < 0) X += ChunkSize;
	if (Y < 0) Y += ChunkSize;
	if (Z < 0) Z += ChunkSize;
	
	VoxelX = X / 100;
	VoxelY = Y / 100;
	VoxelZ = Z / 100;
}

FString UHelperFunctions::GetNewUUID()
{
	const FGuid NewGuid = FGuid::NewGuid();
	FString GuidStr = NewGuid.ToString(EGuidFormats::Digits);
	return GuidStr;
}

FGuid UHelperFunctions::GetDeterministicID(const int64 Seed)
{
	const uint32 A = GetTypeHash(Seed);
	const uint32 B = HashCombine(A, 0x9E3779B9);
	const uint32 C = HashCombine(B, 0x85EBCA6B);
	const uint32 D = HashCombine(C, 0xC2B2AE35);
	
	return FGuid(A, B, C, D);
}

FGuid UHelperFunctions::GetNewID()
{
	return FGuid::NewGuid();
}