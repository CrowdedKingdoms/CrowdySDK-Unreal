#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyExecCodegenMenu.h"
#include "CrowdyExecEditorTestTypes.h"
#include "CrowdyServerCodeFiles.h"
#include "CrowdyServerComputeService.h"
#include "CrowdyServerObjectDefinition.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace CrowdyExecCodegenMenuTests
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	const TCHAR* const BossPath = TEXT("/Game/CrowdyTests/DA_Boss.DA_Boss");
	const TCHAR* const OtherPath = TEXT("/Game/CrowdyTests/DA_Other.DA_Other");
	const TCHAR* const CopyPath = TEXT("/Game/CrowdyTests/DA_Boss1.DA_Boss1");

	struct FServerFolder
	{
		FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("CrowdyExecCodegenMenuTests"),
			FGuid::NewGuid().ToString(), TEXT("Server")));

		~FServerFolder()
		{
			IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Path), false, true);
		}

		/** A crate folder with a logic.rs and a Cargo.toml naming Owner, or naming no definition when Owner is empty. */
		FString MakeCrate(const TCHAR* Name, const FString& Logic, const FString& Owner) const
		{
			const FString Crate = FPaths::Combine(Path, Name);
			FString Cargo = FString::Printf(TEXT("[package]\nname = \"%s\"\nversion = \"0.1.0\"\nedition = \"2021\"\n"), Name);
			if (!Owner.IsEmpty())
			{
				Cargo += FString::Printf(TEXT("\n[package.metadata.crowdy]\ndefinition = \"%s\"\n"), *Owner);
			}
			FFileHelper::SaveStringToFile(Cargo, *FPaths::Combine(Crate, TEXT("Cargo.toml")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			FFileHelper::SaveStringToFile(Logic, *FPaths::Combine(Crate, TEXT("src/logic.rs")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			return Crate;
		}

		FString Find(const TCHAR* DefinitionPath, const TCHAR* TypeName) const
		{
			return CrowdyExecCodegenMenu::FindRenamedCrateDirectory(DefinitionPath, TypeName, Path);
		}
	};

	FString LoadLogic(const FString& Crate)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *FPaths::Combine(Crate, TEXT("src/logic.rs")));
		return Text;
	}

	// A State List, a params List, and a reply List copied from the State, so it holds the same values under the same ids.
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeTipJar()
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Definition->TypeName = TEXT("tip_jar");
		Definition->StateForm = ECrowdyServerValuesForm::List;
		Definition->StateList.AddProperty(TEXT("Total"), EPropertyBagPropertyType::Int32);
		FCrowdyServerFunction& Tip = Definition->Functions.AddDefaulted_GetRef();
		Tip.Name = TEXT("Tip");
		Tip.ParamsForm = ECrowdyServerValuesForm::List;
		Tip.ParamsList.AddProperty(TEXT("Amount"), EPropertyBagPropertyType::Int32);
		Tip.ReplyForm = ECrowdyServerValuesForm::List;
		Tip.ReplyList = Definition->StateList;
		return Definition;
	}

	FGuid IdOf(const FInstancedPropertyBag& List, FName Value)
	{
		const FPropertyBagPropertyDesc* Desc = List.FindPropertyDescByName(Value);
		return Desc ? Desc->ID : FGuid();
	}

	CrowdyExecCodegen::FListValueRename MakeRename(const TCHAR* Struct, const TCHAR* OldName, const TCHAR* NewName, const FGuid& ValueId)
	{
		CrowdyExecCodegen::FListValueRename Rename;
		Rename.Struct = Struct;
		Rename.OldName = OldName;
		Rename.NewName = NewName;
		Rename.ValueId = ValueId;
		return Rename;
	}

	FString GenerateTypes(FAutomationTestBase& Test, const UCrowdyServerObjectDefinition& Definition)
	{
		CrowdyExecCodegen::FGeneratedCrate Crate;
		FString Error;
		const bool bGenerated = CrowdyExecCodegen::Generate(Definition, Crate, Error);
		Test.TestTrue(FString::Printf(TEXT("the crate is generated (%s)"), *Error), bGenerated);
		const CrowdyExecCodegen::FGeneratedFile* Types = Crate.Files.FindByPredicate([](const CrowdyExecCodegen::FGeneratedFile& File) { return File.Path == TEXT("src/types.rs"); });
		return Types ? Types->Text : FString();
	}

	/** A Cargo.toml recording Escaped, already TOML-escaped, as its definition, each line ending in Break. */
	FString TaggedCargo(const FString& Escaped, const TCHAR* Break)
	{
		const FString Text = FString::Printf(TEXT("[package]\nname = \"boss\"\nversion = \"0.1.0\"\n\n[package.metadata.crowdy]\ndefinition = \"%s\"\n\n[lib]\ncrate-type = [\"cdylib\"]\n"), *Escaped);
		return Text.Replace(TEXT("\n"), Break, ESearchCase::CaseSensitive);
	}

	FString WriteCargo(const FServerFolder& Server, const TCHAR* Name, const FString& Text)
	{
		const FString Crate = FPaths::Combine(Server.Path, Name);
		FFileHelper::SaveStringToFile(Text, *FPaths::Combine(Crate, TEXT("Cargo.toml")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		return Crate;
	}

	FString LoadCargo(const FString& Crate)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *FPaths::Combine(Crate, TEXT("Cargo.toml")));
		return Text;
	}

	/** Records Generate's dialogs and answers them in order from Answers; past the last, Ok when it is the only button, else Cancel. */
	struct FScriptedDialogs
	{
		TArray<EAppReturnType::Type> Answers;
		/** Runs once, while the first dialog is open, as the user might act meanwhile. */
		TFunction<void()> WhileAsking;
		TArray<EAppMsgCategory> Categories;
		TArray<EAppMsgType::Type> Types;
		TArray<EAppReturnType::Type> Defaults;
		TArray<FString> Messages;
		bool bActed = false;
		CrowdyExecCodegenMenu::FScopedDialogAnswers Scope{[this](EAppMsgCategory Category, EAppMsgType::Type Type, EAppReturnType::Type Default, const FText& Message)
		{
			return Answer(Category, Type, Default, Message);
		}};

		EAppReturnType::Type Answer(EAppMsgCategory Category, EAppMsgType::Type Type, EAppReturnType::Type Default, const FText& Message)
		{
			const int32 Index = Messages.Num();
			Categories.Add(Category);
			Types.Add(Type);
			Defaults.Add(Default);
			Messages.Add(Message.ToString());
			if (WhileAsking && !bActed)
			{
				bActed = true;
				WhileAsking();
			}
			if (Answers.IsValidIndex(Index))
			{
				return Answers[Index];
			}
			return Type == EAppMsgType::Ok ? EAppReturnType::Ok : EAppReturnType::Cancel;
		}
	};

	/** The crate folder Generate writes for TypeName under Server, spelled as Generate spells it. */
	FString CrateOf(const FServerFolder& Server, const TCHAR* TypeName)
	{
		FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(Server.Path, TypeName));
		FPaths::NormalizeDirectoryName(Directory);
		return Directory;
	}

	void AddFunction(UCrowdyServerObjectDefinition& Definition, const TCHAR* Name)
	{
		Definition.Functions.AddDefaulted_GetRef().Name = Name;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenMenuRenamedFolderTest, "CrowdySDK.CrowdyExecEditor.CodegenMenuFindsRenamedFolder", CrowdyExecCodegenMenuTests::TestFlags)
bool FCrowdyExecCodegenMenuRenamedFolderTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecCodegenMenuTests;
	FServerFolder Server;
	Server.MakeCrate(TEXT("boss_old"), TEXT("// untagged\n"), FString());
	Server.MakeCrate(TEXT("boss_other"), TEXT("// other\n"), OtherPath);
	TestTrue(TEXT("a crate naming no definition, or another definition, is never offered"), Server.Find(BossPath, TEXT("boss_new")).IsEmpty());

	const FString Previous = Server.MakeCrate(TEXT("boss_prev"), TEXT("// boss\n"), BossPath);
	TestEqual(TEXT("the crate naming this definition under another folder is the renamed type's"), Server.Find(BossPath, TEXT("boss_new")), Previous);
	TestTrue(TEXT("a duplicated asset finds nothing to move"), Server.Find(CopyPath, TEXT("boss_new")).IsEmpty());
	TestTrue(TEXT("a duplicated asset keeping the original's Type Name finds nothing either"), Server.Find(CopyPath, TEXT("boss_prev")).IsEmpty());

	Server.MakeCrate(TEXT("boss_new"), TEXT("// new\n"), FString());
	TestTrue(TEXT("a current folder naming no definition yet is taken as this one's"), Server.Find(BossPath, TEXT("boss_new")).IsEmpty());

	Server.MakeCrate(TEXT("boss_new"), TEXT("// new\n"), OtherPath);
	TestEqual(TEXT("a current folder naming another definition leaves the renamed one to offer"), Server.Find(BossPath, TEXT("boss_new")), Previous);

	Server.MakeCrate(TEXT("boss_new"), TEXT("// new\n"), BossPath);
	TestTrue(TEXT("a current folder naming this definition needs no move"), Server.Find(BossPath, TEXT("boss_new")).IsEmpty());
	TestEqual(TEXT("finding moves nothing"), LoadLogic(Previous), FString(TEXT("// boss\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenMenuRetagTest, "CrowdySDK.CrowdyExecEditor.CodegenRetagsCratesOfMovedDefinition", CrowdyExecCodegenMenuTests::TestFlags)
bool FCrowdyExecCodegenMenuRetagTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecCodegenMenuTests;
	FServerFolder Server;
	// The old path holds quotes and the new one a backslash, which the record escapes; the crates spell the old path in another case.
	const FString OldPath = TEXT("/Game/Bosses/DA_\"Boss\".DA_\"Boss\"");
	const FString NewPath = TEXT("/Game/Moved\\Bosses/DA_Boss.DA_Boss");
	const FString OldEscaped = TEXT("/game/bosses/DA_\\\"Boss\\\".DA_\\\"Boss\\\"");
	const FString NewEscaped = TEXT("/Game/Moved\\\\Bosses/DA_Boss.DA_Boss");
	const FString OtherCargo = TaggedCargo(OtherPath, TEXT("\n"));
	const FString Crlf = WriteCargo(Server, TEXT("boss_crlf"), TaggedCargo(OldEscaped, TEXT("\r\n")));
	const FString Lf = WriteCargo(Server, TEXT("boss_lf"), TaggedCargo(OldEscaped, TEXT("\n")));
	const FString Other = WriteCargo(Server, TEXT("other"), OtherCargo);
	const FString Untagged = Server.MakeCrate(TEXT("untagged"), TEXT("// untagged\n"), FString());
	const FString UntaggedCargo = LoadCargo(Untagged);

	TestEqual(TEXT("every crate recording the old path, in any case, is retagged"), CrowdyExecCodegen::RetagCrates(Server.Path, OldPath, NewPath), 2);
	TestEqualSensitive(TEXT("only the path changes, CRLF line breaks kept"), LoadCargo(Crlf), TaggedCargo(NewEscaped, TEXT("\r\n")));
	TestEqualSensitive(TEXT("and LF ones"), LoadCargo(Lf), TaggedCargo(NewEscaped, TEXT("\n")));
	TestEqualSensitive(TEXT("the new path reads back"), CrowdyExecCodegen::ReadCrateDefinition(Crlf), NewPath);
	TestEqualSensitive(TEXT("a crate recording another definition is untouched"), LoadCargo(Other), OtherCargo);
	TestEqualSensitive(TEXT("a crate recording no definition is untouched"), LoadCargo(Untagged), UntaggedCargo);
	TestEqual(TEXT("a Type Name change then finds the crate by the new path"), Server.Find(*NewPath, TEXT("boss_new")), Crlf);
	TestTrue(TEXT("and no longer by the old one"), Server.Find(*OldPath, TEXT("boss_new")).IsEmpty());
	TestEqual(TEXT("retagging again finds nothing to retag"), CrowdyExecCodegen::RetagCrates(Server.Path, OldPath, NewPath), 0);
	TestEqual(TEXT("an empty old path retags nothing"), CrowdyExecCodegen::RetagCrates(Server.Path, FString(), NewPath), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenMenuMoveFolderTest, "CrowdySDK.CrowdyExecEditor.CodegenMenuMovesRenamedFolder", CrowdyExecCodegenMenuTests::TestFlags)
bool FCrowdyExecCodegenMenuMoveFolderTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecCodegenMenuTests;
	FServerFolder Server;
	const FString Logic = TEXT("// the developer's server code\n");
	const FString Old = Server.MakeCrate(TEXT("boss_old"), Logic, BossPath);
	const FString New = FPaths::Combine(Server.Path, TEXT("boss_new"));
	FString Error;
	const bool bMoved = CrowdyExecCodegenMenu::MoveCrateDirectory(Old, New, Error);
	if (!TestTrue(FString::Printf(TEXT("the folder moves (%s)"), *Error), bMoved))
	{
		return false;
	}
	TestEqual(TEXT("logic.rs arrives intact"), LoadLogic(New), Logic);
	TestFalse(TEXT("the old folder is gone"), IFileManager::Get().DirectoryExists(*Old));

	const FString Taken = Server.MakeCrate(TEXT("boss_taken"), TEXT("// taken\n"), OtherPath);
	Error.Reset();
	TestFalse(TEXT("a move onto an existing folder is refused"), CrowdyExecCodegenMenu::MoveCrateDirectory(New, Taken, Error));
	TestTrue(FString::Printf(TEXT("because the target exists (%s)"), *Error), Error.Contains(TEXT("already exists")));
	TestEqual(TEXT("the refused folder keeps its logic.rs"), LoadLogic(New), Logic);
	TestEqual(TEXT("the existing folder keeps its own"), LoadLogic(Taken), FString(TEXT("// taken\n")));

	const FString Root = FPaths::GetPath(Server.Path);
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	TestFalse(TEXT("the test leaves no folder behind"), IFileManager::Get().DirectoryExists(*Root));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenMenuRenamedListTest, "CrowdySDK.CrowdyExecEditor.CodegenMenuFindsRenamedValueList", CrowdyExecCodegenMenuTests::TestFlags)
bool FCrowdyExecCodegenMenuRenamedListTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecCodegenMenuTests;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeTipJar();
	const FGuid Total = IdOf(Definition->StateList, TEXT("Total"));
	const FGuid Amount = IdOf(Definition->Functions[0].ParamsList, TEXT("Amount"));
	TestTrue(TEXT("the reply List holds the State's value under the same id"), Total.IsValid() && IdOf(Definition->Functions[0].ReplyList, TEXT("Total")) == Total);

	TestEqual(TEXT("a value of the State List and of a List shaped like it resolves to the State List, which names the shared struct"),
		CrowdyExecCodegenMenu::FindRenamedList(*Definition, MakeRename(TEXT("TipJarState"), TEXT("Gold"), TEXT("Total"), Total)), FString(TEXT("TipJarState")));
	TestEqual(TEXT("a value of a function's List resolves to that List"),
		CrowdyExecCodegenMenu::FindRenamedList(*Definition, MakeRename(TEXT("TipParams"), TEXT("Coins"), TEXT("Amount"), Amount)), FString(TEXT("TipParams")));
	TestTrue(TEXT("an id no List holds resolves to nothing"),
		CrowdyExecCodegenMenu::FindRenamedList(*Definition, MakeRename(TEXT("TipJarState"), TEXT("Gold"), TEXT("Total"), FGuid::NewGuid())).IsEmpty());
	TestTrue(TEXT("an id held under another name resolves to nothing"),
		CrowdyExecCodegenMenu::FindRenamedList(*Definition, MakeRename(TEXT("TipJarState"), TEXT("Gold"), TEXT("Coins"), Total)).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenMenuOwnDefaultsListTest, "CrowdySDK.CrowdyExecEditor.CodegenMenuFindsListStartingWithOtherValues", CrowdyExecCodegenMenuTests::TestFlags)
bool FCrowdyExecCodegenMenuOwnDefaultsListTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecCodegenMenuTests;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeTipJar();
	Definition->Functions[0].ReplyList.SetValueInt32(TEXT("Total"), 7);
	TArray<FString> Errors;
	const bool bBaked = Definition->Bake(Errors);
	if (!TestTrue(FString::Printf(TEXT("the definition bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked))
	{
		return false;
	}
	TestTrue(TEXT("the reply List starting with other values is a struct of its own"), GenerateTypes(*this, *Definition).Contains(TEXT("pub struct TipReply {"), ESearchCase::CaseSensitive));
	const FGuid Total = IdOf(Definition->StateList, TEXT("Total"));
	TestEqual(TEXT("its renamed value still resolves to the State List, whose names the shared struct carries"),
		CrowdyExecCodegenMenu::FindRenamedList(*Definition, MakeRename(TEXT("TipReply"), TEXT("Gold"), TEXT("Total"), Total)), FString(TEXT("TipJarState")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenMenuKeepOldNamesTest, "CrowdySDK.CrowdyExecEditor.CodegenMenuKeepsOldValueNames", CrowdyExecCodegenMenuTests::TestFlags)
bool FCrowdyExecCodegenMenuKeepOldNamesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecCodegenMenuTests;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeTipJar();
	TArray<FString> Errors;
	const bool bBaked = Definition->Bake(Errors);
	if (!TestTrue(FString::Printf(TEXT("the definition bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked))
	{
		return false;
	}
	TestFalse(TEXT("the server code has no Gold before"), GenerateTypes(*this, *Definition).Contains(TEXT("pub Gold:"), ESearchCase::CaseSensitive));

	const FGuid Total = IdOf(Definition->StateList, TEXT("Total"));
	const TArray<CrowdyExecCodegen::FListValueRename> Renames = {MakeRename(TEXT("TipJarState"), TEXT("Gold"), TEXT("Total"), Total),
		MakeRename(TEXT("TipJarState"), TEXT("Silver"), TEXT("Gone"), FGuid::NewGuid())};
	const TArray<CrowdyExecCodegen::FListValueRename> NotFound = CrowdyExecCodegenMenu::KeepOldListValueNames(*Definition, Renames);
	TestTrue(TEXT("only the rename with no List is reported"), NotFound.Num() == 1 && NotFound[0].OldName == TEXT("Silver"));
	if (!TestEqual(TEXT("one List Value Names entry is added"), Definition->ListValueNames.Num(), 1))
	{
		return false;
	}
	const FCrowdyServerListValueName& Entry = Definition->ListValueNames[0];
	TestTrue(TEXT("under the List that names the struct"), Entry.List == FName(TEXT("TipJarState")));
	TestTrue(TEXT("for the renamed value"), Entry.ValueId == Total);
	TestEqual(TEXT("keeping its old name"), Entry.ServerName, FString(TEXT("Gold")));
	TestTrue(TEXT("the server code generated again keeps the old name"), GenerateTypes(*this, *Definition).Contains(TEXT("pub Gold:"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenMenuLogicGapsTextTest, "CrowdySDK.CrowdyExecEditor.CodegenMenuDescribesLogicGaps", CrowdyExecCodegenMenuTests::TestFlags)
bool FCrowdyExecCodegenMenuLogicGapsTextTest::RunTest(const FString& Parameters)
{
	CrowdyExecCodegen::FLogicGaps Gaps;
	Gaps.Missing = {{TEXT("test_function"), TEXT("    fn test_function() {}\n")}, {TEXT("on_player_joined"), TEXT("    fn on_player_joined() {}\n")}};
	Gaps.InsertAt = 10;
	const auto Describe = [&Gaps]() { return CrowdyExecCodegenMenu::DescribeLogicGaps(Gaps, TEXT("logic.rs"), TEXT("ExampleObjectState")).ToString(); };
	TestEqualSensitive(TEXT("missing functions are named"), Describe(), FString(TEXT("logic.rs is missing test_function, on_player_joined, which this type now has.")));
	Gaps.Leftover = {TEXT("join")};
	TestEqualSensitive(TEXT("then those the type no longer has"), Describe(),
		FString(TEXT("logic.rs is missing test_function, on_player_joined, which this type now has. logic.rs defines join, which this type no longer has: rename or delete it.")));
	Gaps.InsertAt = INDEX_NONE;
	Gaps.Leftover.Reset();
	TestEqualSensitive(TEXT("with no block it says where they belong"), Describe(),
		FString(TEXT("logic.rs is missing test_function, on_player_joined, and has no impl Functions for ExampleObjectState block to add them to.")));
	Gaps.Missing.Reset();
	Gaps.Leftover = {TEXT("join")};
	TestEqualSensitive(TEXT("left over alone"), Describe(), FString(TEXT("logic.rs defines join, which this type no longer has: rename or delete it.")));
	Gaps.Leftover.Reset();
	Gaps.Missing = {{TEXT("r#match"), TEXT("    fn r#match() {}\n")}};
	Gaps.InsertAt = 10;
	TestEqualSensitive(TEXT("a raw identifier reads as its name"), Describe(), FString(TEXT("logic.rs is missing match, which this type now has.")));
	TestEqualSensitive(TEXT("the stubs join one blank line apart"), CrowdyExecCodegenMenu::JoinStubs({{{TEXT("a"), TEXT("    fn a() {}\n")}, {TEXT("b"), TEXT("    fn b() {}\n")}}, {}, 0}),
		FString(TEXT("    fn a() {}\n\n    fn b() {}\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenMenuStubOfferTest, "CrowdySDK.CrowdyExecEditor.CodegenMenuAddsLogicStubsOnlyOnYes", CrowdyExecCodegenMenuTests::TestFlags)
bool FCrowdyExecCodegenMenuStubOfferTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecCodegenMenuTests;
	FServerFolder Server;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
	Definition->TypeName = TEXT("ck_menu_stubs");
	Definition->StateForm = ECrowdyServerValuesForm::List;
	Definition->StateList.AddProperty(TEXT("Gold"), EPropertyBagPropertyType::Int32);
	AddFunction(*Definition, TEXT("Store"));
	const FString Crate = CrateOf(Server, TEXT("ck_menu_stubs"));
	const FString LogicFile = CrowdyExecCodegen::GetLogicFile(*Definition, Crate);
	{
		FScriptedDialogs Dialogs;
		if (!TestTrue(TEXT("the first Generate writes the crate"), CrowdyExecCodegenMenu::GenerateServerCode(*Definition, Server.Path)))
		{
			return false;
		}
		TestEqual(TEXT("and asks nothing"), Dialogs.Messages.Num(), 0);
	}
	FString Original;
	if (!TestTrue(TEXT("the generated logic.rs is written"), FFileHelper::LoadFileToString(Original, *LogicFile)))
	{
		return false;
	}
	AddFunction(*Definition, TEXT("Clear"));
	const FString Missing = TEXT("logic.rs is missing clear, which this type now has.");

	{
		FScriptedDialogs Dialogs;
		Dialogs.Answers = {EAppReturnType::No};
		TestTrue(TEXT("a function the logic lacks does not stop Generate"), CrowdyExecCodegenMenu::GenerateServerCode(*Definition, Server.Path));
		const bool bAsked = Dialogs.Messages.Num() == 1 && Dialogs.Types[0] == EAppMsgType::YesNo && Dialogs.Messages[0].StartsWith(Missing, ESearchCase::CaseSensitive);
		TestTrue(FString::Printf(TEXT("Generate asks once whether to add it (%s)"), *FString::Join(Dialogs.Messages, TEXT(" | "))), bAsked);
		TestTrue(TEXT("as a warning whose default is No"), bAsked && Dialogs.Categories[0] == EAppMsgCategory::Warning && Dialogs.Defaults[0] == EAppReturnType::No);
		TestEqualSensitive(TEXT("No adds nothing"), LoadLogic(Crate), Original);
	}
	{
		FScriptedDialogs Dialogs;
		Dialogs.Answers = {EAppReturnType::Yes};
		TestTrue(TEXT("Generate runs again"), CrowdyExecCodegenMenu::GenerateServerCode(*Definition, Server.Path));
		TestEqual(TEXT("it asks once"), Dialogs.Messages.Num(), 1);
		const FString Logic = LoadLogic(Crate);
		TestTrue(TEXT("Yes adds the missing function"), Logic.Contains(TEXT("fn clear("), ESearchCase::CaseSensitive));
		TestTrue(TEXT("and keeps the one already there"), Logic.Contains(TEXT("fn store("), ESearchCase::CaseSensitive));
	}

	FFileHelper::SaveStringToFile(Original, *LogicFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const FString Emptied = TEXT("// emptied while Generate was asking\n");
	{
		FScriptedDialogs Dialogs;
		Dialogs.Answers = {EAppReturnType::Yes};
		Dialogs.WhileAsking = [&LogicFile, &Emptied]()
		{
			FFileHelper::SaveStringToFile(Emptied, *LogicFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		};
		TestTrue(TEXT("Generate runs a third time"), CrowdyExecCodegenMenu::GenerateServerCode(*Definition, Server.Path));
		if (!TestEqual(TEXT("it asks, then reports"), Dialogs.Messages.Num(), 2))
		{
			return false;
		}
		TestTrue(TEXT("the question is the stub offer"), Dialogs.Messages[0].StartsWith(Missing, ESearchCase::CaseSensitive));
		TestTrue(TEXT("the report is a message"), Dialogs.Types[1] == EAppMsgType::Ok);
		TestEqualSensitive(TEXT("a logic file changed while asking is named and left alone"), Dialogs.Messages[1],
			FString::Printf(TEXT("%s changed while Generate was asking, so nothing was added. Generate again."), *LogicFile));
	}
	FString AfterChange;
	FFileHelper::LoadFileToString(AfterChange, *LogicFile);
	TestEqualSensitive(TEXT("nothing is added to the changed file"), AfterChange, Emptied);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenMenuRenameAnswersTest, "CrowdySDK.CrowdyExecEditor.CodegenMenuMovesRenamedFolderOnlyOnYes", CrowdyExecCodegenMenuTests::TestFlags)
bool FCrowdyExecCodegenMenuRenameAnswersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecCodegenMenuTests;
	FServerFolder Server;
	// A definition outside the transient package, so its crate records it and a Type Name change finds the old folder.
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/CrowdyExecCodegenMenuTests/%s/DA_Renamed"), *FGuid::NewGuid().ToString()));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(Package, TEXT("DA_Renamed"), RF_Transient));
	Definition->TypeName = TEXT("ck_menu_old");
	// A C++ state keeps its struct name across the rename, so the moved logic.rs still implements Functions on it.
	Definition->State = FCrowdyExecTestKinds::StaticStruct();
	FCrowdyServerFunction& Echo = Definition->Functions.AddDefaulted_GetRef();
	Echo.Name = TEXT("Echo");
	Echo.Params = FCrowdyExecTestKinds::StaticStruct();
	Echo.Reply = FCrowdyExecTestKinds::StaticStruct();
	const FString Old = CrateOf(Server, TEXT("ck_menu_old"));
	const FString New = CrateOf(Server, TEXT("ck_menu_new"));
	{
		FScriptedDialogs Dialogs;
		if (!TestTrue(TEXT("the code is generated under the old Type Name"), CrowdyExecCodegenMenu::GenerateServerCode(*Definition, Server.Path)))
		{
			return false;
		}
		TestEqual(TEXT("without a question"), Dialogs.Messages.Num(), 0);
	}
	const FString Logic = LoadLogic(Old) + TEXT("// the developer's server code\n");
	FFileHelper::SaveStringToFile(Logic, *FPaths::Combine(Old, TEXT("src/logic.rs")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	Definition->TypeName = TEXT("ck_menu_new");
	const TCHAR* const Changed = TEXT("The Type Name changed from ck_menu_old to ck_menu_new since the server code was last generated.");

	{
		FScriptedDialogs Dialogs;
		Dialogs.Answers = {EAppReturnType::Cancel};
		TestFalse(TEXT("Cancel stops Generate"), CrowdyExecCodegenMenu::GenerateServerCode(*Definition, Server.Path));
		const bool bAsked = Dialogs.Messages.Num() == 1 && Dialogs.Types[0] == EAppMsgType::YesNoCancel && Dialogs.Messages[0].StartsWith(Changed, ESearchCase::CaseSensitive);
		TestTrue(FString::Printf(TEXT("after asking once about the old folder (%s)"), *FString::Join(Dialogs.Messages, TEXT(" | "))), bAsked);
		TestTrue(TEXT("as a warning whose default is Cancel"), bAsked && Dialogs.Categories[0] == EAppMsgCategory::Warning && Dialogs.Defaults[0] == EAppReturnType::Cancel);
		TestEqualSensitive(TEXT("the old folder keeps the developer's code"), LoadLogic(Old), Logic);
		TestFalse(TEXT("no new folder is made"), IFileManager::Get().DirectoryExists(*New));
	}
	{
		FScriptedDialogs Dialogs;
		Dialogs.Answers = {EAppReturnType::No};
		TestTrue(TEXT("No generates"), CrowdyExecCodegenMenu::GenerateServerCode(*Definition, Server.Path));
		TestEqual(TEXT("after the one question"), Dialogs.Messages.Num(), 1);
		TestEqualSensitive(TEXT("leaving the old folder and the developer's code as they are"), LoadLogic(Old), Logic);
		TestEqualSensitive(TEXT("the old folder still names the definition"), CrowdyExecCodegen::ReadCrateDefinition(Old), Definition->GetPathName());
		const FString Fresh = LoadLogic(New);
		TestTrue(TEXT("into a new folder with a logic.rs of its own"), !Fresh.IsEmpty() && !Fresh.Contains(TEXT("// the developer's server code"), ESearchCase::CaseSensitive));
		TestEqualSensitive(TEXT("which names the definition too"), CrowdyExecCodegen::ReadCrateDefinition(New), Definition->GetPathName());
	}
	// Without the new folder, the next Generate asks about moving the old one again.
	IFileManager::Get().DeleteDirectory(*New, false, true);
	if (!TestFalse(TEXT("the new folder is removed again"), IFileManager::Get().DirectoryExists(*New)))
	{
		return false;
	}
	{
		FScriptedDialogs Dialogs;
		Dialogs.Answers = {EAppReturnType::Yes};
		TestTrue(TEXT("Yes generates"), CrowdyExecCodegenMenu::GenerateServerCode(*Definition, Server.Path));
		TestEqual(TEXT("after the one question"), Dialogs.Messages.Num(), 1);
		TestFalse(TEXT("the old folder is moved away"), IFileManager::Get().DirectoryExists(*Old));
		TestEqualSensitive(TEXT("to the new Type Name, with the developer's code"), LoadLogic(New), Logic);
		TestEqualSensitive(TEXT("and the moved crate still names the definition"), CrowdyExecCodegen::ReadCrateDefinition(New), Definition->GetPathName());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenMenuSharedTypeNameTest, "CrowdySDK.CrowdyExecEditor.CodegenMenuRefusesSharedTypeName", CrowdyExecCodegenMenuTests::TestFlags)
bool FCrowdyExecCodegenMenuSharedTypeNameTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecCodegenMenuTests;
	FCrowdyServerComputeService& Compute = FCrowdyServerComputeService::Get();
	if (!TestFalse(TEXT("no deploy runs, so the project's types are read again"), Compute.IsBusy()))
	{
		return false;
	}
	// Public objects in a package outside the transient one are in-memory assets, which the project's types include.
	const FString Root = FString::Printf(TEXT("/Temp/CrowdyExecCodegenMenuTests/%s"), *FGuid::NewGuid().ToString());
	const auto MakeAsset = [&Root](const TCHAR* Name)
	{
		UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s/%s"), *Root, Name));
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(Package, Name, RF_Public));
		Definition->TypeName = TEXT("ck_menu_shared");
		return Definition;
	};
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> First = MakeAsset(TEXT("DA_SharedA"));
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Second = MakeAsset(TEXT("DA_SharedB"));
	FScriptedDialogs Dialogs;
	TestTrue(TEXT("a Type Name another definition has is refused"), CrowdyExecCodegenMenu::RefuseSharedTypeName(*First));
	const FString Expected = FString::Printf(TEXT("%s %s uses it too."), *CrowdyServerCodeFiles::SharedTypeNameProblem().ToString(), *Second->GetPathName());
	if (TestEqual(TEXT("in one message"), Dialogs.Messages.Num(), 1))
	{
		TestTrue(TEXT("with only an Ok button"), Dialogs.Types[0] == EAppMsgType::Ok);
		TestTrue(TEXT("which is its default"), Dialogs.Defaults[0] == EAppReturnType::Ok);
		TestTrue(TEXT("as an error"), Dialogs.Categories[0] == EAppMsgCategory::Error);
		TestEqualSensitive(TEXT("naming the other definition"), Dialogs.Messages[0], Expected);
	}
	// A transient object is no asset, so the project's types no longer include it.
	Second->SetFlags(RF_Transient);
	TestFalse(TEXT("a Type Name no other definition has is not refused"), CrowdyExecCodegenMenu::RefuseSharedTypeName(*First));
	TestEqual(TEXT("and nothing more is said"), Dialogs.Messages.Num(), 1);
	First->SetFlags(RF_Transient);
	Compute.RefreshTypes();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenMenuIdentityTest, "CrowdySDK.CrowdyExecEditor.CodegenMenuSaysWhenAListChangesType", CrowdyExecCodegenMenuTests::TestFlags)
bool FCrowdyExecCodegenMenuIdentityTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecCodegenMenuTests;
	FServerFolder Server;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeTipJar();
	{
		FScriptedDialogs Dialogs;
		if (!TestTrue(TEXT("the reply sharing the State's struct is generated"), CrowdyExecCodegenMenu::GenerateServerCode(*Definition, Server.Path)))
		{
			return false;
		}
		TestEqual(TEXT("without a word"), Dialogs.Messages.Num(), 0);
	}
	Definition->Functions[0].ReplyList.SetValueInt32(TEXT("Total"), 7);
	{
		FScriptedDialogs Dialogs;
		TestTrue(TEXT("a reply starting with other values does not stop Generate"), CrowdyExecCodegenMenu::GenerateServerCode(*Definition, Server.Path));
		if (TestEqual(TEXT("which says so once"), Dialogs.Messages.Num(), 1))
		{
			TestTrue(TEXT("as a warning with only an Ok"), Dialogs.Categories[0] == EAppMsgCategory::Warning && Dialogs.Types[0] == EAppMsgType::Ok);
			TestEqualSensitive(TEXT("naming the reply and the struct it no longer is"), Dialogs.Messages[0],
				FString(TEXT("TipReply is now its own struct, since its starting values differ from those of TipJarState; code that uses one for the other no longer builds.")));
		}
	}
	Definition->Functions[0].ReplyList.SetValueInt32(TEXT("Total"), 0);
	{
		FScriptedDialogs Dialogs;
		TestTrue(TEXT("a reply starting with the State's values again generates"), CrowdyExecCodegenMenu::GenerateServerCode(*Definition, Server.Path));
		if (TestEqual(TEXT("and says so once"), Dialogs.Messages.Num(), 1))
		{
			TestEqualSensitive(TEXT("naming the struct it is again"), Dialogs.Messages[0],
				FString(TEXT("TipReply is now another name for TipJarState, since they now hold the same values with the same starting values; code that treats them as two types no longer builds.")));
		}
	}
	{
		FScriptedDialogs Dialogs;
		TestTrue(TEXT("generating with nothing changed"), CrowdyExecCodegenMenu::GenerateServerCode(*Definition, Server.Path));
		TestEqual(TEXT("says nothing"), Dialogs.Messages.Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenMenuPendingRetagsTest, "CrowdySDK.CrowdyExecEditor.CodegenRetagsOnlyWhenTheMoveIsSaved", CrowdyExecCodegenMenuTests::TestFlags)
bool FCrowdyExecCodegenMenuPendingRetagsTest::RunTest(const FString& Parameters)
{
	const FString A = TEXT("/Game/Bosses/DA_Boss.DA_Boss");
	const FString B = TEXT("/Game/Moved/DA_Boss.DA_Boss");
	const FString C = TEXT("/Game/Final/DA_Boss.DA_Boss");
	const auto Describe = [](const TArray<TPair<FString, FString>>& Moves)
	{
		TArray<FString> Lines;
		for (const TPair<FString, FString>& Move : Moves)
		{
			Lines.Add(Move.Key + TEXT(" -> ") + Move.Value);
		}
		return FString::Join(Lines, TEXT(" | "));
	};
	{
		CrowdyExecCodegenMenu::FPendingRetags Pending;
		Pending.Record(A, B);
		TestEqualSensitive(TEXT("saving another package retags nothing"), Describe(Pending.TakeForPackage(TEXT("/Game/Other/DA_Boss"))), FString());
		TestEqualSensitive(TEXT("nor does saving the package it left"), Describe(Pending.TakeForPackage(TEXT("/Game/Bosses/DA_Boss"))), FString());
		TestEqualSensitive(TEXT("saving the moved package gives the move"), Describe(Pending.TakeForPackage(TEXT("/Game/Moved/DA_Boss"))), A + TEXT(" -> ") + B);
		TestEqualSensitive(TEXT("once"), Describe(Pending.TakeForPackage(TEXT("/Game/Moved/DA_Boss"))), FString());
	}
	{
		CrowdyExecCodegenMenu::FPendingRetags Pending;
		Pending.Record(A, B);
		Pending.Record(B, C);
		TestEqualSensitive(TEXT("an unsaved move on is nothing to save at the middle path"), Describe(Pending.TakeForPackage(TEXT("/Game/Moved/DA_Boss"))), FString());
		TestEqualSensitive(TEXT("moves in a row retag from the first path to the last"), Describe(Pending.TakeForPackage(TEXT("/Game/Final/DA_Boss"))), A + TEXT(" -> ") + C);
	}
	{
		CrowdyExecCodegenMenu::FPendingRetags Pending;
		Pending.Record(A, B);
		Pending.Record(B, A);
		TestEqualSensitive(TEXT("a move back retags nothing"), Describe(Pending.TakeForPackage(TEXT("/Game/Bosses/DA_Boss"))), FString());
		TestEqualSensitive(TEXT("and leaves nothing behind"), Describe(Pending.TakeForPackage(TEXT("/Game/Moved/DA_Boss"))), FString());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenMenuReadOnlyRetagTest, "CrowdySDK.CrowdyExecEditor.CodegenRetagLeavesUnwritableCargoWhole", CrowdyExecCodegenMenuTests::TestFlags)
bool FCrowdyExecCodegenMenuReadOnlyRetagTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecCodegenMenuTests;
	FServerFolder Server;
	const FString OldPath = TEXT("/Game/Bosses/DA_Boss.DA_Boss");
	const FString Cargo = TaggedCargo(OldPath, TEXT("\n"));
	const FString Crate = WriteCargo(Server, TEXT("boss"), Cargo);
	const FString Path = FPaths::Combine(Crate, TEXT("Cargo.toml"));
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!TestTrue(TEXT("the Cargo.toml is made read-only"), PlatformFile.SetReadOnly(*Path, true)))
	{
		return false;
	}
	AddExpectedMessagePlain(FString::Printf(TEXT("Could not update %s"), *Path), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TestEqual(TEXT("a Cargo.toml that cannot be replaced is not counted"), CrowdyExecCodegen::RetagCrates(Server.Path, OldPath, TEXT("/Game/Moved/DA_Boss.DA_Boss")), 0);
	PlatformFile.SetReadOnly(*Path, false);
	TestEqualSensitive(TEXT("and stays as it was"), LoadCargo(Crate), Cargo);
	TestFalse(TEXT("with no temporary file left beside it"), PlatformFile.FileExists(*(Path + TEXT(".tmp"))));
	return true;
}

#endif
