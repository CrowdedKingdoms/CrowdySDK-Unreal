// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "CrowdyActiveSessionMemory.generated.h"

/**
 * The active Game Model session, kept on the game instance so it outlives a map travel. The world-scoped
 * UCrowdyGameModelSubsystem writes it on SetActiveSession / ClearActiveSession and reads it back when a new
 * world's subsystem initializes, so the match map's placed entities register inside the session the lobby
 * map created or joined.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyActiveSessionMemory : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	FString SessionId;
};
