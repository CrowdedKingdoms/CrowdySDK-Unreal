#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyGameModelPullOnBindTestTarget.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Utils/CrowdyBakedRegistry.h"

// A Game Model container's initial-bind pull is a setting of the container TYPE: the class carries
// meta=(CrowdyPullOnStart="False") when its author turned the pull off, and carries nothing at all otherwise. These
// cover the whole resolution: reading the tag off a class, inheriting it from a base, reading it back out of the
// baked table a packaged build uses instead, and the two participant-level gates built on it (first bind, and the
// retry that runs when a resolve completes). The gated call sites themselves cannot complete headlessly (no live
// API context), which is why the decision is factored out to be tested here directly.
namespace
{
	constexpr EAutomationTestFlags CrowdyPullOnBindTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// The tag as read off one class: present and "False" turns the pull off, everything else leaves it on. A class
// with no tag, a class that is no container at all, and a null class all mean "pulls", which is what keeps the tag
// off every container that never opts out.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullOnStartTagOnClassTest,
	"CrowdySDK.GameModel.PullOnStartTagOnClass", CrowdyPullOnBindTestFlags)
bool FCrowdyPullOnStartTagOnClassTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("a container tagged CrowdyPullOnStart=False does not pull"),
		UCrowdyBakedRegistry::ShouldPullModelOnStart(UCrowdyGameModelPullOffTarget::StaticClass()));
	TestTrue(TEXT("a container carrying no tag pulls"),
		UCrowdyBakedRegistry::ShouldPullModelOnStart(UCrowdyGameModelPullOnTarget::StaticClass()));
	TestTrue(TEXT("a class that is no container at all pulls"),
		UCrowdyBakedRegistry::ShouldPullModelOnStart(UCrowdyGameModelTestTarget::StaticClass()));
	TestTrue(TEXT("a null class pulls"),
		UCrowdyBakedRegistry::ShouldPullModelOnStart(nullptr));

	return true;
}

// Class metadata is not inherited, so the resolution walks the super chain itself: a subclass that restates
// nothing follows its base's opt-out, and one that declares its own value stops the walk there.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullOnStartInheritsFromBaseTest,
	"CrowdySDK.GameModel.PullOnStartInheritsFromBase", CrowdyPullOnBindTestFlags)
bool FCrowdyPullOnStartInheritsFromBaseTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("a container subclass with no tag of its own inherits its base's opt-out"),
		UCrowdyBakedRegistry::ShouldPullModelOnStart(UCrowdyGameModelPullInheritedTarget::StaticClass()));
	TestTrue(TEXT("a container subclass declaring True opts back in"),
		UCrowdyBakedRegistry::ShouldPullModelOnStart(UCrowdyGameModelPullReenabledTarget::StaticClass()));
	// The tag resolution answers for any class; excluding a non-container is the bind gate's job, not this one's.
	TestFalse(TEXT("the walk does not stop at the container tag, only at the pull tag"),
		UCrowdyBakedRegistry::ShouldPullModelOnStart(UCrowdyGameModelPullUntaggedSubclass::StaticClass()));

	return true;
}

// The cooked read: a packaged build has no class metadata, so the flag comes back out of the baked container
// entries. An entry written before the flag existed carries no value for it and must read as "pulls", which is
// what an absent tag means, and a class with no entry at all is simply unknown here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullOnStartBakeRoundTripTest,
	"CrowdySDK.GameModel.PullOnStartBakeRoundTrip", CrowdyPullOnBindTestFlags)
bool FCrowdyPullOnStartBakeRoundTripTest::RunTest(const FString& Parameters)
{
	UCrowdyBakedRegistry* Baked = NewObject<UCrowdyBakedRegistry>(GetTransientPackage());
	if (!TestNotNull(TEXT("baked registry created"), Baked))
	{
		return false;
	}

	const FSoftClassPath OffPath(UCrowdyGameModelPullOffTarget::StaticClass());
	const FSoftClassPath OnPath(UCrowdyGameModelPullOnTarget::StaticClass());
	const FSoftClassPath LegacyPath(UCrowdyGameModelPullInheritedTarget::StaticClass());
	const FSoftClassPath UnknownPath(UCrowdyGameModelTestTarget::StaticClass());

	Baked->ModelClasses.Add({ OffPath, TEXT("TestPullOff"), false });
	Baked->ModelClasses.Add({ OnPath, TEXT("TestPullOn"), true });
	// Deliberately written the way a registry baked before the flag existed was: the value is left to its default.
	Baked->ModelClasses.Add({ LegacyPath, TEXT("TestPullOffChild") });

	bool bPullOnStart = true;
	TestTrue(TEXT("the opted-out class has a baked entry"), Baked->FindPullModelOnStart(OffPath, bPullOnStart));
	TestFalse(TEXT("the opted-out class reads back as not pulling"), bPullOnStart);

	bPullOnStart = false;
	TestTrue(TEXT("the pulling class has a baked entry"), Baked->FindPullModelOnStart(OnPath, bPullOnStart));
	TestTrue(TEXT("the pulling class reads back as pulling"), bPullOnStart);

	bPullOnStart = false;
	TestTrue(TEXT("an entry baked before the flag existed is still found"),
		Baked->FindPullModelOnStart(LegacyPath, bPullOnStart));
	TestTrue(TEXT("an entry baked before the flag existed reads as pulling"), bPullOnStart);

	bPullOnStart = false;
	TestFalse(TEXT("a class with no baked entry is not found"),
		Baked->FindPullModelOnStart(UnknownPath, bPullOnStart));
	TestFalse(TEXT("a class with no baked entry leaves the caller's value untouched"), bPullOnStart);

	// The container type name rides the same entries, so it must still resolve alongside the new flag.
	FString TypeName;
	TestTrue(TEXT("the baked container type name still resolves"), Baked->FindContainerTypeName(OffPath, TypeName));
	TestEqual(TEXT("baked container type name"), TypeName, FString(TEXT("TestPullOff")));

	return true;
}

// The first-bind gate on a real participant: it reads the participant's OWN class, whatever shape the participant
// is. A container component answers from its own class rather than from its anchor actor.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullOnBindContainerSettingTest,
	"CrowdySDK.GameModel.PullOnBindContainerSetting", CrowdyPullOnBindTestFlags)
bool FCrowdyPullOnBindContainerSettingTest::RunTest(const FString& Parameters)
{
	UObject* OptedOut = NewObject<UCrowdyGameModelPullOffTarget>(GetTransientPackage());
	TestFalse(TEXT("a participant of an opted-out container type does not pull"),
		UCrowdyGameModelSubsystem::ShouldPullOnBindForTest(OptedOut));

	UObject* Pulling = NewObject<UCrowdyGameModelPullOnTarget>(GetTransientPackage());
	TestTrue(TEXT("a participant of a container type that never opted out pulls"),
		UCrowdyGameModelSubsystem::ShouldPullOnBindForTest(Pulling));

	// A component sub-participant is a container in its own right, so it answers from its own class; its anchor
	// actor has no say in it.
	AActor* Anchor = NewObject<AActor>(GetTransientPackage());
	UCrowdyGameModelTestComponent* SubComp = NewObject<UCrowdyGameModelTestComponent>(Anchor);
	Anchor->AddOwnedComponent(SubComp);
	TestTrue(TEXT("a container component that never opted out pulls"),
		UCrowdyGameModelSubsystem::ShouldPullOnBindForTest(SubComp));

	return true;
}

// Pull on start only exists for container types. A participant whose class is not a container has no setting to
// read and pulls, even when it happens to derive from a container that opted out, and a null participant is the
// same "nothing to consult" case.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullOnBindNonContainerParticipantTest,
	"CrowdySDK.GameModel.PullOnBindNonContainerParticipant", CrowdyPullOnBindTestFlags)
bool FCrowdyPullOnBindNonContainerParticipantTest::RunTest(const FString& Parameters)
{
	UObject* PlainParticipant = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	TestTrue(TEXT("a participant whose class is not a container pulls"),
		UCrowdyGameModelSubsystem::ShouldPullOnBindForTest(PlainParticipant));

	UObject* UntaggedSubclass = NewObject<UCrowdyGameModelPullUntaggedSubclass>(GetTransientPackage());
	TestTrue(TEXT("an untagged subclass of an opted-out container is no container, so it pulls"),
		UCrowdyGameModelSubsystem::ShouldPullOnBindForTest(UntaggedSubclass));

	TestTrue(TEXT("a null participant pulls"),
		UCrowdyGameModelSubsystem::ShouldPullOnBindForTest(nullptr));

	return true;
}

// RetryPendingModelEntities re-resolves the participant from the NetID once the ensure/read completes, and that
// resolve can come back null even for an entity that would have pulled at first bind: it was unregistered while
// the round trip was in flight. ShouldPullOnRetry is the gate that treats THAT null as "skip", separately from
// ShouldPullOnBind's own null handling (exercised by PullOnBindNonContainerParticipant above), which pulls because
// it means "no setting to consult", not "gone".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullOnRetryGateTest,
	"CrowdySDK.GameModel.PullOnRetryGate", CrowdyPullOnBindTestFlags)
bool FCrowdyPullOnRetryGateTest::RunTest(const FString& Parameters)
{
	// No participant at all (resolved to null - the entity died during the round trip) -> must not pull.
	TestFalse(TEXT("a participant that no longer resolves (died during the round trip) does not pull"),
		UCrowdyGameModelSubsystem::ShouldPullOnRetryForTest(nullptr));

	// A resolved participant defers to ShouldPullOnBind verbatim: still pulls by default...
	UObject* Pulling = NewObject<UCrowdyGameModelPullOnTarget>(GetTransientPackage());
	TestTrue(TEXT("a resolved participant of a pulling container type still pulls on retry"),
		UCrowdyGameModelSubsystem::ShouldPullOnRetryForTest(Pulling));

	// ...and still honors the container type's opt-out.
	UObject* OptedOut = NewObject<UCrowdyGameModelPullOffTarget>(GetTransientPackage());
	TestFalse(TEXT("a resolved participant of an opted-out container type does not pull on retry"),
		UCrowdyGameModelSubsystem::ShouldPullOnRetryForTest(OptedOut));

	return true;
}

#endif
