// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Engine/Blueprint.h"
#include "GameModel/CrowdyContainerBlueprintExtension.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyContainerMarkerTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// The pure container type-name resolution: an authored override wins (trimmed); an empty or whitespace override
// defaults to the Blueprint's asset name. This is the rule the compiler extension stamps and the toolbar field
// shows, so a duplicated container (empty override) re-derives a clean name from its own asset instead of
// colliding with the original.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContainerMarkerResolveTest,
	"CrowdySDK.GameModel.ContainerMarkerResolve", CrowdyContainerMarkerTestFlags)
bool FCrowdyContainerMarkerResolveTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyContainerMarker;

	TestEqual(TEXT("empty override defaults to the asset name"),
		ResolveTypeName(FString(), TEXT("BP_PlayerStats")), FString(TEXT("BP_PlayerStats")));
	TestEqual(TEXT("whitespace override defaults to the asset name"),
		ResolveTypeName(TEXT("   "), TEXT("BP_Chest")), FString(TEXT("BP_Chest")));
	TestEqual(TEXT("a non-empty override wins"),
		ResolveTypeName(TEXT("PlayerStats"), TEXT("BP_PlayerStats")), FString(TEXT("PlayerStats")));
	TestEqual(TEXT("an override is trimmed"),
		ResolveTypeName(TEXT("  Chest  "), TEXT("BP_Chest")), FString(TEXT("Chest")));

	return true;
}

// The marker read/resolve/gate path on real Blueprints: adding the marker makes the class a container (and drives
// the dropdown gate IsGameModelContainerClass) and resolves to the asset name by default; an authored
// ContainerTypeName overrides it; a second defaulted container resolves to ITS OWN asset name (duplicate-safety);
// removing the marker un-marks it (gate false again), at which point the compiler extension removes the class tag.
// Exercised by adding/removing the extension directly, without SetMarked's structural recompile side effects,
// which are thin editor glue over engine calls (mirrors how RecompileForMetaChange is likewise not unit-tested).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContainerMarkerExtensionTest,
	"CrowdySDK.GameModel.ContainerMarkerExtension", CrowdyContainerMarkerTestFlags)
bool FCrowdyContainerMarkerExtensionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyContainerMarker;

	// Auto-named transient Blueprints (no fixed name), so the test is safe to re-run in one session; the
	// assertions compare against each object's own GetName() rather than a hard-coded string.
	UBlueprint* BlueprintA = NewObject<UBlueprint>(GetTransientPackage());
	UBlueprint* BlueprintB = NewObject<UBlueprint>(GetTransientPackage());
	if (!TestNotNull(TEXT("BlueprintA"), BlueprintA) || !TestNotNull(TEXT("BlueprintB"), BlueprintB))
	{
		return false;
	}

	// Unmarked by default: no marker, and container-hood resolves to empty (so the stamp removes any tag).
	TestFalse(TEXT("A is not marked initially"), IsMarkedContainer(BlueprintA));
	TestNull(TEXT("A has no marker initially"), FindExtension(BlueprintA));
	TestTrue(TEXT("an unmarked class resolves to no container type"), ResolveContainerTypeName(BlueprintA).IsEmpty());
	TestFalse(TEXT("an unmarked plain Blueprint fails the container-class gate"), IsGameModelContainerClass(BlueprintA));

	// Mark A by adding the marker directly.
	UCrowdyContainerBlueprintExtension* MarkerA = NewObject<UCrowdyContainerBlueprintExtension>(BlueprintA);
	BlueprintA->AddExtension(MarkerA);

	TestTrue(TEXT("A is marked once the marker is added"), IsMarkedContainer(BlueprintA));
	TestTrue(TEXT("a marked Blueprint passes the container-class gate before any compile"),
		IsGameModelContainerClass(BlueprintA));
	TestTrue(TEXT("A finds the marker it was given"), FindExtension(BlueprintA) == MarkerA);
	TestEqual(TEXT("A defaults its container type to its asset name"),
		ResolveContainerTypeName(BlueprintA), BlueprintA->GetName());

	// An authored override wins over the asset-name default.
	MarkerA->ContainerTypeName = TEXT("PlayerStats");
	TestEqual(TEXT("A uses its authored override"),
		ResolveContainerTypeName(BlueprintA), FString(TEXT("PlayerStats")));

	// A second, defaulted container resolves to ITS OWN asset name, distinct from A's, so a duplicate does not
	// collide with the original's type.
	UCrowdyContainerBlueprintExtension* MarkerB = NewObject<UCrowdyContainerBlueprintExtension>(BlueprintB);
	BlueprintB->AddExtension(MarkerB);
	TestEqual(TEXT("B defaults to its own asset name"),
		ResolveContainerTypeName(BlueprintB), BlueprintB->GetName());
	TestNotEqual(TEXT("B's default type differs from A's asset name"),
		ResolveContainerTypeName(BlueprintB), BlueprintA->GetName());

	// Un-mark A: removing the marker clears container-hood, so the compiler extension removes the class tag.
	BlueprintA->RemoveExtension(MarkerA);
	TestFalse(TEXT("A is unmarked after the marker is removed"), IsMarkedContainer(BlueprintA));
	TestFalse(TEXT("A fails the container-class gate again after unmarking"), IsGameModelContainerClass(BlueprintA));
	TestTrue(TEXT("an unmarked class again resolves to no container type"),
		ResolveContainerTypeName(BlueprintA).IsEmpty());

	return true;
}

// The pull-on-start decision the compiler extension stamps from: only a marked container whose author turned the
// pull off asks for the "False" tag. An unmarked class and a container left at the default both answer false, so
// the tag stays off every container that never opts out, and turning the pull back on (or unmarking the class)
// takes the un-stamp arm that clears a stale tag.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContainerMarkerPullOnStartTest,
	"CrowdySDK.GameModel.ContainerMarkerPullOnStart", CrowdyContainerMarkerTestFlags)
bool FCrowdyContainerMarkerPullOnStartTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyContainerMarker;

	UBlueprint* Blueprint = NewObject<UBlueprint>(GetTransientPackage());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}

	// Unmarked: nothing to stamp, and the authoring surface reads the default a container starts in.
	TestFalse(TEXT("an unmarked class never stamps the pull-off tag"), ShouldStampPullOnStartOff(Blueprint));
	TestTrue(TEXT("an unmarked class reads the on-by-default pull"), GetPullModelOnStart(Blueprint));

	UCrowdyContainerBlueprintExtension* Marker = NewObject<UCrowdyContainerBlueprintExtension>(Blueprint);
	Blueprint->AddExtension(Marker);

	// Marked and left alone: pulling on bind is the default, so no tag.
	TestTrue(TEXT("a new marker defaults its pull on start to on"), Marker->bPullModelOnStart);
	TestTrue(TEXT("a marked container reads its pull as on"), GetPullModelOnStart(Blueprint));
	TestFalse(TEXT("a marked container at the default stamps no tag"), ShouldStampPullOnStartOff(Blueprint));

	// Marked with the pull turned off: the one case that writes the tag.
	Marker->bPullModelOnStart = false;
	TestFalse(TEXT("a marked container reads its pull as off once turned off"), GetPullModelOnStart(Blueprint));
	TestTrue(TEXT("a marked container with the pull off stamps the tag"), ShouldStampPullOnStartOff(Blueprint));

	// Turned back on: the un-stamp arm clears the tag again.
	Marker->bPullModelOnStart = true;
	TestFalse(TEXT("turning the pull back on stops stamping the tag"), ShouldStampPullOnStartOff(Blueprint));

	// Unmarked while the pull was off: unmarking alone clears the tag, so no stale opt-out survives.
	Marker->bPullModelOnStart = false;
	Blueprint->RemoveExtension(Marker);
	TestFalse(TEXT("unmarking clears the pull-off stamp"), ShouldStampPullOnStartOff(Blueprint));
	TestTrue(TEXT("an unmarked class reads the default pull again"), GetPullModelOnStart(Blueprint));

	return true;
}

// A marked container states its pull setting outright instead of leaving the tag off when it is on. The lookup
// takes the nearest answer up the class chain, so a container left at the default and derived from one that turned
// the pull off would otherwise inherit the parent's answer while its own setting showed the pull enabled.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContainerMarkerPullOnStartStampTest,
	"CrowdySDK.GameModel.ContainerMarkerPullOnStartStamp", CrowdyContainerMarkerTestFlags)
bool FCrowdyContainerMarkerPullOnStartStampTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyContainerMarker;

	UBlueprint* Blueprint = NewObject<UBlueprint>(GetTransientPackage());
	if (!TestNotNull(TEXT("Blueprint"), Blueprint))
	{
		return false;
	}

	TestTrue(TEXT("an unmarked class writes no tag at all"),
		ResolvePullOnStartStamp(Blueprint) == ECrowdyPullOnStartStamp::None);

	UCrowdyContainerBlueprintExtension* Marker = NewObject<UCrowdyContainerBlueprintExtension>(Blueprint);
	Blueprint->AddExtension(Marker);

	TestTrue(TEXT("a marked container at the default states the pull is on"),
		ResolvePullOnStartStamp(Blueprint) == ECrowdyPullOnStartStamp::On);

	Marker->bPullModelOnStart = false;
	TestTrue(TEXT("a marked container with the pull off states that"),
		ResolvePullOnStartStamp(Blueprint) == ECrowdyPullOnStartStamp::Off);

	Marker->bPullModelOnStart = true;
	TestTrue(TEXT("turning the pull back on states the pull is on rather than clearing the tag"),
		ResolvePullOnStartStamp(Blueprint) == ECrowdyPullOnStartStamp::On);

	Blueprint->RemoveExtension(Marker);
	TestTrue(TEXT("unmarking goes back to writing no tag"),
		ResolvePullOnStartStamp(Blueprint) == ECrowdyPullOnStartStamp::None);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
