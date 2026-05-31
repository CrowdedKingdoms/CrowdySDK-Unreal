#pragma once

#include "CoreMinimal.h"

UENUM(BlueprintType)
enum class EAttachmentType : uint8
{
	None UMETA(DisplayName = "None"),
	FPGun UMETA(DisplayName = "First Person Gun Attachment")
	//Add more types here
	
};