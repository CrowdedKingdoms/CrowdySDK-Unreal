#pragma once

#include "CoreMinimal.h"
#include "CrowdyModScriptTypes.generated.h"

/** The grid a Mod Script runs in: its id and its box, in chunks. Empty (GridId 0) outside a grid. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModScriptGrid
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int64 GridId = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int64 LowChunkX = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int64 LowChunkY = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int64 LowChunkZ = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int64 HighChunkX = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int64 HighChunkY = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int64 HighChunkZ = 0;
};

/** A group of host calls, as the consent prompt shows them. */
UENUM(BlueprintType)
enum class ECrowdyModScriptGroup : uint8
{
	State UMETA(ToolTip="Reads and writes the player's own app state."),
	WorldRead UMETA(DisplayName="World Read", ToolTip="Reads the grid's chunks and the players in it."),
	WorldWrite UMETA(DisplayName="World Write", ToolTip="Changes voxels in the grid, as the player."),
	Meta UMETA(ToolTip="Reads the grid's box and the player's code permissions there."),
	Egress UMETA(ToolTip="Sends messages within the grid and to its channels."),
	Present UMETA(ToolTip="Shows text in the mod HUD and draws shapes only this player sees."),
	Input UMETA(ToolTip="Reads the player's mouse clicks."),
};

/** Why a Mod Script stopped. */
UENUM(BlueprintType)
enum class ECrowdyModScriptStop : uint8
{
	Removed UMETA(ToolTip="The grid no longer serves it."),
	Changed UMETA(ToolTip="Its code, permissions or tick changed; the new version is asked about again."),
	Unconsented UMETA(ToolTip="The player's agreement to it was taken back elsewhere."),
	Revoked UMETA(ToolTip="The player took their agreement back here."),
	LeftGrid UMETA(DisplayName="Left Grid", ToolTip="The player left its grid."),
	Stopped UMETA(ToolTip="Sign-out, travel, the world ending, or Mod Scripts switched off."),
	CircuitOpen UMETA(DisplayName="Circuit Open", ToolTip="Five calls in a row failed; it is started again after 60 seconds."),
	Failed UMETA(ToolTip="Fetching, checking or starting it failed."),
};

/** A running Mod Script. Generation changes each time it starts, so an answer meant for an earlier run is told apart. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModScript
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	FString ModId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	FString Name;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int64 AuthorId = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int64 GridId = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int32 Version = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int32 Generation = 0;

	/** The host calls it may make. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	TArray<FString> Calls;
};

/** The question to ask the player before an author's Mod Scripts run on this grid. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModScriptPrompt
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int32 PromptId = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int64 GridId = 0;

	/** The author, who owns the grid. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int64 AuthorId = 0;

	/** The names of the Mod Scripts a yes runs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	TArray<FString> Scripts;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	TArray<FString> Calls;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	TArray<ECrowdyModScriptGroup> Groups;

	/** Calls the scripts ask for that this game refuses. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	TArray<FString> NotOffered;
};

/** A refusal from the platform, such as a revoke it would not take. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModScriptError
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	FString ModId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	FString Code;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	FString Message;
};

/** One press or release. X and Y run from -1 to 1 across the game viewport, +Y up. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModScriptClick
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	bool bDown = false;

	/** 0 left, 1 middle, 2 right. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int32 Button = 0;

	/** Wall-clock milliseconds since the Unix epoch. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int64 AtMs = 0;

	/** How long the button was held, on a release. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int32 HeldMs = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	float X = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	float Y = 0.f;
};

/** The clicks since the last drain, leaving out those the UI took. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModScriptClicks
{
	GENERATED_BODY()

	/** Held now: 1 left, 2 right, 4 middle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int32 Buttons = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int32 LeftHeldMs = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int32 MiddleHeldMs = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int32 RightHeldMs = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	TArray<FCrowdyModScriptClick> Clicks;
};

/** A voxel with a state. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModScriptCellState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int32 X = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int32 Y = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int32 Z = 0;

	/** The state, base64. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	FString State;
};

/** A loaded chunk as a Mod Script reads it. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModScriptChunk
{
	GENERATED_BODY()

	/** 4,096 voxel types, index x + 16y + 256z. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	TArray<uint8> Types;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	TArray<FCrowdyModScriptCellState> States;
};

/** A player in the box asked about. Location is in voxels from the world origin. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModScriptPlayer
{
	GENERATED_BODY()

	/** The entity id, 32 hex digits. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	FString ActorId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	bool bSelf = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	FString Name;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	FVector Location = FVector::ZeroVector;

	/** Degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	float Yaw = 0.f;

	/** Degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	float Pitch = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	int32 Program = 0;
};

UENUM(BlueprintType)
enum class ECrowdyModScriptShapeKind : uint8
{
	Box,
	Sphere,
	Cylinder,
	Capsule,
	Plane,
};

/** A shape a Mod Script draws for this player only. Location is in voxels from the world origin, inside the grid. */
USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyModScriptShape
{
	GENERATED_BODY()

	/** Unique within its script, at most 32 characters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	FString Id;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	ECrowdyModScriptShapeKind Kind = ECrowdyModScriptShapeKind::Box;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	FVector Location = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	FQuat Rotation = FQuat::Identity;

	/** In voxels, 0.04 to 8 on each axis. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	FVector Size = FVector::OneVector;

	/** From the script's 0xRRGGBB, read as sRGB. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	FLinearColor Color = FLinearColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	float Opacity = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	bool bVisible = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Mod Scripts")
	bool bUnlit = false;
};
