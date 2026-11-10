#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "CrowdyVoxels.generated.h"

class FCrowdyCppClient;

/** How a voxel call ended. Only Ok carries data. */
UENUM(BlueprintType)
enum class ECrowdyVoxelResult : uint8
{
	Ok,
	/** The player has no app token yet. */
	NotSignedIn,
	/** The platform answered with an error; the message carries its code. */
	Refused,
	/** The request did not complete. */
	Failed,
	/** The answer was malformed, so nothing from it is returned. */
	BadReply,
	/** A voxel edit got no answer in time, so the chunk was fetched again and its value stands. */
	NoAnswer
};

/** One voxel: where it sits in its chunk, its type and its state. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyVoxel
{
	GENERATED_BODY()

	/** The chunk's own Unreal X, Y and Z. 0 to 15 is inside the grid; the platform allows any 16-bit value. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Voxels")
	FIntVector Position = FIntVector::ZeroValue;

	/** The app's own numbering, any 16-bit value; 0 is empty. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Voxels")
	int32 Type = 0;

	/** Up to 1,024 bytes the app gives meaning to; empty when the voxel has none. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Voxels")
	TArray<uint8> State;
};

/** One 16 x 16 x 16 chunk as the platform holds it. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyVoxelChunk
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Voxels")
	int64 X = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Voxels")
	int64 Y = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Voxels")
	int64 Z = 0;

	/** 4,096 types, index x + 16y + 256z, with the edits inside the grid applied; a type outside 0 to 255 reads 0 here, so read voxels through Get Voxel. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Voxels")
	TArray<uint8> Grid;

	/** The chunk's per-voxel entries, one per position (the later where the platform lists one twice), sorted z, then y, then x; the helpers rely on that order. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Voxels")
	TArray<FCrowdyVoxel> Edits;

	/** A chunk write has stored this chunk. Voxel edits alone do not, and until one does the platform returns neither the chunk nor its edits. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Voxels")
	bool bStored = false;

	/** The platform cut the edits it sent, so this chunk's newest may be missing. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Voxels")
	bool bEditsTruncated = false;
};

USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyChunkFetch
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Voxels")
	ECrowdyVoxelResult Result = ECrowdyVoxelResult::Failed;

	/** The chunks, in the platform's order. Fetch Chunk answers exactly one. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Voxels")
	TArray<FCrowdyVoxelChunk> Chunks;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Voxels")
	FString Message;
};

/**
 * Fetches the platform's voxel chunks for the current app. Chunk (x, y, z) is the one the SDK places an actor in at that
 * spot, and a voxel's position is Unreal local X, Y and Z, z up. Needs the player signed in to the app.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdyVoxels : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** Chunks one request asks for; a fetch pages until an answer comes back short. */
	static constexpr int32 PageSize = 32;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/**
	 * Every chunk the platform stores within Range (clamped to 1 to 8) of the given chunk on every axis. OnDone runs once
	 * on the game thread, unless the game instance ends first, and before this returns when the fetch cannot start.
	 */
	void FetchChunksAround(int64 ChunkX, int64 ChunkY, int64 ChunkZ, int32 Range, TFunction<void(const FCrowdyChunkFetch&)> OnDone);

	/** One chunk, answered whether or not the platform stores it. OnDone runs as for FetchChunksAround. */
	void FetchChunk(int64 ChunkX, int64 ChunkY, int64 ChunkZ, TFunction<void(const FCrowdyChunkFetch&)> OnDone);

#if WITH_DEV_AUTOMATION_TESTS
	/** Stands in for Initialize on a subsystem built outside a collection, sending every call to Client. */
	void InitializeForTest(const TSharedPtr<FCrowdyCppClient>& Client, int64 AppId);
#endif

private:
	struct FRead;

	void RequestPage(const TSharedRef<FRead>& Read);
	FCrowdyCppClient* ResolveClient() const;
	int64 GetAppId() const;
	bool HasAppToken() const;

	TSharedPtr<uint8> LiveSessionToken;

#if WITH_DEV_AUTOMATION_TESTS
	TWeakPtr<FCrowdyCppClient> ClientForTest;
	int64 AppIdForTest = 0;
#endif
};
