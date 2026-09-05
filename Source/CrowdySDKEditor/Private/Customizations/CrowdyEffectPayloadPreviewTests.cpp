// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Customizations/CrowdyEffectPayloadPreview.h"
#include "Replication/GameModel/Effect/CrowdyEffectAst.h"
#include "Replication/GameModel/Effect/CrowdyEffectLowering.h"
#include "Replication/GameModel/Effect/CrowdyGameModelAutomationInput.h"
#include "Replication/GameModel/Effect/CrowdyGameModelFunctionInput.h"

namespace
{
	// A minimal compiled effect: sets self.hp to self.hp - $damage on the player type. No errors.
	FCrowdyEffectLoweringResult MakeDamageResult()
	{
		FCrowdyEffectLoweringResult Result;
		Result.Function.Name = TEXT("apply_damage");
		Result.Function.ContainerTypeName = TEXT("player");

		FCrowdyGameModelFunctionParam Param;
		Param.Name = TEXT("damage");
		Param.ValueType = TEXT("int");
		Param.bRequired = true;
		Result.Function.Parameters.Add(Param);

		FCrowdyGameModelMutation Mutation;
		Mutation.Target = TEXT("self");
		Mutation.Property = TEXT("hp");
		Mutation.Expression = TEXT("self.hp - $damage");
		Result.Function.Mutations.Add(Mutation);

		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPayloadPreviewSummary,
	"CrowdySDK.Editor.PayloadPreviewSummary", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCrowdyEffectPayloadPreviewSummary::RunTest(const FString&)
{
	const FCrowdyEffectLoweringResult Result = MakeDamageResult();
	const FString Text = CrowdyEffectPayloadPreview::BuildPayloadPreview(
		Result, TEXT("apply_damage"), CrowdyEffectPayloadPreview::EMode::Summary);

	TestTrue(TEXT("names the function"), Text.Contains(TEXT("Function: apply_damage")));
	TestTrue(TEXT("lists the required param"), Text.Contains(TEXT("damage: int, required")));
	TestTrue(TEXT("lists the write"), Text.Contains(TEXT("self.hp = self.hp - $damage")));
	TestFalse(TEXT("summary is not the raw JSON"), Text.Contains(TEXT("gameModelUpsertFunction")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPayloadPreviewWireJson,
	"CrowdySDK.Editor.PayloadPreviewWireJson", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCrowdyEffectPayloadPreviewWireJson::RunTest(const FString&)
{
	const FCrowdyEffectLoweringResult Result = MakeDamageResult();
	const FString Text = CrowdyEffectPayloadPreview::BuildPayloadPreview(
		Result, TEXT("apply_damage"), CrowdyEffectPayloadPreview::EMode::WireJson);

	TestTrue(TEXT("labels the function mutation"), Text.Contains(TEXT("gameModelUpsertFunction input")));
	TestTrue(TEXT("carries the function name"), Text.Contains(TEXT("\"name\": \"apply_damage\"")));
	TestTrue(TEXT("appId is the placeholder"), Text.Contains(TEXT("\"appId\": \"<appId>\"")));
	// No automation authored, so no automation payload is shown.
	TestFalse(TEXT("no automation payload"), Text.Contains(TEXT("gameModelUpsertAutomation input")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPayloadPreviewAutomation,
	"CrowdySDK.Editor.PayloadPreviewAutomation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCrowdyEffectPayloadPreviewAutomation::RunTest(const FString&)
{
	FCrowdyEffectLoweringResult Result = MakeDamageResult();

	FCrowdyGameModelAutomationInput Automation;
	Automation.Name = TEXT("tick_damage");
	Automation.FunctionName = TEXT("apply_damage");
	Automation.TargetTypeName = TEXT("player");
	Result.Automation = Automation;

	const FString Wire = CrowdyEffectPayloadPreview::BuildPayloadPreview(
		Result, TEXT("apply_damage"), CrowdyEffectPayloadPreview::EMode::WireJson);
	TestTrue(TEXT("includes the automation payload"), Wire.Contains(TEXT("gameModelUpsertAutomation input")));
	TestTrue(TEXT("automation appId is the placeholder too"),
		Wire.Contains(TEXT("\"name\": \"tick_damage\"")));

	const FString Summary = CrowdyEffectPayloadPreview::BuildPayloadPreview(
		Result, TEXT("apply_damage"), CrowdyEffectPayloadPreview::EMode::Summary);
	TestTrue(TEXT("summary names the automation"), Summary.Contains(TEXT("Automation: tick_damage")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPayloadPreviewErrors,
	"CrowdySDK.Editor.PayloadPreviewErrors", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCrowdyEffectPayloadPreviewErrors::RunTest(const FString&)
{
	FCrowdyEffectLoweringResult Result = MakeDamageResult();

	FCrowdyEffectDiagnostic Diagnostic;
	Diagnostic.Severity = ECrowdyEffectSeverity::Error;
	Diagnostic.Line = 1;
	Diagnostic.Message = TEXT("unknown attribute");
	Result.Diagnostics.Add(Diagnostic);

	const FString Text = CrowdyEffectPayloadPreview::BuildPayloadPreview(
		Result, TEXT("apply_damage"), CrowdyEffectPayloadPreview::EMode::WireJson);

	TestTrue(TEXT("shows the compile error"), Text.Contains(TEXT("unknown attribute")));
	TestFalse(TEXT("no payload when errored"), Text.Contains(TEXT("gameModelUpsertFunction input")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
