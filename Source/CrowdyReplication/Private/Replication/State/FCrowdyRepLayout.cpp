#include "Replication/State/FCrowdyRepLayout.h"

#include "Replication/State/CrowdyStateMetaKeys.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"
#include "CrowdyReplicationLog.h"

#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/EnumProperty.h"

// The transitive-nested-container guard is shared with the RPC signature validator and lives on
// FCrowdyRPC::StructTransitivelyContainsContainer (both are in the CrowdyReplication module, and RPC
// needs the identical check to reject a container buried inside a struct parameter). The
// net-serialized exemption below is specific to CrowdyState, so it stays at the call site, not in the
// shared helper.

// Locks CrowdyState scope to POD leaves + USTRUCTs. FCrowdyRPC::IsSupportedParamType would also
// accept object references and containers (they ride the RPC codec), so this applies the stricter
// CrowdyState rules directly rather than reusing it. FCrowdyRPC::IsObjectProperty is file-static
// in CrowdyRPC.cpp and not callable here, so the object-ref check is spelled out inline.
ECrowdyStatePropertySupport FCrowdyStateLayoutBuilder::ClassifyStateProperty(const FProperty* Prop)
{
	if (!Prop)
	{
		return ECrowdyStatePropertySupport::Unsupported;
	}

	// A fixed-size C array (ArrayDim > 1) reflects as a single FProperty whose two-argument Identical
	// compares only element [0], while the codec serializes and the shadow copies all elements. A change
	// to a later element would diff as clean and silently never replicate, so reject it (out of scope for
	// this plane, which is single-value POD leaves + USTRUCTs).
	if (Prop->ArrayDim > 1)
	{
		return ECrowdyStatePropertySupport::StaticArray;
	}

	// Containers first: a positional per-property diff cannot express element-level churn, so they
	// are rejected with their own message across the entire plan.
	if (CastField<FArrayProperty>(Prop) || CastField<FSetProperty>(Prop) || CastField<FMapProperty>(Prop))
	{
		return ECrowdyStatePropertySupport::Container;
	}

	// Object/class/soft references, interfaces, and delegates carry no portable value on this
	// plane. FClassProperty derives from FObjectProperty and FSoftClassProperty from
	// FSoftObjectProperty, so the two base casts cover all four object kinds.
	if (CastField<FObjectProperty>(Prop) || CastField<FSoftObjectProperty>(Prop)
		|| CastField<FInterfaceProperty>(Prop) || CastField<FDelegateProperty>(Prop)
		|| CastField<FMulticastDelegateProperty>(Prop))
	{
		return ECrowdyStatePropertySupport::ObjectRef;
	}

	// A USTRUCT that is not net-serialized rides SerializeItem; if it transitively contains a container,
	// that container's untrusted element count drives an unbounded allocation on decode (a remote OOM the
	// FString/FName reader cap does not cover), so reject it. Net-serialized structs are exempt the
	// codec rides their own NetSerializeItem. This net-serialized test matches the codec's IsNetQuantizedProperty.
	if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		const bool bNetSerialized = StructProp->Struct
			&& (StructProp->Struct->StructFlags & STRUCT_NetSerializeNative) != 0;
		if (!bNetSerialized && FCrowdyRPC::StructTransitivelyContainsContainer(StructProp->Struct))
		{
			return ECrowdyStatePropertySupport::NestedContainer;
		}
		return ECrowdyStatePropertySupport::Supported;
	}

	// Accept only POD leaves (USTRUCTs are handled above). FByteProperty is itself an FNumericProperty;
	// it is listed for parity with the plan and to make the TEnumAsByte case explicit.
	const bool bAccepted =
		CastField<FNumericProperty>(Prop) != nullptr
		|| CastField<FBoolProperty>(Prop) != nullptr
		|| CastField<FByteProperty>(Prop) != nullptr
		|| CastField<FEnumProperty>(Prop) != nullptr
		|| CastField<FNameProperty>(Prop) != nullptr
		|| CastField<FStrProperty>(Prop) != nullptr;

	return bAccepted ? ECrowdyStatePropertySupport::Supported : ECrowdyStatePropertySupport::Unsupported;
}

bool FCrowdyStateLayoutBuilder::IsStateReplicatable(const FProperty* Prop)
{
	return ClassifyStateProperty(Prop) == ECrowdyStatePropertySupport::Supported;
}

// The fragments below are the exact mid-sentence text BuildLayout's per-reason log lines already used;
// factoring them here keeps the discovery log and any editor surface reporting the same reason in sync.
// The Unsupported fragment stops before the type token so the caller can append the canonical type in
// its original position, preserving the existing log wording verbatim.
FString FCrowdyStateLayoutBuilder::DescribeStateSupport(ECrowdyStatePropertySupport Support)
{
	switch (Support)
	{
	case ECrowdyStatePropertySupport::Container:
		return TEXT("is a container; CrowdyState does not replicate containers; use a CrowdyEvent RPC or a Game Model container");
	case ECrowdyStatePropertySupport::NestedContainer:
		return TEXT("is a USTRUCT that transitively contains a container (TArray/TSet/TMap); such nested containers are rejected because a forged element count would drive an unbounded allocation on decode");
	case ECrowdyStatePropertySupport::StaticArray:
		return TEXT("is a fixed-size array; CrowdyState replicates only single-value POD and USTRUCT properties (its positional diff would miss changes past element 0)");
	case ECrowdyStatePropertySupport::ObjectRef:
		return TEXT("is an object/interface/delegate reference; CrowdyState replicates only POD and USTRUCT values");
	case ECrowdyStatePropertySupport::Unsupported:
		return TEXT("has an unsupported type");
	case ECrowdyStatePropertySupport::Supported:
	default:
		return FString();
	}
}

int64 FCrowdyStateLayoutBuilder::ComputeLayoutHash(const TArray<FCrowdyRepProperty>& Properties)
{
	FString HashSource;
	for (const FCrowdyRepProperty& RepProp : Properties)
	{
		if (!RepProp.Property)
		{
			continue;
		}
		if (!HashSource.IsEmpty())
		{
			HashSource.AppendChar(TEXT(';'));
		}
		HashSource += RepProp.Property->GetName();
		HashSource.AppendChar(TEXT(':'));
		HashSource += FCrowdyRPC::CanonicalParamType(RepProp.Property);
	}
	return HashSource.IsEmpty() ? 0 : FCrowdyTypeIDGenerator::GenerateFromString(HashSource);
}

int32 FCrowdyRepLayout::IndexOfPropertyID(int64 InPropertyID) const
{
	for (int32 Index = 0; Index < Properties.Num(); ++Index)
	{
		if (Properties[Index].PropertyID == InPropertyID)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

bool FCrowdyStateLayoutBuilder::BuildLayout(const UClass* Class, FCrowdyRepLayout& Out)
{
	Out = FCrowdyRepLayout{};
	if (!Class)
	{
		return false;
	}

	Out.OwnerClass = Class;

	// Declaration order, including super-class properties (TFieldIterator's default IncludeSuper), so a
	// subclass layout is a stable superset of its parent's.
	for (TFieldIterator<FProperty> It(Class); It; ++It)
	{
		const FProperty* Prop = *It;
		if (!CrowdyStateMetaKeys::HasStateMeta(Prop))
		{
			continue;
		}

		const ECrowdyStatePropertySupport Support = FCrowdyStateLayoutBuilder::ClassifyStateProperty(Prop);
		if (Support != ECrowdyStatePropertySupport::Supported)
		{
			// Compose the log line from the shared reason fragment so discovery, the Blueprint compiler
			// extension, and the variable-details dropdown cannot drift. Only the Unsupported case needs the
			// canonical type, appended in its original position to keep the message byte-identical.
			FString Reason = FCrowdyStateLayoutBuilder::DescribeStateSupport(Support);
			if (Support == ECrowdyStatePropertySupport::Unsupported)
			{
				Reason += FString::Printf(TEXT(" '%s'; CrowdyState replicates only POD and USTRUCT values"),
					*FCrowdyRPC::CanonicalParamType(Prop));
			}
			UE_LOG(LogCrowdyReplication, Error,
				TEXT("CrowdyState: property '%s' on '%s' %s. Omitting it."),
				*Prop->GetName(), *Class->GetPathName(), *Reason);
			continue;
		}

		const FString CanonicalType = FCrowdyRPC::CanonicalParamType(Prop);

		FCrowdyRepProperty RepProp;
		RepProp.Property = Prop;
		RepProp.PropertyID = FCrowdyTypeIDGenerator::GenerateFromString(
			Class->GetPathName() + TEXT("::") + Prop->GetName() + TEXT(":") + CanonicalType);

#if WITH_METADATA
		RepProp.bOwnerOnly = Prop->HasMetaData(CrowdyStateMetaKeys::OwnerOnly);
		RepProp.bManualDirty = Prop->HasMetaData(CrowdyStateMetaKeys::ManualDirty);
		RepProp.bHeartbeat = Prop->HasMetaData(CrowdyStateMetaKeys::Heartbeat);
		const FString& OnRepValue = Prop->GetMetaData(CrowdyStateMetaKeys::OnRep);
		RepProp.OnRepFunctionName = OnRepValue.IsEmpty() ? NAME_None : FName(*OnRepValue);
#endif

		Out.Properties.Add(MoveTemp(RepProp));
	}

	// Fold the accepted properties' ordered "name:type" tokens through the single hash formula. The
	// helper walks Out.Properties in the same declaration order they were added and reads the same
	// FProperty*, so this is byte-identical to an inline accumulation during the loop.
	Out.LayoutHash = ComputeLayoutHash(Out.Properties);

	return Out.IsValid();
}
