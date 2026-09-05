// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Replication/GameModel/Effect/CrowdyEffectLowering.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyEffectAutomationTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A minimal, compilable automatic effect on the shared discovery target (container type "TestHero", a "health"
	// attribute clamped [0,100]). Every field beyond bRunAutomatically stays at its default so each test tweaks only
	// what it asserts. Compiling this class runs FCrowdyAttributeRegistry::DiscoverForClass, which logs one error for
	// the fixture's deliberate dual-plane property and one for its wrong-arity notify, so a test that compiles it must
	// expect those (see ExpectDiscoveryErrors).
	UCrowdyEffect* MakeAutomaticEffect()
	{
		UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
		Effect->ContainerClass = UCrowdyGameModelDiscoveryTarget::StaticClass();
		Effect->FunctionName = TEXT("hero_tick");
		Effect->Source = ECrowdyEffectSource::Text;
		Effect->EffectScript = TEXT("self.health -= 5");
		Effect->bRunAutomatically = true;
		return Effect;
	}

	// The two error logs the discovery fixture emits per compile. NumCompiles is how many times the test calls
	// Compile(), since DiscoverForClass re-logs on every call (it does not cache).
	void ExpectDiscoveryErrors(FAutomationTestBase& Test, int32 NumCompiles)
	{
		Test.AddExpectedError(TEXT("marked both CrowdyModel and CrowdyState"), EAutomationExpectedErrorFlags::Contains, NumCompiles);
		Test.AddExpectedError(TEXT("must be parameterless"), EAutomationExpectedErrorFlags::Contains, NumCompiles);
	}
}

// An interval effect compiles to an autonomous function plus a schedule/interval automation whose function name,
// type target, interval, and safety budget carry through, with no event trigger.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectAutomationEveryIntervalTest,
	"CrowdySDK.Replication.EffectAutomationEveryInterval", CrowdyEffectAutomationTestFlags)
bool FCrowdyEffectAutomationEveryIntervalTest::RunTest(const FString& Parameters)
{
	ExpectDiscoveryErrors(*this, 1);

	UCrowdyEffect* Effect = MakeAutomaticEffect();
	Effect->AutomationTrigger = ECrowdyEffectAutomationTrigger::EveryInterval;
	Effect->AutomationIntervalMs = 500;

	const FCrowdyEffectLoweringResult R = Effect->Compile();

	TestFalse(TEXT("no compile errors"), R.HasErrors());
	TestTrue(TEXT("function is autonomous-invocable"), R.Function.bAutonomousInvocable);
	TestEqual(TEXT("invoke scope stays player"), R.Function.InvokeScope, FString(TEXT("player")));

	if (TestTrue(TEXT("automation is set"), R.Automation.IsSet()))
	{
		const FCrowdyGameModelAutomationInput& A = R.Automation.GetValue();
		TestEqual(TEXT("function name"), A.FunctionName, FString(TEXT("hero_tick")));
		TestEqual(TEXT("automation name defaults to function"), A.Name, FString(TEXT("hero_tick")));
		TestEqual(TEXT("action kind"), A.ActionKind, FString(TEXT("model_function")));
		TestEqual(TEXT("target mode"), A.TargetMode, FString(TEXT("type")));
		TestEqual(TEXT("target type defaults to container type"), A.TargetTypeName, FString(TEXT("TestHero")));
		TestEqual(TEXT("trigger type"), A.TriggerType, FString(TEXT("schedule")));
		TestEqual(TEXT("schedule kind"), A.ScheduleKind, FString(TEXT("interval")));
		TestEqual(TEXT("interval carried"), A.IntervalMs, 500);
		TestTrue(TEXT("enabled defaults true"), A.bEnabled);

		TestEqual(TEXT("budget MaxTargets"), A.MaxTargets, 50);
		TestEqual(TEXT("budget GasLimit"), A.GasLimit, 100000);
		TestEqual(TEXT("budget RunTimeoutMs"), A.RunTimeoutMs, 2000);
		TestEqual(TEXT("budget MaxRunsPerMinute"), A.MaxRunsPerMinute, 120);
		TestEqual(TEXT("budget FailureThreshold"), A.FailureThreshold, 5);
		TestEqual(TEXT("budget CooldownMs"), A.CooldownMs, 30000);
	}

	TestFalse(TEXT("no event trigger for a schedule"), R.Trigger.IsSet());
	return true;
}

// A property-change effect compiles to an event-typed automation plus an event trigger carrying the watched
// property key and defaulting its container type filter to the effect's own type.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectAutomationOnPropertyChangeTest,
	"CrowdySDK.Replication.EffectAutomationOnPropertyChange", CrowdyEffectAutomationTestFlags)
bool FCrowdyEffectAutomationOnPropertyChangeTest::RunTest(const FString& Parameters)
{
	ExpectDiscoveryErrors(*this, 1);

	UCrowdyEffect* Effect = MakeAutomaticEffect();
	Effect->AutomationTrigger = ECrowdyEffectAutomationTrigger::OnPropertyChange;
	Effect->AutomationChangePropertyKey = TEXT("health");
	Effect->AutomationDebounceMs = 250;

	const FCrowdyEffectLoweringResult R = Effect->Compile();

	TestFalse(TEXT("no compile errors"), R.HasErrors());
	TestTrue(TEXT("function is autonomous-invocable"), R.Function.bAutonomousInvocable);

	if (TestTrue(TEXT("automation is set"), R.Automation.IsSet()))
	{
		TestEqual(TEXT("automation trigger type is event"), R.Automation.GetValue().TriggerType, FString(TEXT("event")));
	}

	if (TestTrue(TEXT("event trigger is set"), R.Trigger.IsSet()))
	{
		const FCrowdyGameModelAutomationTriggerInput& T = R.Trigger.GetValue();
		TestEqual(TEXT("trigger automation name"), T.AutomationName, FString(TEXT("hero_tick")));
		TestEqual(TEXT("on event"), T.OnEvent, FString(TEXT("property_changed")));
		TestEqual(TEXT("property key carried"), T.PropertyKey, FString(TEXT("health")));
		TestEqual(TEXT("container type defaults to effect type"), T.ContainerTypeName, FString(TEXT("TestHero")));
		TestEqual(TEXT("write source defaults to any"), T.WriteSource, FString(TEXT("any")));
		TestEqual(TEXT("debounce carried"), T.DebounceMs, 250);
	}
	return true;
}

// Narrowing the observed write source carries through to the trigger, so an automation can be limited to direct
// writes when a function's own mutations should deliberately not re-trigger it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectAutomationWriteSourceTest,
	"CrowdySDK.Replication.EffectAutomationWriteSource", CrowdyEffectAutomationTestFlags)
bool FCrowdyEffectAutomationWriteSourceTest::RunTest(const FString& Parameters)
{
	ExpectDiscoveryErrors(*this, 2);

	UCrowdyEffect* Effect = MakeAutomaticEffect();
	Effect->AutomationTrigger = ECrowdyEffectAutomationTrigger::OnPropertyChange;
	Effect->AutomationChangePropertyKey = TEXT("health");

	Effect->AutomationWriteSource = ECrowdyEffectPropertyWriteSource::Direct;
	const FCrowdyEffectLoweringResult Direct = Effect->Compile();
	if (TestTrue(TEXT("direct trigger is set"), Direct.Trigger.IsSet()))
	{
		TestEqual(TEXT("direct write source"), Direct.Trigger.GetValue().WriteSource, FString(TEXT("direct")));
	}

	Effect->AutomationWriteSource = ECrowdyEffectPropertyWriteSource::Function;
	const FCrowdyEffectLoweringResult Function = Effect->Compile();
	if (TestTrue(TEXT("function trigger is set"), Function.Trigger.IsSet()))
	{
		TestEqual(TEXT("function write source"), Function.Trigger.GetValue().WriteSource, FString(TEXT("function")));
	}
	return true;
}

// A function-invoked effect compiles to an event-typed automation plus an event trigger carrying the watched
// function name. Its container type filter is deliberately left EMPTY when unset: unlike a property-change filter,
// this one names the container the WATCHED function runs on, which is normally a different type from the effect's
// own, so defaulting it to the effect's type would author a trigger that silently never matches.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectAutomationOnFunctionInvokedTest,
	"CrowdySDK.Replication.EffectAutomationOnFunctionInvoked", CrowdyEffectAutomationTestFlags)
bool FCrowdyEffectAutomationOnFunctionInvokedTest::RunTest(const FString& Parameters)
{
	ExpectDiscoveryErrors(*this, 1);

	UCrowdyEffect* Effect = MakeAutomaticEffect();
	Effect->AutomationTrigger = ECrowdyEffectAutomationTrigger::OnFunctionInvoked;
	Effect->AutomationWatchFunctionName = TEXT("initiate_boss_wave");
	Effect->AutomationDebounceMs = 100;

	const FCrowdyEffectLoweringResult R = Effect->Compile();

	TestFalse(TEXT("no compile errors"), R.HasErrors());
	TestTrue(TEXT("function is autonomous-invocable"), R.Function.bAutonomousInvocable);

	if (TestTrue(TEXT("automation is set"), R.Automation.IsSet()))
	{
		TestEqual(TEXT("automation trigger type is event"), R.Automation.GetValue().TriggerType, FString(TEXT("event")));
	}

	if (TestTrue(TEXT("event trigger is set"), R.Trigger.IsSet()))
	{
		const FCrowdyGameModelAutomationTriggerInput& T = R.Trigger.GetValue();
		TestEqual(TEXT("trigger automation name"), T.AutomationName, FString(TEXT("hero_tick")));
		TestEqual(TEXT("on event"), T.OnEvent, FString(TEXT("function_invoked")));
		TestEqual(TEXT("function name carried"), T.FunctionName, FString(TEXT("initiate_boss_wave")));
		TestTrue(TEXT("no property key for a function-invoked trigger"), T.PropertyKey.IsEmpty());
		TestTrue(TEXT("unset container type filter stays empty so it matches any type"),
			T.ContainerTypeName.IsEmpty());
		TestTrue(TEXT("no write source: the server rejects a filter function_invoked cannot match"),
			T.WriteSource.IsEmpty());
		TestEqual(TEXT("debounce carried"), T.DebounceMs, 100);
	}
	return true;
}

// An explicit container type on a function-invoked trigger is sent as authored: it names the type the watched
// function runs on, which is how a cross-type reaction is narrowed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectAutomationOnFunctionInvokedExplicitTypeTest,
	"CrowdySDK.Replication.EffectAutomationOnFunctionInvokedExplicitType", CrowdyEffectAutomationTestFlags)
bool FCrowdyEffectAutomationOnFunctionInvokedExplicitTypeTest::RunTest(const FString& Parameters)
{
	ExpectDiscoveryErrors(*this, 1);

	UCrowdyEffect* Effect = MakeAutomaticEffect();
	Effect->AutomationTrigger = ECrowdyEffectAutomationTrigger::OnFunctionInvoked;
	Effect->AutomationWatchFunctionName = TEXT("initiate_boss_wave");
	Effect->AutomationChangeContainerType = TEXT("BossCharacter");

	const FCrowdyEffectLoweringResult R = Effect->Compile();

	if (TestTrue(TEXT("event trigger is set"), R.Trigger.IsSet()))
	{
		TestEqual(TEXT("explicit container type is the watched function's type, not the effect's"),
			R.Trigger.GetValue().ContainerTypeName, FString(TEXT("BossCharacter")));
	}
	return true;
}

// A cron effect compiles to a schedule/cron automation carrying the cron expression and no event trigger.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectAutomationCronTest,
	"CrowdySDK.Replication.EffectAutomationCron", CrowdyEffectAutomationTestFlags)
bool FCrowdyEffectAutomationCronTest::RunTest(const FString& Parameters)
{
	ExpectDiscoveryErrors(*this, 1);

	UCrowdyEffect* Effect = MakeAutomaticEffect();
	Effect->AutomationTrigger = ECrowdyEffectAutomationTrigger::Cron;
	Effect->AutomationCronExpr = TEXT("*/5 * * * *");

	const FCrowdyEffectLoweringResult R = Effect->Compile();

	TestFalse(TEXT("no compile errors"), R.HasErrors());
	if (TestTrue(TEXT("automation is set"), R.Automation.IsSet()))
	{
		const FCrowdyGameModelAutomationInput& A = R.Automation.GetValue();
		TestEqual(TEXT("trigger type"), A.TriggerType, FString(TEXT("schedule")));
		TestEqual(TEXT("schedule kind"), A.ScheduleKind, FString(TEXT("cron")));
		TestEqual(TEXT("cron expression carried"), A.CronExpr, FString(TEXT("*/5 * * * *")));
	}
	TestFalse(TEXT("no event trigger for a cron schedule"), R.Trigger.IsSet());
	return true;
}

// Regression: a plain effect (bRunAutomatically off) emits no automation, no trigger, and a non-autonomous
// function, so its compile is unchanged by the automation feature.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectAutomationPlainEffectUnchangedTest,
	"CrowdySDK.Replication.EffectAutomationPlainEffectUnchanged", CrowdyEffectAutomationTestFlags)
bool FCrowdyEffectAutomationPlainEffectUnchangedTest::RunTest(const FString& Parameters)
{
	ExpectDiscoveryErrors(*this, 1);

	UCrowdyEffect* Effect = MakeAutomaticEffect();
	Effect->bRunAutomatically = false;

	const FCrowdyEffectLoweringResult R = Effect->Compile();

	TestFalse(TEXT("no compile errors"), R.HasErrors());
	TestFalse(TEXT("no automation"), R.Automation.IsSet());
	TestFalse(TEXT("no trigger"), R.Trigger.IsSet());
	TestFalse(TEXT("function is not autonomous-invocable"), R.Function.bAutonomousInvocable);
	TestEqual(TEXT("invoke scope stays player"), R.Function.InvokeScope, FString(TEXT("player")));
	return true;
}

// Closing an automatic effect to players lowers the function to server invoke scope while still emitting the
// automation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectAutomationAutonomousOnlyTest,
	"CrowdySDK.Replication.EffectAutomationAutonomousOnly", CrowdyEffectAutomationTestFlags)
bool FCrowdyEffectAutomationAutonomousOnlyTest::RunTest(const FString& Parameters)
{
	ExpectDiscoveryErrors(*this, 1);

	UCrowdyEffect* Effect = MakeAutomaticEffect();
	Effect->CallableFrom = ECrowdyEffectCallableFrom::ServerOnly;

	const FCrowdyEffectLoweringResult R = Effect->Compile();

	TestFalse(TEXT("no compile errors"), R.HasErrors());
	TestEqual(TEXT("invoke scope is server"), R.Function.InvokeScope, FString(TEXT("server")));
	TestTrue(TEXT("function is autonomous-invocable"), R.Function.bAutonomousInvocable);
	TestTrue(TEXT("automation still emitted"), R.Automation.IsSet());
	return true;
}

// The automation name override is respected when set, and an empty override falls back to the effective function
// name.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectAutomationNameOverrideTest,
	"CrowdySDK.Replication.EffectAutomationNameOverride", CrowdyEffectAutomationTestFlags)
bool FCrowdyEffectAutomationNameOverrideTest::RunTest(const FString& Parameters)
{
	ExpectDiscoveryErrors(*this, 2);

	UCrowdyEffect* Named = MakeAutomaticEffect();
	Named->AutomationName = TEXT("hero_spawner");
	const FCrowdyEffectLoweringResult NamedResult = Named->Compile();
	if (TestTrue(TEXT("named automation is set"), NamedResult.Automation.IsSet()))
	{
		TestEqual(TEXT("override respected"), NamedResult.Automation.GetValue().Name, FString(TEXT("hero_spawner")));
	}

	UCrowdyEffect* Default = MakeAutomaticEffect();
	Default->AutomationName.Reset();
	const FCrowdyEffectLoweringResult DefaultResult = Default->Compile();
	if (TestTrue(TEXT("default automation is set"), DefaultResult.Automation.IsSet()))
	{
		TestEqual(TEXT("empty defaults to function name"), DefaultResult.Automation.GetValue().Name, FString(TEXT("hero_tick")));
	}
	return true;
}

// A Container-mode automation carries its target id in the automation's SelfContainerId (server targetMode
// "container"), but bakes NO static params: the model-changed notification names the container via the
// server-injected $self_container_id, evaluated per run, so nothing is passed in.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectAutomationContainerNotifyIdTest,
	"CrowdySDK.Replication.EffectAutomationContainerNotifyId", CrowdyEffectAutomationTestFlags)
bool FCrowdyEffectAutomationContainerNotifyIdTest::RunTest(const FString& Parameters)
{
	ExpectDiscoveryErrors(*this, 1);

	UCrowdyEffect* Effect = MakeAutomaticEffect();
	Effect->NotificationCarrier = ECrowdyEffectNotificationCarrier::Channel;
	Effect->AutomationTargetMode = ECrowdyEffectAutomationTargetMode::Container;
	Effect->AutomationTargetContainerId = TEXT("boss-container-1");

	const FCrowdyEffectLoweringResult R = Effect->Compile();

	TestFalse(TEXT("no compile errors"), R.HasErrors());
	if (TestTrue(TEXT("automation is set"), R.Automation.IsSet()))
	{
		const FCrowdyGameModelAutomationInput& A = R.Automation.GetValue();
		TestEqual(TEXT("target mode is container"), A.TargetMode, FString(TEXT("container")));
		TestEqual(TEXT("self container id carried"), A.SelfContainerId, FString(TEXT("boss-container-1")));
		TestTrue(TEXT("no params baked (server injects $self_container_id)"), A.ParamsJson.IsEmpty());
	}
	return true;
}

// No automation mode bakes static params: a None carrier authors no notification, and a carrier-bearing Container or
// Type mode relies on the server-injected $self_container_id, so every mode leaves ParamsJson empty (server "{}").
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectAutomationNoNotifyIdWhenAbsentTest,
	"CrowdySDK.Replication.EffectAutomationNoNotifyIdWhenAbsent", CrowdyEffectAutomationTestFlags)
bool FCrowdyEffectAutomationNoNotifyIdWhenAbsentTest::RunTest(const FString& Parameters)
{
	ExpectDiscoveryErrors(*this, 2);

	UCrowdyEffect* NoneCarrier = MakeAutomaticEffect();
	NoneCarrier->NotificationCarrier = ECrowdyEffectNotificationCarrier::None;
	NoneCarrier->AutomationTargetMode = ECrowdyEffectAutomationTargetMode::Container;
	NoneCarrier->AutomationTargetContainerId = TEXT("boss-container-1");
	const FCrowdyEffectLoweringResult NoneResult = NoneCarrier->Compile();
	if (TestTrue(TEXT("none-carrier automation is set"), NoneResult.Automation.IsSet()))
	{
		TestTrue(TEXT("none carrier bakes no params"), NoneResult.Automation.GetValue().ParamsJson.IsEmpty());
	}

	UCrowdyEffect* TypeMode = MakeAutomaticEffect();
	TypeMode->NotificationCarrier = ECrowdyEffectNotificationCarrier::Channel;
	TypeMode->AutomationTargetMode = ECrowdyEffectAutomationTargetMode::Type;
	const FCrowdyEffectLoweringResult TypeResult = TypeMode->Compile();
	if (TestTrue(TEXT("type-mode automation is set"), TypeResult.Automation.IsSet()))
	{
		TestTrue(TEXT("type mode bakes no params"), TypeResult.Automation.GetValue().ParamsJson.IsEmpty());
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
