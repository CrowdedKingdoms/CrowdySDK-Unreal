// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "GameModel/CrowdySchemaSync.h"
#include "Model/CrowdyStudioTypes.h"
#include "Replication/GameModel/Effect/CrowdyGameModelAutomationInput.h"
#include "Replication/GameModel/Effect/CrowdyGameModelFunctionInput.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyAutomationDiffTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A representative effect-authored automation: a type-scoped interval schedule that runs its entry-point function
	// over every container of the effect's own type (the shape an effect with a schedule lowers to). Budget fields
	// keep the input struct's explicit defaults.
	FCrowdyGameModelAutomationInput MakeDesiredAutomation()
	{
		FCrowdyGameModelAutomationInput A;
		A.Name = TEXT("goblin_tick");
		A.Description = TEXT("Ticks the goblin.");
		A.FunctionName = TEXT("goblin_tick");
		A.TargetMode = TEXT("type");
		A.TargetTypeName = TEXT("Goblin");
		A.ParamsJson = TEXT("{\"a\":100}");
		A.TriggerType = TEXT("schedule");
		A.ScheduleKind = TEXT("interval");
		A.IntervalMs = 1000;
		return A;
	}

	// A server automation that matches a desired input field-for-field (the idempotency baseline the tests perturb).
	FStudioAutomation MakeServerAutomationFrom(const FCrowdyGameModelAutomationInput& D)
	{
		FStudioAutomation S;
		S.Name = D.Name;
		S.Description = D.Description;
		S.bEnabled = D.bEnabled;
		S.ActionKind = D.ActionKind;
		S.FunctionName = D.FunctionName;
		S.TargetMode = D.TargetMode;
		S.SelfContainerId = D.SelfContainerId;
		S.TargetTypeName = D.TargetTypeName;
		S.SessionId = D.SessionId;
		S.ParamsJson = D.ParamsJson;
		S.SelectorJson = D.SelectorJson;
		S.TriggerType = D.TriggerType;
		S.ScheduleKind = D.ScheduleKind;
		S.IntervalMs = D.IntervalMs;
		S.CronExpr = D.CronExpr;
		S.MaxTargets = D.MaxTargets;
		S.GasLimit = D.GasLimit;
		S.RunTimeoutMs = D.RunTimeoutMs;
		S.MaxRunsPerMinute = D.MaxRunsPerMinute;
		S.FailureThreshold = D.FailureThreshold;
		S.CooldownMs = D.CooldownMs;
		return S;
	}

	// An autonomous-invocable function, the valid entry point a schedule automation references.
	FCrowdyGameModelFunctionInput MakeAutonomousFunction(const FString& Name)
	{
		FCrowdyGameModelFunctionInput F;
		F.Name = Name;
		F.ContainerTypeName = TEXT("Goblin");
		F.bAutonomousInvocable = true;
		return F;
	}

	FCrowdyGameModelAutomationTriggerInput MakeDesiredTrigger()
	{
		FCrowdyGameModelAutomationTriggerInput T;
		T.AutomationName = TEXT("goblin_tick");
		T.OnEvent = TEXT("property_changed");
		T.ContainerTypeName = TEXT("Goblin");
		T.PropertyKey = TEXT("health");
		T.DebounceMs = 200;
		return T;
	}

	FStudioAutomationTrigger MakeServerTriggerFrom(const FCrowdyGameModelAutomationTriggerInput& D)
	{
		FStudioAutomationTrigger S;
		S.AutomationName = D.AutomationName;
		S.OnEvent = D.OnEvent;
		S.FunctionName = D.FunctionName;
		S.ContainerTypeName = D.ContainerTypeName;
		S.PropertyKey = D.PropertyKey;
		S.DebounceMs = D.DebounceMs;
		return S;
	}
}

// A re-sync of an unchanged automation yields ZERO upserts even when the server formats paramsJson differently
// (100 vs 100.0). The correctness bar: paramsJson is compared SEMANTICALLY, every other field by value, and a
// referenced autonomous function produces no warning.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutomationDiffResyncNoOpTest,
	"CrowdySDK.Studio.AutomationDiffResyncIsNoOp", CrowdyAutomationDiffTestFlags)
bool FCrowdyAutomationDiffResyncNoOpTest::RunTest(const FString& Parameters)
{
	const FCrowdyGameModelAutomationInput Desired = MakeDesiredAutomation();

	FStudioAutomation Server = MakeServerAutomationFrom(Desired);
	Server.ParamsJson = TEXT("{\"a\":100.0}"); // 100.0 == 100 semantically -> not a change

	TArray<FCrowdyGameModelAutomationInput> DesiredAutos;
	DesiredAutos.Add(Desired);
	TArray<FStudioAutomation> CurrentAutos;
	CurrentAutos.Add(Server);
	TArray<FCrowdyGameModelFunctionInput> DesiredFns;
	DesiredFns.Add(MakeAutonomousFunction(TEXT("goblin_tick")));

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffAutomations(DesiredAutos, {}, CurrentAutos, {}, DesiredFns, Delta);

	TestEqual(TEXT("no automation upserts on a semantic-equal resync"), Delta.AutomationUpserts.Num(), 0);
	TestEqual(TEXT("no trigger upserts"), Delta.TriggerUpserts.Num(), 0);
	TestEqual(TEXT("no server-only automations"), Delta.ServerOnlyAutomations.Num(), 0);
	TestEqual(TEXT("no warnings (function present and autonomous)"), Delta.Warnings.Num(), 0);

	return true;
}

// An event-triggered automation carries the input struct's leftover schedule defaults (ScheduleKind "interval",
// IntervalMs 1000), but the server stores an event automation with those fields unset. The diff must ignore the
// schedule fields for a non-schedule trigger, so a re-sync is a no-op, not a perpetual re-upsert.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutomationDiffEventResyncNoOpTest,
	"CrowdySDK.Studio.AutomationDiffEventResyncIsNoOp", CrowdyAutomationDiffTestFlags)
bool FCrowdyAutomationDiffEventResyncNoOpTest::RunTest(const FString& Parameters)
{
	FCrowdyGameModelAutomationInput Desired = MakeDesiredAutomation();
	Desired.TriggerType = TEXT("event"); // an event automation keeps the struct's default ScheduleKind / IntervalMs

	FStudioAutomation Server = MakeServerAutomationFrom(Desired);
	Server.ScheduleKind = FString(); // the server stores an event automation with the schedule fields unset
	Server.IntervalMs = 0;

	TArray<FCrowdyGameModelAutomationInput> DesiredAutos;
	DesiredAutos.Add(Desired);
	TArray<FStudioAutomation> CurrentAutos;
	CurrentAutos.Add(Server);
	TArray<FCrowdyGameModelFunctionInput> DesiredFns;
	DesiredFns.Add(MakeAutonomousFunction(TEXT("goblin_tick")));

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffAutomations(DesiredAutos, {}, CurrentAutos, {}, DesiredFns, Delta);

	TestEqual(TEXT("no upsert for an unchanged event automation despite leftover schedule defaults"),
		Delta.AutomationUpserts.Num(), 0);
	return true;
}

// A cron automation uses cronExpr, not intervalMs; the input struct still carries the default IntervalMs (1000) but
// the server leaves intervalMs unset for a cron automation. The diff must compare only the field the schedule kind
// uses (cronExpr for cron), so a re-sync is a no-op.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutomationDiffCronResyncNoOpTest,
	"CrowdySDK.Studio.AutomationDiffCronResyncIsNoOp", CrowdyAutomationDiffTestFlags)
bool FCrowdyAutomationDiffCronResyncNoOpTest::RunTest(const FString& Parameters)
{
	FCrowdyGameModelAutomationInput Desired = MakeDesiredAutomation();
	Desired.ScheduleKind = TEXT("cron");
	Desired.CronExpr = TEXT("0 0 * * *"); // IntervalMs stays at its default 1000 (unused for cron)

	FStudioAutomation Server = MakeServerAutomationFrom(Desired);
	Server.IntervalMs = 0; // the server leaves intervalMs unset for a cron automation

	TArray<FCrowdyGameModelAutomationInput> DesiredAutos;
	DesiredAutos.Add(Desired);
	TArray<FStudioAutomation> CurrentAutos;
	CurrentAutos.Add(Server);
	TArray<FCrowdyGameModelFunctionInput> DesiredFns;
	DesiredFns.Add(MakeAutonomousFunction(TEXT("goblin_tick")));

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffAutomations(DesiredAutos, {}, CurrentAutos, {}, DesiredFns, Delta);

	TestEqual(TEXT("no upsert for an unchanged cron automation despite the leftover intervalMs default"),
		Delta.AutomationUpserts.Num(), 0);
	return true;
}

// An automation with no static params (empty ParamsJson) matches the server's non-null "{}" default; the diff
// normalizes an empty desired paramsJson to "{}" so a re-sync is a no-op rather than a perpetual re-upsert.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutomationDiffEmptyParamsResyncNoOpTest,
	"CrowdySDK.Studio.AutomationDiffEmptyParamsResyncIsNoOp", CrowdyAutomationDiffTestFlags)
bool FCrowdyAutomationDiffEmptyParamsResyncNoOpTest::RunTest(const FString& Parameters)
{
	FCrowdyGameModelAutomationInput Desired = MakeDesiredAutomation();
	Desired.ParamsJson = FString(); // no static params authored

	FStudioAutomation Server = MakeServerAutomationFrom(Desired);
	Server.ParamsJson = TEXT("{}"); // the server's non-null default read-back

	TArray<FCrowdyGameModelAutomationInput> DesiredAutos;
	DesiredAutos.Add(Desired);
	TArray<FStudioAutomation> CurrentAutos;
	CurrentAutos.Add(Server);
	TArray<FCrowdyGameModelFunctionInput> DesiredFns;
	DesiredFns.Add(MakeAutonomousFunction(TEXT("goblin_tick")));

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffAutomations(DesiredAutos, {}, CurrentAutos, {}, DesiredFns, Delta);

	TestEqual(TEXT("no upsert when empty desired paramsJson matches the server's {} default"),
		Delta.AutomationUpserts.Num(), 0);
	return true;
}

// An effect toggling "run automatically" flips only the function's autonomous flag. DiffFunctions must plan a
// function upsert for that lone change (else the flag never reaches the server and the automation cannot run), and
// once the server agrees the re-sync must plan nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutomationDiffAutonomousToggleTest,
	"CrowdySDK.Studio.AutomationDiffAutonomousFunctionToggle", CrowdyAutomationDiffTestFlags)
bool FCrowdyAutomationDiffAutonomousToggleTest::RunTest(const FString& Parameters)
{
	const FCrowdyGameModelFunctionInput Desired = MakeAutonomousFunction(TEXT("goblin_tick")); // bAutonomousInvocable = true

	FStudioFunction Server;
	Server.Name = Desired.Name;
	Server.ContainerTypeName = Desired.ContainerTypeName;
	Server.InvokeScope = Desired.InvokeScope; // both "player"
	Server.bAutonomousInvocable = false;      // the only difference

	{
		TArray<FCrowdyGameModelFunctionInput> DesiredFns;
		DesiredFns.Add(Desired);
		TArray<FStudioFunction> CurrentFns;
		CurrentFns.Add(Server);
		FCrowdySchemaDelta Delta;
		FCrowdySchemaSync::DiffFunctions(DesiredFns, CurrentFns, Delta);
		TestEqual(TEXT("autonomous toggle drives one function upsert"), Delta.FunctionUpserts.Num(), 1);
	}

	{
		Server.bAutonomousInvocable = true; // the server now agrees
		TArray<FCrowdyGameModelFunctionInput> DesiredFns;
		DesiredFns.Add(Desired);
		TArray<FStudioFunction> CurrentFns;
		CurrentFns.Add(Server);
		FCrowdySchemaDelta Delta;
		FCrowdySchemaSync::DiffFunctions(DesiredFns, CurrentFns, Delta);
		TestEqual(TEXT("no function upsert when the autonomous flag already matches"), Delta.FunctionUpserts.Num(), 0);
	}
	return true;
}

// An automation with no server counterpart is a create upsert (bIsNew), with no warning and no server-only entry.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutomationDiffCreatesNewTest,
	"CrowdySDK.Studio.AutomationDiffCreatesNew", CrowdyAutomationDiffTestFlags)
bool FCrowdyAutomationDiffCreatesNewTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyGameModelAutomationInput> DesiredAutos;
	DesiredAutos.Add(MakeDesiredAutomation());
	TArray<FStudioAutomation> CurrentAutos; // empty server
	TArray<FCrowdyGameModelFunctionInput> DesiredFns;
	DesiredFns.Add(MakeAutonomousFunction(TEXT("goblin_tick")));

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffAutomations(DesiredAutos, {}, CurrentAutos, {}, DesiredFns, Delta);

	if (TestEqual(TEXT("one automation upsert"), Delta.AutomationUpserts.Num(), 1))
	{
		TestTrue(TEXT("flagged new"), Delta.AutomationUpserts[0].bIsNew);
		TestEqual(TEXT("the goblin_tick automation"), Delta.AutomationUpserts[0].Automation.Name, FString(TEXT("goblin_tick")));
	}
	TestEqual(TEXT("no warnings for a plain create"), Delta.Warnings.Num(), 0);
	TestEqual(TEXT("no server-only automations"), Delta.ServerOnlyAutomations.Num(), 0);

	return true;
}

// A budget field or the interval differing on an otherwise-matching automation is an update (never a create), and
// nothing else is upserted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutomationDiffDetectsChangeTest,
	"CrowdySDK.Studio.AutomationDiffDetectsChange", CrowdyAutomationDiffTestFlags)
bool FCrowdyAutomationDiffDetectsChangeTest::RunTest(const FString& Parameters)
{
	const FCrowdyGameModelAutomationInput Desired = MakeDesiredAutomation();
	TArray<FCrowdyGameModelAutomationInput> DesiredAutos;
	DesiredAutos.Add(Desired);
	TArray<FCrowdyGameModelFunctionInput> DesiredFns;
	DesiredFns.Add(MakeAutonomousFunction(TEXT("goblin_tick")));

	// The interval differs: an update.
	FStudioAutomation IntervalServer = MakeServerAutomationFrom(Desired);
	IntervalServer.IntervalMs = 2000;
	TArray<FStudioAutomation> IntervalAutos;
	IntervalAutos.Add(IntervalServer);

	FCrowdySchemaDelta IntervalDelta;
	FCrowdySchemaSync::DiffAutomations(DesiredAutos, {}, IntervalAutos, {}, DesiredFns, IntervalDelta);
	if (TestEqual(TEXT("an interval change is one upsert"), IntervalDelta.AutomationUpserts.Num(), 1))
	{
		TestFalse(TEXT("the interval change is an update, not a create"), IntervalDelta.AutomationUpserts[0].bIsNew);
	}
	TestEqual(TEXT("an interval change is not a prune"), IntervalDelta.ServerOnlyAutomations.Num(), 0);

	// A budget field differs: also an update.
	FStudioAutomation BudgetServer = MakeServerAutomationFrom(Desired);
	BudgetServer.MaxTargets = Desired.MaxTargets + 5;
	TArray<FStudioAutomation> BudgetAutos;
	BudgetAutos.Add(BudgetServer);

	FCrowdySchemaDelta BudgetDelta;
	FCrowdySchemaSync::DiffAutomations(DesiredAutos, {}, BudgetAutos, {}, DesiredFns, BudgetDelta);
	if (TestEqual(TEXT("a budget change is one upsert"), BudgetDelta.AutomationUpserts.Num(), 1))
	{
		TestFalse(TEXT("the budget change is an update"), BudgetDelta.AutomationUpserts[0].bIsNew);
	}

	return true;
}

// A server automation no effect authors is a server-only prune candidate (+warning); the sync never deletes it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutomationDiffServerOnlyTest,
	"CrowdySDK.Studio.AutomationDiffServerOnly", CrowdyAutomationDiffTestFlags)
bool FCrowdyAutomationDiffServerOnlyTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyGameModelAutomationInput> DesiredAutos; // nothing authored in code

	FStudioAutomation Orphan;
	Orphan.Name = TEXT("legacy_auto");
	TArray<FStudioAutomation> CurrentAutos;
	CurrentAutos.Add(Orphan);

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffAutomations(DesiredAutos, {}, CurrentAutos, {}, {}, Delta);

	if (TestEqual(TEXT("one server-only automation"), Delta.ServerOnlyAutomations.Num(), 1))
	{
		TestEqual(TEXT("legacy_auto is the prune candidate"), Delta.ServerOnlyAutomations[0], FString(TEXT("legacy_auto")));
	}
	TestEqual(TEXT("the server-only automation is warned"), Delta.Warnings.Num(), 1);
	TestEqual(TEXT("no upserts"), Delta.AutomationUpserts.Num(), 0);

	return true;
}

// A server automation whose name carries a deployed Game Kit's function-name prefix (toSnakeCase(prefix) + "_") is
// kit-owned, so the diff keeps it off the prune list; a name matching no recognized prefix is still a candidate, and
// without any recognized prefixes both are prunable (guards against the protection silently becoming a no-op).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutomationDiffProtectsKitPrefixTest,
	"CrowdySDK.Studio.AutomationDiffProtectsKitPrefix", CrowdyAutomationDiffTestFlags)
bool FCrowdyAutomationDiffProtectsKitPrefixTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyGameModelAutomationInput> DesiredAutos; // nothing authored in code

	FStudioAutomation KitAuto;
	KitAuto.Name = TEXT("goblin_tick"); // kit automation for prefix "Goblin"
	FStudioAutomation Orphan;
	Orphan.Name = TEXT("legacy_auto"); // no kit, no effect owns this

	TArray<FStudioAutomation> CurrentAutos;
	CurrentAutos.Add(KitAuto);
	CurrentAutos.Add(Orphan);

	TSet<FString> Prefixes;
	Prefixes.Add(TEXT("Goblin"));

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffAutomations(DesiredAutos, {}, CurrentAutos, {}, {}, Delta, TSet<FString>(), Prefixes);
	if (TestEqual(TEXT("only the non-kit orphan is a prune candidate"), Delta.ServerOnlyAutomations.Num(), 1))
	{
		TestEqual(TEXT("legacy_auto is the only server-only automation"), Delta.ServerOnlyAutomations[0], FString(TEXT("legacy_auto")));
	}
	TestFalse(TEXT("a kit automation is not offered for prune"), Delta.ServerOnlyAutomations.Contains(TEXT("goblin_tick")));

	// Without recognized prefixes, both server automations are prune candidates.
	FCrowdySchemaDelta Unprotected;
	FCrowdySchemaSync::DiffAutomations(DesiredAutos, {}, CurrentAutos, {}, {}, Unprotected);
	TestEqual(TEXT("without protection both are prunable"), Unprotected.ServerOnlyAutomations.Num(), 2);

	return true;
}

// The exact-name kit layer: a bare-named kit automation carries no recognizable prefix. With its name in
// RecognizedKitAutomationNames the diff keeps it off the prune list; an unrecognized automation is still a candidate.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutomationDiffProtectsExactKitNamesTest,
	"CrowdySDK.Studio.AutomationDiffProtectsExactKitNames", CrowdyAutomationDiffTestFlags)
bool FCrowdyAutomationDiffProtectsExactKitNamesTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyGameModelAutomationInput> DesiredAutos;

	FStudioAutomation KitAuto;
	KitAuto.Name = TEXT("spawn_wave"); // empty-prefix kit automation, protected by exact name
	FStudioAutomation Orphan;
	Orphan.Name = TEXT("legacy_auto");

	TArray<FStudioAutomation> CurrentAutos;
	CurrentAutos.Add(KitAuto);
	CurrentAutos.Add(Orphan);

	TSet<FString> KitNames;
	KitNames.Add(TEXT("spawn_wave"));

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffAutomations(DesiredAutos, {}, CurrentAutos, {}, {}, Delta, TSet<FString>(), TSet<FString>(), KitNames);
	if (TestEqual(TEXT("only the unrecognized automation is a prune candidate"), Delta.ServerOnlyAutomations.Num(), 1))
	{
		TestEqual(TEXT("legacy_auto is the only server-only automation"), Delta.ServerOnlyAutomations[0], FString(TEXT("legacy_auto")));
	}
	TestFalse(TEXT("an exact-named kit automation is not offered for prune"), Delta.ServerOnlyAutomations.Contains(TEXT("spawn_wave")));

	return true;
}

// A server automation whose name is in RecognizedAutomationNames (owned by an effect SKIPPED this plan, e.g. an
// unmigrated or non-compiling effect) is protected: NOT offered for prune even though it is absent from the desired
// set. A genuine orphan is still a candidate. Guards against a transiently-skipped effect losing its live automation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutomationDiffProtectsRecognizedTest,
	"CrowdySDK.Studio.AutomationDiffProtectsRecognized", CrowdyAutomationDiffTestFlags)
bool FCrowdyAutomationDiffProtectsRecognizedTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyGameModelAutomationInput> DesiredAutos; // every effect was skipped this plan, or there are none

	FStudioAutomation Skipped;
	Skipped.Name = TEXT("goblin_tick"); // an effect owns this name but was skipped
	FStudioAutomation Orphan;
	Orphan.Name = TEXT("legacy_auto");

	TArray<FStudioAutomation> CurrentAutos;
	CurrentAutos.Add(Skipped);
	CurrentAutos.Add(Orphan);

	TSet<FString> Recognized;
	Recognized.Add(TEXT("goblin_tick"));

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffAutomations(DesiredAutos, {}, CurrentAutos, {}, {}, Delta, Recognized);
	if (TestEqual(TEXT("one server-only automation"), Delta.ServerOnlyAutomations.Num(), 1))
	{
		TestEqual(TEXT("only the orphan is prunable"), Delta.ServerOnlyAutomations[0], FString(TEXT("legacy_auto")));
	}
	TestFalse(TEXT("the recognized automation is not a prune candidate"),
		Delta.ServerOnlyAutomations.Contains(TEXT("goblin_tick")));

	// Without the recognized set, both are prune candidates (guards against the protection silently no-opping).
	FCrowdySchemaDelta Unprotected;
	FCrowdySchemaSync::DiffAutomations(DesiredAutos, {}, CurrentAutos, {}, {}, Unprotected);
	TestEqual(TEXT("without protection both are prunable"), Unprotected.ServerOnlyAutomations.Num(), 2);

	return true;
}

// A desired automation referencing a function no effect authors warns (a dangling reference), while the automation
// itself still plans as a create (the warning is non-destructive).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutomationDiffDanglingFunctionWarnsTest,
	"CrowdySDK.Studio.AutomationDiffDanglingFunctionWarns", CrowdyAutomationDiffTestFlags)
bool FCrowdyAutomationDiffDanglingFunctionWarnsTest::RunTest(const FString& Parameters)
{
	FCrowdyGameModelAutomationInput Desired = MakeDesiredAutomation();
	Desired.FunctionName = TEXT("missing_fn");
	TArray<FCrowdyGameModelAutomationInput> DesiredAutos;
	DesiredAutos.Add(Desired);

	TArray<FCrowdyGameModelFunctionInput> DesiredFns; // nothing authors missing_fn

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffAutomations(DesiredAutos, {}, {}, {}, DesiredFns, Delta);

	TestEqual(TEXT("the automation still plans as a create"), Delta.AutomationUpserts.Num(), 1);
	const bool bWarned = Delta.Warnings.ContainsByPredicate([](const FString& W)
	{
		return W.Contains(TEXT("missing_fn")) && W.Contains(TEXT("no effect authors"));
	});
	TestTrue(TEXT("the dangling function reference is warned"), bWarned);

	return true;
}

// A desired automation whose referenced function exists but is NOT autonomous-invocable warns (it cannot run).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutomationDiffNonAutonomousWarnsTest,
	"CrowdySDK.Studio.AutomationDiffNonAutonomousWarns", CrowdyAutomationDiffTestFlags)
bool FCrowdyAutomationDiffNonAutonomousWarnsTest::RunTest(const FString& Parameters)
{
	FCrowdyGameModelAutomationInput Desired = MakeDesiredAutomation();
	Desired.FunctionName = TEXT("plain_fn");
	TArray<FCrowdyGameModelAutomationInput> DesiredAutos;
	DesiredAutos.Add(Desired);

	FCrowdyGameModelFunctionInput Plain = MakeAutonomousFunction(TEXT("plain_fn"));
	Plain.bAutonomousInvocable = false; // a player function the automation cannot run
	TArray<FCrowdyGameModelFunctionInput> DesiredFns;
	DesiredFns.Add(Plain);

	FCrowdySchemaDelta Delta;
	FCrowdySchemaSync::DiffAutomations(DesiredAutos, {}, {}, {}, DesiredFns, Delta);

	const bool bWarned = Delta.Warnings.ContainsByPredicate([](const FString& W)
	{
		return W.Contains(TEXT("plain_fn")) && W.Contains(TEXT("autonomous"));
	});
	TestTrue(TEXT("the non-autonomous function reference is warned"), bWarned);

	return true;
}

// Event triggers: a trigger with no server counterpart is a create, an identical one is a no-op, and a debounce-only
// change is an update on the same trigger (matched by automation + event + filters).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutomationDiffTriggersTest,
	"CrowdySDK.Studio.AutomationDiffTriggers", CrowdyAutomationDiffTestFlags)
bool FCrowdyAutomationDiffTriggersTest::RunTest(const FString& Parameters)
{
	const FCrowdyGameModelAutomationTriggerInput Desired = MakeDesiredTrigger();
	TArray<FCrowdyGameModelAutomationTriggerInput> DesiredTriggers;
	DesiredTriggers.Add(Desired);

	// No server trigger -> a create.
	FCrowdySchemaDelta CreateDelta;
	FCrowdySchemaSync::DiffAutomations({}, DesiredTriggers, {}, {}, {}, CreateDelta);
	if (TestEqual(TEXT("a new trigger is one create upsert"), CreateDelta.TriggerUpserts.Num(), 1))
	{
		TestTrue(TEXT("flagged new"), CreateDelta.TriggerUpserts[0].bIsNew);
	}

	// An identical server trigger -> a no-op.
	TArray<FStudioAutomationTrigger> SameTriggers;
	SameTriggers.Add(MakeServerTriggerFrom(Desired));
	FCrowdySchemaDelta NoOp;
	FCrowdySchemaSync::DiffAutomations({}, DesiredTriggers, {}, SameTriggers, {}, NoOp);
	TestEqual(TEXT("an identical trigger is a no-op"), NoOp.TriggerUpserts.Num(), 0);

	// A debounce-only change -> an update on the same trigger (the filter key still matches).
	FStudioAutomationTrigger Debounced = MakeServerTriggerFrom(Desired);
	Debounced.DebounceMs = Desired.DebounceMs + 100;
	TArray<FStudioAutomationTrigger> DebouncedTriggers;
	DebouncedTriggers.Add(Debounced);
	FCrowdySchemaDelta UpdateDelta;
	FCrowdySchemaSync::DiffAutomations({}, DesiredTriggers, {}, DebouncedTriggers, {}, UpdateDelta);
	if (TestEqual(TEXT("a debounce change is one update upsert"), UpdateDelta.TriggerUpserts.Num(), 1))
	{
		TestFalse(TEXT("the debounce change is an update, not a create"), UpdateDelta.TriggerUpserts[0].bIsNew);
	}

	return true;
}

// BuildReport fills the automation / trigger counts and adds one human-readable line per planned upsert, and the
// server-only automation count flows through.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAutomationDiffReportCountsTest,
	"CrowdySDK.Studio.AutomationDiffReportCounts", CrowdyAutomationDiffTestFlags)
bool FCrowdyAutomationDiffReportCountsTest::RunTest(const FString& Parameters)
{
	FCrowdySchemaDelta Delta;
	Delta.AutomationUpserts.Add({ MakeDesiredAutomation(), /*bIsNew*/ true });

	FCrowdyGameModelAutomationInput Updated = MakeDesiredAutomation();
	Updated.Name = TEXT("dragon_tick");
	Delta.AutomationUpserts.Add({ Updated, /*bIsNew*/ false });

	Delta.TriggerUpserts.Add({ MakeDesiredTrigger(), /*bIsNew*/ true });
	Delta.ServerOnlyAutomations.Add(TEXT("legacy_auto"));

	const FCrowdySchemaSyncReport Report = FCrowdySchemaSync::BuildReport(Delta, {}, /*bApplied*/ false);

	TestEqual(TEXT("one automation create counted"), Report.AutomationsToCreate, 1);
	TestEqual(TEXT("one automation update counted"), Report.AutomationsToUpdate, 1);
	TestEqual(TEXT("one trigger create counted"), Report.TriggersToCreate, 1);
	TestEqual(TEXT("no trigger updates"), Report.TriggersToUpdate, 0);
	TestEqual(TEXT("the server-only automation count flows through"), Report.ServerOnlyAutomationCount, 1);

	const bool bHasAutoLine = Report.Lines.ContainsByPredicate([](const FString& L)
	{
		return L.Contains(TEXT("automation")) && L.Contains(TEXT("goblin_tick"));
	});
	TestTrue(TEXT("a per-automation line is present"), bHasAutoLine);
	const bool bHasTriggerLine = Report.Lines.ContainsByPredicate([](const FString& L)
	{
		return L.Contains(TEXT("trigger")) && L.Contains(TEXT("property_changed"));
	});
	TestTrue(TEXT("a per-trigger line is present"), bHasTriggerLine);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
