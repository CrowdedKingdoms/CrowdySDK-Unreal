#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CrowdyServerComputeSettings.generated.h"

/** Server Compute's project settings, edited on the Settings tab of the Server Compute page and saved in Config/DefaultEditor.ini. */
UCLASS(Config = Editor, DefaultConfig)
class UCrowdyServerComputeSettings : public UObject
{
	GENERATED_BODY()

public:
	static constexpr int32 MinRevisionsToKeep = 3;
	static constexpr int32 MaxRevisionsToKeep = 50;

	/** How many deployed revisions of each type's server code the project keeps, newest first. */
	UPROPERTY(Config)
	int32 RevisionsToKeep = 5;

	/** RevisionsToKeep within its allowed range, whatever the ini says. */
	int32 GetRevisionsToKeep() const { return FMath::Clamp(RevisionsToKeep, MinRevisionsToKeep, MaxRevisionsToKeep); }

	/** Clamps and saves to the project's DefaultEditor.ini; false when the file could not be written. */
	bool SetRevisionsToKeep(int32 Count);
};
