// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/CrowdyAttributeRegistry.h"

#include "CrowdyGameModelLog.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "Replication/GameModel/CrowdyModelValueCodec.h"
#include "Replication/State/CrowdyStateMetaKeys.h"
#include "Utils/CrowdyBakedRegistry.h" // cooked-build fallback (metadata is stripped)
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

#if WITH_EDITOR
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Blueprint.h"
#include "Modules/ModuleManager.h"
#endif

FString FCrowdyAttributeRegistry::MapPropertyToValueType(const FProperty* Property)
{
	if (!Property)
	{
		return FString();
	}
	// FNumericProperty covers every integer width (int8/16/32/64 + unsigned variants + byte/TEnumAsByte) and
	// both float widths; classify by IsFloatingPoint. enum class : uint8 (FEnumProperty) maps to its underlying
	// integer. bool and FString are their own leaves. Kept in lockstep with the codec's write branches so a
	// discovered type is always actually written.
	if (const FNumericProperty* Num = CastField<FNumericProperty>(Property))
	{
		return Num->IsFloatingPoint() ? FString(TEXT("float")) : FString(TEXT("int"));
	}
	if (CastField<FEnumProperty>(Property))
	{
		return TEXT("int");
	}
	if (CastField<FBoolProperty>(Property))
	{
		return TEXT("bool");
	}
	if (CastField<FStrProperty>(Property))
	{
		return TEXT("string");
	}
	// Aggregates: a scalar array maps to "array", an FCrowdyModelRef to "container_ref", and a plain native struct
	// (every member itself supported) to "object". The codec owns their element/member/reference validity so this
	// classifier and the value read/write can never disagree. Any other type (a Blueprint struct, an object
	// reference, a name/text, a map/set) stays unsupported.
	const FString Aggregate = FCrowdyModelValueCodec::MapAggregatePropertyToValueType(Property);
	if (!Aggregate.IsEmpty())
	{
		return Aggregate;
	}
	return FString();
}

bool FCrowdyAttributeRegistry::GetContainerTypeName(const UClass* Class, FString& OutTypeName)
{
#if WITH_METADATA
	if (Class && Class->HasMetaData(CrowdyGameModelMetaKeys::Container))
	{
		const FString TypeName = Class->GetMetaData(CrowdyGameModelMetaKeys::Container);
		if (!TypeName.IsEmpty())
		{
			OutTypeName = TypeName;
			return true;
		}
	}
	return false;
#else
	// Cooked builds strip UCLASS metadata; the tag comes from the baked table instead.
	return Class ? UCrowdyBakedRegistry::FindContainerTypeName(Class, OutTypeName) : false;
#endif
}

bool FCrowdyAttributeRegistry::IsContainerAppScoped(const UClass* Class)
{
	if (!Class)
	{
		return false;
	}
#if WITH_METADATA
	// Class metadata is not inherited: the nearest class in the chain that declares a scope decides.
	const UClass* Declaring = Class;
	while (Declaring && !Declaring->HasMetaData(CrowdyGameModelMetaKeys::Scope))
	{
		Declaring = Declaring->GetSuperClass();
	}
	if (!Declaring)
	{
		return false;
	}
	const FString Word = Declaring->GetMetaData(CrowdyGameModelMetaKeys::Scope).TrimStartAndEnd();
	if (Word.Equals(TEXT("App"), ESearchCase::IgnoreCase))
	{
		return true;
	}
	UE_CLOG(!Word.Equals(TEXT("Session"), ESearchCase::IgnoreCase), LogCrowdyGameModel, Warning,
		TEXT("[GameModel] class '%s' declares CrowdyScope=\"%s\", which is neither App nor Session; treating it as Session."),
		*Declaring->GetName(), *Word);
	return false;
#else
	return UCrowdyBakedRegistry::IsContainerClassAppScoped(Class);
#endif
}

namespace
{
	// A Blueprint compile leaves reflection classes behind that carry the same UCLASS metadata as the real generated
	// class: the skeleton class, and the reinstanced / trashed copies of a previous compile. Any of them would answer
	// a container-type lookup with the right name and the wrong class, so a scan over loaded classes has to skip
	// them. CLASS_NewerVersionExists covers a superseded class; the rest are recognized by the prefix the engine
	// gives them.
	bool IsTransientReflectionClass(const UClass* Class)
	{
		if (Class->HasAnyClassFlags(CLASS_NewerVersionExists))
		{
			return true;
		}
		const FString Name = Class->GetName();
		return Name.StartsWith(TEXT("SKEL_"))
			|| Name.StartsWith(TEXT("REINST_"))
			|| Name.StartsWith(TEXT("TRASHCLASS_"))
			|| Name.StartsWith(TEXT("PLACEHOLDER-"));
	}

#if WITH_EDITOR
	// A Blueprint container answers a type-name lookup only once something has opened it, so at editor startup a
	// type nothing has referenced yet reads as a type that does not exist. Every Blueprint save publishes its
	// container type name as an asset-registry tag, so a miss over loaded classes becomes a targeted load of the
	// one asset that claims the name rather than a denial.
	UClass* LoadContainerClassFromAssetRegistry(const FString& TypeName)
	{
		FAssetRegistryModule* Module = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry"));
		if (!Module)
		{
			return nullptr;
		}

		IAssetRegistry& AssetRegistry = Module->Get();
		FARFilter Filter;
		Filter.bRecursiveClasses = true;
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Filter.TagsAndValues.Add(FName(CrowdyGameModelMetaKeys::ContainerTypeAssetTag), TOptional<FString>(TypeName));

		TArray<FAssetData> Assets;
		AssetRegistry.GetAssets(Filter, Assets);
		if (Assets.IsEmpty() && AssetRegistry.IsLoadingAssets())
		{
			// What is missing is the scan, not the answer. Calling a declared type absent because a background
			// scan is still mid-flight would fail an effect that is correct, so pay the rest of the scan once.
			AssetRegistry.WaitForCompletion();
			AssetRegistry.GetAssets(Filter, Assets);
		}

		for (const FAssetData& Asset : Assets)
		{
			const UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
			UClass* Generated = Blueprint ? Blueprint->GeneratedClass.Get() : nullptr;
			if (!Generated || FCrowdyAttributeRegistry::IsTestContainer(Generated))
			{
				continue;
			}
			// The registry compares tag values case-insensitively, so the tag only narrows the candidates and the
			// loaded class decides, on the same case-sensitive comparison the scan over loaded classes uses.
			FString Candidate;
			if (FCrowdyAttributeRegistry::GetContainerTypeName(Generated, Candidate)
				&& Candidate.Equals(TypeName, ESearchCase::CaseSensitive))
			{
				return Generated;
			}
		}
		return nullptr;
	}
#endif
}

UClass* FCrowdyAttributeRegistry::FindContainerClassByTypeName(const FString& TypeName)
{
	if (TypeName.IsEmpty())
	{
		return nullptr;
	}
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (!IsValid(Class) || IsTransientReflectionClass(Class) || IsTestContainer(Class))
		{
			continue;
		}
		FString Candidate;
		if (GetContainerTypeName(Class, Candidate) && Candidate.Equals(TypeName, ESearchCase::CaseSensitive))
		{
			return Class;
		}
	}
#if WITH_EDITOR
	return LoadContainerClassFromAssetRegistry(TypeName);
#else
	return nullptr;
#endif
}

bool FCrowdyAttributeRegistry::IsTestContainer(const UClass* Class)
{
#if WITH_METADATA
	return Class && Class->HasMetaData(CrowdyGameModelMetaKeys::ContainerTest);
#else
	// Every caller (the schema sync, the container-type lookup) is an authoring path, so a cooked build never asks;
	// a test fixture is never a real cooked container either.
	return false;
#endif
}

bool FCrowdyAttributeRegistry::IsTestFixture(const UClass* Class)
{
#if WITH_METADATA
	return Class && Class->HasMetaData(CrowdyGameModelMetaKeys::TestFixture);
#else
	// Metadata is stripped in a cooked build, so this answers false there, and every caller is an editor or startup-sweep path.
	return false;
#endif
}

namespace
{
	// Reads a native ClampMin/ClampMax pair off a property, reusing Unreal's own clamp metadata instead of a
	// bespoke "lo..hi" string. Both must be present and parse as numbers for a clamp to be recorded.
	void ReadNativeClamp(const FProperty* Property, FCrowdyAttributeDef& Def)
	{
#if WITH_METADATA
		if (!Property || !Property->HasMetaData(TEXT("ClampMin")) || !Property->HasMetaData(TEXT("ClampMax")))
		{
			return;
		}
		const FString MinStr = Property->GetMetaData(TEXT("ClampMin"));
		const FString MaxStr = Property->GetMetaData(TEXT("ClampMax"));
		if (MinStr.IsNumeric() && MaxStr.IsNumeric())
		{
			Def.bHasClamp = true;
			Def.ClampMin = FCString::Atod(*MinStr);
			Def.ClampMax = FCString::Atod(*MaxStr);
		}
#endif
	}

	// Resolves and validates the CrowdyOnRep notify, mirroring CrowdyState's ValidateOnRepSignatures exactly:
	// the named function must exist AND be parameterless (no params, no return). A missing or wrong-arity
	// notify is dropped (OnRepFunctionName left NAME_None) with an error the attribute still replicates; only
	// its notify is unbound. Shared, live-only (functions survive cooking, so this runs on the baked path too).
	void ResolveOnRep(const UClass* Class, const FProperty* Property, FCrowdyAttributeDef& Def)
	{
#if WITH_METADATA
		const FString OnRep = Property->GetMetaData(CrowdyStateMetaKeys::OnRep);
		if (OnRep.IsEmpty())
		{
			return;
		}
		const FName OnRepName(*OnRep);
		const UFunction* OnRepFn = Class ? Class->FindFunctionByName(OnRepName) : nullptr;
		if (!OnRepFn)
		{
			UE_LOG(LogCrowdyGameModel, Error,
				TEXT("[GameModel] property '%s' on '%s' names CrowdyOnRep '%s', which is not a valid notify (no such function). Dropping the notify; the attribute still replicates."),
				*Def.PropertyName.ToString(), Class ? *Class->GetPathName() : TEXT("?"), *OnRep);
			return;
		}
		if (OnRepFn->NumParms != 0)
		{
			UE_LOG(LogCrowdyGameModel, Error,
				TEXT("[GameModel] property '%s' on '%s' names CrowdyOnRep '%s', which is not a valid notify (must be parameterless; it takes %d parameter(s) or returns a value). Dropping the notify; the attribute still replicates."),
				*Def.PropertyName.ToString(), Class ? *Class->GetPathName() : TEXT("?"), *OnRep, OnRepFn->NumParms);
			return;
		}
		Def.OnRepFunctionName = OnRepName;
#endif
	}
}

bool FCrowdyAttributeRegistry::ClassHasModelAttributes(const UClass* Class)
{
	if (!Class)
	{
		return false;
	}
#if WITH_METADATA
	for (TFieldIterator<FProperty> It(Class); It; ++It)
	{
		const FProperty* Property = *It;
		if (!CrowdyGameModelMetaKeys::HasModelMeta(Property))
		{
			continue;
		}
		// Skip a dual-plane property (also CrowdyState) and an unsupported type it is not an accepted attribute.
		if (Property->HasMetaData(CrowdyStateMetaKeys::Replicate))
		{
			continue;
		}
		if (!MapPropertyToValueType(Property).IsEmpty())
		{
			return true;
		}
	}
	return false;
#else
	// Cooked builds: metadata is stripped, so the accepted-attribute set is answered from the bake.
	const TArray<FCrowdyBakedAttribute>* Attrs = UCrowdyBakedRegistry::FindModelAttributes(Class);
	return Attrs && Attrs->Num() > 0;
#endif
}

TArray<FCrowdyAttributeDef> FCrowdyAttributeRegistry::DiscoverForClass(const UClass* Class)
{
	TArray<FCrowdyAttributeDef> Defs;
#if WITH_METADATA
	if (!Class)
	{
		return Defs;
	}

	TMap<FString, FName> ServerKeyToProperty; // server key -> the property that first claimed it (duplicate guard)

	for (TFieldIterator<FProperty> It(Class); It; ++It)
	{
		FProperty* Property = *It;
		if (!CrowdyGameModelMetaKeys::HasModelMeta(Property))
		{
			continue;
		}

		// Plane-exclusivity (keeps the two planes from collapsing): a field lives in exactly one plane. A
		// property hand-marked both CrowdyModel and CrowdyState is a discovery-time reject the BP dropdown
		// makes this impossible, but defense-in-depth for C++ that hand-writes both.
		if (Property->HasMetaData(CrowdyStateMetaKeys::Replicate))
		{
			UE_LOG(LogCrowdyGameModel, Error,
				TEXT("[GameModel] property '%s' on '%s' is marked both CrowdyModel and CrowdyState; a field lives in exactly one plane. Dropping it from the Game Model attribute set (put authoritative state in exactly one plane)."),
				*Property->GetName(), *Class->GetPathName());
			continue;
		}

		const FString ValueType = MapPropertyToValueType(Property);
		if (ValueType.IsEmpty())
		{
			UE_LOG(LogCrowdyGameModel, Warning,
				TEXT("[GameModel] property '%s' on '%s' is marked CrowdyModel but its type is not a supported Server Owned type (a scalar, string, scalar array, model ref, or plain struct). Dropping it."),
				*Property->GetName(), *Class->GetPathName());
			continue;
		}

		FCrowdyAttributeDef Def;
		Def.PropertyName = Property->GetFName();
		Def.ValueType = ValueType;

		// Server key: the lowercased property name, or a lowercased/trimmed meta=(CrowdyKey=...) override that
		// pins a stable server key across a later C++/BP rename. An empty override is ignored (keeps the derived
		// key) with a warning, so a blank tag never silently yields an empty key.
		bool bBlankOverride = false;
		Def.Key = CrowdyGameModelMetaKeys::ResolvedServerKeyForProperty(Property, &bBlankOverride);
		if (bBlankOverride)
		{
			UE_LOG(LogCrowdyGameModel, Warning,
				TEXT("[GameModel] property '%s' on '%s' has an empty CrowdyKey override; using the derived key '%s'."),
				*Property->GetName(), *Class->GetPathName(), *Def.Key);
		}

		// Duplicate server key within the class: two attributes cannot share one server key (the later would
		// silently shadow the earlier on the server, and the schema sync would upsert one def twice). Drop the
		// duplicate with an error; the author gives one an explicit distinct CrowdyKey. Reachable because a
		// CrowdyKey override can force a collision between two otherwise-distinct property names.
		if (const FName* FirstOwner = ServerKeyToProperty.Find(Def.Key))
		{
			UE_LOG(LogCrowdyGameModel, Error,
				TEXT("[GameModel] property '%s' on '%s' resolves to server key '%s', already used by '%s'. Dropping the duplicate; give one an explicit meta=(CrowdyKey=...) with a distinct key."),
				*Property->GetName(), *Class->GetPathName(), *Def.Key, *FirstOwner->ToString());
			continue;
		}
		ServerKeyToProperty.Add(Def.Key, Def.PropertyName);

		// Read visibility (public|owner|hidden). Absent keeps the "public" default on the def; an unrecognized
		// value warns and keeps the default (an author typo must not silently ship a wrong read-access level).
		if (Property->HasMetaData(CrowdyGameModelMetaKeys::Visibility))
		{
			const FString Vis = Property->GetMetaData(CrowdyGameModelMetaKeys::Visibility).TrimStartAndEnd().ToLower();
			if (CrowdyGameModelMetaKeys::IsValidVisibility(Vis))
			{
				Def.Visibility = Vis;
			}
			else
			{
				UE_LOG(LogCrowdyGameModel, Warning,
					TEXT("[GameModel] property '%s' on '%s' has CrowdyVisibility='%s', which is not one of public|owner|hidden; defaulting to '%s'."),
					*Property->GetName(), *Class->GetPathName(), *Vis, CrowdyGameModelMetaKeys::DefaultVisibility);
			}
		}

		ReadNativeClamp(Property, Def);
		ResolveOnRep(Class, Property, Def);
		Defs.Add(MoveTemp(Def));
	}

	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Verbose,
		TEXT("[GameModel] DiscoverForClass %s -> %d attribute(s)."), *Class->GetName(), Defs.Num());
#else
	// Cooked builds: metadata is stripped, so the attribute defs are read back from the baked table
	// (the bake writes exactly what live discovery produced, validated by the round-trip test).
	if (!Class)
	{
		return Defs;
	}
	if (const TArray<FCrowdyBakedAttribute>* Attrs = UCrowdyBakedRegistry::FindModelAttributes(Class))
	{
		Defs.Reserve(Attrs->Num());
		for (const FCrowdyBakedAttribute& Attr : *Attrs)
		{
			FCrowdyAttributeDef Def;
			Def.PropertyName      = Attr.PropertyName;
			Def.Key               = Attr.Key;
			Def.ValueType         = Attr.ValueType;
			Def.bHasClamp         = Attr.bHasClamp;
			Def.ClampMin          = Attr.ClampMin;
			Def.ClampMax          = Attr.ClampMax;
			Def.OnRepFunctionName = Attr.OnRepFunctionName;
			Defs.Add(MoveTemp(Def));
		}
	}
#endif
	return Defs;
}
