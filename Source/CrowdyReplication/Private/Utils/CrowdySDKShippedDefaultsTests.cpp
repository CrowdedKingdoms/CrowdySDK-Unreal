// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

#include "Data/CrowdyActorPoolBackend.h"
#include "Data/CrowdyMapProfile.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

// The SDK ships a map profile so a project can run without authoring settings first. Three things have to
// hold for that to be true of a real project rather than only of the editor this was written in: the SDK has
// to actually offer the profile, at the path the asset lives at; the folder holding it has to be cooked even
// though nothing in a game's content graph ever references it; and what it selects has to be the actor pool,
// because that is what makes every other backend opt-in.
namespace
{
	constexpr EAutomationTestFlags CrowdySDKShippedDefaultsTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Written out here rather than shared with the registration, so that moving the asset without moving the
	// registration is a failure rather than two constants agreeing with each other about the wrong place.
	const TCHAR* const CrowdySDKExpectedProfilePath =
		TEXT("/CrowdySDK/Data/DA_CrowdySDKDefaultProfile.DA_CrowdySDKDefaultProfile");

	const TCHAR* const CrowdySDKExpectedCookedDirectory = TEXT("/CrowdySDK/Data");
}

/**
 * The offer itself, made from this module's startup. Its path is the only link between the shipped asset and
 * the SDK's resolution order, so a rename that misses one of the two ends is caught here rather than by a map
 * quietly resolving nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySDKOffersItsShippedProfileTest,
	"CrowdySDK.MapProfile.SDKOffersItsShippedProfile",
	CrowdySDKShippedDefaultsTestFlags)

bool FCrowdySDKOffersItsShippedProfileTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("The SDK is the one offering a default map profile"),
		UCrowdySDKDeveloperSettings::GetShippedDefaultProfileProvider(), FString(TEXT("CrowdySDK")));

	TestEqual(TEXT("It offers the profile at the path the shipped asset lives at"),
		UCrowdySDKDeveloperSettings::GetShippedDefaultProfilePath().ToString(),
		FString(CrowdySDKExpectedProfilePath));

	return true;
}

/**
 * What the shipped profile SELECTS, which is the half that decides whether any other backend is opt-in.
 *
 * A profile choosing a crowd backend here would make every project installing a rendering plugin draw rows
 * by default and would turn that plugin's authoring surfaces on everywhere, which is the opt-out behaviour
 * this arrangement exists to remove. The asset carries no serialised properties on purpose, so this is also
 * the check that the C++ default it inherits is still the actor pool.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySDKShippedProfileSelectsTheActorPoolTest,
	"CrowdySDK.MapProfile.SDKShippedProfileSelectsTheActorPool",
	CrowdySDKShippedDefaultsTestFlags)

bool FCrowdySDKShippedProfileSelectsTheActorPoolTest::RunTest(const FString& Parameters)
{
	FString UnavailableReason;
	const UCrowdyMapProfile* const Shipped =
		UCrowdySDKDeveloperSettings::ResolveShippedDefaultProfile(UnavailableReason);

	if (!Shipped)
	{
		AddError(FString::Printf(
			TEXT("The shipped default profile did not resolve, so what a project configuring nothing runs on could not be read: %s"),
			*UnavailableReason));
		return false;
	}

	TestTrue(TEXT("A project that configures nothing has the SDK switched on"), Shipped->bEnableNetworking);

	TestEqual(TEXT("and draws its entities with actors, so every other backend is reached only by choice"),
		Shipped->ActorManagement.BackendClass.Get(), UCrowdyActorPoolBackend::StaticClass());

	return true;
}

#if WITH_EDITOR

/**
 * The cook half. Nothing in a game's content ever points at the shipped profile, since it is reached by path
 * from this module's startup, so without an explicit entry a packaged build would simply not contain it and
 * every map that configured nothing would fall back to no SDK at all.
 *
 * Read off the settings object the cook commandlet itself walks, not off the ini text, because the question is
 * whether the plugin's own config file reached that object rather than whether the file exists.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySDKShippedProfileFolderIsCookedTest,
	"CrowdySDK.MapProfile.SDKShippedProfileFolderIsCooked",
	CrowdySDKShippedDefaultsTestFlags)

bool FCrowdySDKShippedProfileFolderIsCookedTest::RunTest(const FString& Parameters)
{
	// The class lives in the developer tool settings module and only its CONFIG SECTION is named after the
	// older editor module, which is why the ini section this plugin writes and the class path here differ. The
	// older path is still tried, so an engine that moves the class back is answered rather than reported as a
	// missing check.
	UClass* PackagingSettingsClass = FindObject<UClass>(nullptr, TEXT("/Script/DeveloperToolSettings.ProjectPackagingSettings"));
	if (!PackagingSettingsClass)
		PackagingSettingsClass = FindObject<UClass>(nullptr, TEXT("/Script/UnrealEd.ProjectPackagingSettings"));

	if (!PackagingSettingsClass)
	{
		AddError(TEXT("The project packaging settings class was not found, so what the cook will include could not be read."));
		return false;
	}

	const FArrayProperty* const DirectoriesProperty =
		FindFProperty<FArrayProperty>(PackagingSettingsClass, TEXT("DirectoriesToAlwaysCook"));
	if (!DirectoriesProperty)
	{
		AddError(TEXT("The packaging settings no longer carry a DirectoriesToAlwaysCook list, so this check measures nothing."));
		return false;
	}

	const FStructProperty* const EntryProperty = CastField<FStructProperty>(DirectoriesProperty->Inner);
	const FStrProperty* const PathProperty = EntryProperty
		? FindFProperty<FStrProperty>(EntryProperty->Struct, TEXT("Path"))
		: nullptr;

	if (!PathProperty)
	{
		AddError(TEXT("A cooked-directory entry no longer carries a Path, so this check measures nothing."));
		return false;
	}

	const UObject* const PackagingSettings = PackagingSettingsClass->GetDefaultObject();
	FScriptArrayHelper_InContainer Directories(DirectoriesProperty, PackagingSettings);

	bool bFoundShippedFolder = false;
	for (int32 Index = 0; Index < Directories.Num(); Index++)
	{
		const FString Path = PathProperty->GetPropertyValue_InContainer(Directories.GetRawPtr(Index));
		if (Path == CrowdySDKExpectedCookedDirectory)
		{
			bFoundShippedFolder = true;
			break;
		}
	}

	TestTrue(TEXT("The folder holding the shipped profile is cooked even with nothing referencing it"),
		bFoundShippedFolder);

	return true;
}

#endif

#endif
