#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "Templates/Function.h"

class FBoolProperty;
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

/** What a Game Kit hub's message on a topic is. */
enum class ECrowdyExecKitKind : uint8
{
	Snapshot,
	Delta,
	Changed,
	Event
};

/** A Game Kit message's reserved keys; the hub's own data sits beside them in the same map. */
struct FCrowdyExecKitHeader
{
	uint64 Epoch = 0;
	uint64 Seq = 0;
	ECrowdyExecKitKind Kind = ECrowdyExecKitKind::Snapshot;
};

/** One member a Game Kit collection message carried: its props in the Record struct, or that it was removed. */
struct FCrowdyExecKitMember
{
	/** The member's id as decimal text, which is its record id. */
	FString Id;
	FInstancedStruct Values;
	bool bRemoved = false;
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

/** Which message of a type that holds a record per entity is being read. */
enum class ECrowdyExecRecordsMessage : uint8
{
	/** A page of read: every record, replacing what each held. */
	Read,
	/** A state push: changed values over the stored records, and the removed ids. */
	Push,
	/** A page of changes: the restricted values the player may see, over the stored records. */
	Changes
};

/** One record a records message carried. */
struct FCrowdyExecRecordUpdate
{
	FString Id;
	/** The record in the State struct: a read's from the defaults, a push's or changes' from the stored record. */
	FInstancedStruct Values;
	/** Which of the State's fields the message carried, by their index in the field table. */
	TBitArray<> Fields;
	FString Team;
	int64 Owner = 0;
	bool bHasOwner = false;
	bool bHasTeam = false;
};

struct FCrowdyExecRecordsMessage
{
	TArray<FCrowdyExecRecordUpdate> Records;
	TArray<FString> Removed;
	uint64 Epoch = 0;
	uint64 Seq = 0;
	uint64 RSeq = 0;
	bool bRestricted = false;
	bool bMore = false;
};

namespace CrowdyExec
{
	/** The version of the message shapes this client reads; server code built for another is refused. */
	constexpr int32 ContractVersion = 1;

	/** The version of the message shapes of a type that holds a record per entity. */
	constexpr int32 RecordsContractVersion = 2;

	/** The longest record id or team the server keeps, in bytes. */
	constexpr int32 MaxRecordIdBytes = 64;

	constexpr int32 MaxDepth = 16;
	constexpr int32 MaxElements = 4096;
	constexpr int32 MaxStringBytes = 65535;
	constexpr int32 MaxNameChars = 1023;
	/** Elements of every array, set and map in one decoded message together; a map entry counts once. */
	constexpr int32 MaxMessageElements = 65536;

	/** The longest a timer's Seconds or a function's Cooldown may be: 30 days. */
	constexpr int32 MaxWaitSeconds = 2592000;

	/** The most a schedule timer's UTC Offset may shift it, in minutes either way. */
	constexpr int32 MaxUtcOffsetMinutes = 840;

	constexpr int32 MinPushIntervalMs = 50;
	constexpr int32 MaxPushIntervalMs = 5000;
	constexpr int32 MaxAreaSizeChunks = 64;
	constexpr int32 MaxInterestRadiusAreas = 3;

	/** Method names the generated server code uses itself, so no function or timer may take them. */
	inline const TCHAR* const ReservedMethodNames[] = {TEXT("read"), TEXT("on_timer"), TEXT("on_topic"), TEXT("on_presence"), TEXT("on_session"),
		TEXT("on_player_joined"), TEXT("on_player_left")};

	/** The timer the generated server code sends a Record per Entity type's changes with; no timer of any type may take it. */
	inline const TCHAR* const FlushTimerName = TEXT("crowdy_flush");

	/** The timer the generated server code ends the visits of restored player connections with; no timer of any type may take it. */
	inline const TCHAR* const SessionsTimerName = TEXT("crowdy_sessions");

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

	/**
	 * Reads a records message of a type that holds a record per entity. FindStored gives a stored record's values, or
	 * null for an id not held, for a push or changes to decode over. On failure Out is untouched.
	 */
	bool DecodeRecords(const UCrowdyServerObjectDefinition& Definition, ECrowdyExecRecordsMessage Message, TConstArrayView<uint8> Bytes,
		TFunctionRef<const void*(const FString&)> FindStored, FCrowdyExecRecordsMessage& Out, FString& OutError);

	/** Resolves a definition's baked tables against the loaded structs. */
	bool BuildLayout(const UCrowdyServerObjectDefinition& Definition, FCrowdyExecLayout& Out, FString& OutError);

	/** Characters of new names that decoding may add to the name table, which never shrinks, in the whole process; past it a new name reads as None. */
	constexpr int64 MaxNewNameChars = 1024 * 1024;

	/** Reads only the epoch and seq of a read reply or state push, skipping the fields, so they can be checked before a decode. */
	bool ReadStateHeader(TConstArrayView<uint8> Bytes, FCrowdyExecStateHeader& OutHeader, FString& OutError);

	/** The version of the Game Kit's message shapes this client reads. */
	constexpr int32 KitProtocolVersion = 1;

	/** Reads a Game Kit message's v, epoch, seq and kind, skipping the hub's data; a message of another version is refused. */
	bool ReadKitHeader(TConstArrayView<uint8> Bytes, FCrowdyExecKitHeader& OutHeader, FString& OutError);

	/** Decodes a Game Kit snapshot into State, a whole State: what it leaves out takes the definition's values. On failure State is untouched. */
	bool DecodeKitSnapshot(const UCrowdyServerObjectDefinition& Definition, TConstArrayView<uint8> Bytes, void* State,
		FCrowdyExecStateHeader& OutHeader, FString& OutError);

	/**
	 * Reads a Game Kit collection message: a snapshot's members, leaving out those marked removed, or a delta's one member.
	 * Each member's props fill the Record struct from its defaults, since a member carries all of them, and its id fills the struct's
	 * id field when it has one. On failure Out is untouched.
	 */
	bool DecodeKitCollection(const UCrowdyServerObjectDefinition& Definition, TConstArrayView<uint8> Bytes, FCrowdyExecKitHeader& OutHeader,
		TArray<FCrowdyExecKitMember>& OutMembers, FString& OutError);

	/** Reads a Game Kit changed message's who, the user ids it names, and what, the name of the value that changed for them. */
	bool ReadKitChanged(TConstArrayView<uint8> Bytes, TArray<int64>& OutWho, FString& OutWhat, FString& OutError);

	/** Decodes a Game Kit answer into one variable of State, Variable, a property of the definition's State struct, starting from its default; on failure State is untouched. */
	bool DecodeKitVariable(const UCrowdyServerObjectDefinition& Definition, const FProperty& Variable, TConstArrayView<uint8> Bytes, void* State, FString& OutError);

	/** Server text made safe for one line: at most MaxChars characters, control and direction-override characters replaced, a cut marked. */
	FString SafeText(const FString& Text, int32 MaxChars);

	/** Whether Text is the platform's reason for ending a player's access: "access ended: <why>", or "access ended" alone. */
	inline bool IsAccessEndedText(const FString& Text)
	{
		return Text.StartsWith(TEXT("access ended"), ESearchCase::CaseSensitive);
	}

	/** Whether Reason is one AccessEndedReason made. */
	inline bool IsAccessEndedReason(const FString& Reason)
	{
		return Reason.StartsWith(TEXT("Your access ended: "), ESearchCase::CaseSensitive);
	}

	/** The reason an object or call fails with once the player's access ended, from the platform's text or an account block's message. */
	inline FString AccessEndedReason(const FString& ServerText)
	{
		FString Why = ServerText;
		if (IsAccessEndedText(ServerText))
		{
			Why = ServerText.RightChop(FCString::Strlen(TEXT("access ended"))).TrimStart();
			Why.RemoveFromStart(TEXT(":"), ESearchCase::CaseSensitive);
			Why.TrimStartInline();
		}
		return FString::Printf(TEXT("Your access ended: %s"), Why.IsEmpty() ? TEXT("no reason was given") : *SafeText(Why, 256));
	}

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
	/** Sent only to the record's owner or team, for a type that holds a record per entity. */
	bool bRestricted = false;
	/** The struct's bool that says whether this field arrived with a value, when it pairs one; null otherwise. */
	const FBoolProperty* Presence = nullptr;
};

struct FCrowdyExecStruct
{
	const UScriptStruct* Struct = nullptr;
	FString DisplayName;
	TArray<FCrowdyExecField> Fields;
	/** Whether any field has a Presence flag. */
	bool bHasPresence = false;
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
	/** The struct a record holds: a Game Kit starter's Record struct, else the State. */
	int32 RecordStruct = INDEX_NONE;

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
