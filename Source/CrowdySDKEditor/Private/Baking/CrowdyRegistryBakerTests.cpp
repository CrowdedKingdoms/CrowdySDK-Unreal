// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS && WITH_METADATA

#include "Misc/AutomationTest.h"

#include "Baking/CrowdyRegistryBaker.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "UObject/UObjectIterator.h"
#include "Utils/CrowdyBakedRegistry.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyRegistryBakerTestFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	// Every SDK test-only class, asked the way the baker asks: a container fixture is marked CrowdyContainerTest and
	// the rest are marked CrowdyTestFixture, so a sweep that checks only one marker lets the other kind through.
	TSet<FSoftClassPath> GatherTestOnlyClassPaths()
	{
		TSet<FSoftClassPath> Paths;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!IsValid(Class)) continue;

			if (FCrowdyAttributeRegistry::IsTestFixture(Class) || FCrowdyAttributeRegistry::IsTestContainer(Class))
			{
				Paths.Add(FSoftClassPath(Class));
			}
		}
		return Paths;
	}
}

/**
 * A shipped registry must carry nothing that only exists to be tested against. Several fixtures declare markup that
 * is deliberately invalid - a field claimed by both planes, two attributes resolving to one server key, a notify with
 * the wrong arity - so a sweep that inspects them reports errors, and errors during a cook fail the cook however
 * healthy the project is.
 *
 * The assertions below name what leaked, and the test also fails on the reports themselves: an error logged while a
 * test runs fails it, which is the same signal the cook reads.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRegistryBakerFixtureExclusionTest,
	"CrowdySDK.CrowdySDKEditor.RegistryBakerExcludesTestFixtures", CrowdyRegistryBakerTestFlags)
bool FCrowdyRegistryBakerFixtureExclusionTest::RunTest(const FString& Parameters)
{
	const TSet<FSoftClassPath> TestOnly = GatherTestOnlyClassPaths();

	// Nothing below can fail if the fixtures are not loaded, and a silently empty run would read as a pass.
	if (!TestTrue(TEXT("the SDK's own test fixtures are loaded, so there is something to exclude"),
		TestOnly.Num() > 0))
	{
		return false;
	}

	UCrowdyBakedRegistry* Registry = NewObject<UCrowdyBakedRegistry>(GetTransientPackage());
	UCrowdyRegistryBaker::PopulateFromLoadedObjects(Registry);

	for (const FCrowdyBakedAttribute& Entry : Registry->ModelAttributes)
	{
		TestFalse(*FString::Printf(TEXT("no Game Model attribute is baked from the fixture '%s'"),
			*Entry.OwnerClassPath.ToString()), TestOnly.Contains(Entry.OwnerClassPath));
	}

	for (const FCrowdyBakedModelClass& Entry : Registry->ModelClasses)
	{
		TestFalse(*FString::Printf(TEXT("no container row is baked for the fixture '%s'"),
			*Entry.ClassPath.ToString()), TestOnly.Contains(Entry.ClassPath));
	}

	for (const FCrowdyBakedRepProperty& Entry : Registry->RepProperties)
	{
		TestFalse(*FString::Printf(TEXT("no CrowdyState property is baked from the fixture '%s'"),
			*Entry.OwnerClassPath.ToString()), TestOnly.Contains(Entry.OwnerClassPath));
	}

	for (const FCrowdyBakedRepLayoutHash& Entry : Registry->RepLayoutHashes)
	{
		TestFalse(*FString::Printf(TEXT("no CrowdyState layout hash is baked for the fixture '%s'"),
			*Entry.ClassPath.ToString()), TestOnly.Contains(Entry.ClassPath));
	}

	// The sweep still has to have done its job: a bake that excluded everything would pass every check above.
	TestTrue(TEXT("the sweep baked the project's real classes"), Registry->ModelAttributes.Num() > 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_METADATA
