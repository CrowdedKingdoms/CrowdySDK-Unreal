// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Backend/CrowdyMassRenderingBackend.h"
#include "Data/CrowdyActorPoolBackend.h"
#include "Data/CrowdyMapProfile.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Events/CrowdyMassEventLibrary.h"
#include "Mass/CrowdyMassRowEventTestTypes.h"
#include "Misc/AutomationTest.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/FCrowdyRpcTarget.h"
#include "Spawners/CrowdyMassAreaSpawner.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "Subsystems/CrowdyMassServerRecordsSubsystem.h"
#include "Subsystems/CrowdyMassStateLogCapture.h"
#include "Target/CrowdyMassEntityTarget.h"
#include "Target/CrowdyMassEntityTargetResolver.h"
#include "Target/CrowdyMassServerValuesLibrary.h"
#include "Trace/CrowdyMassTraceBlueprintLibrary.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

// The Mass world subsystems exist only in a Game or PIE world whose own map profile selects a backend that
// draws entities as crowd rows. These cases ask the subsystems' class defaults the same question the engine
// asks while it builds a world, against editor worlds relabelled as Game or PIE so that no real subsystem
// is ever created.
namespace CrowdyMassWorldGateTest
{
	constexpr EAutomationTestFlags TestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const TCHAR* const ShippedProviderName = TEXT("CrowdyMassWorldGateTestPlugin");

	// A world with a name of its own, so a Map Profiles entry can name it. The name is unique per world
	// because a destroyed world's package can outlive it until the next collection.
	struct FScopedWorld
	{
		UWorld* World = nullptr;
		FString MapName;

		explicit FScopedWorld(const EWorldType::Type PresentedType)
			: MapName(FString::Printf(TEXT("CrowdyMassWorldGate_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
		{
			UPackage* Package = CreatePackage(*(TEXT("/Temp/") + MapName));
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, FName(*MapName), Package);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);

			World->WorldType = PresentedType;
		}

		~FScopedWorld()
		{
			if (!World)
				return;

			World->WorldType = EWorldType::Editor;

			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}

		TSoftObjectPtr<UWorld> AsMapKey() const
		{
			return TSoftObjectPtr<UWorld>(FSoftObjectPath(FString::Printf(TEXT("/Temp/%s.%s"), *MapName, *MapName)));
		}
	};

	// The developer settings and the shipped-profile offer are process-wide, so a case that changes them puts
	// both back or every later test resolves against what it left behind.
	struct FScopedSettings
	{
		UCrowdySDKDeveloperSettings* Settings = GetMutableDefault<UCrowdySDKDeveloperSettings>();
		TMap<TSoftObjectPtr<UWorld>, TSoftObjectPtr<UCrowdyMapProfile>> SavedMapProfiles;
		TSoftObjectPtr<UCrowdyMapProfile> SavedDefaultProfile;
		FString SavedShippedProvider;
		FSoftObjectPath SavedShippedPath;

		FScopedSettings()
		{
			SavedMapProfiles = Settings->MapProfiles;
			SavedDefaultProfile = Settings->DefaultProfile;
			SavedShippedProvider = UCrowdySDKDeveloperSettings::GetShippedDefaultProfileProvider();
			SavedShippedPath = UCrowdySDKDeveloperSettings::GetShippedDefaultProfilePath();

			Settings->MapProfiles.Empty();
			Settings->DefaultProfile.Reset();
			UCrowdySDKDeveloperSettings::UnregisterShippedDefaultProfile(SavedShippedProvider);
		}

		~FScopedSettings()
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

	TStrongObjectPtr<UCrowdyMapProfile> MakeProfile(const TSubclassOf<UCrowdyRenderingBackend> BackendClass)
	{
		TStrongObjectPtr<UCrowdyMapProfile> Profile(NewObject<UCrowdyMapProfile>());
		Profile->ActorManagement.BackendClass = BackendClass;
		return Profile;
	}

	bool EntitySubsystemWouldBeCreated(UWorld* World)
	{
		return GetDefault<UCrowdyMassEntitySubsystem>()->ShouldCreateSubsystem(World);
	}

	bool ServerRecordsWouldBeCreated(UWorld* World)
	{
		return GetDefault<UCrowdyMassServerRecordsSubsystem>()->ShouldCreateSubsystem(World);
	}
}

/**
 * The positive half: a Game world and a PIE world whose map resolves a Mass profile each get both subsystems.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassWorldGateCreatedWhereThisMapDrawsCrowdRowsTest,
	"CrowdySDK.Mass.WorldGate.CreatedWhereThisMapDrawsCrowdRows", CrowdyMassWorldGateTest::TestFlags)
bool FCrowdyMassWorldGateCreatedWhereThisMapDrawsCrowdRowsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassWorldGateTest;

	FScopedSettings ScopedSettings;
	const TStrongObjectPtr<UCrowdyMapProfile> MassProfile = MakeProfile(UCrowdyMassRenderingBackend::StaticClass());
	ScopedSettings.Settings->DefaultProfile = MassProfile.Get();

	FScopedWorld GameMap(EWorldType::Game);
	FScopedWorld PieMap(EWorldType::PIE);

	TestTrue(TEXT("the entity subsystem is created in a Game world on a Mass profile"),
		EntitySubsystemWouldBeCreated(GameMap.World));
	TestTrue(TEXT("the entity subsystem is created in a PIE world on a Mass profile"),
		EntitySubsystemWouldBeCreated(PieMap.World));
	TestTrue(TEXT("the server records subsystem is created in a Game world on a Mass profile"),
		ServerRecordsWouldBeCreated(GameMap.World));
	TestTrue(TEXT("the server records subsystem is created in a PIE world on a Mass profile"),
		ServerRecordsWouldBeCreated(PieMap.World));

	return true;
}

/**
 * The gate reads THIS world's profile. A Mass Default Profile does not reach a map that has an actor-pool
 * entry of its own, which is the case a project-wide "is any profile Mass" question would get wrong, and a
 * neighbouring map with no entry still takes the Mass default so the case cannot pass by the gate being shut.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassWorldGateNotCreatedOnActorPoolMapTest,
	"CrowdySDK.Mass.WorldGate.NotCreatedOnActorPoolMap", CrowdyMassWorldGateTest::TestFlags)
bool FCrowdyMassWorldGateNotCreatedOnActorPoolMapTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassWorldGateTest;

	FScopedSettings ScopedSettings;
	const TStrongObjectPtr<UCrowdyMapProfile> ActorPoolProfile = MakeProfile(UCrowdyActorPoolBackend::StaticClass());
	const TStrongObjectPtr<UCrowdyMapProfile> MassProfile = MakeProfile(UCrowdyMassRenderingBackend::StaticClass());

	ScopedSettings.Settings->DefaultProfile = ActorPoolProfile.Get();

	{
		FScopedWorld GameMap(EWorldType::Game);
		FScopedWorld PieMap(EWorldType::PIE);

		TestFalse(TEXT("no entity subsystem in a Game world on an actor-pool Default Profile"),
			EntitySubsystemWouldBeCreated(GameMap.World));
		TestFalse(TEXT("no entity subsystem in a PIE world on an actor-pool Default Profile"),
			EntitySubsystemWouldBeCreated(PieMap.World));
		TestFalse(TEXT("no server records subsystem in a Game world on an actor-pool Default Profile"),
			ServerRecordsWouldBeCreated(GameMap.World));
		TestFalse(TEXT("no server records subsystem in a PIE world on an actor-pool Default Profile"),
			ServerRecordsWouldBeCreated(PieMap.World));
	}

	ScopedSettings.Settings->DefaultProfile = MassProfile.Get();

	FScopedWorld OverriddenMap(EWorldType::Game);
	FScopedWorld NeighbourMap(EWorldType::Game);
	ScopedSettings.Settings->MapProfiles.Add(OverriddenMap.AsMapKey(), TSoftObjectPtr<UCrowdyMapProfile>(ActorPoolProfile.Get()));

	TestTrue(TEXT("the override is what this map resolves"),
		UCrowdySDKDeveloperSettings::ResolveProfileForWorld(OverriddenMap.World) == ActorPoolProfile.Get());
	TestFalse(TEXT("no entity subsystem on a map whose own entry is actor pool, whatever the Mass default says"),
		EntitySubsystemWouldBeCreated(OverriddenMap.World));
	TestFalse(TEXT("no server records subsystem on a map whose own entry is actor pool, whatever the Mass default says"),
		ServerRecordsWouldBeCreated(OverriddenMap.World));

	TestTrue(TEXT("a neighbouring map with no entry still takes the Mass default and gets the entity subsystem"),
		EntitySubsystemWouldBeCreated(NeighbourMap.World));

	return true;
}

/**
 * A project that configures nothing runs on the profile a plugin ships, which draws actors, so it gets no
 * Mass subsystem. The profile is a default-constructed one, so this also pins that a profile that never
 * chose a backend does not draw crowd rows.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassWorldGateShippedDefaultDrawsWithActorsTest,
	"CrowdySDK.Mass.WorldGate.ShippedDefaultDrawsWithActors", CrowdyMassWorldGateTest::TestFlags)
bool FCrowdyMassWorldGateShippedDefaultDrawsWithActorsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassWorldGateTest;

	FScopedSettings ScopedSettings;
	const TStrongObjectPtr<UCrowdyMapProfile> Shipped(NewObject<UCrowdyMapProfile>());
	UCrowdySDKDeveloperSettings::RegisterShippedDefaultProfile(ShippedProviderName,
		TSoftObjectPtr<UCrowdyMapProfile>(Shipped.Get()));

	FScopedWorld GameMap(EWorldType::Game);
	FScopedWorld PieMap(EWorldType::PIE);

	TestTrue(TEXT("the unconfigured map resolves the shipped profile"),
		UCrowdySDKDeveloperSettings::ResolveProfileForWorld(GameMap.World) == Shipped.Get());

	TestFalse(TEXT("no entity subsystem in a Game world on the shipped profile"),
		EntitySubsystemWouldBeCreated(GameMap.World));
	TestFalse(TEXT("no entity subsystem in a PIE world on the shipped profile"),
		EntitySubsystemWouldBeCreated(PieMap.World));
	TestFalse(TEXT("no server records subsystem in a Game world on the shipped profile"),
		ServerRecordsWouldBeCreated(GameMap.World));
	TestFalse(TEXT("no server records subsystem in a PIE world on the shipped profile"),
		ServerRecordsWouldBeCreated(PieMap.World));

	return true;
}

/**
 * Only Game and PIE worlds can hold the subsystems, whatever their profile says. The resolver does answer
 * for an editor world, so it is the type check that refuses here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassWorldGateNotCreatedOutsidePlayTest,
	"CrowdySDK.Mass.WorldGate.NotCreatedOutsidePlay", CrowdyMassWorldGateTest::TestFlags)
bool FCrowdyMassWorldGateNotCreatedOutsidePlayTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassWorldGateTest;

	FScopedSettings ScopedSettings;
	const TStrongObjectPtr<UCrowdyMapProfile> MassProfile = MakeProfile(UCrowdyMassRenderingBackend::StaticClass());
	ScopedSettings.Settings->DefaultProfile = MassProfile.Get();

	FScopedWorld EditorMap(EWorldType::Editor);

	TestTrue(TEXT("the editor world resolves the Mass profile, so the type check is what refuses"),
		UCrowdySDKDeveloperSettings::ResolveProfileForWorld(EditorMap.World) == MassProfile.Get());
	TestFalse(TEXT("no entity subsystem in an editor world"), EntitySubsystemWouldBeCreated(EditorMap.World));
	TestFalse(TEXT("no server records subsystem in an editor world"), ServerRecordsWouldBeCreated(EditorMap.World));

	return true;
}

/**
 * The shared predicate answers false for everything the actor manager would not build a crowd-row backend
 * from: no profile at all, and a profile naming no backend.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassWorldGatePredicateRefusesWhatIsNotCrowdRowsTest,
	"CrowdySDK.Mass.WorldGate.PredicateRefusesWhatIsNotCrowdRows", CrowdyMassWorldGateTest::TestFlags)
bool FCrowdyMassWorldGatePredicateRefusesWhatIsNotCrowdRowsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassWorldGateTest;

	TestFalse(TEXT("no profile draws no crowd rows"), UCrowdySDKDeveloperSettings::ProfileDrawsCrowdRows(nullptr));

	const TStrongObjectPtr<UCrowdyMapProfile> NoBackend = MakeProfile(nullptr);
	TestFalse(TEXT("a profile naming no backend draws no crowd rows"),
		UCrowdySDKDeveloperSettings::ProfileDrawsCrowdRows(NoBackend.Get()));

	const TStrongObjectPtr<UCrowdyMapProfile> ActorPool = MakeProfile(UCrowdyActorPoolBackend::StaticClass());
	TestFalse(TEXT("the actor-pool backend draws no crowd rows"),
		UCrowdySDKDeveloperSettings::ProfileDrawsCrowdRows(ActorPool.Get()));

	const TStrongObjectPtr<UCrowdyMapProfile> Mass = MakeProfile(UCrowdyMassRenderingBackend::StaticClass());
	TestTrue(TEXT("the Mass backend draws crowd rows"), UCrowdySDKDeveloperSettings::ProfileDrawsCrowdRows(Mass.Get()));

	return true;
}

/**
 * A world with no Mass subsystems stands in for an actor-pool map. Everything that reaches for the
 * subsystems declines without a warning, except the two places where authored Mass content meets such a map:
 * a spawner in a play world that cannot spawn, and a backend that cannot initialize. Each of those warns once.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassWorldGateCallersDegradeQuietlyTest,
	"CrowdySDK.Mass.WorldGate.CallersDegradeQuietly", CrowdyMassWorldGateTest::TestFlags)
bool FCrowdyMassWorldGateCallersDegradeQuietlyTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyMassWorldGateTest;

	FScopedWorld Map(EWorldType::Editor);
	UWorld* World = Map.World;

	if (!TestNull(TEXT("the stand-in world holds no entity subsystem"), World->GetSubsystem<UCrowdyMassEntitySubsystem>()))
		return false;

	FCrowdyMassStateLogCapture Capture;

	UCrowdyMassBoundEventTestHandler* Handler = NewObject<UCrowdyMassBoundEventTestHandler>(World);
	UClass* EventClass = ACrowdyMassRowEventTestActor::StaticClass();
	const FName EventName(TEXT("PlayOneShot_Implementation"));
	const UFunction* EventFunction = FCrowdyRPC::ResolveFunction(EventClass, *EventName.ToString());
	if (!TestNotNull(TEXT("the test event resolved"), EventFunction))
		return false;

	TestFalse(TEXT("unbinding an event on a world with no subsystem returns false"),
		UCrowdyMassEventLibrary::CrowdyMassUnbindEventOnEntity(Handler, TEXT("OnPlayOneShot"), EventClass, EventName));
	TestEqual(TEXT("and writes no warning"), Capture.TakeWarnings().Num(), 0);
	TestEqual(TEXT("and no log line"), Capture.TakeLogs().Num(), 0);

	const int64 Signature = FCrowdyRPC::ComputeFunctionID(EventFunction);
	TestFalse(TEXT("binding an event on a world with no subsystem returns false"),
		UCrowdyMassEventLibrary::CrowdyMassBindEventOnEntity(Handler, TEXT("OnPlayOneShot"), EventClass, EventName, Signature));
	TestFalse(TEXT("and does so again"),
		UCrowdyMassEventLibrary::CrowdyMassBindEventOnEntity(Handler, TEXT("OnPlayOneShot"), EventClass, EventName, Signature));
	TestEqual(TEXT("neither bind writes a warning"), Capture.TakeWarnings().Num(), 0);
	TestEqual(TEXT("the world is told once at log level, not once per bind"),
		CrowdyMassCountLinesContaining(Capture.TakeLogs(), TEXT("has no Crowdy Mass entity subsystem")), 1);

	TestFalse(TEXT("binding server values on a world with no records subsystem returns false"),
		UCrowdyMassServerValuesLibrary::CrowdyMassBindServerValuesChangedOnEntity(Handler, TEXT("OnChanged"), EventClass));
	TestFalse(TEXT("and does so again"),
		UCrowdyMassServerValuesLibrary::CrowdyMassBindServerValuesChangedOnEntity(Handler, TEXT("OnChanged"), EventClass));
	TestEqual(TEXT("neither server values bind writes a warning"), Capture.TakeWarnings().Num(), 0);
	TestEqual(TEXT("the world is told once at log level that it has no crowd server records"),
		CrowdyMassCountLinesContaining(Capture.TakeLogs(), TEXT("has no crowd server records")), 1);

	FCrowdyMassEntityTarget Target;
	Target.Entity = FMassEntityHandle(1, 1);
	Target.World = World;

	const FProperty* DestProperty = ACrowdyMassAreaSpawner::StaticClass()->FindPropertyByName(TEXT("EntityCount"));
	if (!TestNotNull(TEXT("an int property to read into exists"), DestProperty))
		return false;

	int32 Dest = 7;
	TestFalse(TEXT("reading a server value on a world with no records subsystem returns false"),
		UCrowdyMassServerValuesLibrary::GetServerValueInto(Target, TEXT("Any"), DestProperty, &Dest));
	TestEqual(TEXT("and clears the destination"), Dest, 0);

	FCrowdyRpcTarget RpcTarget;
	TestTrue(TEXT("resolving a target on a world with no entity manager answers NoWorld"),
		CrowdyMassTarget::ResolveRpcTarget(Target, RpcTarget) == CrowdyMassTarget::EResolveResult::NoWorld);
	TestEqual(TEXT("the reads and the resolve write no warning"), Capture.TakeWarnings().Num(), 0);

	ACrowdyMassAreaSpawner* Spawner = World->SpawnActor<ACrowdyMassAreaSpawner>();
	if (!TestNotNull(TEXT("the spawner was placed"), Spawner))
		return false;

	TestEqual(TEXT("a spawner in an editor world spawns nothing"), Spawner->SpawnEntities(), 0);
	TestEqual(TEXT("and says nothing, because an editor world never holds the subsystem"), Capture.TakeWarnings().Num(), 0);

	FScopedWorld GameMap(EWorldType::Game);
	ACrowdyMassAreaSpawner* PlaySpawner = GameMap.World->SpawnActor<ACrowdyMassAreaSpawner>();
	if (!TestNotNull(TEXT("the play-world spawner was placed"), PlaySpawner))
		return false;

	TestEqual(TEXT("a spawner on a play world with no entity subsystem spawns nothing"), PlaySpawner->SpawnEntities(), 0);
	const TArray<FString> SpawnerWarnings = Capture.TakeWarnings();
	TestEqual(TEXT("and warns once"), SpawnerWarnings.Num(), 1);
	TestEqual(TEXT("the warning names the profile"),
		CrowdyMassCountLinesContaining(SpawnerWarnings, TEXT("profile does not select the Mass backend")), 1);

	TestEqual(TEXT("a second spawn spawns nothing"), PlaySpawner->SpawnEntities(), 0);
	TestEqual(TEXT("and does not warn again"), Capture.TakeWarnings().Num(), 0);

	UCrowdyMassTraceBlueprintLibrary::SetHitZoneTable(GameMap.World, nullptr);
	UCrowdyMassTraceBlueprintLibrary::SetHitZoneTable(GameMap.World, nullptr);
	TestEqual(TEXT("setting the hit zone table on a play world with no entity subsystem warns once"),
		CrowdyMassCountLinesContaining(Capture.TakeWarnings(), TEXT("hit zones need a map whose profile selects the Mass backend")), 1);

	UCrowdyMassRenderingBackend* Backend = NewObject<UCrowdyMassRenderingBackend>(GetTransientPackage());
	TestFalse(TEXT("the Mass backend refuses to initialize on a world with no entity subsystem"),
		Backend->InitializeBackend(World, nullptr));
	const TArray<FString> BackendWarnings = Capture.TakeWarnings();
	TestEqual(TEXT("and warns once"), BackendWarnings.Num(), 1);
	TestEqual(TEXT("the warning names the missing subsystem"),
		CrowdyMassCountLinesContaining(BackendWarnings, TEXT("no UCrowdyMassEntitySubsystem")), 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
