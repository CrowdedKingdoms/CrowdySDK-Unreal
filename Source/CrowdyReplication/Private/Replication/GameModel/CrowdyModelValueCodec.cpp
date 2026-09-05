// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/CrowdyModelValueCodec.h"

#include "CrowdyGameModelLog.h"
#include "Replication/GameModel/CrowdyJsonStringSupport.h"
#include "Replication/GameModel/CrowdyModelRef.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace
{
	// A double as canonical JSON text: an integral value prints as an integer (so 10 and 10.0 read the same),
	// a fractional value uses the same float print both sides. Mirrors JsonValueToCompactString / CanonicalNumber
	// so an encoded array default is byte-stable and the schema-sync diff sees no spurious change.
	FString CanonicalNumberText(double Number)
	{
		if (FMath::Abs(Number) < 9.2e18 && Number == FMath::RoundToDouble(Number))
		{
			return FString::Printf(TEXT("%lld"), static_cast<int64>(FMath::RoundToDouble(Number)));
		}
		return FString::SanitizeFloat(Number);
	}

	// Guards the double -> int64 cast: a value with |x| >= ~9.2e18 (or NaN) is outside int64 range, where a plain
	// static_cast is undefined behavior on forged input. Saturate to the range instead (mirrors the 9.2e18 gate
	// the canonicalizers use). A well-formed server integer is always in range, so this only bites forged input.
	int64 SafeDoubleToInt64(double Number)
	{
		if (!(FMath::Abs(Number) < 9.2e18))
		{
			return Number < 0.0 ? MIN_int64 : MAX_int64;
		}
		return static_cast<int64>(Number);
	}

	// Writes one decoded JSON value onto a single scalar leaf (a numeric width, an enum's underlying integer, a
	// bool, or an FString). This is the one scalar-write implementation the whole apply path shares, so an array
	// element is written exactly like a top-level attribute of the same leaf type. Coercion matches the historic
	// inline write (AsNumber/AsBool/AsString), now with a saturating int cast; array element writes are strictly
	// type-checked before they reach here (see JsonMatchesLeaf). Returns true when Leaf is a supported leaf that
	// was written, false for an unsupported leaf type, so the caller can distinguish an applied value from a no-op.
	bool WriteScalarLeaf(const FProperty* Leaf, void* Addr, const TSharedPtr<FJsonValue>& Value)
	{
		if (const FNumericProperty* Num = CastField<FNumericProperty>(Leaf))
		{
			if (Num->IsFloatingPoint())
			{
				Num->SetFloatingPointPropertyValue(Addr, Value->AsNumber());
			}
			else
			{
				Num->SetIntPropertyValue(Addr, SafeDoubleToInt64(Value->AsNumber()));
			}
			return true;
		}
		if (const FEnumProperty* Enum = CastField<FEnumProperty>(Leaf))
		{
			if (const FNumericProperty* Underlying = Enum->GetUnderlyingProperty())
			{
				Underlying->SetIntPropertyValue(Addr, SafeDoubleToInt64(Value->AsNumber()));
				return true;
			}
			return false;
		}
		if (const FBoolProperty* Bool = CastField<FBoolProperty>(Leaf))
		{
			Bool->SetPropertyValue(Addr, Value->AsBool());
			return true;
		}
		if (const FStrProperty* Str = CastField<FStrProperty>(Leaf))
		{
			Str->SetPropertyValue(Addr, Value->AsString());
			return true;
		}
		return false;
	}

	// Reads one scalar leaf back to canonical JSON text for a CDO default (the inverse of WriteScalarLeaf).
	FString EncodeScalarLeaf(const FProperty* Leaf, const void* Addr)
	{
		if (const FEnumProperty* Enum = CastField<FEnumProperty>(Leaf))
		{
			const FNumericProperty* Underlying = Enum->GetUnderlyingProperty();
			return Underlying ? FString::Printf(TEXT("%lld"), Underlying->GetSignedIntPropertyValue(Addr)) : TEXT("0");
		}
		if (const FNumericProperty* Num = CastField<FNumericProperty>(Leaf))
		{
			return Num->IsFloatingPoint()
				? CanonicalNumberText(Num->GetFloatingPointPropertyValue(Addr))
				: FString::Printf(TEXT("%lld"), Num->GetSignedIntPropertyValue(Addr));
		}
		if (const FBoolProperty* Bool = CastField<FBoolProperty>(Leaf))
		{
			return Bool->GetPropertyValue(Addr) ? TEXT("true") : TEXT("false");
		}
		if (const FStrProperty* Str = CastField<FStrProperty>(Leaf))
		{
			return FString::Printf(TEXT("\"%s\""), *EscapeJsonStringBody(Str->GetPropertyValue(Addr)));
		}
		return TEXT("null");
	}

	// Strict per-element type gate: an array element must match its destination leaf's JSON kind exactly. A
	// nested array or object element is a numeric/bool/string mismatch and so is rejected here, which also caps
	// decode depth at one for a scalar array.
	bool JsonMatchesLeaf(const FProperty* Leaf, const FJsonValue& Value)
	{
		if (CastField<FNumericProperty>(Leaf) || CastField<FEnumProperty>(Leaf))
		{
			return Value.Type == EJson::Number;
		}
		if (CastField<FBoolProperty>(Leaf))
		{
			return Value.Type == EJson::Boolean;
		}
		if (CastField<FStrProperty>(Leaf))
		{
			return Value.Type == EJson::String;
		}
		return false;
	}

	// Forward decls for the mutual recursion between the object type predicate, decoder, and encoder and their members.
	bool StructInScopeForObject(const UScriptStruct* Struct, int32 Depth);
	bool DecodeValueRecursive(const FProperty* Property, void* ValueAddr, const TSharedPtr<FJsonValue>& Value, int32 Depth);
	FString EncodeMemberValue(const FProperty* Member, const void* Addr, int32 Depth);

	// One member of a plain-struct "object" is in scope when it is a scalar leaf, a supported scalar array, or a
	// nested in-scope plain native struct. An object-reference member, a map/set, an array of non-scalars, or an
	// FCrowdyModelRef member all take the containing struct out of scope: a server "object" is opaque data, so it
	// cannot carry reference or map semantics.
	bool MemberInScopeForObject(const FProperty* Member, int32 Depth)
	{
		if (!Member)
		{
			return false;
		}
		if (FCrowdyModelValueCodec::IsSupportedArrayInner(Member)) // a scalar leaf (numeric/enum/bool/string)
		{
			return true;
		}
		if (const FArrayProperty* Array = CastField<FArrayProperty>(Member))
		{
			return FCrowdyModelValueCodec::IsSupportedArrayInner(Array->Inner);
		}
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Member))
		{
			return !FCrowdyModelValueCodec::IsModelRefStruct(StructProp->Struct)
				&& StructInScopeForObject(StructProp->Struct, Depth + 1);
		}
		return false;
	}

	// A struct maps to "object" when it is a NATIVE UScriptStruct (a Blueprint UUserDefinedStruct's authored
	// member names differ between the editor and a cooked build when a member is renamed, so its keys would not
	// round-trip), is not the FCrowdyModelRef wrapper (that has dedicated container_ref semantics), has at least
	// one member, and every member is itself in scope. Bounded by MaxStructDepth so a pathological type cannot
	// drive unbounded recursion. Kept in lockstep with DecodeObjectRecursive / EncodeObjectBody.
	bool StructInScopeForObject(const UScriptStruct* Struct, int32 Depth)
	{
		if (!Struct || Depth > FCrowdyModelValueCodec::MaxStructDepth)
		{
			return false;
		}
		if (Struct->GetClass() != UScriptStruct::StaticClass()) // excludes UUserDefinedStruct (Blueprint structs)
		{
			return false;
		}
		if (FCrowdyModelValueCodec::IsModelRefStruct(Struct))
		{
			return false;
		}
		int32 MemberCount = 0;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			// An editor-only member does not exist in a cooked build, so ignore it here too: the editor then
			// classifies a struct by exactly the members a packaged client will see, decode, and default.
			if (It->HasAnyPropertyFlags(CPF_EditorOnly))
			{
				continue;
			}
			++MemberCount;
			if (!MemberInScopeForObject(*It, Depth))
			{
				return false;
			}
		}
		return MemberCount > 0;
	}

	// Strictly decodes one object member into TempAddr, rejecting the whole object (false) on a missing or
	// wrong-typed member. Unlike a top-level scalar attribute (which coerces, since a server may send 10.0 for an
	// int), an object member is type-checked like an array element so a malformed member can never silently write
	// a coerced value; arrays and nested objects reuse DecodeValueRecursive, which validates before it writes.
	bool DecodeObjectMemberStrict(const FProperty* Member, void* TempAddr, const TSharedPtr<FJsonValue>& Value, int32 Depth)
	{
		if (!Value.IsValid())
		{
			return false; // the object omitted this member
		}
		if (FCrowdyModelValueCodec::IsSupportedArrayInner(Member)) // a scalar leaf, strictly typed
		{
			return JsonMatchesLeaf(Member, *Value) && WriteScalarLeaf(Member, TempAddr, Value);
		}
		if (CastField<FArrayProperty>(Member) || CastField<FStructProperty>(Member))
		{
			return DecodeValueRecursive(Member, TempAddr, Value, Depth + 1);
		}
		return false;
	}

	// Decodes a JSON object onto a live struct. Decodes into a temporary instance first and commits with
	// CopyScriptStruct only when every member decoded, so a rejected member leaves the live struct untouched
	// (never half-written) - the all-or-nothing rule the array path uses before Resize.
	bool DecodeObjectRecursive(const UScriptStruct* Struct, void* LiveAddr, const TSharedPtr<FJsonValue>& Value, int32 Depth)
	{
		if (Value->Type != EJson::Object)
		{
			UE_LOG(LogCrowdyGameModel, Warning,
				TEXT("[GameModel] object attribute received a non-object value; leaving it unchanged."));
			return false;
		}
		const TSharedPtr<FJsonObject> JsonObject = Value->AsObject();
		if (!JsonObject.IsValid())
		{
			return false;
		}

		FStructOnScope Temp(Struct);
		void* TempMemory = Temp.GetStructMemory();
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FProperty* Member = *It;
			if (Member->HasAnyPropertyFlags(CPF_EditorOnly))
			{
				continue; // stripped in a cooked build; match that here so the JSON member contract is identical
			}
			const FString Key = Member->GetAuthoredName();
			void* MemberAddr = Member->ContainerPtrToValuePtr<void>(TempMemory);
			if (!DecodeObjectMemberStrict(Member, MemberAddr, JsonObject->TryGetField(Key), Depth))
			{
				UE_LOG(LogCrowdyGameModel, Warning,
					TEXT("[GameModel] object attribute member '%s' is missing or the wrong type; rejecting the whole value."),
					*Key);
				return false;
			}
		}
		Struct->CopyScriptStruct(LiveAddr, TempMemory);
		return true;
	}

	// The single recursive write dispatch: a scalar array, a container_ref, a plain-struct object, or a scalar
	// leaf. The public DecodeJsonToProperty calls this at depth 0; object members recurse at depth + 1. Scalars
	// coerce (a server may send 10.0 for an int); arrays and objects validate the whole value before writing.
	bool DecodeValueRecursive(const FProperty* Property, void* ValueAddr, const TSharedPtr<FJsonValue>& Value, int32 Depth)
	{
		if (!Property || !ValueAddr || !Value.IsValid())
		{
			return false;
		}

		if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
		{
			if (!FCrowdyModelValueCodec::IsSupportedArrayInner(ArrayProp->Inner))
			{
				return false; // an array of an unsupported element type is not a Server Owned attribute
			}

			const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
			if (!Value->TryGetArray(JsonArray) || JsonArray == nullptr)
			{
				UE_LOG(LogCrowdyGameModel, Warning,
					TEXT("[GameModel] array attribute '%s' received a non-array value; leaving it unchanged."),
					*Property->GetName());
				return false;
			}
			if (JsonArray->Num() > FCrowdyModelValueCodec::MaxArrayElements)
			{
				UE_LOG(LogCrowdyGameModel, Warning,
					TEXT("[GameModel] array attribute '%s' received %d elements (max %d); rejecting the whole value."),
					*Property->GetName(), JsonArray->Num(), FCrowdyModelValueCodec::MaxArrayElements);
				return false;
			}
			// Validate every element BEFORE touching the live array: a single bad element rejects the whole value,
			// so a malformed response can never leave a half-written TArray.
			for (const TSharedPtr<FJsonValue>& Element : *JsonArray)
			{
				if (!Element.IsValid() || !JsonMatchesLeaf(ArrayProp->Inner, *Element))
				{
					UE_LOG(LogCrowdyGameModel, Warning,
						TEXT("[GameModel] array attribute '%s' has an element of the wrong type; rejecting the whole value."),
						*Property->GetName());
					return false;
				}
			}

			FScriptArrayHelper Helper(ArrayProp, ValueAddr);
			Helper.Resize(JsonArray->Num());
			for (int32 Index = 0; Index < JsonArray->Num(); ++Index)
			{
				WriteScalarLeaf(ArrayProp->Inner, Helper.GetRawPtr(Index), (*JsonArray)[Index]);
			}
			return true;
		}

		if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
		{
			if (FCrowdyModelValueCodec::IsModelRefStruct(StructProp->Struct))
			{
				if (Value->Type != EJson::String)
				{
					UE_LOG(LogCrowdyGameModel, Warning,
						TEXT("[GameModel] container_ref attribute '%s' received a non-string value; leaving it unchanged."),
						*Property->GetName());
					return false;
				}
				static_cast<FCrowdyModelRef*>(ValueAddr)->ModelId = Value->AsString();
				return true;
			}
			if (!StructInScopeForObject(StructProp->Struct, Depth))
			{
				return false;
			}
			return DecodeObjectRecursive(StructProp->Struct, ValueAddr, Value, Depth);
		}

		return WriteScalarLeaf(Property, ValueAddr, Value);
	}

	// Encodes one plain-scalar array to compact JSON text (the body the top-level array default also uses).
	FString EncodeArrayBody(const FArrayProperty* ArrayProp, const void* Addr)
	{
		FScriptArrayHelper Helper(ArrayProp, Addr);
		const int32 Num = Helper.Num();
		FString Out = TEXT("[");
		for (int32 Index = 0; Index < Num; ++Index)
		{
			if (Index > 0)
			{
				Out += TEXT(",");
			}
			Out += EncodeScalarLeaf(ArrayProp->Inner, Helper.GetRawPtr(Index));
		}
		Out += TEXT("]");
		return Out;
	}

	// Encodes a plain-struct object to compact JSON text with SORTED member keys (recursively), so the encoded
	// default is canonical and matches the schema sync's CanonicalizeJsonValue - an object default is then
	// idempotent across a sync regardless of member declaration order.
	FString EncodeObjectBody(const UScriptStruct* Struct, const void* Addr, int32 Depth)
	{
		TArray<TPair<FString, FString>> Members;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FProperty* Member = *It;
			if (Member->HasAnyPropertyFlags(CPF_EditorOnly))
			{
				continue; // not present in a cooked build, so never part of the server default
			}
			const void* MemberAddr = Member->ContainerPtrToValuePtr<void>(Addr);
			Members.Emplace(Member->GetAuthoredName(), EncodeMemberValue(Member, MemberAddr, Depth + 1));
		}
		Members.Sort([](const TPair<FString, FString>& A, const TPair<FString, FString>& B) { return A.Key < B.Key; });
		FString Out = TEXT("{");
		for (int32 Index = 0; Index < Members.Num(); ++Index)
		{
			if (Index > 0)
			{
				Out += TEXT(",");
			}
			Out += FString::Printf(TEXT("\"%s\":%s"), *EscapeJsonStringBody(Members[Index].Key), *Members[Index].Value);
		}
		Out += TEXT("}");
		return Out;
	}

	// Encodes one in-scope object member (a scalar leaf, a scalar array, or a nested plain-struct object). Every
	// member here is already known in scope (StructInScopeForObject validated the whole tree), so a struct member
	// is always an object, never a container_ref.
	FString EncodeMemberValue(const FProperty* Member, const void* Addr, int32 Depth)
	{
		if (const FArrayProperty* Array = CastField<FArrayProperty>(Member))
		{
			return EncodeArrayBody(Array, Addr);
		}
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Member))
		{
			return EncodeObjectBody(StructProp->Struct, Addr, Depth);
		}
		return EncodeScalarLeaf(Member, Addr);
	}
}

bool FCrowdyModelValueCodec::IsModelRefStruct(const UStruct* Struct)
{
	return Struct == FCrowdyModelRef::StaticStruct();
}

bool FCrowdyModelValueCodec::IsSupportedArrayInner(const FProperty* Inner)
{
	return Inner
		&& (CastField<FNumericProperty>(Inner)
			|| CastField<FEnumProperty>(Inner)
			|| CastField<FBoolProperty>(Inner)
			|| CastField<FStrProperty>(Inner));
}

FString FCrowdyModelValueCodec::MapAggregatePropertyToValueType(const FProperty* Property)
{
	if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
	{
		return IsSupportedArrayInner(Array->Inner) ? FString(TEXT("array")) : FString();
	}
	if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		if (IsModelRefStruct(StructProp->Struct))
		{
			return TEXT("container_ref");
		}
		if (StructInScopeForObject(StructProp->Struct, 0))
		{
			return TEXT("object");
		}
		return FString();
	}
	return FString();
}

bool FCrowdyModelValueCodec::DecodeJsonToProperty(const FProperty* Property, void* ValueAddr,
	const TSharedPtr<FJsonValue>& Value)
{
	return DecodeValueRecursive(Property, ValueAddr, Value, 0);
}

bool FCrowdyModelValueCodec::EncodePropertyDefaultToJson(const FProperty* Property, const void* ValueAddr,
	FString& OutJson)
{
	if (!Property || !ValueAddr)
	{
		return false;
	}

	if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
	{
		if (!IsSupportedArrayInner(ArrayProp->Inner))
		{
			return false;
		}
		OutJson = EncodeArrayBody(ArrayProp, ValueAddr);
		return true;
	}

	if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		if (IsModelRefStruct(StructProp->Struct))
		{
			const FCrowdyModelRef* Ref = static_cast<const FCrowdyModelRef*>(ValueAddr);
			if (Ref->ModelId.IsEmpty())
			{
				return false; // an unset reference sends no default
			}
			OutJson = FString::Printf(TEXT("\"%s\""), *EscapeJsonStringBody(Ref->ModelId));
			return true;
		}
		if (StructInScopeForObject(StructProp->Struct, 0))
		{
			OutJson = EncodeObjectBody(StructProp->Struct, ValueAddr, 0);
			return true;
		}
		return false;
	}

	return false;
}
