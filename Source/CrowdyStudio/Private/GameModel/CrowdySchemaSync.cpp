// Fill out your copyright notice in the Description page of Project Settings.

#include "GameModel/CrowdySchemaSync.h"

#include "CrowdyStudioModule.h" // LogCrowdyStudio
#include "GameModel/CrowdyEffectPlanCache.h" // FCrowdyEffectGatherContext, FCrowdyEffectPlanCache::BuildRecords
#include "Replication/GameModel/CrowdyAttributeRegistry.h" // DiscoverForClass, GetContainerTypeName, FCrowdyAttributeDef
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"    // ModelChangedChannelPrefix / ModelChangedEventType
#include "Replication/GameModel/CrowdyModelValueCodec.h"      // EncodePropertyDefaultToJson for array/container_ref defaults
#include "Replication/GameModel/Effect/CrowdyEffect.h"        // UCrowdyEffect::Compile
#include "Replication/GameModel/Effect/CrowdyEffectLowering.h" // FCrowdyEffectLoweringResult

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"

#include "Engine/AssetManager.h"
#include "Engine/Blueprint.h"
#include "Engine/StreamableManager.h"
#include "Interfaces/IPluginManager.h"
#include "PluginDescriptor.h"
#include "PluginReferenceDescriptor.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/CrowdyJsonSafety.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

namespace
{
#if WITH_METADATA
	// A class meta from a fixed word list, lowercased; absent yields Default and an unrecognized value also warns.
	FString ReadContainerMetaWord(const UClass* Class, const TCHAR* MetaKey, std::initializer_list<const TCHAR*> Accepted,
		const TCHAR* Default, TArray<FString>* OutWarnings)
	{
		if (!Class->HasMetaData(MetaKey))
		{
			return Default;
		}
		const FString Value = Class->GetMetaData(MetaKey).TrimStartAndEnd().ToLower();
		for (const TCHAR* Word : Accepted)
		{
			if (Value == Word)
			{
				return Value;
			}
		}
		if (OutWarnings)
		{
			OutWarnings->Add(FString::Printf(TEXT("%s declares %s=\"%s\", which is not a recognized value; using '%s'."),
				*Class->GetName(), MetaKey, *Class->GetMetaData(MetaKey), Default));
		}
		return Default;
	}
#endif

	bool IsTransientClassName(const FString& Name)
	{
		return Name.StartsWith(TEXT("SKEL_"))
			|| Name.StartsWith(TEXT("REINST_"))
			|| Name.StartsWith(TEXT("TRASHCLASS_"))
			|| Name.StartsWith(TEXT("PLACEHOLDER-"));
	}

	const TCHAR* SdkPluginName = TEXT("CrowdySDK");

	// True when a plugin's transitive dependency closure reaches CrowdySDK, which is what makes its content
	// capable of declaring Crowdy metadata no matter where the plugin is installed.
	bool SchemaScanPluginDependsOnSdk(const TSharedRef<IPlugin>& Plugin, IPluginManager& PluginManager, TSet<FString>& Visited)
	{
		if (Visited.Contains(Plugin->GetName()))
		{
			return false;
		}
		Visited.Add(Plugin->GetName());

		for (const FPluginReferenceDescriptor& Reference : Plugin->GetDescriptor().Plugins)
		{
			if (Reference.Name == SdkPluginName)
			{
				return true;
			}
			if (const TSharedPtr<IPlugin> Dependency = PluginManager.FindPlugin(Reference.Name))
			{
				if (SchemaScanPluginDependsOnSdk(Dependency.ToSharedRef(), PluginManager, Visited))
				{
					return true;
				}
			}
		}
		return false;
	}

	// One streamed container scan at a time. The handle keeps the streamed assets resident until the completion
	// runs; a second request while one is in flight is answered rather than queued, so nothing waits forever.
	bool GContainerScanStreamInFlight = false;
	TSharedPtr<FStreamableHandle> GContainerScanStreamHandle;


	// JSON string-body escape (mirrors CrowdyGameModelSubsystem's EscapeJsonStringBody; the two live in
	// different modules so there is no unity-build collision, and canonicalization must stay identical
	// on both sides of the plane).
	FString EscapeJsonBody(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len() + 8);
		for (const TCHAR C : In)
		{
			switch (C)
			{
			case TEXT('\"'): Out += TEXT("\\\""); break;
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('\b'): Out += TEXT("\\b"); break;
			case TEXT('\f'): Out += TEXT("\\f"); break;
			case TEXT('\n'): Out += TEXT("\\n"); break;
			case TEXT('\r'): Out += TEXT("\\r"); break;
			case TEXT('\t'): Out += TEXT("\\t"); break;
			default:
				if (C < 0x20)
				{
					Out += FString::Printf(TEXT("\\u%04x"), static_cast<int32>(C));
				}
				else
				{
					Out += C;
				}
			}
		}
		return Out;
	}

	// A double as canonical JSON: an integral value prints as an integer so 100 and 100.0 compare equal;
	// a fractional value uses the same float print on both sides (mirrors JsonValueToCompactString).
	FString CanonicalNumber(double Number)
	{
		// EXACT integral test (not a tolerance): a value within a small epsilon of an integer, e.g. 0.0001, is a
		// genuine fractional default that must be preserved, not collapsed to "0". 100 and 100.0 still compare equal.
		if (FMath::Abs(Number) < 9.2e18 && Number == FMath::RoundToDouble(Number))
		{
			return FString::Printf(TEXT("%lld"), static_cast<int64>(FMath::RoundToDouble(Number)));
		}
		return FString::SanitizeFloat(Number);
	}

	// Canonical compact string for a JSON value (mirrors CrowdyGameModelSubsystem::JsonValueToCompactString):
	// integral numbers round to %lld, strings are escaped-quoted, objects/arrays serialize condensed.
	FString CanonicalizeJsonValue(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return FString();
		}
		switch (Value->Type)
		{
		case EJson::Boolean:
			return Value->AsBool() ? TEXT("true") : TEXT("false");
		case EJson::Number:
			return CanonicalNumber(Value->AsNumber());
		case EJson::String:
			return FString::Printf(TEXT("\"%s\""), *EscapeJsonBody(Value->AsString()));
		case EJson::Null:
			return TEXT("null");
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (!Object.IsValid())
			{
				return TEXT("{}");
			}
			// Emit keys in SORTED order (recursively) so a server that re-serializes an object in a different key
			// order still compares equal. FJsonSerializer alone preserves each side's own insertion order, which
			// would read a reordered multi-key invokePolicyJson (an and/or gate) as a spurious change.
			TArray<TPair<FString, TSharedPtr<FJsonValue>>> Pairs;
			Pairs.Reserve(Object->Values.Num());
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
			{
				Pairs.Add(Pair);
			}
			Pairs.Sort([](const TPair<FString, TSharedPtr<FJsonValue>>& A, const TPair<FString, TSharedPtr<FJsonValue>>& B)
			{
				return A.Key < B.Key;
			});
			FString Out = TEXT("{");
			for (int32 Index = 0; Index < Pairs.Num(); ++Index)
			{
				if (Index > 0)
				{
					Out += TEXT(",");
				}
				Out += TEXT("\"") + EscapeJsonBody(Pairs[Index].Key) + TEXT("\":")
					+ CanonicalizeJsonValue(Pairs[Index].Value);
			}
			return Out + TEXT("}");
		}
		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
			FString Out = TEXT("[");
			for (int32 Index = 0; Index < Array.Num(); ++Index)
			{
				if (Index > 0)
				{
					Out += TEXT(",");
				}
				Out += CanonicalizeJsonValue(Array[Index]); // arrays stay ordered (order is significant)
			}
			return Out + TEXT("]");
		}
		default:
			return FString();
		}
	}

	// Parses a standalone JSON value string ("87", "\"Aria\"", "12.5", "true"). A bare top-level scalar is
	// not valid JSON, so wrap it in an object and pull the value out (mirrors ParseJsonValueString).
	TSharedPtr<FJsonValue> ParseJsonValueString(const FString& Json)
	{
		// Reject a deeply-nested value before parsing: a forged server default-value string would otherwise build a
		// DOM that overflows the stack on teardown during an editor schema sync. Silent (the caller reads a null as
		// "no value"); the shipped-runtime siblings warn.
		if (!CrowdyJsonSafety::IsNestingWithinLimit(Json))
		{
			return nullptr;
		}
		const FString Wrapped = FString::Printf(TEXT("{\"v\":%s}"), *Json);
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Wrapped);
		TSharedPtr<FJsonObject> Object;
		if (FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid())
		{
			return Object->TryGetField(TEXT("v"));
		}
		return nullptr;
	}

	// Parameters match by NAME (order-independent), so a server that returns params in sortOrder rather than
	// authored order is not a false diff; sortOrder itself is compared, so a genuine reorder still upserts.
	// The default value is compared semantically (100 vs 100.0), the rest textually.
	bool FunctionParamsEqual(const TArray<FCrowdyGameModelFunctionParam>& Desired, const TArray<FStudioFunctionParam>& Current)
	{
		if (Desired.Num() != Current.Num())
		{
			return false;
		}
		TMap<FString, const FStudioFunctionParam*> CurByName;
		CurByName.Reserve(Current.Num());
		for (const FStudioFunctionParam& P : Current)
		{
			CurByName.Add(P.Name, &P);
		}
		for (const FCrowdyGameModelFunctionParam& D : Desired)
		{
			const FStudioFunctionParam* const* CP = CurByName.Find(D.Name);
			if (!CP)
			{
				return false;
			}
			const FStudioFunctionParam& C = **CP;
			if (C.ValueType != D.ValueType || C.bRequired != D.bRequired
				|| C.SortOrder != D.SortOrder || C.Description != D.Description
				|| !FCrowdySchemaSync::JsonValueEquals(C.DefaultValueJson, D.DefaultValueJson))
			{
				return false;
			}
		}
		return true;
	}

	// Mutations execute in sequence, so they are compared in ORDER (a reorder is a genuine behaviour change).
	bool FunctionMutationsEqual(const TArray<FCrowdyGameModelMutation>& Desired, const TArray<FStudioFunctionMutation>& Current)
	{
		if (Desired.Num() != Current.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Desired.Num(); ++Index)
		{
			if (Desired[Index].Target != Current[Index].Target
				|| Desired[Index].Property != Current[Index].Property
				|| Desired[Index].Expression != Current[Index].Expression)
			{
				return false;
			}
		}
		return true;
	}

	// The prune-candidate warning for a server property code does not declare. One place, because the per-asset plan
	// (which never prunes) has to drop exactly the warnings whose candidates it drops, and matching them by a second
	// copy of the sentence would silently stop matching the day either copy is reworded.
	FString SchemaSyncUndeclaredServerPropWarning(const FString& TypeName, const FString& Key)
	{
		return FString::Printf(
			TEXT("Server property '%s.%s' is not declared in code; left in place (the sync never deletes; prune it explicitly)."),
			*TypeName, *Key);
	}

	bool NotificationHasArg(const FCrowdyGameModelNotification& N, const TCHAR* Name)
	{
		return N.Args.ContainsByPredicate([Name](const FCrowdyGameModelNotificationArg& A) { return A.Name == Name; });
	}

	// A trigger's identity for diffing: the automation it fires, the event, and every filter, joined by a control
	// character no field contains. DebounceMs and WriteSource are the tunables outside the key, so changing either
	// updates the same trigger while a different filter combination reads as a distinct (new) one.
	FString AutomationTriggerKey(const FString& AutomationName, const FString& OnEvent,
		const FString& FunctionName, const FString& ContainerTypeName, const FString& PropertyKey)
	{
		return FString::Printf(TEXT("%s\x1f%s\x1f%s\x1f%s\x1f%s"),
			*AutomationName, *OnEvent, *FunctionName, *ContainerTypeName, *PropertyKey);
	}

	// Timer equality, order-independent like the notification compare: the server may echo an authored set in a
	// different order, and a reorder is not drift. An empty Target means "self" on both sides, so the two spellings
	// have to normalize before comparing or a synced function reports drift on every plan.
	bool OneTimerEquals(const FCrowdyGameModelTimer& A, const FCrowdyGameModelTimer& B)
	{
		const auto NormalizedTarget = [](const FString& In)
		{
			const FString Trimmed = In.TrimStartAndEnd();
			return Trimmed.IsEmpty() ? FString(TEXT("self")) : Trimmed;
		};
		if (A.FunctionName != B.FunctionName
			|| A.DelayMsExpression != B.DelayMsExpression
			|| A.DedupeKeyExpression != B.DedupeKeyExpression
			|| NormalizedTarget(A.Target) != NormalizedTarget(B.Target)
			|| A.Params.Num() != B.Params.Num())
		{
			return false;
		}
		// Params are name-keyed, not positional, matching how notification args compare.
		for (const FCrowdyGameModelTimerParam& ParamA : A.Params)
		{
			const FCrowdyGameModelTimerParam* Match = B.Params.FindByPredicate(
				[&ParamA](const FCrowdyGameModelTimerParam& Candidate) { return Candidate.Name == ParamA.Name; });
			if (!Match || Match->Expression != ParamA.Expression)
			{
				return false;
			}
		}
		return true;
	}

	bool FunctionTimersEqual(const TArray<FCrowdyGameModelTimer>& Desired, const TArray<FCrowdyGameModelTimer>& Current)
	{
		if (Desired.Num() != Current.Num())
		{
			return false;
		}
		TArray<bool> Claimed;
		Claimed.Init(false, Current.Num());
		for (const FCrowdyGameModelTimer& D : Desired)
		{
			bool bMatched = false;
			for (int32 Index = 0; Index < Current.Num(); ++Index)
			{
				if (!Claimed[Index] && OneTimerEquals(D, Current[Index]))
				{
					Claimed[Index] = true;
					bMatched = true;
					break;
				}
			}
			if (!bMatched)
			{
				return false;
			}
		}
		return true;
	}

	// Whether an authored writeSource differs from what the server reports. Absence on EITHER side matches anything:
	// a server that predates the field returns nothing, and a client that authors none (every non-property_changed
	// trigger; the field only means something for property writes) lets the server default it - observed live as
	// "any" echoed back for a function_invoked trigger. Reading either absence as drift re-upserts the trigger on
	// every plan, and the server's trigger upsert CREATES a row rather than updating, so the churn does not just
	// nag: it duplicated one trigger 7x on a live app, and every duplicate multiplied the automation's runs.
	// Only two concrete values that genuinely differ count as drift. Compared case- and whitespace-insensitively
	// because the value is a server enum spelled as a string.
	bool WriteSourceDiffers(const FString& ServerValue, const FString& DesiredValue)
	{
		const FString Server = ServerValue.TrimStartAndEnd();
		const FString Desired = DesiredValue.TrimStartAndEnd();
		if (Server.IsEmpty() || Desired.IsEmpty())
		{
			return false;
		}
		return !Server.Equals(Desired, ESearchCase::IgnoreCase);
	}

	// Whether the SDK can author this notification as-is. A channel notification needs a destination arg
	// (InjectSessionChannelTarget stamps channel_name before the diff); a spatial notification needs chunk
	// coordinates the pure lowering does not carry, so it is not authored yet (a named follow-up) and its server
	// form is preserved. The server takes EXACTLY ONE of channel_name and channel_id, so carrying both is as
	// unauthorable as carrying neither: accepting either alone would let a stale channel_id survive beside an
	// injected name and the mutation would fail at the server with nothing to point at earlier.
	bool IsAuthorableNotification(const FCrowdyGameModelNotification& N)
	{
		return N.Kind == TEXT("channel")
			&& NotificationHasArg(N, TEXT("channel_name")) != NotificationHasArg(N, TEXT("channel_id"));
	}

	bool NotificationArgsEqual(const TArray<FCrowdyGameModelNotificationArg>& A, const TArray<FCrowdyGameModelNotificationArg>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (const FCrowdyGameModelNotificationArg& ArgA : A)
		{
			const FCrowdyGameModelNotificationArg* Match = B.FindByPredicate(
				[&ArgA](const FCrowdyGameModelNotificationArg& X) { return X.Name == ArgA.Name; });
			if (!Match || Match->Expression != ArgA.Expression)
			{
				return false;
			}
		}
		return true;
	}

	// Whether two notifications' emitAs differ. An EMPTY value means "no emitAs was authored", and the marshaller
	// omits the field entirely in that case, so the server stores its own default and reports that default back. A
	// blank side therefore has to match anything, exactly as WriteSourceDiffers treats an absent server value: a
	// plain != comparison reads the server's default as drift and re-upserts the function on EVERY plan forever.
	// This is what made every function carrying a channel notification (every signal, every model-changed hint, and
	// the SDK's own __crowdy_touch_<type> on every container type) show up as a permanent pending update.
	bool EmitAsDiffers(const FString& A, const FString& B)
	{
		const FString TrimmedA = A.TrimStartAndEnd();
		const FString TrimmedB = B.TrimStartAndEnd();
		if (TrimmedA.IsEmpty() || TrimmedB.IsEmpty())
		{
			return false;
		}
		return !TrimmedA.Equals(TrimmedB, ESearchCase::IgnoreCase);
	}

	bool OneNotificationEquals(const FCrowdyGameModelNotification& A, const FCrowdyGameModelNotification& B)
	{
		return A.Kind == B.Kind && !EmitAsDiffers(A.EmitAs, B.EmitAs) && NotificationArgsEqual(A.Args, B.Args);
	}

	// Order-independent multiset equality: the server may echo notifications in a different order than authored, so
	// a reorder must not read as drift (each desired notification must match one distinct current notification).
	bool NotificationsEqual(const TArray<FCrowdyGameModelNotification>& Desired, const TArray<FCrowdyGameModelNotification>& Current)
	{
		if (Desired.Num() != Current.Num())
		{
			return false;
		}
		TArray<bool> Used;
		Used.Init(false, Current.Num());
		for (const FCrowdyGameModelNotification& D : Desired)
		{
			bool bMatched = false;
			for (int32 Index = 0; Index < Current.Num(); ++Index)
			{
				if (!Used[Index] && OneNotificationEquals(D, Current[Index]))
				{
					Used[Index] = true;
					bMatched = true;
					break;
				}
			}
			if (!bMatched)
			{
				return false;
			}
		}
		return true;
	}

	// Decide the notifications an upsert sends: author the effect's SDK notifications and preserve the server's
	// non-SDK (seed/console-authored) ones. Returns whether the result differs from Current (drives the upsert).
	//
	// The partitioning is per-KIND rather than all-or-nothing. An effect's signals and its model-changed hint share
	// one notifications array, so treating a single unauthorable entry as "author none of it" silently dropped every
	// signal on any effect whose carrier was Spatial: the warning named only the spatial notification, and the
	// signals were never written to the server at all. A kind the sync cannot author instead has the server's own
	// notifications of that kind preserved verbatim, while the kinds it can author are authored normally.
	bool ComputeUpsertNotifications(const FString& FunctionName,
		const TArray<FCrowdyGameModelNotification>& EffectNotifs,
		const TArray<FCrowdyGameModelNotification>& CurrentNotifs,
		TArray<FCrowdyGameModelNotification>& OutNotifs,
		TArray<FString>& OutWarnings)
	{
		TArray<FCrowdyGameModelNotification> Authorable;
		TSet<FString> UnauthorableKinds;
		for (const FCrowdyGameModelNotification& N : EffectNotifs)
		{
			if (IsAuthorableNotification(N))
			{
				Authorable.Add(N);
				continue;
			}

			UnauthorableKinds.Add(N.Kind);
			if (N.Kind == TEXT("channel"))
			{
				OutWarnings.Add(FString::Printf(
					TEXT("Function '%s' declares a channel notification (a model-changed hint or a signal) the sync cannot address: it names neither a channel nor exactly one destination. Its channel notifications were left unchanged."),
					*FunctionName));
			}
			else
			{
				OutWarnings.Add(FString::Printf(
					TEXT("Function '%s' declares a %s model-changed notification the sync does not author yet; its server notification of that kind was left unchanged. Any signal it declares is still authored."),
					*FunctionName, *N.Kind));
			}
		}

		// Nothing authorable is NOT a reason to short-circuit into "keep everything". The per-kind pass below already
		// preserves a non-SDK notification and an SDK-owned one of a kind this sync cannot express, and the
		// empty-result branch after it catches the case where that leaves nothing to send. Keeping the whole server
		// set here would also preserve the kinds the sync CAN author, so an effect whose carrier moved from Channel to
		// Spatial would leave its retired channel notification broadcasting forever.
		OutNotifs = MoveTemp(Authorable);
		for (const FCrowdyGameModelNotification& N : CurrentNotifs)
		{
			// Preserve a non-SDK server notification, and an SDK-owned one whose kind this sync cannot express:
			// overwriting the latter with nothing would silently retire a working notification.
			if (!FCrowdySchemaSync::IsSdkOwnedNotification(N) || UnauthorableKinds.Contains(N.Kind))
			{
				OutNotifs.Add(N);
			}
		}
		if (OutNotifs.Num() == 0 && CurrentNotifs.Num() > 0)
		{
			// An empty set cannot reach the server: the upsert omits the notifications key when the array is empty,
			// so the server keeps what it has and a plan asking for the removal would ask for it again on every
			// check, forever. Keep what the server has, and name what a sync cannot take away.
			OutWarnings.Add(FString::Printf(
				TEXT("Function '%s' no longer declares any notification, but a sync cannot remove the %d it already has on the server. Delete them on the Models tab if they are no longer wanted."),
				*FunctionName, CurrentNotifs.Num()));
			OutNotifs = CurrentNotifs;
			return false;
		}
		return !NotificationsEqual(OutNotifs, CurrentNotifs);
	}
}

FString FCrowdyContainerScanPlan::Describe() const
{
	return FString::Printf(
		TEXT("%d Blueprint asset(s) swept, %d container(s) named by tags, %d answered as non-containers with no load, ")
		TEXT("%d not yet tag-migrated (will load). %d asset(s) to load in total."),
		SweptAssetCount, KnownContainerCount, KnownNonContainerCount, UnmigratedAssetCount, AssetsToLoad.Num());
}

FString FCrowdyContainerScanPlan::DescribeStatus() const
{
	if (UnmigratedAssetCount == 0)
	{
		return TEXT("Reading this project's Game Model containers...");
	}
	return FString::Printf(
		TEXT("Reading this project's Game Model containers (%d asset(s) not yet tag-migrated, loading them)..."),
		UnmigratedAssetCount);
}

TArray<UClass*> FCrowdySchemaSync::GatherContainerClasses()
{
	TArray<UClass*> Candidates;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		Candidates.Add(*It);
	}
	return SelectContainerClasses(Candidates, /*bExcludeTestContainers*/ true);
}

TArray<UClass*> FCrowdySchemaSync::GatherNativeContainerClasses()
{
	TArray<UClass*> Candidates;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		// A Blueprint's generated class is reached through the registry sweep, which can decide whether its asset
		// needs loading. A native class has no asset at all, so it is only ever reachable this way.
		if (Class && Class->HasAnyClassFlags(CLASS_Native))
		{
			Candidates.Add(Class);
		}
	}
	return SelectContainerClasses(Candidates, /*bExcludeTestContainers*/ true);
}

bool FCrowdyContainerScan::IsPathUnderAnyRoot(const FString& PackagePath, const TArray<FString>& Roots)
{
	for (const FString& Raw : Roots)
	{
		FString Root = Raw.TrimStartAndEnd();
		Root.RemoveFromEnd(TEXT("/"));
		if (Root.IsEmpty())
		{
			// An empty entry would match every path, quietly turning one stray blank line in a setting into
			// "scan nothing" (or "exclude everything"), so it is ignored rather than honoured.
			continue;
		}
		if (PackagePath.Equals(Root, ESearchCase::IgnoreCase))
		{
			return true;
		}
		if (PackagePath.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

TArray<FString> FCrowdyContainerScan::ResolveScannedRoots()
{
	const UCrowdySchemaScanSettings* Settings = GetDefault<UCrowdySchemaScanSettings>();

	TArray<FString> Roots;
	if (Settings && Settings->ScannedContentRoots.Num() > 0)
	{
		Roots = Settings->ScannedContentRoots;
	}
	else
	{
		// The default: the game's own content, plus the content of any plugin that could declare Crowdy metadata.
		// Engine and unrelated third-party plugins are left out, which is also what keeps the sweep away from
		// sample content that hard-references editor-only assets.
		Roots.Add(TEXT("/Game"));

		IPluginManager& PluginManager = IPluginManager::Get();
		for (const TSharedRef<IPlugin>& Plugin : PluginManager.GetEnabledPluginsWithContent())
		{
			const EPluginType Type = Plugin->GetType();
			const bool bIsUserContent = (Type == EPluginType::Project || Type == EPluginType::Mod);
			const bool bIsSdk = (Plugin->GetName() == SdkPluginName);

			bool bConsumesSdk = false;
			if (!bIsUserContent && !bIsSdk)
			{
				TSet<FString> Visited;
				bConsumesSdk = SchemaScanPluginDependsOnSdk(Plugin, PluginManager, Visited);
			}
			if (!bIsUserContent && !bIsSdk && !bConsumesSdk)
			{
				continue;
			}

			FString Mount = Plugin->GetMountedAssetPath();
			Mount.RemoveFromEnd(TEXT("/"));
			if (!Mount.IsEmpty())
			{
				Roots.AddUnique(Mount);
			}
		}
	}

	const TArray<FString> Excluded = Settings ? Settings->ExcludedContentRoots : TArray<FString>();

	TArray<FString> Out;
	Out.Reserve(Roots.Num());
	for (const FString& Raw : Roots)
	{
		FString Root = Raw.TrimStartAndEnd();
		Root.RemoveFromEnd(TEXT("/"));
		if (Root.IsEmpty() || IsPathUnderAnyRoot(Root, Excluded))
		{
			continue;
		}
		Out.AddUnique(Root);
	}
	return Out;
}

FCrowdyContainerScanPlan FCrowdyContainerScan::PartitionScannedAssets(const TArray<FCrowdyScannedAsset>& Assets)
{
	FCrowdyContainerScanPlan Plan;
	Plan.SweptAssetCount = Assets.Num();

	const FString CurrentScanVersion(CrowdyGameModelMetaKeys::ScanAssetTagValue);

	for (const FCrowdyScannedAsset& Asset : Assets)
	{
		// Case-SENSITIVE: the version is an exact token written by one place and read by one place, and FString's
		// default comparison folds case, which would let a differently-cased future value read as this one.
		if (!Asset.ScanVersion.Equals(CurrentScanVersion, ESearchCase::CaseSensitive))
		{
			// The registry has no answer for this asset, so the only way to know is to open it. This is what keeps
			// an untagged container visible: the candidate set never depends on a tag being present, because a
			// container that disappears from a plan makes every model the live app holds a prune candidate.
			// It is also the migration's real candidate set: an already-tagged known container below still goes
			// into AssetsToLoad, but retagging it again would gain nothing, so it is kept out of UnmigratedAssets.
			Plan.AssetsToLoad.Add(Asset.ObjectPath);
			Plan.UnmigratedAssets.Add(Asset.ObjectPath);
			++Plan.UnmigratedAssetCount;
			continue;
		}

		if (Asset.ContainerTypeName.IsEmpty())
		{
			// Described, and described as not a container. This is the answer that removes the load, and it is the
			// only reason the sweep gets cheaper: without it every non-container would stay in the load partition
			// forever.
			++Plan.KnownNonContainerCount;
			continue;
		}

		// A container still loads, exactly as it always has: the schema is reflected from the live class, and this
		// tag only spared the project the cost of opening everything that is not one.
		Plan.AssetsToLoad.Add(Asset.ObjectPath);
		Plan.KnownContainerTypeNames.Add(Asset.ContainerTypeName);
		++Plan.KnownContainerCount;
	}

	// Registry sweep order is not stable from one call to the next, and a stream's order is observable in the log
	// and in what a partially-completed load left resident. Sorting makes two scans over unchanged content behave
	// identically.
	Plan.AssetsToLoad.Sort(FSoftObjectPathLexicalLess());
	Plan.UnmigratedAssets.Sort(FSoftObjectPathLexicalLess());
	Plan.KnownContainerTypeNames.Sort([](const FString& A, const FString& B)
	{
		return A.Compare(B, ESearchCase::CaseSensitive) < 0;
	});

	return Plan;
}

FCrowdyContainerScanPlan FCrowdyContainerScan::BuildContainerScanPlan()
{
	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	AssetRegistry.WaitForCompletion();

	// Blueprints only. A container is a CLASS, and the schema reflects classes, so user-defined structs are not
	// candidates here even though the cooked-metadata bake sweeps them for its own reasons. Including them would
	// also park every one of them permanently in the load partition, since nothing ever tags a struct.
	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());

	for (const FString& Root : ResolveScannedRoots())
	{
		Filter.PackagePaths.AddUnique(FName(*Root));
	}

	if (Filter.PackagePaths.Num() == 0)
	{
		// Every root was excluded, which would otherwise make the filter unscoped and sweep the whole engine.
		UE_LOG(LogCrowdyStudio, Warning,
			TEXT("Game Model container scan: every scanned content root is excluded, so no Blueprint container can be found. Check the Crowdy Game Model Schema Scan settings."));
		return FCrowdyContainerScanPlan();
	}

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	const FName ScanTagName(CrowdyGameModelMetaKeys::ScanAssetTag);
	const FName ContainerTagName(CrowdyGameModelMetaKeys::ContainerTypeAssetTag);

	TArray<FCrowdyScannedAsset> Scanned;
	Scanned.Reserve(Assets.Num());
	for (const FAssetData& Asset : Assets)
	{
		FCrowdyScannedAsset Entry;
		Entry.ObjectPath = Asset.ToSoftObjectPath();
		Entry.AssetPath = Asset.GetObjectPathString();
		Asset.GetTagValue<FString>(ScanTagName, Entry.ScanVersion);
		Asset.GetTagValue<FString>(ContainerTagName, Entry.ContainerTypeName);
		Scanned.Add(MoveTemp(Entry));
	}

	return PartitionScannedAssets(Scanned);
}

void FCrowdySchemaSync::StreamContainerScanPlan(const FCrowdyContainerScanPlan& Plan, TFunction<void()> OnComplete)
{
	UE_LOG(LogCrowdyStudio, Log, TEXT("Game Model container scan: %s"), *Plan.Describe());

	if (Plan.AssetsToLoad.Num() == 0)
	{
		if (OnComplete)
		{
			OnComplete();
		}
		return;
	}

	// No interactive editor to keep responsive (a commandlet), so the synchronous load is what the caller wants.
	if (IsRunningCommandlet())
	{
		for (const FSoftObjectPath& Path : Plan.AssetsToLoad)
		{
			Path.TryLoad();
		}
		if (OnComplete)
		{
			OnComplete();
		}
		return;
	}

	if (GContainerScanStreamInFlight)
	{
		UE_LOG(LogCrowdyStudio, Log,
			TEXT("Game Model container scan: a stream is already in flight, so this request is answered from what is resident."));
		if (OnComplete)
		{
			OnComplete();
		}
		return;
	}

	GContainerScanStreamInFlight = true;

	TArray<FSoftObjectPath> Paths = Plan.AssetsToLoad;
	// The completion is COPIED into the delegate rather than moved, because the refusal path below still has to be
	// able to call it. Moving would empty the local exactly when it is the only way to tell the caller that
	// nothing is coming.
	GContainerScanStreamHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(
		MoveTemp(Paths),
		FStreamableDelegate::CreateLambda([OnComplete]() mutable
		{
			GContainerScanStreamHandle.Reset();
			GContainerScanStreamInFlight = false;
			if (OnComplete)
			{
				OnComplete();
			}
		}),
		FStreamableManager::AsyncLoadHighPriority);

	if (!GContainerScanStreamHandle.IsValid())
	{
		// The request was refused outright, so no completion is ever coming from the delegate above. Clear the
		// latch and answer now, or a plan waits behind a guard nothing will release.
		GContainerScanStreamInFlight = false;
		if (OnComplete)
		{
			OnComplete();
		}
	}
}

TArray<UClass*> FCrowdySchemaSync::SelectContainerClasses(
	const TArray<UClass*>& Candidates, bool bExcludeTestContainers)
{
	TArray<UClass*> Out;
	for (UClass* Class : Candidates)
	{
		if (!Class || IsTransientClassName(Class->GetName()))
		{
			continue;
		}
		// A test-only container fixture (meta=(CrowdyContainerTest)) is reflected like any container class but must
		// never reach a live app's schema, so skip it here or the schema sync would upsert a bogus type.
		if (bExcludeTestContainers && FCrowdyAttributeRegistry::IsTestContainer(Class))
		{
			continue;
		}
		// The CrowdyContainer tag is the whole declaration. Requiring an accepted Server Owned attribute as well
		// dropped every container whose contribution is functions (signals, timers, automations): its type never
		// entered the desired schema, so it was never created, its effects' functions had no type to bind to, and a
		// type that already existed on the server read as an orphan and was offered for prune.
		FString TypeName;
		if (FCrowdyAttributeRegistry::GetContainerTypeName(Class, TypeName))
		{
			Out.Add(Class);
		}
	}
	return Out;
}

TArray<FCrowdyDesiredContainerType> FCrowdySchemaSync::BuildDesiredSchema(
	const TArray<UClass*>& Classes, TArray<FString>& OutWarnings)
{
	TArray<FCrowdyDesiredContainerType> Out;
	TMap<FString, FString> TypeNameToClass; // typeName -> first declaring class path (dup detection)

	for (const UClass* Class : Classes)
	{
		FCrowdyDesiredContainerType Desired = BuildDesiredForClass(Class, &OutWarnings);
		if (Desired.TypeName.IsEmpty())
		{
			continue;
		}
		if (const FString* Existing = TypeNameToClass.Find(Desired.TypeName))
		{
			OutWarnings.Add(FString::Printf(
				TEXT("Container type '%s' is declared by more than one class ('%s' and '%s'); using the first. Give each a unique CrowdyContainer tag."),
				*Desired.TypeName, **Existing, Class ? *Class->GetPathName() : TEXT("?")));
			continue;
		}
		TypeNameToClass.Add(Desired.TypeName, Class ? Class->GetPathName() : FString());
		Out.Add(MoveTemp(Desired));
	}
	return Out;
}

FCrowdyDesiredContainerType FCrowdySchemaSync::BuildDesiredForClass(const UClass* Class, TArray<FString>* OutWarnings)
{
	FCrowdyDesiredContainerType Desired;
	if (!Class)
	{
		return Desired;
	}

	FString TypeName;
	if (!FCrowdyAttributeRegistry::GetContainerTypeName(Class, TypeName))
	{
		return Desired; // no CrowdyContainer tag -> empty (caller skips)
	}
	Desired.TypeName = TypeName;
	Desired.DisplayName = TypeName;
#if WITH_METADATA
	Desired.Scope = ReadContainerMetaWord(Class, CrowdyGameModelMetaKeys::Scope,
		{ TEXT("session"), TEXT("app") }, TEXT("session"), OutWarnings);
	Desired.InstantiableBy = ReadContainerMetaWord(Class, CrowdyGameModelMetaKeys::InstantiableBy,
		{ TEXT("member"), TEXT("admin"), TEXT("owner") }, TEXT("member"), OutWarnings);
#endif
	// Recorded as a soft class path so a reader can round-trip it back to the declaring Blueprint asset or C++ class.
	Desired.OwningClassPath = FSoftClassPath(Class).ToString();

	const UObject* CDO = Class->GetDefaultObject();
	const TArray<FCrowdyAttributeDef> Defs = FCrowdyAttributeRegistry::DiscoverForClass(Class);
	Desired.Props.Reserve(Defs.Num());
	for (const FCrowdyAttributeDef& Def : Defs)
	{
		FCrowdyDesiredPropertyDef Prop;
		// Carried alongside the key rather than derived from it later: the key is already lowercased by the time
		// anything downstream sees it, so the authored spelling has to travel with it or it is gone for good.
		Prop.PropertyName = Def.PropertyName;
		Prop.Key = Def.Key;
		Prop.ValueType = Def.ValueType;
		Prop.Visibility = Def.Visibility; // meta=(CrowdyVisibility); Writable keeps its "function" default
		// Carried but never upserted: the server has no clamp concept, while an effect's assignment lowers itself
		// wrapped in these bounds, so they belong to what an effect compiled against.
		Prop.bHasClamp = Def.bHasClamp;
		Prop.ClampMin = Def.ClampMin;
		Prop.ClampMax = Def.ClampMax;
		const FProperty* Property = Class->FindPropertyByName(Def.PropertyName);
		Prop.DefaultValueJson = PropertyDefaultToJson(Property, CDO);
		Desired.Props.Add(MoveTemp(Prop));
	}
	return Desired;
}

FString FCrowdySchemaSync::PropertyDefaultToJson(const FProperty* Property, const UObject* CDO)
{
	if (!Property || !CDO)
	{
		return FString();
	}
	const void* Value = Property->ContainerPtrToValuePtr<void>(CDO);

	if (const FBoolProperty* B = CastField<FBoolProperty>(Property))
	{
		return B->GetPropertyValue(Value) ? TEXT("true") : TEXT("false");
	}
	if (const FStrProperty* S = CastField<FStrProperty>(Property))
	{
		return FString::Printf(TEXT("\"%s\""), *EscapeJsonBody(S->GetPropertyValue(Value)));
	}
	// An enum class : uint8 discovers as "int"; read its underlying integer for the default.
	if (const FEnumProperty* E = CastField<FEnumProperty>(Property))
	{
		const FNumericProperty* Under = E->GetUnderlyingProperty();
		return Under ? FString::Printf(TEXT("%lld"), Under->GetSignedIntPropertyValue(Value)) : FString();
	}
	// FNumericProperty covers every integer width + byte + both floats (parity with MapPropertyToValueType).
	if (const FNumericProperty* N = CastField<FNumericProperty>(Property))
	{
		return N->IsFloatingPoint()
			? CanonicalNumber(N->GetFloatingPointPropertyValue(Value))
			: FString::Printf(TEXT("%lld"), N->GetSignedIntPropertyValue(Value));
	}
	// Aggregates (a scalar array, an FCrowdyModelRef, a plain-struct object) canonicalize through the shared
	// codec; a scalar default's bespoke formatting above is kept here because its exact text is contract-tested.
	FString Aggregate;
	if (FCrowdyModelValueCodec::EncodePropertyDefaultToJson(Property, Value, Aggregate))
	{
		return Aggregate;
	}
	return FString();
}

bool FCrowdySchemaSync::JsonValueEquals(const FString& A, const FString& B)
{
	const FString TA = A.TrimStartAndEnd();
	const FString TB = B.TrimStartAndEnd();
	if (TA == TB)
	{
		return true;
	}
	if (TA.IsEmpty() || TB.IsEmpty())
	{
		return false; // an absent default is not equal to a present one (e.g. "" vs "0")
	}
	const TSharedPtr<FJsonValue> VA = ParseJsonValueString(TA);
	const TSharedPtr<FJsonValue> VB = ParseJsonValueString(TB);
	if (!VA.IsValid() || !VB.IsValid())
	{
		return false; // unparseable JSON on one side: TA != TB and both non-empty here, so treat as changed
	}
	return CanonicalizeJsonValue(VA) == CanonicalizeJsonValue(VB);
}

bool FCrowdySchemaSync::InvokePolicyEquals(const FString& Desired, const FString& Current)
{
	const FString TrimmedDesired = Desired.TrimStartAndEnd();
	const FString TrimmedCurrent = Current.TrimStartAndEnd();
	if (TrimmedDesired == TrimmedCurrent)
	{
		return true;
	}
	if (TrimmedDesired.IsEmpty() || TrimmedCurrent.IsEmpty())
	{
		return false; // no policy vs a policy is a real difference (and an empty send explicitly CLEARS the server's)
	}

	const TSharedPtr<FJsonValue> DesiredValue = ParseJsonValueString(TrimmedDesired);
	const TSharedPtr<FJsonValue> CurrentValue = ParseJsonValueString(TrimmedCurrent);
	if (!DesiredValue.IsValid() || !CurrentValue.IsValid())
	{
		return false;
	}

	// The server compiles a stored policy and writes the compiled form back INTO the policy JSON it returns: an
	// authored {"type":"condition","expression":"true"} reads back with an extra "ast" object beside it. That key
	// is server-computed, never authored, so comparing it makes every function carrying a policy read as drifted
	// on every plan, forever - which is how the SDK's own __crowdy_touch functions (an explicit policy is part of
	// their definition) and every owner-gated effect showed as a permanent pending update. Strip it from BOTH
	// sides before the semantic compare, so a future SDK that echoes the enriched form back stays idempotent too.
	//
	// Recursive, because a policy is a TREE: an effect with two require lines lowers to an "and" node whose rules
	// array nests one condition node per require, and the server compiles EACH node, so the ast appears inside the
	// nested rules too. A top-level-only strip fixed the single-condition touch functions and left every
	// multi-requirement effect churning, which is exactly how the miss showed itself.
	TFunction<void(const TSharedPtr<FJsonValue>&)> StripServerComputed =
		[&StripServerComputed](const TSharedPtr<FJsonValue>& Value)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Value->TryGetObject(Object) && Object->IsValid())
		{
			(*Object)->RemoveField(TEXT("ast"));
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*Object)->Values)
			{
				StripServerComputed(Field.Value);
			}
			return;
		}
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Value->TryGetArray(Array))
		{
			for (const TSharedPtr<FJsonValue>& Element : *Array)
			{
				StripServerComputed(Element);
			}
		}
	};
	StripServerComputed(DesiredValue);
	StripServerComputed(CurrentValue);

	return CanonicalizeJsonValue(DesiredValue) == CanonicalizeJsonValue(CurrentValue);
}

FString FCrowdySchemaSync::ToKitSnakeCase(const FString& Name)
{
	FString Out;
	Out.Reserve(Name.Len() + 4);
	for (int32 Index = 0; Index < Name.Len(); ++Index)
	{
		const TCHAR C = Name[Index];
		if (C == TEXT(' ') || C == TEXT('-'))
		{
			if (Out.Len() > 0 && Out[Out.Len() - 1] != TEXT('_'))
			{
				Out.AppendChar(TEXT('_'));
			}
			continue;
		}
		if (FChar::IsUpper(C))
		{
			if (Index > 0 && (FChar::IsLower(Name[Index - 1]) || FChar::IsDigit(Name[Index - 1])))
			{
				Out.AppendChar(TEXT('_'));
			}
			Out.AppendChar(FChar::ToLower(C));
		}
		else
		{
			Out.AppendChar(C);
		}
	}
	return Out;
}

bool FCrowdySchemaSync::IsRecognizedKitTypeName(const FString& TypeName, const TSet<FString>& RecognizedTypePrefixes)
{
	for (const FString& Prefix : RecognizedTypePrefixes)
	{
		if (!Prefix.IsEmpty() && TypeName.StartsWith(Prefix, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	return false;
}

bool FCrowdySchemaSync::IsRecognizedKitFunctionName(const FString& FunctionName, const TSet<FString>& RecognizedTypePrefixes)
{
	for (const FString& Prefix : RecognizedTypePrefixes)
	{
		if (Prefix.IsEmpty())
		{
			continue;
		}
		const FString SnakePrefix = ToKitSnakeCase(Prefix) + TEXT("_");
		if (FunctionName.StartsWith(SnakePrefix, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	return false;
}

FCrowdySchemaDelta FCrowdySchemaSync::DiffSchema(
	const TArray<FCrowdyDesiredContainerType>& Desired,
	const TArray<FStudioContainerType>& CurrentTypes,
	const TMap<FString, TArray<FStudioPropertyDef>>& CurrentPropsByType,
	const TSet<FString>& RecognizedKitTypePrefixes,
	const TSet<FString>& RecognizedKitTypeNames)
{
	FCrowdySchemaDelta Delta;

	TMap<FString, const FStudioContainerType*> CurrentTypeByName;
	CurrentTypeByName.Reserve(CurrentTypes.Num());
	for (const FStudioContainerType& T : CurrentTypes)
	{
		CurrentTypeByName.Add(T.TypeName, &T);
	}

	TSet<FString> DesiredTypeNames;
	DesiredTypeNames.Reserve(Desired.Num());

	for (const FCrowdyDesiredContainerType& D : Desired)
	{
		DesiredTypeNames.Add(D.TypeName);

		const FStudioContainerType* const* CurPtr = CurrentTypeByName.Find(D.TypeName);
		const FStudioContainerType* Cur = CurPtr ? *CurPtr : nullptr;
		if (!Cur)
		{
			Delta.TypeUpserts.Add({ D, /*bIsNew*/ true });
		}
		else if (Cur->InstantiableBy != D.InstantiableBy || Cur->DefaultPropertyVisibility != D.DefaultVisibility
			|| Cur->Scope != D.Scope)
		{
			Delta.TypeUpserts.Add({ D, /*bIsNew*/ false });
			if (Cur->Scope != D.Scope)
			{
				Delta.Warnings.Add(FString::Printf(
					TEXT("Container type '%s' scope changes '%s' -> '%s' (where its rows live). The server refuses 'session' -> 'app' while the type still holds session rows."),
					*D.TypeName, *Cur->Scope, *D.Scope));
			}
			// instantiableBy / defaultPropertyVisibility gate who may create instances and what is visible by
			// default, so a change is access-control-relevant: disclose the exact transition in the plan (the
			// code has no meta to express a hand-locked admin/hidden type, so a sync resets it to the defaults).
			if (Cur->InstantiableBy != D.InstantiableBy)
			{
				Delta.Warnings.Add(FString::Printf(
					TEXT("Container type '%s' instantiableBy changes '%s' -> '%s' (who may create instances)."),
					*D.TypeName, *Cur->InstantiableBy, *D.InstantiableBy));
			}
			if (Cur->DefaultPropertyVisibility != D.DefaultVisibility)
			{
				Delta.Warnings.Add(FString::Printf(
					TEXT("Container type '%s' default property visibility changes '%s' -> '%s'."),
					*D.TypeName, *Cur->DefaultPropertyVisibility, *D.DefaultVisibility));
			}
		}
		// else: authored type fields already match -> no upsert.

		const TArray<FStudioPropertyDef>* CurProps = CurrentPropsByType.Find(D.TypeName);
		TMap<FString, const FStudioPropertyDef*> CurPropByKey;
		if (CurProps)
		{
			CurPropByKey.Reserve(CurProps->Num());
			for (const FStudioPropertyDef& P : *CurProps)
			{
				CurPropByKey.Add(P.Key, &P);
			}
		}

		TSet<FString> DesiredKeys;
		DesiredKeys.Reserve(D.Props.Num());
		for (const FCrowdyDesiredPropertyDef& P : D.Props)
		{
			DesiredKeys.Add(P.Key);

			const FStudioPropertyDef* const* CP = CurPropByKey.Find(P.Key);
			const FStudioPropertyDef* CurP = CP ? *CP : nullptr;
			if (!CurP)
			{
				Delta.PropUpserts.Add({ D.TypeName, P, /*bIsNew*/ true });
				continue;
			}

			const bool bTypeChanged = CurP->ValueType != P.ValueType;
			// A default that shrank to nothing cannot reach the server: the property upsert omits defaultValueJson
			// when the value is empty, so the server keeps the default it has. Planning that removal would re-plan it
			// on every check, forever. Suppress it as a reason to upsert, and name where it can actually be removed.
			// A property def is keyed by (type, key), so an omitted field really is "leave the stored one alone";
			// an omitted field inside a list the upsert replaces wholesale is a different thing entirely.
			const bool bDefaultCleared = P.DefaultValueJson.TrimStartAndEnd().IsEmpty()
				&& !CurP->DefaultValueJson.TrimStartAndEnd().IsEmpty();
			if (bDefaultCleared)
			{
				Delta.Warnings.Add(FString::Printf(
					TEXT("Property '%s.%s' no longer declares a default, but a sync cannot remove the '%s' the server already has. Clear it on the Models tab if it is no longer wanted."),
					*D.TypeName, *P.Key, *CurP->DefaultValueJson));
			}
			const bool bDefaultChanged = !bDefaultCleared && !JsonValueEquals(CurP->DefaultValueJson, P.DefaultValueJson);
			const bool bVisChanged = CurP->Visibility != P.Visibility;
			const bool bWriteChanged = CurP->Writable != P.Writable;
			if (bTypeChanged || bDefaultChanged || bVisChanged || bWriteChanged)
			{
				Delta.PropUpserts.Add({ D.TypeName, P, /*bIsNew*/ false });
				if (bTypeChanged)
				{
					Delta.Warnings.Add(FString::Printf(
						TEXT("Property '%s.%s' value type changes '%s' -> '%s'; existing container data may need migration."),
						*D.TypeName, *P.Key, *CurP->ValueType, *P.ValueType));
				}
				// A sync resets visibility/writable to the code-declared values. That can widen a hand-hardened
				// (hidden/admin) server property, so disclose the exact transition in the plan (mirrors the
				// type-level warning above).
				if (bVisChanged)
				{
					Delta.Warnings.Add(FString::Printf(
						TEXT("Property '%s.%s' visibility changes '%s' -> '%s' (who may read it)."),
						*D.TypeName, *P.Key, *CurP->Visibility, *P.Visibility));
				}
				if (bWriteChanged)
				{
					Delta.Warnings.Add(FString::Printf(
						TEXT("Property '%s.%s' writable changes '%s' -> '%s' (who may write it)."),
						*D.TypeName, *P.Key, *CurP->Writable, *P.Writable));
				}
			}
		}

		// Server-only props on a code-owned type: surfaced, never deleted. Defensively, if this code-owned type name
		// also matches a deployed Game Kit prefix, a kit generally owns its whole type, so leave its server-only props
		// in place rather than offer them for prune (non-destructive; a coincidental name overlap only stays cautious).
		if (CurProps)
		{
			const bool bKitOwnedType = IsRecognizedKitTypeName(D.TypeName, RecognizedKitTypePrefixes);
			for (const FStudioPropertyDef& P : *CurProps)
			{
				if (!DesiredKeys.Contains(P.Key))
				{
					if (bKitOwnedType)
					{
						Delta.Warnings.Add(FString::Printf(
							TEXT("Server property '%s.%s' is not declared in code but its type matches a deployed Game Kit prefix; left in place (kit-owned)."),
							*D.TypeName, *P.Key));
						continue;
					}
					Delta.Warnings.Add(SchemaSyncUndeclaredServerPropWarning(D.TypeName, P.Key));
					Delta.ServerOnlyProps.Add({ D.TypeName, P.Key });
				}
			}
		}
	}

	// Server-only types: surfaced, never deleted. A type whose name matches a deployed Game Kit prefix is kit-owned
	// schema (emitted from a kit blueprint, not reflected from a code class), so it is left in place and kept off the
	// prune list rather than mistaken for an orphan.
	for (const FStudioContainerType& T : CurrentTypes)
	{
		if (!DesiredTypeNames.Contains(T.TypeName))
		{
			if (IsRecognizedKitTypeName(T.TypeName, RecognizedKitTypePrefixes))
			{
				Delta.Warnings.Add(FString::Printf(
					TEXT("Server container type '%s' matches a deployed Game Kit type prefix; left in place (kit-owned, not a prune candidate)."),
					*T.TypeName));
				continue;
			}
			// The exact-name layer: a type the deploy actually seeded is kit-owned even when its name carries no
			// recognizable prefix (an empty-prefix kit's bare "Combatant"), so it is left in place, never pruned.
			if (RecognizedKitTypeNames.Contains(T.TypeName))
			{
				Delta.Warnings.Add(FString::Printf(
					TEXT("Server container type '%s' matches a deployed Game Kit type name; left in place (kit-owned, not a prune candidate)."),
					*T.TypeName));
				continue;
			}
			Delta.Warnings.Add(FString::Printf(
				TEXT("Server container type '%s' is not declared in code; left in place (the sync never deletes; prune it explicitly)."),
				*T.TypeName));
			Delta.ServerOnlyTypes.Add(T.TypeName);
		}
	}

	return Delta;
}

FCrowdySchemaSyncReport FCrowdySchemaSync::BuildReport(
	const FCrowdySchemaDelta& Delta, const TArray<FString>& ExtraWarnings, bool bApplied)
{
	FCrowdySchemaSyncReport Report;
	Report.bValid = true;
	Report.bApplied = bApplied;
	Report.ServerOnlyTypeCount = Delta.ServerOnlyTypes.Num();
	Report.ServerOnlyPropCount = Delta.ServerOnlyProps.Num();
	Report.ServerOnlyFunctionCount = Delta.ServerOnlyFunctions.Num();
	Report.ServerOnlyAutomationCount = Delta.ServerOnlyAutomations.Num();

	for (const FCrowdySchemaTypeUpsert& T : Delta.TypeUpserts)
	{
		(T.bIsNew ? Report.TypesToCreate : Report.TypesToUpdate)++;
		Report.Lines.Add(FString::Printf(TEXT("%s container type '%s'"),
			T.bIsNew ? TEXT("+ create") : TEXT("~ update"), *T.Type.TypeName));
	}
	for (const FCrowdySchemaPropUpsert& P : Delta.PropUpserts)
	{
		(P.bIsNew ? Report.PropsToCreate : Report.PropsToUpdate)++;
		Report.Lines.Add(FString::Printf(TEXT("%s property '%s.%s' (%s)"),
			P.bIsNew ? TEXT("+ create") : TEXT("~ update"), *P.ContainerTypeName, *P.Prop.Key, *P.Prop.ValueType));
	}
	for (const FCrowdySchemaFunctionUpsert& F : Delta.FunctionUpserts)
	{
		(F.bIsNew ? Report.FunctionsToCreate : Report.FunctionsToUpdate)++;
		Report.Lines.Add(FString::Printf(TEXT("%s function '%s' (%s)"),
			F.bIsNew ? TEXT("+ create") : TEXT("~ update"), *F.Function.Name, *F.Function.ContainerTypeName));
	}
	for (const FCrowdySchemaAutomationUpsert& A : Delta.AutomationUpserts)
	{
		(A.bIsNew ? Report.AutomationsToCreate : Report.AutomationsToUpdate)++;
		Report.Lines.Add(FString::Printf(TEXT("%s automation '%s' -> %s"),
			A.bIsNew ? TEXT("+ create") : TEXT("~ update"), *A.Automation.Name, *A.Automation.FunctionName));
	}
	for (const FCrowdySchemaTriggerUpsert& T : Delta.TriggerUpserts)
	{
		(T.bIsNew ? Report.TriggersToCreate : Report.TriggersToUpdate)++;
		Report.Lines.Add(FString::Printf(TEXT("%s trigger on '%s' (%s)"),
			T.bIsNew ? TEXT("+ create") : TEXT("~ update"), *T.Trigger.AutomationName, *T.Trigger.OnEvent));
	}

	// A hard duplicate-name conflict (GatherDesiredFunctions / GatherDesiredAutomations, marked via
	// MarkDuplicateConflictWarning) is not an advisory warning: neither authoring effect was added to the desired
	// list, so this plan cannot create or update that name at all until it is resolved. Promote it into the
	// durable StatusNote banner -- the same mechanism FCrowdyStudioController::FailSchemaPlan uses for a failed
	// read -- so it shows above the counts instead of being buried in the warning list.
	TArray<FString> AllWarnings = ExtraWarnings;
	AllWarnings.Append(Delta.Warnings);
	SplitDuplicateConflictWarnings(AllWarnings, Report.Warnings, Report.StatusNote);
	return Report;
}

FString FCrowdySchemaSync::ScopedNameKey(const FString& Scope, const FString& Name)
{
	// A line feed appears in neither a container type name nor a function or automation name, so no two distinct
	// (scope, name) pairs can produce the same key.
	return Scope + TEXT("\n") + Name;
}

TArray<FCrowdySchemaNameConflict> FCrowdySchemaSync::DetectDuplicateNames(
	const TArray<FCrowdySchemaNameCandidate>& Candidates)
{
	// Preserve input order for both the group iteration and each conflict's AssetPaths, so the result (and any
	// message built from it) is deterministic regardless of how the caller assembled the list, e.g. an
	// asset-registry sweep order that is not guaranteed stable run to run.
	struct FGroup
	{
		FString Scope;
		FString Name;
		TArray<FString> Paths;
	};

	TArray<FString> OrderedKeys;
	TMap<FString, FGroup> GroupsByKey;
	for (const FCrowdySchemaNameCandidate& Candidate : Candidates)
	{
		const FString Key = ScopedNameKey(Candidate.Scope, Candidate.Name);
		FGroup* Group = GroupsByKey.Find(Key);
		if (!Group)
		{
			OrderedKeys.Add(Key);
			Group = &GroupsByKey.Add(Key);
			Group->Scope = Candidate.Scope;
			Group->Name = Candidate.Name;
		}
		Group->Paths.Add(Candidate.AssetPath);
	}

	TArray<FCrowdySchemaNameConflict> Conflicts;
	for (const FString& Key : OrderedKeys)
	{
		const FGroup& Group = GroupsByKey[Key];
		if (Group.Paths.Num() < 2)
		{
			continue;
		}
		FCrowdySchemaNameConflict Conflict;
		Conflict.Scope = Group.Scope;
		Conflict.Name = Group.Name;
		Conflict.AssetPaths = Group.Paths;
		Conflicts.Add(MoveTemp(Conflict));
	}
	return Conflicts;
}

namespace
{
	// Not user-facing: SplitDuplicateConflictWarnings strips this before a warning reaches the report panel. Chosen
	// to be vanishingly unlikely to collide with the start of a genuine warning string.
	const TCHAR* const GDuplicateConflictWarningMarker = TEXT("[[crowdy-duplicate-name-conflict]] ");
}

FString FCrowdySchemaSync::MarkDuplicateConflictWarning(const FString& Message)
{
	return FString(GDuplicateConflictWarningMarker) + Message;
}

void FCrowdySchemaSync::SplitDuplicateConflictWarnings(
	const TArray<FString>& InWarnings, TArray<FString>& OutPlainWarnings, FString& OutStatusNote)
{
	OutPlainWarnings.Reset();
	OutStatusNote.Reset();

	const FString Marker = GDuplicateConflictWarningMarker;
	TArray<FString> ConflictMessages;
	for (const FString& Warning : InWarnings)
	{
		if (Warning.StartsWith(Marker))
		{
			ConflictMessages.Add(Warning.Mid(Marker.Len()));
		}
		else
		{
			OutPlainWarnings.Add(Warning);
		}
	}

	// The stripped message stays useful as ordinary warning-list context too; only the internal marker itself is
	// never user-facing.
	OutPlainWarnings.Append(ConflictMessages);

	if (ConflictMessages.Num() > 0)
	{
		OutStatusNote = FString::Printf(
			TEXT("Plan blocked by %d duplicate name conflict(s); each is excluded from this plan until fixed. %s"),
			ConflictMessages.Num(), *FString::Join(ConflictMessages, TEXT(" ")));
	}
}

TArray<FCrowdyGameModelFunctionInput> FCrowdySchemaSync::GatherDesiredFunctions(
	TArray<FString>& OutWarnings, TSet<FString>& OutRecognizedNames,
	TArray<FCrowdySchemaAuthorship>& OutAuthorship, FCrowdyEffectGatherContext* Context)
{
	// A caller with no context of its own gets a throwaway one: it sweeps and loads everything and remembers
	// nothing, which is the behaviour this gather has always had on its own.
	FCrowdyEffectGatherContext Standalone;
	FCrowdyEffectGatherContext& Sweep = Context ? *Context : Standalone;

	return SelectDesiredFunctions(
		FCrowdyEffectPlanCache::BuildRecords(Sweep), OutWarnings, OutRecognizedNames, OutAuthorship);
}

bool FCrowdySchemaSync::FunctionDoesNothing(const FCrowdyGameModelFunctionInput& Function)
{
	// Parameters and an invoke policy are deliberately not part of this test: neither changes any state on its own, so
	// a function carrying only those still has nothing to do when it is invoked.
	return Function.Mutations.IsEmpty()
		&& Function.Timers.IsEmpty()
		&& Function.Notifications.IsEmpty()
		&& Function.ReturnExpression.IsEmpty();
}

TArray<FCrowdyGameModelFunctionInput> FCrowdySchemaSync::SelectDesiredFunctions(
	const TArray<FCrowdyEffectPlanRecord>& Records, TArray<FString>& OutWarnings,
	TSet<FString>& OutRecognizedNames, TArray<FCrowdySchemaAuthorship>& OutAuthorship)
{
	TArray<FCrowdyGameModelFunctionInput> Out;

	// Every function this plan would otherwise sync, gathered before any duplicate exclusion so
	// DetectDuplicateNames sees the complete authorship of every name (not just whichever asset the sweep visited
	// first). AuthorshipIndex points back at the entry to mark when the pair turns out to be claimed twice.
	struct FFunctionCandidate
	{
		FString AssetPath;
		FCrowdyGameModelFunctionInput Function;
		int32 AuthorshipIndex = INDEX_NONE;
	};
	TArray<FFunctionCandidate> Candidates;

	for (const FCrowdyEffectPlanRecord& Record : Records)
	{
		// Record this effect's function name up front, before any skip, so DiffFunctions never treats the live server
		// function of a skipped (unmigrated / non-compiling / duplicate) effect as an orphan to prune.
		OutRecognizedNames.Add(Record.EffectiveFunctionName);

		// One authorship entry per effect asset, skipped ones included, so a name that never reached the server can
		// still be traced back to the asset that claims it. A skipped effect is scoped by its target container type
		// rather than by the compiled function, which it does not have.
		FCrowdySchemaAuthorship Authorship;
		Authorship.AssetPath = Record.AssetPath;
		Authorship.Scope = Record.TargetTypeName;
		Authorship.Name = Record.EffectiveFunctionName;

		if (Record.bCompileFailed)
		{
			OutWarnings.Add(FString::Printf(
				TEXT("Effect '%s' has %s; skipped (fix it, then re-plan)."),
				*Record.AssetPath, *Record.FirstCompileError));
			Authorship.bSkipped = true;
			OutAuthorship.Add(MoveTemp(Authorship));
			continue;
		}

		const FCrowdyGameModelFunctionInput& Fn = Record.Function;
		if (Fn.Name.IsEmpty())
		{
			OutWarnings.Add(FString::Printf(
				TEXT("Effect '%s' compiled to an empty function name; skipped."), *Record.AssetPath));
			Authorship.bSkipped = true;
			OutAuthorship.Add(MoveTemp(Authorship));
			continue;
		}
		// A designer effect must not author a function in the SDK's reserved collection-touch namespace, or it
		// would masquerade as (or shadow) a provisioned touch function. Skip + warn (its name is already recorded
		// above, so it is not treated as a prune orphan).
		if (CrowdyGameModelMetaKeys::IsReservedCollectionFunctionName(Fn.Name))
		{
			OutWarnings.Add(FString::Printf(
				TEXT("Effect '%s' uses the reserved '%s' function-name prefix; rename it (that prefix is reserved for SDK collection touch functions). It was NOT synced."),
				*Record.AssetPath, CrowdyGameModelMetaKeys::CollectionTouchFunctionPrefix));
			Authorship.bSkipped = true;
			OutAuthorship.Add(MoveTemp(Authorship));
			continue;
		}

		if (FunctionDoesNothing(Fn))
		{
			OutWarnings.Add(FString::Printf(TEXT(
				"Effect '%s' compiles to a function that writes nothing, arms no timer and returns nothing; it was NOT "
				"synced (that would replace the server's function of the same name with an empty one). Author a change "
				"in its body, or delete the asset."), *Record.AssetPath));
			Authorship.bSkipped = true;
			OutAuthorship.Add(MoveTemp(Authorship));
			continue;
		}

		// Scoped and named by the compiled function from here on, which is what the server resolves and what
		// DetectDuplicateNames groups by.
		Authorship.Scope = Fn.ContainerTypeName;
		Authorship.Name = Fn.Name;
		const int32 AuthorshipIndex = OutAuthorship.Add(MoveTemp(Authorship));

		Candidates.Add({ Record.AssetPath, Fn, AuthorshipIndex });
	}

	// A (container type, function name) pair authored by two or more effects is excluded from the desired list
	// entirely -- for BOTH authors, not just the second one seen -- so a duplicate can never silently override (or
	// silently be dropped in favor of) the other; asset-registry sweep order never decides a winner. The container
	// type is part of the identity because the server scopes a function by it: two container types may each declare
	// a "take_damage" and both sync normally. Each conflict is reported by pair and by every authoring asset path,
	// promoted into the report's StatusNote banner by BuildReport.
	TArray<FCrowdySchemaNameCandidate> NameCandidates;
	NameCandidates.Reserve(Candidates.Num());
	for (const FFunctionCandidate& Candidate : Candidates)
	{
		NameCandidates.Add({ Candidate.AssetPath, Candidate.Function.ContainerTypeName, Candidate.Function.Name });
	}

	TSet<FString> ConflictingKeys;
	for (const FCrowdySchemaNameConflict& Conflict : DetectDuplicateNames(NameCandidates))
	{
		ConflictingKeys.Add(ScopedNameKey(Conflict.Scope, Conflict.Name));
		OutWarnings.Add(MarkDuplicateConflictWarning(FString::Printf(TEXT(
			"Function name '%s' on container type '%s' is authored by %d effects: %s. Give each a unique function "
			"name on that container type; none of them will be synced until this is resolved."),
			*Conflict.Name, *Conflict.Scope, Conflict.AssetPaths.Num(),
			*FString::Join(Conflict.AssetPaths, TEXT(", ")))));
	}

	for (const FFunctionCandidate& Candidate : Candidates)
	{
		if (ConflictingKeys.Contains(
			ScopedNameKey(Candidate.Function.ContainerTypeName, Candidate.Function.Name)))
		{
			// Every author of the pair is marked, not just the one that lost a race, because none of them is synced.
			if (OutAuthorship.IsValidIndex(Candidate.AuthorshipIndex))
			{
				OutAuthorship[Candidate.AuthorshipIndex].bConflicted = true;
				OutAuthorship[Candidate.AuthorshipIndex].bSkipped = true;
			}
			continue;
		}
		Out.Add(Candidate.Function);
	}

	return Out;
}

void FCrowdySchemaSync::DiffFunctions(
	const TArray<FCrowdyGameModelFunctionInput>& Desired,
	const TArray<FStudioFunction>& CurrentFunctions,
	FCrowdySchemaDelta& InOutDelta,
	const TSet<FString>& RecognizedFunctionNames,
	const TSet<FString>& RecognizedKitTypePrefixes,
	const TSet<FString>& RecognizedKitFunctionNames)
{
	// The server identifies a function by (container type, name), so that pair is the primary lookup: without it,
	// two server functions sharing a name on different container types collapse onto one map entry and a desired
	// function is compared against whichever one happened to be added last.
	TMap<FString, const FStudioFunction*> CurrentByScopedName;
	CurrentByScopedName.Reserve(CurrentFunctions.Num());
	// The name-only view is the rebind fallback: when a function's container type changed, no scoped entry matches,
	// but a single server function still bears the name and is the one being rebound (rather than a create that
	// would strand the old definition). Ambiguous names (two server functions, neither on the desired container
	// type) deliberately fall through to a create.
	TMap<FString, TArray<const FStudioFunction*>> CurrentByNameOnly;
	CurrentByNameOnly.Reserve(CurrentFunctions.Num());
	for (const FStudioFunction& F : CurrentFunctions)
	{
		CurrentByScopedName.Add(ScopedNameKey(F.ContainerTypeName, F.Name), &F);
		CurrentByNameOnly.FindOrAdd(F.Name).Add(&F);
	}

	TSet<FString> DesiredNames;
	DesiredNames.Reserve(Desired.Num());

	for (const FCrowdyGameModelFunctionInput& D : Desired)
	{
		DesiredNames.Add(D.Name);

		const FStudioFunction* Cur = nullptr;
		if (const FStudioFunction* const* ScopedPtr =
			CurrentByScopedName.Find(ScopedNameKey(D.ContainerTypeName, D.Name)))
		{
			Cur = *ScopedPtr;
		}
		else if (const TArray<const FStudioFunction*>* SameName = CurrentByNameOnly.Find(D.Name))
		{
			if (SameName->Num() == 1)
			{
				Cur = (*SameName)[0];
			}
		}
		if (!Cur)
		{
			// A create authors the effect's complete SDK model-changed notification (its destination stamped
			// beforehand); an unaddressable channel / deferred spatial is skipped with a warning, not shipped incomplete.
			FCrowdyGameModelFunctionInput ToUpsert = D;
			ComputeUpsertNotifications(D.Name, D.Notifications, {}, ToUpsert.Notifications, InOutDelta.Warnings);
			InOutDelta.FunctionUpserts.Add({ MoveTemp(ToUpsert), /*bIsNew*/ true });
			continue;
		}

		// Plan the notifications up front (author the SDK's own, preserve non-SDK server ones); a difference is a
		// reason to upsert on its own. Warnings (e.g. an unresolved channel) surface regardless of whether the rest
		// of the definition changed, since "create the session channel" is actionable even for a synced function.
		FCrowdyGameModelFunctionInput ToUpsert = D;
		const bool bNotificationsChanged =
			ComputeUpsertNotifications(D.Name, D.Notifications, Cur->Notifications, ToUpsert.Notifications, InOutDelta.Warnings);

		const bool bContainerChanged = Cur->ContainerTypeName != D.ContainerTypeName;
		const bool bDescChanged = Cur->Description != D.Description;
		const bool bReturnTypeChanged = Cur->ReturnType != D.ReturnType;
		const bool bReturnExprChanged = Cur->ReturnExpression != D.ReturnExpression;
		const bool bScopeChanged = Cur->InvokeScope != D.InvokeScope;
		// An effect toggling "run automatically" flips the function's autonomous flag with no other change; it must
		// drive a function upsert or the flag never reaches the server (and the automation cannot run).
		const bool bAutonomousChanged = Cur->bAutonomousInvocable != D.bAutonomousInvocable;
		// Policy-specific compare: the server returns the stored policy with its compiled "ast" written into the
		// same JSON, and comparing that server-computed key against the lean authored form is permanent false drift.
		const bool bPolicyChanged = !InvokePolicyEquals(D.InvokePolicyJson, Cur->InvokePolicyJson);
		const bool bParamsChanged = !FunctionParamsEqual(D.Parameters, Cur->Parameters);
		const bool bMutationsChanged = !FunctionMutationsEqual(D.Mutations, Cur->Mutations);
		// A timer set that shrank to empty cannot reach the server: the upsert omits the timers key when the array
		// is empty, so the server keeps the timers it has. Planning that removal would re-plan it on every check,
		// forever. The upsert carries the server's own timers so it describes the state it really leaves behind.
		const bool bTimersCleared = D.Timers.Num() == 0 && Cur->Timers.Num() > 0;
		if (bTimersCleared)
		{
			ToUpsert.Timers = Cur->Timers;
			InOutDelta.Warnings.Add(FString::Printf(
				TEXT("Function '%s' no longer declares any timer, but a sync cannot remove the %d it already has on the server. Delete them on the Models tab if they are no longer wanted."),
				*D.Name, Cur->Timers.Num()));
		}
		const bool bTimersChanged = !bTimersCleared && !FunctionTimersEqual(D.Timers, Cur->Timers);

		if (bContainerChanged || bDescChanged || bReturnTypeChanged || bReturnExprChanged
			|| bScopeChanged || bAutonomousChanged || bPolicyChanged || bParamsChanged || bMutationsChanged
			|| bNotificationsChanged || bTimersChanged)
		{
			InOutDelta.FunctionUpserts.Add({ MoveTemp(ToUpsert), /*bIsNew*/ false });

			// A rebind, an invoke-scope change, or a policy change is invoke-authority-relevant: disclose it
			// in the plan the same way the type/property access-control transitions are surfaced.
			if (bContainerChanged)
			{
				InOutDelta.Warnings.Add(FString::Printf(
					TEXT("Function '%s' rebinds container type '%s' -> '%s'."),
					*D.Name, *Cur->ContainerTypeName, *D.ContainerTypeName));
			}
			if (bScopeChanged)
			{
				InOutDelta.Warnings.Add(FString::Printf(
					TEXT("Function '%s' invokeScope changes '%s' -> '%s' (who may invoke it)."),
					*D.Name, *Cur->InvokeScope, *D.InvokeScope));
			}
			if (bPolicyChanged)
			{
				InOutDelta.Warnings.Add(FString::Printf(
					TEXT("Function '%s' invoke policy changes (who may invoke it)."), *D.Name));
			}
		}
	}

	// Server-only functions: surfaced, never deleted (the opt-in prune deletes these via gameModelDeleteFunction).
	// A function some effect asset OWNS but that was skipped this plan (RecognizedFunctionNames: an unmigrated or
	// non-compiling effect) is NOT an orphan -- it is left in place and kept off the prune list, so a transiently
	// skipped effect can never lose its live server function. The skip already warned about the effect itself.
	for (const FStudioFunction& F : CurrentFunctions)
	{
		if (DesiredNames.Contains(F.Name) || RecognizedFunctionNames.Contains(F.Name))
		{
			continue;
		}
		// The exact-name layer: a function the deploy actually seeded is kit-owned even when its bare name carries no
		// recognizable prefix (an empty-prefix kit's "attack"), so it is left in place, never pruned.
		if (RecognizedKitFunctionNames.Contains(F.Name))
		{
			InOutDelta.Warnings.Add(FString::Printf(
				TEXT("Server function '%s' matches a deployed Game Kit function name; left in place (kit-owned, not a prune candidate)."),
				*F.Name));
			continue;
		}
		// A function whose name carries a deployed Game Kit's function-name prefix is kit-owned schema, not an effect
		// orphan; leave it in place and keep it off the prune list.
		if (IsRecognizedKitFunctionName(F.Name, RecognizedKitTypePrefixes))
		{
			InOutDelta.Warnings.Add(FString::Printf(
				TEXT("Server function '%s' matches a deployed Game Kit function prefix; left in place (kit-owned, not a prune candidate)."),
				*F.Name));
			continue;
		}
		// A composed kit function can be verb-first (e.g. 'open_guild_hall'), so its name does not carry the kit
		// prefix even though it belongs to a kit-owned container type. Recognize it by its owning type instead;
		// leave it in place and keep it off the prune list.
		if (IsRecognizedKitTypeName(F.ContainerTypeName, RecognizedKitTypePrefixes))
		{
			InOutDelta.Warnings.Add(FString::Printf(
				TEXT("Server function '%s' lives on a deployed Game Kit container type '%s'; left in place (kit-owned, not a prune candidate)."),
				*F.Name, *F.ContainerTypeName));
			continue;
		}
		InOutDelta.Warnings.Add(FString::Printf(
			TEXT("Server function '%s' is not authored by any effect; left in place (the sync never deletes; prune it explicitly)."),
			*F.Name));
		// Scoped by the container type this diff identified the function by. An empty container type on the server
		// record leaves the scope undetermined rather than app-wide: the read cannot tell "the server binds this to
		// nothing" from "the read did not carry a binding", and only the first of those is safe to present as fact.
		InOutDelta.ServerOnlyFunctions.Add(FCrowdySchemaFunctionRef{ F.ContainerTypeName, F.Name });
	}
}

FCrowdySchemaSyncReport FCrowdySchemaSync::PlanForSingleEffect(
	const FCrowdyDesiredContainerType& DesiredType,
	const FCrowdyGameModelFunctionInput& DesiredFunction,
	const TArray<FStudioContainerType>& CurrentTypes,
	const TMap<FString, TArray<FStudioPropertyDef>>& CurrentPropsByType,
	const TArray<FStudioFunction>& CurrentFunctions,
	const TArray<FString>& ExtraWarnings,
	FCrowdySchemaDelta& OutDelta)
{
	OutDelta = FCrowdySchemaDelta();

	// Diff the one target container type, scoping the server snapshot to just that type so no other server type is
	// ever surfaced (a per-asset sync neither prunes nor warns about unrelated schema). A missing type yields a create.
	if (!DesiredType.TypeName.IsEmpty())
	{
		TArray<FStudioContainerType> ScopedTypes = CurrentTypes.FilterByPredicate(
			[&DesiredType](const FStudioContainerType& T) { return T.TypeName == DesiredType.TypeName; });

		TMap<FString, TArray<FStudioPropertyDef>> ScopedProps;
		if (const TArray<FStudioPropertyDef>* Props = CurrentPropsByType.Find(DesiredType.TypeName))
		{
			ScopedProps.Add(DesiredType.TypeName, *Props);
		}

		OutDelta = DiffSchema({ DesiredType }, ScopedTypes, ScopedProps);
	}

	// Diff the one function, scoping the server functions to just this name so no other server function reads as an
	// orphan. Its own name is recognized so even the scoped set never yields a prune candidate. Channel notifications
	// are expected to be addressed by the caller (InjectSessionChannelTarget) before this runs.
	if (!DesiredFunction.Name.IsEmpty())
	{
		TArray<FStudioFunction> ScopedFunctions = CurrentFunctions.FilterByPredicate(
			[&DesiredFunction](const FStudioFunction& F) { return F.Name == DesiredFunction.Name; });

		TSet<FString> RecognizedNames;
		RecognizedNames.Add(DesiredFunction.Name);
		DiffFunctions({ DesiredFunction }, ScopedFunctions, OutDelta, RecognizedNames);
	}

	// A per-asset sync never prunes: drop any server-only entries the scoped diff surfaced (e.g. a server property on
	// this type that code no longer declares), so the plan only ever creates or updates. The warning naming each of
	// them goes with the candidate: it offers work only the console's review can do, and on this path it would fire
	// for every effect whose container type carries a server property code does not declare, the SDK's own
	// collection revision counter included.
	for (const FCrowdySchemaPropRef& Ref : OutDelta.ServerOnlyProps)
	{
		OutDelta.Warnings.Remove(SchemaSyncUndeclaredServerPropWarning(Ref.ContainerTypeName, Ref.Key));
	}
	OutDelta.ServerOnlyTypes.Reset();
	OutDelta.ServerOnlyProps.Reset();
	OutDelta.ServerOnlyFunctions.Reset();
	OutDelta.ServerOnlyAutomations.Reset();

	return BuildReport(OutDelta, ExtraWarnings, /*bApplied*/ false);
}

TArray<FCrowdyGameModelAutomationInput> FCrowdySchemaSync::GatherDesiredAutomations(
	TArray<FString>& OutWarnings,
	TSet<FString>& OutRecognizedAutomationNames,
	TArray<FCrowdyGameModelAutomationTriggerInput>& OutTriggers,
	TArray<FCrowdySchemaAuthorship>& OutAuthorship,
	FCrowdyEffectGatherContext* Context)
{
	FCrowdyEffectGatherContext Standalone;
	FCrowdyEffectGatherContext& Sweep = Context ? *Context : Standalone;

	return SelectDesiredAutomations(FCrowdyEffectPlanCache::BuildRecords(Sweep),
		OutWarnings, OutRecognizedAutomationNames, OutTriggers, OutAuthorship);
}

TArray<FCrowdyGameModelAutomationInput> FCrowdySchemaSync::SelectDesiredAutomations(
	const TArray<FCrowdyEffectPlanRecord>& Records,
	TArray<FString>& OutWarnings,
	TSet<FString>& OutRecognizedAutomationNames,
	TArray<FCrowdyGameModelAutomationTriggerInput>& OutTriggers,
	TArray<FCrowdySchemaAuthorship>& OutAuthorship)
{
	TArray<FCrowdyGameModelAutomationInput> Out;

	struct FAutomationCandidate
	{
		FString AssetPath;
		FCrowdyGameModelAutomationInput Automation;
		bool bHasTrigger = false;
		FCrowdyGameModelAutomationTriggerInput Trigger;
		// The (container type, name) key of the function this automation runs, so an automation can be dropped
		// along with a function the plan is not going to create.
		FString FunctionKey;
		int32 AuthorshipIndex = INDEX_NONE;
	};
	// Every automation this plan would otherwise sync, gathered before any duplicate exclusion so
	// DetectDuplicateNames sees the complete authorship of every name.
	TArray<FAutomationCandidate> Candidates;

	// The same function candidates SelectDesiredFunctions would collect, recomputed here from the same records. An
	// automation is only worth syncing if the function it points at is going to exist, and the two selections exclude
	// on different names (automation name vs function name), so the function-side exclusions have to be recomputed
	// rather than inferred from an automation name.
	TArray<FCrowdySchemaNameCandidate> FunctionCandidates;
	TSet<FString> UnsyncableFunctionKeys;

	for (const FCrowdyEffectPlanRecord& Record : Records)
	{
		const FString& AssetPath = Record.AssetPath;

		// Record a best-effort prune-protection name before any skip: an effect's automation defaults to its function
		// name, so record that up front (an override is recorded after compile). This deliberately over-protects (a
		// plain effect that authors no automation still contributes its name), which is non-destructive -- it only ever
		// keeps a server automation OFF the prune list -- and guarantees a transiently-skipped effect's live automation
		// is never mistaken for an orphan.
		OutRecognizedAutomationNames.Add(Record.EffectiveFunctionName);

		if (Record.bCompileFailed)
		{
			OutWarnings.Add(FString::Printf(
				TEXT("Effect '%s' has a compile error; its automation (if any) was skipped (fix it, then re-plan)."), *AssetPath));

			// Attributed even though nothing compiled. An effect that ran automatically before it broke still owns
			// whatever automation it put on the server, and an automation defaults to its effect's function name, which
			// is the one name a non-compiling effect still has (the same name recorded for prune protection above).
			// Without this entry that live automation is indistinguishable from one no asset in the project claims,
			// which is the deletion-candidate reading, while the plan itself keeps it off the prune list. An automation
			// name is unique app-wide, so the scope stays empty.
			FCrowdySchemaAuthorship Authorship;
			Authorship.AssetPath = AssetPath;
			Authorship.Name = Record.EffectiveFunctionName;
			Authorship.bSkipped = true;
			OutAuthorship.Add(MoveTemp(Authorship));
			continue;
		}

		// Mirror the function selection's own acceptance rules, so this pass knows exactly which functions the plan
		// will and will not create. A function in the SDK's reserved collection-touch namespace is never synced.
		const FString FunctionKey =
			ScopedNameKey(Record.Function.ContainerTypeName, Record.Function.Name);
		if (!Record.Function.Name.IsEmpty())
		{
			if (CrowdyGameModelMetaKeys::IsReservedCollectionFunctionName(Record.Function.Name))
			{
				UnsyncableFunctionKeys.Add(FunctionKey);
			}
			else
			{
				FunctionCandidates.Add({ AssetPath, Record.Function.ContainerTypeName, Record.Function.Name });
			}
		}
		else
		{
			UnsyncableFunctionKeys.Add(FunctionKey);
		}

		if (!Record.bHasAutomation)
		{
			// A plain effect that does not run automatically authors no automation, so there is no automation name
			// to attribute to it: an authorship entry here would invent an entity that does not exist. Its function
			// is attributed on the function side.
			continue;
		}

		// An automation name is unique app-wide on the server, so its authorship scope is empty, matching
		// ScopedNameKey's convention for a name the server does not scope by container type.
		FCrowdySchemaAuthorship Authorship;
		Authorship.AssetPath = AssetPath;
		Authorship.Name = Record.Automation.Name;

		const FCrowdyGameModelAutomationInput& Automation = Record.Automation;
		if (Automation.Name.IsEmpty())
		{
			OutWarnings.Add(FString::Printf(
				TEXT("Effect '%s' compiled to an empty automation name; skipped."), *AssetPath));
			// Named by the function it would have defaulted to, so the entry still points somewhere a reader can act on.
			Authorship.Name = Record.EffectiveFunctionName;
			Authorship.bSkipped = true;
			OutAuthorship.Add(MoveTemp(Authorship));
			continue;
		}

		// Record the effective (possibly overridden) automation name up front, before any duplicate exclusion, so
		// a conflicting effect's live server automation is still protected from prune.
		OutRecognizedAutomationNames.Add(Automation.Name);

		const int32 AuthorshipIndex = OutAuthorship.Add(MoveTemp(Authorship));

		// The automation's own key for its entry-point function: the automation names the function, and the effect
		// that compiled both puts them on the same container type.
		Candidates.Add({ AssetPath, Automation, Record.bHasTrigger, Record.Trigger,
			ScopedNameKey(Record.Function.ContainerTypeName, Automation.FunctionName), AuthorshipIndex });
	}

	// Any function name claimed by two or more effects on one container type is excluded from the plan by
	// GatherDesiredFunctions, so an automation pointing at it would be planned against a function that is never
	// going to be created. Drop those automations too, rather than leaving them dangling.
	for (const FCrowdySchemaNameConflict& FunctionConflict : DetectDuplicateNames(FunctionCandidates))
	{
		UnsyncableFunctionKeys.Add(ScopedNameKey(FunctionConflict.Scope, FunctionConflict.Name));
	}

	// A name authored by two or more effects is excluded entirely -- for BOTH authors, and its trigger goes with
	// it -- so a duplicate can never silently override (or silently lose to) the other; asset-registry sweep order
	// never decides a winner. Each conflict is reported by name and by every authoring asset path, promoted into
	// the report's StatusNote banner by BuildReport. An automation name is unique app-wide on the server, so unlike
	// a function name it is grouped on the name alone (an empty scope).
	TArray<FCrowdySchemaNameCandidate> NameCandidates;
	NameCandidates.Reserve(Candidates.Num());
	for (const FAutomationCandidate& Candidate : Candidates)
	{
		NameCandidates.Add({ Candidate.AssetPath, FString(), Candidate.Automation.Name });
	}

	TSet<FString> ConflictingNames;
	for (const FCrowdySchemaNameConflict& Conflict : DetectDuplicateNames(NameCandidates))
	{
		ConflictingNames.Add(Conflict.Name);
		OutWarnings.Add(MarkDuplicateConflictWarning(FString::Printf(TEXT(
			"Automation name '%s' is authored by %d effects: %s. Give each a unique automation name; none of them "
			"will be synced until this is resolved."),
			*Conflict.Name, Conflict.AssetPaths.Num(), *FString::Join(Conflict.AssetPaths, TEXT(", ")))));
	}

	for (const FAutomationCandidate& Candidate : Candidates)
	{
		if (ConflictingNames.Contains(Candidate.Automation.Name))
		{
			// Every author of the name is marked, not just the one that lost a race, because none of them is synced.
			if (OutAuthorship.IsValidIndex(Candidate.AuthorshipIndex))
			{
				OutAuthorship[Candidate.AuthorshipIndex].bConflicted = true;
				OutAuthorship[Candidate.AuthorshipIndex].bSkipped = true;
			}
			continue;
		}
		// An automation whose entry-point function is not being synced cannot run; syncing it would either dangle
		// or reference a function this plan never creates. The function-side skip already warned about its own
		// asset, so this only names the consequence for the automation.
		if (UnsyncableFunctionKeys.Contains(Candidate.FunctionKey))
		{
			OutWarnings.Add(FString::Printf(TEXT(
				"Automation '%s' (effect '%s') was NOT synced: the function '%s' it runs is excluded from this plan. "
				"Fix that function, then re-plan."),
				*Candidate.Automation.Name, *Candidate.AssetPath, *Candidate.Automation.FunctionName));
			if (OutAuthorship.IsValidIndex(Candidate.AuthorshipIndex))
			{
				OutAuthorship[Candidate.AuthorshipIndex].bSkipped = true;
			}
			continue;
		}
		Out.Add(Candidate.Automation);
		if (Candidate.bHasTrigger)
		{
			OutTriggers.Add(Candidate.Trigger);
		}
	}

	return Out;
}

void FCrowdySchemaSync::DiffAutomations(
	const TArray<FCrowdyGameModelAutomationInput>& DesiredAutomations,
	const TArray<FCrowdyGameModelAutomationTriggerInput>& DesiredTriggers,
	const TArray<FStudioAutomation>& CurrentAutomations,
	const TArray<FStudioAutomationTrigger>& CurrentTriggers,
	const TArray<FCrowdyGameModelFunctionInput>& DesiredFunctions,
	FCrowdySchemaDelta& InOutDelta,
	const TSet<FString>& RecognizedAutomationNames,
	const TSet<FString>& RecognizedKitTypePrefixes,
	const TSet<FString>& RecognizedKitAutomationNames)
{
	// Function lookups for the dangling / non-autonomous warnings.
	TMap<FString, const FCrowdyGameModelFunctionInput*> DesiredFunctionByName;
	DesiredFunctionByName.Reserve(DesiredFunctions.Num());
	for (const FCrowdyGameModelFunctionInput& F : DesiredFunctions)
	{
		DesiredFunctionByName.Add(F.Name, &F);
	}

	TMap<FString, const FStudioAutomation*> CurrentByName;
	CurrentByName.Reserve(CurrentAutomations.Num());
	for (const FStudioAutomation& A : CurrentAutomations)
	{
		CurrentByName.Add(A.Name, &A);
	}

	TSet<FString> DesiredNames;
	DesiredNames.Reserve(DesiredAutomations.Num());

	for (const FCrowdyGameModelAutomationInput& D : DesiredAutomations)
	{
		DesiredNames.Add(D.Name);

		// A model_function automation must reference a function that exists and is autonomous-invocable, or it cannot
		// run. Both are non-destructive warnings (the upsert still plans) so the author sees the broken reference.
		if (D.ActionKind == TEXT("model_function") && !D.FunctionName.IsEmpty())
		{
			const FCrowdyGameModelFunctionInput* const* FnPtr = DesiredFunctionByName.Find(D.FunctionName);
			if (!FnPtr)
			{
				InOutDelta.Warnings.Add(FString::Printf(
					TEXT("Automation '%s' invokes function '%s', which no effect authors; the automation will not run until that function exists."),
					*D.Name, *D.FunctionName));
			}
			else if (!(*FnPtr)->bAutonomousInvocable)
			{
				InOutDelta.Warnings.Add(FString::Printf(
					TEXT("Automation '%s' invokes function '%s', which is not marked autonomous-invocable; the automation cannot run it."),
					*D.Name, *D.FunctionName));
			}
		}

		const FStudioAutomation* const* CurPtr = CurrentByName.Find(D.Name);
		const FStudioAutomation* Cur = CurPtr ? *CurPtr : nullptr;
		if (!Cur)
		{
			InOutDelta.AutomationUpserts.Add({ D, /*bIsNew*/ true });
			continue;
		}

		// paramsJson / selectorJson carry no server byte-stability guarantee, so compare them semantically; every
		// other authored field is a scalar / enum-string compared by value. An empty desired paramsJson is the same
		// as the server's non-null "{}" default (the upsert writes "{}" for an empty value), so normalize before
		// comparing or a re-sync would perpetually re-upsert an automation with no static params.
		const FString DesiredParams = D.ParamsJson.IsEmpty() ? FString(TEXT("{}")) : D.ParamsJson;
		const bool bParamsChanged = !JsonValueEquals(Cur->ParamsJson, DesiredParams);
		const bool bSelectorChanged = !JsonValueEquals(Cur->SelectorJson, D.SelectorJson);
		bool bScalarChanged =
			Cur->Description != D.Description
			|| Cur->bEnabled != D.bEnabled
			|| Cur->ActionKind != D.ActionKind
			|| Cur->FunctionName != D.FunctionName
			|| Cur->TargetMode != D.TargetMode
			|| Cur->SelfContainerId != D.SelfContainerId
			|| Cur->TargetTypeName != D.TargetTypeName
			|| Cur->SessionId != D.SessionId
			|| Cur->TriggerType != D.TriggerType
			|| Cur->MaxTargets != D.MaxTargets
			|| Cur->GasLimit != D.GasLimit
			|| Cur->RunTimeoutMs != D.RunTimeoutMs
			|| Cur->MaxRunsPerMinute != D.MaxRunsPerMinute
			|| Cur->FailureThreshold != D.FailureThreshold
			|| Cur->CooldownMs != D.CooldownMs;

		// The schedule fields only apply to a schedule-triggered automation, and within that only the field the
		// schedule kind uses (intervalMs for interval, cronExpr for cron). An event or manual automation leaves them
		// unset on the server, and a cron automation leaves intervalMs unset, so comparing the desired struct's
		// leftover defaults against the server's unset values would read as perpetual drift. Diff only what is
		// actually emitted, matching how the upsert emits them.
		if (D.TriggerType == TEXT("schedule"))
		{
			bScalarChanged = bScalarChanged || Cur->ScheduleKind != D.ScheduleKind;
			if (D.ScheduleKind == TEXT("cron"))
			{
				bScalarChanged = bScalarChanged || Cur->CronExpr != D.CronExpr;
			}
			else
			{
				bScalarChanged = bScalarChanged || Cur->IntervalMs != D.IntervalMs;
			}
		}

		if (bParamsChanged || bSelectorChanged || bScalarChanged)
		{
			InOutDelta.AutomationUpserts.Add({ D, /*bIsNew*/ false });
		}
	}

	// Triggers matched by (automationName, onEvent) + every filter; DebounceMs is the only tunable outside the key, so
	// a debounce change updates the same trigger and a different filter combination reads as a new one. This slice does
	// not prune a server-only trigger (there is no server-only-trigger channel in the delta).
	TMap<FString, const FStudioAutomationTrigger*> CurrentTriggerByKey;
	CurrentTriggerByKey.Reserve(CurrentTriggers.Num());
	for (const FStudioAutomationTrigger& T : CurrentTriggers)
	{
		CurrentTriggerByKey.Add(
			AutomationTriggerKey(T.AutomationName, T.OnEvent, T.FunctionName, T.ContainerTypeName, T.PropertyKey), &T);
	}

	for (const FCrowdyGameModelAutomationTriggerInput& D : DesiredTriggers)
	{
		const FString Key = AutomationTriggerKey(D.AutomationName, D.OnEvent, D.FunctionName, D.ContainerTypeName, D.PropertyKey);
		const FStudioAutomationTrigger* const* CurPtr = CurrentTriggerByKey.Find(Key);
		const FStudioAutomationTrigger* Cur = CurPtr ? *CurPtr : nullptr;
		if (!Cur)
		{
			InOutDelta.TriggerUpserts.Add({ D, /*bIsNew*/ true });
		}
		else if (Cur->DebounceMs != D.DebounceMs || WriteSourceDiffers(Cur->WriteSource, D.WriteSource))
		{
			InOutDelta.TriggerUpserts.Add({ D, /*bIsNew*/ false });
		}
	}

	// Server-only automations: surfaced, never deleted (the opt-in prune deletes these via gameModelDeleteAutomation).
	// An automation owned by an effect skipped this plan (RecognizedAutomationNames) is left in place and kept off the
	// prune list, so a transiently-skipped effect never loses its live automation. Kit-deployed automations are likewise
	// protected: by exact name (RecognizedKitAutomationNames, the names a deploy seeded) or by a deployed kit's
	// function-name prefix (an automation is named from its entry-point function). Kit automations carry no universally-
	// known prefix, so a bare-named kit automation with no matching exact name would still be offered for prune; the
	// exact-name persistence for kit automations at deploy time is a named follow-up (until then a kit-deployed app
	// should not run the opt-in prune).
	for (const FStudioAutomation& A : CurrentAutomations)
	{
		if (DesiredNames.Contains(A.Name) || RecognizedAutomationNames.Contains(A.Name))
		{
			continue;
		}
		if (RecognizedKitAutomationNames.Contains(A.Name))
		{
			InOutDelta.Warnings.Add(FString::Printf(
				TEXT("Server automation '%s' matches a deployed Game Kit automation name; left in place (kit-owned, not a prune candidate)."),
				*A.Name));
			continue;
		}
		if (IsRecognizedKitFunctionName(A.Name, RecognizedKitTypePrefixes))
		{
			InOutDelta.Warnings.Add(FString::Printf(
				TEXT("Server automation '%s' matches a deployed Game Kit function prefix; left in place (kit-owned, not a prune candidate)."),
				*A.Name));
			continue;
		}
		InOutDelta.Warnings.Add(FString::Printf(
			TEXT("Server automation '%s' is not authored by any effect; left in place (the sync never deletes; prune it explicitly)."),
			*A.Name));
		InOutDelta.ServerOnlyAutomations.Add(A.Name);
	}
}

void FCrowdySchemaSync::AppendReservedCollectionSchema(
	TArray<FCrowdyDesiredContainerType>& InOutTypes,
	TArray<FCrowdyGameModelFunctionInput>& InOutFunctions,
	TSet<FString>& InOutRecognizedFunctionNames,
	TArray<FString>& OutWarnings)
{
	// Names already claimed by a gathered effect (or a previously provisioned touch this call) so a reserved-name
	// collision is caught before it would create an ambiguous function.
	TSet<FString> ClaimedFunctionNames = InOutRecognizedFunctionNames;
	for (const FCrowdyGameModelFunctionInput& Fn : InOutFunctions)
	{
		ClaimedFunctionNames.Add(Fn.Name);
	}

	for (FCrowdyDesiredContainerType& Type : InOutTypes)
	{
		if (Type.TypeName.IsEmpty())
		{
			continue;
		}

		// A designer attribute already resolves to the reserved crowdy_rev key, or a function already claims the
		// reserved touch name: skip this type's whole collection plumbing (rev + touch stay all-or-nothing, never a
		// rev with no notifier) rather than clobber the designer's property/function, and name the fix. This is the
		// plan-time collision error.
		const bool bRevCollision = Type.Props.ContainsByPredicate(
			[](const FCrowdyDesiredPropertyDef& P) { return CrowdyGameModelMetaKeys::IsReservedCollectionKey(P.Key); });

		const FString TouchName = CrowdyGameModelMetaKeys::CollectionTouchFunctionName(Type.TypeName);
		const bool bTouchCollision = ClaimedFunctionNames.Contains(TouchName);

		if (bRevCollision || bTouchCollision)
		{
			OutWarnings.Add(FString::Printf(TEXT(
				"Container type '%s' cannot get Model Collection support: it already declares the reserved '%s' "
				"attribute or the reserved '%s' function. Rename the conflicting attribute or effect; collections "
				"on this type stay disabled until then."),
				*Type.TypeName, CrowdyGameModelMetaKeys::CollectionRevKey, *TouchName));
			continue;
		}

		// crowdy_rev: an int revision counter (default 0), public so a bump is observable on a watcher's re-pull
		// (a hidden property would make the re-pull a no-op), written only through the touch function.
		FCrowdyDesiredPropertyDef Rev;
		Rev.Key = CrowdyGameModelMetaKeys::CollectionRevKey;
		Rev.ValueType = TEXT("int");
		Rev.DefaultValueJson = TEXT("0");
		Rev.Visibility = TEXT("public");
		Rev.Writable = TEXT("function");
		Type.Props.Add(MoveTemp(Rev));

		// The per-type touch function: bump self.crowdy_rev and emit the channel model-changed notification naming the
		// changed container (via the server-injected $self_container_id: the touch runs against the parent, so self is
		// the parent), so a watcher re-pulls and observes the bumped revision. Its channel-notification shape matches
		// the effect lowering exactly, so the existing InjectSessionChannelTarget / DiffFunctions /
		// IsSdkOwnedNotification path addresses it and authors it unchanged. The invoke policy is an
		// explicit always-true condition ("any entitled player"): collection edges are unrestricted graph operations
		// server-side, and the touch only bumps a bookkeeping counter and emits a re-pull hint (authority for the
		// actual item state lives on the owner-gated item functions). An explicit policy (vs an empty one) round-trips
		// idempotently, since an effect never lowers an empty policy for the diff to have exercised that path.
		FCrowdyGameModelFunctionInput Touch;
		Touch.Name = TouchName;
		Touch.ContainerTypeName = Type.TypeName;
		Touch.Description = TEXT("Bumps the collection revision and notifies watchers after a collection membership change.");
		Touch.InvokeScope = TEXT("player");
		Touch.InvokePolicyJson = TEXT("{\"type\":\"condition\",\"expression\":\"true\"}");

		FCrowdyGameModelMutation Bump;
		Bump.Target = TEXT("self");
		Bump.Property = CrowdyGameModelMetaKeys::CollectionRevKey;
		// coalesce guards a container created BEFORE this schema existed (a property-def default applies to new
		// containers, not retroactively): an unset crowdy_rev reads as null, so a bare "+ 1" could error or stay
		// null (no observable change on the re-pull). coalesce(...,0)+1 always yields an observable bump.
		Bump.Expression = FString::Printf(TEXT("coalesce(self.%s, 0) + 1"), CrowdyGameModelMetaKeys::CollectionRevKey);
		Touch.Mutations.Add(MoveTemp(Bump));

		FCrowdyGameModelNotification Notif;
		Notif.Kind = TEXT("channel");
		FCrowdyGameModelNotificationArg Payload;
		Payload.Name = TEXT("payload");
		// $self_container_id (a server-injected string naming the touched parent) is already a string, so no to_string
		// cast is needed; the touch runs against the parent, so self is the parent whose collection changed.
		Payload.Expression = FString::Printf(TEXT("concat(\"%s\", $%s)"),
			CrowdyGameModelMetaKeys::ModelChangedChannelPrefix, CrowdyGameModelMetaKeys::SelfContainerIdParam);
		Notif.Args.Add(MoveTemp(Payload));
		Touch.Notifications.Add(MoveTemp(Notif));

		ClaimedFunctionNames.Add(TouchName);
		InOutRecognizedFunctionNames.Add(TouchName);
		InOutFunctions.Add(MoveTemp(Touch));
	}
}

bool FCrowdySchemaSync::IsSdkOwnedNotification(const FCrowdyGameModelNotification& Notification)
{
	if (Notification.Kind == TEXT("channel"))
	{
		// The lowering authors payload = concat("cmc:", $self_container_id); the cmc: prefix identifies it as
		// SDK-owned regardless of what carries the id after it.
		for (const FCrowdyGameModelNotificationArg& Arg : Notification.Args)
		{
			if (Arg.Name == TEXT("payload")
				&& Arg.Expression.Contains(CrowdyGameModelMetaKeys::ModelChangedChannelPrefix))
			{
				return true;
			}
			// A signal is authored the same way with its own prefix. It has to be recognized here or the sync would
			// read every authored signal as a server-owned notification, preserve it, and add the authored copy
			// beside it, doubling the set on each plan. Being SDK-owned is also what gets it addressed at the channel.
			if (Arg.Name == TEXT("payload")
				&& Arg.Expression.Contains(CrowdyGameModelMetaKeys::SignalChannelPrefix))
			{
				return true;
			}
		}
	}
	else if (Notification.Kind == TEXT("spatial"))
	{
		// The lowering stamps the reserved model-changed event_type; that literal identifies the SDK's own one.
		const FString EventType = FString::FromInt(static_cast<int32>(CrowdyGameModelMetaKeys::ModelChangedEventType));
		for (const FCrowdyGameModelNotificationArg& Arg : Notification.Args)
		{
			if (Arg.Name == TEXT("event_type") && Arg.Expression == EventType)
			{
				return true;
			}
		}
	}
	return false;
}

void FCrowdySchemaSync::InjectSessionChannelTarget(TArray<FCrowdyGameModelFunctionInput>& Functions)
{
	const FString Expr = FString::Printf(TEXT("$%s"), CrowdyGameModelMetaKeys::SessionChannelNameParam);
	for (FCrowdyGameModelFunctionInput& Fn : Functions)
	{
		for (FCrowdyGameModelNotification& Notification : Fn.Notifications)
		{
			if (Notification.Kind != TEXT("channel") || !IsSdkOwnedNotification(Notification))
			{
				continue;
			}

			// The server takes exactly one destination, so a literal channel_id an earlier apply left behind has to
			// go rather than sit beside the name. This is also the migration: the diff sees a different arg set and
			// rewrites every function that still carries a baked id.
			Notification.Args.RemoveAll(
				[](const FCrowdyGameModelNotificationArg& A) { return A.Name == TEXT("channel_id"); });

			if (FCrowdyGameModelNotificationArg* Existing = Notification.Args.FindByPredicate(
					[](const FCrowdyGameModelNotificationArg& A) { return A.Name == TEXT("channel_name"); }))
			{
				Existing->Expression = Expr;
			}
			else
			{
				FCrowdyGameModelNotificationArg Arg;
				Arg.Name = TEXT("channel_name");
				Arg.Expression = Expr;
				Notification.Args.Add(MoveTemp(Arg));
			}
		}
	}
}

bool FCrowdySchemaSync::AnyFunctionNeedsSessionChannel(const TArray<FCrowdyGameModelFunctionInput>& Functions)
{
	// Mirror InjectSessionChannelTarget's filter: an SDK-owned channel notification is the only kind addressed at the
	// app's session channel, so it is the only thing whose absence a channel auto-create would fix.
	for (const FCrowdyGameModelFunctionInput& Fn : Functions)
	{
		for (const FCrowdyGameModelNotification& Notification : Fn.Notifications)
		{
			if (Notification.Kind == TEXT("channel") && IsSdkOwnedNotification(Notification))
			{
				return true;
			}
		}
	}
	return false;
}
