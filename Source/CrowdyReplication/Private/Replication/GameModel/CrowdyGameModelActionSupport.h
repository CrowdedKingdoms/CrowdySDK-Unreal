#pragma once

#include "CoreMinimal.h"
#include "Engine/World.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"

// Resolves Ctx's Game Model subsystem. Null when Ctx has been garbage-collected, has no world, or the
// subsystem is absent (not a play world).
inline UCrowdyGameModelSubsystem* GetGameModelSubsystem(const TWeakObjectPtr<UObject>& Ctx)
{
	if (!Ctx.IsValid())
	{
		return nullptr;
	}
	const UWorld* World = Ctx->GetWorld();
	return World ? World->GetSubsystem<UCrowdyGameModelSubsystem>() : nullptr;
}
