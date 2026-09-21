// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreGlobals.h"
#include "CrowdyReplicationLog.h"
#include "Data/CrowdyActorManagementConfig.h"
#include "Data/CrowdyActorPoolBackend.h"
#include "Data/CrowdyMapProfile.h"
#include "Data/CrowdyRenderingBackend.h"
#include "Data/CrowdyRenderingBackendConfig.h"
#include "Data/CrowdyRenderingBackendTestTypes.h"
#include "Engine/Engine.h"
#include "Replication/Subsystems/CrowdyActorManager.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/PackageName.h"
#include "Misc/StringOutputDevice.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

// A map with no profile leaves every Crowdy subsystem inactive, which used to look exactly like a working
// SDK that had nothing to say. These cases cover that the fact is stated once for the world rather than once
// for each of the seven subsystems that ask, that a second map still gets its own, that a map which does
// resolve a profile stays silent, and that a world the SDK never runs in is not told either.
//
// The other half is the actor-pool default on FCrowdyActorManagementConfigStruct::BackendClass, which is a
// behaviour change for every profile that never chose a backend. Those cases run the same delta the editor
// runs when it saves and loads a profile asset, so "an existing profile keeps its explicit backend" is
// measured rather than assumed.
namespace
{
	constexpr EAutomationTestFlags CrowdyMapProfileDefaultsTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// The sentence every missing-profile line contains and nothing else in this category does, so an
	// unrelated CrowdyReplication warning cannot be counted as one of these.
	const TCHAR* const CrowdyMapProfileWarningMarker = TEXT("resolved no map profile");

	// Collects LogCrowdyReplication warnings while it is in scope. The line is the deliverable here, so the
	// cases read the real log output rather than a counter beside it, which would still read green if the
	// line itself were dropped.
	class FCrowdyMapProfileLogCapture : public FOutputDevice
	{
	public:

		FCrowdyMapProfileLogCapture()
		{
			GLog->AddOutputDevice(this);
		}

		virtual ~FCrowdyMapProfileLogCapture()
		{
			GLog->RemoveOutputDevice(this);
		}

		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Verbosity != ELogVerbosity::Warning || Category != LogCrowdyReplication.GetCategoryName())
				return;

			FScopeLock Lock(&Mutex);
			Lines.Add(Message);
		}

		virtual bool CanBeUsedOnMultipleThreads() const override
		{
			return true;
		}

		// Captured lines carrying a given phrase, for a case whose subject is not the missing-profile line.
		// Taking clears the same way, so the two takes cannot both read the same line.
		TArray<FString> TakeLinesContaining(const TCHAR* Needle)
		{
			GLog->FlushThreadedLogs();

			FScopeLock Lock(&Mutex);

			TArray<FString> Taken;
			for (const FString& Line : Lines)
			{
				if (Line.Contains(Needle))
					Taken.Add(Line);
			}

			Lines.Reset();
			return Taken;
		}

		// Missing-profile lines since the last take. Taking clears, so consecutive calls read consecutive
		// stretches of the run, which is what lets a case say "and then it was silent".
		TArray<FString> TakeProfileWarnings()
		{
			GLog->FlushThreadedLogs();

			FScopeLock Lock(&Mutex);

			TArray<FString> Taken;
			for (const FString& Line : Lines)
			{
				if (Line.Contains(CrowdyMapProfileWarningMarker))
					Taken.Add(Line);
			}

			Lines.Reset();
			return Taken;
		}

	private:

		FCriticalSection Mutex;
		TArray<FString> Lines;
	};

	// A minimal world that presents as a chosen type. The resolver reads nothing but the world's type and
	// its package name.
	//
	// It is always built as an editor world and then relabelled: creating one as Game brings up every game
	// world subsystem in the project, including ones from other plugins that assert outside a running game,
	// which is a whole engine's worth of behaviour to stand up for a check on two fields.
	struct FCrowdyMapProfileScopedWorld
	{
		UWorld* World = nullptr;

		explicit FCrowdyMapProfileScopedWorld(const EWorldType::Type PresentedType)
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);

			World->WorldType = PresentedType;
		}

		~FCrowdyMapProfileScopedWorld()
		{
			if (!World)
				return;

			// Torn down as what it was built as, so teardown runs the path creation ran.
			World->WorldType = EWorldType::Editor;

			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}
	};

	// The developer settings are a CDO shared by the whole process, so a case that empties them has to put
	// them back or every later test resolves against whatever it left behind.
	//
	// The profile an installed plugin offers for maps that configure nothing is part of "nothing configures
	// this map" too: leaving it in place would answer these cases with a real profile and they are about what
	// happens when nothing answers at all.
	struct FCrowdyMapProfileScopedSettings
	{
		UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>();
		TMap<TSoftObjectPtr<UWorld>, TSoftObjectPtr<UCrowdyMapProfile>> SavedMapProfiles;
		TSoftObjectPtr<UCrowdyMapProfile> SavedDefaultProfile;
		FString SavedShippedProvider;
		FSoftObjectPath SavedShippedPath;

		FCrowdyMapProfileScopedSettings()
		{
			SavedMapProfiles = Settings->MapProfiles;
			SavedDefaultProfile = Settings->DefaultProfile;
			SavedShippedProvider = UCrowdySDKDeveloperSettings::GetShippedDefaultProfileProvider();
			SavedShippedPath = UCrowdySDKDeveloperSettings::GetShippedDefaultProfilePath();

			Settings->MapProfiles.Empty();
			Settings->DefaultProfile.Reset();
			UCrowdySDKDeveloperSettings::UnregisterShippedDefaultProfile(SavedShippedProvider);
		}

		~FCrowdyMapProfileScopedSettings()
		{
			Settings->MapProfiles = SavedMapProfiles;
			Settings->DefaultProfile = SavedDefaultProfile;

			if (!SavedShippedProvider.IsEmpty())
				UCrowdySDKDeveloperSettings::RegisterShippedDefaultProfile(SavedShippedProvider,
					TSoftObjectPtr<UCrowdyMapProfile>(SavedShippedPath));
		}
	};
}

/**
 * The message itself: a running map that resolves nothing is named once, however many subsystems ask, and a
 * second map still speaks in its own right. Both worlds are unconfigured in the same way, so the only thing
 * separating "said" from "silent" is which world is asking and whether it has already been told.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMissingMapProfileReportedOncePerWorldTest,
	"CrowdySDK.MapProfile.MissingProfileReportedOncePerWorld",
	CrowdyMapProfileDefaultsTestFlags)

bool FCrowdyMissingMapProfileReportedOncePerWorldTest::RunTest(const FString& Parameters)
{
	FCrowdyMapProfileScopedSettings ScopedSettings;

	FCrowdyMapProfileScopedWorld FirstMap(EWorldType::Game);
	FCrowdyMapProfileScopedWorld SecondMap(EWorldType::Game);

	FCrowdyMapProfileLogCapture Capture;

	TestNull(TEXT("A map with nothing configured resolves no profile"),
		UCrowdySDKDeveloperSettings::ResolveProfileForWorld(FirstMap.World));

	const TArray<FString> FirstLines = Capture.TakeProfileWarnings();
	TestEqual(TEXT("The map is named exactly once"), FirstLines.Num(), 1);

	if (FirstLines.Num() == 1)
	{
		const FString MapName = FPackageName::GetShortName(FirstMap.World->GetOutermost()->GetName());
		TestTrue(TEXT("The line names the map that resolved nothing"), FirstLines[0].Contains(MapName));
		TestTrue(TEXT("The line says Default Profile is the setting that is unset"),
			FirstLines[0].Contains(TEXT("Default Profile is unset")));
		TestTrue(TEXT("The line names where the setting lives"),
			FirstLines[0].Contains(TEXT("Crowdy SDK -> Map Profiles")));
	}

	// Every Crowdy subsystem on a map resolves the profile during its own startup, so the second and later
	// asks are the normal case rather than a contrived repeat.
	for (int32 Ask = 0; Ask < 6; Ask++)
		UCrowdySDKDeveloperSettings::ResolveProfileForWorld(FirstMap.World);

	TestEqual(TEXT("Later subsystems asking about the same map are answered in silence"),
		Capture.TakeProfileWarnings().Num(), 0);

	TestNull(TEXT("A second map with nothing configured also resolves no profile"),
		UCrowdySDKDeveloperSettings::ResolveProfileForWorld(SecondMap.World));

	const TArray<FString> SecondLines = Capture.TakeProfileWarnings();
	TestEqual(TEXT("A different map is reported in its own right"), SecondLines.Num(), 1);

	if (SecondLines.Num() == 1)
	{
		const FString SecondMapName = FPackageName::GetShortName(SecondMap.World->GetOutermost()->GetName());
		TestTrue(TEXT("The second line names the second map"), SecondLines[0].Contains(SecondMapName));
	}

	return true;
}

/**
 * The silent half, which is what stops the warning from being a line that always prints. A map that resolves
 * a profile is not warned about, and neither is a world of a type the SDK never runs in, because nothing is
 * inactive there for a developer to fix.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyResolvedMapProfileIsSilentTest,
	"CrowdySDK.MapProfile.ResolvedProfileIsSilent",
	CrowdyMapProfileDefaultsTestFlags)

bool FCrowdyResolvedMapProfileIsSilentTest::RunTest(const FString& Parameters)
{
	FCrowdyMapProfileScopedSettings ScopedSettings;

	const TStrongObjectPtr<UCrowdyMapProfile> Profile(NewObject<UCrowdyMapProfile>());
	ScopedSettings.Settings->DefaultProfile = Profile.Get();

	FCrowdyMapProfileScopedWorld CoveredMap(EWorldType::Game);
	FCrowdyMapProfileLogCapture Capture;

	TestTrue(TEXT("A map with no entry of its own falls back to Default Profile"),
		UCrowdySDKDeveloperSettings::ResolveProfileForWorld(CoveredMap.World) == Profile.Get());
	TestEqual(TEXT("A map that resolves a profile is not warned about"),
		Capture.TakeProfileWarnings().Num(), 0);

	// A world the SDK does not run in has nothing switched off in it, so it has nothing to be told. Every
	// Crowdy world subsystem restricts itself to Game and PIE, which is what makes this the right silence.
	ScopedSettings.Settings->DefaultProfile.Reset();

	FCrowdyMapProfileScopedWorld EditorMap(EWorldType::Editor);

	TestNull(TEXT("An editor world resolves no profile either"),
		UCrowdySDKDeveloperSettings::ResolveProfileForWorld(EditorMap.World));
	TestEqual(TEXT("An editor world is not warned about"), Capture.TakeProfileWarnings().Num(), 0);

	return true;
}

/**
 * A profile that never chose a backend used to leave the map with none, so remote entities were tracked and
 * then drawn by nothing. It now starts on the actor-pool backend, which is the one the SDK ships.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBackendClassDefaultsToActorPoolTest,
	"CrowdySDK.MapProfile.BackendClassDefaultsToActorPool",
	CrowdyMapProfileDefaultsTestFlags)

bool FCrowdyBackendClassDefaultsToActorPoolTest::RunTest(const FString& Parameters)
{
	const FCrowdyActorManagementConfigStruct FreshConfig;

	TestSamePtr(TEXT("A profile that chose no backend starts on the actor-pool backend"),
		FreshConfig.BackendClass.Get(), UCrowdyActorPoolBackend::StaticClass());

	// A new profile asset is built from this same default, so the class the manager reads is loadable rather
	// than merely non-null.
	TestTrue(TEXT("The default backend class is a rendering backend the manager can instantiate"),
		FreshConfig.BackendClass->IsChildOf(UCrowdyRenderingBackend::StaticClass()));

	const UCrowdyMapProfile* ProfileDefaults = GetDefault<UCrowdyMapProfile>();
	TestSamePtr(TEXT("A newly created map profile carries the same default"),
		ProfileDefaults->ActorManagement.BackendClass.Get(), UCrowdyActorPoolBackend::StaticClass());

	return true;
}

/**
 * The case a reviewer asks for. Adding a C++ default changes what a saved profile loads as, because a value
 * equal to the default is not written to the asset at all. This runs that exact delta: the text is exported
 * against the OLD default (no backend), which is what an already-saved profile asset contains, and imported
 * onto a struct carrying the NEW default. A profile that named a backend must come back with it, and only a
 * profile that named none may pick the new default up.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExplicitBackendClassSurvivesTheNewDefaultTest,
	"CrowdySDK.MapProfile.ExplicitBackendClassSurvivesTheNewDefault",
	CrowdyMapProfileDefaultsTestFlags)

bool FCrowdyExplicitBackendClassSurvivesTheNewDefaultTest::RunTest(const FString& Parameters)
{
	UScriptStruct* ConfigStruct = FCrowdyActorManagementConfigStruct::StaticStruct();

	// What a profile asset saved before this change was compared against: a struct whose BackendClass is
	// unset. Anything matching it was left out of the asset entirely.
	FCrowdyActorManagementConfigStruct DefaultsAtSaveTime;
	DefaultsAtSaveTime.BackendClass = nullptr;

	// The only other rendering backend class this module can name. The case needs a value that is valid,
	// serialisable and not the new default; which backend it is does not matter.
	UClass* const ChosenBackend = UCrowdyRenderingBackend::StaticClass();

	FCrowdyActorManagementConfigStruct Chose;
	Chose.BackendClass = ChosenBackend;
	Chose.MaxTrackedActors = 777;

	FString ChoseText;
	ConfigStruct->ExportText(ChoseText, &Chose, &DefaultsAtSaveTime, nullptr, PPF_None, nullptr);
	TestTrue(TEXT("A profile that chose a backend wrote it to the asset"),
		ChoseText.Contains(TEXT("BackendClass")));

	FStringOutputDevice ChoseErrors;
	FCrowdyActorManagementConfigStruct ChoseLoaded;
	ConfigStruct->ImportText(*ChoseText, &ChoseLoaded, nullptr, PPF_None, &ChoseErrors,
		ConfigStruct->GetName());

	TestEqual(TEXT("Reading back a profile that chose a backend reports nothing wrong"), ChoseErrors.Len(), 0);
	TestSamePtr(TEXT("A profile that chose a backend keeps it, not the new default"),
		ChoseLoaded.BackendClass.Get(), ChosenBackend);
	TestEqual(TEXT("The rest of that profile is unaffected"), ChoseLoaded.MaxTrackedActors, 777);

	// The other half of the same delta, and the behaviour change this item is making: a profile that never
	// chose a backend wrote nothing, so it now loads as the actor-pool backend instead of as none.
	FCrowdyActorManagementConfigStruct ChoseNothing;
	ChoseNothing.BackendClass = nullptr;
	ChoseNothing.MaxTrackedActors = 512;

	FString ChoseNothingText;
	ConfigStruct->ExportText(ChoseNothingText, &ChoseNothing, &DefaultsAtSaveTime, nullptr, PPF_None, nullptr);
	TestFalse(TEXT("A profile that chose no backend wrote nothing about one"),
		ChoseNothingText.Contains(TEXT("BackendClass")));

	FStringOutputDevice ChoseNothingErrors;
	FCrowdyActorManagementConfigStruct ChoseNothingLoaded;
	ConfigStruct->ImportText(*ChoseNothingText, &ChoseNothingLoaded, nullptr, PPF_None, &ChoseNothingErrors,
		ConfigStruct->GetName());

	TestEqual(TEXT("Reading back a profile that chose no backend reports nothing wrong"),
		ChoseNothingErrors.Len(), 0);
	TestSamePtr(TEXT("A profile that chose no backend picks the new default up"),
		ChoseNothingLoaded.BackendClass.Get(), UCrowdyActorPoolBackend::StaticClass());
	TestEqual(TEXT("The rest of that profile is unaffected"), ChoseNothingLoaded.MaxTrackedActors, 512);

	return true;
}

/**
 * Naming a backend is not the same as configuring one. Defaulting BackendClass means every profile now
 * carries a backend that will be constructed, so a backend that cannot work with what the profile gave it
 * has to be refused rather than installed: installing it leaves every entity tracked, slotted and updated
 * with nothing drawn, which is the silence the default was added to remove.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBackendThatCannotInitializeIsRefusedTest,
	"CrowdySDK.MapProfile.BackendThatCannotInitializeIsRefused",
	CrowdyMapProfileDefaultsTestFlags)

bool FCrowdyBackendThatCannotInitializeIsRefusedTest::RunTest(const FString& Parameters)
{
	FCrowdyMapProfileScopedSettings ScopedSettings;
	FCrowdyMapProfileScopedWorld Map(EWorldType::Game);

	const TStrongObjectPtr<UCrowdyMapProfile> Profile(NewObject<UCrowdyMapProfile>());
	Profile->ActorManagement.BackendClass = UCrowdyRefusingBackend::StaticClass();
	ScopedSettings.Settings->DefaultProfile = Profile.Get();

	UCrowdyActorManager* Manager = NewObject<UCrowdyActorManager>(Map.World);

	TestFalse(TEXT("A backend that reports it cannot initialize is refused"), Manager->LoadConfigForTest());
	TestNull(TEXT("A refused backend is not installed"), Manager->GetActiveBackend());

	// The same profile with a backend that does initialize, so the refusal above is the backend's answer
	// being read rather than anything about this fixture failing to reach the load at all.
	Profile->ActorManagement.BackendClass = UCrowdyRecordingBackend::StaticClass();

	UCrowdyActorManager* SecondManager = NewObject<UCrowdyActorManager>(Map.World);

	TestTrue(TEXT("A backend that initializes is accepted"), SecondManager->LoadConfigForTest());
	TestNotNull(TEXT("An accepted backend is installed"), SecondManager->GetActiveBackend());

	return true;
}

/**
 * The shipped actor-pool backend is the one a profile that chose nothing now lands on. A missing config runs
 * on the shipped default (CrowdySDKShippedDefaultsTests covers that resolution); a config of another
 * backend's class is an authoring mistake, reported and refused rather than ensured, which is also what
 * makes it visible in a packaged build.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorPoolBackendRefusesAnUnusableConfigTest,
	"CrowdySDK.MapProfile.ActorPoolBackendRefusesAnUnusableConfig",
	CrowdyMapProfileDefaultsTestFlags)

bool FCrowdyActorPoolBackendRefusesAnUnusableConfigTest::RunTest(const FString& Parameters)
{
	FCrowdyMapProfileScopedWorld Map(EWorldType::Game);
	FCrowdyMapProfileLogCapture Capture;

	UCrowdyActorPoolBackend* Backend = NewObject<UCrowdyActorPoolBackend>(Map.World);
	UCrowdyRenderingBackendConfig* WrongType = NewObject<UCrowdyUnrelatedBackendConfig>(Map.World);

	TestFalse(TEXT("The actor-pool backend refuses a config of the wrong type"),
		Backend->InitializeBackend(Map.World, WrongType));

	// The remedy has to name the setting the author has to fill in. A refusal that only says it failed sends
	// the reader looking for a bug instead of a field.
	TestTrue(TEXT("The refusal names the setting to fill in"),
		Capture.TakeLinesContaining(TEXT("Backend Config")).Num() > 0);

	return true;
}

#endif
