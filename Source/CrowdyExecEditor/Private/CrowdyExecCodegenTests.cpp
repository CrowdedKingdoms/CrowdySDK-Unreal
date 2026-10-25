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

	/** ck_exec_rules: members kept by the object, a ranged and cooled-down input, a function needing a feature, timers, both events and Can Call. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeRulesDefinition(UCrowdyServerObjectDefinition& Bank)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Rules = MakeListDefinition(TEXT("ck_exec_rules"), {TEXT("Gold"), TEXT("Secret")});
		Rules->WatchedFields = {TEXT("Gold")};
		Rules->MembersFrom = ECrowdyServerMembersSource::ThisObject;
		Rules->MaxMembers = 5;
		AddTimer(*Rules, TEXT("EndMatch"), ECrowdyServerTimerRepeat::After, 10.f, false);
		AddTimer(*Rules, TEXT("Tick"), ECrowdyServerTimerRepeat::Every, 5.f, true);
		AddTimer(*Rules, TEXT("Restock"), ECrowdyServerTimerRepeat::Schedule, 0.f, true);
		Rules->Timers.Last().Cron = TEXT("0 */6 * * *");
		Rules->Timers.Last().UtcOffsetMinutes = 60;
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
		FCrowdyServerFunction& Claim = AddPlainFunction(*Rules, TEXT("Claim"), ECrowdyServerFunctionCaller::Members);
		Claim.RequiredFeature = TEXT("vip");
		Claim.CooldownSeconds = 3.f;
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

	Definition->Functions[0].WhoCanCall = ECrowdyServerFunctionCaller::DevelopersOnly;
	CrowdyExecCodegen::FGeneratedCrate DevelopersOnlyHit;
	if (!GenerateCrate(*this, *Definition, DevelopersOnlyHit))
	{
		return false;
	}
	const FString DevelopersLib = GetFileText(*this, DevelopersOnlyHit, LibPath);
	TestTrue(TEXT("a Developers only function lets in developer tools alone"), HasText(DevelopersLib,
		TEXT("            \"hit\" => {\n                call.developer().map_err(|_| Error::new(\"denied: only developer tools may call this function\"))?;\n")));
	TestFalse(TEXT("and no longer runs the player check"), HasText(DevelopersLib, HitAllowed) || HasText(DevelopersLib, HitRefused));
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
		HasText(Lib, TEXT("members::remove(player);\n        self.finish(ctx, before, Ok(()))?;\n        // The removal stands even when on_player_left fails.\n")));
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
	TestTrue(TEXT("server code starts only the definition's timers"), HasText(Lib, TEXT("if !matches!(name, \"EndMatch\" | \"Tick\" | \"Restock\") || !queue(name, true) {")));
	TestTrue(TEXT("server code names them by constant"), HasText(Lib, TEXT("    pub const END_MATCH: &str = \"EndMatch\";\n    pub const TICK: &str = \"Tick\";\n    pub const RESTOCK: &str = \"Restock\";\n")));
	TestTrue(TEXT("a schedule arms its next run from its cron and offset"), HasText(Lib, TEXT("\"Restock\" => ctx.timer_cron(\"Restock\", &ckx_sdk::cron::Cron::parse(\"0 */6 * * *\")?.with_utc_offset(60)?),")));
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
	TestTrue(TEXT("Remove Members Who Leave removes the leaver"), HasText(MembersOnly, TEXT("members::remove(player);\n        self.finish(ctx, before, Ok(()))\n")));
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
	// FCrowdyServerTimer's Seconds carries meta=(ClampMin = 0.01) and UtcOffsetMinutes ClampMin and ClampMax.
	FCrowdyServerFunction& Arm = AddPlainFunction(*Definition, TEXT("Arm"), Players);
	Arm.Params = FCrowdyServerTimer::StaticStruct();
	FString Lib;
	if (!GenerateLib(*this, *Definition, Lib))
	{
		return false;
	}
	TestTrue(TEXT("a C++ input's ClampMin is checked"), HasText(Lib, TEXT("                if params.Seconds < 0.01 {\n                    return Err(Error::new(\"bad_params: Seconds must be at least 0.01\"));\n")));
	TestTrue(TEXT("a C++ input's ClampMin and ClampMax are checked together"), HasText(Lib, TEXT("bad_params: UtcOffsetMinutes must be")));
	TestEqual(TEXT("only the ranged inputs are checked"), CountText(Lib, TEXT("return Err(Error::new(\"bad_params: ")), 2);
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
	const FString LoadWarning = TEXT("let _ = ctx.log(Level::Warn, &format!(\"timer {name} keeps any old schedule: the changed one could not be armed: {error}\"));");
	TestTrue(TEXT("a load that cannot arm a changed timer logs it"), HasText(Timed, *LoadWarning));
	TestTrue(TEXT("a load asks which timers are pending and never cancels one to re-arm it"),
		HasText(Timed, TEXT("        let pending: BTreeSet<String> = ctx.timers().into_iter().map(|(name, _)| name).collect();\n"))
		&& HasText(Timed, TEXT("                Some(old) => old.as_str() != kind && pending.contains(name),\n                None => pending.contains(name) || automatic,\n"))
		&& !HasText(Timed, TEXT("&& ctx.cancel_timer(name)")) && !HasText(Timed, TEXT("ctx.cancel_timer(name) || automatic")));
	TestFalse(TEXT("a timer that cannot start is not only logged"), HasText(Timed.Replace(*LoadWarning, TEXT("")), TEXT("Level::Warn")));
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
	// ck_exec_kinds: kinds checked inside a list, a set, an optional and a map, watched and taken and given by Echo.
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Kinds(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
	Kinds->TypeName = TEXT("ck_exec_kinds");
	Kinds->State = FCrowdyExecTestKinds::StaticStruct();
	Kinds->WatchedFields = {TEXT("Names"), TEXT("MaybeId"), TEXT("SeenAt"), TEXT("Tags")};
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
	TestTrue(TEXT("a set's names are checked one by one, then as Unreal names"),
		HasText(Lib, TEXT("        Wire::check(&self.Tags).and_then(|()| self.Tags.iter().try_for_each(|v| check_name(&v)).and_then(|()| check_name_keys(self.Tags.iter())))?;\n")));
	return true;
}

namespace CrowdyCodegenTest
{
	/** ck_exec_records: a record per entity with Health, Label and Rows for everyone, Gold for its owner, Ammo for its team and a server-only Secret; Hit for players and a Regen timer. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeRecordsDefinition()
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Records(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Records->TypeName = TEXT("ck_exec_records");
		Records->Holds = ECrowdyServerObjectHolds::RecordPerEntity;
		Records->State = FCrowdyExecTestRecord::StaticStruct();
		Records->WatchedFields = {TEXT("Health"), TEXT("Gold"), TEXT("Ammo"), TEXT("Label"), TEXT("Rows")};
		Records->OwnerFields = {TEXT("Gold")};
		Records->TeamFields = {TEXT("Ammo")};
		Records->MaxRecords = 8;
		Records->PushIntervalMs = 250;
		FCrowdyServerFunction& Hit = AddPlainFunction(*Records, TEXT("Hit"), Players);
		Hit.Params = FCrowdyExecTestRecordHit::StaticStruct();
		AddTimer(*Records, TEXT("Regen"), ECrowdyServerTimerRepeat::Every, 5.f, true);
		return Records;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenRecordsCrateTest, "CrowdySDK.CrowdyExecEditor.CodegenRecordsMatchLocalCheckCrate", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenRecordsCrateTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Records = MakeRecordsDefinition();
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!GenerateCrate(*this, *Records, Crate))
	{
		return false;
	}
	CheckLocalCheckCrate(*this, Crate, TEXT("ck_exec_records"));
	const FString Lib = GetFileText(*this, Crate, LibPath);
	const FString Logic = GetFileText(*this, Crate, LogicPath);
	TestEqualSensitive(TEXT("logic.rs implements Functions on the records"), Crate.StateName, FString(TEXT("Records")));
	TestTrue(TEXT("and the stub says so"), HasText(Logic, TEXT("impl Functions for Records {\n")));
	TestEqualSensitive(TEXT("the stub lacks nothing"), FString::Join(CrowdyExecCodegen::FindLogicGaps(Crate, Logic).Leftover, TEXT(",")), FString());
	TestEqual(TEXT("nor misses anything"), CrowdyExecCodegen::FindLogicGaps(Crate, Logic).Missing.Num(), 0);
	TestTrue(TEXT("a records type has its own contract"), HasText(Lib, TEXT("\nconst CONTRACT: u64 = 2;\n")));
	TestTrue(TEXT("Max Records reaches the glue"), HasText(Lib, TEXT("\nconst MAX_RECORDS: usize = 8;\n")));
	TestTrue(TEXT("and Send Changes Every"), HasText(Lib, TEXT("\nconst PUSH_INTERVAL_MS: u64 = 250;\n")));
	TestTrue(TEXT("the send has its reserved timer"), HasText(Lib, TEXT("\nconst FLUSH_TIMER: &str = \"crowdy_flush\";\n")));
	TestTrue(TEXT("which runs before the definition's timers"),
		HasText(Lib, TEXT("            if name == FLUSH_TIMER {\n                return self.flush(ctx);\n            }\n            timers::open();\n")));
	TestTrue(TEXT("and which logic.rs cannot stop"), HasText(Lib, TEXT("    pub fn stop(ctx: &Ctx, name: &str) -> bool {\n        // The send's timer belongs to the glue, so a stop of it is ignored.\n        if name == FLUSH_TIMER {\n            return false;\n        }\n")));
	TestTrue(TEXT("the records live in a module logic.rs reaches only through their pub functions"),
		HasText(Lib, TEXT("\npub use records::{Records, ServerObject};\n")) && HasText(Lib, TEXT("\nmod records {\n    use super::*;\n")));
	TestFalse(TEXT("no dispatch copies the whole state"), HasText(Lib, TEXT("Clone::clone(&self.state)")));
	TestTrue(TEXT("a function gets the records and ends through the journal"),
		HasText(Lib, TEXT("let result = Functions::hit(&mut self.records, ctx, &call, params).map(|()| vec![0xc0]);\n                    self.finish(ctx, result)\n")));
	TestTrue(TEXT("a push carries the Everyone values"), HasText(Lib, TEXT("            put(out, elements, \"Health\", &now.state.Health)?;\n"))
		&& HasText(Lib, TEXT("            put(out, elements, \"Label\", &now.state.Label)?;\n")) && HasText(Lib, TEXT("            put(out, elements, \"Rows\", &now.state.Rows)?;\n")));
	TestFalse(TEXT("but never an Owner Only, Team Only or server-only one"),
		HasText(Lib, TEXT("&now.state.Gold)")) || HasText(Lib, TEXT("&now.state.Ammo)")) || HasText(Lib, TEXT("\"Secret\"")));
	TestTrue(TEXT("a read gives Gold only to its owner"), HasText(Lib, TEXT("        if owner {\n            put(out, elements, \"Gold\", &entry.state.Gold)?;\n        }\n")));
	TestTrue(TEXT("and Ammo only to its team"), HasText(Lib, TEXT("        if team {\n            put(out, elements, \"Ammo\", &entry.state.Ammo)?;\n        }\n")));
	TestTrue(TEXT("a record over a message's element budget is refused"), HasText(Lib, TEXT("if elements > MAX_MESSAGE_ELEMENTS {\n")));
	TestTrue(TEXT("a load arms the send that announces its epoch"), HasText(Lib, TEXT("match ctx.timer_after(FLUSH_TIMER, PUSH_INTERVAL_MS) {\n")));
	TestTrue(TEXT("a change to either raises the restricted seq"),
		HasText(Lib, TEXT("before.owner != now.owner || before.team != now.team || before.state.Gold != now.state.Gold || before.state.Ammo != now.state.Ammo")));
	TestTrue(TEXT("a seed is refused"), HasText(Lib, TEXT("bad_seed: a Server Object holding a record per entity starts with no records")));
	TestTrue(TEXT("a push carries the restricted seq"), HasText(Lib, TEXT("            write_map(&mut out, 6);\n            write_str(&mut out, \"epoch\");\n"))
		&& HasText(Lib, TEXT("            write_str(&mut out, \"rseq\");\n            write_uint(&mut out, self.rseq);\n            write_str(&mut out, \"records\");\n            write_map(&mut out, count);\n            out.extend_from_slice(records);\n")));
	TestTrue(TEXT("a team is the team of a record the viewer owns here"), HasText(Lib, TEXT("let team = self.records.owned_by(player).next()")));
	TestFalse(TEXT("so the platform is never asked about teams"), HasText(Lib, TEXT("check_permission")));
	TestTrue(TEXT("a Label or Rows can be refused or large, so a touched record is encoded and measured"), HasText(Lib, TEXT("read_record(&mut scratch, &mut elements, id, entry, true, true)?;")));
	TestFalse(TEXT("changes do not gather every changed id before paging"), HasText(Lib, TEXT("let changed: BTreeSet<&str>")));

	// Each change below follows a good bake, so the generator's own refusal is what is seen.
	FString Error;
	CrowdyExecCodegen::FGeneratedCrate Refused;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Flush = MakeRecordsDefinition();
	if (!BakeDefinition(*this, *Flush))
	{
		return false;
	}
	AddTimer(*Flush, TEXT("crowdy_flush"), ECrowdyServerTimerRepeat::Every, 1.f, true);
	TestFalse(TEXT("a timer named crowdy_flush does not generate"), CrowdyExecCodegen::Generate(*Flush, Refused, Error));
	TestEqualSensitive(TEXT("and the refusal names it"), Error, FString(TEXT("Timer crowdy_flush: 'crowdy_flush' is reserved in the server code (Rust); rename the timer")));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Members = MakeRecordsDefinition();
	if (!BakeDefinition(*this, *Members))
	{
		return false;
	}
	Members->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	TestFalse(TEXT("members do not generate for records"), CrowdyExecCodegen::Generate(*Members, Refused, Error));
	TestEqualSensitive(TEXT("and the refusal says why"), Error, FString(TEXT("A Record per Entity cannot use Members From; each record carries its own owner and team")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenRecordsCrowdyTeamCrateTest, "CrowdySDK.CrowdyExecEditor.CodegenRecordsOnCrowdyTeamsMatchLocalCheckCrate", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenRecordsCrowdyTeamCrateTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	// ck_exec_records_team: the same records whose teams are Crowdy Teams, watching only Health, Gold and Ammo.
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Records = MakeRecordsDefinition();
	Records->TypeName = TEXT("ck_exec_records_team");
	Records->WatchedFields = {TEXT("Health"), TEXT("Gold"), TEXT("Ammo")};
	Records->TeamFrom = ECrowdyServerTeamSource::CrowdyTeam;
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!GenerateCrate(*this, *Records, Crate))
	{
		return false;
	}
	CheckLocalCheckCrate(*this, Crate, TEXT("ck_exec_records_team"));
	const FString Lib = GetFileText(*this, Crate, LibPath);
	TestTrue(TEXT("membership of a record's team is the platform's answer"),
		HasText(Lib, TEXT("viewer.ctx.players().check_permission(viewer.player, team, None)")));
	TestTrue(TEXT("kept for the rest of the call"), HasText(Lib, TEXT("viewer.teams.insert(entry.team.clone(), member);")));
	TestTrue(TEXT("the owner always sees its record's Team Only values"), HasText(Lib, TEXT("        if owner || entry.team.is_empty() {\n            return Ok((owner, owner));\n")));
	TestTrue(TEXT("a team must be a Crowdy Team id"), HasText(Lib, TEXT("bad_record: a team must be a Crowdy Team id in decimal digits, or empty for none")));
	TestFalse(TEXT("not the team of a record the viewer owns here"), HasText(Lib, TEXT("owned_by(player).next()")));
	TestTrue(TEXT("watched numbers alone cannot be refused, so a touched record is not encoded again"),
		HasText(Lib, TEXT("    fn check_touched(&self) -> Result<()> {\n            Ok(())\n        }\n")) && !HasText(Lib, TEXT("read_record(&mut scratch")));
	return true;
}

namespace CrowdyCodegenTest
{
	FCrowdyServerMessage& AddServerMessage(UCrowdyServerObjectDefinition& Definition, const TCHAR* Name)
	{
		FCrowdyServerMessage& Message = Definition.Messages.AddDefaulted_GetRef();
		Message.Name = Name;
		return Message;
	}

	/** ck_exec_messages: Defeat sends a message with a struct, one with a List and one with no payload. Null when a test struct is missing. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeMessagesDefinition(FAutomationTestBase& Test)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Sender = MakeListDefinition(TEXT("ck_exec_messages"), {TEXT("Gold")});
		Sender->WatchedFields = {TEXT("Gold")};
		FCrowdyServerFunction& Defeat = AddPlainFunction(*Sender, TEXT("Defeat"), Players);
		Defeat.ParamsForm = ECrowdyServerValuesForm::List;
		AddListValue(Defeat.ParamsList, FPropertyBagPropertyDesc(TEXT("Amount"), EPropertyBagPropertyType::Int32));
		bool bFound = true;
		AddServerMessage(*Sender, TEXT("BossDefeated")).Payload = FindTestStruct(Test, TEXT("CrowdyExecTestHit"), bFound);
		FCrowdyServerMessage& Score = AddServerMessage(*Sender, TEXT("ScoreChanged"));
		Score.PayloadForm = ECrowdyServerValuesForm::List;
		AddListValue(Score.PayloadList, FPropertyBagPropertyDesc(TEXT("Total"), EPropertyBagPropertyType::Int32));
		AddServerMessage(*Sender, TEXT("RoundEnded"));
		// Topics that spell the glue's own constants must not break it.
		AddServerMessage(*Sender, TEXT("MaxMessage"));
		AddServerMessage(*Sender, TEXT("MaxMessageElements"));
		return bFound ? Sender : TStrongObjectPtr<UCrowdyServerObjectDefinition>();
	}

	/** ck_exec_messages_listener: Follow and Unfollow, listening to the sender. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeListenerDefinition(UCrowdyServerObjectDefinition& Sender)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Listener = MakeListDefinition(TEXT("ck_exec_messages_listener"), {TEXT("Gold")});
		Listener->WatchedFields = {TEXT("Gold")};
		AddPlainFunction(*Listener, TEXT("Follow"), Players);
		AddPlainFunction(*Listener, TEXT("Unfollow"), Players);
		Listener->ListenTo = {&Sender};
		return Listener;
	}

	/** A type with a message of each name and no payload, to be listened to. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeSenderOf(const TCHAR* TypeName, const TArray<const TCHAR*>& Messages)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Sender = MakeListDefinition(TypeName, {TEXT("Gold")});
		for (const TCHAR* Message : Messages)
		{
			AddServerMessage(*Sender, Message);
		}
		return Sender;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenMessagesCrateTest, "CrowdySDK.CrowdyExecEditor.CodegenMessagesMatchLocalCheckCrate", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenMessagesCrateTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Sender = MakeMessagesDefinition(*this);
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!Sender || !GenerateCrate(*this, *Sender, Crate))
	{
		return false;
	}
	CheckLocalCheckCrate(*this, Crate, TEXT("ck_exec_messages"));
	const FString Lib = GetFileText(*this, Crate, LibPath);
	const FString Types = GetFileText(*this, Crate, TypesPath);
	const FString Logic = GetFileText(*this, Crate, LogicPath);
	CrowdyExecCodegen::FGeneratedCrate Again;
	if (GenerateCrate(*this, *Sender, Again))
	{
		TestEqualSensitive(TEXT("the same definition gives the same glue"), GetFileText(*this, Again, LibPath), Lib);
	}
	TestTrue(TEXT("a message's topic is its name in snake case, a constant of the messages module"),
		HasText(Lib, TEXT("    pub const BOSS_DEFEATED: &str = \"boss_defeated\";\n    pub const SCORE_CHANGED: &str = \"score_changed\";\n    pub const ROUND_ENDED: &str = \"round_ended\";\n    pub const MAX_MESSAGE: &str = \"max_message\";\n    pub const MAX_MESSAGE_ELEMENTS: &str = \"max_message_elements\";\n")));
	TestTrue(TEXT("a topic named like a glue constant leaves the glue's limits reached through super"),
		HasText(Lib, TEXT("        if payload.elements() > super::MAX_MESSAGE_ELEMENTS {\n")) && HasText(Lib, TEXT("        if bytes.len() > super::MAX_MESSAGE {\n")));	TestTrue(TEXT("a struct message sends its struct"),
		HasText(Lib, TEXT("    pub fn send_boss_defeated(payload: &CrowdyExecTestHit) -> Result<()> {\n        send(BOSS_DEFEATED, payload)\n    }\n")));
	TestTrue(TEXT("a List message sends the struct named after it"),
		HasText(Lib, TEXT("    pub fn send_score_changed(payload: &ScoreChangedMessage) -> Result<()> {\n        send(SCORE_CHANGED, payload)\n    }\n")));
	TestTrue(TEXT("and the struct is written with its value's id"),
		HasText(Types, *FString::Printf(TEXT("pub struct ScoreChangedMessage {\n    pub Total: i32, // Int32 id=%s\n}"), *ValueIdText(TEXT("Total")))));
	TestTrue(TEXT("a message with no payload takes none and publishes nil"),
		HasText(Lib, TEXT("    pub fn send_round_ended() -> Result<()> {\n        queue(ROUND_ENDED, vec![0xc0])\n    }\n")));
	TestTrue(TEXT("a payload is checked as a reply is, before it is queued"),
		HasText(Lib, TEXT("        Wire::check(payload).map_err(|why| unsendable(&what, why))?;\n        if payload.elements() > super::MAX_MESSAGE_ELEMENTS {\n"))
		&& HasText(Lib, TEXT("        if bytes.len() > super::MAX_MESSAGE {\n            return Err(unsendable(&what, \"it is larger than 1 MiB\"));\n        }\n        queue(topic, bytes)\n")));
	TestTrue(TEXT("a send outside a handler is refused"), HasText(Lib, TEXT("Error::new(\"a message can only be sent from a handler\")")));
	TestTrue(TEXT("a function's sends wait for its dispatch"), HasText(Lib, TEXT("                messages::open();\n                let before = Clone::clone(&self.state);\n                let result = Functions::defeat(")));
	TestEqual(TEXT("so do the event hooks'"), CountText(Lib, TEXT("messages::open();")), 4);
	TestTrue(TEXT("finish takes them before anything can fail"), HasText(Lib, TEXT("-> Result<T> {\n        let sent = messages::close();\n        // Everything that can fail runs before the change is kept and pushed.\n")));
	TestTrue(TEXT("and publishes them after the state, once the change is kept"),
		HasText(Lib, TEXT("            let _ = ctx.publish(\"state\", &out);\n        }\n        messages::release(ctx, &sent);\n        Ok(value)\n")));
	TestEqual(TEXT("a failed handler's sends are released nowhere"), CountText(Lib, TEXT("messages::release(ctx")), 1);
	TestTrue(TEXT("sends are published in the order they were made"), HasText(Lib, TEXT("        for (topic, payload) in sent {\n            let _ = ctx.publish(topic, payload);\n        }\n")));
	TestTrue(TEXT("each payload struct is checked as a reply's is"), HasText(Lib, TEXT("impl Wire for ScoreChangedMessage {\n")) && HasText(Lib, TEXT("impl Wire for CrowdyExecTestHit {\n")));
	TestFalse(TEXT("a type with messages and no listening has no listen module"), HasText(Lib, TEXT("pub mod listen")));
	TestEqual(TEXT("the stub asks for the function only"), CrowdyExecCodegen::FindLogicGaps(Crate, Logic).Missing.Num(), 0);
	TestEqual(TEXT("and has no leftover"), CrowdyExecCodegen::FindLogicGaps(Crate, Logic).Leftover.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenNoMessagesTest, "CrowdySDK.CrowdyExecEditor.CodegenWithoutMessagesHasNoMessageGlue", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenNoMessagesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, {HealFunction});
	FString Lib;
	if (!Definition || !GenerateLib(*this, *Definition, Lib))
	{
		return false;
	}
	for (const TCHAR* Part : {TEXT("pub mod messages"), TEXT("messages::"), TEXT("pub mod listen"), TEXT("listen::"), TEXT("match (msg.node_type")})
	{
		TestFalse(FString::Printf(TEXT("a type with no messages and no Listen To has no '%s'"), Part), HasText(Lib, Part));
	}
	TestTrue(TEXT("on_topic goes to server code as before"), HasText(Lib, TEXT("        let before = Clone::clone(&self.state);\n        let result = Functions::on_topic(&mut self.state, ctx, msg);\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenListenCrateTest, "CrowdySDK.CrowdyExecEditor.CodegenListenMatchesLocalCheckCrate", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenListenCrateTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Sender = MakeMessagesDefinition(*this);
	if (!Sender || !BakeDefinition(*this, *Sender))
	{
		return false;
	}
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Listener = MakeListenerDefinition(*Sender);
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!GenerateCrate(*this, *Listener, Crate))
	{
		return false;
	}
	CheckLocalCheckCrate(*this, Crate, TEXT("ck_exec_messages_listener"));
	const FString Lib = GetFileText(*this, Crate, LibPath);
	const FString Logic = GetFileText(*this, Crate, LogicPath);
	TestTrue(TEXT("each message of the listened type has a typed handler taking the sender's key and its payload"),
		HasText(Lib, TEXT("    fn on_ck_exec_messages_boss_defeated(&mut self, ctx: &Ctx, key: &str, payload: calls::ck_exec_messages::CrowdyExecTestHit) -> Result<()>;\n"))
		&& HasText(Lib, TEXT("    fn on_ck_exec_messages_score_changed(&mut self, ctx: &Ctx, key: &str, payload: calls::ck_exec_messages::ScoreChangedMessage) -> Result<()>;\n")));
	TestTrue(TEXT("one with no payload takes none"), HasText(Lib, TEXT("    fn on_ck_exec_messages_round_ended(&mut self, ctx: &Ctx, key: &str) -> Result<()>;\n")));
	TestTrue(TEXT("a listened type is also callable, and its payload types are in its calls module"),
		HasText(Lib, TEXT("pub mod calls {\n    pub mod ck_exec_messages {\n")) && HasText(Lib, TEXT("        pub struct ScoreChangedMessage {\n")));
	TestTrue(TEXT("a listen queues a subscription of the type, the key given and the topic"),
		HasText(Lib, TEXT("        pub fn boss_defeated(ctx: &Ctx, key: &str) -> Result<()> {\n            super::queue(true, \"ck_exec_messages\", key, \"boss_defeated\")\n")));
	TestTrue(TEXT("and its stop queues the unsubscription"),
		HasText(Lib, TEXT("        pub fn stop_boss_defeated(ctx: &Ctx, key: &str) -> Result<()> {\n            super::queue(false, \"ck_exec_messages\", key, \"boss_defeated\")\n")));
	TestTrue(TEXT("a listen outside a handler is refused"), HasText(Lib, TEXT("Error::new(\"listening can only be started or stopped from a handler\")")));
	TestTrue(TEXT("a refused change names the type, the topic and the platform's reason"), HasText(Lib, TEXT("\"could not {verb} {node_type}.{topic}: {error}\"")));
	TestTrue(TEXT("every dispatch opens the queue of listens with its snapshot"),
		HasText(Lib, TEXT("                listen::open();\n                let before = Clone::clone(&self.state);\n                let result = Functions::follow(")));
	TestTrue(TEXT("finish takes them with its other queues"), HasText(Lib, TEXT("-> Result<T> {\n        let listens = listen::close();\n        // Everything that can fail runs before the change is kept and pushed.\n")));
	TestTrue(TEXT("and applies them after the push is prepared, so a refused one fails the handler and rolls it back"),
		HasText(Lib, TEXT("            let push = self.prepare_push(&before)?;\n            listen::apply(ctx, &listens)?;\n            Ok((value, push))\n")));
	TestTrue(TEXT("a refused change undoes the ones applied before it, last first"),
		HasText(Lib, TEXT("            undo(ctx, &changes[..index]);\n")) && HasText(Lib, TEXT("        for change in applied.iter().rev() {\n")));
	TestEqual(TEXT("no subscription is made anywhere else"), CountText(Lib, TEXT("ctx.subscribe(")), 2);
	TestTrue(TEXT("on_topic matches the type and the topic and decodes the payload"),
		HasText(Lib, TEXT("            (\"ck_exec_messages\", \"boss_defeated\") => listen::decode_message(&msg).and_then(|payload| Functions::on_ck_exec_messages_boss_defeated(&mut self.state, ctx, msg.key, payload)),\n")));
	TestTrue(TEXT("a message with no payload is not decoded"),
		HasText(Lib, TEXT("            (\"ck_exec_messages\", \"round_ended\") => Functions::on_ck_exec_messages_round_ended(&mut self.state, ctx, msg.key),\n")));
	TestTrue(TEXT("anything else still reaches on_topic, inside the same keep and finish"),
		HasText(Lib, TEXT("            _ => Functions::on_topic(&mut self.state, ctx, msg),\n        };\n        self.finish(ctx, before, result)\n")));
	TestTrue(TEXT("a payload that does not decode is an error naming the message"), HasText(Lib, TEXT("Error::new(&format!(\"bad_message: {}.{}: {}\", msg.node_type, msg.topic,")));
	TestFalse(TEXT("a listener that sends nothing has no messages module"), HasText(Lib, TEXT("pub mod messages")));
	TestTrue(TEXT("logic.rs gets the handlers, which succeed until written"), HasText(Logic, TEXT("    fn on_ck_exec_messages_round_ended(&mut self, ctx: &Ctx, key: &str) -> Result<()> {\n        Ok(())\n    }\n")));

	const CrowdyExecCodegen::FLogicGaps Gaps = CrowdyExecCodegen::FindLogicGaps(Crate, TEXT("impl Functions for CkExecMessagesListenerState {\n}\n"));
	TestEqual(TEXT("a logic file without them is told it lacks the two functions and the five handlers"), Gaps.Missing.Num(), 7);
	TestTrue(TEXT("including the typed handlers"), Gaps.Missing.ContainsByPredicate([](const CrowdyExecCodegen::FLogicStub& Stub) { return Stub.Method == TEXT("on_ck_exec_messages_boss_defeated"); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenListenCallsTest, "CrowdySDK.CrowdyExecEditor.CodegenListenToJoinsCanCall", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenListenCallsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Sender = MakeMessagesDefinition(*this);
	if (!Sender || !BakeDefinition(*this, *Sender))
	{
		return false;
	}
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Listener = MakeListenerDefinition(*Sender);
	Listener->CanCall = {Sender.Get()};
	Listener->ListenTo.Add(Sender.Get());
	FString Lib;
	if (!GenerateLib(*this, *Listener, Lib))
	{
		return false;
	}
	TestEqual(TEXT("a type both called and listened to has one calls module"), CountText(Lib, TEXT("pub mod calls {\n    pub mod ck_exec_messages {\n")), 1);
	TestEqual(TEXT("and one listen module"), CountText(Lib, TEXT("pub mod listen {\n")), 1);
	TestEqual(TEXT("whose handlers are asked for once"), CountText(Lib, TEXT("fn on_ck_exec_messages_round_ended(")), 1);
	TestEqual(TEXT("and whose functions can be called"), CountText(Lib, TEXT("pub fn defeat(ctx: &Ctx, key: &str, inputs: &DefeatParams)")), 1);

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Bank = MakeBankDefinition();
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Quiet = MakeListenerDefinition(*Bank);
	if (!BakeDefinition(*this, *Bank))
	{
		return false;
	}
	FString QuietLib;
	if (!GenerateLib(*this, *Quiet, QuietLib))
	{
		return false;
	}
	TestTrue(TEXT("a listened type with no messages is only callable"), HasText(QuietLib, TEXT("pub mod calls {\n    pub mod ck_exec_bank {\n")));
	TestFalse(TEXT("and adds no listen module"), HasText(QuietLib, TEXT("pub mod listen")));
	TestFalse(TEXT("nor a match in on_topic"), HasText(QuietLib, TEXT("match (msg.node_type")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenListenRefusalsTest, "CrowdySDK.CrowdyExecEditor.CodegenListenRefusesNamesThatClash", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenListenRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Sender = MakeMessagesDefinition(*this);
	if (!Sender || !BakeDefinition(*this, *Sender))
	{
		return false;
	}
	FString Error;
	CrowdyExecCodegen::FGeneratedCrate Refused;

	// Each change below follows a good bake, so the generator's own refusal is what is seen.
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Empty = MakeListenerDefinition(*Sender);
	if (!BakeDefinition(*this, *Empty))
	{
		return false;
	}
	Empty->ListenTo.Add(nullptr);
	TestFalse(TEXT("an empty Listen To entry does not generate"), CrowdyExecCodegen::Generate(*Empty, Refused, Error));
	TestEqualSensitive(TEXT("and the refusal says so"), Error, FString(TEXT("Listen To: every entry must be another Server Object definition with a Type Name")));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Stale = MakeMessagesDefinition(*this);
	if (!Stale || !BakeDefinition(*this, *Stale))
	{
		return false;
	}
	bool bFound = true;
	Stale->Messages[0].Payload = FindTestStruct(*this, TEXT("CrowdyExecTestRaw"), bFound);
	TestFalse(TEXT("a payload struct the field tables lack does not generate, rather than publishing nil"), CrowdyExecCodegen::Generate(*Stale, Refused, Error));
	TestEqualSensitive(TEXT("and the refusal says to save again"), Error, FString(TEXT("Server Message BossDefeated: its struct is missing from the field tables; save the definition again")));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Function = MakeListenerDefinition(*Sender);
	AddPlainFunction(*Function, TEXT("OnCkExecMessagesRoundEnded"), Players);
	TestTrue(TEXT("the function bakes"), BakeDefinition(*this, *Function));
	TestFalse(TEXT("a function named like a handler does not generate"), CrowdyExecCodegen::Generate(*Function, Refused, Error));
	TestTrue(TEXT("and the refusal names the handler"), Error.Contains(TEXT("on_ck_exec_messages_round_ended")));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Timer = MakeListenerDefinition(*Sender);
	AddTimer(*Timer, TEXT("OnCkExecMessagesRoundEnded"), ECrowdyServerTimerRepeat::Every, 1.f, false);
	TestTrue(TEXT("the timer bakes"), BakeDefinition(*this, *Timer));
	TestFalse(TEXT("a timer named like a handler does not generate"), CrowdyExecCodegen::Generate(*Timer, Refused, Error));
	TestTrue(TEXT("and the refusal names the handler"), Error.Contains(TEXT("on_ck_exec_messages_round_ended")));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Player = MakeSenderOf(TEXT("player"), {TEXT("Joined")});
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Hook = MakeListenerDefinition(*Player);
	TestTrue(TEXT("both bake"), BakeDefinition(*this, *Player) && BakeDefinition(*this, *Hook));
	TestFalse(TEXT("a handler named like the player-joined event does not generate"), CrowdyExecCodegen::Generate(*Hook, Refused, Error));
	TestTrue(TEXT("and the refusal names it"), Error.Contains(TEXT("on_player_joined")));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Stops = MakeSenderOf(TEXT("ck_exec_stops"), {TEXT("Boss"), TEXT("StopBoss")});
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Clash = MakeListenerDefinition(*Stops);
	TestTrue(TEXT("both bake"), BakeDefinition(*this, *Stops) && BakeDefinition(*this, *Clash));
	TestFalse(TEXT("a message whose topic is another's with stop_ in front does not generate"), CrowdyExecCodegen::Generate(*Clash, Refused, Error));
	TestTrue(TEXT("and the refusal names the topic"), Error.Contains(TEXT("'boss'")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenRecordsMessagesTest, "CrowdySDK.CrowdyExecEditor.CodegenRecordsSendMessages", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenRecordsMessagesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Records = MakeRecordsDefinition();
	AddServerMessage(*Records, TEXT("Respawned"));
	FString Lib;
	if (!GenerateLib(*this, *Records, Lib))
	{
		return false;
	}
	TestTrue(TEXT("a record type has the messages module"), HasText(Lib, TEXT("pub mod messages {\n")) && HasText(Lib, TEXT("    pub const RESPAWNED: &str = \"respawned\";\n")));
	TestTrue(TEXT("its dispatches queue sends"), HasText(Lib, TEXT("                messages::open();\n")));
	TestTrue(TEXT("finish takes them before anything can fail"), HasText(Lib, TEXT("            let sent = messages::close();\n            // Everything that can fail runs before the change is kept.\n")));
	TestTrue(TEXT("and publishes them once the records are kept, saying they can arrive before the records' send"),
		HasText(Lib, TEXT("            self.records.flush_pending |= armed;\n            // The records go out with the next send, so a message sent now can arrive before the change it follows.\n            messages::release(ctx, &sent);\n")));
	TestEqual(TEXT("and nowhere else"), CountText(Lib, TEXT("messages::release(ctx")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenSessionsExpireTest, "CrowdySDK.CrowdyExecEditor.CodegenSessionsExpireAfterARestart", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenSessionsExpireTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, {HitFunction, HealFunction});
	if (!Definition)
	{
		return false;
	}
	Definition->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	Definition->bRemoveMembersWhoLeave = true;
	Definition->bOnPlayerLeft = true;
	FString Lib;
	if (!GenerateLib(*this, *Definition, Lib))
	{
		return false;
	}
	TestTrue(TEXT("restored counts are held apart, and not saved"),
		HasText(Lib, TEXT("    sessions: BTreeMap<u64, u32>,\n    // The players whose restored connection count nothing has confirmed yet; not saved.\n    unconfirmed: BTreeSet<u64>,\n")));
	TestEqual(TEXT("only the object holds the set"), CountText(Lib, TEXT("unconfirmed: BTreeSet<u64>,")), 1);
	TestTrue(TEXT("a spawn restores nothing"), HasText(Lib, TEXT("sessions: BTreeMap::new(), unconfirmed: BTreeSet::new(), announced: false })")));
	TestTrue(TEXT("a load holds every restored player as unconfirmed"), HasText(Lib, TEXT("unconfirmed: saved.sessions.keys().copied().collect(), sessions: saved.sessions, announced: false })")));
	TestTrue(TEXT("the timer is reserved and the wait is two minutes"),
		HasText(Lib, TEXT("const SESSIONS_TIMER: &str = \"crowdy_sessions\";\nconst SESSION_CONFIRM_MS: u64 = 120_000;\n")));
	TestTrue(TEXT("the announcement is a flag the object keeps, not saved"),
		HasText(Lib, TEXT("    unconfirmed: BTreeSet<u64>,\n    // Whether the new epoch has been announced to watchers; not saved.\n    announced: bool,\n"))
		&& HasText(Lib, TEXT("unconfirmed: BTreeSet::new(), announced: false })")) && HasText(Lib, TEXT("sessions: saved.sessions, announced: false })")));
	TestTrue(TEXT("a load with restored counts arms the first run, which announces the epoch"),
		HasText(Lib, TEXT("        if !saved.sessions.is_empty() {\n            if let Err(error) = ctx.timer_after(SESSIONS_TIMER, SESSION_ANNOUNCE_MS) {\n")));
	TestTrue(TEXT("the first run publishes an empty push of the next seq and arms the second"),
		HasText(Lib, TEXT("        if !self.announced {\n            self.announced = true;\n            self.seq += 1;\n"))
		&& HasText(Lib, TEXT("            write_uint(&mut out, self.seq);\n            let _ = ctx.publish(\"state\", &out);\n            if let Err(error) = ctx.timer_after(SESSIONS_TIMER, SESSION_CONFIRM_MS) {\n")));
	TestTrue(TEXT("on_timer runs it before the definition's timers, with or without any"),
		HasText(Lib, TEXT("    fn on_timer(&mut self, ctx: &Ctx, name: &str) -> Result<()> {\n        if name == SESSIONS_TIMER {\n            return self.sessions_timer(ctx);\n        }\n")));
	TestTrue(TEXT("any call from a player confirms the restored count"),
		HasText(Lib, TEXT("    fn handle(&mut self, ctx: &Ctx, call: Call<'_>) -> Result<Vec<u8>> {\n        if let Ok(player) = call.player() {\n            self.unconfirmed.remove(&player);\n        }\n        match call.method {\n")));
	TestTrue(TEXT("a Joined for an unconfirmed player sets the count to one and runs no hook, the visit goes on"),
		HasText(Lib, TEXT("        let restored = self.unconfirmed.remove(&player);\n"))
		&& HasText(Lib, TEXT("                if restored {\n                    self.sessions.insert(player, 1);\n                    return Ok(());\n                }\n                if connections == 0 && ")));
	TestTrue(TEXT("a Left that leaves an unconfirmed player a count keeps them unconfirmed"),
		HasText(Lib, TEXT("                    self.sessions.insert(player, connections - 1);\n                    if restored {\n                        self.unconfirmed.insert(player);\n                    }\n                    return Ok(());\n")));
	TestTrue(TEXT("the last connection leaving ends the visit through the one helper"), HasText(Lib, TEXT("                    return Ok(());\n                }\n                self.session_ended(ctx, player)\n            }\n")));
	TestTrue(TEXT("and so does the expiry"), HasText(Lib, TEXT("        for player in std::mem::take(&mut self.unconfirmed) {\n            if let Err(error) = self.session_ended(ctx, player) {\n")));
	TestEqual(TEXT("the removal and On Player Left are written once"), CountText(Lib, TEXT("Functions::on_player_left(&mut self.state, ctx, player)")), 1);
	TestTrue(TEXT("the helper removes the member, which stands even when On Player Left fails"),
		HasText(Lib, TEXT("    fn session_ended(&mut self, ctx: &Ctx, player: u64) -> Result<()> {\n        self.sessions.remove(&player);\n"))
		&& HasText(Lib, TEXT("        members::remove(player);\n        self.finish(ctx, before, Ok(()))?;\n        // The removal stands even when on_player_left fails.\n")));

	AddTimer(*Definition, TEXT("Respawn"), ECrowdyServerTimerRepeat::After, 10.f, false);
	FString Timed;
	if (!GenerateLib(*this, *Definition, Timed))
	{
		return false;
	}
	const int32 ModuleStart = Timed.Find(TEXT("pub mod timers {"), ESearchCase::CaseSensitive);
	const int32 ModuleEnd = Timed.Find(TEXT("\n}\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, ModuleStart);
	TestTrue(TEXT("the timers module is found"), ModuleStart != INDEX_NONE && ModuleEnd > ModuleStart);
	const FString Module = Timed.Mid(ModuleStart, ModuleEnd - ModuleStart);
	TestFalse(TEXT("the definition's timers never cancel or re-arm the sessions timer"), HasText(Module, TEXT("SESSIONS_TIMER")) || HasText(Module, TEXT("crowdy_sessions")));
	TestTrue(TEXT("a type with timers still runs it first"), HasText(Timed, TEXT("        if name == SESSIONS_TIMER {\n            return self.sessions_timer(ctx);\n        }\n        members::lend(&self.roster);\n")));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Plain = MakeDefinition(*this, {HitFunction, HealFunction});
	if (!Plain)
	{
		return false;
	}
	Plain->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	Plain->bRemoveMembersWhoLeave = false;
	FString Without;
	if (!GenerateLib(*this, *Plain, Without))
	{
		return false;
	}
	for (const TCHAR* Part : {TEXT("unconfirmed"), TEXT("SESSIONS_TIMER"), TEXT("SESSION_CONFIRM_MS"), TEXT("session_ended"), TEXT("sessions_timer"), TEXT("crowdy_sessions")})
	{
		TestFalse(FString::Printf(TEXT("a type that counts no sessions has no %s"), Part), HasText(Without, Part));
	}

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Records = MakeRecordsDefinition();
	Records->bOnPlayerLeft = true;
	FString RecordsLib;
	if (!GenerateLib(*this, *Records, RecordsLib))
	{
		return false;
	}
	TestTrue(TEXT("a records type reuses the send that announces its epoch, then waits the minute"),
		HasText(RecordsLib, TEXT("match ctx.timer_after(FLUSH_TIMER, PUSH_INTERVAL_MS) {\n"))
		&& HasText(RecordsLib, TEXT("            if let Err(error) = ctx.timer_after(SESSIONS_TIMER, SESSION_CONFIRM_MS) {\n")));
	TestFalse(TEXT("and has no first run of its own"), HasText(RecordsLib, TEXT("SESSION_ANNOUNCE_MS")) || HasText(RecordsLib, TEXT("announced")));
	TestTrue(TEXT("it runs the expiry after the send"),
		HasText(RecordsLib, TEXT("            if name == FLUSH_TIMER {\n                return self.flush(ctx);\n            }\n            if name == SESSIONS_TIMER {\n                return self.sessions_timer(ctx);\n            }\n")));
	TestTrue(TEXT("a call confirms there too"), HasText(RecordsLib, TEXT("            if let Ok(player) = call.player() {\n                self.unconfirmed.remove(&player);\n            }\n")));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Reserved = MakeDefinition(*this, {HealFunction});
	if (!Reserved || !BakeDefinition(*this, *Reserved))
	{
		return false;
	}
	AddTimer(*Reserved, TEXT("crowdy_sessions"), ECrowdyServerTimerRepeat::Every, 1.f, true);
	CrowdyExecCodegen::FGeneratedCrate Refused;
	FString Error;
	TestFalse(TEXT("a timer named crowdy_sessions does not generate"), CrowdyExecCodegen::Generate(*Reserved, Refused, Error));
	TestEqualSensitive(TEXT("and the refusal names it"), Error, FString(TEXT("Timer crowdy_sessions: 'crowdy_sessions' is reserved in the server code (Rust); rename the timer")));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenGeneratedFolderTest, "CrowdySDK.CrowdyExecEditor.CodegenRecognizesItsOwnCrateFolder", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenGeneratedFolderTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	TArray<FString> Written;
	FString Error;

	UPackage* Package = CreatePackage(TEXT("/Temp/CrowdyExecCodegenTests/DA_OwnFolder"));
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Tagged(NewObject<UCrowdyServerObjectDefinition>(Package, NAME_None, RF_Transient));
	Tagged->TypeName = TEXT("ck_exec_own_folder");
	Tagged->StateForm = ECrowdyServerValuesForm::List;
	Tagged->StateList.AddProperty(TEXT("Gold"), EPropertyBagPropertyType::Int32);
	CrowdyExecCodegen::FGeneratedCrate TaggedCrate;
	FTempFolder WithRecord;
	if (!GenerateCrate(*this, *Tagged, TaggedCrate) || !TestTrue(TEXT("the crate with a record is written"), CrowdyExecCodegen::WriteCrate(TaggedCrate, WithRecord.Path, Written, Error)))
	{
		return false;
	}
	TestTrue(TEXT("a generated crate that records its definition is the SDK's"), CrowdyExecCodegen::IsGeneratedCrateFolder(WithRecord.Path));
	IFileManager::Get().Delete(*FPaths::Combine(WithRecord.Path, LibPath));
	IFileManager::Get().Delete(*FPaths::Combine(WithRecord.Path, TypesPath));
	TestTrue(TEXT("the record alone is enough"), CrowdyExecCodegen::IsGeneratedCrateFolder(WithRecord.Path));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Fixture = MakeDefinition(*this, FixtureFunctions());
	CrowdyExecCodegen::FGeneratedCrate FixtureCrate;
	FTempFolder OldShape;
	if (!Fixture || !GenerateCrate(*this, *Fixture, FixtureCrate) || !TestTrue(TEXT("the crate without a record is written"), CrowdyExecCodegen::WriteCrate(FixtureCrate, OldShape.Path, Written, Error)))
	{
		return false;
	}
	TestFalse(TEXT("the old-shape crate records no definition"), LoadText(FPaths::Combine(OldShape.Path, CargoPath)).Contains(TEXT("[package.metadata.crowdy]")));
	TestTrue(TEXT("a generated crate with no record is the SDK's"), CrowdyExecCodegen::IsGeneratedCrateFolder(OldShape.Path));
	const FString OldTypes = LoadText(FPaths::Combine(OldShape.Path, TypesPath));
	IFileManager::Get().Delete(*FPaths::Combine(OldShape.Path, TypesPath));
	TestTrue(TEXT("its lib.rs alone is enough"), CrowdyExecCodegen::IsGeneratedCrateFolder(OldShape.Path));
	FFileHelper::SaveStringToFile(OldTypes, *FPaths::Combine(OldShape.Path, TypesPath), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	IFileManager::Get().Delete(*FPaths::Combine(OldShape.Path, LibPath));
	TestTrue(TEXT("its types.rs alone is enough"), CrowdyExecCodegen::IsGeneratedCrateFolder(OldShape.Path));
	IFileManager::Get().Delete(*FPaths::Combine(OldShape.Path, CargoPath));
	TestFalse(TEXT("generated sources without a Cargo.toml are not a crate"), CrowdyExecCodegen::IsGeneratedCrateFolder(OldShape.Path));

	FTempFolder HandWritten;
	FFileHelper::SaveStringToFile(FString(TEXT("[package]\nname = \"mine\"\nversion = \"0.1.0\"\n")), *FPaths::Combine(HandWritten.Path, CargoPath), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FFileHelper::SaveStringToFile(FString(TEXT("// my own server code\n// not generated by anyone else\n")), *FPaths::Combine(HandWritten.Path, LibPath), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	TestFalse(TEXT("a hand-written crate is not the SDK's"), CrowdyExecCodegen::IsGeneratedCrateFolder(HandWritten.Path));
	FFileHelper::SaveStringToFile(FString(TEXT("// mine\n// generated by the Crowdy SDK from its definition\n")), *FPaths::Combine(HandWritten.Path, TypesPath), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	TestFalse(TEXT("the marker only counts on the first line"), CrowdyExecCodegen::IsGeneratedCrateFolder(HandWritten.Path));

	FTempFolder Missing;
	TestFalse(TEXT("a folder that does not exist is not the SDK's"), CrowdyExecCodegen::IsGeneratedCrateFolder(Missing.Path));
	FTempFolder Empty;
	IFileManager::Get().MakeDirectory(*Empty.Path, true);
	TestFalse(TEXT("an empty folder is not the SDK's"), CrowdyExecCodegen::IsGeneratedCrateFolder(Empty.Path));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenLogicGapsMacrosTest, "CrowdySDK.CrowdyExecEditor.CodegenLogicGapsNameMacros", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenLogicGapsMacrosTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition;
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!GenerateGapsCrate(*this, Definition, Crate))
	{
		return false;
	}
	const FString Written = FString::Printf(TEXT(
		"impl Functions for %s {\n"
		"    fn hit(&mut self, ctx: &Ctx, call: &Call<'_>, params: CrowdyExecTestHit) -> Result<()> {\n"
		"        inner_macro!(ctx);\n"
		"        Ok(())\n"
		"    }\n"
		"    const SAME: bool = FIRST != (SECOND);\n"
		"    my_macro!(heal);\n"
		"    other! { fn end_match(&mut self){} }\n"
		"    // commented!(fn)\n"
		"}\n"), *Crate.StateName);
	const CrowdyExecCodegen::FLogicGaps Gaps = CrowdyExecCodegen::FindLogicGaps(Crate, Written);
	TestEqualSensitive(TEXT("macros invoked in the block are named with their !; one inside a function, a != and a comment are not"),
		FString::Join(Gaps.Macros, TEXT(",")), FString(TEXT("my_macro!,other!")));
	TestEqualSensitive(TEXT("what a macro might define still counts as missing"), DescribeGaps(Gaps), FString(TEXT("missing heal,end_match,on_player_joined | leftover ")));
	TestEqualSensitive(TEXT("but no stub is added next to a macro"), CrowdyExecCodegen::AddLogicStubs(Written, Gaps), Written);

	const FString Plain = Written.Replace(TEXT("    my_macro!(heal);\n"), TEXT("")).Replace(TEXT("    other! { fn end_match(&mut self){} }\n"), TEXT(""));
	const CrowdyExecCodegen::FLogicGaps NoMacros = CrowdyExecCodegen::FindLogicGaps(Crate, Plain);
	TestTrue(TEXT("without them the block has no macros"), NoMacros.Macros.IsEmpty());
	TestTrue(TEXT("and stubs are added again"), CrowdyExecCodegen::AddLogicStubs(Plain, NoMacros) != Plain);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenTimerConstantsTest,"CrowdySDK.CrowdyExecEditor.CodegenTimerNamesAreConstants", CrowdyCodegenTest::TestFlags)
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

namespace CrowdyCodegenTest
{
	/** ck_exec_library: a Server Function Library with Add (a List in, a List out) and Ping (Server Only). */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeLibraryDefinition()
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Library(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Library->TypeName = TEXT("ck_exec_library");
		Library->Kind = ECrowdyServerObjectKind::FunctionLibrary;
		FCrowdyServerFunction& Add = AddPlainFunction(*Library, TEXT("Add"), Players);
		Add.ParamsForm = ECrowdyServerValuesForm::List;
		AddListValue(Add.ParamsList, FPropertyBagPropertyDesc(TEXT("Left"), EPropertyBagPropertyType::Int32));
		AddListValue(Add.ParamsList, FPropertyBagPropertyDesc(TEXT("Right"), EPropertyBagPropertyType::Int32));
		Add.ReplyForm = ECrowdyServerValuesForm::List;
		AddListValue(Add.ReplyList, FPropertyBagPropertyDesc(TEXT("Total"), EPropertyBagPropertyType::Int32));
		AddPlainFunction(*Library, TEXT("Ping"), ServerOnly);
		return Library;
	}

	/** ck_exec_library_hub: Sum calls the library's Add and Raw calls its Ping through Can Call Any Type. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeLibraryHubDefinition(UCrowdyServerObjectDefinition& Library)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Hub = MakeListDefinition(TEXT("ck_exec_library_hub"), {TEXT("Total")});
		Hub->WatchedFields = {TEXT("Total")};
		AddPlainFunction(*Hub, TEXT("Sum"), Players);
		AddPlainFunction(*Hub, TEXT("Raw"), Players);
		Hub->CanCall = {&Library};
		Hub->bCanCallAnyType = true;
		return Hub;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenLibraryCrateTest, "CrowdySDK.CrowdyExecEditor.CodegenLibraryMatchesLocalCheckCrate", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenLibraryCrateTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Library = MakeLibraryDefinition();
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!GenerateCrate(*this, *Library, Crate))
	{
		return false;
	}
	CheckLocalCheckCrate(*this, Crate, TEXT("ck_exec_library"));
	const FString Lib = GetFileText(*this, Crate, LibPath);
	const FString Logic = GetFileText(*this, Crate, LogicPath);
	const FString Cargo = GetFileText(*this, Crate, CargoPath);
	TestTrue(TEXT("a library is a spoke"), HasText(Lib, TEXT("impl Spoke for ServerObject {\n    fn start(ctx: &Ctx) -> Result<Self> {\n        Ok(Self)\n    }\n")));
	TestTrue(TEXT("exported as one"), HasText(Lib, TEXT("ckx_sdk::export_spoke!(ServerObject);\n")));
	for (const TCHAR* Part : {TEXT("export_hub"), TEXT("impl Hub"), TEXT("fn spawn"), TEXT("fn persist"), TEXT("SnapshotOut"), TEXT("SnapshotIn"), TEXT("CONTRACT"), TEXT("fn finish"),
		TEXT("fn prepare_push"), TEXT("\"read\" =>"), TEXT("ctx.publish"), TEXT("timers"), TEXT("on_timer"), TEXT("on_topic"), TEXT("on_presence"), TEXT("on_session"), TEXT("messages::"),
		TEXT("listen::"), TEXT("members::"), TEXT("Roster"), TEXT("cooldown"), TEXT("ctx.key"), TEXT("OWNER_ONLY"), TEXT("self.state"), TEXT("epoch")})
	{
		TestFalse(FString::Printf(TEXT("a library has no '%s'"), Part), HasText(Lib, Part));
	}
	TestTrue(TEXT("its functions are implemented on a struct with no fields"), HasText(Lib, TEXT("pub struct Library;\n")) && HasText(Logic, TEXT("impl Functions for Library {\n")));
	TestTrue(TEXT("a function keeps its authorize, decoding and reply, and returns the call's result"),
		HasText(Lib, TEXT("            \"add\" => {\n                authorize(ctx, &call, true)?;\n                let params: AddParams = call.decode().map_err(bad_params)?;\n")
			TEXT("                Wire::check(&params).map_err(|why| Error::new(&format!(\"bad_params: {why}\")))?;\n")
			TEXT("                Functions::add(&mut Library, ctx, &call, params).and_then(|reply| encode_reply(&reply))\n            }\n")));
	TestTrue(TEXT("a Server Only function is refused to players"),
		HasText(Lib, TEXT("            \"ping\" => {\n                authorize(ctx, &call, false)?;\n                Functions::ping(&mut Library, ctx, &call).map(|()| vec![0xc0])\n            }\n")));
	TestTrue(TEXT("the trait asks for the two functions only"), HasText(Lib, TEXT("pub trait Functions {\n    fn add(&mut self, ctx: &Ctx, call: &Call<'_>, params: AddParams) -> Result<AddReply>;\n    fn ping(&mut self, ctx: &Ctx, call: &Call<'_>) -> Result<()>;\n}\n")));
	TestTrue(TEXT("the Cargo pin is the platform's"), HasText(Cargo, *FString::Printf(TEXT("ckx-sdk = \"%s\"\n"), CrowdyExecCodegen::CkxSdkVersion)));
	TestEqual(TEXT("logic.rs has the two functions and nothing else"), CrowdyExecCodegen::FindLogicGaps(Crate, Logic).Missing.Num(), 0);
	TestEqual(TEXT("with no leftover"), CrowdyExecCodegen::FindLogicGaps(Crate, Logic).Leftover.Num(), 0);
	TestEqual(TEXT("and the stubs ask for the two"), Crate.Stubs.Num(), 2);
	TestEqualSensitive(TEXT("the logic file implements Functions on the library's struct"), Crate.StateName, FString(TEXT("Library")));

	CrowdyExecCodegen::FGeneratedCrate Again;
	if (GenerateCrate(*this, *Library, Again))
	{
		TestEqualSensitive(TEXT("the same definition gives the same glue"), GetFileText(*this, Again, LibPath), Lib);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenLibraryIgnoresServerObjectSettingsTest, "CrowdySDK.CrowdyExecEditor.CodegenLibraryIgnoresWhatOnlyAServerObjectUses", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenLibraryIgnoresServerObjectSettingsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Library = MakeLibraryDefinition();
	FString Plain;
	if (!GenerateLib(*this, *Library, Plain))
	{
		return false;
	}
	bool bFound = true;
	Library->State = FindTestStruct(*this, TEXT("CrowdyExecTestBossState"), bFound);
	Library->WatchedFields = {TEXT("Health")};
	Library->Visibility = ECrowdyServerObjectVisibility::OwnerOnly;
	Library->bOnlyOneInstance = true;
	Library->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	Library->bOnPlayerJoined = true;
	Library->bOnPlayerLeft = true;
	AddTimer(*Library, TEXT("Tick"), ECrowdyServerTimerRepeat::Every, 5.f, true);
	AddServerMessage(*Library, TEXT("Done"));
	FString Left;
	if (!bFound || !GenerateLib(*this, *Library, Left))
	{
		return false;
	}
	TestEqualSensitive(TEXT("a library that was a Server Object generates what it would have without those settings"), Left, Plain);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenLibraryCalleeTest, "CrowdySDK.CrowdyExecEditor.CodegenCallsToALibraryHaveNoKey", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenLibraryCalleeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Library = MakeLibraryDefinition();
	if (!BakeDefinition(*this, *Library))
	{
		return false;
	}
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Hub = MakeLibraryHubDefinition(*Library);
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!GenerateCrate(*this, *Hub, Crate))
	{
		return false;
	}
	CheckLocalCheckCrate(*this, Crate, TEXT("ck_exec_library_hub"));
	const FString Lib = GetFileText(*this, Crate, LibPath);
	TestTrue(TEXT("the library's functions are typed helpers with no key"),
		HasText(Lib, TEXT("        pub fn add(ctx: &Ctx, inputs: &AddParams) -> Result<AddReply> {\n")));
	TestTrue(TEXT("one that takes nothing has only the context"), HasText(Lib, TEXT("        pub fn ping(ctx: &Ctx) -> Result<()> {\n")));
	TestTrue(TEXT("and calls the library under the empty key"), HasText(Lib, TEXT("ctx.call(\"ck_exec_library\", \"\", \"add\", &payload)")) && HasText(Lib, TEXT("ctx.call(\"ck_exec_library\", \"\", \"ping\", &[0x80])")));
	TestFalse(TEXT("none takes a key"), HasText(Lib, TEXT("pub fn add(ctx: &Ctx, key: &str")) || HasText(Lib, TEXT("pub fn ping(ctx: &Ctx, key: &str")));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Bank = MakeBankDefinition();
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Caller = MakeLibraryDefinition();
	Caller->TypeName = TEXT("ck_exec_library_caller");
	Caller->CanCall = {Bank.Get(), Library.Get()};
	if (!BakeDefinition(*this, *Bank))
	{
		return false;
	}
	FString CallerLib;
	if (!GenerateLib(*this, *Caller, CallerLib))
	{
		return false;
	}
	TestTrue(TEXT("a library can call a Server Object with its key"), HasText(CallerLib, TEXT("        pub fn store(ctx: &Ctx, key: &str, inputs: &StoreParams) -> Result<StoreReply> {\n")));
	TestTrue(TEXT("and another library without"), HasText(CallerLib, TEXT("        pub fn ping(ctx: &Ctx) -> Result<()> {\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenCallAnyTest, "CrowdySDK.CrowdyExecEditor.CodegenCallsAnyOnlyWithCanCallAnyType", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenCallAnyTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Library = MakeLibraryDefinition();
	if (!BakeDefinition(*this, *Library))
	{
		return false;
	}
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Hub = MakeLibraryHubDefinition(*Library);
	const TCHAR* const Any = TEXT("    pub fn any(ctx: &ckx_sdk::Ctx, node_type: &str, key: &str, method: &str, payload: &[u8]) -> std::result::Result<Vec<u8>, ckx_sdk::CallError> {\n        ctx.call(node_type, key, method, payload)\n    }\n");
	FString WithAny;
	if (!GenerateLib(*this, *Hub, WithAny))
	{
		return false;
	}
	TestEqual(TEXT("Can Call Any Type adds the untyped call"), CountText(WithAny, Any), 1);
	TestTrue(TEXT("beside the helpers for the types listed"), HasText(WithAny, TEXT("pub mod calls {\n    pub mod ck_exec_library {\n")));

	Hub->bCanCallAnyType = false;
	FString WithoutAny;
	if (!GenerateLib(*this, *Hub, WithoutAny))
	{
		return false;
	}
	TestFalse(TEXT("without it there is none"), HasText(WithoutAny, TEXT("pub fn any(")));

	Hub->bCanCallAnyType = true;
	Hub->CanCall.Empty();
	FString AnyOnly;
	if (!GenerateLib(*this, *Hub, AnyOnly))
	{
		return false;
	}
	TestTrue(TEXT("a type that lists no one still gets the module"), HasText(AnyOnly, TEXT("pub mod calls {\n    /// A call to any Server Object type")));
	TestEqual(TEXT("with the untyped call once"), CountText(AnyOnly, Any), 1);

	Hub->bCanCallAnyType = false;
	FString Neither;
	if (!GenerateLib(*this, *Hub, Neither))
	{
		return false;
	}
	TestFalse(TEXT("and one that lists no one and cannot call any has no calls module"), HasText(Neither, TEXT("pub mod calls")));
	return true;
}

namespace CrowdyCodegenTest
{
	/** ck_exec_ping_library: a Server Function Library whose one function takes and returns nothing, so it has no structs. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakePingLibraryDefinition()
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Library(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Library->TypeName = TEXT("ck_exec_ping_library");
		Library->Kind = ECrowdyServerObjectKind::FunctionLibrary;
		AddPlainFunction(*Library, TEXT("Ping"), Players);
		return Library;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenStructlessLibraryTest, "CrowdySDK.CrowdyExecEditor.CodegenLibraryWithoutStructs", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenStructlessLibraryTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Library = MakePingLibraryDefinition();
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!GenerateCrate(*this, *Library, Crate))
	{
		return false;
	}
	TestTrue(TEXT("a library with no structs has no field tables"), Library->BakedStructs.IsEmpty());
	const FString Lib = GetFileText(*this, Crate, LibPath);
	TestTrue(TEXT("it is still a spoke"), HasText(Lib, TEXT("ckx_sdk::export_spoke!(ServerObject);\n")));
	TestTrue(TEXT("and dispatches its function"), HasText(Lib, TEXT("            \"ping\" => {\n                authorize(ctx, &call, true)?;\n                Functions::ping(&mut Library, ctx, &call).map(|()| vec![0xc0])\n            }\n")));
	TestEqual(TEXT("and the stub asks for the one function"), Crate.Stubs.Num(), 1);

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Hub = MakeListDefinition(TEXT("ck_exec_ping_hub"), {TEXT("Total")});
	Hub->WatchedFields = {TEXT("Total")};
	AddPlainFunction(*Hub, TEXT("Poke"), Players);
	Hub->CanCall = {Library.Get()};
	FString HubLib;
	if (!GenerateLib(*this, *Hub, HubLib))
	{
		return false;
	}
	TestTrue(TEXT("a Server Object that lists it in Can Call gets a helper for it"), HasText(HubLib, TEXT("        pub fn ping(ctx: &Ctx) -> Result<()> {\n")));
	TestTrue(TEXT("which calls it under the empty key"), HasText(HubLib, TEXT("ctx.call(\"ck_exec_ping_library\", \"\", \"ping\", &[0x80])")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecCodegenLibraryNameFirstTest, "CrowdySDK.CrowdyExecEditor.CodegenLibraryKeepsItsUnitStructName", CrowdyCodegenTest::TestFlags)
bool FCrowdyExecCodegenLibraryNameFirstTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyCodegenTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Library = MakePingLibraryDefinition();
	Library->TypeName = TEXT("ck_exec_name_library");
	AddPlainFunction(*Library, TEXT("Roll"), Players).Params = FLibrary::StaticStruct();
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!GenerateCrate(*this, *Library, Crate))
	{
		return false;
	}
	const FString Lib = GetFileText(*this, Crate, LibPath);
	const FString Types = GetFileText(*this, Crate, TypesPath);
	TestTrue(TEXT("the library's struct keeps the name Library"), HasText(Lib, TEXT("pub struct Library;\n")));
	TestEqualSensitive(TEXT("and logic.rs implements Functions on it"), Crate.StateName, FString(TEXT("Library")));
	TestTrue(TEXT("a struct of the library's own named Library is renamed instead"), HasText(Types, TEXT("pub struct LibraryType {\n")));
	TestFalse(TEXT("and does not take the name"), HasText(Types, TEXT("pub struct Library {\n")));
	return true;
}

#endif
