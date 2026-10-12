#include "Dev/CrowdyMassCrowdSimHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dev/CrowdyMassCrowdSimStateEntity.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

// ResolveBotFields is what Start and EmitBotState both rely on to know which fields a bot class can carry
// CrowdyState traffic on. It has to work from the class alone, with no world and no spawn: a cooked build
// strips the reflection metadata a HasMetaData check would need, so the resolution is by name and type only.
static constexpr EAutomationTestFlags CrowdyMassCrowdSimHarnessTestFlags =
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassCrowdSimHarnessResolvesFixtureFieldsTest,
	"CrowdySDK.CrowdyMass.CrowdSim.ResolvesFieldsOnTheDefaultFixture", CrowdyMassCrowdSimHarnessTestFlags)
bool FCrowdyMassCrowdSimHarnessResolvesFixtureFieldsTest::RunTest(const FString& Parameters)
{
	const FCrowdyMassCrowdSimBotFields Fields =
		UCrowdyMassCrowdSimHarness::ResolveBotFields(ACrowdyMassCrowdSimStateEntity::StaticClass());

	if (!TestNotNull(TEXT("Payload should resolve as an int property named Payload"), Fields.PayloadProperty))
	{
		return false;
	}

	if (!TestNotNull(TEXT("Stance should resolve as an enum-shaped property named Stance"), Fields.StanceProperty))
	{
		return false;
	}

	if (!TestNotNull(TEXT("Stance's enum should resolve alongside the property"), Fields.StanceEnum))
	{
		return false;
	}

	TestTrue(TEXT("Stance's enum should be the plugin's gate stance enum"),
		Fields.StanceEnum == StaticEnum<ECrowdyMassGateStance>());

	// The fixture declares Stance as `enum class : uint8`, which reflects as an enum property, so the cached
	// underlying property is what the per-bot write goes through.
	TestNotNull(TEXT("An enum-property Stance caches its underlying numeric property"), Fields.StanceUnderlyingProperty);
	TestEqual(TEXT("The enum's bound is cached off the UEnum once"),
		Fields.StanceMaxValue, StaticEnum<ECrowdyMassGateStance>()->GetMaxEnumValue());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassCrowdSimHarnessFindsNeitherFieldOnPlainActorTest,
	"CrowdySDK.CrowdyMass.CrowdSim.FindsNeitherFieldOnAPlainActor", CrowdyMassCrowdSimHarnessTestFlags)
bool FCrowdyMassCrowdSimHarnessFindsNeitherFieldOnPlainActorTest::RunTest(const FString& Parameters)
{
	const FCrowdyMassCrowdSimBotFields Fields =
		UCrowdyMassCrowdSimHarness::ResolveBotFields(AActor::StaticClass());

	TestNull(TEXT("A plain Actor carries no Payload property, so none should resolve"), Fields.PayloadProperty);
	TestNull(TEXT("A plain Actor carries no Stance property, so none should resolve"), Fields.StanceProperty);
	TestNull(TEXT("With no Stance property there is no enum to report either"), Fields.StanceEnum);
	TestNull(TEXT("Nor an underlying property to write through"), Fields.StanceUnderlyingProperty);
	TestEqual(TEXT("And the cached bound stays at zero"), Fields.StanceMaxValue, static_cast<int64>(0));

	return true;
}

#endif
