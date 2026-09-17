#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "CrowdyContainerManifestCommandlet.generated.h"

/**
 * Builds a map's container manifest without opening the editor, for a build pipeline:
 *   UnrealEditor-Cmd <project> -run=CrowdyContainerManifest -map=/Game/Maps/L_World[+/Game/Maps/L_Other]
 * Loads each map, scans its placed entities exactly as the Studio card does, saves the manifest beside the map
 * and prints the rows and skips. Exit code 1 when a map fails to load or a manifest fails to save.
 */
UCLASS()
class CROWDYSTUDIO_API UCrowdyContainerManifestCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UCrowdyContainerManifestCommandlet();
	virtual int32 Main(const FString& Params) override;
};
