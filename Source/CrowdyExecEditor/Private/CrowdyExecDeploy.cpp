#include "CrowdyExecDeploy.h"

#include "Algo/Find.h"
#include "Algo/Sort.h"
#include "Algo/Unique.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Containers/StringConv.h"
#include "CrowdyExecCodegen.h"
#include "CrowdyRustSyntaxMarshaller.h"
#include "CrowdyServerObjectDefinition.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace CrowdyExecDeployDetail
{
	const TCHAR* const CargoPath = TEXT("Cargo.toml");
	const TCHAR* const LogicPath = TEXT("src/logic.rs");
	const TCHAR* const NoLogicFileChosen = TEXT("Code Source is My Own File but no file is chosen; choose one, or switch Code Source to Generated");

	constexpr int32 MaxCrates = 16;
	constexpr int32 MaxFilesPerCrate = 64;
	constexpr int64 MaxSourceBytes = 2 * 1024 * 1024;

	constexpr int64 MinSaveIntervalMs = 5000;
	constexpr int64 MaxSaveIntervalMs = 60000;
	constexpr int64 MinIdleTimeoutMs = 1000;
	constexpr int64 MaxIdleTimeoutMs = 1800000;

	const TCHAR* const RootLib =
		TEXT("// The root every Server Object type sits under on the platform, written by the Crowdy SDK. It keeps no state.\n")
		TEXT("use ckx_sdk::prelude::*;\n")
		TEXT("\n")
		TEXT("#[derive(Serialize, Deserialize, Default)]\n")
		TEXT("pub struct Root {}\n")
		TEXT("\n")
		TEXT("impl Hub for Root {\n")
		TEXT("    fn spawn(_ctx: &Ctx, _seed: &[u8]) -> Result<Self> {\n")
		TEXT("        Ok(Self::default())\n")
		TEXT("    }\n")
		TEXT("\n")
		TEXT("    fn load(_ctx: &Ctx, _snapshot: &[u8], _from_version: u64) -> Result<Self> {\n")
		TEXT("        Ok(Self::default())\n")
		TEXT("    }\n")
		TEXT("\n")
		TEXT("    fn persist(&mut self, _ctx: &Ctx) -> Result<Vec<u8>> {\n")
		TEXT("        encode(self)\n")
		TEXT("    }\n")
		TEXT("\n")
		TEXT("    fn handle(&mut self, _ctx: &Ctx, call: Call<'_>) -> Result<Vec<u8>> {\n")
		TEXT("        Err(Error::unknown_method(call.method))\n")
		TEXT("    }\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("ckx_sdk::export_hub!(Root);\n");

	/** The same package shape Generate Server Code writes for every type. */
	FString CargoText(const FString& CrateName)
	{
		return FString::Printf(TEXT("[package]\nname = \"%s\"\n"), *CrateName)
			+ TEXT("version = \"0.1.0\"\n")
			TEXT("edition = \"2024\"\n")
			TEXT("\n")
			TEXT("[lib]\n")
			TEXT("crate-type = [\"cdylib\"]\n")
			TEXT("\n")
			TEXT("[dependencies]\n")
			TEXT("ckx-sdk = \"0.7.0\"\n")
			TEXT("serde = { version = \"1\", features = [\"derive\"] }\n");
	}

	/** Line endings and whitespace at the end of the file do not make a crate stale. */
	FString Comparable(const FString& Text)
	{
		return Text.Replace(TEXT("\r\n"), TEXT("\n")).TrimEnd();
	}

	bool IsBefore(const FString& A, const FString& B)
	{
		return A.Compare(B, ESearchCase::CaseSensitive) < 0;
	}

	FString DisplayName(const UCrowdyServerObjectDefinition& Definition)
	{
		return Definition.TypeName.IsEmpty() ? Definition.GetName() : Definition.TypeName;
	}

	CrowdyExecDeploy::FTypeState WithProblem(CrowdyExecDeploy::FTypeState State, CrowdyExecDeploy::ECrateState CrateState, const FString& Problem)
	{
		State.State = CrateState;
		State.Problem = Problem;
		return State;
	}

	/** Bakes a definition saved without field tables, then writes its crate in memory. */
	bool GenerateInMemory(const UCrowdyServerObjectDefinition& Definition, CrowdyExecCodegen::FGeneratedCrate& OutCrate, FString& OutProblem)
	{
		if (Definition.BakedStructs.IsEmpty())
		{
			TArray<FString> Errors;
			if (!const_cast<UCrowdyServerObjectDefinition&>(Definition).Bake(Errors))
			{
				OutProblem = FString::Join(Errors, TEXT("; "));
				return false;
			}
		}
		return CrowdyExecCodegen::Generate(Definition, OutCrate, OutProblem);
	}

	int64 SourceBytesOf(const FString& Content)
	{
		return FTCHARToUTF8(*Content).Length();
	}

	/** Loads a file only when its size fits what is left of the source limit, and takes its size from it. */
	bool LoadWithinBudget(const FString& Path, int64& InOutBudget, FString& OutContent, FString& OutProblem)
	{
		if (IFileManager::Get().FileSize(*Path) > InOutBudget)
		{
			OutProblem = FString::Printf(TEXT("%s takes the server code past 2 MB of source; one deploy builds at most 2 MB"), *Path);
			return false;
		}
		if (!FFileHelper::LoadFileToString(OutContent, *Path))
		{
			OutProblem = FString::Printf(TEXT("could not read %s"), *Path);
			return false;
		}
		InOutBudget -= SourceBytesOf(OutContent);
		return true;
	}

	/** True when the file, or a folder between it and Root, is a symbolic link or junction. */
	bool IsLinked(const FString& Root, const FString& File)
	{
		for (FString Path = File; Path.Len() > Root.Len(); Path = FPaths::GetPath(Path))
		{
			if (IFileManager::Get().IsSymlink(*Path))
			{
				return true;
			}
		}
		return false;
	}

	/** Cargo.toml without its definition record and every .rs file under src, links left out, with forward-slash paths relative to the crate folder; src files sorted. */
	bool ReadCrate(const FString& Name, const FString& Directory, int64& InOutBudget, CrowdyExecDeveloper::FBuildCrate& OutCrate, FString& OutProblem)
	{
		OutCrate.Name = Name;
		FString Root = FPaths::ConvertRelativePathToFull(Directory);
		FPaths::NormalizeDirectoryName(Root);
		const FString Prefix = Root + TEXT("/src/");
		TArray<FString> Found;
		IFileManager::Get().FindFilesRecursive(Found, *(Root + TEXT("/src")), TEXT("*.rs"), true, false);
		TArray<FString> Paths;
		for (const FString& File : Found)
		{
			const FString Full = FPaths::ConvertRelativePathToFull(File).Replace(TEXT("\\"), TEXT("/"));
			if (Full.StartsWith(Prefix) && !IsLinked(Root, Full))
			{
				Paths.Add(Full.RightChop(Root.Len() + 1));
			}
		}
		Algo::Sort(Paths, &IsBefore);
		if (IsLinked(Root, FPaths::Combine(Root, CargoPath)))
		{
			OutProblem = FString::Printf(TEXT("%s/%s is a link; server code is sent only from files in its server code folder"), *Root, CargoPath);
			return false;
		}
		Paths.Insert(FString(CargoPath), 0);
		for (const FString& Path : Paths)
		{
			CrowdyExecDeveloper::FBuildFile& File = OutCrate.Files.AddDefaulted_GetRef();
			File.Path = Path;
			if (!LoadWithinBudget(FPaths::Combine(Root, Path), InOutBudget, File.Content, OutProblem))
			{
				return false;
			}
			if (Path.Equals(CargoPath, ESearchCase::CaseSensitive))
			{
				File.Content = CrowdyExecCodegen::WithoutCrateDefinition(File.Content);
			}
		}
		return true;
	}

	/** Sends the definition's own Logic File as src/logic.rs, whatever its name on disk, in place of any other copy. */
	bool UseOwnLogicFile(const UCrowdyServerObjectDefinition& Definition, const FString& Directory, int64& InOutBudget, CrowdyExecDeveloper::FBuildCrate& Crate, FString& OutProblem)
	{
		if (!CrowdyExecCodegen::HasOwnLogicFile(Definition))
		{
			return true;
		}
		const FString LogicFile = CrowdyExecCodegen::GetLogicFile(Definition, Directory);
		if (LogicFile.IsEmpty())
		{
			OutProblem = NoLogicFileChosen;
			return false;
		}
		Crate.Files.RemoveAll([&Directory, &LogicFile, &InOutBudget](const CrowdyExecDeveloper::FBuildFile& File)
		{
			const bool bReplaced = File.Path.Equals(LogicPath, ESearchCase::CaseSensitive) || FPaths::IsSamePath(FPaths::Combine(Directory, File.Path), LogicFile);
			InOutBudget += bReplaced ? SourceBytesOf(File.Content) : 0;
			return bReplaced;
		});
		FString Content;
		if (!LoadWithinBudget(LogicFile, InOutBudget, Content, OutProblem))
		{
			return false;
		}
		Crate.Files.Add(CrowdyExecDeveloper::FBuildFile{LogicPath, MoveTemp(Content)});
		Algo::Sort(Crate.Files, [](const CrowdyExecDeveloper::FBuildFile& A, const CrowdyExecDeveloper::FBuildFile& B) { return IsBefore(A.Path, B.Path); });
		return true;
	}

	int32 FindSameName(TConstArrayView<const UCrowdyServerObjectDefinition*> Candidates, const FString& TypeName)
	{
		return Candidates.IndexOfByPredicate([&TypeName](const UCrowdyServerObjectDefinition* Other)
		{
			return Other && Other->TypeName.Equals(TypeName, ESearchCase::CaseSensitive);
		});
	}

	void AddDuplicateProblems(TConstArrayView<const UCrowdyServerObjectDefinition*> Definitions, TArray<FString>& OutProblems)
	{
		for (int32 Index = 1; Index < Definitions.Num(); ++Index)
		{
			const UCrowdyServerObjectDefinition* Definition = Definitions[Index];
			const int32 Earlier = Definition ? FindSameName(Definitions.Left(Index), Definition->TypeName) : INDEX_NONE;
			if (Earlier == INDEX_NONE)
			{
				continue;
			}
			OutProblems.Add(FString::Printf(TEXT("Two Server Object types are named %s (%s and %s); each needs its own Type Name"),
				*Definition->TypeName, *Definitions[Earlier]->GetPathName(), *Definition->GetPathName()));
		}
	}

	/** Each type builds its own copy of its Logic File against its own State, so two types cannot use one file. */
	void AddSharedLogicFileProblems(TConstArrayView<const UCrowdyServerObjectDefinition*> Definitions,
		TFunctionRef<FString(const UCrowdyServerObjectDefinition&)> CrateDirectoryFor, TArray<FString>& OutProblems)
	{
		TArray<FString> LogicFiles;
		for (const UCrowdyServerObjectDefinition* Definition : Definitions)
		{
			LogicFiles.Add(Definition ? CrowdyExecCodegen::GetLogicFile(*Definition, CrateDirectoryFor(*Definition)) : FString());
		}
		for (int32 Index = 1; Index < Definitions.Num(); ++Index)
		{
			const FString& LogicFile = LogicFiles[Index];
			const int32 Earlier = LogicFile.IsEmpty() ? INDEX_NONE : LogicFiles.IndexOfByPredicate([&LogicFile](const FString& Other) { return FPaths::IsSamePath(Other, LogicFile); });
			if (Earlier == INDEX_NONE || Earlier == Index || Definitions[Earlier]->TypeName.Equals(Definitions[Index]->TypeName, ESearchCase::CaseSensitive))
			{
				continue;
			}
			OutProblems.Add(FString::Printf(TEXT("%s and %s use the same Logic File %s; each type needs its own"),
				*DisplayName(*Definitions[Earlier]), *DisplayName(*Definitions[Index]), *LogicFile));
		}
	}

	/** The Can Call types' names, sorted, each once; an empty entry or one with no Type Name is left out. */
	TArray<FString> CallableTypeNames(const UCrowdyServerObjectDefinition& Definition)
	{
		TArray<FString> Names;
		for (const UCrowdyServerObjectDefinition* Target : Definition.CanCall)
		{
			if (Target && !Target->TypeName.IsEmpty())
			{
				Names.Add(Target->TypeName);
			}
		}
		Algo::Sort(Names, &IsBefore);
		Names.SetNum(Algo::Unique(Names, [](const FString& A, const FString& B) { return A.Equals(B, ESearchCase::CaseSensitive); }));
		return Names;
	}

	/** Adds "calls" when the type calls others and "scopes" when it asks for platform data; a type with neither gets nothing. */
	void AppendCallsAndScopes(FStringBuilderBase& Json, const UCrowdyServerObjectDefinition& Definition)
	{
		const TArray<FString> Calls = CallableTypeNames(Definition);
		if (!Calls.IsEmpty())
		{
			Json.Appendf(TEXT(",\"calls\":[\"%s\"]"), *FString::Join(Calls, TEXT("\",\"")));
		}
		const TArray<FString> Scopes = CrowdyExecDeploy::ScopesOf(Definition);
		if (!Scopes.IsEmpty())
		{
			Json.Appendf(TEXT(",\"scopes\":[\"%s\"]"), *FString::Join(Scopes, TEXT("\",\"")));
		}
	}

	struct FPlatformScope
	{
		const TCHAR* Name;
		const TCHAR* Setting;
		bool UCrowdyServerObjectDefinition::* Asked;
	};

	/** In the platform's order, which is the order a manifest lists them in. */
	const FPlatformScope PlatformScopes[] = {
		{TEXT("players.read"), TEXT("Reads Players"), &UCrowdyServerObjectDefinition::bReadsPlayers},
		{TEXT("players.write"), TEXT("Changes Players' App State"), &UCrowdyServerObjectDefinition::bWritesPlayerState},
		{TEXT("world.read"), TEXT("Reads World Data"), &UCrowdyServerObjectDefinition::bReadsWorld},
		{TEXT("world.write"), TEXT("Changes the World"), &UCrowdyServerObjectDefinition::bWritesWorld},
		{TEXT("grids.read"), TEXT("Reads Grids"), &UCrowdyServerObjectDefinition::bReadsGrids},
		{TEXT("permissions.write"), TEXT("Grants Grid Permissions"), &UCrowdyServerObjectDefinition::bGrantsGridPermissions},
	};

	/** A ckx-sdk call into platform data, as it reads with whitespace removed, and the scope it needs. */
	struct FScopedCall
	{
		const TCHAR* Call;
		const TCHAR* Scope;
	};

	const FScopedCall ScopedCalls[] = {
		{TEXT("players().get("), TEXT("players.read")},
		{TEXT("players().features("), TEXT("players.read")},
		{TEXT("players().check_permission("), TEXT("players.read")},
		{TEXT("players().state("), TEXT("players.read")},
		{TEXT("players().avatar_state("), TEXT("players.read")},
		{TEXT("players().set_state("), TEXT("players.write")},
		{TEXT("world().chunk("), TEXT("world.read")},
		{TEXT("world().voxels("), TEXT("world.read")},
		{TEXT("world().actors("), TEXT("world.read")},
		{TEXT("world().actors_near("), TEXT("world.read")},
		{TEXT("world().set_voxels("), TEXT("world.write")},
		{TEXT("world().set_chunk_state("), TEXT("world.write")},
		{TEXT("grids().get("), TEXT("grids.read")},
		{TEXT("grids().settings("), TEXT("grids.read")},
		{TEXT("grids().at("), TEXT("grids.read")},
		{TEXT("grids().check_permission("), TEXT("grids.read")},
		{TEXT("permissions().grant("), TEXT("permissions.write")},
		{TEXT("permissions().revoke("), TEXT("permissions.write")},
	};

	/** The Rust source without its comments, strings and whitespace, so only code is matched and a call split across lines reads as one. */
	FString CodeOnly(const FString& Source)
	{
		TArray<FString> Lines;
		Source.ParseIntoArrayLines(Lines, false);
		CrowdyRustSyntax::FCarry Carry;
		FString Code;
		for (const FString& Line : Lines)
		{
			for (const CrowdyRustSyntax::FSpan& Span : CrowdyRustSyntax::ScanLine(Line, Carry))
			{
				if (Span.Kind == CrowdyRustSyntax::EKind::Comment || Span.Kind == CrowdyRustSyntax::EKind::String)
				{
					continue;
				}
				for (int32 Index = Span.Start; Index < Span.End; ++Index)
				{
					if (!FChar::IsWhitespace(Line[Index]))
					{
						Code.AppendChar(Line[Index]);
					}
				}
			}
		}
		return Code;
	}
}

TArray<FString> CrowdyExecDeploy::ScopesOf(const UCrowdyServerObjectDefinition& Definition)
{
	using namespace CrowdyExecDeployDetail;
	const bool bNeedsFeature = Definition.Functions.ContainsByPredicate([](const FCrowdyServerFunction& Function) { return !Function.RequiredFeature.IsEmpty(); });
	const bool bTeamsNeedPlayers = Definition.MembersFrom == ECrowdyServerMembersSource::CrowdyTeam || Definition.HasCrowdyTeamRecords() || bNeedsFeature;
	TArray<FString> Scopes;
	for (const FPlatformScope& Scope : PlatformScopes)
	{
		const bool bImplied = bTeamsNeedPlayers && Scope.Asked == &UCrowdyServerObjectDefinition::bReadsPlayers;
		if (Definition.*Scope.Asked || bImplied)
		{
			Scopes.Add(Scope.Name);
		}
	}
	return Scopes;
}

TArray<FString> CrowdyExecDeploy::FindScopeWarnings(const UCrowdyServerObjectDefinition& Definition, const CrowdyExecDeveloper::FBuildCrate& Crate)
{
	using namespace CrowdyExecDeployDetail;
	FString Code;
	for (const CrowdyExecDeveloper::FBuildFile& File : Crate.Files)
	{
		Code += File.Path.EndsWith(TEXT(".rs"), ESearchCase::CaseSensitive) ? CodeOnly(File.Content) : FString();
	}
	const TArray<FString> Asked = ScopesOf(Definition);
	TArray<FString> Missing;
	TArray<FString> Warnings;
	for (const FScopedCall& Use : ScopedCalls)
	{
		if (Asked.Contains(Use.Scope) || Missing.Contains(Use.Scope) || !Code.Contains(Use.Call, ESearchCase::CaseSensitive))
		{
			continue;
		}
		Missing.Add(Use.Scope);
		const FPlatformScope* Scope = Algo::FindBy(PlatformScopes, FStringView(Use.Scope), [](const FPlatformScope& Each) { return FStringView(Each.Name); });
		Warnings.Add(FString::Printf(TEXT("%s: its server code calls %s), which needs %s; turn on %s or the platform refuses that call"),
			*DisplayName(Definition), Use.Call, Use.Scope, Scope ? Scope->Setting : Use.Scope));
	}
	return Warnings;
}

TArray<UCrowdyServerObjectDefinition*> CrowdyExecDeploy::FindProjectDefinitions()
{
	using namespace CrowdyExecDeployDetail;
	FARFilter Filter;
	Filter.ClassPaths.Add(UCrowdyServerObjectDefinition::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	TArray<FAssetData> Assets;
	IAssetRegistry::GetChecked().GetAssets(Filter, Assets);
	TArray<UCrowdyServerObjectDefinition*> Definitions;
	for (const FAssetData& Asset : Assets)
	{
		if (UCrowdyServerObjectDefinition* Definition = Cast<UCrowdyServerObjectDefinition>(Asset.GetAsset()))
		{
			Definitions.Add(Definition);
		}
	}
	Algo::Sort(Definitions, [](const UCrowdyServerObjectDefinition* A, const UCrowdyServerObjectDefinition* B) { return IsBefore(A->TypeName, B->TypeName); });
	return Definitions;
}

CrowdyExecDeploy::FTypeState CrowdyExecDeploy::InspectType(const UCrowdyServerObjectDefinition& Definition, const FString& CrateDirectory)
{
	using namespace CrowdyExecDeployDetail;
	FTypeState State;
	State.TypeName = Definition.TypeName;
	State.AssetPath = Definition.GetPathName();
	if (Definition.TypeName.Equals(RootTypeName, ESearchCase::CaseSensitive))
	{
		return WithProblem(State, ECrateState::Invalid, TEXT("root is the name of the platform's root Server Object type; choose another Type Name"));
	}
	const int64 SaveIntervalMs = static_cast<int64>(Definition.SaveIntervalSeconds) * 1000;
	if (SaveIntervalMs < MinSaveIntervalMs || SaveIntervalMs > MaxSaveIntervalMs)
	{
		return WithProblem(State, ECrateState::Invalid, TEXT("Save Interval must be 5 to 60 seconds"));
	}
	const int64 IdleTimeoutMs = static_cast<int64>(Definition.IdleTimeoutSeconds) * 1000;
	if (IdleTimeoutMs < MinIdleTimeoutMs || IdleTimeoutMs > MaxIdleTimeoutMs)
	{
		return WithProblem(State, ECrateState::Invalid, TEXT("Idle Timeout must be 1 to 1800 seconds"));
	}
	CrowdyExecCodegen::FGeneratedCrate Crate;
	FString Problem;
	if (!GenerateInMemory(Definition, Crate, Problem))
	{
		return WithProblem(State, ECrateState::Invalid, Problem);
	}
	if (FPaths::FileExists(FPaths::Combine(CrateDirectory, CargoPath)) && !CrowdyExecCodegen::IsGeneratedCrateFolder(CrateDirectory))
	{
		return WithProblem(State, ECrateState::Invalid, FString::Printf(TEXT("%s holds server code the Crowdy SDK did not generate; choose another Type Name, or move that folder"), *CrateDirectory));
	}
	const FString Logic = CrowdyExecCodegen::GetLogicFile(Definition, CrateDirectory);
	if (Logic.IsEmpty())
	{
		return WithProblem(State, ECrateState::NotGenerated, NoLogicFileChosen);
	}
	for (const CrowdyExecCodegen::FGeneratedFile& File : Crate.Files)
	{
		if (File.Path.Equals(LogicPath, ESearchCase::CaseSensitive))
		{
			continue;
		}
		const FString Path = FPaths::Combine(CrateDirectory, File.Path);
		FString OnDisk;
		if (!FFileHelper::LoadFileToString(OnDisk, *Path))
		{
			return WithProblem(State, ECrateState::NotGenerated, FString::Printf(TEXT("its server code is missing %s; write it with Generate Server Code"), *Path));
		}
		if (!Comparable(OnDisk).Equals(Comparable(File.Text), ESearchCase::CaseSensitive))
		{
			return WithProblem(State, ECrateState::OutOfDate, FString::Printf(TEXT("%s no longer matches the definition; regenerate it with Generate Server Code"), *Path));
		}
	}
	if (!FPaths::FileExists(Logic))
	{
		const FString Missing = CrowdyExecCodegen::HasOwnLogicFile(Definition)
			? FString::Printf(TEXT("its Logic File %s does not exist; choose an existing file, or switch Code Source to Generated"), *Logic)
			: FString::Printf(TEXT("its server code is missing %s; write it with Generate Server Code"), *Logic);
		return WithProblem(State, ECrateState::NotGenerated, Missing);
	}
	State.State = ECrateState::UpToDate;
	return State;
}

CrowdyExecDeveloper::FBuildCrate CrowdyExecDeploy::MakeRootCrate()
{
	using namespace CrowdyExecDeployDetail;
	CrowdyExecDeveloper::FBuildCrate Crate;
	Crate.Name = RootTypeName;
	Crate.Files.Add(CrowdyExecDeveloper::FBuildFile{CargoPath, CargoText(RootTypeName)});
	Crate.Files.Add(CrowdyExecDeveloper::FBuildFile{TEXT("src/lib.rs"), RootLib});
	return Crate;
}

FString CrowdyExecDeploy::MakeManifest(TConstArrayView<const UCrowdyServerObjectDefinition*> Definitions)
{
	using namespace CrowdyExecDeployDetail;
	TArray<const UCrowdyServerObjectDefinition*> Sorted = Definitions.FilterByPredicate([](const UCrowdyServerObjectDefinition* Definition) { return Definition != nullptr; });
	Algo::Sort(Sorted, [](const UCrowdyServerObjectDefinition* A, const UCrowdyServerObjectDefinition* B) { return IsBefore(A->TypeName, B->TypeName); });
	TStringBuilder<1024> Json;
	Json.Appendf(TEXT("{\"root\":\"%s\",\"types\":{\"%s\":{\"kind\":\"hub\",\"crate\":\"%s\",\"client\":false}"), RootTypeName, RootTypeName, RootTypeName);
	for (const UCrowdyServerObjectDefinition* Definition : Sorted)
	{
		const TCHAR* const Name = *Definition->TypeName;
		Json.Appendf(TEXT(",\"%s\":{\"kind\":\"hub\",\"parent\":\"%s\",\"crate\":\"%s\",\"client\":%s,\"persist_every_ms\":%lld,\"evict_after_ms\":%lld"),
			Name, RootTypeName, Name, Definition->IsReachableByPlayers() ? TEXT("true") : TEXT("false"), static_cast<int64>(Definition->SaveIntervalSeconds) * 1000, static_cast<int64>(Definition->IdleTimeoutSeconds) * 1000);
		AppendCallsAndScopes(Json, *Definition);
		Json.AppendChar(TEXT('}'));
	}
	Json.Append(TEXT("}}"));
	return FString(Json.ToView());
}

CrowdyExecDeploy::FProjectDeploy CrowdyExecDeploy::AssembleProject(TConstArrayView<const UCrowdyServerObjectDefinition*> Definitions,
	TFunctionRef<FString(const UCrowdyServerObjectDefinition&)> CrateDirectoryFor)
{
	using namespace CrowdyExecDeployDetail;
	FProjectDeploy Project;
	if (Definitions.IsEmpty())
	{
		Project.Problems.Add(TEXT("The project has no Server Object types"));
		return Project;
	}
	const CrowdyExecDeveloper::FBuildCrate& Root = Project.Crates.Add_GetRef(MakeRootCrate());
	int64 Budget = MaxSourceBytes;
	for (const CrowdyExecDeveloper::FBuildFile& File : Root.Files)
	{
		Budget -= SourceBytesOf(File.Content);
	}
	for (const UCrowdyServerObjectDefinition* Definition : Definitions)
	{
		if (!Definition)
		{
			continue;
		}
		const FString Directory = CrateDirectoryFor(*Definition);
		const FTypeState& State = Project.Types.Add_GetRef(InspectType(*Definition, Directory));
		if (State.State != ECrateState::UpToDate)
		{
			Project.Problems.Add(FString::Printf(TEXT("%s: %s"), *DisplayName(*Definition), *State.Problem));
			continue;
		}
		FString ReadProblem;
		CrowdyExecDeveloper::FBuildCrate Crate;
		if (!ReadCrate(Definition->TypeName, Directory, Budget, Crate, ReadProblem) || !UseOwnLogicFile(*Definition, Directory, Budget, Crate, ReadProblem))
		{
			Project.Problems.Add(FString::Printf(TEXT("%s: %s"), *DisplayName(*Definition), *ReadProblem));
			continue;
		}
		Project.Warnings.Append(FindScopeWarnings(*Definition, Crate));
		Project.Crates.Add(MoveTemp(Crate));
	}
	AddDuplicateProblems(Definitions, Project.Problems);
	AddSharedLogicFileProblems(Definitions, CrateDirectoryFor, Project.Problems);
	FString LimitProblem;
	if (!CheckBuildLimits(Project.Crates, LimitProblem))
	{
		Project.Problems.Add(LimitProblem);
	}
	Project.ManifestJson = MakeManifest(Definitions);
	return Project;
}

bool CrowdyExecDeploy::CheckBuildLimits(TConstArrayView<CrowdyExecDeveloper::FBuildCrate> Crates, FString& OutProblem)
{
	using namespace CrowdyExecDeployDetail;
	if (Crates.Num() > MaxCrates)
	{
		OutProblem = FString::Printf(TEXT("One deploy builds at most %d pieces of server code (the platform's root Server Object type and %d of yours); this one has %d"),
			MaxCrates, MaxCrates - 1, Crates.Num());
		return false;
	}
	int64 SourceBytes = 0;
	for (const CrowdyExecDeveloper::FBuildCrate& Crate : Crates)
	{
		if (Crate.Files.Num() > MaxFilesPerCrate)
		{
			OutProblem = FString::Printf(TEXT("%s's server code has %d files; each type's server code can have at most %d"), *Crate.Name, Crate.Files.Num(), MaxFilesPerCrate);
			return false;
		}
		for (const CrowdyExecDeveloper::FBuildFile& File : Crate.Files)
		{
			SourceBytes += FTCHARToUTF8(*File.Content).Length();
		}
	}
	if (SourceBytes > MaxSourceBytes)
	{
		OutProblem = FString::Printf(TEXT("The server code is %.2f MB in all; one deploy builds at most 2 MB of source"), static_cast<double>(SourceBytes) / (1024.0 * 1024.0));
		return false;
	}
	return true;
}
