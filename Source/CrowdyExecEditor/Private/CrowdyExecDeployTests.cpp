#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyExecCodegen.h"
#include "CrowdyExecDeploy.h"
#include "CrowdyServerObjectDefinition.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace CrowdyExecDeployTest
{
	using CrowdyExecDeploy::ECrateState;
	using CrowdyExecDeveloper::FBuildCrate;
	using CrowdyExecDeveloper::FBuildFile;

	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	const TCHAR* const FixtureType = TEXT("deploy_fixture");
	const TCHAR* const TypesPath = TEXT("src/types.rs");
	const TCHAR* const LogicPath = TEXT("src/logic.rs");

	const TCHAR* StateName(ECrateState State)
	{
		switch (State)
		{
		case ECrateState::UpToDate: return TEXT("UpToDate");
		case ECrateState::NotGenerated: return TEXT("NotGenerated");
		case ECrateState::OutOfDate: return TEXT("OutOfDate");
		default: return TEXT("Invalid");
		}
	}

	bool TestState(FAutomationTestBase& Test, const TCHAR* What, const CrowdyExecDeploy::FTypeState& Actual, ECrateState Expected)
	{
		return Test.TestEqualSensitive(FString::Printf(TEXT("%s (%s)"), What, *Actual.Problem), FString(StateName(Actual.State)), FString(StateName(Expected)));
	}

	/** A type with no functions or intervals set, enough for the manifest, which reads nothing else. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeManifestDefinition(const TCHAR* TypeName, int32 SaveIntervalSeconds, int32 IdleTimeoutSeconds)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Definition->TypeName = TypeName;
		Definition->SaveIntervalSeconds = SaveIntervalSeconds;
		Definition->IdleTimeoutSeconds = IdleTimeoutSeconds;
		return Definition;
	}

	// The test structs are private to CrowdyExec, so they are found by path.
	UScriptStruct* FindTestStruct(FAutomationTestBase& Test, const TCHAR* Name)
	{
		UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, FString::Printf(TEXT("/Script/CrowdyExec.%s"), Name));
		if (!Struct)
		{
			Test.AddError(FString::Printf(TEXT("the test struct %s is not loaded"), Name));
		}
		return Struct;
	}

	/** A buildable type, not yet baked. Null when a test struct is missing. */
	TStrongObjectPtr<UCrowdyServerObjectDefinition> MakeDefinition(FAutomationTestBase& Test, const TCHAR* TypeName)
	{
		TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition(NewObject<UCrowdyServerObjectDefinition>(GetTransientPackage(), NAME_None, RF_Transient));
		Definition->TypeName = TypeName;
		Definition->WatchedFields = {TEXT("Health"), TEXT("bDefeated"), TEXT("Phase")};
		Definition->State = FindTestStruct(Test, TEXT("CrowdyExecTestBossState"));
		FCrowdyServerFunction& Hit = Definition->Functions.AddDefaulted_GetRef();
		Hit.Name = TEXT("Hit");
		Hit.Params = FindTestStruct(Test, TEXT("CrowdyExecTestHit"));
		FCrowdyServerFunction& Heal = Definition->Functions.AddDefaulted_GetRef();
		Heal.Name = TEXT("Heal");
		Heal.WhoCanCall = ECrowdyServerFunctionCaller::ServerOnly;
		if (!Definition->State || !Hit.Params)
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

	bool WriteGenerated(FAutomationTestBase& Test, UCrowdyServerObjectDefinition& Definition, const FString& Directory, CrowdyExecCodegen::FGeneratedCrate& OutCrate)
	{
		if (!GenerateCrate(Test, Definition, OutCrate))
		{
			return false;
		}
		TArray<FString> Written;
		FString Error;
		const bool bWritten = CrowdyExecCodegen::WriteCrate(OutCrate, Directory, Written, Error);
		return Test.TestTrue(FString::Printf(TEXT("the crate is written (%s)"), *Error), bWritten);
	}

	const FString* FindGeneratedText(const CrowdyExecCodegen::FGeneratedCrate& Crate, const TCHAR* Path)
	{
		const CrowdyExecCodegen::FGeneratedFile* File = Crate.Files.FindByPredicate([Path](const CrowdyExecCodegen::FGeneratedFile& Candidate)
		{
			return Candidate.Path.Equals(Path, ESearchCase::CaseSensitive);
		});
		return File ? &File->Text : nullptr;
	}

	const FBuildFile* FindBuildFile(const FBuildCrate& Crate, const TCHAR* Path)
	{
		return Crate.Files.FindByPredicate([Path](const FBuildFile& Candidate) { return Candidate.Path.Equals(Path, ESearchCase::CaseSensitive); });
	}

	bool SaveText(const FString& Text, const FString& Path)
	{
		return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	FString LoadText(const FString& Path)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *Path);
		return Text;
	}

	struct FTempFolder
	{
		FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("CrowdyExecDeployTests"), FGuid::NewGuid().ToString()));

		~FTempFolder()
		{
			IFileManager::Get().DeleteDirectory(*Path, false, true);
		}
	};

	FString GetManifestGoldenPath()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CrowdySDK"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source/CrowdyExecEditor/Tests/Golden/manifest/project.json")));
	}

	TArray<FBuildCrate> MakeCrates(int32 CrateCount, int32 FilesPerCrate, int32 CharactersPerFile)
	{
		TArray<FBuildCrate> Crates;
		for (int32 CrateIndex = 0; CrateIndex < CrateCount; ++CrateIndex)
		{
			FBuildCrate& Crate = Crates.AddDefaulted_GetRef();
			Crate.Name = FString::Printf(TEXT("crate_%d"), CrateIndex);
			for (int32 FileIndex = 0; FileIndex < FilesPerCrate; ++FileIndex)
			{
				Crate.Files.Add(FBuildFile{FString::Printf(TEXT("src/file_%d.rs"), FileIndex), FString::ChrN(CharactersPerFile, TEXT('a'))});
			}
		}
		return Crates;
	}

	FString FilePaths(const FBuildCrate& Crate)
	{
		TArray<FString> Paths;
		for (const FBuildFile& File : Crate.Files)
		{
			Paths.Add(File.Path);
		}
		return FString::Join(Paths, TEXT(", "));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeployRootCrateTest, "CrowdySDK.CrowdyExecEditor.DeployRootCrate", CrowdyExecDeployTest::TestFlags)
bool FCrowdyExecDeployRootCrateTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeployTest;
	const FBuildCrate Root = CrowdyExecDeploy::MakeRootCrate();
	TestEqualSensitive(TEXT("the root crate is named root"), Root.Name, FString(CrowdyExecDeploy::RootTypeName));
	TestEqualSensitive(TEXT("the root crate has a Cargo.toml and a lib.rs"), FilePaths(Root), FString(TEXT("Cargo.toml, src/lib.rs")));
	const FBuildFile* Lib = FindBuildFile(Root, TEXT("src/lib.rs"));
	const FBuildFile* Cargo = FindBuildFile(Root, TEXT("Cargo.toml"));
	if (!TestNotNull(TEXT("the root has a lib.rs"), Lib) || !TestNotNull(TEXT("the root has a Cargo.toml"), Cargo))
	{
		return false;
	}
	for (const TCHAR* Part : {
		TEXT("use ckx_sdk::prelude::*;\n"),
		TEXT("#[derive(Serialize, Deserialize, Default)]\npub struct Root {}\n"),
		TEXT("\nimpl Hub for Root {\n"),
		TEXT("    fn spawn(_ctx: &Ctx, _seed: &[u8]) -> Result<Self> {\n        Ok(Self::default())\n    }\n"),
		TEXT("    fn load(_ctx: &Ctx, _snapshot: &[u8], _from_version: u64) -> Result<Self> {\n        Ok(Self::default())\n    }\n"),
		TEXT("    fn persist(&mut self, _ctx: &Ctx) -> Result<Vec<u8>> {\n        encode(self)\n    }\n"),
		TEXT("    fn handle(&mut self, _ctx: &Ctx, call: Call<'_>) -> Result<Vec<u8>> {\n        Err(Error::unknown_method(call.method))\n    }\n}\n"),
		TEXT("\nckx_sdk::export_hub!(Root);\n")})
	{
		TestTrue(FString::Printf(TEXT("the root's lib.rs has: %s"), Part), Lib->Content.Contains(Part, ESearchCase::CaseSensitive));
	}
	TestFalse(TEXT("the root's lib.rs has no carriage returns"), Lib->Content.Contains(TEXT("\r")));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, FixtureType);
	CrowdyExecCodegen::FGeneratedCrate Generated;
	if (!Definition || !GenerateCrate(*this, *Definition, Generated))
	{
		return false;
	}
	const FString* GeneratedCargo = FindGeneratedText(Generated, TEXT("Cargo.toml"));
	if (!TestNotNull(TEXT("the generated crate has a Cargo.toml"), GeneratedCargo))
	{
		return false;
	}
	const FString Renamed = GeneratedCargo->Replace(TEXT("name = \"deploy_fixture\""), TEXT("name = \"root\""), ESearchCase::CaseSensitive);
	TestEqualSensitive(TEXT("the root's Cargo.toml is a generated one with the root's name"), Cargo->Content, Renamed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeployManifestTest, "CrowdySDK.CrowdyExecEditor.DeployManifestMatchesGolden", CrowdyExecDeployTest::TestFlags)
bool FCrowdyExecDeployManifestTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeployTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Locker = MakeManifestDefinition(TEXT("sbx_locker"), 5, 600);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Arena = MakeManifestDefinition(TEXT("arena"), 30, 300);
	const FString Manifest = CrowdyExecDeploy::MakeManifest({Locker.Get(), Arena.Get()});
	TestEqualSensitive(TEXT("the manifest is the same each time"), CrowdyExecDeploy::MakeManifest({Locker.Get(), Arena.Get()}), Manifest);
	TestEqualSensitive(TEXT("the manifest does not depend on the definitions' order"), CrowdyExecDeploy::MakeManifest({Arena.Get(), Locker.Get()}), Manifest);
	TestTrue(TEXT("the root comes first, before a type whose name sorts earlier"),
		Manifest.StartsWith(TEXT("{\"root\":\"root\",\"types\":{\"root\":{\"kind\":\"hub\",\"crate\":\"root\",\"client\":false},\"arena\":"), ESearchCase::CaseSensitive));

	TSharedPtr<FJsonObject> Parsed;
	const bool bParsed = FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Manifest), Parsed) && Parsed.IsValid();
	if (TestTrue(TEXT("the manifest is valid JSON"), bParsed))
	{
		const TSharedPtr<FJsonObject>* Types = nullptr;
		TestTrue(TEXT("the manifest names the root, sbx_locker and arena"), Parsed->TryGetObjectField(TEXT("types"), Types) && (*Types)->Values.Num() == 3);
	}

	const FString GoldenPath = GetManifestGoldenPath();
	if (!TestFalse(TEXT("the CrowdySDK plugin is found"), GoldenPath.IsEmpty()))
	{
		return false;
	}
	FString Golden;
	if (!FFileHelper::LoadFileToString(Golden, *GoldenPath))
	{
		SaveText(Manifest, GoldenPath);
		AddError(FString::Printf(TEXT("recorded %s; review it and run again"), *GoldenPath));
		return false;
	}
	TestEqualSensitive(FString::Printf(TEXT("the manifest matches %s byte for byte"), *GoldenPath), Manifest, Golden);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeployManifestCallsTest, "CrowdySDK.CrowdyExecEditor.DeployManifestCallsAndScopes", CrowdyExecDeployTest::TestFlags)
bool FCrowdyExecDeployManifestCallsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeployTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Locker = MakeManifestDefinition(TEXT("sbx_locker"), 5, 600);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Arena = MakeManifestDefinition(TEXT("arena"), 30, 300);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Unnamed = MakeManifestDefinition(TEXT(""), 30, 300);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> NotDeployed = MakeManifestDefinition(TEXT("boss"), 30, 300);
	const FString Plain = CrowdyExecDeploy::MakeManifest({Locker.Get(), Arena.Get()});

	Locker->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	Locker->CanCall.Add(nullptr);
	Locker->CanCall.Add(Unnamed.Get());
	TestEqualSensitive(TEXT("members of its own and Can Call entries that name no type add nothing"), CrowdyExecDeploy::MakeManifest({Locker.Get(), Arena.Get()}), Plain);
	const FString GoldenPath = GetManifestGoldenPath();
	if (FPaths::FileExists(GoldenPath))
	{
		TestEqualSensitive(TEXT("a manifest with no calls or scopes still matches the golden"), CrowdyExecDeploy::MakeManifest({Locker.Get(), Arena.Get()}), LoadText(GoldenPath));
	}

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Guild = MakeManifestDefinition(TEXT("guild"), 10, 60);
	Guild->MembersFrom = ECrowdyServerMembersSource::CrowdyTeam;
	for (UCrowdyServerObjectDefinition* Target : {Locker.Get(), Arena.Get(), NotDeployed.Get(), Arena.Get(), Unnamed.Get()})
	{
		Guild->CanCall.Add(Target);
	}
	Guild->CanCall.Add(nullptr);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Bank = MakeManifestDefinition(TEXT("team_bank"), 30, 300);
	Bank->MembersFrom = ECrowdyServerMembersSource::CrowdyTeam;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Lobby = MakeManifestDefinition(TEXT("lobby"), 30, 300);
	Lobby->MembersFrom = ECrowdyServerMembersSource::ThisObject;
	Lobby->CanCall.Add(Guild.Get());

	const FString Manifest = CrowdyExecDeploy::MakeManifest({Locker.Get(), Guild.Get(), Bank.Get(), Lobby.Get(), Arena.Get()});
	TestTrue(TEXT("calls are sorted and listed once, a type not being deployed included, and a Crowdy Team type reads players"), Manifest.Contains(
		TEXT("\"guild\":{\"kind\":\"hub\",\"parent\":\"root\",\"crate\":\"guild\",\"client\":true,\"persist_every_ms\":10000,\"evict_after_ms\":60000,")
		TEXT("\"calls\":[\"arena\",\"boss\",\"sbx_locker\"],\"scopes\":[\"players.read\"]},"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("a Crowdy Team type that calls nothing gets scopes and no calls"), Manifest.Contains(
		TEXT("\"team_bank\":{\"kind\":\"hub\",\"parent\":\"root\",\"crate\":\"team_bank\",\"client\":true,\"persist_every_ms\":30000,\"evict_after_ms\":300000,\"scopes\":[\"players.read\"]}}}"),
		ESearchCase::CaseSensitive));
	TestTrue(TEXT("a type with members of its own that calls another gets calls and no scopes"), Manifest.Contains(
		TEXT("\"lobby\":{\"kind\":\"hub\",\"parent\":\"root\",\"crate\":\"lobby\",\"client\":true,\"persist_every_ms\":30000,\"evict_after_ms\":300000,\"calls\":[\"guild\"]},"),
		ESearchCase::CaseSensitive));
	TestTrue(TEXT("a type that calls nothing is unchanged"), Manifest.Contains(
		TEXT("\"arena\":{\"kind\":\"hub\",\"parent\":\"root\",\"crate\":\"arena\",\"client\":true,\"persist_every_ms\":30000,\"evict_after_ms\":300000},"), ESearchCase::CaseSensitive));

	Guild->CanCall.Empty();
	for (UCrowdyServerObjectDefinition* Target : {NotDeployed.Get(), Arena.Get(), Locker.Get()})
	{
		Guild->CanCall.Add(Target);
	}
	TestEqualSensitive(TEXT("the manifest does not depend on the Can Call order"),
		CrowdyExecDeploy::MakeManifest({Locker.Get(), Guild.Get(), Bank.Get(), Lobby.Get(), Arena.Get()}), Manifest);

	TSharedPtr<FJsonObject> Parsed;
	const bool bParsed = FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Manifest), Parsed) && Parsed.IsValid();
	if (!TestTrue(TEXT("the manifest is valid JSON"), bParsed))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Types = nullptr;
	const TSharedPtr<FJsonObject>* GuildType = nullptr;
	TArray<FString> Calls;
	const bool bRead = Parsed->TryGetObjectField(TEXT("types"), Types) && (*Types)->TryGetObjectField(TEXT("guild"), GuildType)
		&& (*GuildType)->TryGetStringArrayField(TEXT("calls"), Calls);
	TestTrue(TEXT("guild's calls read back as three type names"), bRead && Calls.Num() == 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeployInspectTest, "CrowdySDK.CrowdyExecEditor.DeployInspectType", CrowdyExecDeployTest::TestFlags)
bool FCrowdyExecDeployInspectTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeployTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, FixtureType);
	if (!Definition)
	{
		return false;
	}
	FTempFolder Folder;
	const FString Directory = FPaths::Combine(Folder.Path, FixtureType);
	TestTrue(TEXT("the definition starts without field tables"), Definition->BakedStructs.IsEmpty());
	TestState(*this, TEXT("a type with no folder is not generated"), CrowdyExecDeploy::InspectType(*Definition, Directory), ECrateState::NotGenerated);
	TestFalse(TEXT("inspecting bakes a definition saved without field tables"), Definition->BakedStructs.IsEmpty());

	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!WriteGenerated(*this, *Definition, Directory, Crate))
	{
		return false;
	}
	const CrowdyExecDeploy::FTypeState Written = CrowdyExecDeploy::InspectType(*Definition, Directory);
	TestState(*this, TEXT("a freshly generated type is up to date"), Written, ECrateState::UpToDate);
	TestTrue(TEXT("an up-to-date type has no problem"), Written.Problem.IsEmpty());
	TestEqualSensitive(TEXT("the state names the type"), Written.TypeName, FString(FixtureType));
	TestEqualSensitive(TEXT("the state names the asset"), Written.AssetPath, Definition->GetPathName());

	const FString TypesFile = FPaths::Combine(Directory, TypesPath);
	const FString Types = LoadText(TypesFile);
	SaveText(Types + TEXT("// edited by hand\n"), TypesFile);
	const CrowdyExecDeploy::FTypeState Edited = CrowdyExecDeploy::InspectType(*Definition, Directory);
	TestState(*this, TEXT("a changed types.rs is out of date"), Edited, ECrateState::OutOfDate);
	TestTrue(FString::Printf(TEXT("the problem names the file (%s)"), *Edited.Problem), Edited.Problem.Contains(TEXT("types.rs")));
	TestTrue(FString::Printf(TEXT("the problem says to regenerate (%s)"), *Edited.Problem), Edited.Problem.Contains(TEXT("Generate Server Code")));
	SaveText(Types + TEXT("\n\n  \t\n"), TypesFile);
	TestState(*this, TEXT("blank lines and spaces at the end of types.rs keep it up to date"), CrowdyExecDeploy::InspectType(*Definition, Directory), ECrateState::UpToDate);
	SaveText(Types.TrimEnd(), TypesFile);
	TestState(*this, TEXT("a types.rs without its last newline is up to date"), CrowdyExecDeploy::InspectType(*Definition, Directory), ECrateState::UpToDate);
	SaveText(Types, TypesFile);

	const FString LogicFile = FPaths::Combine(Directory, LogicPath);
	const FString Logic = LoadText(LogicFile);
	IFileManager::Get().Delete(*LogicFile);
	const CrowdyExecDeploy::FTypeState NoLogic = CrowdyExecDeploy::InspectType(*Definition, Directory);
	TestState(*this, TEXT("a type without logic.rs is not generated"), NoLogic, ECrateState::NotGenerated);
	TestTrue(FString::Printf(TEXT("the problem names logic.rs (%s)"), *NoLogic.Problem), NoLogic.Problem.Contains(TEXT("logic.rs")));
	SaveText(Logic + TEXT("// the user's own code\n"), LogicFile);
	TestState(*this, TEXT("an edited logic.rs is the user's and keeps the type up to date"), CrowdyExecDeploy::InspectType(*Definition, Directory), ECrateState::UpToDate);

	for (const CrowdyExecCodegen::FGeneratedFile& File : Crate.Files)
	{
		SaveText(File.Text.Replace(TEXT("\n"), TEXT("\r\n")), FPaths::Combine(Directory, File.Path));
	}
	TestTrue(TEXT("the files now have CRLF line endings"), LoadText(TypesFile).Contains(TEXT("\r\n")));
	TestState(*this, TEXT("CRLF files are up to date"), CrowdyExecDeploy::InspectType(*Definition, Directory), ECrateState::UpToDate);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeployInvalidTest, "CrowdySDK.CrowdyExecEditor.DeployInspectInvalid", CrowdyExecDeployTest::TestFlags)
bool FCrowdyExecDeployInvalidTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeployTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Root = MakeDefinition(*this, CrowdyExecDeploy::RootTypeName);
	if (!Root)
	{
		return false;
	}
	FTempFolder Folder;
	CrowdyExecCodegen::FGeneratedCrate Crate;
	const FString RootDirectory = FPaths::Combine(Folder.Path, CrowdyExecDeploy::RootTypeName);
	if (!WriteGenerated(*this, *Root, RootDirectory, Crate))
	{
		return false;
	}
	const CrowdyExecDeploy::FTypeState RootState = CrowdyExecDeploy::InspectType(*Root, RootDirectory);
	TestState(*this, TEXT("a type named root is invalid even with its server code written"), RootState, ECrateState::Invalid);
	TestTrue(FString::Printf(TEXT("the problem says why (%s)"), *RootState.Problem),
		RootState.Problem.Equals(TEXT("root is the name of the platform's root Server Object type; choose another Type Name"), ESearchCase::CaseSensitive));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> NoState = MakeDefinition(*this, TEXT("no_state"));
	if (!NoState)
	{
		return false;
	}
	NoState->State = nullptr;
	const CrowdyExecDeploy::FTypeState Unbaked = CrowdyExecDeploy::InspectType(*NoState, FPaths::Combine(Folder.Path, TEXT("no_state")));
	TestState(*this, TEXT("a definition that does not bake is invalid"), Unbaked, ECrateState::Invalid);
	TestTrue(FString::Printf(TEXT("the problem is the bake's (%s)"), *Unbaked.Problem), Unbaked.Problem.Contains(TEXT("State")));

	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Timed = MakeDefinition(*this, TEXT("timed"));
	if (!Timed)
	{
		return false;
	}
	const FString TimedDirectory = FPaths::Combine(Folder.Path, TEXT("timed"));
	auto InspectWith = [&Timed, &TimedDirectory](int32 SaveIntervalSeconds, int32 IdleTimeoutSeconds)
	{
		Timed->SaveIntervalSeconds = SaveIntervalSeconds;
		Timed->IdleTimeoutSeconds = IdleTimeoutSeconds;
		return CrowdyExecDeploy::InspectType(*Timed, TimedDirectory);
	};
	const CrowdyExecDeploy::FTypeState SavesTooOften = InspectWith(4, 300);
	TestState(*this, TEXT("a Save Interval under 5 seconds is invalid"), SavesTooOften, ECrateState::Invalid);
	TestEqualSensitive(TEXT("the problem gives the Save Interval's range"), SavesTooOften.Problem, FString(TEXT("Save Interval must be 5 to 60 seconds")));
	TestState(*this, TEXT("a Save Interval over 60 seconds is invalid"), InspectWith(61, 300), ECrateState::Invalid);
	TestState(*this, TEXT("a Save Interval of 5 seconds is allowed"), InspectWith(5, 300), ECrateState::NotGenerated);
	TestState(*this, TEXT("a Save Interval of 60 seconds is allowed"), InspectWith(60, 300), ECrateState::NotGenerated);
	const CrowdyExecDeploy::FTypeState NeverIdle = InspectWith(30, 0);
	TestState(*this, TEXT("an Idle Timeout under 1 second is invalid"), NeverIdle, ECrateState::Invalid);
	TestEqualSensitive(TEXT("the problem gives the Idle Timeout's range"), NeverIdle.Problem, FString(TEXT("Idle Timeout must be 1 to 1800 seconds")));
	TestState(*this, TEXT("an Idle Timeout over 1800 seconds is invalid"), InspectWith(30, 1801), ECrateState::Invalid);
	TestState(*this, TEXT("an Idle Timeout of 1 second is allowed"), InspectWith(30, 1), ECrateState::NotGenerated);
	TestState(*this, TEXT("an Idle Timeout of 1800 seconds is allowed"), InspectWith(30, 1800), ECrateState::NotGenerated);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeployLimitsTest, "CrowdySDK.CrowdyExecEditor.DeployBuildLimits", CrowdyExecDeployTest::TestFlags)
bool FCrowdyExecDeployLimitsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeployTest;
	constexpr int32 TwoMegabytes = 2 * 1024 * 1024;
	FString Problem;
	TestTrue(TEXT("16 crates are allowed"), CrowdyExecDeploy::CheckBuildLimits(MakeCrates(16, 1, 1), Problem));
	TestFalse(TEXT("17 crates are refused"), CrowdyExecDeploy::CheckBuildLimits(MakeCrates(17, 1, 1), Problem));
	TestTrue(FString::Printf(TEXT("the crate refusal names the platform's root Server Object type (%s)"), *Problem), Problem.Contains(TEXT("the platform's root Server Object type")));

	Problem.Reset();
	TestTrue(TEXT("64 files in a crate are allowed"), CrowdyExecDeploy::CheckBuildLimits(MakeCrates(2, 64, 1), Problem));
	TestFalse(TEXT("65 files in a crate are refused"), CrowdyExecDeploy::CheckBuildLimits(MakeCrates(2, 65, 1), Problem));
	TestTrue(FString::Printf(TEXT("the file refusal names the crate (%s)"), *Problem), Problem.Contains(TEXT("crate_0")));

	Problem.Reset();
	TestTrue(TEXT("2 MB of source is allowed"), CrowdyExecDeploy::CheckBuildLimits(MakeCrates(1, 1, TwoMegabytes), Problem));
	TestFalse(TEXT("one byte over 2 MB is refused"), CrowdyExecDeploy::CheckBuildLimits(MakeCrates(1, 1, TwoMegabytes + 1), Problem));
	TestFalse(TEXT("2 MB spread over two crates plus one byte is refused"), CrowdyExecDeploy::CheckBuildLimits(MakeCrates(2, 1, TwoMegabytes / 2 + 1), Problem));
	TestTrue(FString::Printf(TEXT("the size refusal names the limit (%s)"), *Problem), Problem.Contains(TEXT("2 MB")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeployAssembleTest, "CrowdySDK.CrowdyExecEditor.DeployAssembleReadsOnlySource", CrowdyExecDeployTest::TestFlags)
bool FCrowdyExecDeployAssembleTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeployTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Fixture = MakeDefinition(*this, FixtureType);
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Missing = MakeDefinition(*this, TEXT("deploy_missing"));
	if (!Fixture || !Missing)
	{
		return false;
	}
	FTempFolder Folder;
	const FString Directory = FPaths::Combine(Folder.Path, FixtureType);
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!WriteGenerated(*this, *Fixture, Directory, Crate))
	{
		return false;
	}
	const FString Nested = TEXT("pub fn helper() {}\n");
	SaveText(Nested, FPaths::Combine(Directory, TEXT("src/nested/more.rs")));
	SaveText(TEXT("fn built() {}\n"), FPaths::Combine(Directory, TEXT("target/x.rs")));
	const FString TargetModule = TEXT("pub fn aim() {}\n");
	SaveText(TargetModule, FPaths::Combine(Directory, TEXT("src/target/y.rs")));
	SaveText(TEXT("# lock\n"), FPaths::Combine(Directory, TEXT("Cargo.lock")));
	SaveText(TEXT("notes\n"), FPaths::Combine(Directory, TEXT("src/notes.txt")));
	auto DirectoryFor = [&Folder](const UCrowdyServerObjectDefinition& Definition) { return FPaths::Combine(Folder.Path, Definition.TypeName); };

	const CrowdyExecDeploy::FProjectDeploy Ready = CrowdyExecDeploy::AssembleProject({Fixture.Get()}, DirectoryFor);
	TestEqualSensitive(TEXT("an up-to-date project has no problems"), FString::Join(Ready.Problems, TEXT(" | ")), FString());
	if (!TestEqual(TEXT("the build holds the root and the type"), Ready.Crates.Num(), 2))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the root comes first"), Ready.Crates[0].Name, FString(CrowdyExecDeploy::RootTypeName));
	const FBuildCrate& Built = Ready.Crates[1];
	TestEqualSensitive(TEXT("the type's crate is named after it"), Built.Name, FString(FixtureType));
	TestEqualSensitive(TEXT("only Cargo.toml and the rs files under src are sent, src sorted, build output left out"), FilePaths(Built),
		FString(TEXT("Cargo.toml, src/lib.rs, src/logic.rs, src/nested/more.rs, src/target/y.rs, src/types.rs")));
	const FBuildFile* More = FindBuildFile(Built, TEXT("src/nested/more.rs"));
	TestTrue(TEXT("a nested source file is sent as it is on disk"), More && More->Content.Equals(Nested, ESearchCase::CaseSensitive));
	const FBuildFile* Target = FindBuildFile(Built, TEXT("src/target/y.rs"));
	TestTrue(TEXT("a module named target under src is source and is sent"), Target && Target->Content.Equals(TargetModule, ESearchCase::CaseSensitive));
	const FBuildFile* Types = FindBuildFile(Built, TypesPath);
	const FString* GeneratedTypes = FindGeneratedText(Crate, TypesPath);
	TestTrue(TEXT("types.rs is sent as generated"), Types && GeneratedTypes && Types->Content.Equals(*GeneratedTypes, ESearchCase::CaseSensitive));
	TestEqualSensitive(TEXT("the manifest names the project's types"), Ready.ManifestJson, CrowdyExecDeploy::MakeManifest({Fixture.Get()}));

	const CrowdyExecDeploy::FProjectDeploy Partial = CrowdyExecDeploy::AssembleProject({Missing.Get(), Fixture.Get()}, DirectoryFor);
	TestEqual(TEXT("every type is inspected"), Partial.Types.Num(), 2);
	TestTrue(TEXT("types keep the definitions' order"), Partial.Types.Num() == 2 && Partial.Types[0].TypeName == TEXT("deploy_missing"));
	TestEqual(TEXT("a type without server code is one problem"), Partial.Problems.Num(), 1);
	TestTrue(TEXT("the problem names the type"), Partial.Problems.Num() == 1 && Partial.Problems[0].StartsWith(TEXT("deploy_missing: "), ESearchCase::CaseSensitive));
	TestEqual(TEXT("a type that cannot be built sends no crate"), Partial.Crates.Num(), 2);

	const CrowdyExecDeploy::FProjectDeploy Twice = CrowdyExecDeploy::AssembleProject({Fixture.Get(), Fixture.Get()}, DirectoryFor);
	TestTrue(FString::Printf(TEXT("two types with one name are a problem (%s)"), *FString::Join(Twice.Problems, TEXT(" | "))),
		Twice.Problems.ContainsByPredicate([](const FString& Problem) { return Problem.StartsWith(TEXT("Two Server Object types are named deploy_fixture"), ESearchCase::CaseSensitive); }));

	const CrowdyExecDeploy::FProjectDeploy Empty = CrowdyExecDeploy::AssembleProject({}, DirectoryFor);
	TestEqualSensitive(TEXT("a project with no types cannot deploy"), FString::Join(Empty.Problems, TEXT(" | ")), FString(TEXT("The project has no Server Object types")));
	TestEqual(TEXT("a project with no types sends nothing"), Empty.Crates.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeployAssembleDropsRecordTest, "CrowdySDK.CrowdyExecEditor.DeployAssembleSendsNoDefinitionRecord", CrowdyExecDeployTest::TestFlags)
bool FCrowdyExecDeployAssembleDropsRecordTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeployTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Fixture = MakeDefinition(*this, FixtureType);
	if (!Fixture)
	{
		return false;
	}
	Fixture->Rename(nullptr, CreatePackage(TEXT("/Temp/CrowdyExecDeployTests/DA_DeployTagged")), REN_DontCreateRedirectors | REN_NonTransactional);
	FTempFolder Folder;
	const FString Directory = FPaths::Combine(Folder.Path, FixtureType);
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!WriteGenerated(*this, *Fixture, Directory, Crate))
	{
		return false;
	}
	if (!TestEqualSensitive(TEXT("the crate on disk records its definition asset"), CrowdyExecCodegen::ReadCrateDefinition(Directory), Fixture->GetPathName()))
	{
		return false;
	}
	auto DirectoryFor = [&Folder](const UCrowdyServerObjectDefinition& Definition) { return FPaths::Combine(Folder.Path, Definition.TypeName); };

	const CrowdyExecDeploy::FProjectDeploy Project = CrowdyExecDeploy::AssembleProject({Fixture.Get()}, DirectoryFor);
	TestEqualSensitive(TEXT("a crate with its record is up to date"), FString::Join(Project.Problems, TEXT(" | ")), FString());
	const FBuildFile* Cargo = Project.Crates.Num() == 2 ? FindBuildFile(Project.Crates[1], TEXT("Cargo.toml")) : nullptr;
	if (!TestNotNull(TEXT("Cargo.toml is sent"), Cargo))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the platform gets only package, lib and dependencies, as before the record"), Cargo->Content,
		FString(TEXT("[package]\nname = \"deploy_fixture\"\nversion = \"0.1.0\"\nedition = \"2024\"\n\n[lib]\ncrate-type = [\"cdylib\"]\n\n")
			TEXT("[dependencies]\nckx-sdk = \"0.7.0\"\nserde = { version = \"1\", features = [\"derive\"] }\n")));

	TestEqualSensitive(TEXT("CRLF lines and a record as the last section"),
		CrowdyExecCodegen::WithoutCrateDefinition(TEXT("[package]\r\nname = \"x\"\r\n\r\n[package.metadata.crowdy]\r\ndefinition = \"/Game/A.A\"\r\n")),
		FString(TEXT("[package]\r\nname = \"x\"\r\n\r\n")));
	TestEqualSensitive(TEXT("a Cargo.toml without a record is unchanged"),
		CrowdyExecCodegen::WithoutCrateDefinition(TEXT("[package]\nname = \"x\"\n\n[lib]\ncrate-type = [\"cdylib\"]")),
		FString(TEXT("[package]\nname = \"x\"\n\n[lib]\ncrate-type = [\"cdylib\"]")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeployInspectOwnLogicTest, "CrowdySDK.CrowdyExecEditor.DeployInspectOwnLogicFile", CrowdyExecDeployTest::TestFlags)
bool FCrowdyExecDeployInspectOwnLogicTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeployTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, FixtureType);
	if (!Definition)
	{
		return false;
	}
	FTempFolder Folder;
	const FString Directory = FPaths::Combine(Folder.Path, FixtureType);
	const FString OwnLogic = FPaths::Combine(Folder.Path, TEXT("elsewhere/my_logic.rs"));
	Definition->CodeSource = ECrowdyServerCodeSource::OwnFile;
	Definition->LogicFile.FilePath = OwnLogic;
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!WriteGenerated(*this, *Definition, Directory, Crate))
	{
		return false;
	}
	TestFalse(TEXT("the crate folder gets no logic.rs of its own"), FPaths::FileExists(FPaths::Combine(Directory, LogicPath)));

	const CrowdyExecDeploy::FTypeState Missing = CrowdyExecDeploy::InspectType(*Definition, Directory);
	TestState(*this, TEXT("a missing Logic File is not generated"), Missing, ECrateState::NotGenerated);
	TestTrue(FString::Printf(TEXT("the problem names the Logic File (%s)"), *Missing.Problem), Missing.Problem.Contains(OwnLogic));
	TestTrue(FString::Printf(TEXT("the problem says to choose a file or switch to Generated (%s)"), *Missing.Problem), Missing.Problem.Contains(TEXT("switch Code Source to Generated")));

	SaveText(TEXT("// the crate's own copy, not used while a Logic File is set\n"), FPaths::Combine(Directory, LogicPath));
	TestState(*this, TEXT("a logic.rs in the crate does not stand in for a missing Logic File"), CrowdyExecDeploy::InspectType(*Definition, Directory), ECrateState::NotGenerated);

	SaveText(TEXT("// the user's own server code\n"), OwnLogic);
	TestState(*this, TEXT("an existing Logic File makes the type up to date"), CrowdyExecDeploy::InspectType(*Definition, Directory), ECrateState::UpToDate);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeployInspectNoFileChosenTest, "CrowdySDK.CrowdyExecEditor.DeployInspectOwnFileNotChosen", CrowdyExecDeployTest::TestFlags)
bool FCrowdyExecDeployInspectNoFileChosenTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeployTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, FixtureType);
	if (!Definition)
	{
		return false;
	}
	FTempFolder Folder;
	const FString Directory = FPaths::Combine(Folder.Path, FixtureType);
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!WriteGenerated(*this, *Definition, Directory, Crate))
	{
		return false;
	}
	const FString Expected = TEXT("Code Source is My Own File but no file is chosen; choose one, or switch Code Source to Generated");
	Definition->CodeSource = ECrowdyServerCodeSource::OwnFile;
	Definition->LogicFile.FilePath.Reset();
	const CrowdyExecDeploy::FTypeState Unchosen = CrowdyExecDeploy::InspectType(*Definition, Directory);
	TestState(*this, TEXT("My Own File with no file chosen is not generated"), Unchosen, ECrateState::NotGenerated);
	TestEqualSensitive(TEXT("the problem says no file is chosen and how to fix it"), Unchosen.Problem, Expected);

	auto DirectoryFor = [&Folder](const UCrowdyServerObjectDefinition& Def) { return FPaths::Combine(Folder.Path, Def.TypeName); };
	const CrowdyExecDeploy::FProjectDeploy Project = CrowdyExecDeploy::AssembleProject({Definition.Get()}, DirectoryFor);
	TestEqualSensitive(TEXT("the deploy names the type and the missing choice"), FString::Join(Project.Problems, TEXT(" | ")), FString(FixtureType) + TEXT(": ") + Expected);
	TestEqual(TEXT("a type with no file chosen sends no crate"), Project.Crates.Num(), 1);

	Definition->CodeSource = ECrowdyServerCodeSource::Generated;
	TestState(*this, TEXT("switching back to Generated uses the crate's logic.rs"), CrowdyExecDeploy::InspectType(*Definition, Directory), ECrateState::UpToDate);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeployAssembleOwnLogicTest, "CrowdySDK.CrowdyExecEditor.DeployAssembleSendsOwnLogicFile", CrowdyExecDeployTest::TestFlags)
bool FCrowdyExecDeployAssembleOwnLogicTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeployTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Definition = MakeDefinition(*this, FixtureType);
	if (!Definition)
	{
		return false;
	}
	FTempFolder Folder;
	const FString Directory = FPaths::Combine(Folder.Path, FixtureType);
	const FString Outside = FPaths::Combine(Folder.Path, TEXT("elsewhere/my_logic.rs"));
	const FString OutsideText = TEXT("// kept outside the crate\n");
	Definition->CodeSource = ECrowdyServerCodeSource::OwnFile;
	Definition->LogicFile.FilePath = Outside;
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!WriteGenerated(*this, *Definition, Directory, Crate))
	{
		return false;
	}
	SaveText(OutsideText, Outside);
	SaveText(TEXT("// a stale copy in the crate\n"), FPaths::Combine(Directory, LogicPath));
	auto DirectoryFor = [&Folder](const UCrowdyServerObjectDefinition& Def) { return FPaths::Combine(Folder.Path, Def.TypeName); };

	const CrowdyExecDeploy::FProjectDeploy FromOutside = CrowdyExecDeploy::AssembleProject({Definition.Get()}, DirectoryFor);
	TestEqualSensitive(TEXT("a type with an existing Logic File has no problems"), FString::Join(FromOutside.Problems, TEXT(" | ")), FString());
	if (!TestEqual(TEXT("the build holds the root and the type"), FromOutside.Crates.Num(), 2))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the crate sends one logic.rs"), FilePaths(FromOutside.Crates[1]), FString(TEXT("Cargo.toml, src/lib.rs, src/logic.rs, src/types.rs")));
	const FBuildFile* SentOutside = FindBuildFile(FromOutside.Crates[1], LogicPath);
	TestTrue(TEXT("logic.rs carries the Logic File, not the crate's copy"), SentOutside && SentOutside->Content.Equals(OutsideText, ESearchCase::CaseSensitive));

	const FString Inside = FPaths::Combine(Directory, TEXT("src/mine.rs"));
	const FString InsideText = TEXT("// kept in the crate under another name\n");
	SaveText(InsideText, Inside);
	Definition->LogicFile.FilePath = Inside;
	const CrowdyExecDeploy::FProjectDeploy FromInside = CrowdyExecDeploy::AssembleProject({Definition.Get()}, DirectoryFor);
	if (!TestEqual(TEXT("the build still holds the root and the type"), FromInside.Crates.Num(), 2))
	{
		return false;
	}
	TestEqualSensitive(TEXT("a Logic File inside the crate is sent once, as logic.rs"), FilePaths(FromInside.Crates[1]),
		FString(TEXT("Cargo.toml, src/lib.rs, src/logic.rs, src/types.rs")));
	const FBuildFile* SentInside = FindBuildFile(FromInside.Crates[1], LogicPath);
	TestTrue(TEXT("logic.rs carries the file the Logic File names"), SentInside && SentInside->Content.Equals(InsideText, ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyExecDeployAssembleGuardsTest, "CrowdySDK.CrowdyExecEditor.DeployAssembleSkipsLinksAndOversizedFiles", CrowdyExecDeployTest::TestFlags)
bool FCrowdyExecDeployAssembleGuardsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyExecDeployTest;
	const TStrongObjectPtr<UCrowdyServerObjectDefinition> Fixture = MakeDefinition(*this, FixtureType);
	if (!Fixture)
	{
		return false;
	}
	FTempFolder Folder;
	const FString Directory = FPaths::Combine(Folder.Path, FixtureType);
	CrowdyExecCodegen::FGeneratedCrate Crate;
	if (!WriteGenerated(*this, *Fixture, Directory, Crate))
	{
		return false;
	}
	auto DirectoryFor = [&Folder](const UCrowdyServerObjectDefinition& Definition) { return FPaths::Combine(Folder.Path, Definition.TypeName); };

#if PLATFORM_WINDOWS
	const FString Outside = FPaths::Combine(Folder.Path, TEXT("outside"));
	SaveText(TEXT("pub fn elsewhere() {}\n"), FPaths::Combine(Outside, TEXT("elsewhere.rs")));
	const FString Link = FPaths::Combine(Directory, TEXT("src/linked"));
	int32 ReturnCode = -1;
	FPlatformProcess::ExecProcess(TEXT("cmd.exe"), *FString::Printf(TEXT("/c mklink /J \"%s\" \"%s\""), *Link.Replace(TEXT("/"), TEXT("\\")), *Outside.Replace(TEXT("/"), TEXT("\\"))),
		&ReturnCode, nullptr, nullptr);
	if (ReturnCode == 0 && IFileManager::Get().IsSymlink(*Link))
	{
		const CrowdyExecDeploy::FProjectDeploy Linked = CrowdyExecDeploy::AssembleProject({Fixture.Get()}, DirectoryFor);
		TestEqualSensitive(TEXT("a project with a linked folder has no problems"), FString::Join(Linked.Problems, TEXT(" | ")), FString());
		TestTrue(TEXT("a file under a junction in src is not sent"), Linked.Crates.Num() == 2 && FindBuildFile(Linked.Crates[1], TEXT("src/linked/elsewhere.rs")) == nullptr);
	}
	else
	{
		AddInfo(TEXT("mklink /J did not make a junction here, so the linked folder case did not run"));
	}
#endif

	SaveText(FString::ChrN(2 * 1024 * 1024 + 1, TEXT('a')), FPaths::Combine(Directory, TEXT("src/big.rs")));
	const CrowdyExecDeploy::FProjectDeploy Oversized = CrowdyExecDeploy::AssembleProject({Fixture.Get()}, DirectoryFor);
	TestTrue(FString::Printf(TEXT("a file past the source limit is named before it is read (%s)"), *FString::Join(Oversized.Problems, TEXT(" | "))),
		Oversized.Problems.ContainsByPredicate([](const FString& Problem) { return Problem.Contains(TEXT("big.rs")) && Problem.Contains(TEXT("2 MB")); }));
	TestEqual(TEXT("a type with a file past the limit sends no crate"), Oversized.Crates.Num(), 1);
	return true;
}

#endif
