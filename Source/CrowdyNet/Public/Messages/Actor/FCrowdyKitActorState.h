#pragma once

#include "CoreMinimal.h"
#include "FCrowdyKitActorState.generated.h"

struct FCrowdyNativeKitActorState;

/**
 * A server-driven actor a Game Kit starter shows the players near it, such as a mob, an NPC, a pet or a ball, as its latest
 * realtime update carried it. View state: what the actor truly holds is in its hub, read through that starter's Server Object.
 */
USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyKitActorState
{
	GENERATED_BODY()

	/** What it is, such as mob, npc, pet or ball; the starter's Kit Actors table gives each kind its class. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FName Kind;

	/** Its id within the hub that shows it, the one that hub's calls take, such as Attack's Mob. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Id = 0;

	/** Where it is in the world. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FVector Location = FVector::ZeroVector;

	/** Which way it faces: the hub's yaw, in degrees, and zero when the hub gives none. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FRotator Rotation = FRotator::ZeroRotator;

	/** Whether the hub gives it health; Hp and Max Hp are 0 when it does not. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	bool bHasHp = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Hp = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 MaxHp = 0;

	/** What it is doing, in the hub's words, such as idle, chase or follow; empty when the hub gives none. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Mode;

	/** The player it belongs to, such as a pet's or the ball's holder; 0 for none. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	int64 Owner = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString Name;

	/** The Instance Id of the hub that shows it, such as the area "2,-1" of an npcs mob: its Server Object for calls such as Attack. Empty when its hub does not say. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Game Kit")
	FString InstanceId;

	/** Unreal units per chunk: the platform's chunk is 16 blocks of 100 units. */
	static constexpr double ChunkWorldSize = 1600.0;

	static FCrowdyKitActorState FromNative(const FCrowdyNativeKitActorState& Native);

	/** The hub's Instance Id an actor id names as "<capital letter><Instance Id>@<id>", such as "M2,-1@3"; empty for any other id. */
	static FString ReadInstanceId(TConstArrayView<uint8> ActorIdOctets);

	/** False when Bytes are not a kit actor's state, a player's own state say. Any thread. */
	static bool Decode(TConstArrayView<uint8> Bytes, FCrowdyKitActorState& Out);

	/** Whether everything but where it is and which way it faces is the same. */
	bool HasSameFacts(const FCrowdyKitActorState& Other) const
	{
		return Kind == Other.Kind && Id == Other.Id && bHasHp == Other.bHasHp && Hp == Other.Hp && MaxHp == Other.MaxHp && Owner == Other.Owner
			&& Mode.Equals(Other.Mode, ESearchCase::CaseSensitive) && Name.Equals(Other.Name, ESearchCase::CaseSensitive);
	}
};
