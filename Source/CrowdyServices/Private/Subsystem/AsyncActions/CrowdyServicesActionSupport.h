#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Subsystem/CrowdyAvatars.h"
#include "Subsystem/CrowdyTeams.h"

// Resolves the game-instance subsystem backing a Blueprint async action from its world context. Shared by the query
// and write action units so the lookup lives in one place. Null when the context, its world, or the subsystem is gone.
inline UCrowdyAvatars* GetAvatarsSubsystem(const TWeakObjectPtr<UObject>& Ctx)
{
	if (!Ctx.IsValid()) return nullptr;
	UGameInstance* GI = Ctx->GetWorld() ? Ctx->GetWorld()->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UCrowdyAvatars>() : nullptr;
}

inline UCrowdyTeams* GetTeamsSubsystem(const TWeakObjectPtr<UObject>& Ctx)
{
	if (!Ctx.IsValid()) return nullptr;
	UGameInstance* GI = Ctx->GetWorld() ? Ctx->GetWorld()->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UCrowdyTeams>() : nullptr;
}
