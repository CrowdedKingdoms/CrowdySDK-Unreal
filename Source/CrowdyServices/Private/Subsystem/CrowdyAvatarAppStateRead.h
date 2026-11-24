#pragma once

#include "CoreMinimal.h"

class FProperty;
struct FCrowdyAppAvatarState;
struct FCrowdyAvatarError;
struct FCrowdyCppJsonResult;

namespace CrowdyAvatarAppState
{
	// Reads an avatarAppState answer. No row for this app is a success with an empty RawState for AvatarId in AppId.
	bool ReadAnswer(const FCrowdyCppJsonResult& Result, int64 AppId, int64 AvatarId, FCrowdyAppAvatarState& Out,
		FCrowdyAvatarError& OutError);

	// Writes a read state into a latent node's output: no state resets it to the type's default; false when it does not decode.
	bool WriteState(const FString& RawState, FProperty* Prop, void* Data);
}
