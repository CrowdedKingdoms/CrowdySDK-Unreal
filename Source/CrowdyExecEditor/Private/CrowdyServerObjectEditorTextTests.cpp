#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectDefinitionCustomization.h"
#include "CrowdyServerObjectSelection.h"
#include "Misc/AutomationTest.h"
#include "SCrowdyServerObjectMembers.h"
#include "StructUtils/PropertyBag.h"

namespace CrowdyServerObjectEditorTextTest
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	const FCrowdyServerFunction* FindMemberFunction(FName Name)
	{
		return UCrowdyServerObjectDefinition::GetMemberFunctions().FindByPredicate([Name](const FCrowdyServerFunction& Function) { return Function.Name == Name; });
	}

	SCrowdyServerObjectMembers::FItemPtr MakeItem(SCrowdyServerObjectMembers::FItem::EType Type, FName Name = NAME_None, int32 Index = INDEX_NONE)
	{
		SCrowdyServerObjectMembers::FItemPtr Item = MakeShared<SCrowdyServerObjectMembers::FItem>();
		Item->Type = Type;
		Item->Name = Name;
		Item->Index = Index;
		return Item;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerSecondsTextTest, "CrowdySDK.CrowdyExecEditor.SecondsShowWithoutTrailingZero", CrowdyServerObjectEditorTextTest::TestFlags)
bool FCrowdyServerSecondsTextTest::RunTest(const FString& Parameters)
{
	const TSharedRef<INumericTypeInterface<float>> Seconds = CrowdyServerObjectText::MakeSecondsInterface();
	TestEqual(TEXT("whole seconds have no .0"), Seconds->ToString(2.0f), FString(TEXT("2 s")));
	TestEqual(TEXT("a fraction keeps its digits"), Seconds->ToString(0.25f), FString(TEXT("0.25 s")));
	TestEqual(TEXT("five minutes stay in seconds"), Seconds->ToString(300.0f), FString(TEXT("300 s")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerTimerTitleTest, "CrowdySDK.CrowdyExecEditor.TimerTitleSaysWhatItDoes", CrowdyServerObjectEditorTextTest::TestFlags)
bool FCrowdyServerTimerTitleTest::RunTest(const FString& Parameters)
{
	FCrowdyServerTimer Respawn;
	Respawn.Name = TEXT("Respawn");
	Respawn.Repeat = ECrowdyServerTimerRepeat::After;
	Respawn.Seconds = 10.0f;
	TestEqual(TEXT("a one-off timer"), CrowdyServerObjectText::TimerTitle(Respawn).ToString(), FString(TEXT("Respawn: Once After 10 s")));

	FCrowdyServerTimer Tick;
	Tick.Name = TEXT("Tick");
	Tick.Repeat = ECrowdyServerTimerRepeat::Every;
	Tick.Seconds = 2.0f;
	TestEqual(TEXT("a repeating timer"), CrowdyServerObjectText::TimerTitle(Tick).ToString(), FString(TEXT("Tick: Every 2 s")));

	TestEqual(TEXT("an unnamed timer reads as New Timer"), CrowdyServerObjectText::TimerTitle(FCrowdyServerTimer()).ToString(), FString(TEXT("New Timer: Every 60 s")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerSignatureTest, "CrowdySDK.CrowdyExecEditor.SignatureNamesInputsNotStructs", CrowdyServerObjectEditorTextTest::TestFlags)
bool FCrowdyServerSignatureTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectEditorTextTest;
	const FCrowdyServerFunction* AddMember = FindMemberFunction(TEXT("AddMember"));
	const FCrowdyServerFunction* SetOpen = FindMemberFunction(TEXT("SetOpenForJoining"));
	const FCrowdyServerFunction* Join = FindMemberFunction(TEXT("Join"));
	if (!TestNotNull(TEXT("Add Member is built in"), AddMember) || !TestNotNull(TEXT("Set Open for Joining is built in"), SetOpen) || !TestNotNull(TEXT("Join is built in"), Join))
	{
		return false;
	}
	TestEqualSensitive(TEXT("a built-in lists its input as the Call node's pin names it"), CrowdyServerObjectText::Signature(*AddMember, true).ToString(), FString(TEXT("(Player)")));
	TestEqualSensitive(TEXT("a Boolean input loses its b, as its pin shows it"), CrowdyServerObjectText::Signature(*SetOpen, true).ToString(), FString(TEXT("(Open)")));
	TestEqualSensitive(TEXT("no inputs reads as ()"), CrowdyServerObjectText::Signature(*Join, true).ToString(), FString(TEXT("()")));
	TestTrue(TEXT("without field names the struct is named"), CrowdyServerObjectText::Signature(*AddMember, false).ToString().Contains(TEXT("FCrowdyServerMemberInputs")));

	FCrowdyServerFunction Tip;
	Tip.Name = TEXT("Tip");
	Tip.ParamsForm = ECrowdyServerValuesForm::List;
	Tip.ReplyForm = ECrowdyServerValuesForm::List;
	Tip.ParamsList.AddProperty(TEXT("Amount"), EPropertyBagPropertyType::Int32);
	Tip.ReplyList.AddProperty(TEXT("Total"), EPropertyBagPropertyType::Int32);
	TestEqualSensitive(TEXT("outputs follow returns"), CrowdyServerObjectText::Signature(Tip, false).ToString(), FString(TEXT("(Amount) returns Total")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerTypeSettingsRowTest, "CrowdySDK.CrowdyExecEditor.TypeSettingsRowStandsForNoSelection", CrowdyServerObjectEditorTextTest::TestFlags)
bool FCrowdyServerTypeSettingsRowTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectEditorTextTest;
	using EType = SCrowdyServerObjectMembers::FItem::EType;
	using EKind = FCrowdyServerObjectSelection::EKind;
	const SCrowdyServerObjectMembers::FItemPtr TypeSettings = MakeItem(EType::TypeSettings);
	const SCrowdyServerObjectMembers::FItemPtr Variables = MakeItem(EType::Variables);
	const SCrowdyServerObjectMembers::FItemPtr Oil = MakeItem(EType::Variable, TEXT("Oil"));
	const SCrowdyServerObjectMembers::FItemPtr Functions = MakeItem(EType::Functions);
	const SCrowdyServerObjectMembers::FItemPtr Feed = MakeItem(EType::Function, TEXT("Feed"), 0);
	Variables->Children.Add(Oil);
	Functions->Children.Add(Feed);
	const TArray<SCrowdyServerObjectMembers::FItemPtr> Roots = {TypeSettings, Variables, Functions};

	FCrowdyServerObjectSelection Selection;
	TestTrue(TEXT("nothing selected shows the Type Settings row"), SCrowdyServerObjectMembers::FindRow(Roots, Selection) == TypeSettings);

	SCrowdyServerObjectMembers::SelectItem(Oil, Selection);
	TestTrue(TEXT("a variable row selects its variable"), Selection.Kind == EKind::Variable && Selection.Variable == FName(TEXT("Oil")));
	TestTrue(TEXT("the variable's row shows it"), SCrowdyServerObjectMembers::FindRow(Roots, Selection) == Oil);
	SCrowdyServerObjectMembers::SelectItem(TypeSettings, Selection);
	TestTrue(TEXT("the Type Settings row selects nothing"), Selection.Kind == EKind::None);

	SCrowdyServerObjectMembers::SelectItem(Feed, Selection);
	TestTrue(TEXT("a function row selects its function"), Selection.Kind == EKind::Function && Selection.Function == 0);
	SCrowdyServerObjectMembers::SelectItem(Variables, Selection);
	TestTrue(TEXT("a section heading selects nothing"), Selection.Kind == EKind::None);
	SCrowdyServerObjectMembers::SelectItem(Feed, Selection);
	SCrowdyServerObjectMembers::SelectItem(nullptr, Selection);
	TestTrue(TEXT("empty space selects nothing"), Selection.Kind == EKind::None);

	Selection.SelectVariable(TEXT("Gone"));
	TestTrue(TEXT("a variable with no row has none to show"), !SCrowdyServerObjectMembers::FindRow(Roots, Selection).IsValid());
	return true;
}

#endif
