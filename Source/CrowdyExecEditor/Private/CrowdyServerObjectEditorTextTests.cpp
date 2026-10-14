#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectDefinitionCustomization.h"
#include "CrowdyServerObjectSelection.h"
#include "Misc/AutomationTest.h"
#include "SCrowdyServerObjectMembers.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/StrongObjectPtr.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageSelectionTest, "CrowdySDK.CrowdyExecEditor.MessageRowSelectsItsMessage", CrowdyServerObjectEditorTextTest::TestFlags)
bool FCrowdyServerMessageSelectionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectEditorTextTest;
	using EType = SCrowdyServerObjectMembers::FItem::EType;
	using EKind = FCrowdyServerObjectSelection::EKind;
	const SCrowdyServerObjectMembers::FItemPtr Messages = MakeItem(EType::Messages);
	const SCrowdyServerObjectMembers::FItemPtr Started = MakeItem(EType::Message, TEXT("Started"), 0);
	const SCrowdyServerObjectMembers::FItemPtr Ended = MakeItem(EType::Message, TEXT("Ended"), 1);
	const SCrowdyServerObjectMembers::FItemPtr Note = MakeItem(EType::Note);
	const SCrowdyServerObjectMembers::FItemPtr Functions = MakeItem(EType::Functions);
	const SCrowdyServerObjectMembers::FItemPtr Feed = MakeItem(EType::Function, TEXT("Feed"), 1);
	Messages->Children = {Started, Ended, Note};
	Functions->Children.Add(Feed);
	const TArray<SCrowdyServerObjectMembers::FItemPtr> Roots = {MakeItem(EType::TypeSettings), Functions, Messages};

	FCrowdyServerObjectSelection Selection;
	SCrowdyServerObjectMembers::SelectItem(Ended, Selection);
	TestTrue(TEXT("a message row selects its message"), Selection.Kind == EKind::Message && Selection.Message == 1);
	TestTrue(TEXT("the message's own row shows it, not the function with the same index"), SCrowdyServerObjectMembers::FindRow(Roots, Selection) == Ended);

	SCrowdyServerObjectMembers::SelectItem(Feed, Selection);
	TestTrue(TEXT("a function row clears the message"), Selection.Kind == EKind::Function && Selection.Message == INDEX_NONE);
	TestTrue(TEXT("the function's own row shows it, not the message with the same index"), SCrowdyServerObjectMembers::FindRow(Roots, Selection) == Feed);

	SCrowdyServerObjectMembers::SelectItem(Messages, Selection);
	TestTrue(TEXT("the section heading selects nothing"), Selection.Kind == EKind::None);
	SCrowdyServerObjectMembers::SelectItem(Note, Selection);
	TestTrue(TEXT("the privacy note selects nothing"), Selection.Kind == EKind::None && SCrowdyServerObjectMembers::FindRow(Roots, Selection) != Note);

	Selection.SelectMessage(7);
	TestTrue(TEXT("a message with no row has none to show"), !SCrowdyServerObjectMembers::FindRow(Roots, Selection).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageSectionTest, "CrowdySDK.CrowdyExecEditor.MessagesSectionListsEachMessage", CrowdyServerObjectEditorTextTest::TestFlags)
bool FCrowdyServerMessageSectionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectEditorTextTest;
	using EType = SCrowdyServerObjectMembers::FItem::EType;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
	TArray<SCrowdyServerObjectMembers::FItemPtr> Roots = SCrowdyServerObjectMembers::BuildRoots(Definition.Get(), MakeItem(EType::TypeSettings));
	TestEqual(TEXT("a definition with no messages lists type settings, variables, functions and an empty messages section"), Roots.Num(), 4);
	if (Roots.Num() != 4)
	{
		return false;
	}
	TestTrue(TEXT("the messages section follows the functions"), Roots[2]->Type == EType::Functions && Roots[3]->Type == EType::Messages);
	TestEqual(TEXT("the section is named Server Messages"), Roots[3]->Label.ToString(), FString(TEXT("Server Messages")));
	TestEqual(TEXT("no messages, no rows"), Roots[3]->Children.Num(), 0);

	SCrowdyServerObjectMembers::AddMessageTo(*Definition);
	Definition->Messages[0].Name = TEXT("Won");
	const int32 Added = SCrowdyServerObjectMembers::AddMessageTo(*Definition);
	Definition->Messages[Added].PayloadList.AddProperty(TEXT("Winner"), EPropertyBagPropertyType::Int32);
	Roots = SCrowdyServerObjectMembers::BuildRoots(Definition.Get(), MakeItem(EType::TypeSettings));
	const SCrowdyServerObjectMembers::FItemPtr Section = Roots[3];
	if (!TestEqual(TEXT("a row per message"), Section->Children.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("a row is a message with its index and name"), Section->Children[0]->Type == EType::Message && Section->Children[0]->Index == 0 && Section->Children[0]->Name == FName(TEXT("Won")));
	TestEqual(TEXT("a message with no variables shows empty parentheses"), Section->Children[0]->Detail.ToString(), FString(TEXT("()")));
	TestEqual(TEXT("a message shows its payload as its node's pins"), Section->Children[1]->Detail.ToString(), FString(TEXT("(Winner)")));
	TestTrue(TEXT("a type every player may read has no privacy note"), Section->Children.FindByPredicate([](const SCrowdyServerObjectMembers::FItemPtr& Item) { return Item->Type == EType::Note; }) == nullptr);

	Definition->Messages[1].bPlayersCanListen = false;
	Roots = SCrowdyServerObjectMembers::BuildRoots(Definition.Get(), MakeItem(EType::TypeSettings));
	TestEqualSensitive(TEXT("a message only server code hears says so after its payload"), Roots[3]->Children[1]->Detail.ToString(), FString(TEXT("(Winner) (server only)")));
	TestEqualSensitive(TEXT("a message players can hear shows the payload alone"), Roots[3]->Children[0]->Detail.ToString(), FString(TEXT("()")));

	Definition->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	Roots =SCrowdyServerObjectMembers::BuildRoots(Definition.Get(), MakeItem(EType::TypeSettings));
	TestTrue(TEXT("built-in functions come last"), Roots.Num() == 5 && Roots[4]->Type == EType::BuiltIns);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageNamesTest, "CrowdySDK.CrowdyExecEditor.AddedAndCopiedMessagesGetUniqueNames", CrowdyServerObjectEditorTextTest::TestFlags)
bool FCrowdyServerMessageNamesTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
	TestEqual(TEXT("the first message"), SCrowdyServerObjectMembers::AddMessageTo(*Definition), 0);
	TestEqual(TEXT("the second message"), SCrowdyServerObjectMembers::AddMessageTo(*Definition), 1);
	TestEqual(TEXT("the third message"), SCrowdyServerObjectMembers::AddMessageTo(*Definition), 2);
	if (!TestEqual(TEXT("three messages"), Definition->Messages.Num(), 3))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the first default name"), Definition->Messages[0].Name.ToString(), FString(TEXT("NewMessage")));
	TestEqualSensitive(TEXT("a taken name gets a suffix"), Definition->Messages[1].Name.ToString(), FString(TEXT("NewMessage_1")));
	TestEqualSensitive(TEXT("the suffix counts on"), Definition->Messages[2].Name.ToString(), FString(TEXT("NewMessage_2")));
	TestTrue(TEXT("a new message takes its payload as variables, like a new function"), Definition->Messages[0].PayloadForm == ECrowdyServerValuesForm::List);

	Definition->Messages.RemoveAt(0);
	SCrowdyServerObjectMembers::AddMessageTo(*Definition);
	TestEqualSensitive(TEXT("a freed name is reused"), Definition->Messages.Last().Name.ToString(), FString(TEXT("NewMessage")));

	Definition->Messages[0].ServerName = TEXT("custom_topic");
	TestEqual(TEXT("a copy lands after its original"), SCrowdyServerObjectMembers::DuplicateMessageIn(*Definition, 0), 1);
	TestEqualSensitive(TEXT("a copy is named after its original"), Definition->Messages[1].Name.ToString(), FString(TEXT("NewMessage_1Copy")));
	TestTrue(TEXT("a copy does not claim its original's topic"), Definition->Messages[1].ServerName.IsEmpty() && Definition->Messages[0].ServerName == TEXT("custom_topic"));
	TestEqual(TEXT("a second copy"), SCrowdyServerObjectMembers::DuplicateMessageIn(*Definition, 0), 1);
	TestEqualSensitive(TEXT("a second copy takes the next free name"), Definition->Messages[1].Name.ToString(), FString(TEXT("NewMessage_1Copy_1")));

	const int32 Count = Definition->Messages.Num();
	TestEqual(TEXT("no message at the index, no copy"), SCrowdyServerObjectMembers::DuplicateMessageIn(*Definition, 99), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("and nothing is added"), Definition->Messages.Num(), Count);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessagePrivacyWarningTest, "CrowdySDK.CrowdyExecEditor.MessagePrivacyWarningFollowsWhoMayReadVariables", CrowdyServerObjectEditorTextTest::TestFlags)
bool FCrowdyServerMessagePrivacyWarningTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectEditorTextTest;
	using EType = SCrowdyServerObjectMembers::FItem::EType;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
	auto CountNotes = [&Definition]()
	{
		TArray<SCrowdyServerObjectMembers::FItemPtr> Roots = SCrowdyServerObjectMembers::BuildRoots(Definition.Get(), MakeItem(EType::TypeSettings));
		const SCrowdyServerObjectMembers::FItemPtr* Section = Roots.FindByPredicate([](const SCrowdyServerObjectMembers::FItemPtr& Root) { return Root->Type == EType::Messages; });
		return Section ? (*Section)->Children.FilterByPredicate([](const SCrowdyServerObjectMembers::FItemPtr& Item) { return Item->Type == EType::Note; }).Num() : -1;
	};

	Definition->Visibility = ECrowdyServerObjectVisibility::OwnerOnly;
	TestFalse(TEXT("restricted readers but no messages players can hear: nothing to warn about"),CrowdyServerObjectText::ShowsMessagePrivacyWarning(*Definition));
	TestEqual(TEXT("and the section has no note"), CountNotes(), 0);

	SCrowdyServerObjectMembers::AddMessageTo(*Definition);
	TestTrue(TEXT("Readable By Owner Only with a message warns"), CrowdyServerObjectText::ShowsMessagePrivacyWarning(*Definition));
	TestEqual(TEXT("the section lists one note"), CountNotes(), 1);
	TestEqual(TEXT("the note says it as agreed"), CrowdyServerObjectText::MessagePrivacyWarning().ToString(),
		FString(TEXT("Every player who can connect to this type hears its messages, whoever may read its variables. Keep private data out of messages.")));

	Definition->Visibility = ECrowdyServerObjectVisibility::Public;
	TestFalse(TEXT("every player may read: no warning"), CrowdyServerObjectText::ShowsMessagePrivacyWarning(*Definition));
	TestEqual(TEXT("and no note"), CountNotes(), 0);

	Definition->OwnerFields.Add(TEXT("Secret"));
	TestTrue(TEXT("an owner-only variable warns"), CrowdyServerObjectText::ShowsMessagePrivacyWarning(*Definition));
	Definition->OwnerFields.Reset();
	Definition->TeamFields.Add(TEXT("Plans"));
	TestTrue(TEXT("a team-only variable warns"), CrowdyServerObjectText::ShowsMessagePrivacyWarning(*Definition));

	Definition->Messages[0].bPlayersCanListen = false;
	TestFalse(TEXT("only server code hears the messages: no player can overhear, so no warning"), CrowdyServerObjectText::ShowsMessagePrivacyWarning(*Definition));
	TestEqual(TEXT("and no note"), CountNotes(), 0);
	SCrowdyServerObjectMembers::AddMessageTo(*Definition);
	TestTrue(TEXT("one message players can hear brings the warning back"), CrowdyServerObjectText::ShowsMessagePrivacyWarning(*Definition));
	return true;
}

#endif
