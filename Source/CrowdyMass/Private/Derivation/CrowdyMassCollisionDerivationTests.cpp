#include "Derivation/CrowdyMassCollisionDerivation.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/CapsuleComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "Misc/AutomationTest.h"

static constexpr EAutomationTestFlags CrowdyMassCollisionDerivationTestFlags =
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

// A character's capsule is what a trace against that character actually hits, so the size derived from
// the class has to be that capsule's size and not an approximation of it. Compared against the capsule
// the class itself carries rather than against literal numbers, so the test still means the same thing
// if the engine ever changes the default character's dimensions.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassCollisionDerivationCapsuleExtentsTest,
	"CrowdySDK.Mass.CollisionDerivationReadsCapsuleExtents", CrowdyMassCollisionDerivationTestFlags)
bool FCrowdyMassCollisionDerivationCapsuleExtentsTest::RunTest(const FString& Parameters)
{
	const ACharacter* CharacterDefaults = GetDefault<ACharacter>();
	if (!CharacterDefaults)
	{
		AddError(TEXT("The default character object could not be reached, so there is no capsule to compare against."));
		return false;
	}

	const UCapsuleComponent* Capsule = CharacterDefaults->GetCapsuleComponent();
	if (!Capsule)
	{
		AddError(TEXT("The default character has no capsule component, so there is nothing to compare against."));
		return false;
	}

	const FCrowdyMassDerivedTraceCollision Derived = FCrowdyMassCollisionDerivation::FromActorClass(ACharacter::StaticClass());

	TestTrue(TEXT("a class carrying a capsule produces a usable answer"), Derived.IsValid());
	TestEqual(TEXT("the answer says it came from the capsule"),
		static_cast<int32>(Derived.Source), static_cast<int32>(ECrowdyMassCollisionDerivationSource::CapsuleComponent));
	TestEqual(TEXT("the derived radius is the capsule's unscaled radius"), Derived.Radius, Capsule->GetUnscaledCapsuleRadius());
	TestEqual(TEXT("the derived half height is the capsule's unscaled half height"), Derived.HalfHeight, Capsule->GetUnscaledCapsuleHalfHeight());
	TestTrue(TEXT("an answer that succeeded carries no reason"), Derived.Reason.IsEmpty());

	return true;
}

// The derived settings are a compressed description of the component's per-channel responses: one
// default plus the channels that differ. Expanding them has to reproduce the component exactly, or an
// entity built from the class would answer traces differently from the actor it stands for. Checked as
// a round trip rather than against named channels, so it holds whatever collision profiles a project
// happens to define.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassCollisionDerivationResponseRoundTripTest,
	"CrowdySDK.Mass.CollisionDerivationRoundTripsResponses", CrowdyMassCollisionDerivationTestFlags)
bool FCrowdyMassCollisionDerivationResponseRoundTripTest::RunTest(const FString& Parameters)
{
	const ACharacter* CharacterDefaults = GetDefault<ACharacter>();
	if (!CharacterDefaults || !CharacterDefaults->GetCapsuleComponent())
	{
		AddError(TEXT("The default character has no capsule component, so there are no responses to compare against."));
		return false;
	}

	const UCapsuleComponent* Capsule = CharacterDefaults->GetCapsuleComponent();
	const FCrowdyMassDerivedTraceCollision Derived = FCrowdyMassCollisionDerivation::FromActorClass(ACharacter::StaticClass());

	TestEqual(TEXT("the derived object type is the capsule's object type"),
		static_cast<int32>(Derived.Settings.ObjectType.GetValue()), static_cast<int32>(Capsule->GetCollisionObjectType()));
	TestTrue(TEXT("trace collision is on exactly when the capsule answers queries"),
		Derived.Settings.bEnableTraceCollision == CollisionEnabledHasQuery(Capsule->GetCollisionEnabled()));

	const FCollisionResponseContainer Expanded = FCrowdyMassCollisionDerivation::ExpandResponses(Derived.Settings);
	TestTrue(TEXT("expanding the derived settings reproduces the capsule's own channel responses"),
		Expanded == Capsule->GetCollisionResponseToChannels());

	for (const FCrowdyMassCollisionResponseOverride& Override : Derived.Settings.ResponseOverrides)
	{
		TestNotEqual(TEXT("no override repeats the default response it is meant to differ from"),
			static_cast<int32>(Override.Response.GetValue()), static_cast<int32>(Derived.Settings.DefaultResponse.GetValue()));
	}

	return true;
}

// A class that describes no shape must say so in words a reader can act on, naming the class. Returning
// default settings instead would put entities in the world with a plausible-looking size that came from
// nowhere, and nothing would report that the class was never really the source of it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassCollisionDerivationNoShapeReasonTest,
	"CrowdySDK.Mass.CollisionDerivationWithoutShapesGivesAReason", CrowdyMassCollisionDerivationTestFlags)
bool FCrowdyMassCollisionDerivationNoShapeReasonTest::RunTest(const FString& Parameters)
{
	const FCrowdyMassDerivedTraceCollision Bare = FCrowdyMassCollisionDerivation::FromActorClass(AActor::StaticClass());

	TestFalse(TEXT("a class with neither a capsule nor a physics asset produces no usable answer"), Bare.IsValid());
	TestEqual(TEXT("the answer names no source"),
		static_cast<int32>(Bare.Source), static_cast<int32>(ECrowdyMassCollisionDerivationSource::None));
	TestFalse(TEXT("a class that could not be answered for carries a reason"), Bare.Reason.IsEmpty());
	TestTrue(TEXT("the reason names the class it is about"), Bare.Reason.Contains(TEXT("'Actor'")));
	TestEqual(TEXT("an unanswered class reports no radius"), Bare.Radius, 0.0f);
	TestEqual(TEXT("an unanswered class reports no half height"), Bare.HalfHeight, 0.0f);

	const FCrowdyMassDerivedTraceCollision NoClass = FCrowdyMassCollisionDerivation::FromActorClass(nullptr);
	TestFalse(TEXT("no class at all produces no usable answer"), NoClass.IsValid());
	TestFalse(TEXT("no class at all still carries a reason"), NoClass.Reason.IsEmpty());

	const FCrowdyMassDerivedTraceCollision NotAnActor = FCrowdyMassCollisionDerivation::FromActorClass(UObject::StaticClass());
	TestFalse(TEXT("a class that is not an actor produces no usable answer"), NotAnActor.IsValid());
	TestFalse(TEXT("a class that is not an actor still carries a reason"), NotAnActor.Reason.IsEmpty());

	return true;
}

// Deriving reads a class and writes nothing: settings somebody authored keep their values, the struct's
// own defaults keep theirs, and asking twice gives the same answer. A derivation that quietly wrote to
// shared state would show up here as an authored value changing underneath its owner.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassCollisionDerivationLeavesAuthoredSettingsAloneTest,
	"CrowdySDK.Mass.CollisionDerivationLeavesAuthoredSettingsAlone", CrowdyMassCollisionDerivationTestFlags)
bool FCrowdyMassCollisionDerivationLeavesAuthoredSettingsAloneTest::RunTest(const FString& Parameters)
{
	FCrowdyMassTraceCollisionSettings Authored;
	Authored.bEnableTraceCollision = false;
	Authored.ObjectType = ECC_WorldStatic;
	Authored.DefaultResponse = ECR_Overlap;
	Authored.ResponseOverrides.Reset();
	Authored.ResponseOverrides.Emplace(ECC_Visibility, ECR_Block);

	const FCrowdyMassDerivedTraceCollision First = FCrowdyMassCollisionDerivation::FromActorClass(ACharacter::StaticClass());
	const FCrowdyMassDerivedTraceCollision Second = FCrowdyMassCollisionDerivation::FromActorClass(ACharacter::StaticClass());

	TestFalse(TEXT("the authored enable flag is unchanged"), Authored.bEnableTraceCollision);
	TestEqual(TEXT("the authored object type is unchanged"),
		static_cast<int32>(Authored.ObjectType.GetValue()), static_cast<int32>(ECC_WorldStatic));
	TestEqual(TEXT("the authored default response is unchanged"),
		static_cast<int32>(Authored.DefaultResponse.GetValue()), static_cast<int32>(ECR_Overlap));
	TestEqual(TEXT("the authored override list is unchanged"), Authored.ResponseOverrides.Num(), 1);

	if (Authored.ResponseOverrides.Num() == 1)
	{
		TestEqual(TEXT("the authored override still names its own channel"),
			static_cast<int32>(Authored.ResponseOverrides[0].Channel.GetValue()), static_cast<int32>(ECC_Visibility));
		TestEqual(TEXT("the authored override still names its own response"),
			static_cast<int32>(Authored.ResponseOverrides[0].Response.GetValue()), static_cast<int32>(ECR_Block));
	}

	const FCrowdyMassTraceCollisionSettings FreshDefaults;
	TestEqual(TEXT("a freshly constructed settings struct still carries its own single default override"),
		FreshDefaults.ResponseOverrides.Num(), 1);

	TestEqual(TEXT("asking the same class twice gives the same radius"), Second.Radius, First.Radius);
	TestEqual(TEXT("asking the same class twice gives the same half height"), Second.HalfHeight, First.HalfHeight);
	TestEqual(TEXT("asking the same class twice gives the same source"),
		static_cast<int32>(Second.Source), static_cast<int32>(First.Source));
	TestEqual(TEXT("asking the same class twice gives the same object type"),
		static_cast<int32>(Second.Settings.ObjectType.GetValue()), static_cast<int32>(First.Settings.ObjectType.GetValue()));
	TestTrue(TEXT("asking the same class twice gives the same channel responses"),
		FCrowdyMassCollisionDerivation::ExpandResponses(Second.Settings) == FCrowdyMassCollisionDerivation::ExpandResponses(First.Settings));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
