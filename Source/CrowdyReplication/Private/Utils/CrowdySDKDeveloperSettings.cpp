#include "Utils/CrowdySDKDeveloperSettings.h"

#include "CrowdyReplicationLog.h"
#include "Engine/World.h"
#include "Misc/PackageName.h"
#include "UObject/ObjectKey.h"
#include "crowdy/default_origin.hpp"

namespace
{
	// Worlds already told that nothing configures them. Seven Crowdy subsystems resolve the profile while a
	// map starts up and every one of them would otherwise repeat the same sentence, so the fact is said once
	// per world instead of once per asker.
	//
	// Only ever touched from the game thread: resolving a profile loads the asset synchronously, so every
	// caller is on the game thread by construction.
	TSet<FObjectKey> CrowdyWorldsToldTheyHaveNoProfile;

	// The same, for worlds already told which plugin's shipped profile they ended up running on. Separate,
	// because a world reaches exactly one of the two and both are worth saying once.
	TSet<FObjectKey> CrowdyWorldsToldTheyUseTheShippedProfile;

	// The profile a plugin offers for maps that configure nothing, and the plugin offering it. Both are set
	// and cleared together, so an empty provider name means there is no offer.
	TSoftObjectPtr<UCrowdyMapProfile> CrowdyShippedDefaultProfile;
	FString CrowdyShippedDefaultProfileProvider;

	// Offers already refused, so a plugin that registers on every module load is not re-reported. Keyed on
	// the refused provider and path together, so a plugin that changes what it offers is heard again.
	TSet<FString> CrowdyRefusedShippedProfileOffers;

	// The world types the SDK runs in. Every Crowdy world subsystem restricts itself to these two, so a
	// request coming from an editor or preview world is not the SDK starting up on a map and there is
	// nothing there to go wrong or to report.
	bool CrowdyIsSDKWorld(const UWorld& World)
	{
		return World.WorldType == EWorldType::Game || World.WorldType == EWorldType::PIE;
	}

	void CrowdyReportWorldHasNoProfile(const UWorld& World, const FString& MapName, const FString& Cause)
	{
		if (!CrowdyIsSDKWorld(World))
			return;

		bool bAlreadyTold = false;
		CrowdyWorldsToldTheyHaveNoProfile.Add(FObjectKey(&World), &bAlreadyTold);
		if (bAlreadyTold)
			return;

		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[Crowdy SDK]: map '%s' resolved no map profile, so the SDK is inactive on it: no entity registration, ")
			TEXT("no actor or state replication, no event routing and no host election. %s ")
			TEXT("Set this in Project Settings -> Plugins -> Crowdy SDK -> Map Profiles: add an entry naming this map, ")
			TEXT("or set Default Profile to cover maps with no entry. If the map is meant to run without the SDK, give ")
			TEXT("it a profile with Enable Networking unticked, which records that intent and stops this. Reported once ")
			TEXT("for this world."),
			*MapName, *Cause);
	}

	void CrowdyReportWorldUsesShippedProfile(const UWorld& World, const FString& MapName)
	{
		if (!CrowdyIsSDKWorld(World))
			return;

		bool bAlreadyTold = false;
		CrowdyWorldsToldTheyUseTheShippedProfile.Add(FObjectKey(&World), &bAlreadyTold);
		if (bAlreadyTold)
			return;

		UE_LOG(LogCrowdyReplication, Log,
			TEXT("[Crowdy SDK]: map '%s' configures no profile of its own, so it is running on the default profile ")
			TEXT("shipped by the %s plugin ('%s'). To configure it yourself, set Project Settings -> Plugins -> ")
			TEXT("Crowdy SDK -> Map Profiles: either Default Profile, or an entry naming this map. Reported once for ")
			TEXT("this world."),
			*MapName, *CrowdyShippedDefaultProfileProvider, *CrowdyShippedDefaultProfile.ToString());
	}
}

void UCrowdySDKDeveloperSettings::RegisterShippedDefaultProfile(const FString& ProviderName,
	const TSoftObjectPtr<UCrowdyMapProfile>& Profile)
{
	if (ProviderName.IsEmpty() || Profile.IsNull())
		return;

	const bool bHeldByAnotherProvider = !CrowdyShippedDefaultProfileProvider.IsEmpty()
		&& CrowdyShippedDefaultProfileProvider != ProviderName;

	if (!bHeldByAnotherProvider)
	{
		CrowdyShippedDefaultProfileProvider = ProviderName;
		CrowdyShippedDefaultProfile = Profile;
		return;
	}

	// Two plugins offering a profile is not something a project can resolve by editing settings, and taking the
	// newer one would make the answer depend on module load order, so the first offer stands and the second is
	// named. A project that wants the refused one sets it as Default Profile, which outranks both.
	const FString OfferKey = ProviderName + TEXT("|") + Profile.ToString();

	bool bAlreadyRefused = false;
	CrowdyRefusedShippedProfileOffers.Add(OfferKey, &bAlreadyRefused);
	if (bAlreadyRefused)
		return;

	UE_LOG(LogCrowdyReplication, Warning,
		TEXT("[Crowdy SDK]: the %s plugin offers '%s' as the default map profile, but the %s plugin already offers ")
		TEXT("'%s' and that one stays in effect, so maps configuring nothing keep running on it. Set Project Settings ")
		TEXT("-> Plugins -> Crowdy SDK -> Map Profiles -> Default Profile to whichever you want, which outranks both."),
		*ProviderName, *Profile.ToString(), *CrowdyShippedDefaultProfileProvider,
		*CrowdyShippedDefaultProfile.ToString());
}

void UCrowdySDKDeveloperSettings::UnregisterShippedDefaultProfile(const FString& ProviderName)
{
	if (CrowdyShippedDefaultProfileProvider != ProviderName)
		return;

	CrowdyShippedDefaultProfileProvider.Reset();
	CrowdyShippedDefaultProfile.Reset();
}

FSoftObjectPath UCrowdySDKDeveloperSettings::GetShippedDefaultProfilePath()
{
	return CrowdyShippedDefaultProfile.ToSoftObjectPath();
}

FString UCrowdySDKDeveloperSettings::GetShippedDefaultProfileProvider()
{
	return CrowdyShippedDefaultProfileProvider;
}

const UCrowdyMapProfile* UCrowdySDKDeveloperSettings::ResolveShippedDefaultProfile(FString& OutUnavailableReason)
{
	OutUnavailableReason.Reset();

	if (CrowdyShippedDefaultProfile.IsNull())
		return nullptr;

	if (const UCrowdyMapProfile* AlreadyLoaded = CrowdyShippedDefaultProfile.Get())
		return AlreadyLoaded;

	// Asked before loading rather than after failing to load, because a plugin whose profile asset has not been
	// created yet is the ordinary state of a fresh install and would otherwise cost a failed synchronous load on
	// every map start. Nothing is remembered either way, so an asset created and saved during this session is
	// picked up by the next map that asks.
	const FString PackageName = CrowdyShippedDefaultProfile.ToSoftObjectPath().GetLongPackageName();
	if (!PackageName.IsEmpty() && !FPackageName::DoesPackageExist(PackageName))
	{
		OutUnavailableReason = FString::Printf(
			TEXT("The %s plugin offers '%s' as a default profile for maps that configure nothing, but that asset is ")
			TEXT("not present, so it could not be used."),
			*CrowdyShippedDefaultProfileProvider, *CrowdyShippedDefaultProfile.ToString());
		return nullptr;
	}

	const UCrowdyMapProfile* Loaded = CrowdyShippedDefaultProfile.LoadSynchronous();
	if (!Loaded)
	{
		OutUnavailableReason = FString::Printf(
			TEXT("The %s plugin offers '%s' as a default profile for maps that configure nothing, but that asset ")
			TEXT("did not load, so it could not be used."),
			*CrowdyShippedDefaultProfileProvider, *CrowdyShippedDefaultProfile.ToString());
	}

	return Loaded;
}

UCrowdySDKDeveloperSettings::UCrowdySDKDeveloperSettings()
	: Environment(GetReleaseEnvironment())
	, DiscoveryUrl(UTF8_TO_TCHAR(crowdy::kDefaultHttpOrigin))
{
}

ECrowdyEnvironment UCrowdySDKDeveloperSettings::GetReleaseEnvironment()
{
	// The vendored CrowdyCPP is built per tier, so its default origin names the tier this SDK build ships for.
	const FString VendoredTier = UTF8_TO_TCHAR(crowdy::kDefaultTier);
	if (VendoredTier == TEXT("dev"))
		return ECrowdyEnvironment::Dev;
	if (VendoredTier == TEXT("test"))
		return ECrowdyEnvironment::Test;
	if (VendoredTier == TEXT("prod"))
		return ECrowdyEnvironment::Prod;
	return ECrowdyEnvironment::Custom;
}

FString UCrowdySDKDeveloperSettings::GetDiscoveryUrl() const
{
	switch (Environment)
	{
	case ECrowdyEnvironment::Dev:  return TEXT("https://ck.dev.crowdedkingdoms.com");
	case ECrowdyEnvironment::Test: return TEXT("https://ck.test.crowdedkingdoms.com");
	case ECrowdyEnvironment::Prod: return TEXT("https://ck.prod.crowdedkingdoms.com");
	default:                       return DiscoveryUrl;
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

		const UCrowdyMapProfile* MatchedProfile = ProfilePtr.LoadSynchronous();
		if (!MatchedProfile)
		{
			// An entry that names the map but whose asset will not load leaves the map exactly as unconfigured
			// as one with no entry at all, and it is the harder of the two to spot, because the settings screen
			// shows a row for this map.
			CrowdyReportWorldHasNoProfile(*World, CurrentMap, FString::Printf(
				TEXT("A Map Profiles entry names this map and points at '%s', which did not load; check that the asset still exists and is packaged."),
				*ProfilePtr.ToString()));
		}

		return MatchedProfile;
	}

	if (!Settings->DefaultProfile.IsNull())
	{
		const UCrowdyMapProfile* FallbackProfile = Settings->DefaultProfile.LoadSynchronous();
		if (FallbackProfile)
			return FallbackProfile;

		// A Default Profile that was chosen and will not load is a broken setting rather than an unconfigured
		// project, so a plugin's shipped profile is deliberately not substituted for it: that would hide the
		// broken choice behind a map that looks like it works.
		CrowdyReportWorldHasNoProfile(*World, CurrentMap, FString::Printf(
			TEXT("No Map Profiles entry names this map, and Default Profile is set to '%s', which did not load; check that the asset still exists and is packaged."),
			*Settings->DefaultProfile.ToString()));

		return nullptr;
	}

	FString ShippedUnavailable;
	if (const UCrowdyMapProfile* ShippedProfile = ResolveShippedDefaultProfile(ShippedUnavailable))
	{
		CrowdyReportWorldUsesShippedProfile(*World, CurrentMap);
		return ShippedProfile;
	}

	// ShippedUnavailable is empty unless a plugin offered a profile that could not be used, so a project with no
	// such plugin installed reads the same sentence it always did.
	FString Cause = TEXT("No Map Profiles entry names this map, and Default Profile is unset.");
	if (!ShippedUnavailable.IsEmpty())
		Cause += TEXT(" ") + ShippedUnavailable;

	CrowdyReportWorldHasNoProfile(*World, CurrentMap, Cause);

	return nullptr;
}
