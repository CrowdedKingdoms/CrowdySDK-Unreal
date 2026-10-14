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
			AddName(Target);
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
	if (Definition.MembersFrom == ECrowdyServerMembersSource::CrowdyTeam)
	{
		Causes.Add(TEXT("Members From is Crowdy Team"));
	}
	if (Definition.HasCrowdyTeamRecords())
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
	TStringBuilder<1024> Json;
	// Open to players, who read and follow its Players Online.
	Json.Appendf(TEXT("{\"root\":\"%s\",\"types\":{\"%s\":{\"kind\":\"hub\",\"crate\":\"%s\",\"client\":true}"), RootTypeName, RootTypeName, RootTypeName);
	for (const UCrowdyServerObjectDefinition* Definition : Sorted)
	{
		const TCHAR* const Name = *Definition->TypeName;
		Json.Appendf(TEXT(",\"%s\":{\"kind\":\"hub\",\"parent\":\"%s\",\"crate\":\"%s\",\"client\":%s,\"persist_every_ms\":%lld,\"evict_after_ms\":%lld"),
			Name, RootTypeName, Name, Definition->IsReachableByPlayers() ? TEXT("true") : TEXT("false"), static_cast<int64>(Definition->SaveIntervalSeconds) * 1000, static_cast<int64>(Definition->IdleTimeoutSeconds) * 1000);
		AppendCallsAndScopes(Json, *Definition);
		for (const FServerLimit& Limit : LimitsOf(*Definition))
		{
			if (Limit.Value != Limit.Default)
			{
				Json.Appendf(TEXT(",\"%s\":%lld"), Limit.Field, Limit.Value);
			}
		}
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
	const CrowdyExecDeveloper::FBuildCrate& Root = Project.Crates.Add_GetRef(MakeRootCrate());
	if (Root.Files.IsEmpty())
	{
		Project.Problems.Add(FString::Printf(TEXT("The Crowdy SDK's root Server Object, %s, did not load or does not generate; reinstall the Crowdy SDK"),
			UCrowdyServerObjectLibrary::PlayersOnlineDefinitionPath));
	}
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
		Project.Warnings.Append(FindCallerWarnings(*Definition));
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
