// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreGlobals.h"
#include "CrowdyReplicationLog.h"
#include "Data/CrowdyMapProfile.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/PackageName.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

// A plugin may offer a map profile for maps that configure nothing, so a project can render remote entities
// without authoring settings first. These cases cover the resolution order around that offer (a map entry and
// Default Profile both outrank it, and a Default Profile that will not load is not papered over by it), that a
// world is told once which plugin's profile it ended up on, that an offer naming an asset which is not there is
// named in the missing-profile warning instead of vanishing, and that a second plugin's offer is refused rather
// than silently deciding the answer by module load order.
namespace
{
	constexpr EAutomationTestFlags CrowdyShippedProfileTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Sentences unique to each of the three lines these cases are about. Matching the line rather than a
	// counter beside it is what makes a case fail if the line itself is dropped.
	const TCHAR* const CrowdyShippedProfileMissingMarker = TEXT("resolved no map profile");
	const TCHAR* const CrowdyShippedProfileInUseMarker = TEXT("running on the default profile");
	const TCHAR* const CrowdyShippedProfileRefusedMarker = TEXT("as the default map profile");

	// Collects LogCrowdyReplication output at both the verbosities these lines use: the missing-profile and
	// refusal lines are warnings, the line saying which shipped profile a map ended up on is not.
	class FCrowdyShippedProfileLogCapture : public FOutputDevice
	{
	public:

		FCrowdyShippedProfileLogCapture()
		{
			GLog->AddOutputDevice(this);
		}

		virtual ~FCrowdyShippedProfileLogCapture()
		{
			GLog->RemoveOutputDevice(this);
		}

		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Category != LogCrowdyReplication.GetCategoryName())
				return;

			if (Verbosity != ELogVerbosity::Warning && Verbosity != ELogVerbosity::Log)
				return;

			FScopeLock Lock(&Mutex);
			Lines.Add(Message);
		}

		virtual bool CanBeUsedOnMultipleThreads() const override
		{
			return true;
		}

		// Lines carrying Marker since the last take. Taking clears everything collected, so consecutive calls
		// read consecutive stretches of the run and a case can say "and then it was silent".
		TArray<FString> TakeLinesContaining(const TCHAR* Marker)
		{
			GLog->FlushThreadedLogs();

			FScopeLock Lock(&Mutex);

			TArray<FString> Taken;
			for (const FString& Line : Lines)
			{
				if (Line.Contains(Marker))
					Taken.Add(Line);
			}

			Lines.Reset();
			return Taken;
		}

	private:

		FCriticalSection Mutex;
		TArray<FString> Lines;
	};

	// A minimal world that presents as a chosen type. The resolver reads nothing but the world's type and its
	// package name, and building one as Game would bring up every game world subsystem in the project.
	struct FCrowdyShippedProfileScopedWorld
	{
		UWorld* World = nullptr;

		explicit FCrowdyShippedProfileScopedWorld(const EWorldType::Type PresentedType)
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);

			World->WorldType = PresentedType;
		}

		~FCrowdyShippedProfileScopedWorld()
		{
			if (!World)
				return;

			World->WorldType = EWorldType::Editor;

			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}
	};

	// The settings CDO and the shipped-profile offer are both process-wide, and the offer is installed by
	// whichever Crowdy plugins the running project has. A case that changes either has to put both back or
	// every later test resolves against what it left behind.
	struct FCrowdyShippedProfileScopedSettings
	{
		UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>();
		TMap<TSoftObjectPtr<UWorld>, TSoftObjectPtr<UCrowdyMapProfile>> SavedMapProfiles;
		TSoftObjectPtr<UCrowdyMapProfile> SavedDefaultProfile;
		FString SavedProvider;
		FSoftObjectPath SavedShippedPath;

		FCrowdyShippedProfileScopedSettings()
		{
			SavedMapProfiles = Settings->MapProfiles;
			SavedDefaultProfile = Settings->DefaultProfile;
			SavedProvider = UCrowdySDKDeveloperSettings::GetShippedDefaultProfileProvider();
			SavedShippedPath = UCrowdySDKDeveloperSettings::GetShippedDefaultProfilePath();

			Settings->MapProfiles.Empty();
			Settings->DefaultProfile.Reset();
			UCrowdySDKDeveloperSettings::UnregisterShippedDefaultProfile(SavedProvider);
		}

		~FCrowdyShippedProfileScopedSettings()
		{
			Settings->MapProfiles = SavedMapProfiles;
			Settings->DefaultProfile = SavedDefaultProfile;

			UCrowdySDKDeveloperSettings::UnregisterShippedDefaultProfile(
				UCrowdySDKDeveloperSettings::GetShippedDefaultProfileProvider());

			if (!SavedProvider.IsEmpty())
				UCrowdySDKDeveloperSettings::RegisterShippedDefaultProfile(SavedProvider,
					TSoftObjectPtr<UCrowdyMapProfile>(SavedShippedPath));
		}
	};

	// A path under a real mount point naming an asset no project has, used where a case needs an offer that
	// points at something which is not there. That is the state of a fresh install before the plugin's profile
	// asset has been created, and it has to reach the log rather than look like no offer at all.
	//
	// Under the game's own mount point rather than an invented one, so what the case exercises is a missing
	// asset and not a missing content root.
	const TCHAR* const CrowdyShippedProfileAbsentAssetPath =
		TEXT("/Game/CrowdyTestNoSuchProfile.CrowdyTestNoSuchProfile");
}

/**
 * The route the whole item exists for: a map that names no profile, in a project whose Default Profile is
 * unset, runs on the profile a plugin ships instead of leaving the SDK switched off. The map is not warned
 * about, because nothing is wrong with it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyShippedDefaultCoversUnconfiguredMapTest,
	"CrowdySDK.MapProfile.ShippedDefaultCoversUnconfiguredMap",
	CrowdyShippedProfileTestFlags)

bool FCrowdyShippedDefaultCoversUnconfiguredMapTest::RunTest(const FString& Parameters)
{
	FCrowdyShippedProfileScopedSettings ScopedSettings;

	const TStrongObjectPtr<UCrowdyMapProfile> Shipped(NewObject<UCrowdyMapProfile>());
	UCrowdySDKDeveloperSettings::RegisterShippedDefaultProfile(TEXT("TestPlugin"),
		TSoftObjectPtr<UCrowdyMapProfile>(Shipped.Get()));

	TestEqual(TEXT("The offering plugin is named"),
		UCrowdySDKDeveloperSettings::GetShippedDefaultProfileProvider(), FString(TEXT("TestPlugin")));

	FCrowdyShippedProfileScopedWorld UnconfiguredMap(EWorldType::Game);
	FCrowdyShippedProfileLogCapture Capture;

	TestTrue(TEXT("A map that configures nothing resolves the shipped profile"),
		UCrowdySDKDeveloperSettings::ResolveProfileForWorld(UnconfiguredMap.World) == Shipped.Get());
	TestEqual(TEXT("A map that resolved a profile is not warned about"),
		Capture.TakeLinesContaining(CrowdyShippedProfileMissingMarker).Num(), 0);

	return true;
}

/**
 * The once-per-subject half of the line that says which plugin's profile a map ended up on. Every Crowdy
 * subsystem on a map resolves the profile during its own startup, so the repeat is the normal case rather than
 * a contrived one, and a second map is a different subject and still speaks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyShippedDefaultReportedOncePerWorldTest,
	"CrowdySDK.MapProfile.ShippedDefaultReportedOncePerWorld",
	CrowdyShippedProfileTestFlags)

bool FCrowdyShippedDefaultReportedOncePerWorldTest::RunTest(const FString& Parameters)
{
	FCrowdyShippedProfileScopedSettings ScopedSettings;

	const TStrongObjectPtr<UCrowdyMapProfile> Shipped(NewObject<UCrowdyMapProfile>());
	UCrowdySDKDeveloperSettings::RegisterShippedDefaultProfile(TEXT("TestPlugin"),
		TSoftObjectPtr<UCrowdyMapProfile>(Shipped.Get()));

	FCrowdyShippedProfileScopedWorld FirstMap(EWorldType::Game);
	FCrowdyShippedProfileScopedWorld SecondMap(EWorldType::Game);

	FCrowdyShippedProfileLogCapture Capture;

	UCrowdySDKDeveloperSettings::ResolveProfileForWorld(FirstMap.World);

	const TArray<FString> FirstLines = Capture.TakeLinesContaining(CrowdyShippedProfileInUseMarker);
	TestEqual(TEXT("The map is told once which shipped profile it is on"), FirstLines.Num(), 1);

	if (FirstLines.Num() == 1)
	{
		const FString MapName = FPackageName::GetShortName(FirstMap.World->GetOutermost()->GetName());
		TestTrue(TEXT("The line names the map"), FirstLines[0].Contains(MapName));
		TestTrue(TEXT("The line names the plugin the profile came from"), FirstLines[0].Contains(TEXT("TestPlugin")));
		TestTrue(TEXT("The line says where to configure the map instead"),
			FirstLines[0].Contains(TEXT("Crowdy SDK -> Map Profiles")));
	}

	for (int32 Ask = 0; Ask < 6; Ask++)
		UCrowdySDKDeveloperSettings::ResolveProfileForWorld(FirstMap.World);

	TestEqual(TEXT("Later subsystems asking about the same map are answered in silence"),
		Capture.TakeLinesContaining(CrowdyShippedProfileInUseMarker).Num(), 0);

	UCrowdySDKDeveloperSettings::ResolveProfileForWorld(SecondMap.World);

	const TArray<FString> SecondLines = Capture.TakeLinesContaining(CrowdyShippedProfileInUseMarker);
	TestEqual(TEXT("A different map is told in its own right"), SecondLines.Num(), 1);

	if (SecondLines.Num() == 1)
	{
		const FString SecondMapName = FPackageName::GetShortName(SecondMap.World->GetOutermost()->GetName());
		TestTrue(TEXT("The second line names the second map"), SecondLines[0].Contains(SecondMapName));
	}

	return true;
}

/**
 * What the shipped profile must never do: outrank or stand in for a choice the project made. A Default Profile
 * that loads wins, and a Default Profile that was chosen and will not load stays a reported fault rather than
 * being quietly replaced by a map that looks like it works.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyProjectSettingsOutrankTheShippedDefaultTest,
	"CrowdySDK.MapProfile.ProjectSettingsOutrankTheShippedDefault",
	CrowdyShippedProfileTestFlags)

bool FCrowdyProjectSettingsOutrankTheShippedDefaultTest::RunTest(const FString& Parameters)
{
	FCrowdyShippedProfileScopedSettings ScopedSettings;

	const TStrongObjectPtr<UCrowdyMapProfile> Shipped(NewObject<UCrowdyMapProfile>());
	const TStrongObjectPtr<UCrowdyMapProfile> Chosen(NewObject<UCrowdyMapProfile>());

	UCrowdySDKDeveloperSettings::RegisterShippedDefaultProfile(TEXT("TestPlugin"),
		TSoftObjectPtr<UCrowdyMapProfile>(Shipped.Get()));
	ScopedSettings.Settings->DefaultProfile = Chosen.Get();

	FCrowdyShippedProfileScopedWorld ChosenMap(EWorldType::Game);
	FCrowdyShippedProfileLogCapture Capture;

	TestTrue(TEXT("A Default Profile the project chose wins over the shipped one"),
		UCrowdySDKDeveloperSettings::ResolveProfileForWorld(ChosenMap.World) == Chosen.Get());
	TestEqual(TEXT("Nothing is said about a shipped profile that was not used"),
		Capture.TakeLinesContaining(CrowdyShippedProfileInUseMarker).Num(), 0);

	// The honesty case. Default Profile names an asset that is not there, which is a broken setting, and the
	// shipped profile must not hide it.
	ScopedSettings.Settings->DefaultProfile = TSoftObjectPtr<UCrowdyMapProfile>(
		FSoftObjectPath(CrowdyShippedProfileAbsentAssetPath));

	FCrowdyShippedProfileScopedWorld BrokenMap(EWorldType::Game);

	TestNull(TEXT("A Default Profile that will not load is not replaced by the shipped profile"),
		UCrowdySDKDeveloperSettings::ResolveProfileForWorld(BrokenMap.World));

	const TArray<FString> BrokenLines = Capture.TakeLinesContaining(CrowdyShippedProfileMissingMarker);
	TestEqual(TEXT("The broken Default Profile is reported"), BrokenLines.Num(), 1);

	if (BrokenLines.Num() == 1)
		TestTrue(TEXT("The report names the Default Profile that did not load"),
			BrokenLines[0].Contains(TEXT("Default Profile is set to")));

	TestEqual(TEXT("The shipped profile is not announced as being in use"),
		Capture.TakeLinesContaining(CrowdyShippedProfileInUseMarker).Num(), 0);

	return true;
}

/**
 * An offer naming an asset that is not present is the ordinary state of a plugin whose profile has not been
 * created yet. It has to reach the missing-profile warning by name, otherwise the difference between "no plugin
 * offers one" and "the offer could not be honoured" is invisible and the reader is sent to the wrong setting.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAbsentShippedProfileIsNamedTest,
	"CrowdySDK.MapProfile.AbsentShippedProfileIsNamed",
	CrowdyShippedProfileTestFlags)

bool FCrowdyAbsentShippedProfileIsNamedTest::RunTest(const FString& Parameters)
{
	FCrowdyShippedProfileScopedSettings ScopedSettings;

	FString UnavailableReason;
	TestNull(TEXT("With no offer at all there is no shipped profile"),
		UCrowdySDKDeveloperSettings::ResolveShippedDefaultProfile(UnavailableReason));
	TestTrue(TEXT("No offer is not a fault, so there is nothing to say about it"), UnavailableReason.IsEmpty());

	UCrowdySDKDeveloperSettings::RegisterShippedDefaultProfile(TEXT("TestPlugin"),
		TSoftObjectPtr<UCrowdyMapProfile>(FSoftObjectPath(CrowdyShippedProfileAbsentAssetPath)));

	TestNull(TEXT("An offer naming an asset that is not there resolves to nothing"),
		UCrowdySDKDeveloperSettings::ResolveShippedDefaultProfile(UnavailableReason));
	TestFalse(TEXT("An offer that could not be honoured has something to say"), UnavailableReason.IsEmpty());
	TestTrue(TEXT("It names the asset it looked for"),
		UnavailableReason.Contains(TEXT("CrowdyTestNoSuchProfile")));
	TestTrue(TEXT("It names the plugin that offered it"), UnavailableReason.Contains(TEXT("TestPlugin")));

	FCrowdyShippedProfileScopedWorld UncoveredMap(EWorldType::Game);
	FCrowdyShippedProfileLogCapture Capture;

	TestNull(TEXT("The map is left with no profile"),
		UCrowdySDKDeveloperSettings::ResolveProfileForWorld(UncoveredMap.World));

	const TArray<FString> Lines = Capture.TakeLinesContaining(CrowdyShippedProfileMissingMarker);
	TestEqual(TEXT("The map is reported once"), Lines.Num(), 1);

	if (Lines.Num() == 1)
	{
		TestTrue(TEXT("The warning still names the setting that is unset"),
			Lines[0].Contains(TEXT("Default Profile is unset")));
		TestTrue(TEXT("The warning also names the shipped profile that could not be used"),
			Lines[0].Contains(TEXT("CrowdyTestNoSuchProfile")));
	}

	return true;
}

/**
 * Two plugins offering a profile cannot both be right, and taking the newer one would make which profile a map
 * runs on depend on module load order. The first offer stands, the second is named rather than dropped, and
 * saying it twice is not worth a second line.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySecondShippedProfileOfferIsRefusedTest,
	"CrowdySDK.MapProfile.SecondShippedProfileOfferIsRefused",
	CrowdyShippedProfileTestFlags)

bool FCrowdySecondShippedProfileOfferIsRefusedTest::RunTest(const FString& Parameters)
{
	FCrowdyShippedProfileScopedSettings ScopedSettings;

	const TStrongObjectPtr<UCrowdyMapProfile> First(NewObject<UCrowdyMapProfile>());
	const TStrongObjectPtr<UCrowdyMapProfile> Second(NewObject<UCrowdyMapProfile>());

	UCrowdySDKDeveloperSettings::RegisterShippedDefaultProfile(TEXT("FirstPlugin"),
		TSoftObjectPtr<UCrowdyMapProfile>(First.Get()));

	FCrowdyShippedProfileLogCapture Capture;

	UCrowdySDKDeveloperSettings::RegisterShippedDefaultProfile(TEXT("SecondPlugin"),
		TSoftObjectPtr<UCrowdyMapProfile>(Second.Get()));

	TestEqual(TEXT("The first offer stays in effect"),
		UCrowdySDKDeveloperSettings::GetShippedDefaultProfileProvider(), FString(TEXT("FirstPlugin")));

	const TArray<FString> RefusalLines = Capture.TakeLinesContaining(CrowdyShippedProfileRefusedMarker);
	TestEqual(TEXT("The refused offer is reported once"), RefusalLines.Num(), 1);

	if (RefusalLines.Num() == 1)
	{
		TestTrue(TEXT("The line names the plugin that was refused"), RefusalLines[0].Contains(TEXT("SecondPlugin")));
		TestTrue(TEXT("The line names the plugin whose offer stands"), RefusalLines[0].Contains(TEXT("FirstPlugin")));
	}

	// A plugin re-registering the same offer, which happens on any reload, has nothing new to say.
	UCrowdySDKDeveloperSettings::RegisterShippedDefaultProfile(TEXT("SecondPlugin"),
		TSoftObjectPtr<UCrowdyMapProfile>(Second.Get()));

	TestEqual(TEXT("The same refused offer is not reported again"),
		Capture.TakeLinesContaining(CrowdyShippedProfileRefusedMarker).Num(), 0);

	// The provider that holds the offer may replace its own, which is what lets a plugin correct itself and
	// what lets these cases put the running project's real offer back afterwards.
	UCrowdySDKDeveloperSettings::RegisterShippedDefaultProfile(TEXT("FirstPlugin"),
		TSoftObjectPtr<UCrowdyMapProfile>(Second.Get()));

	TestTrue(TEXT("A provider may replace its own offer"),
		UCrowdySDKDeveloperSettings::GetShippedDefaultProfilePath() == FSoftObjectPath(Second.Get()));
	TestEqual(TEXT("Replacing its own offer is not a refusal"),
		Capture.TakeLinesContaining(CrowdyShippedProfileRefusedMarker).Num(), 0);

	// And withdrawing it is what a plugin does when it unloads, so a disabled plugin stops configuring maps.
	UCrowdySDKDeveloperSettings::UnregisterShippedDefaultProfile(TEXT("FirstPlugin"));

	TestTrue(TEXT("A withdrawn offer leaves nothing behind"),
		UCrowdySDKDeveloperSettings::GetShippedDefaultProfileProvider().IsEmpty());

	return true;
}

#endif
