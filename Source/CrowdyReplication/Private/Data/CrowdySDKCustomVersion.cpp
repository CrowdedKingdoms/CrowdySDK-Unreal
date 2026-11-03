#include "Data/CrowdySDKCustomVersion.h"

#include "Serialization/CustomVersion.h"

const FGuid FCrowdySDKCustomVersion::GUID(0xB1C05FD6, 0xBDF04869, 0x917399BD, 0x28723C55);

static FCustomVersionRegistration GRegisterCrowdySDKCustomVersion(FCrowdySDKCustomVersion::GUID,
	FCrowdySDKCustomVersion::LatestVersion, TEXT("CrowdySDK"));
