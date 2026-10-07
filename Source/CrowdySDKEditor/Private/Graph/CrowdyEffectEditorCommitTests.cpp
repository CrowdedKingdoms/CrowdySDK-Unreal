// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Graph/CrowdyEffectEditorTestRig.h"
#include "Graph/CrowdyEffectGraph.h"
#include "Graph/CrowdyEffectGraphNodes.h"
#include "Graph/CrowdyEffectGraphSchema.h"
#include "Graph/CrowdyEffectGraphTestContainer.h"
#include "Graph/CrowdyEffectScriptAssetEditor.h"
#include "Graph/SCrowdyEffectGraphNode.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// The editors' commit handlers write straight onto the asset, past the property system, so nothing but these cases
// sees what a commit leaves behind. Both are driven through the real editor surfaces: the script toolkit opened the way
// a double-click opens it, and the node widget the graph panel builds.
namespace
{
	constexpr EAutomationTestFlags CrowdyEffectEditorCommitTestFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	const FName CrowdyEffectScriptToolkitName(TEXT("CrowdyEffectScriptEditor"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectScriptEditorCommitTest,
	"CrowdySDK.Editor.EffectScriptEditorCommit", CrowdyEffectEditorCommitTestFlags)
bool FCrowdyEffectScriptEditorCommitTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyEffect> Effect(NewObject<UCrowdyEffect>(GetTransientPackage(), NAME_None, RF_Transient));
	Effect->Source = ECrowdyEffectSource::Text;
	Effect->ContainerClass = UCrowdyEffectGraphTestContainer::StaticClass();
	Effect->EffectScript = TEXT("source.health -= 1");
	Effect->bRequiresSource = true;

	const FCrowdyAssetEditorTestRig Rig(Effect.Get());
	if (const FString Why = Rig.WhyUnavailable(); !Why.IsEmpty())
	{
		AddError(Why);
		return false;
	}

	FCrowdyEffectScriptAssetEditor* Editor = Rig.GetToolkit<FCrowdyEffectScriptAssetEditor>(CrowdyEffectScriptToolkitName);
	if (!TestNotNull(TEXT("a script effect opens the script editor"), Editor))
	{
		return false;
	}
	TestTrue(TEXT("with its script tab built"), Editor->HasScriptEditorForTest());

	Editor->CommitScriptForTest(TEXT("self.health -= 1"));
	TestEqual(TEXT("the committed body is written"), Effect->EffectScript, FString(TEXT("self.health -= 1")));
	TestFalse(TEXT("and a body that no longer reads source no longer requires one"), Effect->bRequiresSource);

	// Compared by hand: TestEqual on two FStrings ignores case, so it would pass with nothing written.
	Editor->CommitScriptForTest(TEXT("self.Health -= 1"));
	TestTrue(TEXT("a body changed only in case is written too"),
		Effect->EffectScript.Equals(TEXT("self.Health -= 1"), ESearchCase::CaseSensitive));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectGraphNodeTextFieldCommitTest,
	"CrowdySDK.Editor.EffectGraphNodeTextFieldCommit", CrowdyEffectEditorCommitTestFlags)
bool FCrowdyEffectGraphNodeTextFieldCommitTest::RunTest(const FString& Parameters)
{
	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	Effect->ContainerClass = UCrowdyEffectGraphTestContainer::StaticClass();

	UCrowdyEffectGraph* Graph = NewObject<UCrowdyEffectGraph>(Effect);
	Graph->Schema = UCrowdyEffectGraphSchema::StaticClass();

	UCrowdyEffectGraphNode_Constant* Constant = NewObject<UCrowdyEffectGraphNode_Constant>(Graph);
	Constant->CreateNewGuid();
	Graph->Nodes.Add(Constant);
	Constant->ConstantType = ECrowdyEffectGraphConstantType::String;
	Constant->Literal = TEXT("Hi");
	Constant->AllocateDefaultPins();

	const TSharedRef<SCrowdyEffectGraphNode> Widget = SNew(SCrowdyEffectGraphNode, Constant);
	if (!TestEqual(TEXT("a constant's body has one text field, its value"), Widget->NumTextFieldsForTest(), 1))
	{
		return false;
	}

	Widget->CommitTextFieldForTest(0, TEXT("Hello"));
	TestEqual(TEXT("the committed value is written"), Constant->Literal, FString(TEXT("Hello")));

	Widget->CommitTextFieldForTest(0, TEXT("hello"));
	TestTrue(TEXT("a value changed only in case is written too"),
		Constant->Literal.Equals(TEXT("hello"), ESearchCase::CaseSensitive));

	return true;
}

#endif
