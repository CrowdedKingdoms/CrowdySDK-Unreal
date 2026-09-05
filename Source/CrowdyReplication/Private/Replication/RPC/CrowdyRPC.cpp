#include "Replication/RPC/CrowdyRPC.h"

#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Replication/CrowdyBoundedMemoryReader.h"
#include "Replication/RPC/FCrowdyEventParams.h"
#include "Replication/RPC/CrowdyReplicatedEventLibrary.h"
#include "Replication/RPC/ICrowdyEventSource.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "Utils/CrowdyBakedRegistry.h"
#include "Utils/UCrowdyClassRegistry.h"
#include "Components/ActorComponent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Class.h"          // StaticEnum, UEnum
#include "UObject/UnrealType.h"
#include "UObject/EnumProperty.h"
#include "UObject/TextProperty.h"      // FTextProperty (not pulled in by UnrealType.h)
#include "UObject/ObjectKey.h"         // FObjectKey (identity-stable cache key for a UFunction)
#include "UObject/UObjectBaseUtility.h" // GetNameSafe
#include "UObject/Stack.h"             // FOutParmRec (Blueprint frame out-parm records)
#include "UObject/SoftObjectPath.h"    // FSoftObjectPath / FSoftClassPath
#include "UObject/SoftObjectPtr.h"     // FSoftObjectPtr
#include "UObject/UObjectGlobals.h"    // FindObject / LoadObject
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/StructuredArchive.h"
#include "Serialization/StructuredArchiveAdapters.h"
#include "Serialization/StructuredArchiveSlots.h" // FArray / FStream for the bounded container codecs
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

	// A send-side drop repeats for as long as its cause lasts: a host that is out of range, a target this
	// client holds no instance of. One line per entity per frame from a crowd would bury the log, and
	// warning only once would hide a condition that comes and goes, so each function gets at most one
	// line per interval.
	constexpr double CrowdyRpcDropWarnIntervalSeconds = 5.0;

	// True when this function's drop should be logged now. Game-thread only; every caller asserts it.
	bool ShouldLogRpcDrop(int64 FunctionID)
	{
		static TMap<int64, double> LastLoggedSeconds;

		const double Now = FPlatformTime::Seconds();
		if (const double* Last = LastLoggedSeconds.Find(FunctionID))
		{
			if (Now - *Last < CrowdyRpcDropWarnIntervalSeconds)
			{
				return false;
			}
		}

		LastLoggedSeconds.Add(FunctionID, Now);
		return true;
	}

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
	// beyond ParmsSize; UFunction::InitializeStruct would touch those and overrun a frame
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

	// True when a parameter reaches a container THROUGH a struct: a struct that transitively holds a
	// TArray/TSet/TMap, or a container whose element/key/value is such a struct. A direct container
	// parameter is bounded by the codecs below and is NOT flagged here; only the struct-buried form is,
	// because once a struct's SerializeItem owns the read the nested element count cannot be bounded.
	bool ParamBuriesContainerInStruct(const FProperty* Prop)
	{
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			return FCrowdyRPC::StructTransitivelyContainsContainer(StructProp->Struct);
		}
		if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
		{
			return ParamBuriesContainerInStruct(ArrayProp->Inner);
		}
		if (const FSetProperty* SetProp = CastField<FSetProperty>(Prop))
		{
			return ParamBuriesContainerInStruct(SetProp->ElementProp);
		}
		if (const FMapProperty* MapProp = CastField<FMapProperty>(Prop))
		{
			return ParamBuriesContainerInStruct(MapProp->KeyProp) || ParamBuriesContainerInStruct(MapProp->ValueProp);
		}
		return false;
	}

	// Clamps an untrusted element/pair count read from the blob against what the remaining bytes can hold,
	// capped at CrowdyRpcMaxArrayElements. A legitimate container cannot carry more elements than the packet
	// has bytes, so this is the natural ceiling; a forged count trips it and the caller drops the call before
	// any allocation. Ar is the bounded reader whose TotalSize is the blob length.
	bool IsContainerCountInRange(FArchive& Ar, int32 Count)
	{
		const int64 Remaining = Ar.TotalSize() - Ar.Tell();
		return Count >= 0 && Count <= CrowdyRpcMaxArrayElements && Count <= Remaining;
	}

	// Reverses the engine's non-object array encode with the element count bounded BEFORE allocation.
	// FArrayProperty::SerializeItem (the unchanged encode path) reads the untrusted int32 count and
	// EmptyAndAddValues() it before the short read is caught, so a forged count drives a multi-GB
	// allocation from a tiny packet. This mirrors that load path exactly (EnterArray reads the same count
	// the encode wrote) but clamps it first. Object arrays go through DecodeObjectArray; an array whose
	// element buries a container is rejected at registration, so Inner is a bounded leaf or container-free
	// struct here.
	void DecodeBoundedArray(FArrayProperty* ArrayProp, void* ValuePtr, FArchive& Ar)
	{
		FStructuredArchiveFromArchive Adapter(Ar);
		int32 Num = 0;
		FStructuredArchive::FArray Array = Adapter.GetSlot().EnterArray(Num);

		if (!IsContainerCountInRange(Ar, Num))
		{
			UE_LOG(LogCrowdyRPC, Warning,
				TEXT("DecodeBoundedArray: element count %d exceeds the %d-element cap or the remaining bytes; dropping call."),
				Num, CrowdyRpcMaxArrayElements);
			Ar.SetError();
			return;
		}

		FScriptArrayHelper Helper(ArrayProp, ValuePtr);
		Helper.EmptyAndAddValues(Num);
		for (int32 Index = 0; Index < Num && !Ar.IsError(); ++Index)
		{
			ArrayProp->Inner->SerializeItem(Array.EnterElement(), Helper.GetRawPtr(Index));
		}
	}

	// Writes a TSet as [int32 count][elements]. RPC always sends a full snapshot (never a delta against a
	// default), so this replaces the engine's delta format, whose allocating element count sits mid-stream
	// after a remove block and so cannot be bounded by peeking a leading count. The count is a raw int32 the
	// decoder can clamp before allocating; the elements ride a structured stream so each serializes through
	// its own property.
	void EncodeSetSnapshot(const FSetProperty* SetProp, const void* ValuePtr, FArchive& Ar)
	{
		FScriptSetHelper Helper(SetProp, ValuePtr);
		int32 Num = Helper.Num();
		Ar << Num;

		FStructuredArchiveFromArchive Adapter(Ar);
		FStructuredArchive::FStream Stream = Adapter.GetSlot().EnterStream();
		for (FScriptSetHelper::FIterator It(Helper); It; ++It)
		{
			SetProp->ElementProp->SerializeItem(Stream.EnterElement(), Helper.GetElementPtr(It));
		}
	}

	// Reverses EncodeSetSnapshot with the element count bounded before any allocation. Mirrors the engine's
	// empty-set load (EmptyElements, then per element AddDefaultValue_Invalid_NeedsRehash + SerializeItem +
	// Rehash) but reads its own bounded count instead of the delta format's mid-stream one.
	void DecodeSetSnapshot(FSetProperty* SetProp, void* ValuePtr, FArchive& Ar)
	{
		int32 Num = 0;
		Ar << Num;
		if (!IsContainerCountInRange(Ar, Num))
		{
			UE_LOG(LogCrowdyRPC, Warning,
				TEXT("DecodeSetSnapshot: element count %d exceeds the %d-element cap or the remaining bytes; dropping call."),
				Num, CrowdyRpcMaxArrayElements);
			Ar.SetError();
			return;
		}

		FScriptSetHelper Helper(SetProp, ValuePtr);
		Helper.EmptyElements(Num);

		FStructuredArchiveFromArchive Adapter(Ar);
		FStructuredArchive::FStream Stream = Adapter.GetSlot().EnterStream();
		for (int32 Index = 0; Index < Num; ++Index)
		{
			const int32 ElementIndex = Helper.AddDefaultValue_Invalid_NeedsRehash();
			// GetElementPtr (public, checked) is safe here: the freshly-added element is a valid sparse
			// index (only its hash is stale until Rehash), so the IsValidIndex check passes. The engine's
			// own set load uses the private GetElementPtrWithoutCheck, which is not reachable from here.
			SetProp->ElementProp->SerializeItem(Stream.EnterElement(), Helper.GetElementPtr(ElementIndex));
			if (Ar.IsError())
			{
				return;
			}
		}
		Helper.Rehash();
	}

	// Writes a TMap as [int32 pair-count][key,value pairs]. Same rationale as EncodeSetSnapshot: the engine's
	// delta format buries the allocating pair count after a KeysToRemove block, so RPC uses an explicit
	// leading count the decoder can clamp. Each pair is two stream elements (key then value).
	void EncodeMapSnapshot(const FMapProperty* MapProp, const void* ValuePtr, FArchive& Ar)
	{
		FScriptMapHelper Helper(MapProp, ValuePtr);
		int32 Num = Helper.Num();
		Ar << Num;

		FStructuredArchiveFromArchive Adapter(Ar);
		FStructuredArchive::FStream Stream = Adapter.GetSlot().EnterStream();
		for (FScriptMapHelper::FIterator It(Helper); It; ++It)
		{
			MapProp->KeyProp->SerializeItem(Stream.EnterElement(), Helper.GetKeyPtr(It));
			MapProp->ValueProp->SerializeItem(Stream.EnterElement(), Helper.GetValuePtr(It));
		}
	}

	// Reverses EncodeMapSnapshot with the pair count bounded before any allocation. Mirrors the engine's
	// empty-map load (EmptyValues, then per pair AddDefaultValue_Invalid_NeedsRehash + key/value SerializeItem
	// + Rehash) but reads its own bounded count.
	void DecodeMapSnapshot(FMapProperty* MapProp, void* ValuePtr, FArchive& Ar)
	{
		int32 Num = 0;
		Ar << Num;
		if (!IsContainerCountInRange(Ar, Num))
		{
			UE_LOG(LogCrowdyRPC, Warning,
				TEXT("DecodeMapSnapshot: pair count %d exceeds the %d-element cap or the remaining bytes; dropping call."),
				Num, CrowdyRpcMaxArrayElements);
			Ar.SetError();
			return;
		}

		FScriptMapHelper Helper(MapProp, ValuePtr);
		Helper.EmptyValues(Num);

		FStructuredArchiveFromArchive Adapter(Ar);
		FStructuredArchive::FStream Stream = Adapter.GetSlot().EnterStream();
		for (int32 Index = 0; Index < Num; ++Index)
		{
			const int32 PairIndex = Helper.AddDefaultValue_Invalid_NeedsRehash();
			MapProp->KeyProp->SerializeItem(Stream.EnterElement(), Helper.GetKeyPtr(PairIndex));
			MapProp->ValueProp->SerializeItem(Stream.EnterElement(), Helper.GetValuePtr(PairIndex));
			if (Ar.IsError())
			{
				return;
			}
		}
		Helper.Rehash();
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

bool FCrowdyRPC::StructTransitivelyContainsContainer(const UStruct* Struct, int32 Depth)
{
	if (!Struct || Depth > 8)
	{
		return false;
	}
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		const FProperty* Member = *It;
		if (CastField<FArrayProperty>(Member) || CastField<FSetProperty>(Member) || CastField<FMapProperty>(Member))
		{
			return true;
		}
		if (const FStructProperty* MemberStruct = CastField<FStructProperty>(Member))
		{
			if (StructTransitivelyContainsContainer(MemberStruct->Struct, Depth + 1))
			{
				return true;
			}
		}
	}
	return false;
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

		// A container reached through a struct (a struct parameter that buries a TArray/TSet/TMap, or a
		// container whose element/key/value is such a struct) cannot be bounded on decode: once inside the
		// struct's SerializeItem the untrusted element count drives an allocation before the short read is
		// caught. Direct container parameters are bounded by the RPC codec, so only the buried form is
		// rejected. Not registering it also leaves the inbound resolver without an entry, so a forged call
		// to it drops on receipt.
		if (ParamBuriesContainerInStruct(Prop))
		{
			return FString::Printf(
				TEXT("parameter '%s' buries a container (TArray/TSet/TMap) inside a struct; a forged nested ")
				TEXT("element count cannot be bounded on decode. Pass the container as a top-level CrowdyEvent ")
				TEXT("parameter instead."),
				*Prop->GetName());
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
	Info.bIsAction = Fn->HasMetaData(CrowdyRpcMetaKeys::Action);
#else
	if (const FCrowdyBakedRpcFunction* Baked = UCrowdyBakedRegistry::FindRpcFunction(Fn))
	{
		Info.Recipient = Baked->Recipient;
		Info.DecayRate = Baked->DecayRate;
		Info.Distance  = Baked->Distance;
		Info.ChannelName = Baked->ChannelName;
		Info.bIsAction = Baked->bIsAction;
	}
#endif

	return Info;
}

namespace
{
	// Every send and every receive asks for a function's routing info, and building it walks the whole
	// parameter list twice (once for the signature hash, once for the plain-old-data flag) before reading
	// four metadata keys, so it is cached per function.
	//
	// Keyed by FObjectKey rather than a raw UFunction pointer: an FObjectKey carries the object's serial
	// number as well as its slot, so an address that garbage collection later hands to a different function
	// resolves to a clean miss instead of quietly serving the previous function's routing. Recompiling a
	// Blueprint likewise creates new function objects, which key differently and so cannot hit a stale entry.
	//
	// Live Coding is the one event that can rebuild a function's signature while keeping the same object, and
	// the reload-complete delegate drops the whole cache when it does.
	//
	// The lock covers the map. It does not make building an entry thread-safe: with metadata stripped, the
	// first miss reads the baked registry, which can load that asset and root it, so a first miss must happen
	// on the game thread.
	FCriticalSection GFnInfoCacheLock;
	TMap<FObjectKey, FCrowdyFnInfo> GFnInfoCache;

	FDelegateHandle GFnInfoReloadHandle;
}

FCrowdyFnInfo FCrowdyRPC::GetFnInfo(UFunction* Fn)
{
	if (!Fn)
	{
		return BuildFnInfo(Fn);
	}

	const FObjectKey Key(Fn);

	FScopeLock Lock(&GFnInfoCacheLock);
	if (const FCrowdyFnInfo* Found = GFnInfoCache.Find(Key))
	{
		return *Found;
	}
	const FCrowdyFnInfo Built = BuildFnInfo(Fn);
	GFnInfoCache.Add(Key, Built);
	return Built;
}

void FCrowdyRPC::InvalidateFnInfoCache()
{
	FScopeLock Lock(&GFnInfoCacheLock);
	GFnInfoCache.Reset();
}

int32 FCrowdyRPC::NumCachedFnInfo()
{
	FScopeLock Lock(&GFnInfoCacheLock);
	return GFnInfoCache.Num();
}

void FCrowdyRPC::InstallFnInfoCacheInvalidation()
{
#if WITH_EDITOR
	if (GFnInfoReloadHandle.IsValid())
	{
		return;
	}
	// Live Coding rebuilds native reflection in place, so a function's parameter list (and with it the
	// signature hash cached here) can change without the function object changing.
	GFnInfoReloadHandle = FCoreUObjectDelegates::ReloadCompleteDelegate.AddLambda(
		[](EReloadCompleteReason)
		{
			FCrowdyRPC::InvalidateFnInfoCache();
		});
#endif
}

void FCrowdyRPC::RemoveFnInfoCacheInvalidation()
{
#if WITH_EDITOR
	if (GFnInfoReloadHandle.IsValid())
	{
		FCoreUObjectDelegates::ReloadCompleteDelegate.Remove(GFnInfoReloadHandle);
		GFnInfoReloadHandle.Reset();
	}
#endif
	InvalidateFnInfoCache();
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
		else if (const FSetProperty* SetProp = CastField<FSetProperty>(Prop))
		{
			EncodeSetSnapshot(SetProp, ValuePtr, Writer);
		}
		else if (const FMapProperty* MapProp = CastField<FMapProperty>(Prop))
		{
			EncodeMapSnapshot(MapProp, ValuePtr, Writer);
		}
		else
		{
			// Non-object arrays fall here and ride the engine's FArrayProperty::SerializeItem unchanged
			// ([int32 count][elements]); DecodeBoundedArray reads that same layout with the count bounded.
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

	// Bounded reader: caps ArMaxSerializeSize so a forged string/name length prefix in a parameter cannot
	// drive an unbounded allocation before the short read is detected (see FCrowdyBoundedMemoryReader).
	FCrowdyBoundedMemoryReader Reader(Blob, /*bIsPersistent=*/true);

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
		else if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
		{
			// Non-object array (object arrays are handled above): bound the element count before allocating.
			DecodeBoundedArray(ArrayProp, ValuePtr, Reader);
		}
		else if (FSetProperty* SetProp = CastField<FSetProperty>(Prop))
		{
			DecodeSetSnapshot(SetProp, ValuePtr, Reader);
		}
		else if (FMapProperty* MapProp = CastField<FMapProperty>(Prop))
		{
			DecodeMapSnapshot(MapProp, ValuePtr, Reader);
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
		// read it; the scope restores the previous values so nested replays stay correct.
		FScopedReplay ReplayScope(Target, Fn);
		Target->ProcessEvent(Fn, Frame);
	}

	if (!Info.bParamsPOD)
	{
		DestroyParamProperties(Fn, Frame);
	}
}

bool FCrowdyRPC::DecodeCall(UFunction* Fn, const FCrowdyFnInfo& Info, const FCrowdyRpcCall& Call,
	TFunctionRef<void(const FCrowdyEventParams& Params)> OnDecoded)
{
	if (!Fn)
	{
		return false;
	}

	const int32 FrameSize = FMath::Max<int32>(Fn->ParmsSize, 1);
	uint8* Frame = static_cast<uint8*>(FMemory_Alloca(FrameSize));
	FMemory::Memzero(Frame, FrameSize);
	if (!Info.bParamsPOD)
	{
		InitializeParamProperties(Fn, Frame);
	}

	// The frame is alloca'd in THIS stack frame, so it outlives OnDecoded and is gone the moment this
	// returns. Nothing the callback was handed may be stored.
	const bool bDecoded = DeserializeParams(Fn, Call.ParamBlob, Frame);
	if (bDecoded)
	{
		FCrowdyEventParams Params;
		Params.Function = Fn;
		Params.Frame = Frame;
		OnDecoded(Params);
	}

	if (!Info.bParamsPOD)
	{
		DestroyParamProperties(Fn, Frame);
	}

	return bDecoded;
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

bool FCrowdyRPC::IsBlueprintReplicatedEvent(const UFunction* Function)
{
	if (!Function || !CrowdyRpcMetaKeys::HasReplicatesMeta(Function))
	{
		return false;
	}

	// Declared by a Blueprint, not by C++. A native CROWDY_EVENT sends through the macro's thunk and never
	// carries a gate, so asking whether it has one would report every C++ event in the project as broken.
	return Cast<UBlueprintGeneratedClass>(Function->GetOuterUClass()) != nullptr;
}

bool FCrowdyRPC::CarriesDispatchGate(const UFunction* Function)
{
	if (!Function || Function->Script.Num() == 0)
	{
		return false;
	}

	const UFunction* Gate = UCrowdyReplicatedEventLibrary::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UCrowdyReplicatedEventLibrary, CrowdyDispatchReplicatedEvent));
	if (!Gate)
	{
		return false;
	}

	// Every object a compiled body references is listed here, which is what a called function is. Compared
	// against the resolved gate rather than by name, so a same-named function on another class cannot pass.
	return Function->ScriptAndPropertyObjectReferences.Contains(Gate);
}

FCrowdyRPC::FScopedReplay::FScopedReplay(UObject* Object, UFunction* Function)
	: PreviousObject(ReplayObject)
	, PreviousFunction(ReplayFunction)
{
	ReplayObject = Object;
	ReplayFunction = Function;
}

FCrowdyRPC::FScopedReplay::~FScopedReplay()
{
	ReplayObject = PreviousObject;
	ReplayFunction = PreviousFunction;
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
	// Sent from a view of Call rather than a copy of it into an FInstancedStruct: the send is synchronous,
	// so Call outlives it, and the parameter blob is not duplicated to put it on the wire.
	EntitySubsystem->DispatchGameEventView(ContextActor, FCrowdyRpcCall::StaticStruct(), &Call,
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

bool FCrowdyRPC::RouteFromEventSource(UObject* Obj, ICrowdyEventSource* Source, UFunction* Fn,
	const FCrowdyFnInfo& Info, FCrowdyRpcCall Call, UCrowdyEntitySubsystem* EntitySubsystem,
	UWorld* World)
{
	const FGuid EntityID = Source->GetEventEntityID();
	if (!EntityID.IsValid())
	{
		UE_LOG(LogCrowdyRPC, Warning,
			TEXT("SerializeAndRoute: '%s' on event source '%s' has no entity id, so no receiver could resolve it. Dropping."),
			*Fn->GetName(), *GetNameSafe(Obj->GetClass()));
		return true;
	}

	const FGuid LocalPlayerID = EntitySubsystem->GetLocalPlayerID();
	const FGuid HostID = EntitySubsystem->GetHostID();
	const bool bWeAreHost = HostID.IsValid() && HostID == LocalPlayerID;

	// Speak only for an entity this client is the authority for: one it owns, or a world entity while it
	// is the host. Sending for anything else would put this client's identity on another client's entity.
	const FGuid OwnerID = Source->GetEventOwnerID();
	const ECrowdyRole Role = Source->GetEventRole();
	const bool bMaySpeakForEntity = Source->IsEventLocallyOwned()
		|| (bWeAreHost && Role == ECrowdyRole::HostOwned);
	if (!bMaySpeakForEntity)
	{
		UE_LOG(LogCrowdyRPC, Warning,
			TEXT("SerializeAndRoute: '%s' dropped - this client is not the authority for entity %s and may not send for it."),
			*Fn->GetName(), *EntityID.ToString());
		return true;
	}

	Call.EntityID = EntityID;
	// The owner is who the event speaks as. A world entity has no owner, so the local player stands in:
	// the receive path drops a broadcast that echoes back to its sender by comparing this id, and an
	// invalid one matches nobody, which would run the body a second time here.
	Call.SenderID = OwnerID.IsValid() ? OwnerID : LocalPlayerID;

	// Owner-only and host-only both reach their single recipient over the single-actor transport. That
	// transport addresses a registered id plus a chunk, not an actor, so a source with no actor can use
	// it just as well.
	if (Info.Recipient == ECrowdyEventRecipient::OwningClient
		|| Info.Recipient == ECrowdyEventRecipient::Host)
	{
		FCrowdyRpcTarget Target;
		Target.NetID = EntityID;
		Target.OwnerID = OwnerID;
		Target.Role = Role;

		// The source's own position is carried when it has one. Only a send addressed to the region the
		// entity itself stands in reads it, so a source that genuinely has no position right now can
		// still be routed: a host-addressed send is addressed from where the HOST stands, and a call the
		// model runs on this client goes nowhere at all. The one route that needs a position reports its
		// own drop when there is none.
		FVector SourceLocation = FVector::ZeroVector;
		if (Source->GetEventLocation(SourceLocation))
		{
			Target.Location = SourceLocation;
		}

		UE_CLOG(IsRpcTraceEnabled(), LogCrowdyRPC, Log,
			TEXT("[CrowdyRPC] send %s::%s entity=%s source=event-source route=targeted"),
			*GetNameSafe(Obj->GetClass()), *Fn->GetName(), *EntityID.ToString());

		// The source is the instance the body runs on when the model says it also runs here, exactly as
		// on the two broadcast paths below.
		RouteToTarget(World, EntitySubsystem, Target, Obj, Fn, Info, Call);
		return true;
	}

	// Loopback debug mode runs the call through this client's own receive path instead of invoking it
	// directly, so a single client exercises resolve and dispatch. Same bound as the actor path: a call
	// issued from the replayed body is sent normally but starts no second loopback.
	const bool bLoopback = IsLoopbackEnabled() && !bLoopbackDelivering;

	if (Info.Recipient == ECrowdyEventRecipient::Multicast)
	{
		UE_CLOG(IsRpcTraceEnabled(), LogCrowdyRPC, Log,
			TEXT("[CrowdyRPC] send %s::%s entity=%s source=event-source route=channel"),
			*GetNameSafe(Obj->GetClass()), *Fn->GetName(), *EntityID.ToString());

		if (!bLoopback)
		{
			ApplyCall(Obj, Fn, Info, Call);
		}
		RouteOverChannel(EntitySubsystem, Call, Fn, Info.ChannelName);
	}
	else
	{
		// Spatial Multicast, and the unannotated default. The position comes from the source itself, which
		// reads it from the entity's own simulation state; there is no caller-supplied location to trust.
		FVector Location = FVector::ZeroVector;
		if (!Source->GetEventLocation(Location))
		{
			UE_LOG(LogCrowdyRPC, Warning,
				TEXT("SerializeAndRoute: '%s' dropped - entity %s reports no location, and the spatial transport "
					 "addresses by position."),
				*Fn->GetName(), *EntityID.ToString());
			return true;
		}

		UE_CLOG(IsRpcTraceEnabled(), LogCrowdyRPC, Log,
			TEXT("[CrowdyRPC] send %s::%s entity=%s source=event-source route=spatial at=%s"),
			*GetNameSafe(Obj->GetClass()), *Fn->GetName(), *EntityID.ToString(), *Location.ToCompactString());

		if (!bLoopback)
		{
			ApplyCall(Obj, Fn, Info, Call);
		}
		EntitySubsystem->DispatchGameEventAt(Location, FInstancedStruct::Make(Call),
			ECrowdyTarget::Everyone, nullptr, Info.DecayRate, Info.Distance);
	}

	if (bLoopback)
	{
		DeliverLoopback(World, Call);
	}

	return true;
}

FCrowdyRpcRouteDecision FCrowdyRPC::DecideRoute(ECrowdyEventRecipient Recipient, bool bNonSpatial,
	bool bEntityValid, bool bWeOwnEntity, bool bWeAreHost)
{
	FCrowdyRpcRouteDecision Decision;

	switch (Recipient)
	{
	case ECrowdyEventRecipient::OwningClient:
		// Owner-only: run locally when we own the entity (or it is untracked, which makes us the authority).
		// An actor delegates to the owner over the single-actor transport; a non-spatial participant has no
		// single-actor transport, so it always announces over the channel and the receive gate keeps it to
		// the intended owner.
		if (bNonSpatial)
		{
			Decision.bRunLocally = bWeOwnEntity || !bEntityValid;
			Decision.Route = ECrowdyRpcRoute::Channel;
		}
		else if (bWeOwnEntity || !bEntityValid)
		{
			Decision.bRunLocally = true;
			Decision.Route = ECrowdyRpcRoute::None;
		}
		else
		{
			Decision.bRunLocally = false;
			Decision.Route = ECrowdyRpcRoute::SingleActorToOwner;
		}
		break;

	case ECrowdyEventRecipient::Host:
		// Host-only: run locally when we are the host. An actor delegates to the host's avatar over the
		// single-actor transport; a non-spatial participant announces over the channel and the receive gate
		// keeps it to the host.
		if (bNonSpatial)
		{
			Decision.bRunLocally = bWeAreHost;
			Decision.Route = ECrowdyRpcRoute::Channel;
		}
		else if (bWeAreHost)
		{
			Decision.bRunLocally = true;
			Decision.Route = ECrowdyRpcRoute::None;
		}
		else
		{
			Decision.bRunLocally = false;
			Decision.Route = ECrowdyRpcRoute::SingleActorToHost;
		}
		break;

	case ECrowdyEventRecipient::Multicast:
		// Channel transport: run locally now and announce over the session channel to every member. Identical
		// for an actor and a non-spatial participant.
		Decision.bRunLocally = true;
		Decision.Route = ECrowdyRpcRoute::Channel;
		break;

	case ECrowdyEventRecipient::SpatialMulticast:
	default:
		// Spatial path (also the unannotated default). A non-spatial participant has no world location, so this
		// is hard-rejected at send; the author must set CrowdyRecipient=Multicast or Host. An actor announces to
		// everyone in range (chunk-based, decay-thinned).
		if (bNonSpatial)
		{
			Decision.bRunLocally = false;
			Decision.Route = ECrowdyRpcRoute::Reject;
		}
		else
		{
			Decision.bRunLocally = true;
			Decision.Route = ECrowdyRpcRoute::SpatialBroadcast;
		}
		break;
	}

	return Decision;
}

bool FCrowdyRPC::SerializeAndRoute(UObject* Obj, UFunction* Fn, const FCrowdyFnInfo& Info,
	FCrowdyRpcCall Call)
{
	if (!Obj || !Fn)
	{
		return false;
	}

	// A CrowdyEvent lives on an actor/component (routed by the actor's entity identity) or on a non-actor
	// participant enrolled via RegisterParticipant (a subsystem). ContextActor is null in the latter case.
	AActor* ContextActor = ResolveContextActor(Obj);

	UWorld* World = ContextActor ? ContextActor->GetWorld() : Obj->GetWorld();
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

	// A sender with no owning actor can still carry an entity identity and a world position of its own, by
	// implementing ICrowdyEventSource: that is how an entity simulated without an actor sends. Gated on
	// there being no owning actor, so an actor or a component reaches exactly the code it reached before
	// this branch existed, whether or not it also implements the interface.
	if (!ContextActor)
	{
		if (ICrowdyEventSource* EventSource = Cast<ICrowdyEventSource>(Obj))
		{
			return RouteFromEventSource(Obj, EventSource, Fn, Info, MoveTemp(Call), EntitySubsystem, World);
		}
	}

	// Resolve the sending identity: an actor by its context actor, a non-actor by its own enrolled NetID.
	FGuid EntityID;
	bool bNonSpatial = false;
	if (ContextActor)
	{
		EntityID = EntitySubsystem->FindEntityID(ContextActor);
	}
	else
	{
		// A non-actor sender (a subsystem) resolves its OWN enrolled NetID.
		EntityID = EntitySubsystem->FindEntityID(Obj);
		if (!EntityID.IsValid())
		{
			UE_LOG(LogCrowdyRPC, Warning,
				TEXT("SerializeAndRoute: %s::%s has no owning actor and is not an enrolled participant; "
					 "CrowdyEvents must live on an actor/component or on a subsystem enrolled via RegisterParticipant."),
				*GetNameSafe(Obj->GetClass()), *Fn->GetName());
			return false;
		}
		bNonSpatial = true;
	}

	// The receiver runs the call on the entity named here, resolved from its own local registry.
	// SenderID rides the payload because the single-actor and channel transports carry no wire sender.
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

	const FCrowdyRpcRouteDecision Decision =
		DecideRoute(Info.Recipient, bNonSpatial, EntityID.IsValid(), bWeOwnEntity, bWeAreHost);

	if (Decision.Route == ECrowdyRpcRoute::Reject)
	{
		// SpatialMulticast (also the unannotated default) has no location on a non-spatial participant. Consume
		// the call (return true) so a Blueprint caller does NOT run the body as a local fallback  the author must
		// pick a channel-based recipient.
		UE_LOG(LogCrowdyRPC, Error,
			TEXT("SerializeAndRoute: '%s' on non-spatial participant '%s' uses SpatialMulticast, which has no "
				 "location; set CrowdyRecipient=Multicast or Host. Dropping."),
			*Fn->GetName(), *GetNameSafe(Obj->GetClass()));
		return true;
	}

	if (IsRpcTraceEnabled())
	{
		UE_LOG(LogCrowdyRPC, Log,
			TEXT("[CrowdyRPC] send %s::%s entity=%s nonSpatial=%d route=%d run=%d"),
			*GetNameSafe(Obj->GetClass()), *Fn->GetName(), *EntityID.ToString(),
			bNonSpatial ? 1 : 0, static_cast<int32>(Decision.Route), Decision.bRunLocally ? 1 : 0);
	}

	// Run the implementation on this client now, unless the loopback receive path will run it once instead.
	if (Decision.bRunLocally && !bReceivePathWillRun)
	{
		ApplyCall(Obj, Fn, Info, Call);
	}

	switch (Decision.Route)
	{
	case ECrowdyRpcRoute::None:
		break;

	case ECrowdyRpcRoute::SpatialBroadcast:
		// Only reached for an actor (SpatialMulticast is never a non-spatial route), so ContextActor is non-null.
		RouteOverWire(EntitySubsystem, ContextActor, Call, Info, ECrowdyTarget::Everyone);
		break;

	case ECrowdyRpcRoute::Channel:
		RouteOverChannel(EntitySubsystem, Call, Fn, Info.ChannelName);
		break;

	case ECrowdyRpcRoute::SingleActorToOwner:
		// Only reached for an actor whose owner is another client, so ContextActor is non-null.
		EntitySubsystem->DispatchSingleActorMessage(ContextActor, FInstancedStruct::Make(Call));
		break;

	case ECrowdyRpcRoute::SingleActorToHost:
		// The host is addressed through its avatar entity (PlayerDerived NetID == the host's id), so that entity
		// must be in range for us to read its chunk; if it is not, we drop rather than send a bad chunk.
		if (AActor* HostAvatar = EntitySubsystem->FindEntity(HostID))
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

	default:
		break;
	}

	if (bLoopback)
	{
		DeliverLoopback(World, Call);
	}

	return true;
}

UObject* FCrowdyRPC::ResolveLocalTargetReceiver(UCrowdyEntitySubsystem* EntitySubsystem, const FGuid& NetID,
	const UFunction* Fn)
{
	if (!EntitySubsystem || !Fn)
	{
		return nullptr;
	}

	UObject* Participant = EntitySubsystem->FindParticipant(NetID);
	if (!Participant)
	{
		return nullptr;
	}

	// The body is declared on one class, so it runs on the object of that class the participant carries:
	// the participant itself, or the first component on it that is one. This is the same resolution an
	// inbound call goes through, shared here rather than restated.
	return UCrowdyEventRouter::ResolveStateContainer(Participant, Fn->GetOwnerClass());
}

bool FCrowdyRPC::RouteToTarget(UWorld* World, UCrowdyEntitySubsystem* EntitySubsystem,
	const FCrowdyRpcTarget& Target, UObject* LocalInstance, UFunction* Fn,
	const FCrowdyFnInfo& Info, const FCrowdyRpcCall& Call)
{
	// The registry reads below, the entity context the encode installed, and a local run that reaches
	// ProcessEvent are all game-thread only.
	if (!ensure(IsInGameThread()))
	{
		return true;
	}

	if (!EntitySubsystem)
	{
		// No transport to route through at all, so a caller holding a body may run it as a local fallback.
		return false;
	}

	if (!Fn || !Target.IsRoutable())
	{
		return true;
	}

	const FGuid LocalPlayerID = EntitySubsystem->GetLocalPlayerID();
	const FGuid HostID = EntitySubsystem->GetHostID();
	const bool bWeAreHost = HostID.IsValid() && HostID == LocalPlayerID;

	// Where this client already holds a registration for the target, that record is the authority on who
	// owns the entity and, where it holds an actor, on where the entity stands. What the caller resolved
	// describes the same entity read from wherever its state lives, so preferring the record keeps the two
	// from disagreeing about a decision the rest of this client already makes from the record.
	FCrowdyRpcTarget Resolved = Target;
	if (const FCrowdyEntityRecord* Record = EntitySubsystem->FindRecord(Target.NetID))
	{
		Resolved.OwnerID = Record->OwnerID;
		Resolved.Role = Record->Role;
		if (const AActor* RecordActor = Record->GetActor())
		{
			Resolved.Location = RecordActor->GetActorLocation();
		}
	}

	const bool bWeOwnTarget = Resolved.IsOwnedBy(LocalPlayerID);

	// A target names a place in the world, so every recipient is available to it and the policy is the
	// same table the actor path reads.
	const FCrowdyRpcRouteDecision Decision = DecideRoute(Info.Recipient, /*bNonSpatial=*/false,
		/*bEntityValid=*/true, bWeOwnTarget, bWeAreHost);

	// Loopback debug mode delivers the call to this client's own receive path below, which runs the body
	// once, so the immediate local run is skipped rather than running it twice. The guard keeps a call
	// issued from a replayed body from starting a second loopback.
	const bool bLoopback = IsLoopbackEnabled() && !bLoopbackDelivering;

	if (IsRpcTraceEnabled())
	{
		const FString LocationText = Resolved.HasLocation()
			? Resolved.Location.GetValue().ToCompactString()
			: FString(TEXT("<unknown>"));
		UE_LOG(LogCrowdyRPC, Log,
			TEXT("[CrowdyRPC] send %s target=%s owned=%d route=%d run=%d at=%s"),
			*Fn->GetName(), *Resolved.NetID.ToString(), bWeOwnTarget ? 1 : 0,
			static_cast<int32>(Decision.Route), Decision.bRunLocally ? 1 : 0, *LocationText);
	}

	bool bRanLocally = false;
	if (Decision.bRunLocally && !bLoopback)
	{
		// A caller that already holds the instance hands it in; otherwise the body runs on whatever object
		// holds the target's identity on this client, which is the real actor where this client owns the
		// target and a stand-in proxy where it does not. A target this client only renders has no such
		// object: the function is declared on the target's own class, and a client that holds the target
		// as an instance is what executes it.
		UObject* Receiver = LocalInstance
			? LocalInstance
			: ResolveLocalTargetReceiver(EntitySubsystem, Resolved.NetID, Fn);
		if (Receiver)
		{
			ApplyCall(Receiver, Fn, Info, Call);
			bRanLocally = true;
		}
	}

	switch (Decision.Route)
	{
	case ECrowdyRpcRoute::None:
		// This client is the only one that runs the body, so there is nothing to send. Whether it actually
		// ran is settled below.
		break;

	case ECrowdyRpcRoute::SpatialBroadcast:
		// The spatial transport reaches everyone in range of the region the target stands in, so without a
		// position there is no region to announce from.
		if (!Resolved.HasLocation())
		{
			const bool bLogDrop = ShouldLogRpcDrop(Info.FunctionID);
			UE_CLOG(bLogDrop, LogCrowdyRPC, Warning,
				TEXT("[CrowdyRPC] '%s' dropped - it announces from where entity %s stands, and that position is "
					 "not known right now."),
				*Fn->GetName(), *Resolved.NetID.ToString());
			break;
		}
		EntitySubsystem->DispatchGameEventAt(Resolved.Location.GetValue(), FInstancedStruct::Make(Call),
			ECrowdyTarget::Everyone, nullptr, Info.DecayRate, Info.Distance);
		break;

	case ECrowdyRpcRoute::Channel:
		RouteOverChannel(EntitySubsystem, Call, Fn, Info.ChannelName);
		break;

	case ECrowdyRpcRoute::SingleActorToOwner:
		// The server delivers a single-actor message for an id to the client that owns that id. A world
		// entity is owned by no client, so such a message would be addressed to nobody and vanish.
		if (Resolved.BelongsToNoClient())
		{
			const bool bLogDrop = ShouldLogRpcDrop(Info.FunctionID);
			UE_CLOG(bLogDrop, LogCrowdyRPC, Warning,
				TEXT("[CrowdyRPC] '%s' dropped - it is owner-only and entity %s belongs to no client, so there is "
					 "no client to deliver it to. A world entity is reached with CrowdyRecipient=Host."),
				*Fn->GetName(), *Resolved.NetID.ToString());
			break;
		}
		// Addressed to the target's OWN id, from the chunk the target stands in.
		if (!Resolved.HasLocation())
		{
			const bool bLogDrop = ShouldLogRpcDrop(Info.FunctionID);
			UE_CLOG(bLogDrop, LogCrowdyRPC, Warning,
				TEXT("[CrowdyRPC] '%s' dropped - it is addressed to the chunk entity %s stands in, and that "
					 "position is not known right now."),
				*Fn->GetName(), *Resolved.NetID.ToString());
			break;
		}
		EntitySubsystem->DispatchSingleActorMessageTo(Resolved.NetID, Resolved.Location.GetValue(),
			FInstancedStruct::Make(Call));
		break;

	case ECrowdyRpcRoute::SingleActorToHost:
	{
		// The host is addressed by its OWN id, from the chunk the HOST stands in, never from where this
		// call's target stands. That position comes from what this client holds for the host, which is the
		// host's own registration; a client that holds the host only as rendering data has no position for
		// it and cannot address the message.
		const FCrowdyEntityRecord* HostRecord = EntitySubsystem->FindRecord(HostID);
		const AActor* HostActor = HostRecord ? HostRecord->GetActor() : nullptr;
		if (!HostActor)
		{
			const bool bLogDrop = ShouldLogRpcDrop(Info.FunctionID);
			UE_CLOG(bLogDrop, LogCrowdyRPC, Warning,
				TEXT("[CrowdyRPC] '%s' dropped - it is host-only, and this client holds no positioned instance of "
					 "the host, so the chunk to address it to is unknown."),
				*Fn->GetName());
			break;
		}
		EntitySubsystem->DispatchSingleActorMessageTo(HostID, HostActor->GetActorLocation(),
			FInstancedStruct::Make(Call));
		break;
	}

	default:
		// The only rejection DecideRoute produces is SpatialMulticast on a participant with no world
		// location, and a target is never that, so nothing reaches here.
		break;
	}

	// Nothing ran and nothing went out: the model named this client as the one that runs the body, and
	// this client holds no instance of the target to run it on. No other client will hear about the call
	// either, so it is a drop and is reported as one rather than passing for a quiet success.
	if (Decision.Route == ECrowdyRpcRoute::None && !bRanLocally && !bLoopback)
	{
		const bool bLogDrop = ShouldLogRpcDrop(Info.FunctionID);
		UE_CLOG(bLogDrop, LogCrowdyRPC, Warning,
			TEXT("[CrowdyRPC] '%s' reached nobody - this client is the one that runs it for entity %s, and holds "
				 "no instance of that entity to run it on."),
			*Fn->GetName(), *Resolved.NetID.ToString());
	}

	if (bLoopback)
	{
		DeliverLoopback(World, Call);
	}

	// Every outcome above other than a missing subsystem is either a send, a local run, or a deliberate
	// drop, and none of them is a reason for the caller to run its body as a local fallback.
	return true;
}

bool FCrowdyRPC::SendToTarget(UWorld* World, const FCrowdyRpcTarget& Target, UFunction* Fn,
	const void* Frame, FOutParmRec* OutParms)
{
	// The encode installs a process-wide entity context and the routing below reads the entity registry,
	// both of which are game-thread only. Returning true keeps a caller from running its body off the
	// game thread instead.
	if (!ensure(IsInGameThread()))
	{
		return true;
	}

	if (!Fn)
	{
		return false;
	}

	if (!World)
	{
		// No world means no transport, so a caller holding a body may run it as a local fallback.
		return false;
	}

	if (!Target.IsRoutable())
	{
		UE_LOG(LogCrowdyRPC, Warning,
			TEXT("SendToTarget: '%s' dropped - the target names no entity, so no receiver could resolve the call."),
			*Fn->GetName());
		return true;
	}

	// Every input parameter is read out of the frame, so a function that takes any needs one. A function
	// that takes none may pass null; the reflected walk still needs an address to start from, so it gets
	// a local it never reads through.
	uint8 EmptyFrame = 0;
	if (!Frame && Fn->ParmsSize > 0)
	{
		UE_LOG(LogCrowdyRPC, Warning,
			TEXT("SendToTarget: '%s' takes %d byte(s) of parameters but no frame was supplied; dropping."),
			*Fn->GetName(), static_cast<int32>(Fn->ParmsSize));
		return true;
	}

	UCrowdyEntitySubsystem* EntitySubsystem = World->GetSubsystem<UCrowdyEntitySubsystem>();
	if (!EntitySubsystem)
	{
		UE_LOG(LogCrowdyRPC, Warning, TEXT("SendToTarget: entity subsystem unavailable; dropping %s."),
			*Fn->GetName());
		return false;
	}

	const FCrowdyFnInfo Info = GetFnInfo(Fn);

	// Object-reference parameters resolve against the target's world while the call is encoded.
	FScopedEntityContext EntityContext(EntitySubsystem);

	// Keyed by the DECLARING class, as on every other send path, so the receiver resolves the same
	// function on whatever instance it holds for this entity.
	FCrowdyRpcCall Call = BuildCall(Fn, Info, Frame ? Frame : &EmptyFrame, OutParms);

	// The receiver runs the call on the entity named here. The sender is always this client: a call aimed
	// at another player's entity still goes out as us, never as them.
	Call.EntityID = Target.NetID;
	Call.SenderID = EntitySubsystem->GetLocalPlayerID();

	// No instance issued this call, so there is none to run the body on; RouteToTarget looks one up only
	// if the ownership model says the body also runs on this client.
	return RouteToTarget(World, EntitySubsystem, Target, /*LocalInstance=*/nullptr, Fn, Info, Call);
}

#if !UE_BUILD_SHIPPING
/**
 * Answers, in THIS process, what a Crowdy event actually is here: whether the class and function resolve,
 * whether the function is marked replicated, where it routes, and whether its compiled body still carries
 * the dispatch gate.
 *
 * It exists because two clients running the same asset can disagree about that last one, and nothing else
 * can see the disagreement: the send path of an ungated event is never entered, so it reports no drop and
 * logs nothing at all. Run it in each process and compare the two lines.
 */
static FAutoConsoleCommand GCrowdyRpcDumpFn(
	TEXT("crowdy.rpc.dumpfn"),
	TEXT("Reports what a Crowdy event is in this process: resolved, replicated, its recipient, its FunctionID, and whether its compiled body carries the dispatch gate. Args: <ClassPathOrName> <FunctionName>. A Blueprint class name ends in _C."),
	FConsoleCommandWithArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogCrowdyRPC, Warning,
					TEXT("[CrowdyRPC] dumpfn: name a class and a function, for example crowdy.rpc.dumpfn BP_Hero_C MyEvent"));
				return;
			}

			UClass* Class = UClass::TryFindTypeSlow<UClass>(Args[0]);
			if (!Class)
			{
				Class = LoadObject<UClass>(nullptr, *Args[0]);
			}

			if (!Class)
			{
				UE_LOG(LogCrowdyRPC, Warning,
					TEXT("[CrowdyRPC] dumpfn: nothing loaded for '%s'. A Blueprint class name ends in _C."), *Args[0]);
				return;
			}

			// By name rather than through the registry, so a function the registry REFUSED still reports.
			UFunction* Function = Class->FindFunctionByName(FName(*Args[1]));
			if (!Function)
			{
				// A C++ CROWDY_EVENT's receiver is the _Implementation, which is what a caller naming the
				// event itself would miss, so it is tried rather than left as "no such function".
				Function = Class->FindFunctionByName(FName(*(Args[1] + TEXT("_Implementation"))));
			}

			if (!Function)
			{
				UE_LOG(LogCrowdyRPC, Warning,
					TEXT("[CrowdyRPC] dumpfn: '%s' declares no function '%s' (nor its _Implementation)."),
					*Class->GetName(), *Args[1]);
				return;
			}

			const bool bReplicates = CrowdyRpcMetaKeys::HasReplicatesMeta(Function);
			const bool bBlueprintEvent = FCrowdyRPC::IsBlueprintReplicatedEvent(Function);
			const bool bGate = FCrowdyRPC::CarriesDispatchGate(Function);
			const FCrowdyFnInfo Info = FCrowdyRPC::GetFnInfo(Function);
			const UEnum* RecipientEnum = StaticEnum<ECrowdyEventRecipient>();

			UE_LOG(LogCrowdyRPC, Display,
				TEXT("[CrowdyRPC] dumpfn %s::%s declaredOn=%s replicates=%d blueprintEvent=%d gate=%s recipient=%s action=%d functionID=%lld"),
				*Class->GetName(), *Function->GetName(), *GetNameSafe(Function->GetOuterUClass()),
				bReplicates ? 1 : 0, bBlueprintEvent ? 1 : 0,
				bBlueprintEvent ? (bGate ? TEXT("YES") : TEXT("MISSING")) : TEXT("n/a, native"),
				RecipientEnum ? *RecipientEnum->GetNameStringByValue(static_cast<int64>(Info.Recipient)) : TEXT("?"),
				Info.bIsAction ? 1 : 0,
				Info.FunctionID);

			// Reported in the same breath as the gate, because the two fail the same way: the event travels,
			// the body runs, and the half that reads this flag does nothing. An event declared an action is
			// what makes a crowd entity animate without any registration, so an event that means to be one
			// and is not here starts nothing on a client that draws the entity as a row or a promoted actor.
			UE_CLOG(!Info.bIsAction, LogCrowdyRPC, Display,
				TEXT("[CrowdyRPC] dumpfn: this event is NOT an action in this process, so it starts no animation on a crowd entity. A Blueprint event carries that through its Is A One-Shot Action marker; if it is ticked in the editor and reads 0 here, this process did not get the marker."));

			if (bBlueprintEvent && !bGate)
			{
				UE_LOG(LogCrowdyRPC, Error,
					TEXT("[CrowdyRPC] dumpfn: this event sends NOTHING in this process. Its body runs locally and no send code is entered, so nothing else reports it."));
			}
		}));
#endif
