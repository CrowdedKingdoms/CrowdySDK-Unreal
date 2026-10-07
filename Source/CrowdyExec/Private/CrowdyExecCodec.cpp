#include "CrowdyExecCodec.h"

#include "Algo/Count.h"
#include "CrowdyExecInternal.h"
#include "CrowdyExecLog.h"
#include "CrowdyServerObjectDefinition.h"
#include "GameplayTagContainer.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeLock.h"
#include "Misc/Timespan.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/EnumProperty.h"
#include "UObject/FieldPathProperty.h"
#include "UObject/PropertyOptional.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include <atomic>

namespace
{
	std::atomic<uint32> GStructGeneration{1};

	FCriticalSection GWarnedLock;
	TSet<FString> GWarnedEnumNames;
	bool GWarnedEnumNamesFull = false;
	constexpr int32 MaxWarnedEnumNames = 256;
	constexpr int32 MaxLoggedChars = 64;

	/** Characters of the names decoding has added to the name table, which never shrinks. */
	std::atomic<int64> GNewNameChars{0};
	std::atomic<bool> GWarnedNameBudget{false};

	constexpr int64 UnixEpochTicks = 621355968000000000;
	constexpr int64 TicksPerMs = ETimespan::TicksPerMillisecond;

	struct FCrowdyExecPart
	{
		const ANSICHAR* Name;
		int32 Offset;
		ECrowdyExecKind Kind;
	};

	const FCrowdyExecPart VectorParts[] = {
		{"X", STRUCT_OFFSET(FVector, X), ECrowdyExecKind::Double},
		{"Y", STRUCT_OFFSET(FVector, Y), ECrowdyExecKind::Double},
		{"Z", STRUCT_OFFSET(FVector, Z), ECrowdyExecKind::Double}};
	const FCrowdyExecPart Vector2DParts[] = {
		{"X", STRUCT_OFFSET(FVector2D, X), ECrowdyExecKind::Double},
		{"Y", STRUCT_OFFSET(FVector2D, Y), ECrowdyExecKind::Double}};
	const FCrowdyExecPart IntPointParts[] = {
		{"X", STRUCT_OFFSET(FIntPoint, X), ECrowdyExecKind::Int32},
		{"Y", STRUCT_OFFSET(FIntPoint, Y), ECrowdyExecKind::Int32}};
	const FCrowdyExecPart IntVectorParts[] = {
		{"X", STRUCT_OFFSET(FIntVector, X), ECrowdyExecKind::Int32},
		{"Y", STRUCT_OFFSET(FIntVector, Y), ECrowdyExecKind::Int32},
		{"Z", STRUCT_OFFSET(FIntVector, Z), ECrowdyExecKind::Int32}};
	const FCrowdyExecPart RotatorParts[] = {
		{"Pitch", STRUCT_OFFSET(FRotator, Pitch), ECrowdyExecKind::Double},
		{"Yaw", STRUCT_OFFSET(FRotator, Yaw), ECrowdyExecKind::Double},
		{"Roll", STRUCT_OFFSET(FRotator, Roll), ECrowdyExecKind::Double}};
	const FCrowdyExecPart QuatParts[] = {
		{"X", STRUCT_OFFSET(FQuat, X), ECrowdyExecKind::Double},
		{"Y", STRUCT_OFFSET(FQuat, Y), ECrowdyExecKind::Double},
		{"Z", STRUCT_OFFSET(FQuat, Z), ECrowdyExecKind::Double},
		{"W", STRUCT_OFFSET(FQuat, W), ECrowdyExecKind::Double}};
	const FCrowdyExecPart LinearColorParts[] = {
		{"R", STRUCT_OFFSET(FLinearColor, R), ECrowdyExecKind::Float},
		{"G", STRUCT_OFFSET(FLinearColor, G), ECrowdyExecKind::Float},
		{"B", STRUCT_OFFSET(FLinearColor, B), ECrowdyExecKind::Float},
		{"A", STRUCT_OFFSET(FLinearColor, A), ECrowdyExecKind::Float}};
	// FColor's memory order is B, G, R, A; it travels as R, G, B, A.
	const FCrowdyExecPart ColorParts[] = {
		{"R", STRUCT_OFFSET(FColor, R), ECrowdyExecKind::UInt8},
		{"G", STRUCT_OFFSET(FColor, G), ECrowdyExecKind::UInt8},
		{"B", STRUCT_OFFSET(FColor, B), ECrowdyExecKind::UInt8},
		{"A", STRUCT_OFFSET(FColor, A), ECrowdyExecKind::UInt8}};

	TConstArrayView<FCrowdyExecPart> EngineParts(ECrowdyExecKind Kind)
	{
		switch (Kind)
		{
		case ECrowdyExecKind::Vector: return VectorParts;
		case ECrowdyExecKind::Vector2D: return Vector2DParts;
		case ECrowdyExecKind::IntPoint: return IntPointParts;
		case ECrowdyExecKind::IntVector: return IntVectorParts;
		case ECrowdyExecKind::Rotator: return RotatorParts;
		case ECrowdyExecKind::Quat: return QuatParts;
		case ECrowdyExecKind::LinearColor: return LinearColorParts;
		case ECrowdyExecKind::Color: return ColorParts;
		default: return {};
		}
	}

	struct FIntRange
	{
		int64 Min;
		uint64 Max;
		const TCHAR* Name;
	};

	FIntRange IntRange(ECrowdyExecKind Kind)
	{
		switch (Kind)
		{
		case ECrowdyExecKind::Int8: return {MIN_int8, MAX_int8, TEXT("int8")};
		case ECrowdyExecKind::Int16: return {MIN_int16, MAX_int16, TEXT("int16")};
		case ECrowdyExecKind::Int32: return {MIN_int32, MAX_int32, TEXT("int32")};
		case ECrowdyExecKind::Int64: return {MIN_int64, MAX_int64, TEXT("int64")};
		case ECrowdyExecKind::UInt8: return {0, MAX_uint8, TEXT("uint8")};
		case ECrowdyExecKind::UInt16: return {0, MAX_uint16, TEXT("uint16")};
		case ECrowdyExecKind::UInt32: return {0, MAX_uint32, TEXT("uint32")};
		default: return {0, MAX_uint64, TEXT("uint64")};
		}
	}

	bool IsIntegerKind(ECrowdyExecKind Kind)
	{
		return Kind >= ECrowdyExecKind::Int8 && Kind <= ECrowdyExecKind::UInt64;
	}

	TArray<uint8> ToUtf8(const FString& Text)
	{
		FTCHARToUTF8 Utf8(*Text, Text.Len());
		return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}

	FString FromUtf8(const uint8* Data, int32 Count)
	{
		FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Data), Count);
		return FString::ConstructFromPtrSize(Text.Get(), Text.Length());
	}

	bool BytesEqual(const FCrowdyExecToken& Token, TConstArrayView<uint8> Bytes)
	{
		return Token.Count == Bytes.Num() && FMemory::Memcmp(Token.Data, Bytes.GetData(), Bytes.Num()) == 0;
	}

	bool KeyIs(const FCrowdyExecToken& Token, const ANSICHAR* Literal)
	{
		const int32 Length = FCStringAnsi::Strlen(Literal);
		return Token.Count == Length && FMemory::Memcmp(Token.Data, Literal, Length) == 0;
	}

	const TCHAR* Describe(const FCrowdyExecToken& Token)
	{
		switch (Token.Type)
		{
		case ECrowdyExecToken::Nil: return TEXT("nil");
		case ECrowdyExecToken::Bool: return TEXT("a boolean");
		case ECrowdyExecToken::Int: return TEXT("an integer");
		case ECrowdyExecToken::UInt: return TEXT("an integer");
		case ECrowdyExecToken::Float: return TEXT("a float");
		case ECrowdyExecToken::String: return TEXT("a string");
		case ECrowdyExecToken::Binary: return TEXT("binary");
		case ECrowdyExecToken::Array: return TEXT("an array");
		default: return TEXT("a map");
		}
	}

	FString DepthError()
	{
		return FString::Printf(TEXT("nesting deeper than %d levels"), CrowdyExec::MaxDepth);
	}

	FString ExpectedText(const TCHAR* Expected, const FCrowdyExecToken& Token)
	{
		return FString::Printf(TEXT("expected %s, got %s"), Expected, Describe(Token));
	}

	/** Server text shown in a message. */
	FString Quoted(const FString& Text)
	{
		return CrowdyExec::SafeText(Text, MaxLoggedChars);
	}

	/** Counts a name the name table does not hold yet against the budget; false, and nothing counted, once it is spent. */
	bool AdmitName(FStringView Text)
	{
		if (Text.IsEmpty())
		{
			return true;
		}
		const FName Found(Text, FNAME_Find);
		// "None" in any case is NAME_None, which stores nothing.
		bool bHeld = !Found.IsNone() || Text.Equals(TEXT("None"), ESearchCase::IgnoreCase);
#if WITH_CASE_PRESERVING_NAME
		// A case variant finds the held name, but making it stores the variant's spelling as well.
		bHeld = bHeld && Text.Equals(Found.ToString(), ESearchCase::CaseSensitive);
#endif
		if (bHeld)
		{
			return true;
		}
		if (GNewNameChars.fetch_add(Text.Len()) + Text.Len() <= CrowdyExec::MaxNewNameChars)
		{
			return true;
		}
		GNewNameChars.fetch_sub(Text.Len());
		return false;
	}

	int32 PathPartEnd(FStringView Path, int32 Start)
	{
		int32 End = Start;
		while (End < Path.Len() && Path[End] != TEXT('.') && Path[End] != TEXT(':'))
		{
			++End;
		}
		return End;
	}

	/** FSoftObjectPath::SetPath makes names of the package and asset parts, each ending at the next '.' or ':'. */
	bool AdmitPathNames(FStringView Path)
	{
		const int32 PackageEnd = PathPartEnd(Path, 0);
		return AdmitName(Path.Left(PackageEnd))
			&& (PackageEnd >= Path.Len() || AdmitName(Path.Mid(PackageEnd + 1, PathPartEnd(Path, PackageEnd + 1) - PackageEnd - 1)));
	}

	/** FName None travels as an empty string. */
	FString NameText(FName Name)
	{
		return Name.IsNone() ? FString() : Name.ToString();
	}

	/** Contract is the value the message carried, or null when it carried none. */
	FString ContractError(const FCrowdyExecToken* Contract)
	{
		const FString Format = !Contract ? FString(TEXT("none"))
			: Contract->Type == ECrowdyExecToken::UInt ? FString::Printf(TEXT("%llu"), Contract->UInt) : FString(TEXT("unknown"));
		return FString::Printf(TEXT("the server code was built for another SDK version (message format %s, this client reads %d); regenerate it and deploy it again"),
			*Format, CrowdyExec::ContractVersion);
	}

	int64 GetEnumValue(const FProperty* Property, const void* Address)
	{
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			return EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Address);
		}
		return *static_cast<const uint8*>(Address);
	}

	void SetEnumValue(const FProperty* Property, void* Address, int64 Value)
	{
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(Address, Value);
			return;
		}
		*static_cast<uint8*>(Address) = static_cast<uint8>(Value);
	}

	/** A property value on the heap, for decoding a set element or map entry before it is added. */
	struct FScratchValue
	{
		explicit FScratchValue(const FProperty* InProperty)
			: Property(InProperty)
			, Memory(FMemory::Malloc(InProperty->GetSize(), InProperty->GetMinAlignment()))
		{
			Property->InitializeValue(Memory);
		}

		~FScratchValue()
		{
			Property->DestroyValue(Memory);
			FMemory::Free(Memory);
		}

		void Reset()
		{
			Property->DestroyValue(Memory);
			Property->InitializeValue(Memory);
		}

		const FProperty* Property;
		void* Memory;
	};

	struct FCodecContext
	{
		explicit FCodecContext(const FCrowdyExecLayout& InLayout) : Layout(InLayout) {}

		bool Fail(const FString& InReason)
		{
			Reason = InReason;
			return false;
		}

		bool FailExpected(const TCHAR* Expected, const FCrowdyExecToken& Token)
		{
			return Fail(ExpectedText(Expected, Token));
		}

		FString Error(const FString& Root) const
		{
			return Root + Path + TEXT(": ") + Reason;
		}

		const FCrowdyExecLayout& Layout;
		FString Path;
		FString Reason;
		const FCrowdyExecStruct* Struct = nullptr;
		const FCrowdyExecField* Field = nullptr;
		int32 Elements = 0;
		/** Set when a new name was read as None because the budget is spent; a map or set drops the entry it keys. */
		bool bSkippedName = false;
	};

	/** An enum name this client does not know leaves the value unwritten; the caller keeps the default or drops the entry. */
	enum class ERead : uint8
	{
		Written,
		UnknownEnum,
		Failed
	};

	void WarnUnknownEnumName(const FCodecContext& Ctx, const FCrowdyExecEnum& Enum, const FString& Name)
	{
		const FString Where = Ctx.Struct && Ctx.Field ? Ctx.Struct->DisplayName + TEXT(".") + Ctx.Field->Name : Enum.Enum->GetName();
		const FString Safe = Quoted(Name);
		bool bAlreadyWarned = true;
		bool bJustFilled = false;
		{
			FScopeLock Lock(&GWarnedLock);
			if (GWarnedEnumNames.Num() < MaxWarnedEnumNames)
			{
				GWarnedEnumNames.Add(Where + TEXT("=") + Safe, &bAlreadyWarned);
			}
			else
			{
				bJustFilled = !GWarnedEnumNamesFull;
				GWarnedEnumNamesFull = true;
			}
		}
		UE_CLOG(!bAlreadyWarned, LogCrowdyExec, Warning, TEXT("%s: '%s' is not a value of %s this client knows, so it is ignored."),
			*Where, *Safe, *Enum.Enum->GetName());
		UE_CLOG(bJustFilled, LogCrowdyExec, Warning, TEXT("Further unknown enum values are not reported."));
	}

	void SkipNewName(FCodecContext& Ctx)
	{
		Ctx.bSkippedName = true;
		if (GWarnedNameBudget.exchange(true))
		{
			return;
		}
		UE_LOG(LogCrowdyExec, Warning, TEXT("The server sent more new names than this client accepts, so from now on they read as None and object paths made of them as empty."));
	}

	bool CountElements(FCodecContext& Ctx, int32 Count)
	{
		Ctx.Elements += Count;
		return Ctx.Elements <= CrowdyExec::MaxMessageElements
			|| Ctx.Fail(FString::Printf(TEXT("more than %d elements in one message"), CrowdyExec::MaxMessageElements));
	}

	bool TokenToInt64(FCodecContext& Ctx, const FCrowdyExecToken& Token, int64 Min, int64 Max, const TCHAR* TypeName, int64& OutValue)
	{
		if (Token.Type == ECrowdyExecToken::UInt)
		{
			if (Token.UInt > static_cast<uint64>(Max))
			{
				return Ctx.Fail(FString::Printf(TEXT("%llu is out of range for %s"), Token.UInt, TypeName));
			}
			OutValue = static_cast<int64>(Token.UInt);
			return true;
		}
		if (Token.Type != ECrowdyExecToken::Int)
		{
			return Ctx.FailExpected(TEXT("an integer"), Token);
		}
		if (Token.Int < Min)
		{
			return Ctx.Fail(FString::Printf(TEXT("%lld is out of range for %s"), Token.Int, TypeName));
		}
		OutValue = Token.Int;
		return true;
	}

	bool TokenToReal(FCodecContext& Ctx, const FCrowdyExecToken& Token, bool bSingle, double& OutValue)
	{
		switch (Token.Type)
		{
		case ECrowdyExecToken::Float: OutValue = Token.Float; break;
		case ECrowdyExecToken::UInt: OutValue = static_cast<double>(Token.UInt); break;
		case ECrowdyExecToken::Int: OutValue = static_cast<double>(Token.Int); break;
		default: return Ctx.FailExpected(TEXT("a number"), Token);
		}
		if (!FMath::IsFinite(OutValue))
		{
			return Ctx.Fail(TEXT("NaN and infinity are not accepted"));
		}
		if (bSingle && FMath::Abs(OutValue) > MAX_FLT)
		{
			return Ctx.Fail(FString::Printf(TEXT("%g is out of range for float"), OutValue));
		}
		return true;
	}

	bool TokenToString(FCodecContext& Ctx, const FCrowdyExecToken& Token, FString& OutValue)
	{
		if (Token.Type != ECrowdyExecToken::String)
		{
			return Ctx.FailExpected(TEXT("a string"), Token);
		}
		OutValue = FromUtf8(Token.Data, Token.Count);
		return true;
	}

	ERead ReadEntry(FCodecContext& Ctx, int32 TypeIndex, void* Address, FCrowdyExecReader& Reader, int32 Depth);
	bool ReadWithToken(FCodecContext& Ctx, int32 TypeIndex, void* Address, FCrowdyExecReader& Reader, const FCrowdyExecToken& Token, int32 Depth);

	const FCrowdyExecField* FindField(const FCrowdyExecStruct& Struct, const FCrowdyExecToken& Key)
	{
		for (const FCrowdyExecField& Field : Struct.Fields)
		{
			if (BytesEqual(Key, Field.Key))
			{
				return &Field;
			}
		}
		return nullptr;
	}

	bool ReadStructBody(FCodecContext& Ctx, int32 StructIndex, void* Address, FCrowdyExecReader& Reader, int32 Count, int32 Depth,
		TArray<FString>* OutSeen = nullptr, bool bWatchedOnly = false)
	{
		if (Depth > CrowdyExec::MaxDepth)
		{
			return Ctx.Fail(DepthError());
		}
		const FCrowdyExecStruct& Struct = Ctx.Layout.Structs[StructIndex];
		// Restored on return, so an unknown enum read after this struct is reported against the field that holds it.
		TGuardValue<const FCrowdyExecStruct*> StructGuard(Ctx.Struct, &Struct);
		TGuardValue<const FCrowdyExecField*> FieldGuard(Ctx.Field, nullptr);
		TBitArray<> Named(false, Struct.Fields.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			FCrowdyExecToken Key;
			if (!Reader.Next(Key, Ctx.Reason))
			{
				return false;
			}
			if (Key.Type != ECrowdyExecToken::String)
			{
				return Ctx.FailExpected(TEXT("a field name"), Key);
			}
			const FCrowdyExecField* Field = FindField(Struct, Key);
			const int32 FieldIndex = Field ? static_cast<int32>(Field - Struct.Fields.GetData()) : INDEX_NONE;
			if (Field && Named[FieldIndex])
			{
				return Ctx.Fail(FString::Printf(TEXT("the field %s appears twice"), *Field->Name));
			}
			if (Field)
			{
				Named[FieldIndex] = true;
			}
			if (!Field || (bWatchedOnly && !Field->bWatched))
			{
				if (!Reader.Skip(CrowdyExec::MaxDepth - Depth, Ctx.Reason))
				{
					Ctx.Path = TEXT(".") + Quoted(FromUtf8(Key.Data, Key.Count)) + Ctx.Path;
					return false;
				}
				continue;
			}
			Ctx.Field = Field;
			void* FieldAddress = Ctx.Layout.Types[Field->Type].Property->ContainerPtrToValuePtr<void>(Address);
			const ERead Result = ReadEntry(Ctx, Field->Type, FieldAddress, Reader, Depth + 1);
			if (Result == ERead::Failed)
			{
				Ctx.Path = TEXT(".") + Field->Name + Ctx.Path;
				return false;
			}
			if (OutSeen && Result == ERead::Written)
			{
				OutSeen->Add(Field->Name);
			}
		}
		return true;
	}

	bool ReadEngineStruct(FCodecContext& Ctx, ECrowdyExecKind Kind, void* Address, FCrowdyExecReader& Reader, const FCrowdyExecToken& Token, int32 Depth)
	{
		if (Depth > CrowdyExec::MaxDepth)
		{
			return Ctx.Fail(DepthError());
		}
		if (Token.Type != ECrowdyExecToken::Map)
		{
			return Ctx.FailExpected(TEXT("a map"), Token);
		}
		const TConstArrayView<FCrowdyExecPart> Parts = EngineParts(Kind);
		for (int32 Index = 0; Index < Token.Count; ++Index)
		{
			FCrowdyExecToken Key;
			if (!Reader.Next(Key, Ctx.Reason))
			{
				return false;
			}
			if (Key.Type != ECrowdyExecToken::String)
			{
				return Ctx.FailExpected(TEXT("a field name"), Key);
			}
			const FCrowdyExecPart* Part = Parts.FindByPredicate([&Key](const FCrowdyExecPart& Candidate) { return KeyIs(Key, Candidate.Name); });
			if (!Part)
			{
				if (!Reader.Skip(CrowdyExec::MaxDepth - Depth, Ctx.Reason))
				{
					return false;
				}
				continue;
			}
			FCrowdyExecToken Value;
			if (!Reader.Next(Value, Ctx.Reason))
			{
				return false;
			}
			uint8* PartAddress = static_cast<uint8*>(Address) + Part->Offset;
			bool bRead = false;
			if (Part->Kind == ECrowdyExecKind::Double || Part->Kind == ECrowdyExecKind::Float)
			{
				double Real = 0.0;
				bRead = TokenToReal(Ctx, Value, Part->Kind == ECrowdyExecKind::Float, Real);
				if (bRead && Part->Kind == ECrowdyExecKind::Double)
				{
					*reinterpret_cast<double*>(PartAddress) = Real;
				}
				else if (bRead)
				{
					*reinterpret_cast<float*>(PartAddress) = static_cast<float>(Real);
				}
			}
			else
			{
				const FIntRange Range = IntRange(Part->Kind);
				int64 Integer = 0;
				bRead = TokenToInt64(Ctx, Value, Range.Min, static_cast<int64>(Range.Max), Range.Name, Integer);
				if (bRead && Part->Kind == ECrowdyExecKind::Int32)
				{
					*reinterpret_cast<int32*>(PartAddress) = static_cast<int32>(Integer);
				}
				else if (bRead)
				{
					*PartAddress = static_cast<uint8>(Integer);
				}
			}
			if (!bRead)
			{
				Ctx.Path = TEXT(".") + FString(Part->Name) + Ctx.Path;
				return false;
			}
		}
		return true;
	}

	bool ReadArray(FCodecContext& Ctx, const FCrowdyExecType& Type, void* Address, FCrowdyExecReader& Reader, const FCrowdyExecToken& Token, int32 Depth)
	{
		FScriptArrayHelper Helper(CastFieldChecked<FArrayProperty>(Type.Property), Address);
		const bool bByteArray = Ctx.Layout.Types[Type.Inner].Kind == ECrowdyExecKind::UInt8;
		if (Token.Type == ECrowdyExecToken::Binary && bByteArray)
		{
			if (Token.Count > CrowdyExec::MaxElements)
			{
				return Ctx.Fail(FString::Printf(TEXT("%d elements, over the limit of %d"), Token.Count, CrowdyExec::MaxElements));
			}
			if (!CountElements(Ctx, Token.Count))
			{
				return false;
			}
			Helper.EmptyValues();
			if (Token.Count > 0)
			{
				Helper.AddValues(Token.Count);
				FMemory::Memcpy(Helper.GetRawPtr(0), Token.Data, Token.Count);
			}
			return true;
		}
		if (Token.Type != ECrowdyExecToken::Array)
		{
			return Ctx.FailExpected(TEXT("an array"), Token);
		}
		if (!CountElements(Ctx, Token.Count))
		{
			return false;
		}
		Helper.EmptyValues();
		for (int32 Index = 0; Index < Token.Count; ++Index)
		{
			const int32 Added = Helper.AddValue();
			const ERead Result = ReadEntry(Ctx, Type.Inner, Helper.GetRawPtr(Added), Reader, Depth + 1);
			if (Result == ERead::Failed)
			{
				Ctx.Path = FString::Printf(TEXT("[%d]"), Index) + Ctx.Path;
				return false;
			}
			if (Result == ERead::UnknownEnum)
			{
				Helper.RemoveValues(Added);
			}
		}
		return true;
	}

	FString DescribeDuplicate(const FCodecContext& Ctx, const FCrowdyExecType& KeyType, const void* Key)
	{
		if (KeyType.Kind == ECrowdyExecKind::String || KeyType.Kind == ECrowdyExecKind::Name)
		{
			const FString Text = KeyType.Kind == ECrowdyExecKind::String ? CastFieldChecked<FStrProperty>(KeyType.Property)->GetPropertyValue(Key)
				: NameText(CastFieldChecked<FNameProperty>(KeyType.Property)->GetPropertyValue(Key));
			return FString::Printf(TEXT("two keys collide: '%s' (Unreal compares these keys without case)"), *Quoted(Text));
		}
		if (KeyType.Kind == ECrowdyExecKind::Enum)
		{
			const FCrowdyExecEnum& Enum = Ctx.Layout.Enums[KeyType.Target];
			const int32 Named = Enum.Values.IndexOfByKey(GetEnumValue(KeyType.Property, Key));
			return FString::Printf(TEXT("the key %s appears twice"), Named == INDEX_NONE ? TEXT("?") : *Enum.Names[Named]);
		}
		const FNumericProperty* Numeric = CastFieldChecked<FNumericProperty>(KeyType.Property);
		const FString Value = KeyType.Kind == ECrowdyExecKind::UInt64 ? FString::Printf(TEXT("%llu"), Numeric->GetUnsignedIntPropertyValue(Key))
			: FString::Printf(TEXT("%lld"), Numeric->GetSignedIntPropertyValue(Key));
		return FString::Printf(TEXT("the key %s appears twice"), *Value);
	}

	bool ReadSet(FCodecContext& Ctx, const FCrowdyExecType& Type, void* Address, FCrowdyExecReader& Reader, const FCrowdyExecToken& Token, int32 Depth)
	{
		if (Token.Type != ECrowdyExecToken::Array)
		{
			return Ctx.FailExpected(TEXT("an array"), Token);
		}
		if (!CountElements(Ctx, Token.Count))
		{
			return false;
		}
		const FSetProperty* SetProperty = CastFieldChecked<FSetProperty>(Type.Property);
		FScriptSetHelper Helper(SetProperty, Address);
		Helper.EmptyElements();
		FScratchValue Element(SetProperty->ElementProp);
		for (int32 Index = 0; Index < Token.Count; ++Index)
		{
			Element.Reset();
			Ctx.bSkippedName = false;
			const ERead Result = ReadEntry(Ctx, Type.Inner, Element.Memory, Reader, Depth + 1);
			if (Result == ERead::Failed)
			{
				Ctx.Path = FString::Printf(TEXT("[%d]"), Index) + Ctx.Path;
				return false;
			}
			if (Result == ERead::UnknownEnum || Ctx.bSkippedName)
			{
				continue;
			}
			const int32 Before = Helper.Num();
			Helper.AddElement(Element.Memory);
			if (Helper.Num() == Before)
			{
				return Ctx.Fail(DescribeDuplicate(Ctx, Ctx.Layout.Types[Type.Inner], Element.Memory));
			}
		}
		return true;
	}

	bool ReadMap(FCodecContext& Ctx, const FCrowdyExecType& Type, void* Address, FCrowdyExecReader& Reader, const FCrowdyExecToken& Token, int32 Depth)
	{
		if (Token.Type != ECrowdyExecToken::Map)
		{
			return Ctx.FailExpected(TEXT("a map"), Token);
		}
		if (!CountElements(Ctx, Token.Count))
		{
			return false;
		}
		const FMapProperty* MapProperty = CastFieldChecked<FMapProperty>(Type.Property);
		FScriptMapHelper Helper(MapProperty, Address);
		Helper.EmptyValues();
		FScratchValue Key(MapProperty->KeyProp);
		FScratchValue Value(MapProperty->ValueProp);
		for (int32 Index = 0; Index < Token.Count; ++Index)
		{
			Key.Reset();
			Value.Reset();
			Ctx.bSkippedName = false;
			const ERead KeyRead = ReadEntry(Ctx, Type.Inner, Key.Memory, Reader, Depth + 1);
			const bool bKeySkipped = Ctx.bSkippedName;
			if (KeyRead == ERead::Failed || ReadEntry(Ctx, Type.Value, Value.Memory, Reader, Depth + 1) == ERead::Failed)
			{
				Ctx.Path = FString::Printf(TEXT("[%d]"), Index) + Ctx.Path;
				return false;
			}
			if (KeyRead == ERead::UnknownEnum || bKeySkipped)
			{
				continue;
			}
			const int32 Before = Helper.Num();
			Helper.AddPair(Key.Memory, Value.Memory);
			if (Helper.Num() == Before)
			{
				return Ctx.Fail(DescribeDuplicate(Ctx, Ctx.Layout.Types[Type.Inner], Key.Memory));
			}
		}
		return true;
	}

	ERead ReadEnum(FCodecContext& Ctx, const FCrowdyExecType& Type, void* Address, const FCrowdyExecToken& Token)
	{
		if (Token.Type != ECrowdyExecToken::String)
		{
			Ctx.FailExpected(TEXT("a string"), Token);
			return ERead::Failed;
		}
		const FCrowdyExecEnum& Enum = Ctx.Layout.Enums[Type.Target];
		for (int32 Index = 0; Index < Enum.Keys.Num(); ++Index)
		{
			if (BytesEqual(Token, Enum.Keys[Index]))
			{
				SetEnumValue(Type.Property, Address, Enum.Values[Index]);
				return ERead::Written;
			}
		}
		WarnUnknownEnumName(Ctx, Enum, FromUtf8(Token.Data, Token.Count));
		return ERead::UnknownEnum;
	}

	ERead ReadEntryWithToken(FCodecContext& Ctx, int32 TypeIndex, void* Address, FCrowdyExecReader& Reader, const FCrowdyExecToken& Token, int32 Depth)
	{
		const FCrowdyExecType& Type = Ctx.Layout.Types[TypeIndex];
		if (Type.Kind == ECrowdyExecKind::Enum && Token.Type == ECrowdyExecToken::String)
		{
			return ReadEnum(Ctx, Type, Address, Token);
		}
		return ReadWithToken(Ctx, TypeIndex, Address, Reader, Token, Depth) ? ERead::Written : ERead::Failed;
	}

	ERead ReadEntry(FCodecContext& Ctx, int32 TypeIndex, void* Address, FCrowdyExecReader& Reader, int32 Depth)
	{
		FCrowdyExecToken Token;
		if (!Reader.Next(Token, Ctx.Reason))
		{
			return ERead::Failed;
		}
		return ReadEntryWithToken(Ctx, TypeIndex, Address, Reader, Token, Depth);
	}

	bool ReadTextKind(FCodecContext& Ctx, const FCrowdyExecType& Type, void* Address, const FCrowdyExecToken& Token)
	{
		FString Text;
		if (!TokenToString(Ctx, Token, Text))
		{
			return false;
		}
		switch (Type.Kind)
		{
		case ECrowdyExecKind::String:
			CastFieldChecked<FStrProperty>(Type.Property)->SetPropertyValue(Address, Text);
			return true;
		case ECrowdyExecKind::Name:
			if (Text.Len() > CrowdyExec::MaxNameChars)
			{
				return Ctx.Fail(FString::Printf(TEXT("a name of %d characters, over the limit of %d"), Text.Len(), CrowdyExec::MaxNameChars));
			}
			if (!AdmitName(Text))
			{
				SkipNewName(Ctx);
				Text.Reset();
			}
			CastFieldChecked<FNameProperty>(Type.Property)->SetPropertyValue(Address, FName(*Text));
			return true;
		case ECrowdyExecKind::Guid:
		{
			FGuid Guid;
			if (!FGuid::Parse(Text, Guid))
			{
				return Ctx.Fail(FString::Printf(TEXT("'%s' is not a GUID"), *Quoted(Text)));
			}
			*static_cast<FGuid*>(Address) = Guid;
			return true;
		}
		case ECrowdyExecKind::GameplayTag:
		{
			if (Text.IsEmpty())
			{
				*static_cast<FGameplayTag*>(Address) = FGameplayTag();
				return true;
			}
			// A tag that exists is already a name, so looking it up by FNAME_Find never grows the name table.
			const FName TagName = Text.Len() <= CrowdyExec::MaxNameChars ? FName(*Text, FNAME_Find) : NAME_None;
			const FGameplayTag Tag = TagName.IsNone() ? FGameplayTag() : FGameplayTag::RequestGameplayTag(TagName, false);
			if (!Tag.IsValid())
			{
				return Ctx.Fail(FString::Printf(TEXT("'%s' is not a gameplay tag this build knows"), *Quoted(Text)));
			}
			*static_cast<FGameplayTag*>(Address) = Tag;
			return true;
		}
		default:
		{
			FSoftObjectPath Path;
			// Checked before any path parsing, since a name over the limit is fatal in development builds.
			if (Text.Len() > CrowdyExec::MaxNameChars)
			{
				return Ctx.Fail(FString::Printf(TEXT("an object path of %d characters, over the limit of %d"), Text.Len(), CrowdyExec::MaxNameChars));
			}
			if (!Text.IsEmpty() && !FPackageName::IsValidObjectPath(Text))
			{
				return Ctx.Fail(FString::Printf(TEXT("'%s' is not an object path"), *Quoted(Text)));
			}
			if (!AdmitPathNames(Text))
			{
				SkipNewName(Ctx);
				Text.Reset();
			}
			if (!Text.IsEmpty())
			{
				Path.SetPath(Text);
			}
			if (const FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Type.Property))
			{
				SoftProperty->SetPropertyValue(Address, FSoftObjectPtr(Path));
				return true;
			}
			*static_cast<FSoftObjectPath*>(Address) = Path;
			return true;
		}
		}
	}

	bool ReadWithToken(FCodecContext& Ctx, int32 TypeIndex, void* Address, FCrowdyExecReader& Reader, const FCrowdyExecToken& Token, int32 Depth)
	{
		const FCrowdyExecType& Type = Ctx.Layout.Types[TypeIndex];
		if (Token.Type == ECrowdyExecToken::Nil && Type.Kind != ECrowdyExecKind::Optional)
		{
			return Ctx.Fail(TEXT("nil is accepted only for an optional field"));
		}
		if (IsIntegerKind(Type.Kind))
		{
			const FNumericProperty* Numeric = CastFieldChecked<FNumericProperty>(Type.Property);
			const FIntRange Range = IntRange(Type.Kind);
			if (Type.Kind == ECrowdyExecKind::UInt64 && Token.Type == ECrowdyExecToken::UInt)
			{
				Numeric->SetIntPropertyValue(Address, Token.UInt);
				return true;
			}
			int64 Value = 0;
			const int64 Max = Range.Max > static_cast<uint64>(MAX_int64) ? MAX_int64 : static_cast<int64>(Range.Max);
			if (!TokenToInt64(Ctx, Token, Range.Min, Max, Range.Name, Value))
			{
				return false;
			}
			Numeric->SetIntPropertyValue(Address, Value);
			return true;
		}
		switch (Type.Kind)
		{
		case ECrowdyExecKind::Bool:
			if (Token.Type != ECrowdyExecToken::Bool)
			{
				return Ctx.FailExpected(TEXT("a boolean"), Token);
			}
			CastFieldChecked<FBoolProperty>(Type.Property)->SetPropertyValue(Address, Token.bBool);
			return true;
		case ECrowdyExecKind::Float:
		case ECrowdyExecKind::Double:
		{
			double Value = 0.0;
			if (!TokenToReal(Ctx, Token, Type.Kind == ECrowdyExecKind::Float, Value))
			{
				return false;
			}
			CastFieldChecked<FNumericProperty>(Type.Property)->SetFloatingPointPropertyValue(Address, Value);
			return true;
		}
		case ECrowdyExecKind::Enum:
			return ReadEnum(Ctx, Type, Address, Token) != ERead::Failed;
		case ECrowdyExecKind::Struct:
			if (Token.Type != ECrowdyExecToken::Map)
			{
				return Ctx.FailExpected(TEXT("a map"), Token);
			}
			return ReadStructBody(Ctx, Type.Target, Address, Reader, Token.Count, Depth);
		case ECrowdyExecKind::Array:
		case ECrowdyExecKind::Set:
		case ECrowdyExecKind::Map:
			if (Depth > CrowdyExec::MaxDepth)
			{
				return Ctx.Fail(DepthError());
			}
			if (Type.Kind == ECrowdyExecKind::Array)
			{
				return ReadArray(Ctx, Type, Address, Reader, Token, Depth);
			}
			return Type.Kind == ECrowdyExecKind::Set ? ReadSet(Ctx, Type, Address, Reader, Token, Depth) : ReadMap(Ctx, Type, Address, Reader, Token, Depth);
		case ECrowdyExecKind::Optional:
		{
			const FOptionalProperty* Optional = CastFieldChecked<FOptionalProperty>(Type.Property);
			if (Token.Type == ECrowdyExecToken::Nil)
			{
				Optional->MarkUnset(Address);
				return true;
			}
			const ERead Result = ReadEntryWithToken(Ctx, Type.Inner, Optional->MarkSetAndGetInitializedValuePointerToReplace(Address), Reader, Token, Depth);
			if (Result == ERead::UnknownEnum)
			{
				Optional->MarkUnset(Address);
			}
			return Result != ERead::Failed;
		}
		case ECrowdyExecKind::DateTime:
		case ECrowdyExecKind::Timespan:
		{
			const bool bDate = Type.Kind == ECrowdyExecKind::DateTime;
			const int64 Min = bDate ? -UnixEpochTicks / TicksPerMs : MIN_int64 / TicksPerMs;
			const int64 Max = bDate ? (FDateTime::MaxValue().GetTicks() - UnixEpochTicks) / TicksPerMs : MAX_int64 / TicksPerMs;
			int64 Ms = 0;
			if (!TokenToInt64(Ctx, Token, Min, Max, bDate ? TEXT("a date") : TEXT("a timespan"), Ms))
			{
				return false;
			}
			if (bDate)
			{
				*static_cast<FDateTime*>(Address) = FDateTime(Ms * TicksPerMs + UnixEpochTicks);
				return true;
			}
			*static_cast<FTimespan*>(Address) = FTimespan(Ms * TicksPerMs);
			return true;
		}
		case ECrowdyExecKind::String:
		case ECrowdyExecKind::Name:
		case ECrowdyExecKind::Guid:
		case ECrowdyExecKind::GameplayTag:
		case ECrowdyExecKind::SoftPath:
			return ReadTextKind(Ctx, Type, Address, Token);
		default:
			return ReadEngineStruct(Ctx, Type.Kind, Address, Reader, Token, Depth);
		}
	}

	bool WriteText(FCodecContext& Ctx, FCrowdyExecWriter& Writer, const FString& Text)
	{
		const TArray<uint8> Bytes = ToUtf8(Text);
		if (Bytes.Num() > CrowdyExec::MaxStringBytes)
		{
			return Ctx.Fail(FString::Printf(TEXT("a string of %d bytes, over the limit of %d"), Bytes.Num(), CrowdyExec::MaxStringBytes));
		}
		Writer.String(Bytes);
		return true;
	}

	bool WriteReal(FCodecContext& Ctx, FCrowdyExecWriter& Writer, double Value, bool bSingle)
	{
		if (!FMath::IsFinite(Value))
		{
			return Ctx.Fail(TEXT("NaN and infinity are not accepted"));
		}
		if (bSingle)
		{
			Writer.Float(static_cast<float>(Value));
			return true;
		}
		Writer.Double(Value);
		return true;
	}

	bool WriteValue(FCodecContext& Ctx, int32 TypeIndex, const void* Address, FCrowdyExecWriter& Writer, int32 Depth);

	bool WriteStruct(FCodecContext& Ctx, int32 StructIndex, const void* Address, FCrowdyExecWriter& Writer, int32 Depth, TConstArrayView<FString> OnlyFields = {})
	{
		if (Depth > CrowdyExec::MaxDepth)
		{
			return Ctx.Fail(DepthError());
		}
		const FCrowdyExecStruct& Struct = Ctx.Layout.Structs[StructIndex];
		auto IsIncluded = [OnlyFields](const FCrowdyExecField& Field)
		{
			return OnlyFields.IsEmpty() || OnlyFields.ContainsByPredicate([&Field](const FString& Name) { return Name.Equals(Field.Name, ESearchCase::CaseSensitive); });
		};
		for (const FString& Name : OnlyFields)
		{
			if (!Struct.Fields.ContainsByPredicate([&Name](const FCrowdyExecField& Field) { return Name.Equals(Field.Name, ESearchCase::CaseSensitive); }))
			{
				return Ctx.Fail(FString::Printf(TEXT("there is no field named '%s'"), *Name));
			}
		}
		Writer.MapHeader(Algo::CountIf(Struct.Fields, IsIncluded));
		for (const FCrowdyExecField& Field : Struct.Fields)
		{
			if (!IsIncluded(Field))
			{
				continue;
			}
			Writer.String(Field.Key);
			const void* FieldAddress = Ctx.Layout.Types[Field.Type].Property->ContainerPtrToValuePtr<void>(Address);
			if (!WriteValue(Ctx, Field.Type, FieldAddress, Writer, Depth + 1))
			{
				Ctx.Path = TEXT(".") + Field.Name + Ctx.Path;
				return false;
			}
		}
		return true;
	}

	/** Orders set elements or map keys the way the wire requires: strings by their UTF-8 bytes, integers by value, enums in declaration order. */
	bool SortKeys(FCodecContext& Ctx, const FCrowdyExecType& KeyType, const TArray<const void*>& Keys, TArray<int32>& OutOrder)
	{
		OutOrder.SetNum(Keys.Num());
		for (int32 Index = 0; Index < Keys.Num(); ++Index)
		{
			OutOrder[Index] = Index;
		}
		if (KeyType.Kind == ECrowdyExecKind::UInt64)
		{
			const FNumericProperty* Numeric = CastFieldChecked<FNumericProperty>(KeyType.Property);
			OutOrder.Sort([&](int32 A, int32 B) { return Numeric->GetUnsignedIntPropertyValue(Keys[A]) < Numeric->GetUnsignedIntPropertyValue(Keys[B]); });
			return true;
		}
		if (IsIntegerKind(KeyType.Kind))
		{
			const FNumericProperty* Numeric = CastFieldChecked<FNumericProperty>(KeyType.Property);
			OutOrder.Sort([&](int32 A, int32 B) { return Numeric->GetSignedIntPropertyValue(Keys[A]) < Numeric->GetSignedIntPropertyValue(Keys[B]); });
			return true;
		}
		if (KeyType.Kind == ECrowdyExecKind::Enum)
		{
			// The table lists the values in declaration order, which is how the server's enum orders them.
			const FCrowdyExecEnum& Enum = Ctx.Layout.Enums[KeyType.Target];
			TArray<int32> Rank;
			Rank.Reserve(Keys.Num());
			for (const void* Key : Keys)
			{
				Rank.Add(Enum.Values.IndexOfByKey(GetEnumValue(KeyType.Property, Key)));
				if (Rank.Last() == INDEX_NONE)
				{
					return Ctx.Fail(FString::Printf(TEXT("%lld is not a named value of %s"), GetEnumValue(KeyType.Property, Key), *Enum.Enum->GetName()));
				}
			}
			OutOrder.Sort([&Rank](int32 A, int32 B) { return Rank[A] < Rank[B]; });
			return true;
		}
		TArray<TArray<uint8>> Text;
		Text.Reserve(Keys.Num());
		for (const void* Key : Keys)
		{
			Text.Add(ToUtf8(KeyType.Kind == ECrowdyExecKind::String ? CastFieldChecked<FStrProperty>(KeyType.Property)->GetPropertyValue(Key)
				: NameText(CastFieldChecked<FNameProperty>(KeyType.Property)->GetPropertyValue(Key))));
		}
		OutOrder.Sort([&Text](int32 A, int32 B)
		{
			const int32 Common = FMath::Min(Text[A].Num(), Text[B].Num());
			const int32 Compared = Common > 0 ? FMemory::Memcmp(Text[A].GetData(), Text[B].GetData(), Common) : 0;
			return Compared != 0 ? Compared < 0 : Text[A].Num() < Text[B].Num();
		});
		return true;
	}

	bool WriteKeyed(FCodecContext& Ctx, const FCrowdyExecType& Type, const void* Address, FCrowdyExecWriter& Writer, int32 Depth)
	{
		const bool bSet = Type.Kind == ECrowdyExecKind::Set;
		TArray<const void*> Keys;
		TArray<const void*> Values;
		if (bSet)
		{
			FScriptSetHelper Helper(CastFieldChecked<FSetProperty>(Type.Property), Address);
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (Helper.IsValidIndex(Index))
				{
					Keys.Add(Helper.GetElementPtr(Index));
				}
			}
		}
		else
		{
			FScriptMapHelper Helper(CastFieldChecked<FMapProperty>(Type.Property), Address);
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (Helper.IsValidIndex(Index))
				{
					Keys.Add(Helper.GetKeyPtr(Index));
					Values.Add(Helper.GetValuePtr(Index));
				}
			}
		}
		if (Keys.Num() > CrowdyExec::MaxElements)
		{
			return Ctx.Fail(FString::Printf(TEXT("%d elements, over the limit of %d"), Keys.Num(), CrowdyExec::MaxElements));
		}
		TArray<int32> Order;
		if (!SortKeys(Ctx, Ctx.Layout.Types[Type.Inner], Keys, Order))
		{
			return false;
		}
		if (bSet)
		{
			Writer.ArrayHeader(Keys.Num());
		}
		else
		{
			Writer.MapHeader(Keys.Num());
		}
		for (int32 Index = 0; Index < Order.Num(); ++Index)
		{
			const int32 Entry = Order[Index];
			if (!WriteValue(Ctx, Type.Inner, Keys[Entry], Writer, Depth + 1) || (!bSet && !WriteValue(Ctx, Type.Value, Values[Entry], Writer, Depth + 1)))
			{
				Ctx.Path = FString::Printf(TEXT("[%d]"), Index) + Ctx.Path;
				return false;
			}
		}
		return true;
	}

	bool WriteEngineStruct(FCodecContext& Ctx, ECrowdyExecKind Kind, const void* Address, FCrowdyExecWriter& Writer)
	{
		const TConstArrayView<FCrowdyExecPart> Parts = EngineParts(Kind);
		Writer.MapHeader(Parts.Num());
		for (const FCrowdyExecPart& Part : Parts)
		{
			Writer.String(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Part.Name), FCStringAnsi::Strlen(Part.Name)));
			const uint8* PartAddress = static_cast<const uint8*>(Address) + Part.Offset;
			bool bWritten = true;
			switch (Part.Kind)
			{
			case ECrowdyExecKind::Double: bWritten = WriteReal(Ctx, Writer, *reinterpret_cast<const double*>(PartAddress), false); break;
			case ECrowdyExecKind::Float: bWritten = WriteReal(Ctx, Writer, *reinterpret_cast<const float*>(PartAddress), true); break;
			case ECrowdyExecKind::Int32: Writer.Int(*reinterpret_cast<const int32*>(PartAddress)); break;
			default: Writer.UInt(*PartAddress); break;
			}
			if (!bWritten)
			{
				Ctx.Path = TEXT(".") + FString(Part.Name) + Ctx.Path;
				return false;
			}
		}
		return true;
	}

	bool WriteValue(FCodecContext& Ctx, int32 TypeIndex, const void* Address, FCrowdyExecWriter& Writer, int32 Depth)
	{
		const FCrowdyExecType& Type = Ctx.Layout.Types[TypeIndex];
		if (Type.Kind == ECrowdyExecKind::UInt64)
		{
			Writer.UInt(CastFieldChecked<FNumericProperty>(Type.Property)->GetUnsignedIntPropertyValue(Address));
			return true;
		}
		if (IsIntegerKind(Type.Kind))
		{
			Writer.Int(CastFieldChecked<FNumericProperty>(Type.Property)->GetSignedIntPropertyValue(Address));
			return true;
		}
		switch (Type.Kind)
		{
		case ECrowdyExecKind::Bool:
			Writer.Bool(CastFieldChecked<FBoolProperty>(Type.Property)->GetPropertyValue(Address));
			return true;
		case ECrowdyExecKind::Float:
			return WriteReal(Ctx, Writer, CastFieldChecked<FFloatProperty>(Type.Property)->GetPropertyValue(Address), true);
		case ECrowdyExecKind::Double:
			return WriteReal(Ctx, Writer, CastFieldChecked<FDoubleProperty>(Type.Property)->GetPropertyValue(Address), false);
		case ECrowdyExecKind::String:
			return WriteText(Ctx, Writer, CastFieldChecked<FStrProperty>(Type.Property)->GetPropertyValue(Address));
		case ECrowdyExecKind::Name:
			return WriteText(Ctx, Writer, NameText(CastFieldChecked<FNameProperty>(Type.Property)->GetPropertyValue(Address)));
		case ECrowdyExecKind::Enum:
		{
			const FCrowdyExecEnum& Enum = Ctx.Layout.Enums[Type.Target];
			const int64 Value = GetEnumValue(Type.Property, Address);
			const int32 Named = Enum.Values.IndexOfByKey(Value);
			if (Named == INDEX_NONE)
			{
				return Ctx.Fail(FString::Printf(TEXT("%lld is not a named value of %s"), Value, *Enum.Enum->GetName()));
			}
			Writer.String(Enum.Keys[Named]);
			return true;
		}
		case ECrowdyExecKind::Struct:
			return WriteStruct(Ctx, Type.Target, Address, Writer, Depth);
		case ECrowdyExecKind::Array:
		{
			if (Depth > CrowdyExec::MaxDepth)
			{
				return Ctx.Fail(DepthError());
			}
			FScriptArrayHelper Helper(CastFieldChecked<FArrayProperty>(Type.Property), Address);
			if (Helper.Num() > CrowdyExec::MaxElements)
			{
				return Ctx.Fail(FString::Printf(TEXT("%d elements, over the limit of %d"), Helper.Num(), CrowdyExec::MaxElements));
			}
			Writer.ArrayHeader(Helper.Num());
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				if (!WriteValue(Ctx, Type.Inner, Helper.GetRawPtr(Index), Writer, Depth + 1))
				{
					Ctx.Path = FString::Printf(TEXT("[%d]"), Index) + Ctx.Path;
					return false;
				}
			}
			return true;
		}
		case ECrowdyExecKind::Set:
		case ECrowdyExecKind::Map:
			if (Depth > CrowdyExec::MaxDepth)
			{
				return Ctx.Fail(DepthError());
			}
			return WriteKeyed(Ctx, Type, Address, Writer, Depth);
		case ECrowdyExecKind::Optional:
		{
			const FOptionalProperty* Optional = CastFieldChecked<FOptionalProperty>(Type.Property);
			if (!Optional->IsSet(Address))
			{
				Writer.Nil();
				return true;
			}
			return WriteValue(Ctx, Type.Inner, Optional->GetValuePointerForRead(Address), Writer, Depth);
		}
		case ECrowdyExecKind::DateTime:
			Writer.Int((static_cast<const FDateTime*>(Address)->GetTicks() - UnixEpochTicks) / TicksPerMs);
			return true;
		case ECrowdyExecKind::Timespan:
			Writer.Int(static_cast<const FTimespan*>(Address)->GetTicks() / TicksPerMs);
			return true;
		case ECrowdyExecKind::Guid:
			return WriteText(Ctx, Writer, static_cast<const FGuid*>(Address)->ToString(EGuidFormats::Digits).ToLower());
		case ECrowdyExecKind::GameplayTag:
		{
			const FGameplayTag& Tag = *static_cast<const FGameplayTag*>(Address);
			return WriteText(Ctx, Writer, Tag.IsValid() ? Tag.GetTagName().ToString() : FString());
		}
		case ECrowdyExecKind::SoftPath:
		{
			const FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Type.Property);
			const FSoftObjectPath Path = SoftProperty ? SoftProperty->GetPropertyValue(Address).ToSoftObjectPath() : *static_cast<const FSoftObjectPath*>(Address);
			return WriteText(Ctx, Writer, Path.IsNull() ? FString() : Path.ToString());
		}
		default:
			if (Depth > CrowdyExec::MaxDepth)
			{
				return Ctx.Fail(DepthError());
			}
			return WriteEngineStruct(Ctx, Type.Kind, Address, Writer);
		}
	}

	bool ResolveType(FCrowdyExecLayout& Layout, const FProperty* Property, int32& OutIndex, FString& OutError)
	{
		FCrowdyExecType Type;
		Type.Property = Property;
		if (!CrowdyExec::ClassifyProperty(Property, Type.Kind, OutError))
		{
			return false;
		}
		switch (Type.Kind)
		{
		case ECrowdyExecKind::Struct:
			Type.Target = Layout.FindStruct(CastFieldChecked<FStructProperty>(Property)->Struct);
			if (Type.Target == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("%s is missing from the field tables"), *CrowdyExec::DisplayName(CastFieldChecked<FStructProperty>(Property)->Struct));
				return false;
			}
			break;
		case ECrowdyExecKind::Enum:
		{
			const UEnum* Enum = CrowdyExec::GetPropertyEnum(Property);
			Type.Target = Layout.Enums.IndexOfByPredicate([Enum](const FCrowdyExecEnum& Candidate) { return Candidate.Enum == Enum; });
			if (Type.Target == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("%s is missing from the field tables"), *GetNameSafe(Enum));
				return false;
			}
			break;
		}
		case ECrowdyExecKind::Array:
			if (!ResolveType(Layout, CastFieldChecked<FArrayProperty>(Property)->Inner, Type.Inner, OutError))
			{
				return false;
			}
			break;
		case ECrowdyExecKind::Optional:
			if (!ResolveType(Layout, CastFieldChecked<FOptionalProperty>(Property)->GetValueProperty(), Type.Inner, OutError))
			{
				return false;
			}
			break;
		case ECrowdyExecKind::Set:
			if (!ResolveType(Layout, CastFieldChecked<FSetProperty>(Property)->ElementProp, Type.Inner, OutError))
			{
				return false;
			}
			break;
		case ECrowdyExecKind::Map:
			if (!ResolveType(Layout, CastFieldChecked<FMapProperty>(Property)->KeyProp, Type.Inner, OutError)
				|| !ResolveType(Layout, CastFieldChecked<FMapProperty>(Property)->ValueProp, Type.Value, OutError))
			{
				return false;
			}
			break;
		default:
			break;
		}
		if ((Type.Kind == ECrowdyExecKind::Set || Type.Kind == ECrowdyExecKind::Map) && !CrowdyExec::IsKeyKind(Layout.Types[Type.Inner].Kind))
		{
			OutError = TEXT("set elements and map keys must be strings, names, integers or enums");
			return false;
		}
		OutIndex = Layout.Types.Add(Type);
		return true;
	}

	/** By GUID first, which survives a Blueprint struct rename; by name when the struct has no record of that GUID. */
	/** A struct's name for messages: a List by its own name, never the engine's name for its struct. */
	FString NameIn(const UCrowdyServerObjectDefinition& Definition, const UScriptStruct* Struct)
	{
		const FString List = Definition.FindListName(Struct);
		if (!List.IsEmpty())
		{
			return List;
		}
		return Cast<UPropertyBag>(Struct) ? FString(TEXT("A List changed since it was used")) : CrowdyExec::DisplayName(Struct);
	}

	const FProperty* FindBakedProperty(const UScriptStruct* Struct, const FCrowdyExecBakedField& Field)
	{
		const FName ByGuid = Field.PropertyGuid.IsValid() ? Struct->FindPropertyNameFromGuid(Field.PropertyGuid) : NAME_None;
		const FProperty* Found = ByGuid.IsNone() ? nullptr : FindFProperty<FProperty>(Struct, ByGuid);
		return Found ? Found : FindFProperty<FProperty>(Struct, Field.Property);
	}

	/** Each baked value still has its enumerator, and that enumerator still has the baked value. */
	bool EnumMatchesTable(const FCrowdyExecBakedEnum& Baked)
	{
		for (int32 Index = 0; Index < Baked.Values.Num(); ++Index)
		{
			const int32 Live = Baked.Enum->GetIndexByName(Baked.EnumeratorNames[Index]);
			if (Live == INDEX_NONE || Baked.Enum->GetValueByIndex(Live) != Baked.Values[Index])
			{
				return false;
			}
		}
		return true;
	}

	enum class EEnvelopeKey : uint8
	{
		Contract,
		Epoch,
		Seq,
		Fields,
		MemberCount,
		Leader,
		Open,
		Members,
		IsMember,
		IsLeader,
		Other
	};

	/** In EEnvelopeKey order. */
	const ANSICHAR* const EnvelopeKeyNames[] = {"contract", "epoch", "seq", "fields", "member_count", "leader", "open", "members", "is_member", "is_leader"};

	EEnvelopeKey EnvelopeKeyOf(const FCrowdyExecToken& Key)
	{
		for (int32 Index = 0; Index < static_cast<int32>(EEnvelopeKey::Other); ++Index)
		{
			if (KeyIs(Key, EnvelopeKeyNames[Index]))
			{
				return static_cast<EEnvelopeKey>(Index);
			}
		}
		return EEnvelopeKey::Other;
	}

	bool ReadHeaderValue(FCrowdyExecReader& Reader, uint64& OutValue, FString& OutError)
	{
		FCrowdyExecToken Value;
		if (!Reader.Next(Value, OutError))
		{
			return false;
		}
		if (Value.Type != ECrowdyExecToken::UInt)
		{
			OutError = ExpectedText(TEXT("an unsigned integer"), Value);
			return false;
		}
		OutValue = Value.UInt;
		return true;
	}

	bool ReadEnvelopeUserId(const FCrowdyExecToken& Value, int64& OutId, FString& OutError)
	{
		if (Value.Type != ECrowdyExecToken::UInt)
		{
			OutError = ExpectedText(TEXT("a user id"), Value);
			return false;
		}
		if (Value.UInt > static_cast<uint64>(MAX_int64))
		{
			OutError = TEXT("a user id over the int64 range");
			return false;
		}
		OutId = static_cast<int64>(Value.UInt);
		return true;
	}

	bool ReadEnvelopeFlag(const FCrowdyExecToken& Value, bool& OutFlag, bool& bOutHas, FString& OutError)
	{
		if (Value.Type != ECrowdyExecToken::Bool)
		{
			OutError = ExpectedText(TEXT("a boolean"), Value);
			return false;
		}
		OutFlag = Value.bBool;
		bOutHas = true;
		return true;
	}

	bool ReadEnvelopeMemberList(FCrowdyExecReader& Reader, const FCrowdyExecToken& Value, TArray<int64>& OutMembers, FString& OutError)
	{
		if (Value.Type != ECrowdyExecToken::Array)
		{
			OutError = ExpectedText(TEXT("an array of user ids"), Value);
			return false;
		}
		// The reader refused a count over MaxElements or over the bytes left, so this reserve is bounded.
		OutMembers.Reset(Value.Count);
		for (int32 Index = 0; Index < Value.Count; ++Index)
		{
			FCrowdyExecToken Item;
			if (!Reader.Next(Item, OutError) || !ReadEnvelopeUserId(Item, OutMembers.AddDefaulted_GetRef(), OutError))
			{
				return false;
			}
		}
		return true;
	}

	bool ReadEnvelopeMemberValue(FCrowdyExecReader& Reader, EEnvelopeKey Which, FCrowdyExecMembersView& Out, FString& OutError)
	{
		FCrowdyExecToken Value;
		if (!Reader.Next(Value, OutError))
		{
			return false;
		}
		switch (Which)
		{
		case EEnvelopeKey::MemberCount:
			if (Value.Type != ECrowdyExecToken::UInt || Value.UInt > static_cast<uint64>(MAX_int32))
			{
				OutError = Value.Type == ECrowdyExecToken::UInt ? FString(TEXT("a member count over the int32 range")) : ExpectedText(TEXT("a member count"), Value);
				return false;
			}
			Out.MemberCount = static_cast<int32>(Value.UInt);
			Out.bHasMemberCount = true;
			return true;
		case EEnvelopeKey::Leader:
			Out.bHasLeader = true;
			return Value.Type == ECrowdyExecToken::Nil || ReadEnvelopeUserId(Value, Out.Leader, OutError);
		case EEnvelopeKey::Open:
			return ReadEnvelopeFlag(Value, Out.bOpen, Out.bHasOpen, OutError);
		case EEnvelopeKey::IsMember:
			return ReadEnvelopeFlag(Value, Out.bIsMember, Out.bHasIsMember, OutError);
		case EEnvelopeKey::IsLeader:
			return ReadEnvelopeFlag(Value, Out.bIsLeader, Out.bHasIsLeader, OutError);
		default:
			Out.bHasMembers = true;
			return ReadEnvelopeMemberList(Reader, Value, Out.Members, OutError);
		}
	}

	/** Reads one of the members keys into Out; a wrong type or an id outside int64 refuses the message. */
	bool ReadEnvelopeMemberKey(FCrowdyExecReader& Reader, EEnvelopeKey Which, FCrowdyExecMembersView& Out, FString& OutError)
	{
		if (ReadEnvelopeMemberValue(Reader, Which, Out, OutError))
		{
			return true;
		}
		OutError = FString::Printf(TEXT("%s: %s"), *FString(EnvelopeKeyNames[static_cast<int32>(Which)]), *OutError);
		return false;
	}

	/** Reads a read reply or state push envelope: epoch, seq and the members keys here, contract and fields through the callbacks, other keys skipped. */
	template <typename FReadContract, typename FReadFields>
	bool ReadEnvelope(FCrowdyExecReader& Reader, FCrowdyExecStateHeader& OutHeader, FCrowdyExecMembersView& OutMembers, FString& OutError,
		FReadContract&& ReadContract, FReadFields&& ReadFields)
	{
		FCrowdyExecToken Envelope;
		if (!Reader.Next(Envelope, OutError))
		{
			return false;
		}
		if (Envelope.Type != ECrowdyExecToken::Map)
		{
			OutError = ExpectedText(TEXT("a map"), Envelope);
			return false;
		}
		bool bSeen[static_cast<int32>(EEnvelopeKey::Other) + 1] = {};
		for (int32 Index = 0; Index < Envelope.Count; ++Index)
		{
			FCrowdyExecToken Key;
			if (!Reader.Next(Key, OutError))
			{
				return false;
			}
			if (Key.Type != ECrowdyExecToken::String)
			{
				OutError = ExpectedText(TEXT("a field name"), Key);
				return false;
			}
			const EEnvelopeKey Which = EnvelopeKeyOf(Key);
			if (Which != EEnvelopeKey::Other && bSeen[static_cast<int32>(Which)])
			{
				OutError = FString::Printf(TEXT("the envelope key %s appears twice"), *FString(EnvelopeKeyNames[static_cast<int32>(Which)]));
				return false;
			}
			bSeen[static_cast<int32>(Which)] = true;
			bool bRead = false;
			switch (Which)
			{
			case EEnvelopeKey::Contract: bRead = ReadContract(); break;
			case EEnvelopeKey::Epoch: bRead = ReadHeaderValue(Reader, OutHeader.Epoch, OutError); break;
			case EEnvelopeKey::Seq: bRead = ReadHeaderValue(Reader, OutHeader.Seq, OutError); break;
			case EEnvelopeKey::Fields: bRead = ReadFields(); break;
			case EEnvelopeKey::Other: bRead = Reader.Skip(CrowdyExec::MaxDepth, OutError); break;
			default: bRead = ReadEnvelopeMemberKey(Reader, Which, OutMembers, OutError); break;
			}
			if (!bRead)
			{
				return false;
			}
		}
		if (!Reader.AtEnd())
		{
			OutError = TEXT("unexpected bytes after the message");
			return false;
		}
		if (!bSeen[static_cast<int32>(EEnvelopeKey::Epoch)] || !bSeen[static_cast<int32>(EEnvelopeKey::Seq)])
		{
			OutError = TEXT("the message carries no epoch or seq");
			return false;
		}
		return true;
	}
}

FString CrowdyExec::SafeText(const FString& Text, int32 MaxChars)
{
	FString Safe = Text.Left(MaxChars);
	for (TCHAR& Char : Safe)
	{
		const bool bControl = Char < 0x20 || (Char >= 0x7f && Char <= 0x9f);
		const bool bBidi = (Char >= 0x202a && Char <= 0x202e) || (Char >= 0x2066 && Char <= 0x2069);
		Char = bControl || bBidi ? TCHAR('?') : Char;
	}
	return Text.Len() > MaxChars ? Safe + TEXT("...") : Safe;
}

#if WITH_DEV_AUTOMATION_TESTS
void CrowdyExec::ResetUnknownEnumReportsForTest()
{
	FScopeLock Lock(&GWarnedLock);
	GWarnedEnumNames.Reset();
	GWarnedEnumNamesFull = false;
}

void CrowdyExec::ResetNewNameBudgetForTest(int64 SpentChars)
{
	GNewNameChars.store(SpentChars);
	GWarnedNameBudget.store(false);
}

int64 CrowdyExec::NewNameCharsSpentForTest()
{
	return GNewNameChars.load();
}
#endif

void FCrowdyExecWriter::BigEndian(uint64 Value, int32 Bytes)
{
	for (int32 Shift = (Bytes - 1) * 8; Shift >= 0; Shift -= 8)
	{
		Out.Add(static_cast<uint8>(Value >> Shift));
	}
}

void FCrowdyExecWriter::Nil()
{
	Out.Add(0xc0);
}

void FCrowdyExecWriter::Bool(bool bValue)
{
	Out.Add(bValue ? 0xc3 : 0xc2);
}

void FCrowdyExecWriter::Int(int64 Value)
{
	if (Value >= 0)
	{
		UInt(static_cast<uint64>(Value));
		return;
	}
	if (Value >= -32)
	{
		Out.Add(static_cast<uint8>(Value));
		return;
	}
	const int32 Bytes = Value >= MIN_int8 ? 1 : Value >= MIN_int16 ? 2 : Value >= MIN_int32 ? 4 : 8;
	Out.Add(Bytes == 1 ? 0xd0 : Bytes == 2 ? 0xd1 : Bytes == 4 ? 0xd2 : 0xd3);
	BigEndian(static_cast<uint64>(Value), Bytes);
}

void FCrowdyExecWriter::UInt(uint64 Value)
{
	if (Value <= 0x7f)
	{
		Out.Add(static_cast<uint8>(Value));
		return;
	}
	const int32 Bytes = Value <= MAX_uint8 ? 1 : Value <= MAX_uint16 ? 2 : Value <= MAX_uint32 ? 4 : 8;
	Out.Add(Bytes == 1 ? 0xcc : Bytes == 2 ? 0xcd : Bytes == 4 ? 0xce : 0xcf);
	BigEndian(Value, Bytes);
}

void FCrowdyExecWriter::Float(float Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	Out.Add(0xca);
	BigEndian(Bits, 4);
}

void FCrowdyExecWriter::Double(double Value)
{
	uint64 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	Out.Add(0xcb);
	BigEndian(Bits, 8);
}

void FCrowdyExecWriter::String(TConstArrayView<uint8> Bytes)
{
	const int32 Count = Bytes.Num();
	if (Count <= 31)
	{
		Out.Add(static_cast<uint8>(0xa0 | Count));
	}
	else
	{
		Out.Add(Count <= MAX_uint8 ? 0xd9 : 0xda);
		BigEndian(Count, Count <= MAX_uint8 ? 1 : 2);
	}
	Out.Append(Bytes.GetData(), Count);
}

void FCrowdyExecWriter::ArrayHeader(int32 Count)
{
	if (Count <= 15)
	{
		Out.Add(static_cast<uint8>(0x90 | Count));
		return;
	}
	Out.Add(0xdc);
	BigEndian(Count, 2);
}

void FCrowdyExecWriter::MapHeader(int32 Count)
{
	if (Count <= 15)
	{
		Out.Add(static_cast<uint8>(0x80 | Count));
		return;
	}
	Out.Add(0xde);
	BigEndian(Count, 2);
}

bool FCrowdyExecReader::Take(int32 Count, const uint8*& OutData)
{
	if (Count < 0 || Bytes.Num() - Offset < Count)
	{
		return false;
	}
	OutData = Bytes.GetData() + Offset;
	Offset += Count;
	return true;
}

bool FCrowdyExecReader::ReadBigEndian(int32 Count, uint64& OutValue)
{
	const uint8* Data = nullptr;
	if (!Take(Count, Data))
	{
		return false;
	}
	OutValue = 0;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		OutValue = (OutValue << 8) | Data[Index];
	}
	return true;
}

bool FCrowdyExecReader::Next(FCrowdyExecToken& Out, FString& OutError)
{
	Out = FCrowdyExecToken();
	const TCHAR* Truncated = TEXT("the message ends early");
	const uint8* Head = nullptr;
	if (!Take(1, Head))
	{
		OutError = Truncated;
		return false;
	}
	const uint8 Byte = *Head;
	auto SetSigned = [&Out](int64 Value)
	{
		Out.Type = Value < 0 ? ECrowdyExecToken::Int : ECrowdyExecToken::UInt;
		Out.Int = Value;
		Out.UInt = static_cast<uint64>(Value);
	};

	int32 LengthBytes = 0;
	int32 Length = -1;
	if (Byte <= 0x7f)
	{
		SetSigned(Byte);
		return true;
	}
	if (Byte >= 0xe0)
	{
		SetSigned(static_cast<int8>(Byte));
		return true;
	}
	if ((Byte & 0xf0) == 0x80 || (Byte & 0xf0) == 0x90)
	{
		Out.Type = (Byte & 0xf0) == 0x80 ? ECrowdyExecToken::Map : ECrowdyExecToken::Array;
		Length = Byte & 0x0f;
	}
	if ((Byte & 0xe0) == 0xa0)
	{
		Out.Type = ECrowdyExecToken::String;
		Length = Byte & 0x1f;
	}

	uint64 Raw = 0;
	switch (Byte)
	{
	case 0xc0:
		return true;
	case 0xc2:
	case 0xc3:
		Out.Type = ECrowdyExecToken::Bool;
		Out.bBool = Byte == 0xc3;
		return true;
	case 0xcc:
	case 0xcd:
	case 0xce:
	case 0xcf:
		if (!ReadBigEndian(1 << (Byte - 0xcc), Raw))
		{
			OutError = Truncated;
			return false;
		}
		Out.Type = ECrowdyExecToken::UInt;
		Out.UInt = Raw;
		return true;
	case 0xd0:
	case 0xd1:
	case 0xd2:
	case 0xd3:
	{
		const int32 Width = 1 << (Byte - 0xd0);
		if (!ReadBigEndian(Width, Raw))
		{
			OutError = Truncated;
			return false;
		}
		const int32 Unused = 64 - Width * 8;
		SetSigned(static_cast<int64>(Raw << Unused) >> Unused);
		return true;
	}
	case 0xca:
	{
		if (!ReadBigEndian(4, Raw))
		{
			OutError = Truncated;
			return false;
		}
		const uint32 Bits = static_cast<uint32>(Raw);
		float Value = 0.f;
		FMemory::Memcpy(&Value, &Bits, sizeof(Value));
		Out.Type = ECrowdyExecToken::Float;
		Out.Float = Value;
		return true;
	}
	case 0xcb:
	{
		if (!ReadBigEndian(8, Raw))
		{
			OutError = Truncated;
			return false;
		}
		double Value = 0.0;
		FMemory::Memcpy(&Value, &Raw, sizeof(Value));
		Out.Type = ECrowdyExecToken::Float;
		Out.Float = Value;
		return true;
	}
	case 0xd9: Out.Type = ECrowdyExecToken::String; LengthBytes = 1; break;
	case 0xda: Out.Type = ECrowdyExecToken::String; LengthBytes = 2; break;
	case 0xdb: Out.Type = ECrowdyExecToken::String; LengthBytes = 4; break;
	case 0xc4: Out.Type = ECrowdyExecToken::Binary; LengthBytes = 1; break;
	case 0xc5: Out.Type = ECrowdyExecToken::Binary; LengthBytes = 2; break;
	case 0xc6: Out.Type = ECrowdyExecToken::Binary; LengthBytes = 4; break;
	case 0xdc: Out.Type = ECrowdyExecToken::Array; LengthBytes = 2; break;
	case 0xdd: Out.Type = ECrowdyExecToken::Array; LengthBytes = 4; break;
	case 0xde: Out.Type = ECrowdyExecToken::Map; LengthBytes = 2; break;
	case 0xdf: Out.Type = ECrowdyExecToken::Map; LengthBytes = 4; break;
	default:
		if (Length < 0)
		{
			OutError = Byte == 0xc1 ? FString(TEXT("the reserved byte 0xc1")) : FString(TEXT("an ext value, which is not accepted"));
			return false;
		}
		break;
	}

	if (LengthBytes > 0)
	{
		if (!ReadBigEndian(LengthBytes, Raw))
		{
			OutError = Truncated;
			return false;
		}
		Length = Raw > static_cast<uint64>(MAX_int32) ? MAX_int32 : static_cast<int32>(Raw);
	}
	if (Out.Type == ECrowdyExecToken::Array || Out.Type == ECrowdyExecToken::Map)
	{
		if (Length > CrowdyExec::MaxElements)
		{
			OutError = FString::Printf(TEXT("a container of %d elements, over the limit of %d"), Length, CrowdyExec::MaxElements);
			return false;
		}
		// Every element takes at least one byte, so a larger count is refused before anything is allocated for it.
		if (Length > Bytes.Num() - Offset)
		{
			OutError = FString::Printf(TEXT("a container of %d elements, more than the %d bytes left in the message"), Length, Bytes.Num() - Offset);
			return false;
		}
		Out.Count = Length;
		return true;
	}
	if (Length > CrowdyExec::MaxStringBytes)
	{
		OutError = FString::Printf(TEXT("a string of %d bytes, over the limit of %d"), Length, CrowdyExec::MaxStringBytes);
		return false;
	}
	if (!Take(Length, Out.Data))
	{
		OutError = Truncated;
		return false;
	}
	Out.Count = Length;
	return true;
}

bool FCrowdyExecReader::Skip(int32 Depth, FString& OutError)
{
	FCrowdyExecToken Token;
	if (!Next(Token, OutError))
	{
		return false;
	}
	if (Token.Type != ECrowdyExecToken::Array && Token.Type != ECrowdyExecToken::Map)
	{
		return true;
	}
	if (Depth <= 0)
	{
		OutError = DepthError();
		return false;
	}
	const int32 Items = Token.Type == ECrowdyExecToken::Map ? Token.Count * 2 : Token.Count;
	for (int32 Index = 0; Index < Items; ++Index)
	{
		if (!Skip(Depth - 1, OutError))
		{
			return false;
		}
	}
	return true;
}

int32 FCrowdyExecLayout::FindStruct(const UScriptStruct* Struct) const
{
	return Structs.IndexOfByPredicate([Struct](const FCrowdyExecStruct& Candidate) { return Candidate.Struct == Struct; });
}

bool CrowdyExec::ClassifyProperty(const FProperty* Property, ECrowdyExecKind& OutKind, FString& OutRefusal)
{
	if (Property->ArrayDim > 1)
	{
		OutRefusal = TEXT("fixed-size arrays are not supported; use TArray");
		return false;
	}
	if (CastField<FBoolProperty>(Property)) { OutKind = ECrowdyExecKind::Bool; return true; }
	if (const FByteProperty* Byte = CastField<FByteProperty>(Property))
	{
		OutKind = Byte->Enum ? ECrowdyExecKind::Enum : ECrowdyExecKind::UInt8;
		return true;
	}
	if (CastField<FInt8Property>(Property)) { OutKind = ECrowdyExecKind::Int8; return true; }
	if (CastField<FInt16Property>(Property)) { OutKind = ECrowdyExecKind::Int16; return true; }
	if (CastField<FIntProperty>(Property)) { OutKind = ECrowdyExecKind::Int32; return true; }
	if (CastField<FInt64Property>(Property)) { OutKind = ECrowdyExecKind::Int64; return true; }
	if (CastField<FUInt16Property>(Property)) { OutKind = ECrowdyExecKind::UInt16; return true; }
	if (CastField<FUInt32Property>(Property)) { OutKind = ECrowdyExecKind::UInt32; return true; }
	if (CastField<FUInt64Property>(Property)) { OutKind = ECrowdyExecKind::UInt64; return true; }
	if (CastField<FFloatProperty>(Property)) { OutKind = ECrowdyExecKind::Float; return true; }
	if (CastField<FDoubleProperty>(Property)) { OutKind = ECrowdyExecKind::Double; return true; }
	if (CastField<FStrProperty>(Property)) { OutKind = ECrowdyExecKind::String; return true; }
	if (CastField<FNameProperty>(Property)) { OutKind = ECrowdyExecKind::Name; return true; }
	if (CastField<FEnumProperty>(Property)) { OutKind = ECrowdyExecKind::Enum; return true; }
	if (CastField<FArrayProperty>(Property)) { OutKind = ECrowdyExecKind::Array; return true; }
	if (CastField<FSetProperty>(Property)) { OutKind = ECrowdyExecKind::Set; return true; }
	if (CastField<FMapProperty>(Property)) { OutKind = ECrowdyExecKind::Map; return true; }
	if (CastField<FOptionalProperty>(Property)) { OutKind = ECrowdyExecKind::Optional; return true; }
	if (CastField<FSoftObjectProperty>(Property)) { OutKind = ECrowdyExecKind::SoftPath; return true; }
	if (CastField<FTextProperty>(Property))
	{
		OutRefusal = TEXT("text is not supported; use FString");
		return false;
	}
	if (CastField<FObjectPropertyBase>(Property) || CastField<FInterfaceProperty>(Property))
	{
		OutRefusal = TEXT("object references are not supported; use a soft reference");
		return false;
	}
	if (CastField<FDelegateProperty>(Property) || CastField<FMulticastDelegateProperty>(Property))
	{
		OutRefusal = TEXT("delegates are not supported");
		return false;
	}
	if (CastField<FFieldPathProperty>(Property))
	{
		OutRefusal = TEXT("field paths are not supported");
		return false;
	}
	const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
	if (!StructProperty)
	{
		OutRefusal = FString::Printf(TEXT("%s is not supported"), *Property->GetCPPType());
		return false;
	}

	struct FEngineStruct
	{
		const UScriptStruct* Struct;
		ECrowdyExecKind Kind;
	};
	static const FEngineStruct EngineStructs[] = {
		{TBaseStructure<FVector>::Get(), ECrowdyExecKind::Vector},
		{TBaseStructure<FVector2D>::Get(), ECrowdyExecKind::Vector2D},
		{TBaseStructure<FIntPoint>::Get(), ECrowdyExecKind::IntPoint},
		{TBaseStructure<FIntVector>::Get(), ECrowdyExecKind::IntVector},
		{TBaseStructure<FRotator>::Get(), ECrowdyExecKind::Rotator},
		{TBaseStructure<FQuat>::Get(), ECrowdyExecKind::Quat},
		{TBaseStructure<FLinearColor>::Get(), ECrowdyExecKind::LinearColor},
		{TBaseStructure<FColor>::Get(), ECrowdyExecKind::Color},
		{TBaseStructure<FDateTime>::Get(), ECrowdyExecKind::DateTime},
		{FindObject<UScriptStruct>(nullptr, TEXT("/Script/CoreUObject.Timespan")), ECrowdyExecKind::Timespan},
		{TBaseStructure<FGuid>::Get(), ECrowdyExecKind::Guid},
		{FGameplayTag::StaticStruct(), ECrowdyExecKind::GameplayTag},
		{TBaseStructure<FSoftObjectPath>::Get(), ECrowdyExecKind::SoftPath},
		{TBaseStructure<FSoftClassPath>::Get(), ECrowdyExecKind::SoftPath}};
	for (const FEngineStruct& Engine : EngineStructs)
	{
		if (Engine.Struct == StructProperty->Struct)
		{
			OutKind = Engine.Kind;
			return true;
		}
	}
	if (StructProperty->Struct == FInstancedStruct::StaticStruct())
	{
		OutRefusal = TEXT("FInstancedStruct is not supported");
		return false;
	}
	// Its parent-tag cache is transient, so a decoded container would answer tag queries wrongly.
	if (StructProperty->Struct == FGameplayTagContainer::StaticStruct())
	{
		OutRefusal = TEXT("FGameplayTagContainer is not supported; use TArray<FGameplayTag>");
		return false;
	}
	OutKind = ECrowdyExecKind::Struct;
	return true;
}

bool CrowdyExec::IsKeyKind(ECrowdyExecKind Kind)
{
	return IsIntegerKind(Kind) || Kind == ECrowdyExecKind::String || Kind == ECrowdyExecKind::Name || Kind == ECrowdyExecKind::Enum;
}

bool CrowdyExec::IsTravellingProperty(const FProperty* Property)
{
	return !Property->HasAnyPropertyFlags(CPF_EditorOnly | CPF_Deprecated | CPF_Transient);
}

const UEnum* CrowdyExec::GetPropertyEnum(const FProperty* Property)
{
	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		return EnumProperty->GetEnum();
	}
	const FByteProperty* Byte = CastField<FByteProperty>(Property);
	return Byte ? Byte->Enum.Get() : nullptr;
}

FString CrowdyExec::DisplayName(const UStruct* Struct)
{
	if (!Struct)
	{
		return TEXT("None");
	}
	return Cast<UUserDefinedStruct>(Struct) ? Struct->GetName() : FString(Struct->GetPrefixCPP()) + Struct->GetName();
}

uint32 CrowdyExec::GetStructGeneration()
{
	return GStructGeneration.load();
}

void CrowdyExec::NotifyStructsChanged()
{
	GStructGeneration.fetch_add(1);
}

FInstancedPropertyBag CrowdyExec::ToList(const FInstancedStruct& Values)
{
	FInstancedPropertyBag List;
	const UPropertyBag* Bag = Cast<UPropertyBag>(Values.GetScriptStruct());
	if (!Bag)
	{
		return List;
	}
	List.InitializeFromBagStruct(Bag);
	Bag->CopyScriptStruct(List.GetMutableValue().GetMemory(), Values.GetMemory());
	return List;
}

bool CrowdyExec::BuildLayout(const UCrowdyServerObjectDefinition& Definition, FCrowdyExecLayout& Out, FString& OutError)
{
	if (Definition.BakedStructs.IsEmpty())
	{
		OutError = FString::Printf(TEXT("%s has no field tables; fix the problems it reports and save it"), *Definition.GetName());
		return false;
	}
	for (const FCrowdyExecBakedEnum& Baked : Definition.BakedEnums)
	{
		if (!Baked.Enum || Baked.Values.Num() != Baked.ServerNames.Num() || Baked.Values.Num() != Baked.EnumeratorNames.Num())
		{
			OutError = FString::Printf(TEXT("%s has a damaged enum table; save it again"), *Definition.GetName());
			return false;
		}
		if (!EnumMatchesTable(Baked))
		{
			OutError = FString::Printf(TEXT("%s has changed since %s was saved; save the definition again"), *Baked.Enum->GetName(), *Definition.GetName());
			return false;
		}
		FCrowdyExecEnum& Enum = Out.Enums.AddDefaulted_GetRef();
		Enum.Enum = Baked.Enum;
		Enum.Values = Baked.Values;
		Enum.Names = Baked.ServerNames;
		for (const FString& Name : Baked.ServerNames)
		{
			Enum.Keys.Add(ToUtf8(Name));
		}
	}
	for (const FCrowdyExecBakedStruct& Baked : Definition.BakedStructs)
	{
		const UScriptStruct* Found = Baked.List.IsEmpty() ? Baked.Struct.Get() : Definition.FindListStruct(Baked.List);
		if (!Found)
		{
			OutError = FString::Printf(TEXT("%s names a struct that no longer exists; save it again"), *Definition.GetName());
			return false;
		}
		FCrowdyExecStruct& Struct = Out.Structs.AddDefaulted_GetRef();
		Struct.Struct = Found;
		Struct.DisplayName = Baked.List.IsEmpty() ? DisplayName(Found) : Baked.List;
	}
	for (int32 StructIndex = 0; StructIndex < Definition.BakedStructs.Num(); ++StructIndex)
	{
		const FCrowdyExecBakedStruct& Baked = Definition.BakedStructs[StructIndex];
		const UScriptStruct* Resolved = Out.Structs[StructIndex].Struct;
		const FString& StructName = Out.Structs[StructIndex].DisplayName;
		int32 Travelling = 0;
		for (TFieldIterator<FProperty> It(Resolved); It; ++It)
		{
			Travelling += IsTravellingProperty(*It) ? 1 : 0;
		}
		if (Travelling != Baked.Fields.Num())
		{
			OutError = FString::Printf(TEXT("%s has changed since %s was saved; save the definition again"), *StructName, *Definition.GetName());
			return false;
		}
		for (const FCrowdyExecBakedField& BakedField : Baked.Fields)
		{
			const FProperty* Property = FindBakedProperty(Resolved, BakedField);
			if (!Property)
			{
				OutError = FString::Printf(TEXT("%s has no field %s any more; save %s again"), *StructName, *BakedField.ServerName, *Definition.GetName());
				return false;
			}
			FCrowdyExecField Field;
			Field.Name = BakedField.ServerName;
			Field.Key = ToUtf8(BakedField.ServerName);
			Field.bWatched = BakedField.bWatched;
			FString TypeError;
			if (!ResolveType(Out, Property, Field.Type, TypeError))
			{
				OutError = StructName + TEXT(".") + BakedField.ServerName + TEXT(": ") + TypeError;
				return false;
			}
			Out.Structs[StructIndex].Fields.Add(MoveTemp(Field));
		}
	}
	Out.StateStruct = Out.FindStruct(Definition.GetStateStruct());
	return true;
}

bool CrowdyExec::Encode(const UCrowdyServerObjectDefinition& Definition, const UScriptStruct* Struct, const void* Value,
	TArray<uint8>& OutBytes, FString& OutError, TConstArrayView<FString> OnlyFields)
{
	OutBytes.Reset();
	FCrowdyExecWriter Writer(OutBytes);
	if (!Struct)
	{
		Writer.MapHeader(0);
		return true;
	}
	const FCrowdyExecLayout* Layout = Definition.ResolveLayout(OutError);
	if (!Layout)
	{
		return false;
	}
	const int32 StructIndex = Layout->FindStruct(Struct);
	if (StructIndex == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("%s is not one of %s's structs"), *NameIn(Definition, Struct), *Definition.GetName());
		return false;
	}
	FCodecContext Ctx(*Layout);
	if (!WriteStruct(Ctx, StructIndex, Value, Writer, 1, OnlyFields))
	{
		OutError = Ctx.Error(Layout->Structs[StructIndex].DisplayName);
		OutBytes.Reset();
		return false;
	}
	if (OutBytes.Num() > MaxPayloadBytes)
	{
		OutError = FString::Printf(TEXT("%s: the message is %d bytes, over the limit of %d"), *Layout->Structs[StructIndex].DisplayName, OutBytes.Num(), MaxPayloadBytes);
		OutBytes.Reset();
		return false;
	}
	return true;
}

bool CrowdyExec::Decode(const UCrowdyServerObjectDefinition& Definition, const UScriptStruct& Struct, TConstArrayView<uint8> Bytes,
	void* Value, FString& OutError, const void* Defaults)
{
	if (Bytes.Num() > MaxPayloadBytes)
	{
		OutError = FString::Printf(TEXT("%s: the message is %d bytes, over the limit of %d"), *NameIn(Definition, &Struct), Bytes.Num(), MaxPayloadBytes);
		return false;
	}
	const FCrowdyExecLayout* Layout = Definition.ResolveLayout(OutError);
	if (!Layout)
	{
		return false;
	}
	const int32 StructIndex = Layout->FindStruct(&Struct);
	if (StructIndex == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("%s is not one of %s's structs"), *NameIn(Definition, &Struct), *Definition.GetName());
		return false;
	}
	FInstancedStruct Decoded;
	if (Defaults)
	{
		Decoded.InitializeAs(&Struct, static_cast<const uint8*>(Defaults));
	}
	else
	{
		Definition.InitializeValues(&Struct, Decoded);
	}
	FCodecContext Ctx(*Layout);
	FCrowdyExecReader Reader(Bytes);
	FCrowdyExecToken Token;
	const bool bRead = Reader.Next(Token, Ctx.Reason)
		&& (Token.Type == ECrowdyExecToken::Map || Ctx.FailExpected(TEXT("a map"), Token))
		&& ReadStructBody(Ctx, StructIndex, Decoded.GetMutableMemory(), Reader, Token.Count, 1)
		&& (Reader.AtEnd() || Ctx.Fail(TEXT("unexpected bytes after the message")));
	if (!bRead)
	{
		OutError = Ctx.Error(Layout->Structs[StructIndex].DisplayName);
		return false;
	}
	Struct.CopyScriptStruct(Value, Decoded.GetMemory());
	return true;
}

bool CrowdyExec::ReadStateHeader(TConstArrayView<uint8> Bytes, FCrowdyExecStateHeader& OutHeader, FString& OutError)
{
	if (Bytes.Num() > MaxPayloadBytes)
	{
		OutError = FString::Printf(TEXT("the message is %d bytes, over the limit of %d"), Bytes.Num(), MaxPayloadBytes);
		return false;
	}
	FCrowdyExecReader Reader(Bytes);
	FCrowdyExecStateHeader Header;
	FCrowdyExecMembersView Members;
	auto SkipValue = [&Reader, &OutError]() { return Reader.Skip(MaxDepth, OutError); };
	if (!ReadEnvelope(Reader, Header, Members, OutError, SkipValue, SkipValue))
	{
		return false;
	}
	OutHeader = Header;
	return true;
}

bool CrowdyExec::DecodeState(const UCrowdyServerObjectDefinition& Definition, ECrowdyExecStateMessage Message,
	TConstArrayView<uint8> Bytes, void* State, FCrowdyExecStateHeader& OutHeader, TArray<FString>& OutFields, FString& OutError,
	FCrowdyExecMembersView* OutMembers)
{
	const FCrowdyExecLayout* Layout = Definition.ResolveLayout(OutError);
	if (!Layout)
	{
		return false;
	}
	const UScriptStruct* StateStruct = Definition.GetStateStruct();
	if (!StateStruct || Layout->StateStruct == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("%s has no State struct"), *Definition.GetName());
		return false;
	}
	const FString& StateName = Layout->Structs[Layout->StateStruct].DisplayName;
	if (Layout->Structs[Layout->StateStruct].Struct != StateStruct)
	{
		OutError = FString::Printf(TEXT("%s: the State struct changed after its field tables were built; save it again"), *Definition.GetName());
		return false;
	}
	if (Bytes.Num() > MaxPayloadBytes)
	{
		OutError = FString::Printf(TEXT("%s: the message is %d bytes, over the limit of %d"), *StateName, Bytes.Num(), MaxPayloadBytes);
		return false;
	}
	// A read replaces the whole view, starting from the definition's values so a List keeps the ones players never see.
	FInstancedStruct Decoded;
	if (Message == ECrowdyExecStateMessage::Push)
	{
		Decoded.InitializeAs(StateStruct, static_cast<const uint8*>(State));
	}
	else
	{
		Definition.InitializeValues(StateStruct, Decoded);
	}

	FCodecContext Ctx(*Layout);
	FCrowdyExecReader Reader(Bytes);
	FCrowdyExecStateHeader Header;
	FCrowdyExecMembersView Members;
	TArray<FString> Seen;
	bool bContract = false;
	// Refused on sight: a message of another format could otherwise fail on a later key for a less useful reason.
	auto ReadContract = [&Reader, &Ctx, &bContract]()
	{
		FCrowdyExecToken Value;
		bContract = true;
		return Reader.Next(Value, Ctx.Reason)
			&& ((Value.Type == ECrowdyExecToken::UInt && Value.UInt == static_cast<uint64>(ContractVersion)) || Ctx.Fail(ContractError(&Value)));
	};
	auto ReadFields = [&Reader, &Ctx, &Decoded, &Seen, Layout]()
	{
		FCrowdyExecToken Value;
		return Reader.Next(Value, Ctx.Reason)
			&& (Value.Type == ECrowdyExecToken::Map || Ctx.FailExpected(TEXT("a map"), Value))
			&& ReadStructBody(Ctx, Layout->StateStruct, Decoded.GetMutableMemory(), Reader, Value.Count, 1, &Seen, true);
	};
	const bool bRead = ReadEnvelope(Reader, Header, Members, Ctx.Reason, ReadContract, ReadFields)
		&& (Message == ECrowdyExecStateMessage::Push || bContract || Ctx.Fail(ContractError(nullptr)));
	if (!bRead)
	{
		OutError = Ctx.Error(StateName);
		return false;
	}
	StateStruct->CopyScriptStruct(State, Decoded.GetMemory());
	OutHeader = Header;
	OutFields = MoveTemp(Seen);
	if (OutMembers)
	{
		*OutMembers = MoveTemp(Members);
	}
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
const FProperty* CrowdyExec::FindResolvedPropertyForTest(const UCrowdyServerObjectDefinition& Definition, const UScriptStruct& Struct,
	const FString& ServerName)
{
	FString Error;
	const FCrowdyExecLayout* Layout = Definition.ResolveLayout(Error);
	const int32 StructIndex = Layout ? Layout->FindStruct(&Struct) : INDEX_NONE;
	if (StructIndex == INDEX_NONE)
	{
		return nullptr;
	}
	for (const FCrowdyExecField& Field : Layout->Structs[StructIndex].Fields)
	{
		if (Field.Name.Equals(ServerName, ESearchCase::CaseSensitive))
		{
			return Layout->Types[Field.Type].Property;
		}
	}
	return nullptr;
}
#endif
