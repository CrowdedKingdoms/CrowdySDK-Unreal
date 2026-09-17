#include "GameModel/CrowdyPreSeedPlan.h"

#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	FString PreSeedRowKey(const FString& TypeName, const FString& BindingKey)
	{
		return TypeName + TEXT("|") + BindingKey;
	}

	// FString sets ignore case; type names and keys are exact everywhere else.
	struct FPreSeedRowKeyFuncs : BaseKeyFuncs<FString, FString, false>
	{
		static const FString& GetSetKey(const FString& Element) { return Element; }
		static bool Matches(const FString& A, const FString& B) { return A.Equals(B, ESearchCase::CaseSensitive); }
		static uint32 GetKeyHash(const FString& Key) { return FCrc::StrCrc32(*Key); }
	};

	template <typename ValueType>
	struct TPreSeedMapKeyFuncs : TDefaultMapHashableKeyFuncs<FString, ValueType, false>
	{
		static bool Matches(const FString& A, const FString& B) { return A.Equals(B, ESearchCase::CaseSensitive); }
		static uint32 GetKeyHash(const FString& Key) { return FCrc::StrCrc32(*Key); }
	};
	using FPreSeedServerRowMap = TMap<FString, const FCrowdyPreSeedServerRow*, FDefaultSetAllocator,
		TPreSeedMapKeyFuncs<const FCrowdyPreSeedServerRow*>>;
}

TArray<FString> FCrowdyPreSeedPlan::DistinctTypes(const TArray<FCrowdyContainerManifestRow>& Manifest)
{
	TArray<FString> Types;
	for (const FCrowdyContainerManifestRow& Row : Manifest)
	{
		const bool bKnown = Types.ContainsByPredicate([&Row](const FString& Known)
		{
			return Known.Equals(Row.TypeName, ESearchCase::CaseSensitive);
		});
		if (!bKnown && !Row.TypeName.IsEmpty())
		{
			Types.Add(Row.TypeName);
		}
	}
	return Types;
}

void FCrowdyPreSeedPlan::Diff(const TArray<FCrowdyContainerManifestRow>& Manifest,
	const TArray<FCrowdyPreSeedServerRow>& Server, const FString& ScopeSessionId, FCrowdyPreSeedReport& Out,
	const TArray<FStudioContainerType>& Types)
{
	Out.Rows.Reset();
	Out.Warnings.Reset();
	Out.ToCreate = Out.Existing = Out.Orphans = Out.Created = Out.Failed = 0;
	Out.ManifestRowCount = Manifest.Num();
	Out.ScopeSessionId = ScopeSessionId;
	Out.bApplied = false;

	TSet<FString, FPreSeedRowKeyFuncs> InScopeTypes;
	for (const FString& Type : DistinctTypes(Manifest))
	{
		InScopeTypes.Add(Type);
	}
	TSet<FString, FPreSeedRowKeyFuncs> AppScopedTypes;
	TSet<FString, FPreSeedRowKeyFuncs> SeedEligibleTypes;
	for (const FStudioContainerType& Type : Types)
	{
		if (IsAppScoped(Type)) { AppScopedTypes.Add(Type.TypeName); }
		if (IsSeedEligible(Type)) { SeedEligibleTypes.Add(Type.TypeName); }
	}

	// Server rows of a manifest type in this scope, by (type, key). Another scope's row is not this plan's concern;
	// an app-scoped type's rows have no session, whatever scope was picked.
	FPreSeedServerRowMap ServerByKey;
	for (const FCrowdyPreSeedServerRow& Row : Server)
	{
		const FString RowScope = AppScopedTypes.Contains(Row.TypeName) ? FString() : ScopeSessionId;
		if (!Row.SessionId.Equals(RowScope, ESearchCase::CaseSensitive) || !InScopeTypes.Contains(Row.TypeName))
		{
			continue;
		}
		ServerByKey.Add(PreSeedRowKey(Row.TypeName, Row.BindingKey), &Row);
	}

	TSet<FString, FPreSeedRowKeyFuncs> Claimed;
	for (const FCrowdyContainerManifestRow& Row : Manifest)
	{
		const FString Key = PreSeedRowKey(Row.TypeName, Row.BindingKey);
		bool bAlreadyClaimed = false;
		Claimed.Add(Key, &bAlreadyClaimed);
		if (bAlreadyClaimed)
		{
			Out.Warnings.Add(FString::Printf(TEXT("%s repeats the key of an earlier %s row; two placements would share one server row."),
				*Row.DisplayName, *Row.TypeName));
			continue;
		}

		FCrowdyPreSeedRow& Planned = Out.Rows.AddDefaulted_GetRef();
		Planned.TypeName = Row.TypeName;
		Planned.BindingKey = Row.BindingKey;
		Planned.DisplayName = Row.DisplayName;
		Planned.bAppScoped = AppScopedTypes.Contains(Row.TypeName);
		Planned.bSeedEligible = SeedEligibleTypes.Contains(Row.TypeName);
		if (const FCrowdyPreSeedServerRow* const* Found = ServerByKey.Find(Key))
		{
			Planned.Kind = ECrowdyPreSeedRowKind::Existing;
			Planned.ContainerId = (*Found)->ContainerId;
			++Out.Existing;
		}
		else
		{
			Planned.Kind = ECrowdyPreSeedRowKind::Create;
			++Out.ToCreate;
		}
	}

	for (const TPair<FString, const FCrowdyPreSeedServerRow*>& Pair : ServerByKey)
	{
		if (Claimed.Contains(Pair.Key))
		{
			continue;
		}
		FCrowdyPreSeedRow& Orphan = Out.Rows.AddDefaulted_GetRef();
		Orphan.Kind = ECrowdyPreSeedRowKind::Orphan;
		Orphan.TypeName = Pair.Value->TypeName;
		Orphan.BindingKey = Pair.Value->BindingKey;
		Orphan.ContainerId = Pair.Value->ContainerId;
		Orphan.bAppScoped = AppScopedTypes.Contains(Orphan.TypeName);
		++Out.Orphans;
	}
	Out.bValid = true;
}

bool FCrowdyPreSeedPlan::IsAppScoped(const FStudioContainerType& Type)
{
	return Type.Scope.Equals(TEXT("app"), ESearchCase::IgnoreCase);
}

bool FCrowdyPreSeedPlan::IsSeedEligible(const FStudioContainerType& Type)
{
	return Type.InstantiableBy.Equals(TEXT("admin"), ESearchCase::IgnoreCase) || !Type.BindPolicyJson.IsEmpty();
}

void FCrowdyPreSeedPlan::SplitCreateRows(const TArray<FCrowdyPreSeedRow>& Rows, int32 MaxPerBatch,
	TArray<FCrowdyPreSeedBatch>& OutBatches, TArray<int32>& OutEnsureRows)
{
	OutBatches.Reset();
	OutEnsureRows.Reset();
	// One open batch per scope; a full one is closed and a fresh one opened, so row order survives within a scope.
	int32 OpenBatch[2] = { INDEX_NONE, INDEX_NONE };
	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		const FCrowdyPreSeedRow& Row = Rows[Index];
		if (Row.Kind != ECrowdyPreSeedRowKind::Create)
		{
			continue;
		}
		if (!Row.bSeedEligible)
		{
			OutEnsureRows.Add(Index);
			continue;
		}
		int32& Open = OpenBatch[Row.bAppScoped ? 1 : 0];
		if (Open == INDEX_NONE || OutBatches[Open].RowIndices.Num() >= MaxPerBatch)
		{
			Open = OutBatches.AddDefaulted();
			OutBatches[Open].bAppScoped = Row.bAppScoped;
		}
		OutBatches[Open].RowIndices.Add(Index);
	}
}

TSharedPtr<FJsonObject> FCrowdyPreSeedPlan::BuildSeedVariables(int64 AppId, const FString& SessionId,
	const TArray<FCrowdyPreSeedRow>& Rows, const TArray<int32>& RowIndices)
{
	const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	// appId is a BigInt scalar: a JSON string, never a number.
	Input->SetStringField(TEXT("appId"), LexToString(AppId));
	if (!SessionId.IsEmpty())
	{
		Input->SetStringField(TEXT("sessionId"), SessionId);
	}
	TArray<TSharedPtr<FJsonValue>> Containers;
	Containers.Reserve(RowIndices.Num());
	for (const int32 Index : RowIndices)
	{
		const FCrowdyPreSeedRow& Row = Rows[Index];
		const TSharedPtr<FJsonObject> Container = MakeShared<FJsonObject>();
		Container->SetStringField(TEXT("tempId"), LexToString(Index));
		Container->SetStringField(TEXT("typeName"), Row.TypeName);
		Container->SetStringField(TEXT("displayName"), Row.DisplayName.IsEmpty() ? Row.TypeName : Row.DisplayName);
		Container->SetStringField(TEXT("bindingKey"), Row.BindingKey);
		Containers.Add(MakeShared<FJsonValueObject>(Container));
	}
	Input->SetArrayField(TEXT("containers"), Containers);

	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("input"), Input);
	return Variables;
}

int32 FCrowdyPreSeedPlan::ApplySeedIdMap(const FString& IdMapJson, const TArray<int32>& RowIndices, TArray<FCrowdyPreSeedRow>& Rows)
{
	TSharedPtr<FJsonObject> IdMap;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(IdMapJson);
	if (!FJsonSerializer::Deserialize(Reader, IdMap) || !IdMap.IsValid())
	{
		return 0;
	}
	int32 Mapped = 0;
	for (const int32 Index : RowIndices)
	{
		FString ContainerId;
		if (!Rows.IsValidIndex(Index) || !IdMap->TryGetStringField(LexToString(Index), ContainerId) || ContainerId.IsEmpty())
		{
			continue;
		}
		Rows[Index].ContainerId = ContainerId;
		Rows[Index].Kind = ECrowdyPreSeedRowKind::Existing;
		++Mapped;
	}
	return Mapped;
}

FString FCrowdyPreSeedPlan::BuildCountLine(const FCrowdyPreSeedReport& Report)
{
	if (!Report.bValid)
	{
		return TEXT("Not checked yet.");
	}
	if (Report.bApplied)
	{
		return FString::Printf(TEXT("Applied: %d created, %d already existed, %d failed."), Report.Created, Report.Existing, Report.Failed);
	}
	return FString::Printf(TEXT("Plan: %d to create, %d existing, %d orphan(s)."), Report.ToCreate, Report.Existing, Report.Orphans);
}

FString FCrowdyPreSeedPlan::BuildReportText(const FCrowdyPreSeedReport& Report)
{
	FString Text;
	if (!Report.StatusNote.IsEmpty())
	{
		Text += Report.StatusNote + TEXT("\n\n");
	}
	if (!Report.bValid)
	{
		return Text.IsEmpty() ? TEXT("Scan the open map, then click Preview.") : Text;
	}

	Text += FString::Printf(TEXT("%s, scope %s, %d manifest row(s). %s"),
		*Report.MapPackage, Report.ScopeSessionId.IsEmpty() ? TEXT("app") : *Report.ScopeSessionId,
		Report.ManifestRowCount, *BuildCountLine(Report));
	TSet<FString, FPreSeedRowKeyFuncs> AppScopedTypes;
	for (const FCrowdyPreSeedRow& Row : Report.Rows)
	{
		if (Row.bAppScoped)
		{
			AppScopedTypes.Add(Row.TypeName);
		}
	}
	for (const FString& TypeName : AppScopedTypes)
	{
		Text += FString::Printf(TEXT("\n  %s (app-scoped, applied app-wide)"), *TypeName);
	}
	for (const FCrowdyPreSeedRow& Row : Report.Rows)
	{
		const TCHAR* Mark = Row.Kind == ECrowdyPreSeedRowKind::Create ? TEXT("+")
			: Row.Kind == ECrowdyPreSeedRowKind::Existing ? TEXT("=") : TEXT("?");
		Text += FString::Printf(TEXT("\n  %s %s %s %s%s"), Mark, *Row.TypeName, *Row.BindingKey,
			*Row.DisplayName, Row.ContainerId.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" [%s]"), *Row.ContainerId));
	}
	if (Report.Orphans > 0)
	{
		Text += TEXT("\n\n? rows exist on the server with no placement in the map. Nothing here deletes them; a moved or copied actor gets a new key, so re-scan after editing the level.");
	}
	if (Report.Warnings.Num() > 0)
	{
		Text += TEXT("\n\nWarnings:");
		for (const FString& Warning : Report.Warnings)
		{
			Text += TEXT("\n  ! ") + Warning;
		}
	}
	return Text;
}
