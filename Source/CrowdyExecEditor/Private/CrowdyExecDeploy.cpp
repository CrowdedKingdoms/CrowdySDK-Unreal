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
#include "CrowdyServerObjectLibrary.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace CrowdyExecDeployDetail
{
	const TCHAR* const CargoPath = TEXT("Cargo.toml");
	const TCHAR* const LogicPath = TEXT("src/logic.rs");
	const TCHAR* const StarterLibPath = TEXT("src/lib.rs");
	const TCHAR* const NoLogicFileChosen = TEXT("Code Source is My Own File but no file is chosen; choose one, or switch Code Source to Generated");

	constexpr int32 MaxCrates = 16;
	constexpr int32 MaxFilesPerCrate = 64;
	constexpr int64 MaxSourceBytes = 2 * 1024 * 1024;

	constexpr int64 MinSaveIntervalMs = 5000;
	constexpr int64 MaxSaveIntervalMs = 60000;
	constexpr int64 MinIdleTimeoutMs = 1000;
	constexpr int64 MaxIdleTimeoutMs = 1800000;

	/** The root's own code for its state struct StateName; the platform calls on_presence on the root alone. */
	FString RootLogic(const FString& StateName)
	{
		return FString(TEXT("// The root every Server Object type sits under on the platform, written by the Crowdy SDK: it keeps the number of\n")
			TEXT("// players in the app's world, which players read as Players Online.\n")
			TEXT("use crate::*;\n")
			TEXT("use ckx_sdk::prelude::*;\n")
			TEXT("\n"))
			+ FString::Printf(TEXT("impl Functions for %s {\n"), *StateName)
			+ TEXT("    fn on_presence(&mut self, _ctx: &Ctx, presence: &Presence) -> Result<()> {\n")
			TEXT("        self.PlayersOnline = i32::try_from(presence.count).unwrap_or(i32::MAX);\n")
			TEXT("        Ok(())\n")
			TEXT("    }\n")
			TEXT("}\n");
	}

	/** Line endings and whitespace at the end of the file do not make a crate stale. */
	FString Comparable(const FString& Text)
	{
		return Text.Replace(TEXT("\r\n"), TEXT("\n")).TrimEnd();
	}

	FString QuotedLine(FString Line)
	{
		Line.TrimStartAndEndInline();
		return FString::Printf(TEXT("\"%s\""), Line.Len() > 80 ? *(Line.Left(80) + TEXT("...")) : *Line);
	}

	/** Where two texts first differ, after the same normalisation Comparable applies. */
	FString FirstDifference(const FString& OnDisk, const FString& Generated)
	{
		TArray<FString> Have;
		TArray<FString> Want;
		Comparable(OnDisk).ParseIntoArray(Have, TEXT("\n"), false);
		Comparable(Generated).ParseIntoArray(Want, TEXT("\n"), false);
		int32 Index = 0;
		while (Index < Have.Num() && Index < Want.Num() && Have[Index].Equals(Want[Index], ESearchCase::CaseSensitive))
		{
			++Index;
		}
		const FString Has = Have.IsValidIndex(Index) ? FString::Printf(TEXT("the file has %s"), *QuotedLine(Have[Index])) : FString(TEXT("the file ends"));
		const FString Writes = Want.IsValidIndex(Index) ? FString::Printf(TEXT("Generate writes %s"), *QuotedLine(Want[Index])) : FString(TEXT("Generate writes nothing more"));
		return FString::Printf(TEXT("line %d: %s, %s"), Index + 1, *Has, *Writes);
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

	int64 SourceBytesOfCrate(const CrowdyExecDeveloper::FBuildCrate& Crate)
	{
		int64 Bytes = 0;
		for (const CrowdyExecDeveloper::FBuildFile& File : Crate.Files)
		{
			Bytes += SourceBytesOf(File.Content);
		}
		return Bytes;
	}

	FString Condensed(const TSharedRef<FJsonObject>& Object)
	{
		FString Json;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Object, Writer);
		return Json;
	}

	/** Loads a file only when its size fits what is left of the source limit, and takes its size from it. */
	bool LoadWithinBudget(const FString& Path, int64& InOutBudget, FString& OutContent, FString& OutProblem)
	{
		if (IFileManager::Get().FileSize(*Path) > InOutBudget)
		{
			OutProblem = FString::Printf(TEXT("%s takes its type's server code past 2 MB of source; one build takes at most 2 MB"), *Path);
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

	/** The Can Call and Listen To types' names, sorted, each once; an empty entry or one with no Type Name is left out. */
	TArray<FString> CallableTypeNames(const UCrowdyServerObjectDefinition& Definition)
	{
		TArray<FString> Names;
		auto AddName = [&Names](const UCrowdyServerObjectDefinition* Target)
		{
			if (Target && !Target->TypeName.IsEmpty())
			{
				Names.Add(Target->TypeName);
			}
		};
		for (const UCrowdyServerObjectDefinition* Target : Definition.CanCall)
		{
			AddName(Target);
		}
		for (const UCrowdyServerObjectDefinition* Target : Definition.ListenTo)
		{
			if (!Definition.IsFunctionLibrary())
			{
				AddName(Target);
			}
		}
		Algo::Sort(Names, &IsBefore);
		Names.SetNum(Algo::Unique(Names, [](const FString& A, const FString& B) { return A.Equals(B, ESearchCase::CaseSensitive); }));
		return Names;
	}

	/** The Can Call and Listen To types, and for a Game Kit starter each of its Calls the project has, sorted, each once. */
	TArray<FString> CalledTypeNames(const UCrowdyServerObjectDefinition& Definition, TConstArrayView<const UCrowdyServerObjectDefinition*> Definitions)
	{
		TArray<FString> Names = CallableTypeNames(Definition);
		if (!Definition.IsGameKitStarter())
		{
			return Names;
		}
		for (const FString& Called : Definition.KitCalls)
		{
			if (!Called.Equals(Definition.TypeName, ESearchCase::CaseSensitive) && FindSameName(Definitions, Called) != INDEX_NONE)
			{
				Names.Add(Called);
			}
		}
		Algo::Sort(Names, &IsBefore);
		Names.SetNum(Algo::Unique(Names, [](const FString& A, const FString& B) { return A.Equals(B, ESearchCase::CaseSensitive); }));
		return Names;
	}

	/** A starter's Config, trimmed; empty for every other type. */
	FString SeedOf(const UCrowdyServerObjectDefinition& Definition)
	{
		return Definition.IsGameKitStarter() ? Definition.KitConfig.TrimStartAndEnd() : FString();
	}

	bool IsJsonObject(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) && Object.IsValid();
	}

	/** Adds "calls" when the type calls others (["*"] for any type) and "scopes" when it asks for platform data; a type with neither gets nothing. */
	void AppendCallsAndScopes(FStringBuilderBase& Json, const UCrowdyServerObjectDefinition& Definition, TConstArrayView<const UCrowdyServerObjectDefinition*> Definitions)
	{
		const TArray<FString> Calls = Definition.bCanCallAnyType ? TArray<FString>{TEXT("*")} : CalledTypeNames(Definition, Definitions);
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

	/** Calls and scopes, a starter's seed, the server limits a type changes, and the end of its entry. */
	void AppendTypeTail(FStringBuilderBase& Json, const UCrowdyServerObjectDefinition& Definition, TConstArrayView<const UCrowdyServerObjectDefinition*> Definitions)
	{
		AppendCallsAndScopes(Json, Definition, Definitions);
		const FString Seed = CrowdyExecDeploy::SeedB64Of(Definition);
		if (!Seed.IsEmpty())
		{
			Json.Appendf(TEXT(",\"seed_b64\":\"%s\""), *Seed);
		}
		for (const CrowdyExecDeploy::FServerLimit& Limit : CrowdyExecDeploy::LimitsOf(Definition))
		{
			if (Limit.Value != Limit.Default)
			{
				Json.Appendf(TEXT(",\"%s\":%lld"), Limit.Field, Limit.Value);
			}
		}
		Json.AppendChar(TEXT('}'));
	}

	/** The platform refuses a type under a Server Function Library, and Parents that run in a circle. */
	void AddKitParentProblems(TConstArrayView<const UCrowdyServerObjectDefinition*> Definitions, TArray<FString>& OutProblems)
	{
		for (const UCrowdyServerObjectDefinition* Definition : Definitions)
		{
			if (!Definition || !Definition->IsGameKitStarter() || Definition->KitParent.IsEmpty())
			{
				continue;
			}
			const int32 Parent = FindSameName(Definitions, Definition->KitParent);
			if (Parent != INDEX_NONE && Definitions[Parent]->IsFunctionLibrary())
			{
				OutProblems.Add(FString::Printf(TEXT("%s: its Parent %s is a Server Function Library; a type runs under a Server Object"), *DisplayName(*Definition), *Definition->KitParent));
			}
			TArray<FString> Chain = {Definition->TypeName};
			for (const UCrowdyServerObjectDefinition* Next = Definition; Next && Next->IsGameKitStarter() && !Next->KitParent.IsEmpty() && Chain.Num() <= Definitions.Num();)
			{
				const int32 Found = FindSameName(Definitions, Next->KitParent);
				Next = Found == INDEX_NONE ? nullptr : Definitions[Found];
				if (Next && Next == Definition)
				{
					OutProblems.Add(FString::Printf(TEXT("%s: its Parents run in a circle (%s); give one of them another Parent"), *DisplayName(*Definition), *FString::Join(Chain, TEXT(", "))));
					break;
				}
				Chain.Add(Next ? Next->TypeName : FString());
			}
		}
	}

	/** Get Players Online reads the type the Game Kit World setting names, so it must be the root this deploy sends. */
	void AddGameKitWorldWarning(const UCrowdyServerObjectDefinition* KitRoot, TArray<FString>& OutWarnings)
	{
		const UCrowdyServerObjectDefinition* Named = UCrowdyServerObjectLibrary::GetGameKitWorldDefinition();
		if (Named == KitRoot)
		{
			return;
		}
		OutWarnings.Add(KitRoot
			? FString::Printf(TEXT("Get Players Online reads %s, but this deploy's root is %s; set Game Kit World to %s in Project Settings, Crowdy SDK"),
				Named ? *Named->GetPathName() : TEXT("the Crowdy SDK's root"), *KitRoot->TypeName, *KitRoot->GetPathName())
			: FString::Printf(TEXT("Get Players Online reads %s, which this deploy does not make the root; clear Game Kit World in Project Settings, Crowdy SDK"), *Named->GetPathName()));
	}

	/** A starter whose Config names a board posts its scores to a leaderboard of that name, which the project must have. */
	void AddKitBoardWarnings(TConstArrayView<const UCrowdyServerObjectDefinition*> Definitions, TArray<FString>& OutWarnings)
	{
		if (FindSameName(Definitions, TEXT("leaderboard")) != INDEX_NONE)
		{
			return;
		}
		for (const UCrowdyServerObjectDefinition* Definition : Definitions)
		{
			TSharedPtr<FJsonObject> Config;
			FString Board;
			const FString Seed = Definition ? SeedOf(*Definition) : FString();
			if (Seed.IsEmpty() || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Seed), Config) || !Config.IsValid() || !Config->TryGetStringField(TEXT("board"), Board) || Board.IsEmpty())
			{
				continue;
			}
			OutWarnings.Add(FString::Printf(TEXT("%s: its Config posts scores to the board %s, but the project has no leaderboard, so they go nowhere; add the leaderboard starter"),
				*DisplayName(*Definition), *Board));
		}
	}

	/** A starter's crate is its own Rust as Add starter copied it, never generated: it needs its Cargo.toml and src/lib.rs, and a Config that is a JSON object. */
	CrowdyExecDeploy::FTypeState InspectStarter(const UCrowdyServerObjectDefinition& Definition, const FString& CrateDirectory, CrowdyExecDeploy::FTypeState State)
	{
		using CrowdyExecDeploy::ECrateState;
		TArray<FString> Errors;
		if (Definition.BakedStructs.IsEmpty() && !const_cast<UCrowdyServerObjectDefinition&>(Definition).Bake(Errors))
		{
			return WithProblem(State, ECrateState::Invalid, FString::Join(Errors, TEXT("; ")));
		}
		const FString Seed = SeedOf(Definition);
		if (!Seed.IsEmpty() && !IsJsonObject(Seed))
		{
			return WithProblem(State, ECrateState::Invalid, TEXT("its Game Kit Config is not a JSON object; correct it, or empty it to keep the starter's defaults"));
		}
		for (const TCHAR* const Path : {CargoPath, StarterLibPath})
		{
			const FString File = FPaths::Combine(CrateDirectory, Path);
			if (!FPaths::FileExists(File))
			{
				return WithProblem(State, ECrateState::NotGenerated, FString::Printf(TEXT("its server code is missing %s; restore it, or remove the type in Server Compute and add the starter again"), *File));
			}
		}
		State.State = ECrateState::UpToDate;
		return State;
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
		{TEXT("grids.write"), TEXT("Manages Grids"), &UCrowdyServerObjectDefinition::bManagesGrids},
		{TEXT("permissions.write"), TEXT("Grants Grid Permissions"), &UCrowdyServerObjectDefinition::bGrantsGridPermissions},
		{TEXT("access.read"), TEXT("Reads Players' Access"), &UCrowdyServerObjectDefinition::bReadsAccess},
		{TEXT("access.write"), TEXT("Manages Players' Access"), &UCrowdyServerObjectDefinition::bManagesAccess},
	};

	struct FLimitSetting
	{
		const TCHAR* Field;
		int32 UCrowdyServerObjectDefinition::* Setting;
		int32 Default;
		int32 Min;
		int32 Max;
		/** Manifest units per unit of the setting. */
		int64 Scale;
	};

	/** The platform's defaults and ranges: it brings a value outside a range into it, so the editor does the same before comparing. */
	const FLimitSetting LimitSettings[] = {
		{TEXT("memory_mb"), &UCrowdyServerObjectDefinition::MemoryMegabytes, 64, 1, 512, 1},
		{TEXT("fuel_per_call"), &UCrowdyServerObjectDefinition::FuelPerCallMillions, 50, 1, 1000, 1000000},
		{TEXT("mailbox"), &UCrowdyServerObjectDefinition::MaxWaitingCalls, 1024, 1, 4096, 1},
		{TEXT("deadline_ms"), &UCrowdyServerObjectDefinition::CallTimeLimitMs, 2000, 10, 10000, 1},
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
		{TEXT("grids().create("), TEXT("grids.write")},
		{TEXT("grids().delete("), TEXT("grids.write")},
		{TEXT("grids().assign_group("), TEXT("grids.write")},
		{TEXT("grids().unassign_group("), TEXT("grids.write")},
		{TEXT("grids().set_open_permissions("), TEXT("grids.write")},
		{TEXT("permissions().grant("), TEXT("permissions.write")},
		{TEXT("permissions().revoke("), TEXT("permissions.write")},
		{TEXT("access().get("), TEXT("access.read")},
		{TEXT("access().grant("), TEXT("access.write")},
		{TEXT("access().revoke("), TEXT("access.write")},
		{TEXT("access().suspend("), TEXT("access.write")},
		{TEXT("access().unsuspend("), TEXT("access.write")},
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

	/** Where the item an attribute at Index marks ends: after its braces, or after the semicolon of an item with none. */
	int32 EndOfAttributedItem(const FString& Code, int32 Index)
	{
		const int32 Length = Code.Len();
		while (Index + 1 < Length && Code[Index] == TEXT('#') && Code[Index + 1] == TEXT('['))
		{
			int32 Depth = 0;
			do
			{
				Depth += (Code[Index] == TEXT('[')) - (Code[Index] == TEXT(']'));
				++Index;
			} while (Index < Length && Depth > 0);
		}
		int32 Nest = 0;
		for (; Index < Length; ++Index)
		{
			const TCHAR Char = Code[Index];
			Nest += (Char == TEXT('(') || Char == TEXT('[')) - (Char == TEXT(')') || Char == TEXT(']'));
			// A field, variant or match arm ends at its comma, or where its enclosing block closes.
			if (Nest <= 0 && (Char == TEXT('{') || Char == TEXT(';') || Char == TEXT(',') || Char == TEXT('}')))
			{
				break;
			}
		}
		if (Index < Length && Code[Index] == TEXT('}'))
		{
			return Index;
		}
		if (Index >= Length || Code[Index] == TEXT(';') || Code[Index] == TEXT(','))
		{
			return FMath::Min(Index + 1, Length);
		}
		for (int32 Depth = 0; Index < Length; ++Index)
		{
			Depth += (Code[Index] == TEXT('{')) - (Code[Index] == TEXT('}'));
			if (Depth == 0)
			{
				return Index + 1;
			}
		}
		return Length;
	}

	/** Code with each #[cfg(test)] item removed, since test code never runs on the platform. Takes CodeOnly's output. */
	FString StripCfgTest(const FString& Code)
	{
		const FString Marker = TEXT("#[cfg(test)]");
		FString Kept;
		int32 From = 0;
		for (int32 At = Code.Find(Marker, ESearchCase::CaseSensitive); At != INDEX_NONE; At = Code.Find(Marker, ESearchCase::CaseSensitive, ESearchDir::FromStart, From))
		{
			Kept.Append(Code.Mid(From, At - From));
			From = EndOfAttributedItem(Code, At);
		}
		return Kept + Code.RightChop(From);
	}
}

TArray<FString> CrowdyExecDeploy::ScopesOf(const UCrowdyServerObjectDefinition& Definition)
{
	using namespace CrowdyExecDeployDetail;
	const bool bPlayersRequired = !PlayersReadRequiredBecause(Definition).IsEmpty();
	TArray<FString> Scopes;
	for (const FPlatformScope& Scope : PlatformScopes)
	{
		const bool bImplied = bPlayersRequired && Scope.Asked == &UCrowdyServerObjectDefinition::bReadsPlayers;
		if (Definition.*Scope.Asked || bImplied)
		{
			Scopes.Add(Scope.Name);
		}
	}
	return Scopes;
}

FString CrowdyExecDeploy::PlayersReadRequiredBecause(const UCrowdyServerObjectDefinition& Definition)
{
	TArray<FString> Causes;
	if (!Definition.IsFunctionLibrary() && Definition.MembersFrom == ECrowdyServerMembersSource::CrowdyTeam)
	{
		Causes.Add(TEXT("Members From is Crowdy Team"));
	}
	if (!Definition.IsFunctionLibrary() && Definition.HasCrowdyTeamRecords())
	{
		Causes.Add(TEXT("its records belong to Crowdy Teams"));
	}
	TArray<FString> Gated;
	for (const FCrowdyServerFunction& Function : Definition.Functions)
	{
		// A feature is checked only for a player, and no player may call these.
		const bool bPlayersCall = Function.WhoCanCall != ECrowdyServerFunctionCaller::DevelopersOnly && Function.WhoCanCall != ECrowdyServerFunctionCaller::ServerOnly;
		if (bPlayersCall && !Function.RequiredFeature.IsEmpty())
		{
			Gated.Add(Function.Name.ToString());
		}
	}
	if (!Gated.IsEmpty())
	{
		Causes.Add(FString::Printf(TEXT("Required Feature is set on %s"), *FString::Join(Gated, TEXT(", "))));
	}
	return FString::Join(Causes, TEXT("; "));
}

int64 CrowdyExecDeploy::ClampLimit(const FString& Field, int64 ManifestValue)
{
	using namespace CrowdyExecDeployDetail;
	for (const FLimitSetting& Limit : LimitSettings)
	{
		if (Field.Equals(Limit.Field, ESearchCase::CaseSensitive))
		{
			return FMath::Clamp<int64>(ManifestValue, Limit.Min * Limit.Scale, Limit.Max * Limit.Scale);
		}
	}
	return ManifestValue;
}

TArray<FString> CrowdyExecDeploy::FindCallerWarnings(const UCrowdyServerObjectDefinition& Definition)
{
	using namespace CrowdyExecDeployDetail;
	TArray<FString> Warnings;
	for (const FCrowdyServerFunction& Function : Definition.Functions)
	{
		if (Function.WhoCanCall != ECrowdyServerFunctionCaller::DevelopersOnly && Function.WhoCanCall != ECrowdyServerFunctionCaller::ServerOnly)
		{
			continue;
		}
		if (Function.CooldownSeconds > 0.f)
		{
			Warnings.Add(FString::Printf(TEXT("%s %s: Cooldown has no effect, since only players' calls are limited."), *DisplayName(Definition), *Function.Name.ToString()));
		}
		if (!Function.RequiredFeature.IsEmpty())
		{
			Warnings.Add(FString::Printf(TEXT("%s %s: Required Feature has no effect, since only players are checked."), *DisplayName(Definition), *Function.Name.ToString()));
		}
	}
	return Warnings;
}

TArray<CrowdyExecDeploy::FServerLimit> CrowdyExecDeploy::LimitsOf(const UCrowdyServerObjectDefinition& Definition)
{
	using namespace CrowdyExecDeployDetail;
	TArray<FServerLimit> Limits;
	for (const FLimitSetting& Limit : LimitSettings)
	{
		Limits.Add({Limit.Field, FMath::Clamp(Definition.*Limit.Setting, Limit.Min, Limit.Max) * Limit.Scale, Limit.Default * Limit.Scale});
	}
	return Limits;
}

CrowdyExecDeploy::FScaling CrowdyExecDeploy::ScalingOf(const UCrowdyServerObjectDefinition& Definition)
{
	FScaling Scaling;
	Scaling.Replicas = FMath::Clamp(Definition.MinInstances, 1, 64);
	Scaling.MaxReplicas = FMath::Clamp(Definition.MaxInstances, Scaling.Replicas, 64);
	Scaling.Concurrency = FMath::Clamp(Definition.ParallelCallsPerInstance, 1, 256);
	return Scaling;
}

TArray<FString> CrowdyExecDeploy::FindScopeWarnings(const UCrowdyServerObjectDefinition& Definition, const CrowdyExecDeveloper::FBuildCrate& Crate)
{
	using namespace CrowdyExecDeployDetail;
	FString Code;
	for (const CrowdyExecDeveloper::FBuildFile& File : Crate.Files)
	{
		Code += File.Path.EndsWith(TEXT(".rs"), ESearchCase::CaseSensitive) ? StripCfgTest(CodeOnly(File.Content)) : FString();
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
	const FSoftObjectPath BuiltInRoot(UCrowdyServerObjectLibrary::PlayersOnlineDefinitionPath);
	for (const FAssetData& Asset : Assets)
	{
		// The SDK's own root definition deploys as the root, never as one of the project's types.
		if (Asset.GetSoftObjectPath() == BuiltInRoot)
		{
			continue;
		}
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
	if (Definition.TypeName.StartsWith(StagingTypePrefix, ESearchCase::IgnoreCase))
	{
		return WithProblem(State, ECrateState::Invalid, FString::Printf(TEXT("Type Names starting %s are kept for the stand-ins a deploy in parts stores its builds with; choose another Type Name"), StagingTypePrefix));
	}
	const int64 SaveIntervalMs = static_cast<int64>(Definition.SaveIntervalSeconds) * 1000;
	if (!Definition.IsFunctionLibrary() && (SaveIntervalMs < MinSaveIntervalMs || SaveIntervalMs > MaxSaveIntervalMs))
	{
		return WithProblem(State, ECrateState::Invalid, TEXT("Save Interval must be 5 to 60 seconds"));
	}
	const int64 IdleTimeoutMs = static_cast<int64>(Definition.IdleTimeoutSeconds) * 1000;
	if (!Definition.IsFunctionLibrary() && (IdleTimeoutMs < MinIdleTimeoutMs || IdleTimeoutMs > MaxIdleTimeoutMs))
	{
		return WithProblem(State, ECrateState::Invalid, TEXT("Idle Timeout must be 1 to 1800 seconds"));
	}
	if (Definition.IsGameKitStarter())
	{
		return InspectStarter(Definition, CrateDirectory, MoveTemp(State));
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
			return WithProblem(State, ECrateState::OutOfDate, FString::Printf(TEXT("%s no longer matches the definition (%s); regenerate it with Generate Server Code"), *Path, *FirstDifference(OnDisk, File.Text)));
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
	const UCrowdyServerObjectDefinition* Definition = UCrowdyServerObjectLibrary::GetPlayersOnlineDefinition();
	CrowdyExecCodegen::FGeneratedCrate Generated;
	FString Problem;
	if (!Definition || !GenerateInMemory(*Definition, Generated, Problem))
	{
		return Crate;
	}
	for (const CrowdyExecCodegen::FGeneratedFile& File : Generated.Files)
	{
		const bool bLogic = File.Path.Equals(LogicPath, ESearchCase::CaseSensitive);
		const bool bCargo = File.Path.Equals(CargoPath, ESearchCase::CaseSensitive);
		Crate.Files.Add(CrowdyExecDeveloper::FBuildFile{File.Path,
			bLogic ? RootLogic(Generated.StateName) : bCargo ? CrowdyExecCodegen::WithoutCrateDefinition(File.Text) : File.Text});
	}
	return Crate;
}

FString CrowdyExecDeploy::MakeManifest(TConstArrayView<const UCrowdyServerObjectDefinition*> Definitions)
{
	using namespace CrowdyExecDeployDetail;
	TArray<const UCrowdyServerObjectDefinition*> Sorted = Definitions.FilterByPredicate([](const UCrowdyServerObjectDefinition* Definition) { return Definition != nullptr; });
	Algo::Sort(Sorted, [](const UCrowdyServerObjectDefinition* A, const UCrowdyServerObjectDefinition* B) { return IsBefore(A->TypeName, B->TypeName); });
	const UCrowdyServerObjectDefinition* const KitRoot = FindKitRoot(Sorted);
	const FString Root = KitRoot ? KitRoot->TypeName : FString(RootTypeName);
	TStringBuilder<1024> Json;
	if (KitRoot)
	{
		// The root is never idle, so it has no idle timeout.
		Json.Appendf(TEXT("{\"root\":\"%s\",\"types\":{\"%s\":{\"kind\":\"hub\",\"crate\":\"%s\",\"client\":true,\"persist_every_ms\":%lld"),
			*Root, *Root, *Root, static_cast<int64>(KitRoot->SaveIntervalSeconds) * 1000);
		AppendTypeTail(Json, *KitRoot, Sorted);
	}
	else
	{
		// Open to players, who read and follow its Players Online.
		Json.Appendf(TEXT("{\"root\":\"%s\",\"types\":{\"%s\":{\"kind\":\"hub\",\"crate\":\"%s\",\"client\":true}"), RootTypeName, RootTypeName, RootTypeName);
	}
	for (const UCrowdyServerObjectDefinition* Definition : Sorted)
	{
		if (Definition == KitRoot)
		{
			continue;
		}
		const TCHAR* const Name = *Definition->TypeName;
		const TCHAR* const Client = Definition->IsReachableByPlayers() ? TEXT("true") : TEXT("false");
		const FString Parent = ParentOf(*Definition, Sorted, Root);
		if (Definition->IsFunctionLibrary())
		{
			const FScaling Scaling = ScalingOf(*Definition);
			Json.Appendf(TEXT(",\"%s\":{\"kind\":\"spoke\",\"parent\":\"%s\",\"crate\":\"%s\",\"client\":%s,\"replicas\":%d,\"concurrency\":%d"),
				Name, *Parent, Name, Client, Scaling.Replicas, Scaling.Concurrency);
			if (Scaling.MaxReplicas > Scaling.Replicas)
			{
				Json.Appendf(TEXT(",\"max_replicas\":%d"), Scaling.MaxReplicas);
			}
		}
		else
		{
			Json.Appendf(TEXT(",\"%s\":{\"kind\":\"hub\",\"parent\":\"%s\",\"crate\":\"%s\",\"client\":%s,\"persist_every_ms\":%lld,\"evict_after_ms\":%lld"),
				Name, *Parent, Name, Client, static_cast<int64>(Definition->SaveIntervalSeconds) * 1000, static_cast<int64>(Definition->IdleTimeoutSeconds) * 1000);
		}
		AppendTypeTail(Json, *Definition, Sorted);
	}
	Json.Append(TEXT("}}"));
	return FString(Json.ToView());
}

FString CrowdyExecDeploy::ParentOf(const UCrowdyServerObjectDefinition& Definition, TConstArrayView<const UCrowdyServerObjectDefinition*> Definitions, const FString& Root)
{
	const FString& Parent = Definition.KitParent;
	const bool bHasParent = Definition.IsGameKitStarter() && !Parent.IsEmpty() && !Parent.Equals(Definition.TypeName, ESearchCase::CaseSensitive)
		&& CrowdyExecDeployDetail::FindSameName(Definitions, Parent) != INDEX_NONE;
	return bHasParent ? Parent : Root;
}

FString CrowdyExecDeploy::SeedB64Of(const UCrowdyServerObjectDefinition& Definition)
{
	const FString Seed = CrowdyExecDeployDetail::SeedOf(Definition);
	if (Seed.IsEmpty())
	{
		return FString();
	}
	const FTCHARToUTF8 Utf8(*Seed);
	return FBase64::Encode(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

const UCrowdyServerObjectDefinition* CrowdyExecDeploy::FindKitRoot(TConstArrayView<const UCrowdyServerObjectDefinition*> Definitions)
{
	const UCrowdyServerObjectDefinition* const* Found = Definitions.FindByPredicate([](const UCrowdyServerObjectDefinition* Definition)
	{
		return Definition && Definition->IsGameKitStarter() && Definition->KitParent.IsEmpty();
	});
	return Found ? *Found : nullptr;
}

CrowdyExecDeploy::FProjectDeploy CrowdyExecDeploy::AssembleProject(TConstArrayView<const UCrowdyServerObjectDefinition*> Definitions,
	TFunctionRef<FString(const UCrowdyServerObjectDefinition&)> CrateDirectoryFor)
{
	using namespace CrowdyExecDeployDetail;
	FProjectDeploy Project;
	TArray<const UCrowdyServerObjectDefinition*> KitRoots = Definitions.FilterByPredicate([](const UCrowdyServerObjectDefinition* Definition)
	{
		return Definition && Definition->IsGameKitStarter() && Definition->KitParent.IsEmpty();
	});
	Algo::Sort(KitRoots, [](const UCrowdyServerObjectDefinition* A, const UCrowdyServerObjectDefinition* B) { return IsBefore(A->TypeName, B->TypeName); });
	if (KitRoots.Num() > 1)
	{
		Project.Problems.Add(FString::Printf(TEXT("Game Kit starters %s and %s both have no Parent, and an app has one root; give one of them a Parent"),
			*DisplayName(*KitRoots[0]), *DisplayName(*KitRoots[1])));
	}
	if (!KitRoots.IsEmpty())
	{
		Project.Root = KitRoots[0]->TypeName;
	}
	else
	{
		const CrowdyExecDeveloper::FBuildCrate& Root = Project.Crates.Add_GetRef(MakeRootCrate());
		if (Root.Files.IsEmpty())
		{
			Project.Problems.Add(FString::Printf(TEXT("The Crowdy SDK's root Server Object, %s, did not load or does not generate; reinstall the Crowdy SDK"),
				UCrowdyServerObjectLibrary::PlayersOnlineDefinitionPath));
		}
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
		int64 Budget = MaxSourceBytes;
		if (!ReadCrate(Definition->TypeName, Directory, Budget, Crate, ReadProblem) || !UseOwnLogicFile(*Definition, Directory, Budget, Crate, ReadProblem))
		{
			Project.Problems.Add(FString::Printf(TEXT("%s: %s"), *DisplayName(*Definition), *ReadProblem));
			continue;
		}
		Project.Warnings.Append(FindScopeWarnings(*Definition, Crate));
		Project.Warnings.Append(FindCallerWarnings(*Definition));
		Project.Crates.Add(MoveTemp(Crate));
	}
	AddDuplicateProblems(Definitions, Project.Problems);
	AddSharedLogicFileProblems(Definitions, CrateDirectoryFor, Project.Problems);
	AddKitParentProblems(Definitions, Project.Problems);
	AddGameKitWorldWarning(KitRoots.IsEmpty() ? nullptr : KitRoots[0], Project.Warnings);
	AddKitBoardWarnings(Definitions, Project.Warnings);
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
	for (const CrowdyExecDeveloper::FBuildCrate& Crate : Crates)
	{
		if (Crate.Files.Num() > MaxFilesPerCrate)
		{
			OutProblem = FString::Printf(TEXT("%s's server code has %d files; each type's server code can have at most %d"), *Crate.Name, Crate.Files.Num(), MaxFilesPerCrate);
			return false;
		}
		const int64 SourceBytes = SourceBytesOfCrate(Crate);
		if (SourceBytes > MaxSourceBytes)
		{
			OutProblem = FString::Printf(TEXT("%s's server code is %.2f MB; one build takes at most 2 MB of source"), *Crate.Name, static_cast<double>(SourceBytes) / (1024.0 * 1024.0));
			return false;
		}
	}
	return true;
}

TArray<TArray<CrowdyExecDeveloper::FBuildCrate>> CrowdyExecDeploy::SplitIntoBuilds(TConstArrayView<CrowdyExecDeveloper::FBuildCrate> Crates, int32 MaxCratesPerBuild)
{
	using namespace CrowdyExecDeployDetail;
	const int32 PerBuild = FMath::Clamp(MaxCratesPerBuild, 1, MaxCrates);
	TArray<TArray<CrowdyExecDeveloper::FBuildCrate>> Builds;
	int64 BuildBytes = 0;
	for (const CrowdyExecDeveloper::FBuildCrate& Crate : Crates)
	{
		const int64 CrateBytes = SourceBytesOfCrate(Crate);
		if (Builds.IsEmpty() || Builds.Last().Num() >= PerBuild || BuildBytes + CrateBytes > MaxSourceBytes)
		{
			Builds.AddDefaulted();
			BuildBytes = 0;
		}
		Builds.Last().Add(Crate);
		BuildBytes += CrateBytes;
	}
	return Builds;
}

FString CrowdyExecDeploy::NameByDigest(const FString& ManifestJson, const TMap<FString, FString>& Digests)
{
	using namespace CrowdyExecDeployDetail;
	TSharedPtr<FJsonObject> Manifest;
	const TSharedPtr<FJsonObject>* Types = nullptr;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ManifestJson), Manifest) || !Manifest.IsValid() || !Manifest->TryGetObjectField(TEXT("types"), Types))
	{
		return FString();
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Type : (*Types)->Values)
	{
		const TSharedPtr<FJsonObject>* Fields = nullptr;
		FString Crate;
		if (!Type.Value->TryGetObject(Fields) || !(*Fields)->TryGetStringField(TEXT("crate"), Crate))
		{
			continue;
		}
		if (const FString* Digest = Digests.Find(Crate))
		{
			(*Fields)->RemoveField(TEXT("crate"));
			(*Fields)->SetStringField(TEXT("digest"), *Digest);
		}
	}
	return Condensed(Manifest.ToSharedRef());
}

FString CrowdyExecDeploy::MakeStagingManifest(const FString& LiveManifestJson, const FString& DeployManifestJson, TConstArrayView<CrowdyExecDeveloper::FBuildCrate> Crates,
	FString& OutProblem)
{
	using namespace CrowdyExecDeployDetail;
	TSharedPtr<FJsonObject> Manifest;
	if (LiveManifestJson.IsEmpty())
	{
		Manifest = MakeShared<FJsonObject>();
		Manifest->SetObjectField(TEXT("types"), MakeShared<FJsonObject>());
	}
	else if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(LiveManifestJson), Manifest) || !Manifest.IsValid())
	{
		OutProblem = TEXT("The active version's manifest could not be read, so the builds could not be stored before the deploy");
		return FString();
	}
	const TSharedPtr<FJsonObject>* Types = nullptr;
	if (!Manifest->TryGetObjectField(TEXT("types"), Types))
	{
		OutProblem = TEXT("The active version's manifest names no types, so the builds could not be stored before the deploy");
		return FString();
	}
	TSharedPtr<FJsonObject> Deploying;
	const TSharedPtr<FJsonObject>* DeployingTypes = nullptr;
	if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(DeployManifestJson), Deploying) && Deploying.IsValid())
	{
		Deploying->TryGetObjectField(TEXT("types"), DeployingTypes);
	}
	// The platform gives a live seed back only as its size, so a seed this deploy sends at that size stands in for it.
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Type : (*Types)->Values)
	{
		const TSharedPtr<FJsonObject>* Fields = nullptr;
		int64 SeedBytes = 0;
		if (!Type.Value->TryGetObject(Fields))
		{
			continue;
		}
		(*Fields)->TryGetNumberField(TEXT("seed_bytes"), SeedBytes);
		(*Fields)->RemoveField(TEXT("seed_bytes"));
		if (SeedBytes <= 0 || (*Fields)->HasField(TEXT("seed_b64")))
		{
			continue;
		}
		const TSharedPtr<FJsonObject>* Sent = nullptr;
		FString Seed;
		if (!DeployingTypes || !(*DeployingTypes)->TryGetObjectField(Type.Key, Sent) || !(*Sent)->TryGetStringField(TEXT("seed_b64"), Seed)
			|| static_cast<int64>(FBase64::GetDecodedDataSize(Seed)) != SeedBytes)
		{
			OutProblem = FString::Printf(TEXT("This deploy is built in parts, each first stored by a version that runs what runs now, but the platform does not give back %s's live spawn seed (Config) and this deploy's differs, so that version could not keep it; deploy %s's Config change on its own first"),
				*Type.Key, *Type.Key);
			return FString();
		}
		(*Fields)->SetStringField(TEXT("seed_b64"), Seed);
	}
	FString Root;
	Manifest->TryGetStringField(TEXT("root"), Root);
	for (const CrowdyExecDeveloper::FBuildCrate& Crate : Crates)
	{
		const FString StandIn = StagingTypePrefix + Crate.Name;
		const TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
		Fields->SetStringField(TEXT("kind"), TEXT("hub"));
		if (Root.IsEmpty())
		{
			Root = StandIn;
			Manifest->SetStringField(TEXT("root"), Root);
		}
		else
		{
			Fields->SetStringField(TEXT("parent"), Root);
		}
		Fields->SetStringField(TEXT("crate"), Crate.Name);
		Fields->SetBoolField(TEXT("client"), false);
		(*Types)->SetObjectField(StandIn, Fields);
	}
	return Condensed(Manifest.ToSharedRef());
}
