// ChunkCoordinate.h
#pragma once
#include "CoreMinimal.h"
#include "ChunkCoordinate.generated.h"

USTRUCT(BlueprintType)
struct CKSHAREDTYPES_API FChunkCoordinate
{
    GENERATED_BODY()

    UPROPERTY()
    int64 M;

    UPROPERTY(BlueprintReadWrite, Category = "Chunk")
    int64 X;

    UPROPERTY(BlueprintReadWrite, Category = "Chunk")
    int64 Y;

    UPROPERTY(BlueprintReadWrite, Category = "Chunk")
    int64 Z;

    // Default constructor
    FChunkCoordinate()
        : M(0), X(0), Y(0), Z(0)
    {
    }

    // Constructor with parameters
    FChunkCoordinate(int64 InM, int64 InX, int64 InY, int64 InZ)
        : M(InM), X(InX), Y(InY), Z(InZ)
    {
    }

    // Equality operator
    bool operator==(const FChunkCoordinate& Other) const
    {
        return M == Other.M && X == Other.X && Y == Other.Y && Z == Other.Z;
    }

    bool operator!=(const FChunkCoordinate& Other) const
    {
        return !(*this == Other);
    }

    // Hash function for use with TMap/TSet
    friend uint32 GetTypeHash(const FChunkCoordinate& Coord)
    {
        return HashCombine(
            HashCombine(
                HashCombine(GetTypeHash(Coord.M), GetTypeHash(Coord.X)),
                GetTypeHash(Coord.Y)
            ),
            GetTypeHash(Coord.Z)
        );
    }

    // String conversion for debugging
    FString ToString() const
    {
        return FString::Printf(TEXT("M:%lld, X:%lld, Y:%lld, Z:%lld"), M, X, Y, Z);
    }

    
};