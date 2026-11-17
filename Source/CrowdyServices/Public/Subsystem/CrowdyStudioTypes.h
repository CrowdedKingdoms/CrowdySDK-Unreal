#pragma once

#include "CoreMinimal.h"
#include "CrowdyStudioTypes.generated.h"

/** How a Studio call ended. Values are only ever added at the end. */
UENUM(BlueprintType)
enum class ECrowdyStudioResult : uint8
{
	Success,
	/** The player has no app token yet. */
	NotSignedIn,
	/** A missing permission such as use_studio_agent, or the app's agent policy; the message names which. */
	Denied,
	/** No such project or model. */
	NotFound,
	/** Any other refusal, the platform's or this SDK's before sending. */
	Refused,
	/** The request did not complete. */
	Failed,
	/** The answer was malformed, so nothing from it is returned. */
	BadReply,
	/** This SDK version does not carry the call yet, so nothing was sent. */
	NotBuilt
};

/** Whether a run is a draft test on the grid or a live deploy. Both deploy for real. */
UENUM(BlueprintType)
enum class ECrowdyStudioRunMode : uint8
{
	DraftTest UMETA(DisplayName="Draft Test"),
	Live
};

/** The half of a Studio project: its SERVER mod or its CLIENT Mod Script. */
UENUM(BlueprintType)
enum class ECrowdyStudioTarget : uint8
{
	Server,
	Client
};
