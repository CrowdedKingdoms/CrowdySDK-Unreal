// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Backend/CrowdyMassRenderingBackend.h"
#include "Compiler/CrowdyAuthoringContributions.h"
#include "Data/CrowdyActorPoolBackend.h"
#include "Data/CrowdyMapProfile.h"
#include "Engine/World.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

// Whether the crowd-only authoring surfaces are shown follows from the backend a map profile selects, and
// this is the only module that can see both a backend that draws entities as crowd rows and the SDK query
// that asks. The SDK's own cases cover the half a project with no crowd backend can reach; these cover the
// half it cannot, including the precedence that decides when a plugin's shipped profile is reachable at all.
namespace
{
	constexpr EAutomationTestFlags CrowdyMassAuthoringGateTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

/**
 * The gate follows the Backend Class on the profiles a project can resolve, in the order a map reaches them.
 *
 * Both directions matter and they fail differently. Answering no where a map does draw rows takes the action
 * checkbox and the stand-in warnings away from a class that will be a row, which is silent. Answering yes
 * where nothing draws rows puts them in front of an author who cannot act on them, which is the complaint.
 *
 * The precedence case is the one with no other coverage: a project that sets Default Profile can never reach
 * the profile a plugin ships, so counting that shipped profile anyway would keep the surfaces on for a
 * project that has chosen the actor pool for every map it can open.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAuthoringGateFollowsProfileBackendTest,
	"CrowdySDK.MassNodes.AuthoringGateFollowsProfileBackend", CrowdyMassAuthoringGateTestFlags)
bool FCrowdyMassAuthoringGateFollowsProfileBackendTest::RunTest(const FString& Parameters)
{
	// The developer settings are a CDO shared by the whole process, so a case that empties them has to put
	// them back or every later test resolves against whatever it left behind. The shipped registration is
	// saved too: the precedence case below has to stand a profile that DRAWS ROWS in front of it, which the
	// SDK's own shipped profile deliberately does not do.
	struct FCrowdyMassAuthoringGateScopedSettings
	{
		UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>();
		TMap<TSoftObjectPtr<UWorld>, TSoftObjectPtr<UCrowdyMapProfile>> SavedMapProfiles;
		TSoftObjectPtr<UCrowdyMapProfile> SavedDefaultProfile;
		FString SavedShippedProvider;
		FSoftObjectPath SavedShippedPath;

		FCrowdyMassAuthoringGateScopedSettings()
		{
			SavedMapProfiles = Settings->MapProfiles;
			SavedDefaultProfile = Settings->DefaultProfile;
			SavedShippedProvider = UCrowdySDKDeveloperSettings::GetShippedDefaultProfileProvider();
			SavedShippedPath = UCrowdySDKDeveloperSettings::GetShippedDefaultProfilePath();

			Settings->MapProfiles.Empty();
			Settings->DefaultProfile.Reset();
		}

		~FCrowdyMassAuthoringGateScopedSettings()
		{
			Settings->MapProfiles = SavedMapProfiles;
			Settings->DefaultProfile = SavedDefaultProfile;

			UCrowdySDKDeveloperSettings::UnregisterShippedDefaultProfile(
				UCrowdySDKDeveloperSettings::GetShippedDefaultProfileProvider());

			if (!SavedShippedProvider.IsEmpty())
				UCrowdySDKDeveloperSettings::RegisterShippedDefaultProfile(SavedShippedProvider,
					TSoftObjectPtr<UCrowdyMapProfile>(SavedShippedPath));
		}
	};

	FCrowdyMassAuthoringGateScopedSettings ScopedSettings;

	// The SDK offers the default profile for maps that configure nothing, and it selects the actor pool; this
	// plugin offers none of its own. Asserted rather than assumed because the first case below measures
	// nothing if some other provider's profile is the one on offer.
	if (!TestEqual(TEXT("the SDK's shipped default profile is the one on offer"),
		UCrowdySDKDeveloperSettings::GetShippedDefaultProfileProvider(), FString(TEXT("CrowdySDK"))))
	{
		return false;
	}

	// Drawing rows is opt-in: a project that configured no profile has no map a row can appear on.
	TestFalse(TEXT("a project configuring no profile of its own falls to the shipped profile, which draws actors"),
		CrowdyAuthoringContributions::IsCrowdRepresentationSelected());

	const TStrongObjectPtr<UCrowdyMapProfile> ActorPoolProfile(NewObject<UCrowdyMapProfile>());
	ActorPoolProfile->ActorManagement.BackendClass = UCrowdyActorPoolBackend::StaticClass();

	const TStrongObjectPtr<UCrowdyMapProfile> CrowdProfile(NewObject<UCrowdyMapProfile>());
	CrowdProfile->ActorManagement.BackendClass = UCrowdyMassRenderingBackend::StaticClass();

	// Precedence, exercised against a shipped profile that DOES draw rows. Standing the real one here would
	// prove nothing, because it selects the actor pool and the answer would be false either way: the case has
	// to be able to fail.
	UCrowdySDKDeveloperSettings::UnregisterShippedDefaultProfile(
		UCrowdySDKDeveloperSettings::GetShippedDefaultProfileProvider());
	UCrowdySDKDeveloperSettings::RegisterShippedDefaultProfile(TEXT("TestCrowdPlugin"),
		TSoftObjectPtr<UCrowdyMapProfile>(CrowdProfile.Get()));

	TestTrue(TEXT("a shipped profile that draws rows is reachable when nothing else is configured"),
		CrowdyAuthoringContributions::IsCrowdRepresentationSelected());

	// Setting Default Profile covers every map with no entry of its own, so the shipped profile is no longer
	// reachable from anywhere and must stop counting.
	ScopedSettings.Settings->DefaultProfile = ActorPoolProfile.Get();
	TestFalse(TEXT("setting Default Profile to an actor-pool profile puts that shipped profile out of reach"),
		CrowdyAuthoringContributions::IsCrowdRepresentationSelected());

	ScopedSettings.Settings->DefaultProfile = CrowdProfile.Get();
	TestTrue(TEXT("and a Default Profile that does draw rows turns it back on"),
		CrowdyAuthoringContributions::IsCrowdRepresentationSelected());

	// One map on each backend is the case a class cannot answer for itself, and is why the query is asked of
	// the project rather than of a map: a class authored here can still end up drawn as a row.
	//
	// The map the entry names is never loaded, and does not have to exist: what decides whether a map can be
	// drawn through an entry is that the entry names one at all.
	const TSoftObjectPtr<UWorld> CrowdMapKey(
		FSoftObjectPath(TEXT("/Game/Maps/CrowdyMassGateCrowdMap.CrowdyMassGateCrowdMap")));
	const TSoftObjectPtr<UWorld> ActorMapKey(
		FSoftObjectPath(TEXT("/Game/Maps/CrowdyMassGateActorMap.CrowdyMassGateActorMap")));

	ScopedSettings.Settings->DefaultProfile = ActorPoolProfile.Get();
	ScopedSettings.Settings->MapProfiles.Add(CrowdMapKey, CrowdProfile.Get());
	ScopedSettings.Settings->MapProfiles.Add(ActorMapKey, ActorPoolProfile.Get());
	TestTrue(TEXT("one map on a crowd backend is enough, whatever the rest of the project uses"),
		CrowdyAuthoringContributions::IsCrowdRepresentationSelected());

	ScopedSettings.Settings->MapProfiles.Empty();
	ScopedSettings.Settings->MapProfiles.Add(ActorMapKey, ActorPoolProfile.Get());
	TestFalse(TEXT("and a project with every route on the actor pool reports no crowd representation"),
		CrowdyAuthoringContributions::IsCrowdRepresentationSelected());

	// An entry naming no map draws nothing, so it cannot turn the surfaces on by itself. Without this a
	// half-filled settings row would answer for a project that has not chosen anything yet.
	ScopedSettings.Settings->MapProfiles.Add(TSoftObjectPtr<UWorld>(), CrowdProfile.Get());
	TestFalse(TEXT("nor does a map profiles entry that names no map"),
		CrowdyAuthoringContributions::IsCrowdRepresentationSelected());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
