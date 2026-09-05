// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Gql/CrowdyStudioGqlTestSupport.h"
#include "Gql/CrowdyStudioQueries.h"
#include "Model/CrowdyStudioTypes.h"
#include "Replication/GameModel/Effect/CrowdyGameModelAutomationInput.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyAutomationGqlTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// The input object nested inside a { input: {...} } variables wrapper.
	TSharedPtr<FJsonObject> GetInput(const TSharedPtr<FJsonObject>& Variables)
	{
		if (!Variables.IsValid())
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* Input = nullptr;
		if (Variables->TryGetObjectField(TEXT("input"), Input) && Input->IsValid())
		{
			return *Input;
		}
		return nullptr;
	}

	bool FieldIsJsonString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
		return Value.IsValid() && Value->Type == EJson::String;
	}

	bool FieldIsJsonNull(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
		return Value.IsValid() && Value->Type == EJson::Null;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAutomationUpsertVariablesTest,
	"CrowdySDK.Studio.AutomationUpsertVariables", CrowdyAutomationGqlTestFlags)
bool FCrowdyStudioAutomationUpsertVariablesTest::RunTest(const FString& Parameters)
{
	FCrowdyGameModelAutomationInput Automation;
	Automation.Name = TEXT("goblin_tick");
	Automation.Description = TEXT("Ticks every goblin");
	Automation.bEnabled = true;
	Automation.ActionKind = TEXT("model_function");
	Automation.FunctionName = TEXT("goblin_tick");
	Automation.TargetMode = TEXT("type");
	Automation.TargetTypeName = TEXT("Goblin");
	Automation.TriggerType = TEXT("schedule");
	Automation.ScheduleKind = TEXT("interval");
	Automation.IntervalMs = 1000;
	Automation.MaxTargets = 50;
	Automation.GasLimit = 100000;
	Automation.RunTimeoutMs = 2000;
	Automation.MaxRunsPerMinute = 120;
	Automation.FailureThreshold = 5;
	Automation.CooldownMs = 30000;
	// SelfContainerId / SessionId / SelectorJson / CronExpr / ParamsJson left empty.

	const TSharedPtr<FJsonObject> Variables =
		CrowdyStudioGql::BuildAutomationUpsertVariables(Automation, 123456789012LL);
	const TSharedPtr<FJsonObject> Input = GetInput(Variables);
	TestTrue(TEXT("input object present"), Input.IsValid());
	if (!Input.IsValid())
	{
		return false;
	}

	// appId is BigInt: it MUST be a JSON string, never a number.
	TestTrue(TEXT("appId is a JSON string"), FieldIsJsonString(Input, TEXT("appId")));
	FString AppId;
	TestTrue(TEXT("appId reads back"), Input->TryGetStringField(TEXT("appId"), AppId));
	TestEqual(TEXT("appId value"), AppId, FString(TEXT("123456789012")));

	FString Name;
	TestTrue(TEXT("name present"), Input->TryGetStringField(TEXT("name"), Name));
	TestEqual(TEXT("name value"), Name, FString(TEXT("goblin_tick")));

	FString Description;
	TestTrue(TEXT("description present"), Input->TryGetStringField(TEXT("description"), Description));
	TestEqual(TEXT("description value"), Description, FString(TEXT("Ticks every goblin")));

	bool bEnabled = false;
	TestTrue(TEXT("enabled present"), Input->TryGetBoolField(TEXT("enabled"), bEnabled));
	TestTrue(TEXT("enabled value"), bEnabled);

	FString ActionKind;
	Input->TryGetStringField(TEXT("actionKind"), ActionKind);
	TestEqual(TEXT("actionKind value"), ActionKind, FString(TEXT("model_function")));

	FString FunctionName;
	Input->TryGetStringField(TEXT("functionName"), FunctionName);
	TestEqual(TEXT("functionName value"), FunctionName, FString(TEXT("goblin_tick")));

	FString TargetMode;
	Input->TryGetStringField(TEXT("targetMode"), TargetMode);
	TestEqual(TEXT("targetMode value"), TargetMode, FString(TEXT("type")));

	FString TargetTypeName;
	Input->TryGetStringField(TEXT("targetTypeName"), TargetTypeName);
	TestEqual(TEXT("targetTypeName value"), TargetTypeName, FString(TEXT("Goblin")));

	// Unauthored optional string fields are sent as explicit JSON null so the server clears them and a re-plan is a no-op.
	TestTrue(TEXT("selfContainerId is null"), FieldIsJsonNull(Input, TEXT("selfContainerId")));
	TestTrue(TEXT("sessionId is null"), FieldIsJsonNull(Input, TEXT("sessionId")));
	TestTrue(TEXT("selectorJson is null"), FieldIsJsonNull(Input, TEXT("selectorJson")));
	TestTrue(TEXT("cronExpr is null"), FieldIsJsonNull(Input, TEXT("cronExpr")));

	// paramsJson is non-null on the server and defaults to "{}"; emit that default when unauthored.
	FString ParamsJson;
	TestTrue(TEXT("paramsJson present"), Input->TryGetStringField(TEXT("paramsJson"), ParamsJson));
	TestEqual(TEXT("paramsJson defaults to empty object"), ParamsJson, FString(TEXT("{}")));

	FString ScheduleKind;
	Input->TryGetStringField(TEXT("scheduleKind"), ScheduleKind);
	TestEqual(TEXT("scheduleKind value"), ScheduleKind, FString(TEXT("interval")));

	int32 IntervalMs = 0;
	TestTrue(TEXT("intervalMs present"), Input->TryGetNumberField(TEXT("intervalMs"), IntervalMs));
	TestEqual(TEXT("intervalMs value"), IntervalMs, 1000);

	// The safety budget ints are always emitted (idempotency).
	int32 MaxTargets = 0, GasLimit = 0, RunTimeoutMs = 0, MaxRunsPerMinute = 0, FailureThreshold = 0, CooldownMs = 0;
	Input->TryGetNumberField(TEXT("maxTargets"), MaxTargets);
	Input->TryGetNumberField(TEXT("gasLimit"), GasLimit);
	Input->TryGetNumberField(TEXT("runTimeoutMs"), RunTimeoutMs);
	Input->TryGetNumberField(TEXT("maxRunsPerMinute"), MaxRunsPerMinute);
	Input->TryGetNumberField(TEXT("failureThreshold"), FailureThreshold);
	Input->TryGetNumberField(TEXT("cooldownMs"), CooldownMs);
	TestEqual(TEXT("maxTargets"), MaxTargets, 50);
	TestEqual(TEXT("gasLimit"), GasLimit, 100000);
	TestEqual(TEXT("runTimeoutMs"), RunTimeoutMs, 2000);
	TestEqual(TEXT("maxRunsPerMinute"), MaxRunsPerMinute, 120);
	TestEqual(TEXT("failureThreshold"), FailureThreshold, 5);
	TestEqual(TEXT("cooldownMs"), CooldownMs, 30000);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAutomationParsesEnvelopeTest,
	"CrowdySDK.Studio.AutomationParsesEnvelope", CrowdyAutomationGqlTestFlags)
bool FCrowdyStudioAutomationParsesEnvelopeTest::RunTest(const FString& Parameters)
{
	// A GmAutomation read-back including the circuit-breaker runtime fields, which must be IGNORED (not part of the
	// desired schema). appId is a JSON string on the wire.
	const FString Envelope = TEXT(
		"{\"data\":{\"gameModelAutomations\":[{"
		"\"automationId\":\"auto-uuid-1\",\"appId\":\"123456789012\",\"name\":\"goblin_tick\","
		"\"description\":\"Ticks every goblin\",\"enabled\":true,\"actionKind\":\"model_function\","
		"\"functionName\":\"goblin_tick\",\"targetMode\":\"type\",\"selfContainerId\":null,"
		"\"targetTypeName\":\"Goblin\",\"sessionId\":null,\"paramsJson\":\"{}\",\"selectorJson\":null,"
		"\"triggerType\":\"schedule\",\"scheduleKind\":\"interval\",\"intervalMs\":1000,\"cronExpr\":null,"
		"\"maxTargets\":50,\"gasLimit\":100000,\"runTimeoutMs\":2000,\"maxRunsPerMinute\":120,"
		"\"failureThreshold\":5,\"cooldownMs\":30000,"
		"\"circuitState\":\"closed\",\"consecutiveFailures\":0,\"lastError\":null,\"lastRunAt\":null,\"nextRunAt\":null"
		"}]}}");

	TArray<FStudioAutomation> Automations;
	CrowdyStudioGql::ParseAutomations(CrowdyStudioGqlTest::ParseEnvelope(Envelope), TEXT("gameModelAutomations"), Automations);

	TestEqual(TEXT("one automation parsed"), Automations.Num(), 1);
	if (Automations.Num() != 1)
	{
		return false;
	}
	const FStudioAutomation& A = Automations[0];
	TestEqual(TEXT("automationId"), A.AutomationId, FString(TEXT("auto-uuid-1")));
	TestEqual(TEXT("name"), A.Name, FString(TEXT("goblin_tick")));
	TestEqual(TEXT("description"), A.Description, FString(TEXT("Ticks every goblin")));
	TestTrue(TEXT("enabled"), A.bEnabled);
	TestEqual(TEXT("actionKind"), A.ActionKind, FString(TEXT("model_function")));
	TestEqual(TEXT("functionName"), A.FunctionName, FString(TEXT("goblin_tick")));
	TestEqual(TEXT("targetMode"), A.TargetMode, FString(TEXT("type")));
	TestEqual(TEXT("targetTypeName"), A.TargetTypeName, FString(TEXT("Goblin")));
	TestEqual(TEXT("paramsJson"), A.ParamsJson, FString(TEXT("{}")));
	TestEqual(TEXT("triggerType"), A.TriggerType, FString(TEXT("schedule")));
	TestEqual(TEXT("scheduleKind"), A.ScheduleKind, FString(TEXT("interval")));
	TestEqual(TEXT("intervalMs"), A.IntervalMs, 1000);
	TestEqual(TEXT("maxTargets"), A.MaxTargets, 50);
	TestEqual(TEXT("gasLimit"), A.GasLimit, 100000);
	TestEqual(TEXT("runTimeoutMs"), A.RunTimeoutMs, 2000);
	TestEqual(TEXT("maxRunsPerMinute"), A.MaxRunsPerMinute, 120);
	TestEqual(TEXT("failureThreshold"), A.FailureThreshold, 5);
	TestEqual(TEXT("cooldownMs"), A.CooldownMs, 30000);
	// The unused nullable strings stay empty (null on the wire).
	TestEqual(TEXT("selfContainerId empty"), A.SelfContainerId, FString());
	TestEqual(TEXT("sessionId empty"), A.SessionId, FString());
	TestEqual(TEXT("selectorJson empty"), A.SelectorJson, FString());
	TestEqual(TEXT("cronExpr empty"), A.CronExpr, FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAutomationTriggerVariablesTest,
	"CrowdySDK.Studio.AutomationTriggerVariables", CrowdyAutomationGqlTestFlags)
bool FCrowdyStudioAutomationTriggerVariablesTest::RunTest(const FString& Parameters)
{
	FCrowdyGameModelAutomationTriggerInput Trigger;
	Trigger.AutomationName = TEXT("goblin_tick");
	Trigger.OnEvent = TEXT("property_changed");
	Trigger.ContainerTypeName = TEXT("Goblin");
	Trigger.PropertyKey = TEXT("hp");
	Trigger.DebounceMs = 250;
	// FunctionName left empty.

	const TSharedPtr<FJsonObject> Variables =
		CrowdyStudioGql::BuildAutomationTriggerUpsertVariables(Trigger, 123456789012LL);
	const TSharedPtr<FJsonObject> Input = GetInput(Variables);
	TestTrue(TEXT("input object present"), Input.IsValid());
	if (!Input.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("appId is a JSON string"), FieldIsJsonString(Input, TEXT("appId")));
	FString AppId;
	Input->TryGetStringField(TEXT("appId"), AppId);
	TestEqual(TEXT("appId value"), AppId, FString(TEXT("123456789012")));

	FString AutomationName;
	Input->TryGetStringField(TEXT("automationName"), AutomationName);
	TestEqual(TEXT("automationName value"), AutomationName, FString(TEXT("goblin_tick")));

	FString OnEvent;
	Input->TryGetStringField(TEXT("onEvent"), OnEvent);
	TestEqual(TEXT("onEvent value"), OnEvent, FString(TEXT("property_changed")));

	FString ContainerTypeName;
	Input->TryGetStringField(TEXT("containerTypeName"), ContainerTypeName);
	TestEqual(TEXT("containerTypeName value"), ContainerTypeName, FString(TEXT("Goblin")));

	FString PropertyKey;
	Input->TryGetStringField(TEXT("propertyKey"), PropertyKey);
	TestEqual(TEXT("propertyKey value"), PropertyKey, FString(TEXT("hp")));

	int32 DebounceMs = 0;
	Input->TryGetNumberField(TEXT("debounceMs"), DebounceMs);
	TestEqual(TEXT("debounceMs value"), DebounceMs, 250);

	// The unauthored function-name filter is an explicit JSON null (owned field, cleared).
	TestTrue(TEXT("functionName is null"), FieldIsJsonNull(Input, TEXT("functionName")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAutomationTriggerNameJoinTest,
	"CrowdySDK.Studio.AutomationTriggerNameJoin", CrowdyAutomationGqlTestFlags)
bool FCrowdyStudioAutomationTriggerNameJoinTest::RunTest(const FString& Parameters)
{
	// The trigger read-back references its automation by id, so the parser resolves the name from the automations
	// read in the same plan.
	FStudioAutomation Automation;
	Automation.AutomationId = TEXT("auto-uuid-1");
	Automation.Name = TEXT("goblin_tick");
	TArray<FStudioAutomation> Automations;
	Automations.Add(Automation);

	const FString Envelope = TEXT(
		"{\"data\":{\"gameModelAutomationTriggers\":[{"
		"\"triggerId\":\"trig-uuid-1\",\"appId\":\"123456789012\",\"automationId\":\"auto-uuid-1\","
		"\"onEvent\":\"property_changed\",\"functionName\":null,\"containerTypeName\":\"Goblin\","
		"\"propertyKey\":\"hp\",\"debounceMs\":250"
		"}]}}");

	TArray<FStudioAutomationTrigger> Triggers;
	CrowdyStudioGql::ParseAutomationTriggers(CrowdyStudioGqlTest::ParseEnvelope(Envelope), TEXT("gameModelAutomationTriggers"),
		Automations, Triggers);

	TestEqual(TEXT("one trigger parsed"), Triggers.Num(), 1);
	if (Triggers.Num() != 1)
	{
		return false;
	}
	const FStudioAutomationTrigger& T = Triggers[0];
	TestEqual(TEXT("triggerId"), T.TriggerId, FString(TEXT("trig-uuid-1")));
	TestEqual(TEXT("automationName resolved from id"), T.AutomationName, FString(TEXT("goblin_tick")));
	TestEqual(TEXT("onEvent"), T.OnEvent, FString(TEXT("property_changed")));
	TestEqual(TEXT("containerTypeName"), T.ContainerTypeName, FString(TEXT("Goblin")));
	TestEqual(TEXT("propertyKey"), T.PropertyKey, FString(TEXT("hp")));
	TestEqual(TEXT("debounceMs"), T.DebounceMs, 250);
	TestEqual(TEXT("functionName empty"), T.FunctionName, FString());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
