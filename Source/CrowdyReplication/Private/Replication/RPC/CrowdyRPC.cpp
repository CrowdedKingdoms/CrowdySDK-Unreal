#include "Replication/RPC/CrowdyRPC.h"

#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "Utils/CrowdyBakedRegistry.h"
#include "Utils/UCrowdyClassRegistry.h"
#include "Components/ActorComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Class.h"          // StaticEnum, UEnum
#include "UObject/UnrealType.h"
#include "UObject/EnumProperty.h"
#include "UObject/TextProperty.h"      // FTextProperty (not pulled in by UnrealType.h)
#include "UObject/UObjectBaseUtility.h" // GetNameSafe
#include "UObject/Stack.h"             // FOutParmRec (Blueprint frame out-parm records)
#include "UObject/SoftObjectPath.h"    // FSoftObjectPath / FSoftClassPath
#include "UObject/SoftObjectPtr.h"     // FSoftObjectPtr
#include "UObject/UObjectGlobals.h"    // FindObject / LoadObject
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/StructuredArchive.h"
#include "Serialization/StructuredArchiveAdapters.h"
#include "HAL/CriticalSection.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeLock.h"

DEFINE_LOG_CATEGORY(LogCrowdyRPC);

namespace
{
	TAutoConsoleVariable<int32> CVarCrowdyRpcTrace(
		TEXT("crowdy.rpc.trace"), 0,
		TEXT("When non-zero, logs every CrowdyEvent RPC send and receive: function, entity, ")
		TEXT("addressing, owned/delegated classification, and parameter byte size."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarCrowdyRpcLoopback(
		TEXT("crowdy.rpc.loopback"), 0,
		TEXT("When non-zero, a sent replicated CrowdyEvent is also delivered to this client's own ")
		TEXT("receive path, so the whole serialize/resolve/dispatch round-trip can be tested without ")
		TEXT("a second client. The call runs once locally through that path; a call made from the ")
		TEXT("replayed body is sent normally but does not loop back, so there is no cascade."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarCrowdyRpcReliableTrace(
		TEXT("crowdy.rpc.reliable.trace"), 0,
		TEXT("When non-zero, logs just the reliable (channel-transport) RPC path: a Multicast ")
		TEXT("CrowdyEvent's channel send and the matching receive on every member. Narrower than ")
		TEXT("crowdy.rpc.trace, which covers all RPC sends and receives; either one surfaces the ")
		TEXT("reliable lines."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarCrowdyRpcAllowObjectLoad(
		TEXT("crowdy.rpc.allowObjectLoad"), 0,
		TEXT("When non-zero, a received object or class reference whose asset is not already resident ")
		TEXT("is loaded from disk by path. Default 0 (find-only) so an untrusted peer cannot trigger ")
		TEXT("arbitrary asset loads; an unresolved reference is delivered as null."),
		ECVF_Default);

	// Maps a routing meta string (the enumerator name the macro stringized, e.g.
	// "OwningClient") to its enum value. GetValueByNameString resolves both the
	// short and the namespaced spelling; anything unrecognized keeps the default.
	template <typename TEnum>
	TEnum ResolveMetaEnum(const FString& MetaValue, TEnum DefaultValue)
	{
		if (MetaValue.IsEmpty())
		{
			return DefaultValue;
		}
		const UEnum* EnumType = StaticEnum<TEnum>();
		const int64 Value = EnumType ? EnumType->GetValueByNameString(MetaValue) : INDEX_NONE;
		return Value == INDEX_NONE ? DefaultValue : static_cast<TEnum>(Value);
	}

	ECrowdyEventRecipient ResolveCrowdyRecipientMetaEnum(
		const FString& MetaValue, ECrowdyEventRecipient DefaultValue)
	{
		if (MetaValue.Equals(TEXT("OwningPlayer"), ESearchCase::IgnoreCase)
			|| MetaValue.Equals(TEXT("Owning Player"), ESearchCase::IgnoreCase))
		{
			return ECrowdyEventRecipient::OwningClient;
		}

		return ResolveMetaEnum(MetaValue, DefaultValue);
	}

	// Constructs only the parameter slots of a frame. A C++ UFUNCTION has nothing but
	// parameters, but a Blueprint UFUNCTION also has reflected local variables laid out
	// beyond ParmsSize — UFunction::InitializeStruct would touch those and overrun a frame
	// sized to ParmsSize, so the parameter properties (which alone fit the frame) are
	// constructed directly. CPF_Parm covers the input, out, and return slots; locals do not
	// carry it and are skipped.
	void InitializeParamProperties(const UFunction* Fn, void* Frame)
	{
		for (TFieldIterator<FProperty> It(Fn); It; ++It)
		{
			FProperty* Prop = *It;
			if (Prop->HasAnyPropertyFlags(CPF_Parm))
			{
				Prop->InitializeValue_InContainer(Frame);
			}
		}
	}

	// Mirror of InitializeParamProperties for teardown.
	void DestroyParamProperties(const UFunction* Fn, void* Frame)
	{
		for (TFieldIterator<FProperty> It(Fn); It; ++It)
		{
			FProperty* Prop = *It;
			if (Prop->HasAnyPropertyFlags(CPF_Parm))
			{
				Prop->DestroyValue_InContainer(Frame);
			}
		}
	}

	// True when every parameter slot the frame init/destroy touches is trivially
	// copyable, so SendChecked/ApplyCall can skip InitializeStruct/DestroyStruct.
	bool ComputeParamsPOD(const UFunction* Fn)
	{
		for (TFieldIterator<FProperty> It(Fn); It; ++It)
		{
			const FProperty* Prop = *It;
			if (!Prop->HasAnyPropertyFlags(CPF_Parm))
			{
				continue;
			}
			if (!Prop->HasAnyPropertyFlags(CPF_IsPlainOldData))
			{
				return false;
			}
		}
		return true;
	}

	// Resolves where to read a parameter's value when serializing. A by-ref/out parameter
	// which includes a const-ref container that is not stored inline in a Blueprint event's locals
	// block; the VM keeps it in the out-parm records, pointing at the caller's storage. When such
	// a record exists for this property, read from there; otherwise (a plain input, or the C++
	// send path where OutParms is null) read from the locals offset.
	void* ResolveParamReadAddr(const FProperty* Prop, void* Frame, FOutParmRec* OutParms)
	{
		if (Prop->HasAnyPropertyFlags(CPF_OutParm))
		{
			for (FOutParmRec* Rec = OutParms; Rec; Rec = Rec->NextOutParm)
			{
				if (Rec->Property == Prop)
				{
					return Rec->PropAddr;
				}
			}
		}
		return Prop->ContainerPtrToValuePtr<void>(Frame);
	}

	// A CrowdyEvent may be declared on an actor or on one of its components; both
	// resolve to the actor that carries the entity identity used for routing.
	AActor* ResolveContextActor(UObject* Obj)
	{
		if (AActor* AsActor = Cast<AActor>(Obj))
		{
			return AsActor;
		}
		if (const UActorComponent* AsComponent = Cast<UActorComponent>(Obj))
		{
			return AsComponent->GetOwner();
		}
		return nullptr;
	}

	// Set by FCrowdyRPC::FScopedEntityContext for the duration of an encode or decode so the object
	// codec can map a tracked entity actor to and from its network GUID. Null outside a scope (and
	// in the unit tests), where only path-addressed objects (assets, classes) round-trip.
	// Game-thread only, mirroring the other scoped serialization state on FCrowdyRPC.
	UCrowdyEntitySubsystem* GActiveEntities = nullptr;

	UCrowdyEntitySubsystem* ResolveEntitySubsystemFromContext(const UObject* ContextObject)
	{
		const UWorld* World = ContextObject ? ContextObject->GetWorld() : nullptr;
		return World ? World->GetSubsystem<UCrowdyEntitySubsystem>() : nullptr;
	}

	bool IsObjectLoadAllowed()
	{
		return CVarCrowdyRpcAllowObjectLoad.GetValueOnGameThread() != 0;
	}

	// True for an object, class, or soft reference, which the codec carries by identity rather than
	// by pointer. FClassProperty derives from FObjectProperty and FSoftClassProperty from
	// FSoftObjectProperty, so the two base casts cover all four kinds.
	bool IsObjectProperty(const FProperty* Prop)
	{
		return CastField<FObjectProperty>(Prop) != nullptr
			|| CastField<FSoftObjectProperty>(Prop) != nullptr;
	}

	// True for a TArray whose element is an object-like reference (TArray<AActor*>, TArray<UClass*>,
	// …). Such an array cannot ride FProperty::SerializeItem the inner object SerializeItem drops
	// the pointer just like a scalar object so it is walked element-by-element through the object
	// codec instead. Sets and maps of objects are out of scope (object key/value hashing), so only
	// FArrayProperty qualifies here.
	bool IsObjectArray(const FProperty* Prop)
	{
		const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
		return ArrayProp != nullptr && IsObjectProperty(ArrayProp->Inner);
	}

	// True for a TSet or TMap that carries an object reference as its element, key, or value. These
	// stay rejected at registration (the validator names them specifically); only TArray of objects
	// is supported, so this is used purely to produce a clearer message than the generic one.
	bool IsObjectSetOrMap(const FProperty* Prop)
	{
		if (const FSetProperty* SetProp = CastField<FSetProperty>(Prop))
		{
			return IsObjectProperty(SetProp->ElementProp);
		}
		if (const FMapProperty* MapProp = CastField<FMapProperty>(Prop))
		{
			return IsObjectProperty(MapProp->KeyProp) || IsObjectProperty(MapProp->ValueProp);
		}
		return false;
	}

	// Upper bound on the element count the array decoder will accept from an untrusted peer. A
	// forged or corrupt count cannot be allowed to drive an unbounded allocation, so a count past
	// this (or negative) sets the archive error and the whole call is dropped. Far above any count
	// that fits the transport budgets, so it never rejects a legitimate array.
	constexpr int32 CrowdyRpcMaxArrayElements = 65536;

	// Finds an object or class by path. Find-only by default so untrusted input cannot trigger a
	// disk load; crowdy.rpc.allowObjectLoad opts into loading an asset that is not yet resident.
	UObject* ResolveObjectByPath(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}
		const FSoftObjectPath SoftPath(Path);
		if (UObject* Found = SoftPath.ResolveObject())
		{
			return Found;
		}
		return IsObjectLoadAllowed() ? SoftPath.TryLoad() : nullptr;
	}

	void WriteObjectRefTag(FArchive& Ar, ECrowdyObjectRefTag Tag)
	{
		uint8 Raw = static_cast<uint8>(Tag);
		Ar << Raw;
	}

	// Writes one object-like value as a tag byte plus its identity: an entity GUID for a tracked
	// entity actor, a path for an asset or class, or just the Null tag for null and for a runtime
	// object that has no portable identity.
	void EncodeObjectValue(const FProperty* Prop, const void* ValuePtr, FArchive& Ar)
	{
		// Soft references already hold a path, so carry it directly with no load. The class-typed
		// variant derives from the object-typed one, so this one branch covers both.
		if (CastField<FSoftObjectProperty>(Prop) != nullptr)
		{
			const FSoftObjectPath& SoftPath = static_cast<const FSoftObjectPtr*>(ValuePtr)->GetUniqueID();
			if (SoftPath.IsNull())
			{
				WriteObjectRefTag(Ar, ECrowdyObjectRefTag::Null);
				return;
			}
			WriteObjectRefTag(Ar, ECrowdyObjectRefTag::Path);
			FString PathString = SoftPath.ToString();
			Ar << PathString;
			return;
		}

		const FObjectProperty* ObjectProp = CastField<FObjectProperty>(Prop);
		UObject* Object = ObjectProp ? ObjectProp->GetObjectPropertyValue(ValuePtr) : nullptr;
		if (!Object)
		{
			WriteObjectRefTag(Ar, ECrowdyObjectRefTag::Null);
			return;
		}

		// A class reference (UClass* / TSubclassOf) is addressed by its class path.
		if (CastField<FClassProperty>(Prop) != nullptr)
		{
			WriteObjectRefTag(Ar, ECrowdyObjectRefTag::Path);
			FString PathString = Object->GetPathName();
			Ar << PathString;
			return;
		}

		// A tracked entity actor is addressed by its network GUID so it resolves to the matching
		// instance on the receiver regardless of how the call was routed.
		if (AActor* Actor = Cast<AActor>(Object))
		{
			const FGuid EntityID = GActiveEntities ? GActiveEntities->FindEntityID(Actor) : FGuid();
			if (EntityID.IsValid())
			{
				WriteObjectRefTag(Ar, ECrowdyObjectRefTag::Entity);
				FGuid Mutable = EntityID;
				Ar << Mutable;
				return;
			}
		}

		// An asset is addressed by its object path.
		if (Object->IsAsset())
		{
			WriteObjectRefTag(Ar, ECrowdyObjectRefTag::Path);
			FString PathString = Object->GetPathName();
			Ar << PathString;
			return;
		}

		// A runtime, non-entity, non-asset object has no portable identity; send null and warn.
		UE_LOG(LogCrowdyRPC, Warning,
			TEXT("EncodeObjectValue: parameter '%s' references '%s', which is neither a tracked entity nor an asset; sent as null."),
			*Prop->GetName(), *Object->GetName());
		WriteObjectRefTag(Ar, ECrowdyObjectRefTag::Null);
	}

	// Reverses EncodeObjectValue, assigning the resolved reference into the parameter slot. An
	// unresolved entity or a not-resident asset yields null, so the receiver body must null-check.
	void DecodeObjectValue(FProperty* Prop, void* ValuePtr, FArchive& Ar)
	{
		uint8 RawTag = 0;
		Ar << RawTag;
		const ECrowdyObjectRefTag Tag = static_cast<ECrowdyObjectRefTag>(RawTag);

		FGuid EntityID;
		FString PathString;
		if (Tag == ECrowdyObjectRefTag::Entity)
		{
			Ar << EntityID;
		}
		else if (Tag == ECrowdyObjectRefTag::Path)
		{
			Ar << PathString;
		}

		// Soft references store a path with no load. An entity tag resolves to the actor's path.
		if (CastField<FSoftObjectProperty>(Prop) != nullptr)
		{
			FSoftObjectPath SoftPath;
			if (Tag == ECrowdyObjectRefTag::Path)
			{
				SoftPath = FSoftObjectPath(PathString);
			}
			else if (Tag == ECrowdyObjectRefTag::Entity)
			{
				if (AActor* Actor = GActiveEntities ? GActiveEntities->FindEntity(EntityID) : nullptr)
				{
					SoftPath = FSoftObjectPath(Actor);
				}
			}
			*static_cast<FSoftObjectPtr*>(ValuePtr) = FSoftObjectPtr(SoftPath);
			return;
		}

		FObjectProperty* ObjectProp = CastField<FObjectProperty>(Prop);
		if (!ObjectProp)
		{
			return;
		}

		UObject* Resolved = nullptr;
		if (Tag == ECrowdyObjectRefTag::Entity)
		{
			Resolved = GActiveEntities ? GActiveEntities->FindEntity(EntityID) : nullptr;
			if (!Resolved)
			{
				UE_LOG(LogCrowdyRPC, Warning,
					TEXT("DecodeObjectValue: entity for parameter '%s' is not present locally; assigning null."),
					*Prop->GetName());
			}
		}
		else if (Tag == ECrowdyObjectRefTag::Path)
		{
			Resolved = ResolveObjectByPath(PathString);
			if (!Resolved)
			{
				UE_LOG(LogCrowdyRPC, Warning,
					TEXT("DecodeObjectValue: '%s' for parameter '%s' is not resident; assigning null (set crowdy.rpc.allowObjectLoad to load it)."),
					*PathString, *Prop->GetName());
			}
		}

		// A class reference must be a UClass descending from the property's meta class; any other
		// mismatch assigns null rather than a wrongly typed pointer.
		if (FClassProperty* ClassProp = CastField<FClassProperty>(Prop))
		{
			UClass* ResolvedClass = Cast<UClass>(Resolved);
			if (ResolvedClass && ClassProp->MetaClass && !ResolvedClass->IsChildOf(ClassProp->MetaClass))
			{
				ResolvedClass = nullptr;
			}
			ClassProp->SetObjectPropertyValue(ValuePtr, ResolvedClass);
			return;
		}

		if (Resolved && !Resolved->IsA(ObjectProp->PropertyClass))
		{
			Resolved = nullptr;
		}
		ObjectProp->SetObjectPropertyValue(ValuePtr, Resolved);
	}

	// Writes a TArray of object references as an explicit element count followed by each element
	// through the single-object codec, so identity (entity GUID / asset or class path) survives the
	// archive that drops raw pointers. Positions are stable: an unresolvable element decodes to null
	// in place rather than collapsing the array.
	void EncodeObjectArray(const FArrayProperty* ArrayProp, const void* ValuePtr, FArchive& Ar)
	{
		FScriptArrayHelper Helper(ArrayProp, ValuePtr);
		int32 Num = Helper.Num();
		Ar << Num;
		for (int32 Index = 0; Index < Num; ++Index)
		{
			EncodeObjectValue(ArrayProp->Inner, Helper.GetRawPtr(Index), Ar);
		}
	}

	// Reverses EncodeObjectArray. The count is untrusted, so a negative or oversized value sets the
	// archive error and decodes nothing  DeserializeParams then drops the whole call. The bound also
	// guards FScriptArrayHelper::EmptyAndAddValues, which asserts on a negative count.
	void DecodeObjectArray(FArrayProperty* ArrayProp, void* ValuePtr, FArchive& Ar)
	{
		int32 Num = 0;
		Ar << Num;
		if (Num < 0 || Num > CrowdyRpcMaxArrayElements)
		{
			UE_LOG(LogCrowdyRPC, Warning,
				TEXT("DecodeObjectArray: element count %d is out of range (0..%d); dropping call."),
				Num, CrowdyRpcMaxArrayElements);
			Ar.SetError();
			return;
		}

		FScriptArrayHelper Helper(ArrayProp, ValuePtr);
		Helper.EmptyAndAddValues(Num);
		for (int32 Index = 0; Index < Num; ++Index)
		{
			DecodeObjectValue(ArrayProp->Inner, Helper.GetRawPtr(Index), Ar);
		}
	}
}

UFunction* FCrowdyRPC::ResolveFunction(UClass* Class, const TCHAR* ImplName)
{
	UFunction* Fn = Class ? Class->FindFunctionByName(FName(ImplName)) : nullptr;
	if (!Fn)
	{
		UE_LOG(LogCrowdyRPC, Error, TEXT("ResolveFunction: '%s' not found on class '%s'."),
			ImplName ? ImplName : TEXT("<null>"), *GetNameSafe(Class));
	}
	return Fn;
}

FString FCrowdyRPC::CanonicalParamType(const FProperty* Prop)
{
	// Structs and enums hash by path so an identically named type in another module
	// never aliases; plain numeric/string/bool properties hash by their field class
	// name (e.g. "IntProperty", "StrProperty").
	if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		return StructProp->Struct ? StructProp->Struct->GetPathName() : TEXT("struct");
	}
	if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
	{
		return EnumProp->GetEnum() ? EnumProp->GetEnum()->GetPathName() : TEXT("enum");
	}
	if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
	{
		return ByteProp->Enum ? ByteProp->Enum->GetPathName() : TEXT("u8");
	}

	// Containers hash by their kind plus their inner type(s) so TArray<int32> and
	// TArray<FVector> never collapse to the same id. Otherwise refactoring a parameter's
	// element type would not change the FunctionID and a drifted build would mis-deserialize.
	if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
	{
		return FString::Printf(TEXT("TArray<%s>"), *CanonicalParamType(ArrayProp->Inner));
	}
	if (const FSetProperty* SetProp = CastField<FSetProperty>(Prop))
	{
		return FString::Printf(TEXT("TSet<%s>"), *CanonicalParamType(SetProp->ElementProp));
	}
	if (const FMapProperty* MapProp = CastField<FMapProperty>(Prop))
	{
		return FString::Printf(TEXT("TMap<%s,%s>"),
			*CanonicalParamType(MapProp->KeyProp), *CanonicalParamType(MapProp->ValueProp));
	}

	// Object and class references hash by the referenced class so Foo(AActor*) and Foo(APawn*)
	// stay distinct. Check the class-typed variants first since they derive from the object ones.
	if (const FClassProperty* ClassProp = CastField<FClassProperty>(Prop))
	{
		return FString::Printf(TEXT("TSubclassOf<%s>"),
			ClassProp->MetaClass ? *ClassProp->MetaClass->GetPathName() : TEXT("Object"));
	}
	if (const FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Prop))
	{
		return FString::Printf(TEXT("TSoftClassPtr<%s>"),
			SoftClassProp->MetaClass ? *SoftClassProp->MetaClass->GetPathName() : TEXT("Object"));
	}
	if (const FSoftObjectProperty* SoftObjectProp = CastField<FSoftObjectProperty>(Prop))
	{
		return FString::Printf(TEXT("TSoftObjectPtr<%s>"),
			SoftObjectProp->PropertyClass ? *SoftObjectProp->PropertyClass->GetPathName() : TEXT("Object"));
	}
	if (const FObjectProperty* ObjectProp = CastField<FObjectProperty>(Prop))
	{
		return FString::Printf(TEXT("Object<%s>"),
			ObjectProp->PropertyClass ? *ObjectProp->PropertyClass->GetPathName() : TEXT("Object"));
	}

	return Prop->GetClass()->GetName();
}

bool FCrowdyRPC::IsSupportedParamType(const FProperty* Prop)
{
	if (!Prop)
	{
		return false;
	}

	// Containers ride the same SerializeItem path as long as every inner type does, so a
	// container is supported iff its element (and, for a map, key + value) types are. This
	// recurses, but Unreal reflection forbids nested containers, so in practice the inner is
	// always a leaf. A TArray may also carry object references  including TArray<UObject*> 
	// because the object-array codec walks each element through the identity codec. Sets and
	// maps of objects stay rejected (object key/value hashing is out of scope), so they keep the
	// object-inner guard.
	if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
	{
		return IsSupportedParamType(ArrayProp->Inner);
	}
	if (const FSetProperty* SetProp = CastField<FSetProperty>(Prop))
	{
		return !IsObjectProperty(SetProp->ElementProp) && IsSupportedParamType(SetProp->ElementProp);
	}
	if (const FMapProperty* MapProp = CastField<FMapProperty>(Prop))
	{
		return !IsObjectProperty(MapProp->KeyProp) && !IsObjectProperty(MapProp->ValueProp)
			&& IsSupportedParamType(MapProp->KeyProp) && IsSupportedParamType(MapProp->ValueProp);
	}

	// FNumericProperty covers byte/int/int64/float/double; enums and bools are their own
	// property classes. Object and class references ride the object codec by identity, so the
	// only unsupported parameter kinds left are interfaces and delegates, which have no stable
	// wire form and are rejected so they never reach the transport mis-encoded.
	return IsObjectProperty(Prop)
		|| CastField<FStructProperty>(Prop) != nullptr
		|| CastField<FNumericProperty>(Prop) != nullptr
		|| CastField<FEnumProperty>(Prop) != nullptr
		|| CastField<FBoolProperty>(Prop) != nullptr
		|| CastField<FNameProperty>(Prop) != nullptr
		|| CastField<FStrProperty>(Prop) != nullptr
		|| CastField<FTextProperty>(Prop) != nullptr;
}

FString FCrowdyRPC::DescribeSignatureProblem(const UFunction* Fn)
{
	if (!Fn)
	{
		return TEXT("function is null");
	}

	for (TFieldIterator<FProperty> It(Fn); It; ++It)
	{
		const FProperty* Prop = *It;
		if (!Prop->HasAnyPropertyFlags(CPF_Parm))
		{
			continue;
		}

		// A return value or a non-const output reference would have to travel back to
		// the caller, which a fire-and-forget network call cannot do  reject both so
		// the author does not expect a result that never arrives.
		if (Prop->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			return TEXT("returns a value  CrowdyEvents are one-way and cannot return data");
		}
		// A const reference is tagged CPF_OutParm by reflection but cannot return data, so it is
		// a legitimate input; only a non-const out param is a true output the call cannot honour.
		if (Prop->HasAnyPropertyFlags(CPF_OutParm) && !Prop->HasAnyPropertyFlags(CPF_ConstParm))
		{
			return FString::Printf(
				TEXT("parameter '%s' is an output (non-const reference)  CrowdyEvents cannot pass data back to the caller"),
				*Prop->GetName());
		}
		if (!IsSupportedParamType(Prop))
		{
			// A set or map whose key or value is an object reference is a deliberate limitation
			// (object identity hashing is out of scope), so name it specifically  a TArray of the
			// same references is supported, only the set/map form is not.
			if (IsObjectSetOrMap(Prop))
			{
				return FString::Printf(
					TEXT("parameter '%s' is a set or map of object references, which CrowdyEvents do not ")
					TEXT("support  use a TArray of object references instead"),
					*Prop->GetName());
			}
			return FString::Printf(
				TEXT("parameter '%s' has unsupported type '%s'  CrowdyEvents carry primitives, enums, ")
				TEXT("structs, object and class references, and arrays of any of these (sets and maps of ")
				TEXT("objects, interfaces, and delegates are not supported)"),
				*Prop->GetName(), *Prop->GetCPPType());
		}
	}

	return FString();
}

int32 FCrowdyRPC::EstimateMinChannelPayloadBytes(const UFunction* Fn)
{
	// Channel header (version + flags) + FCrowdyRpcCall fixed fields (ClassID 8, EntityID 16,
	// SenderID 16, FunctionID 8) + the ParamBlob's length prefix (4) and version byte (1).
	int32 MinBytes = 2 + 8 + 16 + 16 + 8 + 4 + 1;

	if (!Fn)
	{
		return MinBytes;
	}

	for (TFieldIterator<FProperty> It(Fn); It; ++It)
	{
		const FProperty* Prop = *It;
		if (!Prop->HasAnyPropertyFlags(CPF_Parm)) continue;
		if (Prop->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm)) continue;

		// Only fixed-width parameters add a guaranteed minimum. Strings, names, text and structs
		// can serialize to nearly nothing, so they cannot raise a safe lower bound.
		if (CastField<FNumericProperty>(Prop) || CastField<FBoolProperty>(Prop) || CastField<FEnumProperty>(Prop))
		{
			MinBytes += Prop->GetSize();
		}
	}

	return MinBytes;
}

int64 FCrowdyRPC::ComputeFunctionID(const UFunction* Fn)
{
	check(Fn);

	// Hash the declaring class (not the calling instance's class) so sender and
	// receiver derive the same id regardless of which subclass issues the call.
	const UClass* OwnerClass = Fn->GetOwnerClass();

	FString Signature;
	Signature.Reserve(128);
	Signature += OwnerClass ? OwnerClass->GetPathName() : FString();
	Signature += TEXT("::");
	Signature += Fn->GetName();
	Signature += TEXT("(");

	// Every parameter (including return/out) participates so any signature change
	// yields a new id and a drifted receiver rejects the call.
	bool bFirst = true;
	for (TFieldIterator<FProperty> It(Fn); It; ++It)
	{
		const FProperty* Prop = *It;
		if (!Prop->HasAnyPropertyFlags(CPF_Parm))
		{
			continue;
		}
		if (!bFirst)
		{
			Signature += TEXT(",");
		}
		Signature += CanonicalParamType(Prop);
		bFirst = false;
	}
	Signature += TEXT(")");

	return FCrowdyTypeIDGenerator::GenerateFromString(Signature);
}

FCrowdyFnInfo FCrowdyRPC::BuildFnInfo(UFunction* Fn)
{
	FCrowdyFnInfo Info;
	if (!Fn)
	{
		return Info;
	}

	// FunctionID and the POD flag are pure reflection over the signature and exist
	// in every build, so they are always computed live  the inbound resolver key
	// and the wire id never depend on the baked asset being loaded.
	Info.FunctionID = ComputeFunctionID(Fn);
	Info.bParamsPOD = ComputeParamsPOD(Fn);

	// Routing lives in meta=(...) keys, which are stripped from cooked builds. Read
	// them live where metadata exists; otherwise from the baked snapshot. Either
	// source leaves the API defaults in place for a function with no routing meta.
#if WITH_METADATA
	Info.Recipient = ResolveCrowdyRecipientMetaEnum(Fn->GetMetaData(CrowdyRpcMetaKeys::Recipient), Info.Recipient);
	Info.DecayRate = ResolveMetaEnum(Fn->GetMetaData(CrowdyRpcMetaKeys::Decay), Info.DecayRate);
	Info.Distance  = ResolveMetaEnum(Fn->GetMetaData(CrowdyRpcMetaKeys::Distance), Info.Distance);
	Info.ChannelName = Fn->GetMetaData(CrowdyRpcMetaKeys::Channel);
#else
	if (const FCrowdyBakedRpcFunction* Baked = UCrowdyBakedRegistry::FindRpcFunction(Fn))
	{
		Info.Recipient = Baked->Recipient;
		Info.DecayRate = Baked->DecayRate;
		Info.Distance  = Baked->Distance;
		Info.ChannelName = Baked->ChannelName;
	}
#endif

	return Info;
}

FCrowdyFnInfo FCrowdyRPC::GetFnInfo(UFunction* Fn)
{
#if WITH_EDITOR
	// Live Coding can recycle UFunction addresses, so a UFunction-keyed cache would
	// return stale info. Rebuild every call in editor; it is only a string hash.
	return BuildFnInfo(Fn);
#else
	static FCriticalSection CacheLock;
	static TMap<UFunction*, FCrowdyFnInfo> Cache;

	FScopeLock Lock(&CacheLock);
	if (const FCrowdyFnInfo* Found = Cache.Find(Fn))
	{
		return *Found;
	}
	const FCrowdyFnInfo Built = BuildFnInfo(Fn);
	Cache.Add(Fn, Built);
	return Built;
#endif
}

void FCrowdyRPC::SerializeParams(const UFunction* Fn, const void* Frame, TArray<uint8>& OutBlob,
	FOutParmRec* OutParms)
{
	OutBlob.Reset();
	if (!Fn || !Frame)
	{
		return;
	}

	// Persistent FMemoryWriter  the same archive configuration the event-payload
	// path (SerializeEventState) uses  so a struct serialized here is byte-identical
	// to the same struct serialized as an event payload.
	FMemoryWriter Writer(OutBlob, /*bIsPersistent=*/true);

	uint8 Version = CrowdyRpcParamBlobVersion;
	Writer << Version;

	void* MutableFrame = const_cast<void*>(Frame);
	for (TFieldIterator<FProperty> It(Fn); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop->HasAnyPropertyFlags(CPF_Parm))
		{
			continue;
		}
		if (IsTrueOutputParam(Prop))
		{
			continue;
		}

		void* ValuePtr = ResolveParamReadAddr(Prop, MutableFrame, OutParms);
		if (IsObjectProperty(Prop))
		{
			EncodeObjectValue(Prop, ValuePtr, Writer);
		}
		else if (IsObjectArray(Prop))
		{
			EncodeObjectArray(CastFieldChecked<FArrayProperty>(Prop), ValuePtr, Writer);
		}
		else
		{
			FStructuredArchiveFromArchive Adapter(Writer);
			Prop->SerializeItem(Adapter.GetSlot(), ValuePtr, nullptr);
		}
	}
}

bool FCrowdyRPC::DeserializeParams(const UFunction* Fn, const TArray<uint8>& Blob, void* Frame)
{
	if (!Fn || !Frame)
	{
		return false;
	}

	if (Blob.Num() < 1)
	{
		UE_LOG(LogCrowdyRPC, Warning, TEXT("DeserializeParams: empty blob; dropping call."));
		return false;
	}

	FMemoryReader Reader(Blob, /*bIsPersistent=*/true);

	uint8 Version = 0;
	Reader << Version;
	if (Version != CrowdyRpcParamBlobVersion)
	{
		// The blob is untrusted network input, so the explicit drop (return false) is
		// the shipping-safe guard; we log rather than ensure so a stale or malformed
		// peer cannot spam assertions.
		UE_LOG(LogCrowdyRPC, Warning,
			TEXT("DeserializeParams: blob version %u != expected %u; dropping call."),
			Version, CrowdyRpcParamBlobVersion);
		return false;
	}

	for (TFieldIterator<FProperty> It(Fn); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop->HasAnyPropertyFlags(CPF_Parm))
		{
			continue;
		}
		if (IsTrueOutputParam(Prop))
		{
			continue;
		}

		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Frame);
		if (IsObjectProperty(Prop))
		{
			DecodeObjectValue(Prop, ValuePtr, Reader);
		}
		else if (IsObjectArray(Prop))
		{
			DecodeObjectArray(CastFieldChecked<FArrayProperty>(Prop), ValuePtr, Reader);
		}
		else
		{
			FStructuredArchiveFromArchive Adapter(Reader);
			Prop->SerializeItem(Adapter.GetSlot(), ValuePtr, nullptr);
		}

		if (Reader.IsError())
		{
			UE_LOG(LogCrowdyRPC, Warning,
				TEXT("DeserializeParams: ran out of bytes on parameter '%s'; dropping call."),
				*Prop->GetName());
			return false;
		}
	}

	if (Reader.Tell() != Blob.Num())
	{
		UE_LOG(LogCrowdyRPC, Warning,
			TEXT("DeserializeParams: %lld unread byte(s) after parameters (read %lld of %d); dropping call."),
			static_cast<int64>(Blob.Num()) - Reader.Tell(), Reader.Tell(), Blob.Num());
		return false;
	}

	return true;
}

void FCrowdyRPC::ApplyCall(UObject* Target, UFunction* Fn, const FCrowdyFnInfo& Info, const FCrowdyRpcCall& Call)
{
	if (!Target || !Fn)
	{
		return;
	}

	const int32 FrameSize = FMath::Max<int32>(Fn->ParmsSize, 1);
	uint8* Frame = static_cast<uint8*>(FMemory_Alloca(FrameSize));
	FMemory::Memzero(Frame, FrameSize);
	if (!Info.bParamsPOD)
	{
		InitializeParamProperties(Fn, Frame);
	}

	if (DeserializeParams(Fn, Call.ParamBlob, Frame))
	{
		// Arm the replay scope so the gate a Blueprint event carries runs its body for this
		// exact invocation instead of re-dispatching it. C++ receivers have no gate and never
		// read it; the guards restore the previous values so nested replays stay correct.
		TGuardValue<UObject*> ReplayObjectGuard(ReplayObject, Target);
		TGuardValue<UFunction*> ReplayFunctionGuard(ReplayFunction, Fn);
		Target->ProcessEvent(Fn, Frame);
	}

	if (!Info.bParamsPOD)
	{
		DestroyParamProperties(Fn, Frame);
	}
}

void FCrowdyRPC::EncodeChannelRpc(const FCrowdyRpcCall& Call, uint8 Flags, TArray<uint8>& OutPayload)
{
	OutPayload.Reset();

	FMemoryWriter Writer(OutPayload, /*bIsPersistent=*/true);

	uint8 Version = CrowdyChannelRpcVersion;
	Writer << Version;
	Writer << Flags;

	// The ParamBlob already carries its own version byte, so the channel header sits in front of
	// the whole call. FMemoryWriter's operators handle each field, including the byte array.
	FCrowdyRpcCall Mutable = Call;
	Writer << Mutable.ClassID;
	Writer << Mutable.EntityID;
	Writer << Mutable.SenderID;
	Writer << Mutable.FunctionID;
	Writer << Mutable.ParamBlob;
}

bool FCrowdyRPC::DecodeChannelRpc(const TArray<uint8>& Payload, FCrowdyRpcCall& OutCall, uint8& OutFlags)
{
	if (Payload.Num() < 2)
	{
		UE_LOG(LogCrowdyRPC, Warning, TEXT("DecodeChannelRpc: payload too short for a header; dropping."));
		return false;
	}

	FMemoryReader Reader(Payload, /*bIsPersistent=*/true);

	uint8 Version = 0;
	Reader << Version;
	if (Version != CrowdyChannelRpcVersion)
	{
		UE_LOG(LogCrowdyRPC, Warning,
			TEXT("DecodeChannelRpc: version %u != expected %u; dropping."), Version, CrowdyChannelRpcVersion);
		return false;
	}

	Reader << OutFlags;
	Reader << OutCall.ClassID;
	Reader << OutCall.EntityID;
	Reader << OutCall.SenderID;
	Reader << OutCall.FunctionID;
	Reader << OutCall.ParamBlob;

	if (Reader.IsError())
	{
		UE_LOG(LogCrowdyRPC, Warning, TEXT("DecodeChannelRpc: ran out of bytes decoding the call; dropping."));
		return false;
	}

	return true;
}

UObject* FCrowdyRPC::ReplayObject = nullptr;
UFunction* FCrowdyRPC::ReplayFunction = nullptr;
bool FCrowdyRPC::bLoopbackDelivering = false;

FCrowdyRPC::FScopedEntityContext::FScopedEntityContext(const UObject* ContextObject)
	: Previous(GActiveEntities)
{
	GActiveEntities = ResolveEntitySubsystemFromContext(ContextObject);
}

FCrowdyRPC::FScopedEntityContext::FScopedEntityContext(UCrowdyEntitySubsystem* Entities)
	: Previous(GActiveEntities)
{
	GActiveEntities = Entities;
}

FCrowdyRPC::FScopedEntityContext::~FScopedEntityContext()
{
	GActiveEntities = Previous;
}

bool FCrowdyRPC::DispatchOrReplayBlueprintCall(UObject* Self, UFunction* EventFn, void* ParamFrame,
	FOutParmRec* OutParms)
{
	// This invocation is the replay of a received call when ApplyCall armed the scope for
	// exactly this object and function. Consume it and run the body. Anything else  including
	// a recursive call or the same event on another entity from within a replayed body  falls
	// through and originates a fresh network call.
	if (Self && Self == ReplayObject && EventFn == ReplayFunction)
	{
		ReplayObject = nullptr;
		ReplayFunction = nullptr;
		return false;
	}

	if (!Self || !EventFn)
	{
		return false;
	}

	const FCrowdyFnInfo Info = GetFnInfo(EventFn);

	// Object-reference parameters resolve against this object's world for the encode and local run.
	FScopedEntityContext EntityContext(Self);

	FCrowdyRpcCall Call = BuildCall(EventFn, Info, ParamFrame, OutParms);

	// SerializeAndRoute applies ownership model J: when this client is the authority it runs
	// the body now (via ApplyCall, which the replay scope routes back through the gate) and
	// announces the call; otherwise it delegates to the owner and the body runs on the echo.
	// If there is no Crowdy world to route through it returns false, so the body runs locally
	// as a plain event instead of silently vanishing.
	return SerializeAndRoute(Self, EventFn, Info, MoveTemp(Call));
}

FCrowdyRpcCall FCrowdyRPC::BuildCall(const UFunction* Fn, const FCrowdyFnInfo& Info, const void* Frame,
	FOutParmRec* OutParms)
{
	FCrowdyRpcCall Call;
	if (!Fn)
	{
		return Call;
	}

	// Key by the declaring class, not the instance class, so the inbound resolver
	// (declaring ClassID + FunctionID) finds the same UFunction on the receiver
	// regardless of which subclass issued the call.
	Call.ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(Fn->GetOwnerClass()));
	Call.FunctionID = Info.FunctionID;
	SerializeParams(Fn, Frame, Call.ParamBlob, OutParms);
	return Call;
}

bool FCrowdyRPC::IsRpcTraceEnabled()
{
	return CVarCrowdyRpcTrace.GetValueOnAnyThread() != 0;
}

bool FCrowdyRPC::IsReliableTraceEnabled()
{
	// The umbrella crowdy.rpc.trace covers the reliable path too, so honour either flag.
	return CVarCrowdyRpcReliableTrace.GetValueOnAnyThread() != 0 || CVarCrowdyRpcTrace.GetValueOnAnyThread() != 0;
}

bool FCrowdyRPC::IsLoopbackEnabled()
{
	return CVarCrowdyRpcLoopback.GetValueOnGameThread() != 0;
}

void FCrowdyRPC::DeliverLoopback(UWorld* World, const FCrowdyRpcCall& Call)
{
	UCrowdyEventRouter* Router = World ? World->GetSubsystem<UCrowdyEventRouter>() : nullptr;
	if (!Router)
	{
		return;
	}

	if (IsRpcTraceEnabled())
	{
		UE_LOG(LogCrowdyRPC, Log,
			TEXT("[CrowdyRPC] loopback delivering own call to the local receive path (entity=%s)"),
			*Call.EntityID.ToString());
	}

	// Hold the guard across the whole replay so a replicated event the body issues is sent
	// normally but does not start a second loopback  that bound is what prevents the cascade.
	TGuardValue<bool> LoopbackGuard(bLoopbackDelivering, true);
	Router->ReceiveLoopbackCall(Call);
}

void FCrowdyRPC::RouteOverWire(UCrowdyEntitySubsystem* EntitySubsystem, const AActor* ContextActor,
	const FCrowdyRpcCall& Call, const FCrowdyFnInfo& Info, ECrowdyTarget Target)
{
	if (!EntitySubsystem || !ContextActor)
	{
		return;
	}

	if (IsRpcTraceEnabled())
	{
		UE_LOG(LogCrowdyRPC, Log,
			TEXT("[CrowdyRPC] wire ClassID=%lld FunctionID=%lld entity=%s target=%d bytes=%d"),
			Call.ClassID, Call.FunctionID, *Call.EntityID.ToString(), static_cast<int32>(Target),
			Call.ParamBlob.Num());
	}

	// FCrowdyRpcCall is an ordinary USTRUCT payload, so it rides the existing event
	// transport unchanged  including the StateBytes fragmentation that splits an
	// oversized ParamBlob across datagrams.
	EntitySubsystem->DispatchGameEvent(ContextActor, FInstancedStruct::Make(Call),
		Target, ContextActor, Info.DecayRate, Info.Distance);
}

void FCrowdyRPC::RouteOverChannel(UCrowdyEntitySubsystem* EntitySubsystem, const FCrowdyRpcCall& Call,
	const UFunction* Fn, const FString& ChannelName)
{
	if (!EntitySubsystem)
	{
		return;
	}

	TArray<uint8> ChannelPayload;
	EncodeChannelRpc(Call, /*Flags*/0, ChannelPayload);

	// The channel caps the payload at CrowdyChannelPayloadMaxBytes. Fail loudly rather than let the
	// transport truncate it  registration already rejects a call whose fixed params can never fit.
	if (ChannelPayload.Num() > CrowdyChannelPayloadMaxBytes)
	{
		UE_LOG(LogCrowdyRPC, Error,
			TEXT("[CrowdyRPC] Reliable '%s' dropped  encoded payload is %d bytes, over the %d-byte channel limit."),
			*GetNameSafe(Fn), ChannelPayload.Num(), CrowdyChannelPayloadMaxBytes);
		return;
	}

	if (IsReliableTraceEnabled())
	{
		UE_LOG(LogCrowdyRPC, Log,
			TEXT("[CrowdyRPC] reliable send ClassID=%lld FunctionID=%lld entity=%s channel='%s' bytes=%d"),
			Call.ClassID, Call.FunctionID, *Call.EntityID.ToString(),
			ChannelName.IsEmpty() ? TEXT("<session>") : *ChannelName, ChannelPayload.Num());
	}

	EntitySubsystem->PublishReliableRpc(ChannelName, ChannelPayload);
}

bool FCrowdyRPC::SerializeAndRoute(UObject* Obj, UFunction* Fn, const FCrowdyFnInfo& Info,
	FCrowdyRpcCall Call)
{
	if (!Obj || !Fn)
	{
		return false;
	}

	AActor* ContextActor = ResolveContextActor(Obj);
	if (!ContextActor)
	{
		UE_LOG(LogCrowdyRPC, Warning,
			TEXT("SerializeAndRoute: %s::%s has no owning actor to route from; CrowdyEvents must live on an actor or one of its components."),
			*GetNameSafe(Obj->GetClass()), *Fn->GetName());
		return false;
	}

	UWorld* World = ContextActor->GetWorld();
	if (!World)
	{
		return false;
	}

	UCrowdyEntitySubsystem* EntitySubsystem = World->GetSubsystem<UCrowdyEntitySubsystem>();
	if (!EntitySubsystem)
	{
		UE_LOG(LogCrowdyRPC, Warning, TEXT("SerializeAndRoute: entity subsystem unavailable; dropping %s."),
			*Fn->GetName());
		return false;
	}

	// The receiver runs the call on the entity named here, resolved from its own local registry.
	// SenderID rides the payload because the single-actor transport carries no wire sender.
	const FGuid EntityID = EntitySubsystem->FindEntityID(ContextActor);
	Call.EntityID = EntityID;
	Call.SenderID = EntitySubsystem->GetLocalPlayerID();

	const bool bWeOwnEntity = EntityID.IsValid() && EntitySubsystem->IsLocallyOwned(EntityID);

	const FGuid HostID = EntitySubsystem->GetHostID();
	const bool bWeAreHost = HostID.IsValid() && HostID == EntitySubsystem->GetLocalPlayerID();

	// Loopback debug mode (crowdy.rpc.loopback) delivers the call to this client's own receive
	// path below so a single client can test the round-trip. A tracked entity then runs its body
	// once through that path, so the immediate local run is skipped to avoid running it twice; an
	// untracked caller has no entity for the receive path to resolve, so it still runs here. The
	// guard keeps a loopback-induced call from spawning another loopback.
	const bool bLoopback = IsLoopbackEnabled() && !bLoopbackDelivering;
	const bool bReceivePathWillRun = bLoopback && EntityID.IsValid();

	// Runs the implementation on this client now, unless the loopback receive path will run it.
	const auto RunLocally = [&]()
	{
		if (!bReceivePathWillRun)
		{
			ApplyCall(Obj, Fn, Info, Call);
		}
	};

	if (IsRpcTraceEnabled())
	{
		const TCHAR* ModeText =
			Info.Recipient == ECrowdyEventRecipient::OwningClient
				? (bWeOwnEntity ? TEXT("(owner: run local)") : TEXT("(owner: route to entity's owner)"))
			: Info.Recipient == ECrowdyEventRecipient::Host
				? (bWeAreHost ? TEXT("(host: run local)") : TEXT("(host: route to host)"))
			: Info.Recipient == ECrowdyEventRecipient::Multicast
				? TEXT("(multicast: channel)")
				: TEXT("(multicast: spatial)");
		UE_LOG(LogCrowdyRPC, Log, TEXT("[CrowdyRPC] send %s::%s entity=%s %s"),
			*GetNameSafe(ContextActor->GetClass()), *Fn->GetName(), *EntityID.ToString(), ModeText);
	}

	switch (Info.Recipient)
	{
	case ECrowdyEventRecipient::OwningClient:
		// Owner-only: run locally if we own the entity (or it is untracked, which makes us the
		// authority); otherwise route a request to the owning client via the single-actor transport.
		if (bWeOwnEntity || !EntityID.IsValid())
		{
			RunLocally();
		}
		else
		{
			EntitySubsystem->DispatchSingleActorMessage(ContextActor, FInstancedStruct::Make(Call));
		}
		break;

	case ECrowdyEventRecipient::Host:
		// Host-only: run locally if we are the host; otherwise route a request to the host. The host
		// is addressed through its avatar entity (PlayerDerived NetID == the host's id), so that
		// entity must be in range for us to read its chunk; if it is not, we drop rather than send a
		// bad chunk.
		if (bWeAreHost)
		{
			RunLocally();
		}
		else if (AActor* HostAvatar = EntitySubsystem->FindEntity(HostID))
		{
			EntitySubsystem->DispatchSingleActorMessage(HostAvatar, FInstancedStruct::Make(Call));
		}
		else
		{
			UE_LOG(LogCrowdyRPC, Warning,
				TEXT("SerializeAndRoute: Run-On-Host '%s' dropped  the host's avatar is not in range, so its chunk is unknown."),
				*Fn->GetName());
		}
		break;

	case ECrowdyEventRecipient::Multicast:
		// Channel transport: run locally now and announce over the event's channel (empty = the default
		// session channel)  every member runs it regardless of distance, never decay-thinned. Our own
		// echo is dropped on receipt.
		RunLocally();
		RouteOverChannel(EntitySubsystem, Call, Fn, Info.ChannelName);
		break;

	case ECrowdyEventRecipient::SpatialMulticast:
	default:
		// Spatial path: run locally now and announce to everyone in range (chunk-based, decay-thinned).
		// Our own echo is dropped on receipt so the body never runs twice.
		RunLocally();
		RouteOverWire(EntitySubsystem, ContextActor, Call, Info, ECrowdyTarget::Everyone);
		break;
	}

	if (bLoopback)
	{
		DeliverLoopback(World, Call);
	}

	return true;
}
