#pragma once

#include "CoreMinimal.h"
#include "Subsystem/CrowdyKitSocial.h"
#include "UObject/Object.h"
#include "CrowdyKitSocialTestTypes.generated.h"

/** Records the chat messages the social kit announces, as a Blueprint graph would receive them. */
UCLASS(Transient)
class UCrowdyKitSocialTestListener : public UObject
{
	GENERATED_BODY()

public:
	TArray<FCrowdyKitGroup> Groups;
	TArray<FString> Senders;
	TArray<FString> Texts;

	UFUNCTION()
	void HandleChat(FCrowdyKitGroup Group, FString SenderUuid, FString Text)
	{
		Groups.Add(Group);
		Senders.Add(SenderUuid);
		Texts.Add(Text);
	}
};
