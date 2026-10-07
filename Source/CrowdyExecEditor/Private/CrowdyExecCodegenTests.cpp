#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyExecCodegen.h"
#include "CrowdyExecEditorTestTypes.h"
#include "CrowdyServerObjectDefinition.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/DataValidation.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include <limits>

namespace CrowdyCodegenTest
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	const TCHAR* const FixtureName = TEXT("ck_exec_fixture");
	const TCHAR* const CargoPath = TEXT("Cargo.toml");
	const TCHAR* const LibPath = TEXT("src/lib.rs");
	const TCHAR* const TypesPath = TEXT("src/types.rs");
	const TCHAR* const LogicPath = TEXT("src/logic.rs");

	struct FFunctionSpec
	{
		const TCHAR* Name;
		const TCHAR* Params;
		const TCHAR* Reply;
		ECrowdyServerFunctionCaller WhoCanCall;
	};

	constexpr ECrowdyServerFunctionCaller Players = ECrowdyServerFunctionCaller::Players;
	constexpr ECrowdyServerFunctionCaller ServerOnly = ECrowdyServerFunctionCaller::ServerOnly;

	const FFunctionSpec HitFunction{TEXT("Hit"), TEXT("CrowdyExecTestHit"), nullptr, Players};
	const FFunctionSpec HealFunction{TEXT("Heal"), nullptr, nullptr, ServerOnly};
	const FFunctionSpec EchoRawFunction{TEXT("EchoRaw"), TEXT("CrowdyExecTestRaw"), TEXT("CrowdyExecTestRaw"), Players};
	const FFunctionSpec DefaultFunction{TEXT("Default"), nullptr, nullptr, Players};
	const FFunctionSpec RandomDefaultFunction{TEXT("Roll"), TEXT("CrowdyExecTestRandomDefault"), nullptr, Players};

	TArray<FFunctionSpec> FixtureFunctions()
	{
		return {
			{TEXT("EchoScalars"), TEXT("CrowdyExecTestScalars"), TEXT("CrowdyExecTestScalars"), Players},
			{TEXT("EchoContainers"), TEXT("CrowdyExecTestContainers"), TEXT("CrowdyExecTestContainers"), Players},
			{TEXT("EchoEngine"), TEXT("CrowdyExecTestEngine"), TEXT("CrowdyExecTestEngine"), Players},
			{TEXT("EchoExtras"), TEXT("CrowdyExecTestExtras"), TEXT("CrowdyExecTestExtras"), Players},
			{TEXT("EchoKeys"), TEXT("CrowdyExecTestKeys"), TEXT("CrowdyExecTestKeys"), Players},
			EchoRawFunction,
			{TEXT("Defaults"), nullptr, TEXT("CrowdyExecTestScalars"), Players},
			HitFunction,
			HealFunction
		};
	}

	// The test structs are private to CrowdyExec, so they are found by path.
	UScriptStruct* FindTestStruct(FAutomationTestBase& Test, const TCHAR* Name, bool& bInOutFound)
	{
		if (!Name)
		{
			return nullptr;
		}
		UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, FString::Printf(TEXT("/Script/CrowdyExec.%s"), Name));
		if (!Struct)
		{
			Test.AddError(FString::Printf(TEXT("the test struct %s is not loaded"), Name));
			bInOutFound = false;
		}
		return Struct;
	}

	bool AddFunction(FAutomationTestBase& Test, UCrowdyServerObjectDefinition& Definition, const FFunctionSpec& Spec)
	{
		bool bFound = true;
		FCrowdyServerFunction& Function = Definition.Functions.AddDefaulted_GetRef();
		Function.Name = Spec.Name;
		Function.Params = FindTestStruct(Test, Spec.Params, bFound);
		Function.Reply = FindTestStruct(Test, Spec.Reply, bFound);
		Function.WhoCanCall = Spec.WhoCanCall;
		return bFound;
	}

	/** The fixture's type with the given functions, not yet baked. Null when a test struct is missing. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeDefinition(FAutomationTestBase& Test, const TArray<FFunctionSpec>& Functions)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Definition->TypeName = FixtureName;
		Definition->Visibility = ECrowdyServerObjectVisibility::Public;
		Definition->WatchedFields = {TEXT("Health"), TEXT("bDefeated"), TEXT("Phase")};
		bool bFound = true;
		Definition->State = FindTestStruct(Test, TEXT("CrowdyExecTestBossState"), bFound);
		for (const FFunctionSpec& Spec : Functions)
		{
			bFound &= AddFunction(Test, *Definition, Spec);
		}
		if (!bFound)
		{
			return TStrongObjectPtr<UCrowdyServerObjectDefinition>();
		}
		return Definition;
	}

	bool GenerateCrate(FAutomationTestBase& Test, UCrowdyServerObjectDefinition& Definition, CrowdyExecCodegen::FGeneratedCrate& OutCrate)
	{
		TArray<FString> Errors;
		if (!Definition.Bake(Errors))
		{
			Test.AddError(FString::Printf(TEXT("the test definition does not bake (%s)"), *FString::Join(Errors, TEXT("; "))));
			return false;
		}
		FString Error;
		if (!CrowdyExecCodegen::Generate(Definition, OutCrate, Error))
		{
			Test.AddError(FString::Printf(TEXT("the test definition does not generate (%s)"), *Error));
			return false;
		}
		return true;
	}

	const FString* FindFileText(const CrowdyExecCodegen::FGeneratedCrate& Crate, const TCHAR* Path)
	{
		const CrowdyExecCodegen::FGeneratedFile* File = Crate.Files.FindByPredicate([Path](const CrowdyExecCodegen::FGeneratedFile& Candidate)
		{
			return Candidate.Path.Equals(Path, ESearchCase::CaseSensitive);
		});
		return File ? &File->Text : nullptr;
	}

	FString GetFileText(FAutomationTestBase& Test, const CrowdyExecCodegen::FGeneratedCrate& Crate, const TCHAR* Path)
	{
		const FString* Text = FindFileText(Crate, Path);
		if (!Text)
		{
			Test.AddError(FString::Printf(TEXT("the crate has no %s"), Path));
			return FString();
		}
		return *Text;
	}

	bool HasText(const FString& Text, const TCHAR* Part)
	{
		return Text.Contains(Part, ESearchCase::CaseSensitive);
	}

	int32 CountText(const FString& Text, const TCHAR* Part)
	{
		int32 Count = 0;
		for (int32 Found = Text.Find(Part, ESearchCase::CaseSensitive); Found != INDEX_NONE; Found = Text.Find(Part, ESearchCase::CaseSensitive, ESearchDir::FromStart, Found + 1))
		{
			++Count;
		}
		return Count;
	}

	int32 FirstDifferentLine(const FString& Expected, const FString& Actual)
	{
		TArray<FString> ExpectedLines;
		TArray<FString> ActualLines;
		Expected.ParseIntoArray(ExpectedLines, TEXT("\n"), false);
		Actual.ParseIntoArray(ActualLines, TEXT("\n"), false);
		const int32 Common = FMath::Min(ExpectedLines.Num(), ActualLines.Num());
		for (int32 Index = 0; Index < Common; ++Index)
		{
			if (!ExpectedLines[Index].Equals(ActualLines[Index], ESearchCase::CaseSensitive))
			{
				return Index + 1;
			}
		}
		return Common + 1;
	}

	FString GetGoldenDirectory()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CrowdySDK"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source/CrowdyExecEditor/Tests/Golden"), FixtureName));
	}

	/** A missing golden file is recorded from this run, which then fails so the new file gets reviewed. */
	void CheckGolden(FAutomationTestBase& Test, const FString& Directory, const CrowdyExecCodegen::FGeneratedCrate& Crate, const TCHAR* Path)
	{
		const FString* Text = FindFileText(Crate, Path);
		if (!Text)
		{
			Test.AddError(FString::Printf(TEXT("the crate has no %s"), Path));
			return;
		}
		const FString GoldenPath = FPaths::Combine(Directory, Path);
		FString Golden;
		if (!FFileHelper::LoadFileToString(Golden, *GoldenPath))
		{
			FFileHelper::SaveStringToFile(*Text, *GoldenPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			Test.AddError(FString::Printf(TEXT("recorded %s; review it and run again"), *GoldenPath));
			return;
		}
		if (Golden.Equals(*Text, ESearchCase::CaseSensitive))
		{
			return;
		}
		Test.AddError(FString::Printf(TEXT("%s differs from %s at line %d%s"), Path, *GoldenPath, FirstDifferentLine(Golden, *Text),
			Golden.Contains(TEXT("\r")) ? TEXT(" (the golden file has CR line endings; the generator writes LF only)") : TEXT("")));
	}

	struct FTempFolder
	{
		FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("CrowdyExecCodegenTests"), FGuid::NewGuid().ToString()));

		~FTempFolder()
		{
			IFileManager::Get().DeleteDirectory(*Path, false, true);
		}
	};

	FString LoadText(const FString& Path)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *Path);
		return Text;
	}

	bool WasWritten(const TArray<FString>& Written, const TCHAR* Path)
	{
		return Written.ContainsByPredicate([Path](const FString& Candidate)
		{
			return Candidate.Replace(TEXT("\\"), TEXT("/")).EndsWith(Path, ESearchCase::CaseSensitive);
		});
	}

	struct FRustField
	{
		const TCHAR* Name;
		const TCHAR* Type;
	};

	const TCHAR* const RustTypesHeader = TEXT("// The Server Object type test's structs, generated by the Crowdy SDK from its definition. Regenerate it; do not edit.\n")
		TEXT("use serde::{Deserialize, Serialize};\nuse std::collections::{BTreeMap, BTreeSet};\n\n");

	FString RustEnum(const TCHAR* Name, const TArray<const TCHAR*>& Variants)
	{
		FString Text = FString::Printf(TEXT("#[derive(Serialize, Deserialize, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]\npub enum %s {\n"), Name);
		for (const TCHAR* Variant : Variants)
		{
			Text += FString::Printf(TEXT("    %s,\n"), Variant);
		}
		return Text + TEXT("}\n\n");
	}

	FString RustStruct(const TCHAR* Name, const TArray<FRustField>& Fields)
	{
		FString Text = FString::Printf(TEXT("#[derive(Serialize, Deserialize, Clone, PartialEq, Debug)]\n#[serde(default)]\npub struct %s {\n"), Name);
		for (const FRustField& Field : Fields)
		{
			Text += FString::Printf(TEXT("    pub %s: %s,\n"), Field.Name, Field.Type);
		}
		Text += FString::Printf(TEXT("}\n\nimpl Default for %s {\n    fn default() -> Self {\n        Self {\n"), Name);
		for (const FRustField& Field : Fields)
		{
			Text += FString::Printf(TEXT("            %s: Default::default(),\n"), Field.Name);
		}
		return Text + TEXT("        }\n    }\n}\n\n");
	}

	/** A List value's id, made from its name so every run writes the same types.rs. */
	FString ValueIdText(const TCHAR* Name)
	{
		return FGuid::NewDeterministicGuid(Name).ToString(EGuidFormats::Digits);
	}

	void AddListValue(FInstancedPropertyBag& List, FPropertyBagPropertyDesc Desc)
	{
		Desc.ID = FGuid::NewDeterministicGuid(Desc.Name.ToString());
		List.AddProperties({Desc});
	}

	/** A definition whose State is a List of Int32 variables, each starting at 0. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeListDefinition(const TCHAR* TypeName, const TArray<const TCHAR*>& StateFields)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Definition->TypeName = TypeName;
		Definition->StateForm = ECrowdyServerValuesForm::List;
		for (const TCHAR* Field : StateFields)
		{
			AddListValue(Definition->StateList, FPropertyBagPropertyDesc(Field, EPropertyBagPropertyType::Int32));
		}
		return Definition;
	}

	FCrowdyServerFunction& AddPlainFunction(UCrowdyServerObjectDefinition& Definition, const TCHAR* Name, ECrowdyServerFunctionCaller WhoCanCall)
	{
		FCrowdyServerFunction& Function = Definition.Functions.AddDefaulted_GetRef();
		Function.Name = Name;
		Function.WhoCanCall = WhoCanCall;
		return Function;
	}

	void AddTimer(UCrowdyServerObjectDefinition& Definition, const TCHAR* Name, ECrowdyServerTimerRepeat Repeat, float Seconds, bool bAutomatic)
	{
		FCrowdyServerTimer& Timer = Definition.Timers.AddDefaulted_GetRef();
		Timer.Name = Name;
		Timer.Repeat = Repeat;
		Timer.Seconds = Seconds;
		Timer.bStartAutomatically = bAutomatic;
	}

	bool BakeDefinition(FAutomationTestBase& Test, UCrowdyServerObjectDefinition& Definition)
	{
		TArray<FString> Errors;
		const bool bBaked = Definition.Bake(Errors);
		return Test.TestTrue(FString::Printf(TEXT("%s bakes (%s)"), *Definition.TypeName, *FString::Join(Errors, TEXT("; "))), bBaked);
	}

	/** ck_exec_bank: a type other types call, with a List input and output. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeBankDefinition()
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Bank = MakeListDefinition(TEXT("ck_exec_bank"), {TEXT("Total")});
		Bank->WatchedFields = {TEXT("Total")};
		FCrowdyServerFunction& Store = AddPlainFunction(*Bank, TEXT("Store"), Players);
		Store.ParamsForm = ECrowdyServerValuesForm::List;
		AddListValue(Store.ParamsList, FPropertyBagPropertyDesc(TEXT("Coins"), EPropertyBagPropertyType::Int32));
		Store.ReplyForm = ECrowdyServerValuesForm::List;
		AddListValue(Store.ReplyList, FPropertyBagPropertyDesc(TEXT("Balance"), EPropertyBagPropertyType::Int64));
		return Bank;
	}

	/** ck_exec_rules: members kept by the object, a ranged and cooled-down input, timers, both events and Can Call. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeRulesDefinition(UCrowdyServerObjectDefinition& Bank)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Rules = MakeListDefinition(TEXT("ck_exec_rules"), {TEXT("Gold"), TEXT("Secret")});
		Rules->WatchedFields = {TEXT("Gold")};
		Rules->MembersFrom = ECrowdyServerMembersSource::ThisObject;
		Rules->MaxMembers = 5;
		AddTimer(*Rules, TEXT("EndMatch"), ECrowdyServerTimerRepeat::After, 10.f, false);
		AddTimer(*Rules, TEXT("Tick"), ECrowdyServerTimerRepeat::Every, 5.f, true);
		Rules->bOnPlayerJoined = true;
		Rules->bOnPlayerLeft = true;
		FCrowdyServerFunction& Deposit = AddPlainFunction(*Rules, TEXT("Deposit"), ECrowdyServerFunctionCaller::Members);
		Deposit.ParamsForm = ECrowdyServerValuesForm::List;
		FPropertyBagPropertyDesc Amount(TEXT("Amount"), EPropertyBagPropertyType::Int32);
		Amount.MetaData.Add(FPropertyBagPropertyDescMetaData(TEXT("ClampMin"), TEXT("1")));
		Amount.MetaData.Add(FPropertyBagPropertyDescMetaData(TEXT("ClampMax"), TEXT("100")));
		AddListValue(Deposit.ParamsList, Amount);
		Deposit.ParamsList.SetValueInt32(TEXT("Amount"), 1);
		Deposit.CooldownSeconds = 2.f;
		AddPlainFunction(*Rules, TEXT("Start"), ECrowdyServerFunctionCaller::Leader);
		Rules->CanCall = {&Bank};
		return Rules;
	}

	/** ck_exec_team: a Crowdy Team's members, readable by members only, with On Player Left. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeTeamDefinition()
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Team = MakeListDefinition(TEXT("ck_exec_team"), {TEXT("Gold")});
		Team->WatchedFields = {TEXT("Gold")};
		Team->Visibility = ECrowdyServerObjectVisibility::Members;
		Team->MembersFrom = ECrowdyServerMembersSource::CrowdyTeam;
		Team->bOnPlayerLeft = true;
		AddPlainFunction(*Team, TEXT("Deposit"), ECrowdyServerFunctionCaller::Members);
		AddPlainFunction(*Team, TEXT("Kick"), ECrowdyServerFunctionCaller::Leader);
		return Team;
	}

	/** ck_exec_world: Only One Instance, one shared object every player deposits into. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeWorldDefinition()
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> World = MakeListDefinition(TEXT("ck_exec_world"), {TEXT("Gold")});
		World->WatchedFields = {TEXT("Gold")};
		World->bOnlyOneInstance = true;
		AddPlainFunction(*World, TEXT("Deposit"), Players);
		return World;
	}

	/** The crates the local cargo check compiles and tests, beside the plugin in the crowdy-sdk repo. */
	FString GetLocalCheckDirectory(const TCHAR* Crate)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CrowdySDK"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(Plugin->GetBaseDir(), TEXT("../../scripts/ck-exec-gate/local-check"), Crate));
		FPaths::CollapseRelativeDirectories(Directory);
		return Directory;
	}

	void CheckLocalCheckCrate(FAutomationTestBase& Test, const CrowdyExecCodegen::FGeneratedCrate& Crate, const TCHAR* Name)
	{
		const FString Directory = GetLocalCheckDirectory(Name);
		if (!Test.TestFalse(TEXT("the CrowdySDK plugin is found"), Directory.IsEmpty()))
		{
			return;
		}
		for (const TCHAR* Path : {CargoPath, LibPath, TypesPath, LogicPath})
		{
			CheckGolden(Test, Directory, Crate, Path);
		}
	}

	bool GenerateLib(FAutomationTestBase& Test, UCrowdyServerObjectDefinition& Definition, FString& OutLib)
	{
		CrowdyExecCodegen::FGeneratedCrate Crate;
		if (!GenerateCrate(Test, Definition, Crate))
		{
			return false;
		}
		OutLib = GetFileText(Test, Crate, LibPath);
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenGoldenTest, "CrowdySDK.CrowdyExecEditor.CodegenFixtureMatchesGoldens", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenGoldenTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, FixtureFunctions());
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!Definition || !GenerateCrate(*this, *Definition, Crate))
	{
		return false;
	}
	const FString Directory = GetGoldenDirectory();
	if (!TestFalse(TEXT("the CrowdySDK plugin is found"), Directory.IsEmpty()))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the crate is named after the type"), Crate.Name, FString(FixtureName));
	TestEqual(TEXT("the crate has four files"), Crate.Files.Num(), 4);
	for (const TCHAR* Path : {CargoPath, LibPath, TypesPath, LogicPath})
	{
		CheckGolden(*this, Directory, Crate, Path);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenDeterministicTest, "CrowdySDK.CrowdyExecEditor.CodegenIsDeterministic", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenDeterministicTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, FixtureFunctions());
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Rebuilt = MakeDefinition(*this, FixtureFunctions());
	CrowdyExecCodegen::FGeneratedCrate First;
	CrowdyExecCodegen::FGeneratedCrate Second;
	CrowdyExecCodegen::FGeneratedCrate Third;
	if (!Definition || !Rebuilt || !GenerateCrate(*this, *Definition, First) || !GenerateCrate(*this, *Definition, Second) || !GenerateCrate(*this, *Rebuilt, Third))
	{
		return false;
	}
	if (!TestEqual(TEXT("each run writes the same files"), Second.Files.Num(), First.Files.Num())
		|| !TestEqual(TEXT("a rebuilt definition writes the same files"), Third.Files.Num(), First.Files.Num()))
	{
		return false;
	}
	for (int32 Index = 0; Index < First.Files.Num(); ++Index)
	{
		const CrowdyExecCodegen::FGeneratedFile& File = First.Files[Index];
		TestEqualSensitive(TEXT("the files come in the same order"), Second.Files[Index].Path, File.Path);
		TestTrue(FString::Printf(TEXT("%s is the same when generated twice"), *File.Path), Second.Files[Index].Text.Equals(File.Text, ESearchCase::CaseSensitive));
		TestTrue(FString::Printf(TEXT("%s is the same from a rebuilt definition"), *File.Path), Third.Files[Index].Text.Equals(File.Text, ESearchCase::CaseSensitive));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenWriteKeepsLogicTest, "CrowdySDK.CrowdyExecEditor.CodegenWriteCrateKeepsLogic", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenWriteKeepsLogicTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, FixtureFunctions());
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!Definition || !GenerateCrate(*this, *Definition, Crate))
	{
		return false;
	}
	FTempFolder Folder;
	const FString Existing = FPaths::Combine(Folder.Path, TEXT("existing"));
	const FString UserLogic = TEXT("// the user's server code\n");
	FFileHelper::SaveStringToFile(UserLogic, *FPaths::Combine(Existing, LogicPath), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FFileHelper::SaveStringToFile(TEXT("stale\n"), *FPaths::Combine(Existing, CargoPath), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	TArray<FString> Written;
	FString Error;
	const bool bRewritten = CrowdyExecCodegen::WriteCrate(Crate, Existing, Written, Error);
	if (!TestTrue(FString::Printf(TEXT("the crate is written over an existing one (%s)"), *Error), bRewritten))
	{
		return false;
	}
	TestEqualSensitive(TEXT("an existing logic.rs is kept"), LoadText(FPaths::Combine(Existing, LogicPath)), UserLogic);
	TestFalse(TEXT("an existing logic.rs is not reported as written"), WasWritten(Written, LogicPath));
	for (const TCHAR* Path : {CargoPath, LibPath, TypesPath})
	{
		TestTrue(FString::Printf(TEXT("%s is replaced"), Path), LoadText(FPaths::Combine(Existing, Path)).Equals(GetFileText(*this, Crate, Path), ESearchCase::CaseSensitive));
		TestTrue(FString::Printf(TEXT("%s is reported as written"), Path), WasWritten(Written, Path));
		TestFalse(FString::Printf(TEXT("%s leaves no temporary file"), Path), FPaths::FileExists(FPaths::Combine(Existing, Path) + TEXT(".tmp")));
	}

	const FString Fresh = FPaths::Combine(Folder.Path, TEXT("fresh"));
	Written.Reset();
	const bool bWritten = CrowdyExecCodegen::WriteCrate(Crate, Fresh, Written, Error);
	if (!TestTrue(FString::Printf(TEXT("the crate is written into an empty folder (%s)"), *Error), bWritten))
	{
		return false;
	}
	TestTrue(TEXT("a missing logic.rs is written"), LoadText(FPaths::Combine(Fresh, LogicPath)).Equals(GetFileText(*this, Crate, LogicPath), ESearchCase::CaseSensitive));
	TestTrue(TEXT("a missing logic.rs is reported as written"), WasWritten(Written, LogicPath));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenLogicFileTest, "CrowdySDK.CrowdyExecEditor.CodegenLogicFileResolves", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenLogicFileTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, {HealFunction});
	if (!Definition)
	{
		return false;
	}
	FTempFolder Folder;
	const FString ProjectDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	TestFalse(TEXT("generated code is not the user's own"), CrowdyExecCodegen::HasOwnLogicFile(*Definition));
	TestEqualSensitive(TEXT("generated code is logic.rs in the project's crate"), CrowdyExecCodegen::GetLogicFile(*Definition),
		FPaths::Combine(ProjectDirectory, TEXT("Server"), FixtureName, TEXT("src/logic.rs")));
	TestEqualSensitive(TEXT("generated code is logic.rs in the given crate"), CrowdyExecCodegen::GetLogicFile(*Definition, Folder.Path), Folder.Path + TEXT("/src/logic.rs"));

	Definition->CodeSource = ECrowdyServerCodeSource::OwnFile;
	Definition->LogicFile.FilePath = TEXT("Custom/my_logic.rs");
	TestTrue(TEXT("a Logic File with Code Source My Own File is the user's own"), CrowdyExecCodegen::HasOwnLogicFile(*Definition));
	const FString Relative = CrowdyExecCodegen::GetLogicFile(*Definition);
	TestFalse(FString::Printf(TEXT("a relative Logic File is made absolute (%s)"), *Relative), FPaths::IsRelative(Relative));
	TestEqualSensitive(TEXT("a relative Logic File is resolved against the project folder"), Relative, FPaths::Combine(ProjectDirectory, TEXT("Custom/my_logic.rs")));
	TestEqualSensitive(TEXT("a set Logic File ignores the crate folder"), CrowdyExecCodegen::GetLogicFile(*Definition, Folder.Path), Relative);

	const FString Absolute = Folder.Path + TEXT("/elsewhere/my_logic.rs");
	Definition->LogicFile.FilePath = Absolute.Replace(TEXT("/"), TEXT("\\"));
	TestEqualSensitive(TEXT("an absolute Logic File is kept, with forward slashes"), CrowdyExecCodegen::GetLogicFile(*Definition), Absolute);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenOwnLogicNoStubTest, "CrowdySDK.CrowdyExecEditor.CodegenOwnLogicFileWritesNoStub", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenOwnLogicNoStubTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, FixtureFunctions());
	if (!Definition)
	{
		return false;
	}
	FTempFolder Folder;
	const FString OwnLogic = FPaths::Combine(Folder.Path, TEXT("elsewhere/my_logic.rs"));
	Definition->CodeSource = ECrowdyServerCodeSource::OwnFile;
	Definition->LogicFile.FilePath = OwnLogic;
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!GenerateCrate(*this, *Definition, Crate))
	{
		return false;
	}
	TestNull(TEXT("with a Logic File the crate has no logic.rs"), FindFileText(Crate, LogicPath));
	for (const TCHAR* Path : {CargoPath, LibPath, TypesPath})
	{
		TestNotNull(FString::Printf(TEXT("with a Logic File the crate still has %s"), Path), FindFileText(Crate, Path));
	}

	const FString Directory = FPaths::Combine(Folder.Path, FixtureName);
	TArray<FString> Written;
	FString Error;
	const bool bWritten = CrowdyExecCodegen::WriteCrate(Crate, Directory, Written, Error);
	if (!TestTrue(FString::Printf(TEXT("the crate is written (%s)"), *Error), bWritten))
	{
		return false;
	}
	TestFalse(TEXT("no logic.rs is written into the crate folder"), FPaths::FileExists(FPaths::Combine(Directory, LogicPath)));
	TestFalse(TEXT("no logic.rs is reported as written"), WasWritten(Written, LogicPath));
	TestFalse(TEXT("the Logic File is not created"), FPaths::FileExists(OwnLogic));
	TestEqual(TEXT("only the generated files are written"), Written.Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenNewFunctionTest, "CrowdySDK.CrowdyExecEditor.CodegenNewFunctionAppearsInGlue", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenNewFunctionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TCHAR* const TraitItem = TEXT("    fn hit(&mut self, ctx: &Ctx, call: &Call<'_>, params: CrowdyExecTestHit) -> Result<()>;\n");
	const TCHAR* const DispatchArm = TEXT("            \"hit\" => {\n");
	const TCHAR* const DispatchCall = TEXT("Functions::hit(&mut self.state, ctx, &call, params)");
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, {HealFunction});
	CrowdyExecCodegen::FGeneratedCrate Before;
	if (!Definition || !GenerateCrate(*this, *Definition, Before))
	{
		return false;
	}
	const FString LibBefore = GetFileText(*this, Before, LibPath);
	TestFalse(TEXT("without the function there is no trait item"), HasText(LibBefore, TraitItem));
	TestFalse(TEXT("without the function there is no dispatch arm"), HasText(LibBefore, DispatchArm));

	CrowdyExecCodegen::FGeneratedCrate After;
	if (!AddFunction(*this, *Definition, HitFunction) || !GenerateCrate(*this, *Definition, After))
	{
		return false;
	}
	const FString LibAfter = GetFileText(*this, After, LibPath);
	TestTrue(TEXT("the added function is a trait item"), HasText(LibAfter, TraitItem));
	TestTrue(TEXT("the added function has a dispatch arm"), HasText(LibAfter, DispatchArm));
	TestTrue(TEXT("the dispatch arm calls the function with its params"), HasText(LibAfter, DispatchCall));
	TestTrue(TEXT("the added function's params struct is written"), HasText(GetFileText(*this, After, TypesPath), TEXT("pub struct CrowdyExecTestHit {\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenListsTest, "CrowdySDK.CrowdyExecEditor.CodegenNamesLists", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenListsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
	Definition->TypeName = TEXT("tip_jar");
	Definition->StateForm = ECrowdyServerValuesForm::List;
	AddListValue(Definition->StateList, FPropertyBagPropertyDesc(TEXT("Total"), EPropertyBagPropertyType::Int32));
	Definition->StateList.SetValueInt32(TEXT("Total"), 5);
	Definition->WatchedFields = {TEXT("Total")};
	FCrowdyServerFunction& Tip = Definition->Functions.AddDefaulted_GetRef();
	Tip.Name = TEXT("Tip");
	Tip.ParamsForm = ECrowdyServerValuesForm::List;
	AddListValue(Tip.ParamsList, FPropertyBagPropertyDesc(TEXT("Amount"), EPropertyBagPropertyType::Int32));
	Tip.ParamsList.SetValueInt32(TEXT("Amount"), 1);
	Tip.ReplyForm = ECrowdyServerValuesForm::List;
	AddListValue(Tip.ReplyList, FPropertyBagPropertyDesc(TEXT("Total"), EPropertyBagPropertyType::Int32));
	Tip.ReplyList.SetValueInt32(TEXT("Total"), 5);
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!GenerateCrate(*this, *Definition, Crate))
	{
		return false;
	}
	const FString Types = GetFileText(*this, Crate, TypesPath);
	TestTrue(TEXT("the State List is a struct named after the type, each value with its id"),
		HasText(Types, *FString::Printf(TEXT("pub struct TipJarState {\n    pub Total: i32, // Int32 id=%s\n}"), *ValueIdText(TEXT("Total")))));
	TestTrue(TEXT("its default is the List's starting value"), HasText(Types, TEXT("impl Default for TipJarState {\n    fn default() -> Self {\n        Self {\n            Total: 5,")));
	TestTrue(TEXT("the params List is a struct named after the function"),
		HasText(Types, *FString::Printf(TEXT("pub struct TipParams {\n    pub Amount: i32, // Int32 id=%s\n}"), *ValueIdText(TEXT("Amount")))));
	TestTrue(TEXT("with its default"), HasText(Types, TEXT("            Amount: 1,")));
	TestTrue(TEXT("a reply List shaped like the State and starting with its values shares its struct under its own name"), HasText(Types, TEXT("pub type TipReply = TipJarState;\n")));
	TestTrue(TEXT("the function takes and returns them"),
		HasText(GetFileText(*this, Crate, LibPath), TEXT("fn tip(&mut self, ctx: &Ctx, call: &Call<'_>, params: TipParams) -> Result<TipReply>;")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecVariableTypesTest, "CrowdySDK.CrowdyExecEditor.VariableTypePickerOffersCarriedTypes", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecVariableTypesTest::RunTest(const FString& Parameters)
{
	const UCrowdyServerObjectDefinition* Definition = GetDefault<UCrowdyServerObjectDefinition>();
	auto Accepts = [Definition](FName Category, UObject* SubCategory = nullptr, bool bChild = false)
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = Category;
		PinType.PinSubCategoryObject = SubCategory;
		return Definition->IsServerValueTypeAccepted(PinType, bChild);
	};
	TestTrue(TEXT("integers are offered"), Accepts(TEXT("int")));
	TestTrue(TEXT("strings are offered"), Accepts(TEXT("string")));
	TestTrue(TEXT("structs are offered"), Accepts(TEXT("struct")));
	TestTrue(TEXT("soft object paths are offered"), Accepts(TEXT("softobject")));
	TestFalse(TEXT("text is not offered"), Accepts(TEXT("text")));
	TestFalse(TEXT("object references are not offered"), Accepts(TEXT("object")));
	TestFalse(TEXT("interfaces are not offered"), Accepts(TEXT("interface")));
	TestTrue(TEXT("FVector is offered as a struct"), Accepts(TEXT("struct"), TBaseStructure<FVector>::Get(), true));
	TestFalse(TEXT("FInstancedStruct is not offered"), Accepts(TEXT("struct"), FInstancedStruct::StaticStruct(), true));
	TestFalse(TEXT("a variable list is not offered as a value"), Accepts(TEXT("struct"), FInstancedPropertyBag::StaticStruct(), true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenFindRemovalsTest, "CrowdySDK.CrowdyExecEditor.CodegenFindRemovals", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenFindRemovalsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const FString Old = RustTypesHeader
		+ RustEnum(TEXT("Phase"), {TEXT("Calm"), TEXT("Enraged"), TEXT("Final")})
		+ RustEnum(TEXT("Rarity"), {TEXT("Common"), TEXT("Rare")})
		+ RustStruct(TEXT("Boss"), {{TEXT("Health"), TEXT("i32")}, {TEXT("Armor"), TEXT("i32")}, {TEXT("r#type"), TEXT("String")}, {TEXT("Phase"), TEXT("Phase")}})
		+ RustStruct(TEXT("Loot"), {{TEXT("Gold"), TEXT("i32")}});
	const FString Changed = RustTypesHeader
		+ RustEnum(TEXT("Phase"), {TEXT("Calm"), TEXT("Enraged"), TEXT("Frenzy")})
		+ RustStruct(TEXT("Boss"), {{TEXT("Health"), TEXT("i64")}, {TEXT("r#type"), TEXT("String")}, {TEXT("Phase"), TEXT("Phase")}, {TEXT("Speed"), TEXT("f32")}})
		+ RustStruct(TEXT("Chest"), {{TEXT("Items"), TEXT("Vec<String>")}});
	const FString Grown = RustTypesHeader
		+ RustEnum(TEXT("Phase"), {TEXT("Calm"), TEXT("Enraged"), TEXT("Final"), TEXT("Frenzy")})
		+ RustEnum(TEXT("Rarity"), {TEXT("Common"), TEXT("Rare")})
		+ RustEnum(TEXT("Weather"), {TEXT("Clear")})
		+ RustStruct(TEXT("Boss"), {{TEXT("Health"), TEXT("i32")}, {TEXT("Armor"), TEXT("i32")}, {TEXT("r#type"), TEXT("String")}, {TEXT("Phase"), TEXT("Phase")}, {TEXT("Speed"), TEXT("f32")}})
		+ RustStruct(TEXT("Loot"), {{TEXT("Gold"), TEXT("i32")}})
		+ RustStruct(TEXT("Chest"), {{TEXT("Items"), TEXT("Vec<String>")}});

	const FString Expected = FString::Join(TArray<FString>{
		TEXT("Phase::Final is gone"),
		TEXT("enum Rarity is gone"),
		TEXT("Boss.Health changes type from i32 to i64"),
		TEXT("Boss.Armor is gone"),
		TEXT("struct Loot is gone")}, TEXT(" | "));
	TestEqualSensitive(TEXT("each removal and type change is reported once, in the old file's order"),
		FString::Join(CrowdyExecCodegen::FindRemovals(Old, Changed), TEXT(" | ")), Expected);
	TestEqualSensitive(TEXT("additions report nothing"), FString::Join(CrowdyExecCodegen::FindRemovals(Old, Grown), TEXT(" | ")), FString());
	TestEqualSensitive(TEXT("the same text reports nothing"), FString::Join(CrowdyExecCodegen::FindRemovals(Old, Old), TEXT(" | ")), FString());
	TestEqualSensitive(TEXT("a first generation reports nothing"), FString::Join(CrowdyExecCodegen::FindRemovals(FString(), Old), TEXT(" | ")), FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenKeywordFieldTest, "CrowdySDK.CrowdyExecEditor.CodegenKeywordFieldIsRaw", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenKeywordFieldTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, {EchoRawFunction});
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!Definition || !GenerateCrate(*this, *Definition, Crate))
	{
		return false;
	}
	const FString Types = GetFileText(*this, Crate, TypesPath);
	TestTrue(TEXT("a field named type is written raw"), HasText(Types, TEXT("    pub r#type: String, // String\n")));
	TestFalse(TEXT("a field named type is never written bare"), HasText(Types, TEXT("    pub type: String,")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenSelfFieldTest, "CrowdySDK.CrowdyExecEditor.CodegenSelfFieldIsRefused", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenSelfFieldTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, {HitFunction});
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!Definition || !GenerateCrate(*this, *Definition, Crate))
	{
		return false;
	}
	FCrowdyServerFieldName& Override = Definition->FieldNames.AddDefaulted_GetRef();
	Override.Struct = Definition->Functions[0].Params;
	Override.Field = TEXT("Damage");
	Override.ServerName = TEXT("self");
	TArray<FString> Errors;
	const bool bBaked = Definition->Bake(Errors);
	if (!TestTrue(FString::Printf(TEXT("self is a valid server name (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked))
	{
		return false;
	}
	FString Error;
	CrowdyExecCodegen::FGeneratedCrate Refused;
	TestFalse(TEXT("a field whose server name is self is not generated"), CrowdyExecCodegen::Generate(*Definition, Refused, Error));
	TestTrue(FString::Printf(TEXT("the refusal names the struct (%s)"), *Error), HasText(Error, TEXT("CrowdyExecTestHit")));
	TestTrue(FString::Printf(TEXT("the refusal names the field (%s)"), *Error), HasText(Error, TEXT("self")));
	TestTrue(FString::Printf(TEXT("the refusal says to set a server name (%s)"), *Error), Error.Contains(TEXT("server name")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenOwnerOnlyTest, "CrowdySDK.CrowdyExecEditor.CodegenOwnerOnlyConstant", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenOwnerOnlyTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, {HealFunction});
	CrowdyExecCodegen::FGeneratedCrate Public;
	if (!Definition || !GenerateCrate(*this, *Definition, Public))
	{
		return false;
	}
	const TCHAR* const UserIdCheck = TEXT("        if !is_user_id(&ctx.key) {\n");
	const FString PublicLib = GetFileText(*this, Public, LibPath);
	TestTrue(TEXT("a public type is not owner-only"), HasText(PublicLib, TEXT("\nconst OWNER_ONLY: bool = false;\n")));
	TestFalse(TEXT("a public type accepts any Instance Id"), HasText(PublicLib, TEXT("is_user_id")));

	Definition->Visibility = ECrowdyServerObjectVisibility::OwnerOnly;
	CrowdyExecCodegen::FGeneratedCrate OwnerOnly;
	if (!GenerateCrate(*this, *Definition, OwnerOnly))
	{
		return false;
	}
	const FString OwnerOnlyLib = GetFileText(*this, OwnerOnly, LibPath);
	TestTrue(TEXT("an owner-only type says so"), HasText(OwnerOnlyLib, TEXT("\nconst OWNER_ONLY: bool = true;\n")));
	TestTrue(TEXT("an owner-only type knows a user id"), HasText(OwnerOnlyLib, TEXT("\nfn is_user_id(key: &str) -> bool {\n")));
	TestEqual(TEXT("an owner-only type checks the Instance Id on spawn and on load"), CountText(OwnerOnlyLib, UserIdCheck), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenNoWatchedTest, "CrowdySDK.CrowdyExecEditor.CodegenNoWatchedFieldsHasNoRead", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenNoWatchedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TCHAR* const ReadArm = TEXT("\"read\" =>");
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, {HealFunction});
	CrowdyExecCodegen::FGeneratedCrate Watched;
	if (!Definition || !GenerateCrate(*this, *Definition, Watched))
	{
		return false;
	}
	const FString WatchedLib = GetFileText(*this, Watched, LibPath);
	TestTrue(TEXT("a type with watched fields answers read"), HasText(WatchedLib, ReadArm));
	TestTrue(TEXT("a type with watched fields writes read replies"), HasText(WatchedLib, TEXT("fn read_reply(")));

	Definition->WatchedFields.Reset();
	CrowdyExecCodegen::FGeneratedCrate Unwatched;
	if (!GenerateCrate(*this, *Definition, Unwatched))
	{
		return false;
	}
	const FString UnwatchedLib = GetFileText(*this, Unwatched, LibPath);
	TestFalse(TEXT("a type without watched fields has no read arm"), HasText(UnwatchedLib, ReadArm));
	TestFalse(TEXT("a type without watched fields writes no read replies"), HasText(UnwatchedLib, TEXT("fn read_reply(")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenDefaultsTest, "CrowdySDK.CrowdyExecEditor.CodegenDefaultsAreTheStructsOwn", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenDefaultsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, FixtureFunctions());
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!Definition || !GenerateCrate(*this, *Definition, Crate))
	{
		return false;
	}
	const FString Types = GetFileText(*this, Crate, TypesPath);
	TestTrue(TEXT("the State's Health starts at its initializer"), HasText(Types, TEXT("            Health: 4000,\n")));
	TestTrue(TEXT("a nested struct is a full literal of the parent's initializer"), HasText(Types, TEXT("            Orientation: Quat { X: 0.0, Y: 0.0, Z: 0.0, W: 1.0 },\n")));
	TestTrue(TEXT("a float nested struct keeps its initializer"), HasText(Types, TEXT("            Tint: LinearColor { R: 1.0, G: 1.0, B: 1.0, A: 1.0 },\n")));
	TestTrue(TEXT("a byte nested struct keeps its initializer"), HasText(Types, TEXT("            Color: Color { R: 255, G: 255, B: 255, A: 255 },\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenDefaultFunctionTest, "CrowdySDK.CrowdyExecEditor.CodegenDefaultFunctionKeepsDefaultCallable", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenDefaultFunctionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, {DefaultFunction});
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!Definition || !GenerateCrate(*this, *Definition, Crate))
	{
		return false;
	}
	const FString Lib = GetFileText(*this, Crate, LibPath);
	TestTrue(TEXT("a function named Default is a trait item"), HasText(Lib, TEXT("    fn default(&mut self, ctx: &Ctx, call: &Call<'_>) -> Result<()>;\n")));
	TestTrue(TEXT("the State's default is called through the Default trait"), HasText(Lib, TEXT("<CrowdyExecTestBossState as Default>::default()")));
	TestFalse(TEXT("the State's default is never called bare"), HasText(Lib, TEXT("CrowdyExecTestBossState::default()")));
	TestFalse(TEXT("the State is never cloned by method call"), HasText(Lib, TEXT("self.state.clone()")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenSetTest, "CrowdySDK.CrowdyExecEditor.CodegenSetIsBTreeSet", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenSetTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, FixtureFunctions());
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!Definition || !GenerateCrate(*this, *Definition, Crate))
	{
		return false;
	}
	const FString Types = GetFileText(*this, Crate, TypesPath);
	TestTrue(TEXT("a string set is a BTreeSet"), HasText(Types, TEXT("    pub Tags: BTreeSet<String>, // Set<String>\n")));
	TestTrue(TEXT("an enum set is a BTreeSet"), HasText(Types, TEXT("    pub Phases: BTreeSet<CrowdyExecTestPhase>, // Set<Enum CrowdyExecTestPhase>\n")));
	TestTrue(TEXT("an empty set defaults to a new BTreeSet"), HasText(Types, TEXT("            Tags: BTreeSet::new(),\n")));
	TestTrue(TEXT("types.rs imports BTreeSet"), HasText(Types, TEXT("\nuse std::collections::{BTreeMap, BTreeSet};\n")));
	TestTrue(TEXT("lib.rs imports BTreeSet"), HasText(GetFileText(*this, Crate, LibPath), TEXT("\nuse std::collections::{BTreeMap, BTreeSet};\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenTypeCommentTest, "CrowdySDK.CrowdyExecEditor.CodegenFieldCommentNamesUnrealType", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenTypeCommentTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, FixtureFunctions());
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!Definition || !GenerateCrate(*this, *Definition, Crate))
	{
		return false;
	}
	const FString Types = GetFileText(*this, Crate, TypesPath);
	TestTrue(TEXT("a GUID field says it is a GUID"), HasText(Types, TEXT("    pub Id: String, // Guid\n")));
	TestTrue(TEXT("a name field says it is a name"), HasText(Types, TEXT("    pub Source: String, // Name\n")));
	TestTrue(TEXT("a map field names its key and value"), HasText(Types, TEXT("    pub DamageByPlayer: BTreeMap<String, i32>, // Map<String, Int32>\n")));
	TestTrue(TEXT("a struct array names its struct"), HasText(Types, TEXT("    pub Attacks: Vec<CrowdyExecTestHit>, // Array<Struct CrowdyExecTestHit>\n")));
	TestTrue(TEXT("an optional names its value"), HasText(Types, TEXT("    pub Maybe: Option<i32>, // Optional<Int32>\n")));
	TestTrue(TEXT("an engine struct field names its kind"), HasText(Types, TEXT("    pub Arena: Vector, // Vector\n")));
	TestTrue(TEXT("an engine struct's own fields name their kinds"), HasText(Types, TEXT("    pub R: u8, // UInt8\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenUnrealTypeChangeTest, "CrowdySDK.CrowdyExecEditor.CodegenFindRemovalsSeesUnrealTypeChange", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenUnrealTypeChangeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const FString Old = FString(RustTypesHeader) + TEXT("pub struct Loot {\n    pub Id: String, // String\n    pub When: i64, // DateTime\n}\n");
	const FString New = FString(RustTypesHeader) + TEXT("pub struct Loot {\n    pub Id: String, // Guid\n    pub When: i64, // DateTime\n}\n");
	const FString Uncommented = FString(RustTypesHeader) + TEXT("pub struct Loot {\n    pub Id: String,\n    pub When: i64,\n}\n");
	const FString Expected = TEXT("Loot.Id changes type from String to Guid");
	TestEqualSensitive(TEXT("a change the Rust type hides is reported from the Unreal type"), FString::Join(CrowdyExecCodegen::FindRemovals(Old, New), TEXT(" | ")), Expected);
	TestEqualSensitive(TEXT("CRLF line endings are read the same"),
		FString::Join(CrowdyExecCodegen::FindRemovals(Old.Replace(TEXT("\n"), TEXT("\r\n")), New), TEXT(" | ")), Expected);
	TestEqualSensitive(TEXT("a file written before the comments compares Rust types"),
		FString::Join(CrowdyExecCodegen::FindRemovals(Uncommented, New), TEXT(" | ")), FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenRandomDefaultTest, "CrowdySDK.CrowdyExecEditor.CodegenRandomDefaultIsRefused", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenRandomDefaultTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, {RandomDefaultFunction});
	if (!Definition)
	{
		return false;
	}
	TArray<FString> Errors;
	const bool bBaked = Definition->Bake(Errors);
	if (!TestTrue(FString::Printf(TEXT("the definition bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked))
	{
		return false;
	}
	FString Error;
	CrowdyExecCodegen::FGeneratedCrate Crate;
	TestFalse(TEXT("a struct whose default changes each time is not generated"), CrowdyExecCodegen::Generate(*Definition, Crate, Error));
	TestTrue(FString::Printf(TEXT("the refusal names the struct and field (%s)"), *Error), HasText(Error, TEXT("CrowdyExecTestRandomDefault.Id")));
	TestTrue(FString::Printf(TEXT("the refusal says the default changes (%s)"), *Error), HasText(Error, TEXT("not the same each time")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenParamsCheckedTest, "CrowdySDK.CrowdyExecEditor.CodegenParamsAreChecked", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenParamsCheckedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TArray<FFunctionSpec> Functions = FixtureFunctions();
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, Functions);
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!Definition || !GenerateCrate(*this, *Definition, Crate))
	{
		return false;
	}
	const int32 WithParams = Functions.FilterByPredicate([](const FFunctionSpec& Spec) { return Spec.Params != nullptr; }).Num();
	const TCHAR* const CheckedDecode = TEXT("call.decode().map_err(bad_params)?;\n                Wire::check(&params).map_err(|why| Error::new(&format!(\"bad_params: {why}\")))?;\n");
	const FString Lib = GetFileText(*this, Crate, LibPath);
	TestEqual(TEXT("each arm with params decodes them"), CountText(Lib, TEXT("let params: ")), WithParams);
	TestEqual(TEXT("each arm with params checks them right after decoding"), CountText(Lib, CheckedDecode), WithParams);
	TestTrue(TEXT("a reply is checked before it is sent"), HasText(Lib, TEXT(".and_then(|reply| encode_reply(&reply));\n")));
	TestTrue(TEXT("a changed watched value is checked before it is sent"), HasText(Lib, TEXT("Wire::check(&self.state.Health).map_err(|why| unsendable(\"Health\", why))?;\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenWhoCanCallTest, "CrowdySDK.CrowdyExecEditor.CodegenWhoCanCallReachesGlue", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenWhoCanCallTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TCHAR* const HitAllowed = TEXT("            \"hit\" => {\n                authorize(ctx, &call, true)?;\n");
	const TCHAR* const HitRefused = TEXT("            \"hit\" => {\n                authorize(ctx, &call, false)?;\n");
	const TCHAR* const HealRefused = TEXT("            \"heal\" => {\n                authorize(ctx, &call, false)?;\n");
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, {HitFunction, HealFunction});
	CrowdyExecCodegen::FGeneratedCrate Mixed;
	if (!Definition || !GenerateCrate(*this, *Definition, Mixed))
	{
		return false;
	}
	const FString MixedLib = GetFileText(*this, Mixed, LibPath);
	TestTrue(TEXT("a function players can call lets players in"), HasText(MixedLib, HitAllowed));
	TestTrue(TEXT("a server-only function refuses players"), HasText(MixedLib, HealRefused));

	Definition->Functions[0].WhoCanCall = ECrowdyServerFunctionCaller::ServerOnly;
	CrowdyExecCodegen::FGeneratedCrate ServerOnlyHit;
	if (!GenerateCrate(*this, *Definition, ServerOnlyHit))
	{
		return false;
	}
	const FString ServerOnlyLib = GetFileText(*this, ServerOnlyHit, LibPath);
	TestTrue(TEXT("switching a function to Server only refuses players"), HasText(ServerOnlyLib, HitRefused));
	TestFalse(TEXT("a Server only function no longer lets players in"), HasText(ServerOnlyLib, HitAllowed));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenLeftoverLogicFileTest, "CrowdySDK.CrowdyExecEditor.CodegenGeneratedIgnoresLeftoverLogicFile", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenLeftoverLogicFileTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, {HealFunction});
	if (!Definition)
	{
		return false;
	}
	FTempFolder Folder;
	Definition->CodeSource = ECrowdyServerCodeSource::Generated;
	Definition->LogicFile.FilePath = TEXT("Custom/leftover_logic.rs");
	TestFalse(TEXT("a Logic File left over from My Own File does not make generated code the user's own"), CrowdyExecCodegen::HasOwnLogicFile(*Definition));
	TestEqualSensitive(TEXT("a leftover Logic File is ignored for generated code"), CrowdyExecCodegen::GetLogicFile(*Definition, Folder.Path), Folder.Path + TEXT("/src/logic.rs"));
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!GenerateCrate(*this, *Definition, Crate))
	{
		return false;
	}
	TestNotNull(TEXT("generated code still gets its logic.rs with a leftover Logic File"), FindFileText(Crate, LogicPath));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenOwnFileUnchosenTest, "CrowdySDK.CrowdyExecEditor.CodegenOwnFileWithoutPathHasNoLogicFile", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenOwnFileUnchosenTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, {HealFunction});
	if (!Definition)
	{
		return false;
	}
	FTempFolder Folder;
	Definition->CodeSource = ECrowdyServerCodeSource::OwnFile;
	Definition->LogicFile.FilePath.Reset();
	TestTrue(TEXT("My Own File with no file chosen is still the user's own"), CrowdyExecCodegen::HasOwnLogicFile(*Definition));
	TestEqualSensitive(TEXT("My Own File with no file chosen has no logic file"), CrowdyExecCodegen::GetLogicFile(*Definition), FString());
	TestEqualSensitive(TEXT("My Own File with no file chosen has no logic file in any crate"), CrowdyExecCodegen::GetLogicFile(*Definition, Folder.Path), FString());

	FDataValidationContext Unchosen;
	Definition->IsDataValid(Unchosen);
	TestEqual(TEXT("validation warns when My Own File has no file chosen"), static_cast<int32>(Unchosen.GetNumWarnings()), 1);
	Definition->LogicFile.FilePath = TEXT("Custom/my_logic.rs");
	FDataValidationContext Chosen;
	Definition->IsDataValid(Chosen);
	TestEqual(TEXT("validation does not warn once a file is chosen"), static_cast<int32>(Chosen.GetNumWarnings()), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenRulesCrateTest, "CrowdySDK.CrowdyExecEditor.CodegenRulesMatchLocalCheckCrate", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenRulesCrateTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Bank = MakeBankDefinition();
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Rules = MakeRulesDefinition(*Bank);
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!BakeDefinition(*this, *Bank) || !GenerateCrate(*this, *Rules, Crate))
	{
		return false;
	}
	CheckLocalCheckCrate(*this, Crate, TEXT("ck_exec_rules"));
	const FString Lib = GetFileText(*this, Crate, LibPath);
	TestTrue(TEXT("a Members function refuses other players"), HasText(Lib, TEXT("\"denied: only members may call {}\", function_name(call.method)")));
	TestTrue(TEXT("a Leader function refuses other players"), HasText(Lib, TEXT("\"denied: only the leader may call {}\", function_name(call.method)")));
	TestTrue(TEXT("refusals name a function as Unreal does"), HasText(Lib, TEXT("        \"deposit\" => \"Deposit\",\n")));
	TestTrue(TEXT("built-in functions too"), HasText(Lib, TEXT("        \"remove_member\" => \"RemoveMember\",\n")));
	TestTrue(TEXT("the function's callers are its rule"), HasText(Lib, TEXT("            \"deposit\" => {\n                authorize(ctx, &call, &self.roster, Rule::Members)?;\n")));
	TestTrue(TEXT("Join refuses a full object"), HasText(Lib, TEXT("return Err(Error::new(\"It is full\"));")));
	TestTrue(TEXT("Join refuses a closed object"), HasText(Lib, TEXT("return Err(Error::new(\"It is not open for joining\"));")));
	for (const TCHAR* Method : {TEXT("join"), TEXT("leave"), TEXT("add_member"), TEXT("remove_member"), TEXT("make_leader"), TEXT("set_open_for_joining")})
	{
		TestTrue(FString::Printf(TEXT("the built-in %s is dispatched"), Method), HasText(Lib, *FString::Printf(TEXT("            \"%s\" => {\n"), Method)));
	}
	TestTrue(TEXT("Add Member is server only"), HasText(Lib, TEXT("\"add_member\" => {\n                authorize(ctx, &call, &self.roster, Rule::ServerOnly)?;\n")));
	TestTrue(TEXT("Set Open For Joining takes bOpen"), HasText(Lib, TEXT("let params: OpenInputs = call.decode().map_err(bad_params)?;")));
	TestTrue(TEXT("a Value Range is checked after decoding"), HasText(Lib, TEXT("Wire::check(&params).map_err(|why| Error::new(&format!(\"bad_params: {why}\")))?;\n                if (params.Amount as i128) < 1 || (params.Amount as i128) > 100 {\n                    return Err(Error::new(\"bad_params: Amount must be 1 to 100\"));\n")));
	TestTrue(TEXT("a cooldown is checked before the logic"), HasText(Lib, TEXT("check_cooldown(&self.cooldowns, ctx, &call, \"Deposit\", 2000)?;\n                members::lend(&self.roster);\n")));
	TestTrue(TEXT("a cooldown is recorded after a success only"), HasText(Lib, TEXT("let result = self.finish(ctx, before, result);\n                if result.is_ok() {\n                    record_cooldown(&mut self.cooldowns, ctx, &call, 2000);\n")));
	TestTrue(TEXT("a cooldown reads in whole seconds"), HasText(Lib, TEXT("\"cooldown: {name} can be called again in {} s\", (ready - now).div_ceil(1000)")));
	TestTrue(TEXT("the cooldowns are capped"), HasText(Lib, TEXT("const MAX_COOLDOWN_PLAYERS: usize = 10000;")));
	TestTrue(TEXT("the snapshot keeps the members, older snapshots loading open"), HasText(Lib, TEXT("    #[serde(default = \"open_by_default\")]\n    open: bool,\n")));
	TestTrue(TEXT("the contract stays 1"), HasText(Lib, TEXT("\nconst CONTRACT: u64 = 1;\n")));
	TestTrue(TEXT("a read reports the members"), HasText(Lib, TEXT("write_field(&mut out, \"members\", &self.roster.members)?;")));
	TestTrue(TEXT("a read reports the reader's membership"), HasText(Lib, TEXT("write_field(&mut out, \"is_member\", &member)?;\n        write_field(&mut out, \"is_leader\", &leader)?;\n")));
	TestTrue(TEXT("an Every Player push carries the changed member keys"), HasText(Lib, TEXT("write_map(&mut out, 3 + roster.len());")));
	TestTrue(TEXT("a roster change alone is pushed"), HasText(Lib, TEXT("if changed.is_empty() && roster.is_empty() {")));
	TestTrue(TEXT("on_session has the platform's signature"), HasText(Lib, TEXT("fn on_session(&mut self, ctx: &Ctx, player: u64, session: ckx_sdk::Session) -> Result<()> {")));
	TestTrue(TEXT("a member who leaves is removed, for good, before On Player Left"),
		HasText(Lib, TEXT("members::remove(player);\n                self.finish(ctx, before, Ok(()))?;\n                // The removal stands even when on_player_left fails.\n")));
	TestTrue(TEXT("sessions are saved"), HasText(Lib, TEXT("    sessions: &'a BTreeMap<u64, u32>,\n")));
	TestTrue(TEXT("older snapshots load without sessions"), HasText(Lib, TEXT("    #[serde(default)]\n    sessions: BTreeMap<u64, u32>,\n")));
	TestTrue(TEXT("sessions are capped"), HasText(Lib, TEXT("if connections == 0 && self.sessions.len() >= MAX_SESSION_PLAYERS {")));
	TestTrue(TEXT("a player never counted does not leave"), HasText(Lib, TEXT("            ckx_sdk::Session::Left => {\n                if connections == 0 {\n                    return Ok(());\n")));
	TestTrue(TEXT("a nested dispatch keeps the outer members"), HasText(Lib, TEXT("let outer = OUTER.with(|stack| stack.borrow_mut().pop()).unwrap_or_default();")));
	TestTrue(TEXT("the last member leaving opens the object"), HasText(Lib, TEXT("        if self.members.is_empty() {\n            self.open = true;\n")));
	TestTrue(TEXT("server code's timers wait for the dispatch"), HasText(Lib, TEXT("                members::lend(&self.roster);\n                timers::open();\n")));
	TestTrue(TEXT("the timers arm after the push is prepared, the roster alongside"),
		HasText(Lib, TEXT("            let push = self.prepare_push(&before, &before_roster)?;\n            timers::arm_queued(ctx, &queued)?;\n")));
	TestTrue(TEXT("a timer that cannot start gives the roster back too"), HasText(Lib, TEXT("                self.state = before;\n                self.roster = before_roster;\n                return Err(error);\n")));
	TestTrue(TEXT("an automatic timer starts in spawn"), HasText(Lib, TEXT("        timers::arm(ctx, \"Tick\")?;\n")));
	TestFalse(TEXT("a timer started by server code does not start in spawn"), HasText(Lib, TEXT("timers::arm(ctx, \"EndMatch\")?;")));
	TestTrue(TEXT("server code starts only the definition's timers"), HasText(Lib, TEXT("if !matches!(name, \"EndMatch\" | \"Tick\") || !queue(name, true) {")));
	TestTrue(TEXT("server code names them by constant"), HasText(Lib, TEXT("    pub const END_MATCH: &str = \"EndMatch\";\n    pub const TICK: &str = \"Tick\";\n")));
	TestTrue(TEXT("an After timer fires once"), HasText(Lib, TEXT("\"EndMatch\" => ctx.timer_after(\"EndMatch\", 10000),")));
	TestTrue(TEXT("an Every timer repeats"), HasText(Lib, TEXT("\"Tick\" => ctx.timer_every(\"Tick\", 5000),")));
	TestTrue(TEXT("on_timer runs a timer's function"), HasText(Lib, TEXT("\"EndMatch\" => Functions::end_match(&mut self.state, ctx),")));
	TestTrue(TEXT("a timer the logic named itself reaches its on_timer"), HasText(Lib, TEXT("other => Functions::on_timer(&mut self.state, ctx, other),")));
	TestTrue(TEXT("Can Call writes a module per type"), HasText(Lib, TEXT("pub mod calls {\n    pub mod ck_exec_bank {\n")));
	TestTrue(TEXT("a callee's inputs are written in its module"),
		HasText(Lib, *FString::Printf(TEXT("        pub struct StoreParams {\n            pub Coins: i32, // Int32 id=%s\n        }\n"), *ValueIdText(TEXT("Coins")))));
	TestTrue(TEXT("a call's error follows the callee's name, so no refusal of the callee's leads it"),
		HasText(Lib, TEXT("let reply = ctx.call(\"ck_exec_bank\", key, \"store\", &payload).map_err(|error| Error::new(&format!(\"ck_exec_bank.store failed: {error}\")))?;\n            decode(&reply)\n")));
	TestFalse(TEXT("the built-in inputs are the glue's own, not a types.rs struct"), HasText(GetFileText(*this, Crate, TypesPath), TEXT("MemberInputs")));
	const FString Logic = GetFileText(*this, Crate, LogicPath);
	TestTrue(TEXT("logic.rs gets a timer's function"), HasText(Logic, TEXT("    fn end_match(&mut self, ctx: &Ctx) -> Result<()> {\n        Ok(())\n    }\n")));
	TestTrue(TEXT("logic.rs gets On Player Joined"), HasText(Logic, TEXT("    fn on_player_joined(&mut self, ctx: &Ctx, player: u64) -> Result<()> {\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenTeamCrateTest, "CrowdySDK.CrowdyExecEditor.CodegenTeamMatchesLocalCheckCrate", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenTeamCrateTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Team = MakeTeamDefinition();
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!GenerateCrate(*this, *Team, Crate))
	{
		return false;
	}
	CheckLocalCheckCrate(*this, Crate, TEXT("ck_exec_team"));
	const FString Lib = GetFileText(*this, Crate, LibPath);
	TestEqual(TEXT("spawn and load refuse a key that is not a team id"), CountText(Lib, TEXT("\"denied: a team Server Object's Instance Id is its team id\"")), 2);
	TestTrue(TEXT("membership is the team's"), HasText(Lib, TEXT("if rule == Rule::Members && !team_check(ctx, player, None)? {")));
	TestTrue(TEXT("the leader holds manage_group"), HasText(Lib, TEXT("if rule == Rule::Leader && !team_check(ctx, player, Some(\"manage_group\"))? {")));
	TestTrue(TEXT("a failed check is denied with the platform's reason"), HasText(Lib, TEXT("\"denied: membership could not be checked ({error})\"")));
	TestTrue(TEXT("logic.rs can ask about the team"), HasText(Lib, TEXT("pub fn is_member_of_team(ctx: &Ctx, player: u64) -> bool {")));
	TestTrue(TEXT("a non-member reads no fields"), HasText(Lib, TEXT("self.read_reply(member, member, leader)")));
	TestTrue(TEXT("a Members push carries only epoch and seq"), HasText(Lib, TEXT("        let mut out = Vec::new();\n        write_map(&mut out, 2);\n")));
	TestFalse(TEXT("a team keeps no roster"), HasText(Lib, TEXT("roster")));
	TestFalse(TEXT("without On Player Joined no hook runs on joining"), HasText(Lib, TEXT("on_player_joined")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenOnlyOneInstanceTest, "CrowdySDK.CrowdyExecEditor.CodegenOnlyOneInstanceMatchesLocalCheckCrate", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenOnlyOneInstanceTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> World = MakeWorldDefinition();
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!GenerateCrate(*this, *World, Crate))
	{
		return false;
	}
	CheckLocalCheckCrate(*this, Crate, TEXT("ck_exec_world"));
	const FString Lib = GetFileText(*this, Crate, LibPath);
	const TCHAR* const Check = TEXT("        if ctx.key != \"main\" {\n")
		TEXT("            return Err(Error::new(\"denied: this Server Object has only one instance, whose Instance Id is main\"));\n")
		TEXT("        }\n");
	TestEqual(TEXT("spawn and load refuse every Instance Id but main"), CountText(Lib, Check), 2);

	World->bOnlyOneInstance = false;
	FString Shared;
	if (!GenerateLib(*this, *World, Shared))
	{
		return false;
	}
	TestFalse(TEXT("without Only One Instance any Instance Id is accepted"), HasText(Shared, TEXT("ctx.key != ")));
	TestTrue(TEXT("Only One Instance adds the check and nothing else"), Lib.Replace(Check, TEXT(""), ESearchCase::CaseSensitive).Equals(Shared, ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenMembersOptionsTest, "CrowdySDK.CrowdyExecEditor.CodegenMembersOptionsReachGlue", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenMembersOptionsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, {HitFunction, HealFunction});
	if (!Definition)
	{
		return false;
	}
	Definition->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	Definition->bShowMembers = false;
	Definition->MaxMembers = 0;
	Definition->bRemoveMembersWhoLeave = false;
	FString Hidden;
	if (!GenerateLib(*this, *Definition, Hidden))
	{
		return false;
	}
	TestTrue(TEXT("a hidden roster still reads its count"), HasText(Hidden, TEXT("write_str(&mut out, \"member_count\");")));
	TestFalse(TEXT("a hidden roster is never read"), HasText(Hidden, TEXT("write_field(&mut out, \"members\"")));
	TestFalse(TEXT("a hidden roster is never pushed"), HasText(Hidden, TEXT("roster.push((\"members\"")));
	TestTrue(TEXT("a hidden roster reads nine entries"), HasText(Hidden, TEXT("        let mut out = Vec::new();\n        write_map(&mut out, 9);\n")));
	TestFalse(TEXT("without events or Remove Members Who Leave there is no on_session"), HasText(Hidden, TEXT("fn on_session(")));
	TestTrue(TEXT("no limit is Max Members 0"), HasText(Hidden, TEXT("\nconst MAX_MEMBERS: usize = 0;\n")));
	TestFalse(TEXT("without a cooldown there are no cooldowns"), HasText(Hidden, TEXT("cooldown")));
	TestFalse(TEXT("without timers there is no timers module"), HasText(Hidden, TEXT("pub mod timers")));
	TestFalse(TEXT("without timers no dispatch holds timer starts"), HasText(Hidden, TEXT("timers::")));
	TestFalse(TEXT("without Can Call there is no calls module"), HasText(Hidden, TEXT("pub mod calls")));

	Definition->bRemoveMembersWhoLeave = true;
	Definition->Visibility = ECrowdyServerObjectVisibility::Members;
	FString MembersOnly;
	if (!GenerateLib(*this, *Definition, MembersOnly))
	{
		return false;
	}
	TestTrue(TEXT("Remove Members Who Leave alone counts sessions"), HasText(MembersOnly, TEXT("fn on_session(")));
	TestTrue(TEXT("Remove Members Who Leave removes the leaver"), HasText(MembersOnly, TEXT("members::remove(player);\n                self.finish(ctx, before, Ok(()))\n")));
	TestTrue(TEXT("Readable By Members hides the fields from non-members"), HasText(MembersOnly, TEXT("self.read_reply(member, member, leader)")));
	TestTrue(TEXT("Readable By Members pushes carry only epoch and seq"), HasText(MembersOnly, TEXT("        let mut out = Vec::new();\n        write_map(&mut out, 2);\n")));
	TestFalse(TEXT("Readable By Members pushes carry no member keys"), HasText(MembersOnly, TEXT("for (name, bytes) in &roster {")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenStructRangeTest, "CrowdySDK.CrowdyExecEditor.CodegenStructValueRangeIsChecked", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenStructRangeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, {HealFunction});
	if (!Definition)
	{
		return false;
	}
	// FCrowdyServerTimer's Seconds carries meta=(ClampMin = 0.01).
	FCrowdyServerFunction& Arm = AddPlainFunction(*Definition, TEXT("Arm"), Players);
	Arm.Params = FCrowdyServerTimer::StaticStruct();
	FString Lib;
	if (!GenerateLib(*this, *Definition, Lib))
	{
		return false;
	}
	TestTrue(TEXT("a C++ input's ClampMin is checked"), HasText(Lib, TEXT("                if params.Seconds < 0.01 {\n                    return Err(Error::new(\"bad_params: Seconds must be at least 0.01\"));\n")));
	TestEqual(TEXT("only the ranged input is checked"), CountText(Lib, TEXT("return Err(Error::new(\"bad_params: ")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenIntegerRangeTest, "CrowdySDK.CrowdyExecEditor.CodegenIntegerValueRangeIsExact", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenIntegerRangeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const auto MakeRanged = [](const TCHAR* Min, const TCHAR* Max)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeListDefinition(TEXT("ck_exec_range"), {TEXT("Gold")});
		FCrowdyServerFunction& Spend = AddPlainFunction(*Definition, TEXT("Spend"), Players);
		Spend.ParamsForm = ECrowdyServerValuesForm::List;
		FPropertyBagPropertyDesc Amount(TEXT("Amount"), EPropertyBagPropertyType::Int64);
		Amount.MetaData.Add(FPropertyBagPropertyDescMetaData(TEXT("ClampMin"), Min));
		Amount.MetaData.Add(FPropertyBagPropertyDescMetaData(TEXT("ClampMax"), Max));
		Spend.ParamsList.AddProperties({Amount});
		return Definition;
	};
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Widest = MakeRanged(TEXT("-9223372036854775808"), TEXT("9223372036854775807"));
	FString Lib;
	if (!GenerateLib(*this, *Widest, Lib))
	{
		return false;
	}
	TestTrue(TEXT("Int64's limits are checked as written"), HasText(Lib, TEXT("if (params.Amount as i128) < -9223372036854775808 || (params.Amount as i128) > 9223372036854775807 {")));
	TestTrue(TEXT("the refusal quotes them as written"), HasText(Lib, TEXT("\"bad_params: Amount must be -9223372036854775808 to 9223372036854775807\"")));

	// A bound past Int64 never reaches the generator: the bake refuses it first.
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> TooWide = MakeRanged(TEXT("0"), TEXT("1000000000000000000000000000000000000000"));
	TArray<FString> Errors;
	TestFalse(TEXT("a bound past Int64 does not bake"), TooWide->Bake(Errors));
	TestTrue(TEXT("the refusal says it is too large"), Errors.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("is too large for the input")); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenTimerKeywordTest, "CrowdySDK.CrowdyExecEditor.CodegenTimerNamedAsKeywordIsRaw", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenTimerKeywordTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, {HealFunction});
	if (!Definition)
	{
		return false;
	}
	AddTimer(*Definition, TEXT("Match"), ECrowdyServerTimerRepeat::Every, 1.f, true);
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!GenerateCrate(*this, *Definition, Crate))
	{
		return false;
	}
	const FString Lib = GetFileText(*this, Crate, LibPath);
	TestTrue(TEXT("a timer named Match has a raw function"), HasText(Lib, TEXT("    fn r#match(&mut self, ctx: &Ctx) -> Result<()>;\n")));
	TestTrue(TEXT("on_timer calls it by its raw name"), HasText(Lib, TEXT("\"Match\" => Functions::r#match(&mut self.state, ctx),")));
	TestTrue(TEXT("the platform knows it by its Name"), HasText(Lib, TEXT("\"Match\" => ctx.timer_every(\"Match\", 1000),")));
	TestTrue(TEXT("logic.rs gets the raw function"), HasText(GetFileText(*this, Crate, LogicPath), TEXT("    fn r#match(&mut self, ctx: &Ctx) -> Result<()> {\n        Ok(())\n    }\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenTimersArmFirstTest, "CrowdySDK.CrowdyExecEditor.CodegenTimersArmBeforeTheChangeIsKept", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenTimersArmFirstTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, {HealFunction});
	FString Plain;
	if (!Definition || !GenerateLib(*this, *Definition, Plain))
	{
		return false;
	}
	AddTimer(*Definition, TEXT("Respawn"), ECrowdyServerTimerRepeat::After, 10.f, false);
	FString Timed;
	if (!GenerateLib(*this, *Definition, Timed))
	{
		return false;
	}
	const FString Queued = TEXT("        let queued = timers::close();\n");
	const FString Arm = TEXT("            timers::arm_queued(ctx, &queued)?;\n");
	const FString Stop = TEXT("        timers::stop_queued(ctx, &queued);\n");
	const FString Finish = FString(TEXT(" result: Result<T>) -> Result<T> {\n")) + Queued
		+ TEXT("        // Everything that can fail runs before the change is kept and pushed.\n")
		TEXT("        let prepared = result.and_then(|value| {\n")
		TEXT("            let push = self.prepare_push(&before)?;\n") + Arm
		+ TEXT("            Ok((value, push))\n")
		TEXT("        });\n")
		TEXT("        let (value, push) = match prepared {\n")
		TEXT("            Ok(prepared) => prepared,\n")
		TEXT("            Err(error) => {\n")
		TEXT("                self.state = before;\n")
		TEXT("                return Err(error);\n")
		TEXT("            }\n")
		TEXT("        };\n")
		TEXT("        if let Some(out) = push {\n")
		TEXT("            self.seq += 1;\n")
		TEXT("            let _ = ctx.publish(\"state\", &out);\n")
		TEXT("        }\n") + Stop
		+ TEXT("        Ok(value)\n")
		TEXT("    }\n");
	TestTrue(TEXT("the timers arm after the push is prepared and before it is published, the stops after"), HasText(Timed, *Finish));
	TestTrue(TEXT("without timers finish is the same order with no timer step"),
		HasText(Plain, *Finish.Replace(*Queued, TEXT("")).Replace(*Arm, TEXT("")).Replace(*Stop, TEXT(""))));
	TestTrue(TEXT("the push is prepared without publishing or counting it"), HasText(Timed, TEXT("    fn prepare_push(&self, before: &")));
	TestFalse(TEXT("nothing but finish counts a push"), HasText(Timed, TEXT("self.seq = ")));
	TestTrue(TEXT("a start that fails cancels the ones armed before it"),
		HasText(Timed, TEXT("            for armed in &starts[..index] {\n                ctx.cancel_timer(armed);\n            }\n")));
	TestTrue(TEXT("and refuses the dispatch naming the timer and the reason"),
		HasText(Timed, TEXT("return Err(Error::new(&format!(\"the change was not kept: timer {name} could not start: {error}\")));")));
	TestFalse(TEXT("a timer that cannot start is not only logged"), HasText(Timed, TEXT("Level::Warn")));
	TestTrue(TEXT("the last start or stop queued for a timer counts"), HasText(Timed, TEXT("            last.retain(|(earlier, _)| *earlier != name);\n            last.push((name, start));\n")));
	TestTrue(TEXT("a timer that cannot start is in the finish comment"), HasText(Timed, TEXT("a timer that cannot start leaves the state as it was")));
	TestFalse(TEXT("but not without timers"), HasText(Plain, TEXT("a timer that cannot start")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenCalleeMembersTest, "CrowdySDK.CrowdyExecEditor.CodegenCallsReachCalleeMembers", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenCalleeMembersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Bank = MakeBankDefinition();
	Bank->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Registry = MakeDefinition(*this, {HealFunction});
	if (!Registry || !BakeDefinition(*this, *Bank))
	{
		return false;
	}
	Registry->CanCall = {Bank.Get(), Bank.Get()};
	FString Lib;
	if (!GenerateLib(*this, *Registry, Lib))
	{
		return false;
	}
	TestEqual(TEXT("a type listed twice has one module"), CountText(Lib, TEXT("    pub mod ck_exec_bank {\n")), 1);
	TestTrue(TEXT("a callee's Add Member is callable"), HasText(Lib, TEXT("        pub fn add_member(ctx: &Ctx, key: &str, inputs: &MemberInputs) -> Result<()> {\n")));
	TestTrue(TEXT("a callee's Set Open For Joining is callable"), HasText(Lib, TEXT("        pub fn set_open_for_joining(ctx: &Ctx, key: &str, inputs: &OpenInputs) -> Result<()> {\n")));
	TestTrue(TEXT("the member inputs are written in the callee's module"), HasText(Lib, TEXT("        pub struct MemberInputs {\n            pub Player: u64,\n        }\n")));
	TestFalse(TEXT("Join takes the caller as the player, so it is not offered to server code"), HasText(Lib, TEXT("pub fn join(")));
	TestFalse(TEXT("the caller keeps no members of its own"), HasText(Lib, TEXT("pub mod members")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenBuiltinNameTest, "CrowdySDK.CrowdyExecEditor.CodegenBuiltinMethodIsRefused", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenBuiltinNameTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, {HealFunction});
	if (!Definition)
	{
		return false;
	}
	AddPlainFunction(*Definition, TEXT("Join"), Players);
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!GenerateCrate(*this, *Definition, Crate))
	{
		return false;
	}
	// The bake refuses this pairing; the generator refuses it too for a definition changed since its bake.
	Definition->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	FString Error;
	TestFalse(TEXT("a function named join is refused beside the built-in"), CrowdyExecCodegen::Generate(*Definition, Crate, Error));
	TestTrue(FString::Printf(TEXT("the refusal names the function (%s)"), *Error), HasText(Error, TEXT("Join")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenRulesDeterministicTest, "CrowdySDK.CrowdyExecEditor.CodegenRulesAreDeterministic", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenRulesDeterministicTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Bank = MakeBankDefinition();
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Rules = MakeRulesDefinition(*Bank);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Rebuilt = MakeRulesDefinition(*Bank);
	CrowdyExecCodegen::FGeneratedCrate First;
	CrowdyExecCodegen::FGeneratedCrate Second;
	CrowdyExecCodegen::FGeneratedCrate Third;
	if (!BakeDefinition(*this, *Bank) || !GenerateCrate(*this, *Rules, First) || !GenerateCrate(*this, *Rules, Second) || !GenerateCrate(*this, *Rebuilt, Third))
	{
		return false;
	}
	if (!TestEqual(TEXT("each run writes the same files"), Second.Files.Num(), First.Files.Num()) || !TestEqual(TEXT("a rebuilt definition writes the same files"), Third.Files.Num(), First.Files.Num()))
	{
		return false;
	}
	for (int32 Index = 0; Index < First.Files.Num(); ++Index)
	{
		const FString& Path = First.Files[Index].Path;
		TestTrue(FString::Printf(TEXT("%s is the same when generated twice"), *Path), Second.Files[Index].Text.Equals(First.Files[Index].Text, ESearchCase::CaseSensitive));
		TestTrue(FString::Printf(TEXT("%s is the same from a rebuilt definition"), *Path), Third.Files[Index].Text.Equals(First.Files[Index].Text, ESearchCase::CaseSensitive));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenKindChecksTest, "CrowdySDK.CrowdyExecEditor.CodegenKindsAreCheckedAsUnrealReadsThem", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenKindChecksTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, FixtureFunctions());
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!Definition || !GenerateCrate(*this, *Definition, Crate))
	{
		return false;
	}
	const FString Lib = GetFileText(*this, Crate, LibPath);
	TestTrue(TEXT("a GUID is checked"), HasText(Lib, TEXT("        Wire::check(&self.Id).and_then(|()| check_guid(&self.Id))?;\n")));
	TestTrue(TEXT("a name is checked"), HasText(Lib, TEXT("        Wire::check(&self.Source).and_then(|()| check_name(&self.Source))?;\n")));
	TestTrue(TEXT("a gameplay tag is checked"), HasText(Lib, TEXT("        Wire::check(&self.Tag).and_then(|()| check_tag(&self.Tag))?;\n")));
	TestTrue(TEXT("a soft path is checked"), HasText(Lib, TEXT("        Wire::check(&self.Mesh).and_then(|()| check_path(&self.Mesh))?;\n")));
	TestTrue(TEXT("a date is checked"), HasText(Lib, TEXT("        Wire::check(&self.When).and_then(|()| check_date(&self.When))?;\n")));
	TestTrue(TEXT("a timespan is checked"), HasText(Lib, TEXT("        Wire::check(&self.Cooldown).and_then(|()| check_timespan(&self.Cooldown))?;\n")));
	TestTrue(TEXT("name map keys are checked one by one and as Unreal names"),
		HasText(Lib, TEXT("        Wire::check(&self.ByName).and_then(|()| self.ByName.iter().try_for_each(|(k, _)| check_name(&k)).and_then(|()| check_name_keys(self.ByName.keys())))?;\n")));
	TestTrue(TEXT("a plain string needs no kind check"), HasText(Lib, TEXT("        Wire::check(&self.r#type)?;\n")));
	TestTrue(TEXT("the date range is the client's"), HasText(Lib, TEXT("\nconst MIN_DATE_MS: i64 = -62135596800000;\nconst MAX_DATE_MS: i64 = 253402300799999;\n")));
	TestTrue(TEXT("the timespan range is the client's"), HasText(Lib, TEXT("\nconst MIN_TIMESPAN_MS: i64 = -922337203685477;\nconst MAX_TIMESPAN_MS: i64 = 922337203685477;\n")));
	TestTrue(TEXT("text keys fold as Unreal compares them"), HasText(Lib, TEXT("        Some(format!(\"{}\\0{}\", compared_part(self), utf16_len(self)))\n")));
	TestTrue(TEXT("only ASCII letters fold"), HasText(Lib, TEXT("    text.split('\\0').next().unwrap_or(\"\").to_ascii_lowercase()\n")));
	TestFalse(TEXT("no Unicode case folding is left"), HasText(Lib, TEXT("to_lowercase")));
	const TCHAR* const Helpers[] = {TEXT("\nfn check_name(text: &str) -> Checked {\n"), TEXT("\nfn check_name_keys<'a>("), TEXT("\nfn check_tag(text: &str) -> Checked {\n"),
		TEXT("\nfn check_guid(text: &str) -> Checked {\n"), TEXT("\nfn check_path(text: &str) -> Checked {\n"), TEXT("\nfn check_date(ms: &i64) -> Checked {\n"),
		TEXT("\nfn check_timespan(ms: &i64) -> Checked {\n")};
	for (const TCHAR* Helper : Helpers)
	{
		TestTrue(FString::Printf(TEXT("the glue defines %s"), *FString(Helper).TrimStartAndEnd()), HasText(Lib, Helper));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenWatchedChecksTest, "CrowdySDK.CrowdyExecEditor.CodegenWatchedValuesAreCheckedOnSeedAndRead", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenWatchedChecksTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeListDefinition(TEXT("ck_exec_ids"), {TEXT("Score")});
	Definition->StateList.AddProperty(TEXT("Owner"), EPropertyBagPropertyType::Name);
	Definition->StateList.AddProperty(TEXT("Id"), EPropertyBagPropertyType::Struct, TBaseStructure<FGuid>::Get());
	Definition->WatchedFields = {TEXT("Owner"), TEXT("Id")};
	FString Lib;
	if (!GenerateLib(*this, *Definition, Lib))
	{
		return false;
	}
	TestTrue(TEXT("a changed GUID is checked before it is pushed"),
		HasText(Lib, TEXT("            Wire::check(&self.state.Id).and_then(|()| check_guid(&self.state.Id)).map_err(|why| unsendable(\"Id\", why))?;\n")));
	TestTrue(TEXT("check_watched checks each watched field by name"),
		HasText(Lib, TEXT("        Wire::check(&state.Owner).and_then(|()| check_name(&state.Owner)).map_err(|why| (\"Owner\", why))?;\n")
			TEXT("        Wire::check(&state.Id).and_then(|()| check_guid(&state.Id)).map_err(|why| (\"Id\", why))?;\n")));
	TestFalse(TEXT("an unwatched field is not checked there"), HasText(Lib, TEXT("(\"Score\", why)")));
	TestTrue(TEXT("a seed is checked after it is decoded"),
		HasText(Lib, TEXT("decode(seed)? };\n        Self::check_watched(&state).map_err(|(field, why)| Error::new(&format!(\"bad_seed: {field}: ")));
	TestTrue(TEXT("a read is refused for a value players cannot read"),
		HasText(Lib, TEXT("                authorize(ctx, &call, true)?;\n                Self::check_watched(&self.state).map_err(unreadable)?;\n                self.read_reply()\n")));
	TestTrue(TEXT("the refusal names the field and says why"),
		HasText(Lib, TEXT("Error::new(&format!(\"unreadable: {field}: the server state holds a value players cannot read: {why}\"))")));
	TestEqual(TEXT("only the seed and the read are checked whole"), CountText(Lib, TEXT("Self::check_watched(")), 2);
	const int32 LoadStart = Lib.Find(TEXT("    fn load("), ESearchCase::CaseSensitive);
	const int32 LoadEnd = Lib.Find(TEXT("    fn persist("), ESearchCase::CaseSensitive);
	TestTrue(TEXT("a load keeps a value players cannot read, so server code can replace it"),
		LoadStart != INDEX_NONE && LoadEnd > LoadStart && !Lib.Mid(LoadStart, LoadEnd - LoadStart).Contains(TEXT("check_watched")));

	Definition->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	Definition->Visibility = ECrowdyServerObjectVisibility::Members;
	FString MembersLib;
	if (!GenerateLib(*this, *Definition, MembersLib))
	{
		return false;
	}
	TestTrue(TEXT("a read with no player is checked"),
		HasText(MembersLib, TEXT("        let Ok(player) = call.player() else {\n            Self::check_watched(&self.state).map_err(unreadable)?;\n")));
	TestTrue(TEXT("a member's read is checked, a non-member's carries no fields and is not"),
		HasText(MembersLib, TEXT("        if member {\n            Self::check_watched(&self.state).map_err(unreadable)?;\n        }\n        self.read_reply(member, member, leader)\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenAliasRemovalsTest, "CrowdySDK.CrowdyExecEditor.CodegenFindRemovalsReadsListAliases", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenAliasRemovalsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const FString Shared = RustTypesHeader
		+ RustStruct(TEXT("TipParams"), {{TEXT("Amount"), TEXT("i32")}, {TEXT("Note"), TEXT("String")}})
		+ TEXT("pub type GiftParams = TipParams;\n");
	const FString Reordered = RustTypesHeader
		+ RustStruct(TEXT("GiftParams"), {{TEXT("Amount"), TEXT("i32")}, {TEXT("Note"), TEXT("String")}})
		+ TEXT("pub type TipParams = GiftParams;\n");
	const FString Unshared = RustTypesHeader
		+ RustStruct(TEXT("TipParams"), {{TEXT("Amount"), TEXT("i32")}, {TEXT("Note"), TEXT("String")}})
		+ RustStruct(TEXT("GiftParams"), {{TEXT("Amount"), TEXT("i32")}});
	const FString Separate = RustTypesHeader
		+ RustStruct(TEXT("TipParams"), {{TEXT("Amount"), TEXT("i32")}, {TEXT("Note"), TEXT("String")}})
		+ RustStruct(TEXT("GiftParams"), {{TEXT("Amount"), TEXT("i32")}, {TEXT("Note"), TEXT("String")}});
	TestEqualSensitive(TEXT("reordering Lists that share a struct reports nothing"), FString::Join(CrowdyExecCodegen::FindRemovals(Shared, Reordered), TEXT(" | ")), FString());
	TestEqualSensitive(TEXT("a List that stops sharing and drops a field reports the field"),
		FString::Join(CrowdyExecCodegen::FindRemovals(Shared, Unshared), TEXT(" | ")), FString(TEXT("GiftParams.Note is gone")));
	TestEqualSensitive(TEXT("a List that starts sharing a struct with the same fields reports nothing"),
		FString::Join(CrowdyExecCodegen::FindRemovals(Separate, Shared), TEXT(" | ")), FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenGridCrateTest, "CrowdySDK.CrowdyExecEditor.CodegenElementBudgetMatchesLocalCheckCrate", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenGridCrateTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	// ck_exec_grid: rows of flags, so a message's lists can be filled to the client's element budget.
	const FPropertyBagPropertyDesc Rows(TEXT("Grid"), FPropertyBagContainerTypes{EPropertyBagContainerType::Array, EPropertyBagContainerType::Array}, EPropertyBagPropertyType::Bool);
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Grid = MakeListDefinition(TEXT("ck_exec_grid"), {});
	AddListValue(Grid->StateList, Rows);
	Grid->WatchedFields = {TEXT("Grid")};
	FCrowdyServerFunction& Store = AddPlainFunction(*Grid, TEXT("Store"), Players);
	Store.ParamsForm = ECrowdyServerValuesForm::List;
	AddListValue(Store.ParamsList, Rows);
	FCrowdyServerFunction& Echo = AddPlainFunction(*Grid, TEXT("Echo"), Players);
	Echo.ParamsForm = ECrowdyServerValuesForm::List;
	AddListValue(Echo.ParamsList, Rows);
	Echo.ReplyForm = ECrowdyServerValuesForm::List;
	AddListValue(Echo.ReplyList, Rows);
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!GenerateCrate(*this, *Grid, Crate))
	{
		return false;
	}
	CheckLocalCheckCrate(*this, Crate, TEXT("ck_exec_grid"));
	const FString Lib = GetFileText(*this, Crate, LibPath);
	TestTrue(TEXT("the budget is the client's"), HasText(Lib, TEXT("\nconst MAX_MESSAGE_ELEMENTS: usize = 65536;\n")));
	TestTrue(TEXT("a list counts itself and what its elements hold"), HasText(Lib, TEXT("        self.len() + self.iter().map(Wire::elements).sum::<usize>()\n")));
	TestTrue(TEXT("a map entry counts once"), HasText(Lib, TEXT("        self.len() + self.values().map(Wire::elements).sum::<usize>()\n")));
	TestTrue(TEXT("a struct counts the fields that hold elements"), HasText(Lib, TEXT("    fn elements(&self) -> usize {\n        self.Grid.elements()\n    }\n")));
	TestTrue(TEXT("a reply over the budget is not sent"),
		HasText(Lib, TEXT("    if reply.elements() > MAX_MESSAGE_ELEMENTS {\n        return Err(unsendable(\"the reply\", \"it holds more than 65536 elements\"));\n")));
	TestTrue(TEXT("a push is refused when the read it implies is over the budget"),
		HasText(Lib, TEXT("        if self.state.Grid.elements() > MAX_MESSAGE_ELEMENTS {\n            return Err(unsendable(\"the watched values\", \"together they hold more than 65536 elements\"));\n")));
	TestTrue(TEXT("a seed or read over the budget is refused"),
		HasText(Lib, TEXT("        if state.Grid.elements() > MAX_MESSAGE_ELEMENTS {\n            return Err((\"the watched values\", \"together they hold more than 65536 elements\"));\n")));

	FString FixtureLib;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Fixture = MakeDefinition(*this, FixtureFunctions());
	if (!Fixture || !GenerateLib(*this, *Fixture, FixtureLib))
	{
		return false;
	}
	TestFalse(TEXT("watched values that hold no elements are not counted"), HasText(FixtureLib, TEXT("elements() > MAX_MESSAGE_ELEMENTS {\n            return Err(unsendable")));
	TestTrue(TEXT("only the fields that can hold elements are counted"), HasText(FixtureLib,
		TEXT("    fn elements(&self) -> usize {\n        self.ByIndex.elements() + self.Phases.elements() + self.Attacks.elements() + self.Notes.elements()\n    }\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenKindsCrateTest, "CrowdySDK.CrowdyExecEditor.CodegenKindChecksMatchLocalCheckCrate", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenKindsCrateTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	// ck_exec_kinds: kinds checked inside a list, an optional and a map, watched and taken and given by Echo.
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Kinds(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
	Kinds->TypeName = TEXT("ck_exec_kinds");
	Kinds->State = FCrowdyExecTestKinds::StaticStruct();
	Kinds->WatchedFields = {TEXT("Names"), TEXT("MaybeId"), TEXT("SeenAt")};
	FCrowdyServerFunction& Echo = AddPlainFunction(*Kinds, TEXT("Echo"), Players);
	Echo.Params = FCrowdyExecTestKinds::StaticStruct();
	Echo.Reply = FCrowdyExecTestKinds::StaticStruct();
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!GenerateCrate(*this, *Kinds, Crate))
	{
		return false;
	}
	CheckLocalCheckCrate(*this, Crate, TEXT("ck_exec_kinds"));
	const FString Lib = GetFileText(*this, Crate, LibPath);
	TestTrue(TEXT("each name of a list is checked"),
		HasText(Lib, TEXT("        Wire::check(&self.Names).and_then(|()| self.Names.iter().try_for_each(|v| check_name(&v)))?;\n")));
	TestTrue(TEXT("an optional GUID is checked when set"),
		HasText(Lib, TEXT("        Wire::check(&self.MaybeId).and_then(|()| self.MaybeId.as_ref().map_or(Ok(()), |v| check_guid(&v)))?;\n")));
	TestTrue(TEXT("a map's name keys and date values are checked, then its keys as Unreal names"),
		HasText(Lib, TEXT("        Wire::check(&self.SeenAt).and_then(|()| self.SeenAt.iter().try_for_each(|(k, v)| check_name(&k).and_then(|()| check_date(&v))).and_then(|()| check_name_keys(self.SeenAt.keys())))?;\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenCrateDefinitionTest, "CrowdySDK.CrowdyExecEditor.CodegenCrateRecordsItsDefinition", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenCrateDefinitionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	UPackage* Package = CreatePackage(TEXT("/Temp/CrowdyExecCodegenTests/DA_Tagged"));
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Tagged(NewObject<UCrowdyServerObjectDefinition>(Package, NAME_None, RF_Transient));
	Tagged->TypeName = TEXT("ck_exec_tagged");
	Tagged->StateForm = ECrowdyServerValuesForm::List;
	Tagged->StateList.AddProperty(TEXT("Gold"), EPropertyBagPropertyType::Int32);
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!GenerateCrate(*this, *Tagged, Crate))
	{
		return false;
	}
	const FString Cargo = GetFileText(*this, Crate, CargoPath);
	TestTrue(TEXT("Cargo.toml names the definition asset"),
		HasText(Cargo, *FString::Printf(TEXT("\n\n[package.metadata.crowdy]\ndefinition = \"%s\"\n\n[lib]\n"), *Tagged->GetPathName())));
	FTempFolder Folder;
	TArray<FString> Written;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("the crate is written (%s)"), *Error), CrowdyExecCodegen::WriteCrate(Crate, Folder.Path, Written, Error)))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the definition reads back"), CrowdyExecCodegen::ReadCrateDefinition(Folder.Path), Tagged->GetPathName());

	FTempFolder Escaped;
	FFileHelper::SaveStringToFile(FString(TEXT("[package]\nname = \"x\"\n\n[package.metadata.crowdy]\ndefinition = \"/Game/A\\\"B\\\\C\\u0009D\"\n")),
		*FPaths::Combine(Escaped.Path, TEXT("Cargo.toml")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	TestEqualSensitive(TEXT("escapes read back"), CrowdyExecCodegen::ReadCrateDefinition(Escaped.Path), FString(TEXT("/Game/A\"B\\C\tD")));
	FTempFolder Missing;
	TestEqualSensitive(TEXT("a folder without Cargo.toml names nothing"), CrowdyExecCodegen::ReadCrateDefinition(Missing.Path), FString());

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Fixture = MakeDefinition(*this, FixtureFunctions());
	CrowdyExecCodegen::FGeneratedCrate FixtureCrate;
	if (!Fixture || !GenerateCrate(*this, *Fixture, FixtureCrate))
	{
		return false;
	}
	TestFalse(TEXT("a definition with no asset records none, so the golden crates stay the same"),
		HasText(GetFileText(*this, FixtureCrate, CargoPath), TEXT("[package.metadata.crowdy]")));
	return true;
}

namespace CrowdyCodegenTest
{
	FString DescribeRenames(const TArray<CrowdyExecCodegen::FListValueRename>& Renames)
	{
		TArray<FString> Lines;
		for (const CrowdyExecCodegen::FListValueRename& Rename : Renames)
		{
			Lines.Add(FString::Printf(TEXT("%s.%s to %s (%s)"), *Rename.Struct, *Rename.OldName, *Rename.NewName, *Rename.ValueId.ToString(EGuidFormats::Digits)));
		}
		return FString::Join(Lines, TEXT(" | "));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenRenamesTest, "CrowdySDK.CrowdyExecEditor.CodegenFindRenamesFollowsListValueIds", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenRenamesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TCHAR* const GoldId = TEXT("0F1E2D3C4B5A69788796A5B4C3D2E1F0");
	const TCHAR* const TypeId = TEXT("11111111222222223333333344444444");
	const TCHAR* const AmountId = TEXT("00112233445566778899AABBCCDDEEFF");
	// GameState is a State List, BuyParams a function List that SellParams shares, Loot a C++ struct, whose fields have no id.
	auto Types = [&](const TCHAR* Gold, const TCHAR* GoldRust, const TCHAR* GoldUnreal, const TCHAR* Kind, const TCHAR* Amount, const TCHAR* LootField, bool bIds)
	{
		auto Id = [bIds](const TCHAR* Value) { return bIds ? FString(TEXT(" id=")) + Value : FString(); };
		return FString(RustTypesHeader)
			+ FString::Printf(TEXT("pub struct GameState {\n    pub %s: %s, // %s%s\n    pub %s: String, // String%s\n}\n\n"), Gold, GoldRust, GoldUnreal, *Id(GoldId), Kind, *Id(TypeId))
			+ FString::Printf(TEXT("pub struct BuyParams {\n    pub %s: i32, // Int32%s\n}\n\npub type SellParams = BuyParams;\n\n"), Amount, *Id(AmountId))
			+ FString::Printf(TEXT("pub struct Loot {\n    pub %s: i32, // Int32\n}\n"), LootField);
	};
	const FString OldWithoutLoot = Types(TEXT("Gold"), TEXT("i32"), TEXT("Int32"), TEXT("r#type"), TEXT("Amount"), TEXT("Coins"), true);
	const FString Renamed = Types(TEXT("Money"), TEXT("i32"), TEXT("Int32"), TEXT("Kind"), TEXT("Count"), TEXT("Gold"), true);
	const FString Expected = FString::Join(TArray<FString>{
		FString::Printf(TEXT("GameState.Gold to Money (%s)"), GoldId),
		FString::Printf(TEXT("GameState.type to Kind (%s)"), TypeId),
		FString::Printf(TEXT("BuyParams.Amount to Count (%s)"), AmountId),
		FString::Printf(TEXT("SellParams.Amount to Count (%s)"), AmountId)}, TEXT(" | "));
	TestEqualSensitive(TEXT("renamed List values are found by id, a keyword by its server name, a shared struct through its alias"),
		DescribeRenames(CrowdyExecCodegen::FindRenames(OldWithoutLoot, Renamed)), Expected);
	TestEqualSensitive(TEXT("generating again with nothing renamed finds no renames"), DescribeRenames(CrowdyExecCodegen::FindRenames(OldWithoutLoot, OldWithoutLoot)), FString());
	TestEqualSensitive(TEXT("a renamed List value is not reported gone; a struct field without an id still is"),
		FString::Join(CrowdyExecCodegen::FindRemovals(OldWithoutLoot, Renamed), TEXT(" | ")), FString(TEXT("Loot.Coins is gone")));

	const FString Widened = Types(TEXT("Money"), TEXT("i64"), TEXT("Int64"), TEXT("Kind"), TEXT("Count"), TEXT("Gold"), true);
	TestTrue(TEXT("a rename with a type change is still a rename"), DescribeRenames(CrowdyExecCodegen::FindRenames(OldWithoutLoot, Widened)).StartsWith(TEXT("GameState.Gold to Money")));
	TestEqualSensitive(TEXT("and its type change is reported"), FString::Join(CrowdyExecCodegen::FindRemovals(OldWithoutLoot, Widened), TEXT(" | ")),
		FString(TEXT("GameState.Gold changes type from Int32 to Int64 | Loot.Coins is gone")));

	const FString Unmarked = Types(TEXT("Gold"), TEXT("i32"), TEXT("Int32"), TEXT("r#type"), TEXT("Amount"), TEXT("Coins"), false);
	TestEqualSensitive(TEXT("a file written before ids finds no renames"), DescribeRenames(CrowdyExecCodegen::FindRenames(Unmarked, Renamed)), FString());
	TestEqualSensitive(TEXT("and reports removals as before"), FString::Join(CrowdyExecCodegen::FindRemovals(Unmarked, Renamed), TEXT(" | ")),
		FString(TEXT("GameState.Gold is gone | GameState.type is gone | BuyParams.Amount is gone | SellParams.Amount is gone | Loot.Coins is gone")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenListIdsTest, "CrowdySDK.CrowdyExecEditor.CodegenListValuesCarryTheirIds", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenListIdsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Before = MakeListDefinition(TEXT("ck_exec_rename"), {TEXT("Gold")});
	FCrowdyServerFunction& Spend = AddPlainFunction(*Before, TEXT("Spend"), Players);
	Spend.ParamsForm = ECrowdyServerValuesForm::List;
	AddListValue(Spend.ParamsList, FPropertyBagPropertyDesc(TEXT("Coins"), EPropertyBagPropertyType::Int32));
	TStrongObjectPtr<UCrowdyServerObjectDefinition> After = MakeListDefinition(TEXT("ck_exec_rename"), {});
	FPropertyBagPropertyDesc Money(TEXT("Money"), EPropertyBagPropertyType::Int32);
	Money.ID = FGuid::NewDeterministicGuid(TEXT("Gold"));
	After->StateList.AddProperties({Money});
	FCrowdyServerFunction& Spent = AddPlainFunction(*After, TEXT("Spend"), Players);
	Spent.ParamsForm = ECrowdyServerValuesForm::List;
	FPropertyBagPropertyDesc Price(TEXT("Price"), EPropertyBagPropertyType::Int32);
	Price.ID = FGuid::NewDeterministicGuid(TEXT("Coins"));
	Spent.ParamsList.AddProperties({Price});
	CrowdyExecCodegen::FGeneratedCrate Old;
	CrowdyExecCodegen::FGeneratedCrate New;
	if (!GenerateCrate(*this, *Before, Old) || !GenerateCrate(*this, *After, New))
	{
		return false;
	}
	const FString OldTypes = GetFileText(*this, Old, TypesPath);
	const FString NewTypes = GetFileText(*this, New, TypesPath);
	TestTrue(TEXT("a State List value carries its id"), HasText(OldTypes, *FString::Printf(TEXT("    pub Gold: i32, // Int32 id=%s\n"), *ValueIdText(TEXT("Gold")))));
	TestEqualSensitive(TEXT("renaming a State List value and a function List value is found"), DescribeRenames(CrowdyExecCodegen::FindRenames(OldTypes, NewTypes)),
		FString::Printf(TEXT("CkExecRenameState.Gold to Money (%s) | SpendParams.Coins to Price (%s)"), *ValueIdText(TEXT("Gold")), *ValueIdText(TEXT("Coins"))));
	TestEqualSensitive(TEXT("and nothing is reported lost"), FString::Join(CrowdyExecCodegen::FindRemovals(OldTypes, NewTypes), TEXT(" | ")), FString());

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Fixture = MakeDefinition(*this, FixtureFunctions());
	CrowdyExecCodegen::FGeneratedCrate FixtureCrate;
	if (!Fixture || !GenerateCrate(*this, *Fixture, FixtureCrate))
	{
		return false;
	}
	TestFalse(TEXT("a C++ struct's fields carry no id"), HasText(GetFileText(*this, FixtureCrate, TypesPath), TEXT(" id=")));
	return true;
}

namespace CrowdyCodegenTest
{
	struct FTradeStart
	{
		const TCHAR* Function;
		int32 Value;
	};

	/** ck_exec_trade: Buy, Sell and Give each send one Int32 named ValueName under the id of Amount, so their Lists share one struct, starting at the given numbers. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeTradeDefinition(const TCHAR* ValueName, int32 Buy, int32 Sell, int32 Give)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Trade = MakeListDefinition(TEXT("ck_exec_trade"), {TEXT("Gold")});
		const FTradeStart Starts[] = {{TEXT("Buy"), Buy}, {TEXT("Sell"), Sell}, {TEXT("Give"), Give}};
		for (const FTradeStart& Start : Starts)
		{
			FCrowdyServerFunction& Function = AddPlainFunction(*Trade, Start.Function, Players);
			Function.ParamsForm = ECrowdyServerValuesForm::List;
			FPropertyBagPropertyDesc Desc(ValueName, EPropertyBagPropertyType::Int32);
			Desc.ID = FGuid::NewDeterministicGuid(TEXT("Amount"));
			Function.ParamsList.AddProperties({Desc});
			Function.ParamsList.SetValueInt32(ValueName, Start.Value);
		}
		return Trade;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenListOwnDefaultsTest, "CrowdySDK.CrowdyExecEditor.CodegenListKeepsItsOwnDefaults", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenListOwnDefaultsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Trade = MakeTradeDefinition(TEXT("Amount"), 1, 2, 1);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Shared = MakeTradeDefinition(TEXT("Amount"), 1, 1, 1);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Renamed = MakeTradeDefinition(TEXT("Price"), 1, 2, 1);
	CrowdyExecCodegen::FGeneratedCrate Crate;
	CrowdyExecCodegen::FGeneratedCrate SharedCrate;
	CrowdyExecCodegen::FGeneratedCrate RenamedCrate;
	if (!GenerateCrate(*this, *Trade, Crate) || !GenerateCrate(*this, *Shared, SharedCrate) || !GenerateCrate(*this, *Renamed, RenamedCrate))
	{
		return false;
	}
	const FString Types = GetFileText(*this, Crate, TypesPath);
	const FString Lib = GetFileText(*this, Crate, LibPath);
	const FString Id = ValueIdText(TEXT("Amount"));
	const FString Fields = FString::Printf(TEXT(" {\n    pub Amount: i32, // Int32 id=%s\n}\n"), *Id);
	TestTrue(TEXT("the first List names the shared struct"), HasText(Types, *(TEXT("#[serde(default)]\npub struct BuyParams") + Fields)));
	TestTrue(TEXT("which starts with its values"), HasText(Types, TEXT("impl Default for BuyParams {\n    fn default() -> Self {\n        Self {\n            Amount: 1,\n")));
	TestTrue(TEXT("a List of that shape starting with other values is a struct of its own with the same fields"),
		HasText(Types, *(TEXT("#[serde(default)]\npub struct SellParams") + Fields)));
	TestTrue(TEXT("which starts with its own values"), HasText(Types, TEXT("impl Default for SellParams {\n    fn default() -> Self {\n        Self {\n            Amount: 2,\n")));
	TestFalse(TEXT("so it is no alias"), HasText(Types, TEXT("pub type SellParams")));
	TestTrue(TEXT("a List starting with the shared struct's values stays an alias"), HasText(Types, TEXT("pub type GiveParams = BuyParams;\n")));
	TestEqual(TEXT("the shared struct is checked once"), CountText(Lib, TEXT("impl Wire for BuyParams {\n")), 1);
	TestEqual(TEXT("the struct of its own is checked as the shared one is"),
		CountText(Lib, TEXT("impl Wire for SellParams {\n    fn check(&self) -> Checked {\n        Wire::check(&self.Amount)?;\n        Ok(())\n    }\n}\n")), 1);
	TestFalse(TEXT("an alias is checked through its struct"), HasText(Lib, TEXT("impl Wire for GiveParams")));
	TestTrue(TEXT("the function takes the struct of its own"), HasText(Lib, TEXT("fn sell(&mut self, ctx: &Ctx, call: &Call<'_>, params: SellParams) -> Result<()>;")));
	TestTrue(TEXT("and its dispatch decodes it"), HasText(Lib, TEXT("let params: SellParams = call.decode().map_err(bad_params)?;")));

	const FString SharedTypes = GetFileText(*this, SharedCrate, TypesPath);
	TestTrue(TEXT("with the same starting values every List shares the struct"), HasText(SharedTypes, TEXT("pub type SellParams = BuyParams;\n")));
	TestEqualSensitive(TEXT("a List that becomes a struct of its own loses nothing"), FString::Join(CrowdyExecCodegen::FindRemovals(SharedTypes, Types), TEXT(" | ")), FString());
	TestEqualSensitive(TEXT("nor does one that becomes an alias again"), FString::Join(CrowdyExecCodegen::FindRemovals(Types, SharedTypes), TEXT(" | ")), FString());
	TestEqualSensitive(TEXT("a List becoming a struct of its own renames nothing"), DescribeRenames(CrowdyExecCodegen::FindRenames(SharedTypes, Types)), FString());
	TestEqualSensitive(TEXT("nor does one becoming an alias again"), DescribeRenames(CrowdyExecCodegen::FindRenames(Types, SharedTypes)), FString());
	TestEqualSensitive(TEXT("a value renamed as its List becomes a struct of its own is found under each List's name"),
		DescribeRenames(CrowdyExecCodegen::FindRenames(SharedTypes, GetFileText(*this, RenamedCrate, TypesPath))),
		FString::Printf(TEXT("BuyParams.Amount to Price (%s) | SellParams.Amount to Price (%s) | GiveParams.Amount to Price (%s)"), *Id, *Id, *Id));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenTypeIdentityTest, "CrowdySDK.CrowdyExecEditor.CodegenFindsListsThatChangeType", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenTypeIdentityTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Trade = MakeTradeDefinition(TEXT("Amount"), 1, 2, 1);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Shared = MakeTradeDefinition(TEXT("Amount"), 1, 1, 1);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Renamed = MakeTradeDefinition(TEXT("Price"), 1, 2, 1);
	CrowdyExecCodegen::FGeneratedCrate Crate;
	CrowdyExecCodegen::FGeneratedCrate SharedCrate;
	CrowdyExecCodegen::FGeneratedCrate RenamedCrate;
	if (!GenerateCrate(*this, *Trade, Crate) || !GenerateCrate(*this, *Shared, SharedCrate) || !GenerateCrate(*this, *Renamed, RenamedCrate))
	{
		return false;
	}
	const FString Types = GetFileText(*this, Crate, TypesPath);
	const FString SharedTypes = GetFileText(*this, SharedCrate, TypesPath);
	const auto Changes = [](const FString& Old, const FString& New) { return FString::Join(CrowdyExecCodegen::FindTypeIdentityChanges(Old, New), TEXT(" | ")); };
	TestEqualSensitive(TEXT("an alias that starts with other values becomes a struct of its own"), Changes(SharedTypes, Types),
		FString(TEXT("SellParams is now its own struct, since its starting values differ from those of BuyParams; code that uses one for the other no longer builds.")));
	TestEqualSensitive(TEXT("a struct of its own that starts with the shared values again becomes an alias"), Changes(Types, SharedTypes),
		FString(TEXT("SellParams is now another name for BuyParams, since they now hold the same values with the same starting values; code that treats them as two types no longer builds.")));
	TestEqualSensitive(TEXT("generating again changes nothing"), Changes(Types, Types), FString());
	TestEqualSensitive(TEXT("nor does renaming a value every List shares"), Changes(Types, GetFileText(*this, RenamedCrate, TypesPath)), FString());

	const FString Aliased = TEXT("pub struct A {\n    pub X: i32, // Int32\n}\n\npub type B = A;\n");
	const FString Reshaped = TEXT("pub struct A {\n    pub X: i32, // Int32\n}\n\npub struct B {\n    pub Y: i32, // Int32\n}\n");
	TestEqualSensitive(TEXT("an alias whose values no longer match its struct's becomes a struct of its own, with no word about starting values"), Changes(Aliased, Reshaped),
		FString(TEXT("B is now its own struct rather than another name for A; code that uses one for the other no longer builds.")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenListDefaultsErrorTest, "CrowdySDK.CrowdyExecEditor.CodegenListDefaultsErrorNamesTheList", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenListDefaultsErrorTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Jar = MakeListDefinition(TEXT("ck_exec_ratio"), {});
	AddListValue(Jar->StateList, FPropertyBagPropertyDesc(TEXT("Ratio"), EPropertyBagPropertyType::Float));
	FCrowdyServerFunction& Tip = AddPlainFunction(*Jar, TEXT("Tip"), Players);
	Tip.ReplyForm = ECrowdyServerValuesForm::List;
	Tip.ReplyList = Jar->StateList;
	Tip.ReplyList.SetValueFloat(TEXT("Ratio"), std::numeric_limits<float>::infinity());
	TArray<FString> Errors;
	const bool bBaked = Jar->Bake(Errors);
	if (!TestTrue(FString::Printf(TEXT("the definition bakes (%s)"), *FString::Join(Errors, TEXT("; "))), bBaked))
	{
		return false;
	}
	CrowdyExecCodegen::FGeneratedCrate Crate;
	FString Error;
	TestFalse(TEXT("a reply List starting at infinity cannot be written as server code"), CrowdyExecCodegen::Generate(*Jar, Crate, Error));
	TestTrue(FString::Printf(TEXT("and the refusal names the function and its List (%s)"), *Error), Error.StartsWith(TEXT("Server Function Tip: TipReply: "), ESearchCase::CaseSensitive));
	return true;
}

namespace CrowdyCodegenTest
{
	FString DescribeGaps(const CrowdyExecCodegen::FLogicGaps& Gaps)
	{
		TArray<FString> Missing;
		for (const CrowdyExecCodegen::FLogicStub& Stub : Gaps.Missing)
		{
			Missing.Add(Stub.Method);
		}
		return FString::Printf(TEXT("missing %s | leftover %s"), *FString::Join(Missing, TEXT(",")), *FString::Join(Gaps.Leftover, TEXT(",")));
	}

	/** Hit and Heal, the End Match timer and On Player Joined: four functions the trait asks logic.rs for. */
	bool GenerateGapsCrate(FAutomationTestBase& Test, TStrongObjectPtr<UCrowdyServerObjectDefinition>& OutDefinition, CrowdyExecCodegen::FGeneratedCrate& OutCrate)
	{
		OutDefinition = MakeDefinition(Test, {HitFunction, HealFunction});
		if (!OutDefinition)
		{
			return false;
		}
		AddTimer(*OutDefinition, TEXT("EndMatch"), ECrowdyServerTimerRepeat::After, 10.f, false);
		OutDefinition->bOnPlayerJoined = true;
		return GenerateCrate(Test, *OutDefinition, OutCrate);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenLogicGapsTest, "CrowdySDK.CrowdyExecEditor.CodegenLogicGapsSeeOnlyCode", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenLogicGapsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition;
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!GenerateGapsCrate(*this, Definition, Crate))
	{
		return false;
	}
	const FString Generated = GetFileText(*this, Crate, LogicPath);
	const CrowdyExecCodegen::FLogicGaps None = CrowdyExecCodegen::FindLogicGaps(Crate, Generated);
	TestEqualSensitive(TEXT("the logic.rs Generate writes lacks nothing"), DescribeGaps(None), FString(TEXT("missing  | leftover ")));
	TestEqual(TEXT("and its block closes at the file's last brace"), None.InsertAt, Generated.Len() - 2);

	const FString Written = FString::Printf(TEXT(
		"use crate::*;\n"
		"/* a comment /* nested } */ fn heal( */\n"
		"impl crate::Functions for %s {\n"
		"    // fn end_match(&mut self) {\n"
		"    fn hit(&mut self, ctx: &Ctx, call: &Call<'_>, params: CrowdyExecTestHit) -> Result<()> {\n"
		"        fn on_player_joined() {}\n"
		"        let _ = (\"} fn heal(\", '{', '}', '\\'', r#\"} \"fn heal(\"#, b'}');\n"
		"        'outer: loop { break 'outer; }\n"
		"        Ok(())\n"
		"    }\n"
		"    fn join(&mut self, ctx: &Ctx, call: &Call<'_>) -> Result<()> {\n"
		"        Ok(())\n"
		"    }\n"
		"    fn on_timer(&mut self, ctx: &Ctx, name: &str) -> Result<()> {\n"
		"        Ok(())\n"
		"    }\n"
		"}\n"
		"fn heal() {}\n"), *Crate.StateName);
	const CrowdyExecCodegen::FLogicGaps Gaps = CrowdyExecCodegen::FindLogicGaps(Crate, Written);
	TestEqualSensitive(TEXT("comments, strings, characters, nested and outside functions define nothing; a function the type lost is left over"),
		DescribeGaps(Gaps), FString(TEXT("missing heal,end_match,on_player_joined | leftover join")));
	TestEqual(TEXT("the stubs go before the block's own closing brace"), Gaps.InsertAt, Written.Find(TEXT("}\nfn heal() {}")));

	const FString Added = CrowdyExecCodegen::AddLogicStubs(Written, Gaps);
	TestEqualSensitive(TEXT("once added nothing is missing, and the leftover function stays"), DescribeGaps(CrowdyExecCodegen::FindLogicGaps(Crate, Added)), FString(TEXT("missing  | leftover join")));
	const FString HealStub = TEXT("    fn heal(&mut self, ctx: &Ctx, call: &Call<'_>) -> Result<()> {\n        Err(Error::new(\"heal is not written yet\"))\n    }\n");
	const FString Tail = TEXT("    fn on_player_joined(&mut self, ctx: &Ctx, player: u64) -> Result<()> {\n        Ok(())\n    }\n}\nfn heal() {}\n");
	TestTrue(TEXT("a function's stub fails until written, after a blank line"), HasText(Added, *(TEXT("    }\n\n") + HealStub + TEXT("\n    fn end_match("))));
	TestTrue(TEXT("an event's stub succeeds, and the block closes after it"), Added.EndsWith(Tail, ESearchCase::CaseSensitive));
	TestTrue(TEXT("everything else stays as written"), Added.StartsWith(Written.Left(Gaps.InsertAt), ESearchCase::CaseSensitive) && Added.EndsWith(Written.Mid(Gaps.InsertAt), ESearchCase::CaseSensitive));
	TestEqualSensitive(TEXT("with nothing missing nothing changes"), CrowdyExecCodegen::AddLogicStubs(Added, CrowdyExecCodegen::FindLogicGaps(Crate, Added)), Added);

	const int32 Impl = Generated.Find(TEXT("impl Functions for "));
	const FString Head = Generated.Left(Impl) + TEXT("impl Functions for ") + Crate.StateName;
	for (const TCHAR* Empty : {TEXT(" {}\n"), TEXT(" {\n}\n")})
	{
		const FString Bare = Head + Empty;
		TestEqualSensitive(FString::Printf(TEXT("an empty block%s gets the stubs as Generate writes them"), Empty[2] == '\n' ? TEXT(" on two lines") : TEXT("")),
			CrowdyExecCodegen::AddLogicStubs(Bare, CrowdyExecCodegen::FindLogicGaps(Crate, Bare)), Generated);
	}

	const FString NoBlock = FString::Printf(TEXT("impl Other for %s {\n    fn hit() {}\n}\nfn heal() {}\n"), *Crate.StateName);
	const CrowdyExecCodegen::FLogicGaps Loose = CrowdyExecCodegen::FindLogicGaps(Crate, NoBlock);
	TestEqual(TEXT("without impl Functions there is nowhere to add stubs"), Loose.InsertAt, INDEX_NONE);
	TestEqualSensitive(TEXT("and any function of the file counts as written, none as left over"), DescribeGaps(Loose), FString(TEXT("missing end_match,on_player_joined | leftover ")));
	TestEqualSensitive(TEXT("so nothing is added"), CrowdyExecCodegen::AddLogicStubs(NoBlock, Loose), NoBlock);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenLogicGapsRustFormsTest, "CrowdySDK.CrowdyExecEditor.CodegenLogicGapsReadRustForms", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenLogicGapsRustFormsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition;
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!GenerateGapsCrate(*this, Definition, Crate))
	{
		return false;
	}
	const FString Written = FString::Printf(TEXT(
		"unsafe impl ::crate::Functions for %s where Self: Sized {\n"
		"    fn hit(&mut self, ctx: &Ctx, call: &Call<'_>, params: CrowdyExecTestHit) -> Result<()> {\n"
		"        let _ = (r##\"} \"# fn heal(\"##, '\\u{7D}', '\U0001F600', br\"}\", cr#\"} fn heal(\"#);\n"
		"        Ok(())\n"
		"    }\n"
		"    fn r#end_match(&mut self, _ctx: &Ctx) -> Result<()> { Ok(()) }\n"
		"}\n"), *Crate.StateName);
	const CrowdyExecCodegen::FLogicGaps Gaps = CrowdyExecCodegen::FindLogicGaps(Crate, Written);
	TestEqualSensitive(TEXT("hashed raw strings, escaped and paired characters hide their braces; a raw identifier counts"),
		DescribeGaps(Gaps), FString(TEXT("missing heal,on_player_joined | leftover ")));
	TestEqual(TEXT("an unsafe impl with a leading :: and a where clause is found"), Gaps.InsertAt, Written.Len() - 2);

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Keyword = MakeDefinition(*this, {HealFunction});
	CrowdyExecCodegen::FGeneratedCrate KeywordCrate;
	if (!Keyword)
	{
		return false;
	}
	AddTimer(*Keyword, TEXT("Match"), ECrowdyServerTimerRepeat::Every, 1.f, true);
	if (!GenerateCrate(*this, *Keyword, KeywordCrate))
	{
		return false;
	}
	const FString Bare = FString::Printf(TEXT("impl Functions for %s {\n    fn heal(&mut self, ctx: &Ctx, call: &Call<'_>) -> Result<()> { Ok(()) }\n}\n"), *KeywordCrate.StateName);
	const CrowdyExecCodegen::FLogicGaps Raw = CrowdyExecCodegen::FindLogicGaps(KeywordCrate, Bare);
	TestEqualSensitive(TEXT("a timer named Match is missing as its raw function"), DescribeGaps(Raw), FString(TEXT("missing r#match | leftover ")));
	const FString Added = CrowdyExecCodegen::AddLogicStubs(Bare, Raw);
	TestTrue(TEXT("and is added by that name"), HasText(Added, TEXT("    fn r#match(&mut self, ctx: &Ctx) -> Result<()> {\n        Ok(())\n    }\n")));
	TestEqualSensitive(TEXT("after which nothing is missing"), DescribeGaps(CrowdyExecCodegen::FindLogicGaps(KeywordCrate, Added)), FString(TEXT("missing  | leftover ")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenTimerConstantsTest, "CrowdySDK.CrowdyExecEditor.CodegenTimerNamesAreConstants", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenTimerConstantsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, {HealFunction});
	if (!Definition)
	{
		return false;
	}
	AddTimer(*Definition, TEXT("EndMatch"), ECrowdyServerTimerRepeat::After, 10.f, false);
	AddTimer(*Definition, TEXT("Match"), ECrowdyServerTimerRepeat::Every, 1.f, true);
	FString Lib;
	if (!GenerateLib(*this, *Definition, Lib))
	{
		return false;
	}
	TestTrue(TEXT("each timer's Name is a constant named after its function"),
		HasText(Lib, TEXT("    pub const END_MATCH: &str = \"EndMatch\";\n    pub const MATCH: &str = \"Match\";\n\n    thread_local! {\n")));

	AddTimer(*Definition, TEXT("Queued"), ECrowdyServerTimerRepeat::Every, 1.f, true);
	TArray<FString> Errors;
	CrowdyExecCodegen::FGeneratedCrate Crate;
	FString Error;
	TestTrue(TEXT("a timer named Queued bakes"), Definition->Bake(Errors));
	TestFalse(TEXT("but its constant would be the module's own static"), CrowdyExecCodegen::Generate(*Definition, Crate, Error));
	TestEqualSensitive(TEXT("and the refusal says so"), Error, FString(TEXT("Timer Queued: the server code (Rust) uses timers::QUEUED itself; rename the timer")));
	return true;
}

#endif
