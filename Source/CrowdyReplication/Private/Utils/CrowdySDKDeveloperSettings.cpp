#include "Utils/CrowdySDKDeveloperSettings.h"

#include "Engine/World.h"

FString UCrowdySDKDeveloperSettings::GetManagementApiUrl() const
{
	switch (Environment)
	{
	case ECrowdyEnvironment::Dev:  return TEXT("https://api.dev.crowdedkingdoms.com");
	case ECrowdyEnvironment::Prod: return TEXT("https://api.crowdedkingdoms.com");
	default:                       return ManagementApiUrl;
	}
}

FString UCrowdySDKDeveloperSettings::GetGameApiHttpUrl() const
{
	// No derivation. The console fetches the app's gameApiUrl from the server and writes it here.
	return GameApiHttpUrl;
}

FString UCrowdySDKDeveloperSettings::GetGameApiWsUrl() const
{
	// No derivation. The console writes this (the game URL with a ws scheme, as the docs do).
	return GameApiWsUrl;
}

const UCrowdyMapProfile* UCrowdySDKDeveloperSettings::ResolveProfileForWorld(const UWorld* World)
{
	if (!IsValid(World))
		return nullptr;

	const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
	if (!Settings)
		return nullptr;

	FString CurrentMap = FPackageName::GetShortName(World->GetOutermost()->GetName());

#if WITH_EDITOR
	if (World->WorldType == EWorldType::Editor || World->WorldType == EWorldType::PIE)
		CurrentMap = UWorld::RemovePIEPrefix(CurrentMap);
#endif

	for (const auto& [WorldPtr, ProfilePtr] : Settings->MapProfiles)
	{
		if (WorldPtr.IsNull())
			continue;

		if (FPackageName::GetShortName(WorldPtr.GetAssetName()) != CurrentMap)
			continue;

		return ProfilePtr.LoadSynchronous();
	}

	return Settings->DefaultProfile.LoadSynchronous();
}
