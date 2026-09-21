// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

#include "Data/CrowdyActorPoolBackend.h"
#include "Data/CrowdyActorPoolBackendConfig.h"
#include "Data/CrowdyMapProfile.h"
#include "Data/CrowdyRenderingBackendTestTypes.h"
#include "Data/CrowdyTransformRepPolicy.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Replication/Executor/ActorUpdateExecutor.h"
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

/**
 * What the actor pool backend runs on when the profile carries no Backend Config, which the shipped profile
 * does not. Before this, the backend refused to start and every remote entity replicated into nothing;
 * the resolution now hands it a transient config and the shipped transform policy. A config of another
 * backend's class is still refused, so the fallback does not paper over a real mistake.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySDKActorPoolRunsWithoutAConfigTest,
	"CrowdySDK.MapProfile.ActorPoolRunsWithoutAConfig",
	CrowdySDKShippedDefaultsTestFlags)

bool FCrowdySDKActorPoolRunsWithoutAConfigTest::RunTest(const FString& Parameters)
{
	UCrowdyActorPoolBackend* const Backend = NewObject<UCrowdyActorPoolBackend>(GetTransientPackage());

	UCrowdyActorPoolBackendConfig* const Defaulted = UCrowdyActorPoolBackend::ResolveConfig(nullptr, Backend);
	if (!TestNotNull(TEXT("No Backend Config resolves to a transient one rather than a refusal"), Defaulted))
	{
		return false;
	}
	TestEqual(TEXT("and that config, naming no policy, runs on the shipped transform policy"),
		UCrowdyActorPoolBackend::ResolvePolicyClass(Defaulted), UCrowdyTransformRepPolicy::StaticClass());

	UCrowdyActorPoolBackendConfig* const Authored = NewObject<UCrowdyActorPoolBackendConfig>(Backend);
	Authored->ReplicationPolicyClass = UCrowdyInertRepPolicy::StaticClass();
	TestEqual(TEXT("CONTROL: a config that names a policy keeps it"),
		UCrowdyActorPoolBackend::ResolvePolicyClass(Authored), UCrowdyInertRepPolicy::StaticClass());
	TestEqual(TEXT("and a config of the right class is used as given"),
		UCrowdyActorPoolBackend::ResolveConfig(Authored, Backend), Authored);

	AddExpectedErrorPlain(TEXT("needs a CrowdyActorPoolBackendConfig"), EAutomationExpectedMessageFlags::Contains, 1);
	UCrowdyRenderingBackendConfig* const Foreign = NewObject<UCrowdyUnrelatedBackendConfig>(Backend);
	TestNull(TEXT("A config of another backend's class is still refused"),
		UCrowdyActorPoolBackend::ResolveConfig(Foreign, Backend));

	return true;
}

/**
 * The shipped policy has to move the actor the way the sample says, or the default draws entities that stand
 * still while their state changes. Two samples 100 ms apart, read halfway: the midpoint, not either end; past
 * the newest, extrapolation stops at 0.2 s; with three samples the bracketing pair is the one read.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySDKTransformPolicyInterpolatesTest,
	"CrowdySDK.MapProfile.TransformPolicyInterpolatesActorState",
	CrowdySDKShippedDefaultsTestFlags)

bool FCrowdySDKTransformPolicyInterpolatesTest::RunTest(const FString& Parameters)
{
	UWorld* const World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(/*bInformEngineOfWorld=*/false);
	};

	AActor* const Actor = World->SpawnActor<AActor>();
	USceneComponent* const Root = NewObject<USceneComponent>(Actor);
	Actor->SetRootComponent(Root);
	Root->RegisterComponent();

	UCrowdyTransformRepPolicy* const Policy = NewObject<UCrowdyTransformRepPolicy>(GetTransientPackage());

	FCrowdyActorState First;
	First.Location = FVector(0.0, 0.0, 0.0);
	FCrowdyActorState Second;
	Second.Location = FVector(100.0, 0.0, 0.0);
	Second.Rotation = FRotator(0.0, 90.0, 0.0);

	TestTrue(TEXT("The first sample is accepted"),
		Policy->ExtractFields(FInstancedStruct::Make(First), 1000, /*SlotId=*/3));
	TestTrue(TEXT("The second sample is accepted"),
		Policy->ExtractFields(FInstancedStruct::Make(Second), 1100, /*SlotId=*/3));

	Policy->ApplyToActor(Actor, 3, 1050);
	TestEqual(TEXT("Halfway between two samples the actor sits at the midpoint"),
		Actor->GetActorLocation(), FVector(50.0, 0.0, 0.0));
	TestEqual(TEXT("and has turned halfway"), Actor->GetActorRotation().Yaw, 45.0, 0.01);

	Policy->ApplyToActor(Actor, 3, 1300);
	const FVector At1300 = Actor->GetActorLocation();
	Policy->ApplyToActor(Actor, 3, 5000);
	TestEqual(TEXT("Extrapolation is capped at 0.2 s past the newest sample, so 5000 ms reads as 1300 ms"),
		Actor->GetActorLocation(), At1300);
	TestNotEqual(TEXT("and the capped value is extrapolated, not the newest sample held"),
		At1300, FVector(100.0, 0.0, 0.0));
	TestEqual(TEXT("and the capped value is the pair's slope carried 0.2 s past the newest sample"),
		At1300, FVector(300.0, 0.0, 0.0));

	TestFalse(TEXT("A state struct this policy cannot read is refused, so the backend skips the update"),
		Policy->ExtractFields(FInstancedStruct::Make(FVector::ZeroVector), 1200, 3));

	FCrowdyActorState Third;
	Third.Location = FVector(400.0, 0.0, 0.0);
	Policy->ExtractFields(FInstancedStruct::Make(First), 1000, /*SlotId=*/5);
	Policy->ExtractFields(FInstancedStruct::Make(Second), 1100, /*SlotId=*/5);
	Policy->ExtractFields(FInstancedStruct::Make(Third), 1200, /*SlotId=*/5);

	Policy->ApplyToActor(Actor, 5, 1150);
	TestEqual(TEXT("With three samples the render time picks the pair that brackets it"),
		Actor->GetActorLocation(), FVector(250.0, 0.0, 0.0));
	Policy->ApplyToActor(Actor, 5, 1050);
	TestEqual(TEXT("and an older pair is still there to bracket an earlier render time"),
		Actor->GetActorLocation(), FVector(50.0, 0.0, 0.0));

	return true;
}

#endif
