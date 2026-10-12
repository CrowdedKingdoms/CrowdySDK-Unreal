#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Dev/CrowdyMassGateStateEntity.h"
#include "Dev/CrowdyMassStateDeltaBuilder.h"
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Utils/UCrowdyClassRegistry.h"

// Covers FCrowdyMassStateDeltaBuilder: synthesizing a real, decodable FCrowdyStateDelta from a class, a
// layout and named byte values, with no game type and no live sender entity involved. This is the
// synthesis half only; the receive path it feeds (the participant-less apply through
// ICrowdyEntitySubscriber) is covered separately in CrowdySDK.Mass.StateFragment*.
namespace
{
	constexpr EAutomationTestFlags CrowdyMassStateDeltaBuilderTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassStateDeltaBuilderRoundTripTest,
	"CrowdySDK.Mass.StateDeltaBuilderRoundTrip", CrowdyMassStateDeltaBuilderTestFlags)
bool FCrowdyMassStateDeltaBuilderRoundTripTest::RunTest(const FString& Parameters)
{
	FCrowdyRepLayout Layout;
	if (!TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyMassGateStateEntity::StaticClass(), Layout)))
	{
		return false;
	}

	const FCrowdyMassStateDeltaByteField Fields[] = {
		{TEXT("Stance"), static_cast<uint8>(ECrowdyMassGateStance::Downed)},
		{TEXT("GateOnlyStateByte"), 222},
	};

	FCrowdyStateDelta Delta;
	FString Why;
	if (!TestTrue(TEXT("build succeeded"),
		FCrowdyMassStateDeltaBuilder::Build(*UCrowdyMassGateStateEntity::StaticClass(), Layout,
			FGuid::NewGuid(), FGuid::NewGuid(), Fields, Delta, Why)))
	{
		return false;
	}

	UCrowdyMassGateStateEntity* Decoded = NewObject<UCrowdyMassGateStateEntity>();
	TArray<int32> ChangedIndices;
	TArray<int32> PresentIndices;
	if (!TestTrue(TEXT("decode succeeded"),
		FCrowdyStateCodec::Decode(Layout, Delta.LayoutHash, Delta.Blob, Decoded, ChangedIndices, &PresentIndices)))
	{
		return false;
	}

	TestTrue(TEXT("the stance arrived"), Decoded->Stance == ECrowdyMassGateStance::Downed);

	// The byte declared beside the stance is the offset guard, and it is not incidental. A builder that
	// addressed an enum field through the byte INSIDE the enum property would write at the container's own
	// start, because that byte is owned by the enum property rather than by the container and its offset is
	// therefore 0. On a UObject that is the object header, so the corruption would not even land here; this
	// asserts the neighbouring property survives, which is the observable half of the same mistake.
	TestEqual(TEXT("GateOnlyStateByte arrived and was not overwritten by the stance write"),
		Decoded->GateOnlyStateByte, static_cast<uint8>(222));

	// A keyframe carries every layout slot, not only the ones this test happened to set. Present, not
	// changed: a freshly constructed target's fields already default to 0, so a value that happened to
	// decode to its default would still count as "changed" only by accident.
	TestEqual(TEXT("every layout slot is present"), PresentIndices.Num(), Layout.Properties.Num());
	for (int32 LayoutIndex = 0; LayoutIndex < Layout.Properties.Num(); ++LayoutIndex)
	{
		TestTrue(FString::Printf(TEXT("layout slot %d is present"), LayoutIndex),
			PresentIndices.Contains(LayoutIndex));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassStateDeltaBuilderCarriesTheDeclaredClassAndLayoutIdsTest,
	"CrowdySDK.Mass.StateDeltaBuilderCarriesTheDeclaredClassAndLayoutIds", CrowdyMassStateDeltaBuilderTestFlags)
bool FCrowdyMassStateDeltaBuilderCarriesTheDeclaredClassAndLayoutIdsTest::RunTest(const FString& Parameters)
{
	FCrowdyRepLayout Layout;
	if (!TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyMassGateStateEntity::StaticClass(), Layout)))
	{
		return false;
	}

	const FCrowdyMassStateDeltaByteField Fields[] = {{TEXT("Stance"), static_cast<uint8>(ECrowdyMassGateStance::Wounded)}};

	FCrowdyStateDelta Delta;
	FString Why;
	if (!TestTrue(TEXT("build succeeded"),
		FCrowdyMassStateDeltaBuilder::Build(*UCrowdyMassGateStateEntity::StaticClass(), Layout,
			FGuid::NewGuid(), FGuid::NewGuid(), Fields, Delta, Why)))
	{
		return false;
	}

	const FCrowdyClassID ExpectedClassID = UCrowdyClassRegistry::Get()->GetID(UCrowdyMassGateStateEntity::StaticClass());
	TestEqual(TEXT("ClassID matches the registry"), Delta.ClassID, static_cast<int64>(ExpectedClassID));
	TestEqual(TEXT("LayoutHash matches the layout it was built from"), Delta.LayoutHash, Layout.LayoutHash);
	TestTrue(TEXT("the Keyframe flag bit is set"), (Delta.Flags & CrowdyStateDeltaFlags::Keyframe) != 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassStateDeltaBuilderRefusesAFieldTheClassDoesNotCarryTest,
	"CrowdySDK.Mass.StateDeltaBuilderRefusesAFieldTheClassDoesNotCarry", CrowdyMassStateDeltaBuilderTestFlags)
bool FCrowdyMassStateDeltaBuilderRefusesAFieldTheClassDoesNotCarryTest::RunTest(const FString& Parameters)
{
	FCrowdyRepLayout Layout;
	if (!TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyMassGateStateEntity::StaticClass(), Layout)))
	{
		return false;
	}

	const FName MadeUpName(TEXT("NoSuchFieldOnThisClass"));
	const FCrowdyMassStateDeltaByteField Fields[] = {{MadeUpName, 1}};

	FCrowdyStateDelta Delta;
	FString Why;
	TestFalse(TEXT("build refused"),
		FCrowdyMassStateDeltaBuilder::Build(*UCrowdyMassGateStateEntity::StaticClass(), Layout,
			FGuid::NewGuid(), FGuid::NewGuid(), Fields, Delta, Why));

	TestTrue(TEXT("the refusal names the field"), Why.Contains(MadeUpName.ToString()));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassStateDeltaBuilderRefusesAClassTheLayoutIsNotForTest,
	"CrowdySDK.Mass.StateDeltaBuilderRefusesAClassTheLayoutIsNotFor", CrowdyMassStateDeltaBuilderTestFlags)
bool FCrowdyMassStateDeltaBuilderRefusesAClassTheLayoutIsNotForTest::RunTest(const FString& Parameters)
{
	FCrowdyRepLayout Layout;
	if (!TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyMassGateStateEntity::StaticClass(), Layout)))
	{
		return false;
	}

	const FCrowdyMassStateDeltaByteField Fields[] = {{TEXT("Stance"), 1}};

	FCrowdyStateDelta Delta;
	FString Why;
	// The layout describes UCrowdyMassGateStateEntity; UObject is a different class entirely, so this must
	// be refused rather than encoded as if the two agreed.
	TestFalse(TEXT("build refused for a class the layout was not built from"),
		FCrowdyMassStateDeltaBuilder::Build(*UObject::StaticClass(), Layout,
			FGuid::NewGuid(), FGuid::NewGuid(), Fields, Delta, Why));

	TestFalse(TEXT("a reason was given"), Why.IsEmpty());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassStateDeltaBuilderRefusesAnInvalidEntityOrSenderIdTest,
	"CrowdySDK.Mass.StateDeltaBuilderRefusesAnInvalidEntityOrSenderId", CrowdyMassStateDeltaBuilderTestFlags)
bool FCrowdyMassStateDeltaBuilderRefusesAnInvalidEntityOrSenderIdTest::RunTest(const FString& Parameters)
{
	FCrowdyRepLayout Layout;
	if (!TestTrue(TEXT("layout built"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyMassGateStateEntity::StaticClass(), Layout)))
	{
		return false;
	}

	const FCrowdyMassStateDeltaByteField Fields[] = {{TEXT("Stance"), 1}};

	{
		FCrowdyStateDelta Delta;
		FString Why;
		TestFalse(TEXT("build refused for an invalid entity id"),
			FCrowdyMassStateDeltaBuilder::Build(*UCrowdyMassGateStateEntity::StaticClass(), Layout,
				FGuid(), FGuid::NewGuid(), Fields, Delta, Why));
		TestFalse(TEXT("a reason was given for the invalid entity id"), Why.IsEmpty());
	}

	{
		FCrowdyStateDelta Delta;
		FString Why;
		TestFalse(TEXT("build refused for an invalid sender id"),
			FCrowdyMassStateDeltaBuilder::Build(*UCrowdyMassGateStateEntity::StaticClass(), Layout,
				FGuid::NewGuid(), FGuid(), Fields, Delta, Why));
		TestFalse(TEXT("a reason was given for the invalid sender id"), Why.IsEmpty());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
