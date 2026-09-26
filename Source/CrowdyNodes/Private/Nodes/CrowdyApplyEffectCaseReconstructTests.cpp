// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_BaseAsyncTask.h"
#include "Nodes/CrowdyApplyEffectNodePins.h"
#include "Nodes/CrowdyApplyEffectNodeShared.h"
#include "Nodes/CrowdyK2Node_ApplyEffect.h"
#include "Nodes/CrowdyK2Node_ApplyEffectFireAndForget.h"
#include "Replication/GameModel/CrowdyEffectActions.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "UObject/UnrealType.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyCaseReconstructTestFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	UCrowdyEffect* MakeCaseReconstructEffect()
	{
		FCrowdyEffectMagnitude Magnitude;
		Magnitude.Name = TEXT("element");
		Magnitude.ValueTypeEnum = ECrowdyEffectValueType::String;
		Magnitude.ValueType = UCrowdyEffect::ValueTypeToWireString(ECrowdyEffectValueType::String);
		Magnitude.DefaultValueJson = TEXT("\"Fire\"");

		UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
		Effect->Magnitudes.Add(Magnitude);
		return Effect;
	}

	UEdGraph* MakeCaseReconstructGraph()
	{
		UBlueprint* Blueprint = NewObject<UBlueprint>(GetTransientPackage());
		UEdGraph* Graph = NewObject<UEdGraph>(Blueprint);
		Graph->Schema = UEdGraphSchema_K2::StaticClass();
		return Graph;
	}

	// Rebuilds Node with Effect on its Effect pin and checks the element pin's value after each rebuild.
	void CheckCaseOnlyValueSurvivesReconstruct(FAutomationTestBase& Test, UK2Node* Node, UCrowdyEffect* Effect)
	{
		const FName ElementPin = CrowdyApplyEffectNodePins::MagnitudePinName(TEXT("element"));
		Node->AllocateDefaultPins();
		UEdGraphPin* EffectPin = Node->FindPin(CrowdyApplyEffectNodeShared::PN_Effect, EGPD_Input);
		if (!Test.TestNotNull(TEXT("the node has an Effect pin"), EffectPin))
		{
			return;
		}
		EffectPin->DefaultObject = Effect;
		Node->ReconstructNode();

		UEdGraphPin* Pin = Node->FindPin(ElementPin, EGPD_Input);
		if (!Test.TestNotNull(TEXT("the magnitude pin exists"), Pin))
		{
			return;
		}
		Test.TestTrue(TEXT("the pin is prefilled with the effect's default"),
			Pin->DefaultValue.Equals(TEXT("Fire"), ESearchCase::CaseSensitive));

		Pin->DefaultValue = TEXT("fire");
		Node->ReconstructNode();
		Pin = Node->FindPin(ElementPin, EGPD_Input);
		if (!Test.TestNotNull(TEXT("the magnitude pin survives the rebuild"), Pin))
		{
			return;
		}
		Test.TestTrue(TEXT("a value changed only in case survives the rebuild"),
			Pin->DefaultValue.Equals(TEXT("fire"), ESearchCase::CaseSensitive));

		Pin->DefaultValue = TEXT("Fire");
		Node->ReconstructNode();
		Pin = Node->FindPin(ElementPin, EGPD_Input);
		if (!Test.TestNotNull(TEXT("the magnitude pin survives a second rebuild"), Pin))
		{
			return;
		}
		Test.TestTrue(TEXT("an untouched value stays the default"),
			Pin->DefaultValue.Equals(TEXT("Fire"), ESearchCase::CaseSensitive));
	}
}

// The schema carries a pin value across a rebuild only when it differs from the default ignoring case, so a string
// magnitude set to "fire" over a default "Fire" would snap back on every reconstruct, including every Blueprint load.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyEffectCallCaseOnlyValueSurvivesReconstructTest,
	"CrowdySDK.Editor.ApplyEffectCallCaseOnlyValueSurvivesReconstruct", CrowdyCaseReconstructTestFlags)
bool FCrowdyApplyEffectCallCaseOnlyValueSurvivesReconstructTest::RunTest(const FString& Parameters)
{
	UEdGraph* Graph = MakeCaseReconstructGraph();
	UCrowdyK2Node_ApplyEffectFireAndForget* Node = NewObject<UCrowdyK2Node_ApplyEffectFireAndForget>(Graph);
	Graph->AddNode(Node, false, false);

	// Set up front as an external member, since this test Blueprint has no class for the node to be scoped to.
	const UFunction* Wrapped = Node->GetWrappedFunction();
	if (!TestNotNull(TEXT("the wrapped function resolves"), Wrapped))
	{
		return false;
	}
	Node->FunctionReference.SetExternalMember(Wrapped->GetFName(), Wrapped->GetOwnerClass());

	CheckCaseOnlyValueSurvivesReconstruct(*this, Node, MakeCaseReconstructEffect());
	return true;
}

// The same for the latent node, which is configured with its factory the way the action menu spawner does it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyApplyEffectLatentCaseOnlyValueSurvivesReconstructTest,
	"CrowdySDK.Editor.ApplyEffectLatentCaseOnlyValueSurvivesReconstruct", CrowdyCaseReconstructTestFlags)
bool FCrowdyApplyEffectLatentCaseOnlyValueSurvivesReconstructTest::RunTest(const FString& Parameters)
{
	UFunction* Factory = UCrowdyApplyEffectAction::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UCrowdyApplyEffectAction, ApplyEffect));
	const FObjectProperty* ReturnProp = Factory ? CastField<FObjectProperty>(Factory->GetReturnProperty()) : nullptr;
	if (!TestNotNull(TEXT("the factory resolves"), ReturnProp))
	{
		return false;
	}

	UEdGraph* Graph = MakeCaseReconstructGraph();
	UCrowdyK2Node_ApplyEffect* Node = NewObject<UCrowdyK2Node_ApplyEffect>(Graph);
	Graph->AddNode(Node, false, false);

	const UClass* AsyncTask = UK2Node_BaseAsyncTask::StaticClass();
	const FNameProperty* FactoryName = CastField<FNameProperty>(AsyncTask->FindPropertyByName(TEXT("ProxyFactoryFunctionName")));
	const FObjectPropertyBase* FactoryClass = CastField<FObjectPropertyBase>(AsyncTask->FindPropertyByName(TEXT("ProxyFactoryClass")));
	const FObjectPropertyBase* ProxyClass = CastField<FObjectPropertyBase>(AsyncTask->FindPropertyByName(TEXT("ProxyClass")));
	if (!TestNotNull(TEXT("ProxyFactoryFunctionName"), FactoryName) || !TestNotNull(TEXT("ProxyFactoryClass"), FactoryClass)
		|| !TestNotNull(TEXT("ProxyClass"), ProxyClass))
	{
		return false;
	}
	FactoryName->SetPropertyValue_InContainer(Node, Factory->GetFName());
	FactoryClass->SetObjectPropertyValue_InContainer(Node, Factory->GetOuterUClass());
	ProxyClass->SetObjectPropertyValue_InContainer(Node, ReturnProp->PropertyClass);

	CheckCaseOnlyValueSurvivesReconstruct(*this, Node, MakeCaseReconstructEffect());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
