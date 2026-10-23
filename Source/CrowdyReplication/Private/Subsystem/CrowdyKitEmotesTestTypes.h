#pragma once

// Declared unconditionally because UnrealHeaderTool refuses a reflected type inside a preprocessor block; only the
// tests that use it are guarded.

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UObject/Object.h"
#include "CrowdyKitEmotesTestTypes.generated.h"

/** Records what an emotes subsystem broadcast. */
UCLASS(meta = (CrowdyTestFixture))
class UCrowdyKitEmotesTestListener : public UObject
{
	GENERATED_BODY()

public:

	UFUNCTION()
	void OnEmote(AActor* SenderActor, const FString& SenderUuid, const FString& Emote, AActor* TargetActor, const FString& TargetUuid)
	{
		++Calls;
		LastSenderActor = SenderActor;
		LastSenderUuid = SenderUuid;
		LastEmote = Emote;
		LastTargetActor = TargetActor;
		LastTargetUuid = TargetUuid;
	}

	int32 Calls = 0;
	AActor* LastSenderActor = nullptr;
	AActor* LastTargetActor = nullptr;
	FString LastSenderUuid;
	FString LastEmote;
	FString LastTargetUuid;
};
