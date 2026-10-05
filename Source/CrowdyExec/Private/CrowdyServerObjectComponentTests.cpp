#include "CrowdyServerObjectComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace CrowdyServerObjectComponentTests
{
	constexpr EAutomationTestFlags ComponentTestFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	/** A component outside any world: no game instance, so no session and no teams cache. */
	UCrowdyServerObjectComponent* MakeWorldlessComponent()
	{
		return NewObject<UCrowdyServerObjectComponent>(GetTransientPackage(), NAME_None, RF_Transient);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectComponentPlayersTeamUsesTeamIdTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.PlayersTeamUsesTeamId", CrowdyServerObjectComponentTests::ComponentTestFlags)
bool FCrowdyServerObjectComponentPlayersTeamUsesTeamIdTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectComponent> Component(CrowdyServerObjectComponentTests::MakeWorldlessComponent());
	Component->InstanceMode = ECrowdyServerObjectInstanceMode::PlayersTeam;

	// Above 2^53, so a detour through a double or an int32 would change the digits.
	Component->TeamId = 9007199254740993;
	FString Error;
	TestEqualSensitive(TEXT("a set Team Id is the Instance Id, in decimal"), Component->ResolveInstanceId(Error), FString(TEXT("9007199254740993")));
	TestTrue(FString::Printf(TEXT("with no error, even with no teams cache (got %s)"), *Error), Error.IsEmpty());

	Component->TeamId = 7;
	Error.Reset();
	TestEqualSensitive(TEXT("a small Team Id reads the same way"), Component->ResolveInstanceId(Error), FString(TEXT("7")));
	TestTrue(TEXT("with no error"), Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectComponentPlayersTeamWaitsForTeamsTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.PlayersTeamWaitsForTeams", CrowdyServerObjectComponentTests::ComponentTestFlags)
bool FCrowdyServerObjectComponentPlayersTeamWaitsForTeamsTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectComponent> Component(CrowdyServerObjectComponentTests::MakeWorldlessComponent());
	Component->InstanceMode = ECrowdyServerObjectInstanceMode::PlayersTeam;
	Component->TeamId = 0;

	FString Error;
	TestTrue(TEXT("Team Id 0 with no teams cache gives nothing"), Component->ResolveInstanceId(Error).IsEmpty());
	TestEqualSensitive(TEXT("saying it waits for the player's teams"), Error, FString(TEXT("Waiting for the player's teams")));
	TestTrue(TEXT("and a component that never joined reports no failure"), Component->GetFailureReason().IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerObjectComponentFromServerValueWaitsForVariableTest,
	"CrowdySDK.CrowdyExec.ServerObjectComponent.FromServerValueWaitsForVariable", CrowdyServerObjectComponentTests::ComponentTestFlags)
bool FCrowdyServerObjectComponentFromServerValueWaitsForVariableTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyServerObjectComponent> Component(CrowdyServerObjectComponentTests::MakeWorldlessComponent());
	Component->InstanceMode = ECrowdyServerObjectInstanceMode::FromServerValue;
	Component->SourceVariable = TEXT("TeamId");

	FString Error;
	TestTrue(TEXT("before the source gives a value there is no Instance Id"), Component->ResolveInstanceId(Error).IsEmpty());
	TestEqualSensitive(TEXT("saying which variable it waits for"), Error, FString(TEXT("Waiting for TeamId")));

	Component->InstanceId = TEXT("chest-7");
	Error.Reset();
	TestTrue(TEXT("a typed Instance Id is not used by From Server Value"), Component->ResolveInstanceId(Error).IsEmpty());
	TestEqualSensitive(TEXT("it still waits for the variable"), Error, FString(TEXT("Waiting for TeamId")));

	Component->SourceVariable = NAME_None;
	Error.Reset();
	TestTrue(TEXT("with no Source Variable there is no Instance Id"), Component->ResolveInstanceId(Error).IsEmpty());
	TestEqualSensitive(TEXT("saying to set one"), Error, FString(TEXT("Set a Source Variable on the Crowdy Server Object component")));
	return true;
}

#endif
