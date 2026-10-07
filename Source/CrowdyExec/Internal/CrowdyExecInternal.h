#pragma once

#include "CoreMinimal.h"

class FProperty;
struct FCrowdyExecLayout;
class UCrowdyServerObjectDefinition;
class UEnum;
class UScriptStruct;
class UStruct;

enum class ECrowdyExecKind : uint8
{
	Bool,
	Int8,
	Int16,
	Int32,
	Int64,
	UInt8,
	UInt16,
	UInt32,
	UInt64,
	Float,
	Double,
	String,
	Name,
	Enum,
	Struct,
	Array,
	Set,
	Map,
	Optional,
	Vector,
	Vector2D,
	IntPoint,
	IntVector,
	Rotator,
	Quat,
	LinearColor,
	Color,
	DateTime,
	Timespan,
	Guid,
	GameplayTag,
	SoftPath
};

/** Which watched-values message is being read: a full read of every watched value, or a change pushed by the server. */
enum class ECrowdyExecStateMessage : uint8
{
	Read,
	Push
};

struct FCrowdyExecStateHeader
{
	uint64 Epoch = 0;
	uint64 Seq = 0;
};

/** The members keys of a read reply or state push. A key the message did not carry keeps its default and its bHas flag stays false. */
struct FCrowdyExecMembersView
{
	TArray<int64> Members;
	int64 Leader = 0;
	int32 MemberCount = 0;
	bool bOpen = true;
	bool bIsMember = false;
	bool bIsLeader = false;
	bool bHasMembers = false;
	bool bHasLeader = false;
	bool bHasMemberCount = false;
	bool bHasOpen = false;
	bool bHasIsMember = false;
	bool bHasIsLeader = false;
};

namespace CrowdyExec
{
	/** The version of the message shapes this client reads; server code built for another is refused. */
	constexpr int32 ContractVersion = 1;

	constexpr int32 MaxDepth = 16;
	constexpr int32 MaxElements = 4096;
	constexpr int32 MaxStringBytes = 65535;
	constexpr int32 MaxNameChars = 1023;
	/** Elements of every array, set and map in one decoded message together; a map entry counts once. */
	constexpr int32 MaxMessageElements = 65536;

	/** The longest a timer's Seconds or a function's Cooldown may be: 30 days. */
	constexpr int32 MaxWaitSeconds = 2592000;

	/** Method names the generated server code uses itself, so no function or timer may take them. */
	inline const TCHAR* const ReservedMethodNames[] = {TEXT("read"), TEXT("on_timer"), TEXT("on_topic"), TEXT("on_presence"), TEXT("on_session"),
		TEXT("on_player_joined"), TEXT("on_player_left")};

	/** The built-in member functions' methods, taken while Members From is This Object. */
	inline const TCHAR* const BuiltInMemberMethodNames[] = {TEXT("join"), TEXT("leave"), TEXT("add_member"), TEXT("remove_member"), TEXT("make_leader"),
		TEXT("set_open_for_joining")};

	inline bool IsReservedMethodName(const FString& Method, bool bKeepsMembers)
	{
		auto Named = [&Method](const TCHAR* Reserved) { return Method.Equals(Reserved, ESearchCase::CaseSensitive); };
		return MakeArrayView(ReservedMethodNames).ContainsByPredicate(Named) || (bKeepsMembers && MakeArrayView(BuiltInMemberMethodNames).ContainsByPredicate(Named));
	}

	/** A Value Range bound as the editor saves it: an optional '-', digits and at most one '.', with a digit; no '+' and no exponent. */
	inline bool ParseRangeNumber(const FString& Text, double& OutValue)
	{
		int32 Digits = 0;
		int32 Dots = 0;
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			const TCHAR Char = Text[Index];
			const bool bSign = Char == '-' && Index == 0;
			Digits += Char >= '0' && Char <= '9' ? 1 : 0;
			Dots += Char == '.' ? 1 : 0;
			if (!bSign && Char != '.' && (Char < '0' || Char > '9'))
			{
				return false;
			}
		}
		OutValue = Digits > 0 && Dots <= 1 ? FCString::Atod(*Text) : 0.0;
		return Digits > 0 && Dots <= 1 && FMath::IsFinite(OutValue);
	}

	/** A whole-number Value Range bound: an optional '-' and digits, within int64, read exactly. */
	inline bool ParseRangeWhole(const FString& Text, int64& OutValue)
	{
		const bool bNegative = Text.StartsWith(TEXT("-"), ESearchCase::CaseSensitive);
		const int32 Start = bNegative ? 1 : 0;
		const uint64 Limit = bNegative ? static_cast<uint64>(MAX_int64) + 1 : static_cast<uint64>(MAX_int64);
		uint64 Magnitude = 0;
		for (int32 Index = Start; Index < Text.Len(); ++Index)
		{
			const TCHAR Char = Text[Index];
			if (Char < '0' || Char > '9')
			{
				return false;
			}
			const uint64 Digit = static_cast<uint64>(Char - '0');
			if (Magnitude > (Limit - Digit) / 10)
			{
				return false;
			}
			Magnitude = Magnitude * 10 + Digit;
		}
		OutValue = static_cast<int64>(bNegative ? 0 - Magnitude : Magnitude);
		return Text.Len() > Start;
	}

	/** Sorts a property into the kind it travels as, or says why it cannot travel. */
	bool ClassifyProperty(const FProperty* Property, ECrowdyExecKind& OutKind, FString& OutRefusal);

	/** Set elements and map keys must be one of these, so their order on the wire can be fixed. */
	bool IsKeyKind(ECrowdyExecKind Kind);

	/** Properties that exist in every build and so can travel; editor-only, deprecated and transient ones never do. */
	CROWDYEXEC_API bool IsTravellingProperty(const FProperty* Property);

	/** The enum a property of kind Enum reads through. */
	const UEnum* GetPropertyEnum(const FProperty* Property);

	/** A struct's name as its author wrote it, for messages. */
	CROWDYEXEC_API FString DisplayName(const UStruct* Struct);

	/** The characters a default server name keeps from an authored name: ASCII letters, digits and underscores. */
	FString KeepNameCharacters(const FString& Authored);

	/** Whether A and B match once KeepNameCharacters is applied to both, ignoring case, without building either. */
	CROWDYEXEC_API bool SameKeptName(FStringView A, FStringView B);

	/** Struct's travelling field called Name, compared by SameKeptName; null when there is none. */
	CROWDYEXEC_API const FProperty* FindTravellingField(const UStruct* Struct, FName Name);

	uint32 GetStructGeneration();

	/**
	 * Applies a watched-values message to State, an instance of the definition's State struct. A read replaces every
	 * watched value; a push changes only the values it carries. OutFields names the values the message carried, and
	 * OutMembers, when given, receives its members keys. On failure State is untouched.
	 */
	bool DecodeState(const UCrowdyServerObjectDefinition& Definition, ECrowdyExecStateMessage Message,
		TConstArrayView<uint8> Bytes, void* State, FCrowdyExecStateHeader& OutHeader, TArray<FString>& OutFields, FString& OutError,
		FCrowdyExecMembersView* OutMembers = nullptr);

	/** Resolves a definition's baked tables against the loaded structs. */
	bool BuildLayout(const UCrowdyServerObjectDefinition& Definition, FCrowdyExecLayout& Out, FString& OutError);

	/** Characters of new names that decoding may add to the name table, which never shrinks, in the whole process; past it a new name reads as None. */
	constexpr int64 MaxNewNameChars = 1024 * 1024;

	/** Reads only the epoch and seq of a read reply or state push, skipping the fields, so they can be checked before a decode. */
	bool ReadStateHeader(TConstArrayView<uint8> Bytes, FCrowdyExecStateHeader& OutHeader, FString& OutError);

	/** Server text made safe for one line: at most MaxChars characters, control and direction-override characters replaced, a cut marked. */
	FString SafeText(const FString& Text, int32 MaxChars);

#if WITH_DEV_AUTOMATION_TESTS
	/** Forgets which unknown enum names were reported, so a test sees every report again. */
	void ResetUnknownEnumReportsForTest();

	/** Sets how many characters of new names the process has spent, and warns again when it runs out; 0 restores the whole budget. */
	void ResetNewNameBudgetForTest(int64 SpentChars = 0);

	/** How many characters of new names the process has spent. */
	int64 NewNameCharsSpentForTest();
#endif
}

struct FCrowdyExecType
{
	const FProperty* Property = nullptr;
	ECrowdyExecKind Kind = ECrowdyExecKind::Bool;
	/** The nested struct's or the enum's index in the layout. */
	int32 Target = INDEX_NONE;
	/** The element, key, or optional value type. */
	int32 Inner = INDEX_NONE;
	/** A map's value type. */
	int32 Value = INDEX_NONE;
};

struct FCrowdyExecField
{
	FString Name;
	TArray<uint8> Key;
	int32 Type = INDEX_NONE;
	bool bWatched = false;
};

struct FCrowdyExecStruct
{
	const UScriptStruct* Struct = nullptr;
	FString DisplayName;
	TArray<FCrowdyExecField> Fields;
};

struct FCrowdyExecEnum
{
	const UEnum* Enum = nullptr;
	TArray<int64> Values;
	TArray<FString> Names;
	TArray<TArray<uint8>> Keys;
};

struct FCrowdyExecLayout
{
	TArray<FCrowdyExecStruct> Structs;
	TArray<FCrowdyExecEnum> Enums;
	TArray<FCrowdyExecType> Types;
	int32 StateStruct = INDEX_NONE;

	CROWDYEXEC_API int32 FindStruct(const UScriptStruct* Struct) const;
};

/** Writes MessagePack in its smallest forms. Never writes bin, ext, or the 32-bit string and container forms. */
class FCrowdyExecWriter
{
public:
	explicit FCrowdyExecWriter(TArray<uint8>& InOut) : Out(InOut) {}

	void Nil();
	void Bool(bool bValue);
	void Int(int64 Value);
	void UInt(uint64 Value);
	void Float(float Value);
	void Double(double Value);
	/** Callers keep Bytes at or under MaxStringBytes. */
	void String(TConstArrayView<uint8> Bytes);
	void ArrayHeader(int32 Count);
	void MapHeader(int32 Count);

private:
	void BigEndian(uint64 Value, int32 Bytes);

	TArray<uint8>& Out;
};

enum class ECrowdyExecToken : uint8
{
	Nil,
	Bool,
	/** A negative integer; non-negative ones are UInt whatever form they were written in. */
	Int,
	UInt,
	Float,
	String,
	Binary,
	Array,
	Map
};

struct FCrowdyExecToken
{
	ECrowdyExecToken Type = ECrowdyExecToken::Nil;
	bool bBool = false;
	int64 Int = 0;
	uint64 UInt = 0;
	double Float = 0.0;
	/** String or Binary bytes, inside the reader's buffer. */
	const uint8* Data = nullptr;
	/** Byte count for String and Binary, element count for Array and Map. */
	int32 Count = 0;
};

/** Reads untrusted MessagePack one value header at a time, refusing ext and anything past the size limits. */
class CROWDYEXEC_API FCrowdyExecReader
{
public:
	explicit FCrowdyExecReader(TConstArrayView<uint8> InBytes) : Bytes(InBytes) {}

	bool Next(FCrowdyExecToken& Out, FString& OutError);
	/** Skips one whole value, following at most Depth levels of nesting. */
	bool Skip(int32 Depth, FString& OutError);
	bool AtEnd() const { return Offset == Bytes.Num(); }
	int32 Tell() const { return Offset; }

private:
	bool Take(int32 Count, const uint8*& OutData);
	bool ReadBigEndian(int32 Count, uint64& OutValue);

	TConstArrayView<uint8> Bytes;
	int32 Offset = 0;
};
