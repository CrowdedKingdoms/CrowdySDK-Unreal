#include "CrowdyExecCodegen.h"

#include "Containers/StringConv.h"
#include "CrowdyExecCodec.h"
#include "CrowdyExecInternal.h"
#include "CrowdyExecLog.h"
#include "CrowdyServerObjectDefinition.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/Timespan.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Templates/Function.h"
#include "UObject/Class.h"
#include "UObject/Package.h"

namespace CrowdyExecCodegenDetail
{
	/** Rust 2024's strict and reserved keywords; a name spelled like one is written as a raw identifier. */
	const TCHAR* const Keywords[] = {
		TEXT("abstract"), TEXT("as"), TEXT("async"), TEXT("await"), TEXT("become"), TEXT("box"), TEXT("break"), TEXT("const"),
		TEXT("continue"), TEXT("crate"), TEXT("do"), TEXT("dyn"), TEXT("else"), TEXT("enum"), TEXT("extern"), TEXT("false"),
		TEXT("final"), TEXT("fn"), TEXT("for"), TEXT("gen"), TEXT("if"), TEXT("impl"), TEXT("in"), TEXT("let"), TEXT("loop"),
		TEXT("macro"), TEXT("match"), TEXT("mod"), TEXT("move"), TEXT("mut"), TEXT("override"), TEXT("priv"), TEXT("pub"),
		TEXT("ref"), TEXT("return"), TEXT("self"), TEXT("Self"), TEXT("static"), TEXT("struct"), TEXT("super"), TEXT("trait"),
		TEXT("true"), TEXT("try"), TEXT("type"), TEXT("typeof"), TEXT("unsafe"), TEXT("unsized"), TEXT("use"), TEXT("virtual"),
		TEXT("where"), TEXT("while"), TEXT("yield")};

	/** Keywords that have no raw form. */
	const TCHAR* const UnwritableNames[] = {TEXT("self"), TEXT("Self"), TEXT("super"), TEXT("crate"), TEXT("_")};

	const TCHAR* const ReservedMethods[] = {TEXT("self"), TEXT("super"), TEXT("crate"), TEXT("on_timer"), TEXT("on_topic"), TEXT("on_presence"),
		TEXT("on_session"), TEXT("on_player_joined"), TEXT("on_player_left")};

	/** Names the generated code, the platform's prelude, the standard library or a crate path already use; a type named so would shadow them. */
	const TCHAR* const ReservedTypes[] = {
		TEXT("Option"), TEXT("Result"), TEXT("Vec"), TEXT("String"), TEXT("Box"), TEXT("Some"), TEXT("None"), TEXT("Ok"), TEXT("Err"),
		TEXT("BTreeMap"), TEXT("Default"), TEXT("Clone"), TEXT("Copy"), TEXT("Send"), TEXT("Sync"), TEXT("Sized"), TEXT("Drop"),
		TEXT("Fn"), TEXT("FnMut"), TEXT("FnOnce"), TEXT("Iterator"), TEXT("IntoIterator"), TEXT("Extend"), TEXT("ToString"),
		TEXT("ToOwned"), TEXT("From"), TEXT("Into"), TEXT("TryFrom"), TEXT("TryInto"), TEXT("AsRef"), TEXT("AsMut"),
		TEXT("PartialEq"), TEXT("Eq"), TEXT("PartialOrd"), TEXT("Ord"), TEXT("Debug"), TEXT("Hash"), TEXT("Serialize"),
		TEXT("Deserialize"), TEXT("Self"), TEXT("Hub"), TEXT("Ctx"), TEXT("Call"), TEXT("Caller"), TEXT("Error"), TEXT("Level"),
		TEXT("TopicMsg"), TEXT("Presence"), TEXT("Session"), TEXT("Cron"), TEXT("ChunkPos"), TEXT("Spatial"), TEXT("GridPick"),
		TEXT("VoxelWrite"), TEXT("Schema"), TEXT("PropKind"), TEXT("Visibility"), TEXT("Viewer"), TEXT("Value"), TEXT("Functions"),
		TEXT("ServerObject"), TEXT("SnapshotIn"), TEXT("SnapshotOut"), TEXT("Vector"), TEXT("Vector2D"), TEXT("IntPoint"),
		TEXT("IntVector"), TEXT("Rotator"), TEXT("Quat"), TEXT("LinearColor"), TEXT("Color"), TEXT("BTreeSet"), TEXT("Wire"),
		TEXT("WireKey"), TEXT("Checked"), TEXT("bool"), TEXT("char"), TEXT("str"), TEXT("i8"), TEXT("i16"), TEXT("i32"), TEXT("i64"),
		TEXT("i128"), TEXT("isize"), TEXT("u8"), TEXT("u16"), TEXT("u32"), TEXT("u64"), TEXT("u128"), TEXT("usize"), TEXT("f32"),
		TEXT("f64"), TEXT("std"), TEXT("core"), TEXT("alloc"), TEXT("serde"), TEXT("ckx_sdk"), TEXT("logic"), TEXT("types"), TEXT("Roster"), TEXT("Rule"),
		TEXT("Cooldowns"), TEXT("MemberInputs"), TEXT("OpenInputs"), TEXT("members"), TEXT("timers"), TEXT("calls")};

	/** In ECrowdyExecKind's order: the Unreal type a types.rs field comment names, which the removal check compares. */
	const TCHAR* const KindWords[] = {
		TEXT("Bool"), TEXT("Int8"), TEXT("Int16"), TEXT("Int32"), TEXT("Int64"), TEXT("UInt8"), TEXT("UInt16"), TEXT("UInt32"), TEXT("UInt64"),
		TEXT("Float"), TEXT("Double"), TEXT("String"), TEXT("Name"), TEXT("Enum"), TEXT("Struct"), TEXT("Array"), TEXT("Set"), TEXT("Map"),
		TEXT("Optional"), TEXT("Vector"), TEXT("Vector2D"), TEXT("IntPoint"), TEXT("IntVector"), TEXT("Rotator"), TEXT("Quat"),
		TEXT("LinearColor"), TEXT("Color"), TEXT("DateTime"), TEXT("Timespan"), TEXT("Guid"), TEXT("GameplayTag"), TEXT("SoftPath")};
	static_assert(UE_ARRAY_COUNT(KindWords) == static_cast<int32>(ECrowdyExecKind::SoftPath) + 1, "every kind needs a word");

	const TCHAR* KindWord(ECrowdyExecKind Kind)
	{
		return KindWords[static_cast<int32>(Kind)];
	}

	struct FEnginePart
	{
		const TCHAR* Name;
		ECrowdyExecKind Kind;
	};

	const FEnginePart VectorParts[] = {{TEXT("X"), ECrowdyExecKind::Double}, {TEXT("Y"), ECrowdyExecKind::Double}, {TEXT("Z"), ECrowdyExecKind::Double}};
	const FEnginePart Vector2DParts[] = {{TEXT("X"), ECrowdyExecKind::Double}, {TEXT("Y"), ECrowdyExecKind::Double}};
	const FEnginePart IntPointParts[] = {{TEXT("X"), ECrowdyExecKind::Int32}, {TEXT("Y"), ECrowdyExecKind::Int32}};
	const FEnginePart IntVectorParts[] = {{TEXT("X"), ECrowdyExecKind::Int32}, {TEXT("Y"), ECrowdyExecKind::Int32}, {TEXT("Z"), ECrowdyExecKind::Int32}};
	const FEnginePart RotatorParts[] = {{TEXT("Pitch"), ECrowdyExecKind::Double}, {TEXT("Yaw"), ECrowdyExecKind::Double}, {TEXT("Roll"), ECrowdyExecKind::Double}};
	const FEnginePart QuatParts[] = {
		{TEXT("X"), ECrowdyExecKind::Double}, {TEXT("Y"), ECrowdyExecKind::Double}, {TEXT("Z"), ECrowdyExecKind::Double}, {TEXT("W"), ECrowdyExecKind::Double}};
	const FEnginePart LinearColorParts[] = {
		{TEXT("R"), ECrowdyExecKind::Float}, {TEXT("G"), ECrowdyExecKind::Float}, {TEXT("B"), ECrowdyExecKind::Float}, {TEXT("A"), ECrowdyExecKind::Float}};
	const FEnginePart ColorParts[] = {
		{TEXT("R"), ECrowdyExecKind::UInt8}, {TEXT("G"), ECrowdyExecKind::UInt8}, {TEXT("B"), ECrowdyExecKind::UInt8}, {TEXT("A"), ECrowdyExecKind::UInt8}};

	struct FEngineStruct
	{
		ECrowdyExecKind Kind;
		const TCHAR* Name;
		/** In the order they travel. */
		TConstArrayView<FEnginePart> Parts;
	};

	/** In the order types.rs lists them. */
	const FEngineStruct EngineStructs[] = {
		{ECrowdyExecKind::Vector, TEXT("Vector"), MakeArrayView(VectorParts)},
		{ECrowdyExecKind::Vector2D, TEXT("Vector2D"), MakeArrayView(Vector2DParts)},
		{ECrowdyExecKind::IntPoint, TEXT("IntPoint"), MakeArrayView(IntPointParts)},
		{ECrowdyExecKind::IntVector, TEXT("IntVector"), MakeArrayView(IntVectorParts)},
		{ECrowdyExecKind::Rotator, TEXT("Rotator"), MakeArrayView(RotatorParts)},
		{ECrowdyExecKind::Quat, TEXT("Quat"), MakeArrayView(QuatParts)},
		{ECrowdyExecKind::LinearColor, TEXT("LinearColor"), MakeArrayView(LinearColorParts)},
		{ECrowdyExecKind::Color, TEXT("Color"), MakeArrayView(ColorParts)}};

	const FEngineStruct* FindEngineStruct(ECrowdyExecKind Kind)
	{
		for (const FEngineStruct& Engine : EngineStructs)
		{
			if (Engine.Kind == Kind)
			{
				return &Engine;
			}
		}
		return nullptr;
	}

	bool IsEngineStructName(const FString& Name)
	{
		for (const FEngineStruct& Engine : EngineStructs)
		{
			if (FCString::Strcmp(*Name, Engine.Name) == 0)
			{
				return true;
			}
		}
		return false;
	}

	bool IsOneOf(const FString& Name, TConstArrayView<const TCHAR*> List)
	{
		for (const TCHAR* Entry : List)
		{
			if (FCString::Strcmp(*Name, Entry) == 0)
			{
				return true;
			}
		}
		return false;
	}

	/** A wire name as a Rust identifier, raw when it is a keyword. False for the keywords that have no raw form. */
	bool RustIdent(const FString& Name, FString& OutIdent)
	{
		if (IsOneOf(Name, UnwritableNames))
		{
			return false;
		}
		OutIdent = IsOneOf(Name, Keywords) ? TEXT("r#") + Name : Name;
		return true;
	}

	FString UnwritableNameError(const FString& Owner, const FString& Name)
	{
		return FString::Printf(TEXT("%s.%s: '%s' cannot be a name in the server code (Rust); set a server name for it on the definition"), *Owner, *Name, *Name);
	}

	bool IsAsciiDigit(TCHAR Char)
	{
		return Char >= '0' && Char <= '9';
	}

	bool IsAsciiUpper(TCHAR Char)
	{
		return Char >= 'A' && Char <= 'Z';
	}

	FString FilterTypeName(const FString& Raw)
	{
		FString Name;
		for (const TCHAR Char : Raw)
		{
			if (IsAsciiDigit(Char) || IsAsciiUpper(Char) || (Char >= 'a' && Char <= 'z') || Char == '_')
			{
				Name.AppendChar(Char);
			}
		}
		return Name.IsEmpty() || IsAsciiDigit(Name[0]) ? TEXT("T") + Name : Name;
	}

	bool IsTypeNameFree(const FString& Name, const TArray<FString>& Taken)
	{
		const bool bReserved = IsOneOf(Name, ReservedTypes) || IsOneOf(Name, Keywords) || IsOneOf(Name, UnwritableNames);
		return !bReserved && !Taken.ContainsByPredicate([&Name](const FString& Other) { return Other.Equals(Name, ESearchCase::CaseSensitive); });
	}

	FString ClaimTypeName(const FString& Base, TArray<FString>& Taken)
	{
		FString Name = Base;
		for (int32 Attempt = 1; !IsTypeNameFree(Name, Taken); ++Attempt)
		{
			Name = Attempt == 1 ? Base + TEXT("Type") : FString::Printf(TEXT("%sType%d"), *Base, Attempt);
		}
		Taken.Add(Name);
		return Name;
	}

	FString TextOf(const FCrowdyExecToken& Token)
	{
		FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Token.Data), Token.Count);
		return FString::ConstructFromPtrSize(Text.Get(), Text.Length());
	}

	bool KeyIs(const FCrowdyExecToken& Token, TConstArrayView<uint8> Key)
	{
		return Token.Type == ECrowdyExecToken::String && Token.Count == Key.Num() && FMemory::Memcmp(Token.Data, Key.GetData(), Key.Num()) == 0;
	}

	/** Reads one code point and advances Index; a lone surrogate, which Rust cannot spell, reads as U+FFFD. */
	uint32 NextCodePoint(const FString& Text, int32& Index)
	{
		const uint32 Code = static_cast<uint32>(Text[Index++]);
		if (StringConv::IsHighSurrogate(Code) && Index < Text.Len() && StringConv::IsLowSurrogate(static_cast<uint32>(Text[Index])))
		{
			return StringConv::EncodeSurrogate(static_cast<uint16>(Code), static_cast<uint16>(Text[Index++]));
		}
		return StringConv::IsHighSurrogate(Code) || StringConv::IsLowSurrogate(Code) ? 0xFFFD : Code;
	}

	/** Code points outside printable ASCII are escaped, so the generated files are plain ASCII. */
	FString EscapeRustText(const FString& Text)
	{
		FString Escaped;
		for (int32 Index = 0; Index < Text.Len();)
		{
			const uint32 Code = NextCodePoint(Text, Index);
			if (Code < 0x20 || Code >= 0x7f)
			{
				Escaped += FString::Printf(TEXT("\\u{%X}"), Code);
				continue;
			}
			if (Code == '\\' || Code == '"')
			{
				Escaped.AppendChar(TEXT('\\'));
			}
			Escaped.AppendChar(static_cast<TCHAR>(Code));
		}
		return Escaped;
	}

	FString RustStringLiteral(const FString& Text)
	{
		return Text.IsEmpty() ? FString(TEXT("String::new()")) : TEXT("String::from(\"") + EscapeRustText(Text) + TEXT("\")");
	}

	/** A &str literal. */
	FString RustStrLiteral(const FString& Text)
	{
		return TEXT("\"") + EscapeRustText(Text) + TEXT("\"");
	}

	/** The inside of a TOML basic string. */
	FString TomlEscape(const FString& Text)
	{
		FString Escaped;
		for (const TCHAR Char : Text)
		{
			if (Char < 0x20 || Char == 0x7f)
			{
				Escaped += FString::Printf(TEXT("\\u%04X"), static_cast<uint32>(Char));
				continue;
			}
			if (Char == TEXT('\\') || Char == TEXT('"'))
			{
				Escaped.AppendChar(TEXT('\\'));
			}
			Escaped.AppendChar(Char);
		}
		return Escaped;
	}

	/** Reads back what TomlEscape writes. */
	FString TomlUnescape(const FString& Text)
	{
		FString Unescaped;
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			if (Text[Index] != TEXT('\\') || Index + 1 >= Text.Len())
			{
				Unescaped.AppendChar(Text[Index]);
				continue;
			}
			const TCHAR Next = Text[++Index];
			if (Next == TEXT('u') && Index + 4 < Text.Len())
			{
				Unescaped.AppendChar(static_cast<TCHAR>(FParse::HexNumber(*Text.Mid(Index + 1, 4))));
				Index += 4;
				continue;
			}
			Unescaped.AppendChar(Next);
		}
		return Unescaped;
	}

	/** Prefixes every line that is not empty. */
	FString IndentLines(const FString& Text, const TCHAR* Prefix)
	{
		TArray<FString> Lines;
		Text.ParseIntoArray(Lines, TEXT("\n"), false);
		FString Out;
		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			if (Index == Lines.Num() - 1 && Lines[Index].IsEmpty())
			{
				break;
			}
			Out += (Lines[Index].IsEmpty() ? FString() : Prefix + Lines[Index]) + TEXT("\n");
		}
		return Out;
	}

	bool RealLiteral(double Value, bool bSingle, FString& Out)
	{
		if (!FMath::IsFinite(Value))
		{
			return false;
		}
		Out = bSingle ? FString::Printf(TEXT("%.9g"), Value) : FString::Printf(TEXT("%.17g"), Value);
		int32 Found = INDEX_NONE;
		if (!Out.FindChar(TEXT('.'), Found) && !Out.FindChar(TEXT('e'), Found) && !Out.FindChar(TEXT('E'), Found))
		{
			Out += TEXT(".0");
		}
		return true;
	}

	bool IntegerLiteral(const FCrowdyExecToken& Token, FString& Out)
	{
		if (Token.Type == ECrowdyExecToken::UInt)
		{
			Out = FString::Printf(TEXT("%llu"), Token.UInt);
			return true;
		}
		if (Token.Type != ECrowdyExecToken::Int)
		{
			return false;
		}
		Out = Token.Int == MIN_int64 ? FString(TEXT("i64::MIN")) : FString::Printf(TEXT("%lld"), Token.Int);
		return true;
	}

	const TCHAR* ScalarRustType(ECrowdyExecKind Kind)
	{
		switch (Kind)
		{
		case ECrowdyExecKind::Bool: return TEXT("bool");
		case ECrowdyExecKind::Int8: return TEXT("i8");
		case ECrowdyExecKind::Int16: return TEXT("i16");
		case ECrowdyExecKind::Int32: return TEXT("i32");
		case ECrowdyExecKind::Int64: return TEXT("i64");
		case ECrowdyExecKind::UInt8: return TEXT("u8");
		case ECrowdyExecKind::UInt16: return TEXT("u16");
		case ECrowdyExecKind::UInt32: return TEXT("u32");
		case ECrowdyExecKind::UInt64: return TEXT("u64");
		case ECrowdyExecKind::Float: return TEXT("f32");
		case ECrowdyExecKind::Double: return TEXT("f64");
		case ECrowdyExecKind::DateTime: return TEXT("i64");
		case ECrowdyExecKind::Timespan: return TEXT("i64");
		default: break;
		}
		const FEngineStruct* Engine = FindEngineStruct(Kind);
		return Engine ? Engine->Name : TEXT("String");
	}

	/** The glue function checking a kind Rust holds as String or i64 the way the client reads it; null for the other kinds. */
	const TCHAR* KindCheckFunction(ECrowdyExecKind Kind)
	{
		switch (Kind)
		{
		case ECrowdyExecKind::Name: return TEXT("check_name");
		case ECrowdyExecKind::Guid: return TEXT("check_guid");
		case ECrowdyExecKind::GameplayTag: return TEXT("check_tag");
		case ECrowdyExecKind::SoftPath: return TEXT("check_path");
		case ECrowdyExecKind::DateTime: return TEXT("check_date");
		case ECrowdyExecKind::Timespan: return TEXT("check_timespan");
		default: return nullptr;
		}
	}

	static_assert(CrowdyExec::MaxMessageElements == 65536, "the glue's MAX_MESSAGE_ELEMENTS must be the client's budget");

	const TCHAR* const PushElementsRefusal = TEXT("unsendable(\"the watched values\", \"together they hold more than 65536 elements\")");

	/** Two Rust Checked expressions run in order; either may be empty. */
	FString ThenCheck(const FString& First, const FString& Second)
	{
		if (First.IsEmpty() || Second.IsEmpty())
		{
			return First + Second;
		}
		return FString::Printf(TEXT("%s.and_then(|()| %s)"), *First, *Second);
	}

	/** Check run on each element of Value bound as Binding; empty when Check is. */
	FString EachCheck(const FString& Value, const TCHAR* Binding, const FString& Check)
	{
		return Check.IsEmpty() ? Check : FString::Printf(TEXT("%s.iter().try_for_each(|%s| %s)"), *Value, Binding, *Check);
	}

	FString BraceLiteral(const FString& Name, const TArray<FString>& Parts)
	{
		return Parts.IsEmpty() ? Name + TEXT(" {}") : Name + TEXT(" { ") + FString::Join(Parts, TEXT(", ")) + TEXT(" }");
	}

	struct FFunctionInfo
	{
		FString Name;
		FString Method;
		FString Ident;
		FString Params;
		FString Reply;
		ECrowdyServerFunctionCaller WhoCanCall = ECrowdyServerFunctionCaller::Players;
		int64 CooldownMs = 0;
		/** Each Value Range check on the params, as dispatch-arm lines. */
		TArray<FString> RangeChecks;
	};

	/** A struct's starting values encoded from two fresh instances, which differ when an initializer is random or time-based; Again is empty when the second cannot be encoded. */
	struct FDefaultBytes
	{
		TArray<uint8> Bytes;
		TArray<uint8> Again;
	};

	/** A List sharing its struct with an earlier List but starting with other values, written as a struct of its own under its name. */
	struct FListStruct
	{
		FString Name;
		int32 StructIndex = INDEX_NONE;
		FDefaultBytes Defaults;
	};

	struct FTimerInfo
	{
		/** The name the platform fires it by: the timer's Name. */
		FString Name;
		FString Method;
		FString Ident;
		/** The timers module's constant holding Name: END_MATCH. */
		FString Constant;
		bool bEvery = true;
		int64 Ms = 0;
		bool bAutomatic = true;
	};

	/** The timers module's own static, which no timer's constant may take. */
	const TCHAR* const TimerQueueStatic = TEXT("QUEUED");

	/** Functions the trait gives a body, so a logic file may define them or not. */
	const TCHAR* const ProvidedMethods[] = {TEXT("on_timer"), TEXT("on_topic"), TEXT("on_presence")};

	/** A built-in member function as the glue dispatches it; Before runs on the lent members before Result is made. */
	struct FBuiltinMember
	{
		const TCHAR* Method;
		const TCHAR* Inputs;
		const TCHAR* Rule;
		const TCHAR* Before;
		const TCHAR* Result;
	};

	const FBuiltinMember BuiltinMembers[] = {
		{TEXT("join"), nullptr, TEXT("Players"), nullptr, TEXT("members::add(player).map(|()| vec![0xc0])")},
		{TEXT("leave"), nullptr, TEXT("Players"), TEXT("members::remove(player);"), TEXT("Ok(vec![0xc0])")},
		{TEXT("add_member"), TEXT("MemberInputs"), TEXT("ServerOnly"), nullptr, TEXT("members::add(params.Player).map(|()| vec![0xc0])")},
		{TEXT("remove_member"), TEXT("MemberInputs"), TEXT("Leader"), TEXT("members::remove(params.Player);"), TEXT("Ok(vec![0xc0])")},
		{TEXT("make_leader"), TEXT("MemberInputs"), TEXT("Leader"), nullptr, TEXT("members::make_leader(params.Player).map(|()| vec![0xc0])")},
		{TEXT("set_open_for_joining"), TEXT("OpenInputs"), TEXT("Leader"), TEXT("members::set_open(params.bOpen);"), TEXT("Ok(vec![0xc0])")}};

	bool IsBuiltinMethod(const FString& Method)
	{
		for (const FBuiltinMember& BuiltIn : BuiltinMembers)
		{
			if (Method.Equals(BuiltIn.Method, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	const TCHAR* RuleName(ECrowdyServerFunctionCaller Caller)
	{
		switch (Caller)
		{
		case ECrowdyServerFunctionCaller::Members: return TEXT("Members");
		case ECrowdyServerFunctionCaller::Leader: return TEXT("Leader");
		case ECrowdyServerFunctionCaller::ServerOnly: return TEXT("ServerOnly");
		default: return TEXT("Players");
		}
	}

	bool IsRangeKind(ECrowdyExecKind Kind)
	{
		return Kind >= ECrowdyExecKind::Int8 && Kind <= ECrowdyExecKind::Double;
	}

	struct FValueRange
	{
		bool bMin = false;
		bool bMax = false;
		double Min = 0.0;
		double Max = 0.0;
		FString MinText;
		FString MaxText;
	};

	/** A List input's Value Range lives on its bag's property desc, a C++ struct's in its property metadata; both editor-only. */
	bool ReadValueRange(const UScriptStruct* Struct, const FProperty* Property, FValueRange& Out)
	{
		FString MinText;
		FString MaxText;
#if WITH_EDITOR
		if (const UPropertyBag* Bag = Cast<UPropertyBag>(Struct))
		{
			if (const FPropertyBagPropertyDesc* Desc = Bag->FindPropertyDescByProperty(Property))
			{
				MinText = Desc->GetMetaData(TEXT("ClampMin"));
				MaxText = Desc->GetMetaData(TEXT("ClampMax"));
			}
		}
		else if (Property)
		{
			MinText = Property->GetMetaData(TEXT("ClampMin"));
			MaxText = Property->GetMetaData(TEXT("ClampMax"));
		}
#endif
		Out.bMin = !MinText.IsEmpty() && FCString::IsNumeric(*MinText);
		Out.bMax = !MaxText.IsEmpty() && FCString::IsNumeric(*MaxText);
		Out.Min = Out.bMin ? FCString::Atod(*MinText) : 0.0;
		Out.Max = Out.bMax ? FCString::Atod(*MaxText) : 0.0;
		Out.MinText = Out.bMin ? MinText : FString();
		Out.MaxText = Out.bMax ? MaxText : FString();
		return Out.bMin || Out.bMax;
	}

	/** A whole-number bound as the i128 literal it was written as; false for any other text or one past i128's 38 digits. */
	bool IntegerBound(const FString& Text, FString& Out)
	{
		const bool bNegative = Text.StartsWith(TEXT("-"));
		const FString Written = Text.RightChop(bNegative ? 1 : 0);
		if (Written.IsEmpty())
		{
			return false;
		}
		for (const TCHAR Char : Written)
		{
			if (!IsAsciiDigit(Char))
			{
				return false;
			}
		}
		int32 Zeros = 0;
		while (Zeros < Written.Len() - 1 && Written[Zeros] == TEXT('0'))
		{
			++Zeros;
		}
		const FString Digits = Written.RightChop(Zeros);
		if (Digits.Len() > 38)
		{
			return false;
		}
		Out = bNegative && Digits != TEXT("0") ? TEXT("-") + Digits : Digits;
		return true;
	}

	/** Refuses params outside the range: "bad_params: Amount must be 1 to 100". False when an integer's bound is not a whole number. */
	bool RangeCheck(ECrowdyExecKind Kind, const FString& Ident, const FString& Name, const FValueRange& Range, FString& Out)
	{
		const bool bReal = Kind == ECrowdyExecKind::Float || Kind == ECrowdyExecKind::Double;
		const FString Value = bReal ? TEXT("params.") + Ident : FString::Printf(TEXT("(params.%s as i128)"), *Ident);
		FString MinCode;
		FString MaxCode;
		if (!bReal && ((Range.bMin && !IntegerBound(Range.MinText, MinCode)) || (Range.bMax && !IntegerBound(Range.MaxText, MaxCode))))
		{
			return false;
		}
		if (bReal)
		{
			RealLiteral(Range.Min, Kind == ECrowdyExecKind::Float, MinCode);
			RealLiteral(Range.Max, Kind == ECrowdyExecKind::Float, MaxCode);
		}
		const FString MinText = bReal ? FString::SanitizeFloat(Range.Min) : MinCode;
		const FString MaxText = bReal ? FString::SanitizeFloat(Range.Max) : MaxCode;
		TArray<FString> Tests;
		if (Range.bMin)
		{
			Tests.Add(Value + TEXT(" < ") + MinCode);
		}
		if (Range.bMax)
		{
			Tests.Add(Value + TEXT(" > ") + MaxCode);
		}
		const FString Allowed = Range.bMin && Range.bMax ? MinText + TEXT(" to ") + MaxText : Range.bMin ? TEXT("at least ") + MinText : TEXT("at most ") + MaxText;
		const FString Message = FString::Printf(TEXT("bad_params: %s must be %s"), *Name, *Allowed);
		Out = FString::Printf(TEXT("                if %s {\n                    return Err(Error::new(%s));\n                }\n"), *FString::Join(Tests, TEXT(" || ")),
			*RustStrLiteral(Message));
		return true;
	}

	/** The trait item without its ending: `fn method(&mut self, ...) -> Result<Reply>`. */
	FString FunctionSignature(const FFunctionInfo& Function)
	{
		const FString Params = Function.Params.IsEmpty() ? FString() : TEXT(", params: ") + Function.Params;
		const FString Reply = Function.Reply.IsEmpty() ? FString(TEXT("()")) : Function.Reply;
		return FString::Printf(TEXT("fn %s(&mut self, ctx: &Ctx, call: &Call<'_>%s) -> Result<%s>"), *Function.Ident, *Params, *Reply);
	}

	struct FCodegen
	{
		const UCrowdyServerObjectDefinition& Definition;
		const FCrowdyExecLayout& Layout;
		TArray<FString> StructNames;
		/** Each struct's field names as Rust identifiers, parallel to the layout's fields. */
		TArray<TArray<FString>> FieldIdents;
		TArray<FString> EnumNames;
		TArray<TArray<FString>> VariantIdents;
		TArray<FFunctionInfo> FunctionInfos;
		TArray<FTimerInfo> TimerInfos;
		TArray<FString> TakenNames;
		/** Lists of the same shape share one struct; each other List starting with its values keeps its own name as an alias: {alias, struct}. */
		TArray<TPair<FString, FString>> Aliases;
		TArray<FListStruct> ListStructs;
		/** The calls module for Can Call, empty without it. */
		FString CallsModule;
		FString Error;

		bool Fail(const FString& Message)
		{
			Error = Message;
			return false;
		}

		const FString& StateName() const
		{
			return StructNames[Layout.StateStruct];
		}

		bool IsOwnerOnly() const
		{
			return Definition.Visibility == ECrowdyServerObjectVisibility::OwnerOnly;
		}

		bool UsesKind(ECrowdyExecKind Kind) const
		{
			return Layout.Types.ContainsByPredicate([Kind](const FCrowdyExecType& Type) { return Type.Kind == Kind; });
		}

		bool HasMembers() const
		{
			return Definition.MembersFrom != ECrowdyServerMembersSource::None;
		}

		bool KeepsRoster() const
		{
			return Definition.MembersFrom == ECrowdyServerMembersSource::ThisObject;
		}

		bool IsTeam() const
		{
			return Definition.MembersFrom == ECrowdyServerMembersSource::CrowdyTeam;
		}

		bool ShowsMembers() const
		{
			return KeepsRoster() && Definition.bShowMembers;
		}

		bool UsesCooldowns() const
		{
			return FunctionInfos.ContainsByPredicate([](const FFunctionInfo& Function) { return Function.CooldownMs > 0; });
		}

		bool RemovesLeavers() const
		{
			return KeepsRoster() && Definition.bRemoveMembersWhoLeave;
		}

		bool UsesSessions() const
		{
			return Definition.bOnPlayerJoined || Definition.bOnPlayerLeft || RemovesLeavers();
		}

		bool HasRead() const
		{
			return !WatchedFields().IsEmpty() || HasMembers();
		}

		bool AssignNames();
		bool NameFields(const FCrowdyExecStruct& Struct);
		bool NameVariants(const FCrowdyExecEnum& Enum);
		bool CollectFunctions();
		bool CollectRanges(const UScriptStruct* Params, FFunctionInfo& Info);
		bool CollectTimers();
		bool BuildCalls();
		bool CalleeModule(const UCrowdyServerObjectDefinition& Callee, FString& Out);
		bool IsBuiltinInputsOnly(int32 StructIndex) const;
		bool TypesItems(TArray<FString>& Items);
		bool NameList(const FCrowdyServerFunction& Function, bool bReply, FString& InOutName);
		bool FindStructName(const UScriptStruct* Struct, FString& Out) const;
		FString RustType(int32 TypeIndex) const;
		FString TypeComment(int32 TypeIndex) const;
		FString ValueIdComment(int32 StructIndex, int32 FieldIndex) const;
		TArray<int32> WatchedFields() const;
		FString KindCheck(int32 TypeIndex, const FString& Value) const;
		FString FieldCheck(int32 TypeIndex, const FString& Value) const;
		bool HoldsElements(int32 TypeIndex) const;
		FString ElementsSum(int32 StructIndex, TConstArrayView<int32> FieldIndexes, const TCHAR* Prefix) const;
		FString WatchedElementsCheck(const TCHAR* Prefix, const TCHAR* Refusal) const;

		bool EncodeInstance(const UScriptStruct* Struct, TFunctionRef<void(FInstancedStruct&)> Initialize, TArray<uint8>& OutBytes, FString& OutError) const;
		bool EncodeDefaults(int32 StructIndex, TFunctionRef<void(FInstancedStruct&)> Initialize, FDefaultBytes& Out);
		bool EncodeStructDefaults(int32 StructIndex, FDefaultBytes& Out);
		bool ReadDefaults(int32 StructIndex, TConstArrayView<uint8> Bytes, TArray<FString>& OutValues, FString& OutError) const;
		FString UnstableDefaultsError(int32 StructIndex, const TArray<FString>& Values, TConstArrayView<uint8> Again) const;
		bool StructItem(int32 StructIndex, const FString& Name, const FDefaultBytes& Defaults, FString& Out);
		FString EnumItem(int32 EnumIndex) const;
		FString EngineItem(const FEngineStruct& Engine) const;
		bool TypesFile(FString& Out);

		FString CargoFile() const;
		FString LibFile() const;
		TArray<CrowdyExecCodegen::FLogicStub> LogicStubs() const;
		FString LogicFile() const;
		void AppendTrait(FString& Text) const;
		void AppendObjectStructs(FString& Text) const;
		void AppendFeatureItems(FString& Text) const;
		void AppendTimersModule(FString& Text) const;
		void AppendHub(FString& Text) const;
		void AppendInstanceCheck(FString& Text) const;
		FString AuthorizeLine(ECrowdyServerFunctionCaller Caller) const;
		FString KeepBefore(const TCHAR* Indent) const;
		void AppendDispatch(FString& Text, const FFunctionInfo& Function) const;
		void AppendBuiltinDispatch(FString& Text) const;
		void AppendTimerHook(FString& Text) const;
		void AppendSessionHook(FString& Text) const;
		void AppendServerObjectImpl(FString& Text) const;
		void AppendReadFor(FString& Text) const;
		void AppendReadReply(FString& Text) const;
		void AppendCheckWatched(FString& Text) const;
		void AppendWatchedChanges(FString& Text, const TArray<int32>& Watched) const;
		void AppendRosterChanges(FString& Text) const;
		void AppendPreparePush(FString& Text) const;
		void AppendPreparePushWithoutMembers(FString& Text, const TArray<int32>& Watched) const;
		void AppendAuthorize(FString& Text) const;
		void AppendStructWireImpl(FString& Text, int32 StructIndex, const FString& Name) const;
		void AppendWireImpls(FString& Text) const;
	};

	/** Writes a struct's defaults as Rust literals by walking their encoded bytes alongside the layout. */
	struct FDefaultsReader
	{
		const FCodegen& Gen;
		FCrowdyExecReader Reader;
		FString Error;

		bool Malformed()
		{
			Error = Error.IsEmpty() ? FString(TEXT("the encoded defaults do not match the field tables")) : Error;
			return false;
		}

		bool Next(int32 TypeIndex, FString& Out);
		bool TokenLiteral(int32 TypeIndex, const FCrowdyExecToken& Token, FString& Out);
		bool Scalar(ECrowdyExecKind Kind, const FCrowdyExecToken& Token, FString& Out);
		bool FieldValues(int32 StructIndex, const FCrowdyExecToken& Token, TArray<FString>& Out);
		bool StructLiteral(int32 StructIndex, const FCrowdyExecToken& Token, FString& Out);
		bool EngineLiteral(const FEngineStruct& Engine, const FCrowdyExecToken& Token, FString& Out);
		bool EnumLiteral(int32 EnumIndex, const FCrowdyExecToken& Token, FString& Out);
		bool ListLiteral(int32 Inner, bool bSet, const FCrowdyExecToken& Token, FString& Out);
		bool MapLiteral(const FCrowdyExecType& Type, const FCrowdyExecToken& Token, FString& Out);
		bool OptionalLiteral(int32 Inner, const FCrowdyExecToken& Token, FString& Out);
	};

	bool FDefaultsReader::Next(int32 TypeIndex, FString& Out)
	{
		FCrowdyExecToken Token;
		return Reader.Next(Token, Error) && TokenLiteral(TypeIndex, Token, Out);
	}

	bool FDefaultsReader::TokenLiteral(int32 TypeIndex, const FCrowdyExecToken& Token, FString& Out)
	{
		const FCrowdyExecType& Type = Gen.Layout.Types[TypeIndex];
		switch (Type.Kind)
		{
		case ECrowdyExecKind::Enum: return EnumLiteral(Type.Target, Token, Out);
		case ECrowdyExecKind::Struct: return StructLiteral(Type.Target, Token, Out);
		case ECrowdyExecKind::Array: return ListLiteral(Type.Inner, false, Token, Out);
		case ECrowdyExecKind::Set: return ListLiteral(Type.Inner, true, Token, Out);
		case ECrowdyExecKind::Map: return MapLiteral(Type, Token, Out);
		case ECrowdyExecKind::Optional: return OptionalLiteral(Type.Inner, Token, Out);
		default: break;
		}
		const FEngineStruct* Engine = FindEngineStruct(Type.Kind);
		return Engine ? EngineLiteral(*Engine, Token, Out) : Scalar(Type.Kind, Token, Out);
	}

	bool FDefaultsReader::Scalar(ECrowdyExecKind Kind, const FCrowdyExecToken& Token, FString& Out)
	{
		switch (Kind)
		{
		case ECrowdyExecKind::Bool:
			Out = Token.bBool ? TEXT("true") : TEXT("false");
			return Token.Type == ECrowdyExecToken::Bool || Malformed();
		case ECrowdyExecKind::Float:
		case ECrowdyExecKind::Double:
			if (Token.Type != ECrowdyExecToken::Float)
			{
				return Malformed();
			}
			if (!RealLiteral(Token.Float, Kind == ECrowdyExecKind::Float, Out))
			{
				Error = TEXT("NaN and infinity cannot be defaults");
				return false;
			}
			return true;
		case ECrowdyExecKind::String:
		case ECrowdyExecKind::Name:
		case ECrowdyExecKind::Guid:
		case ECrowdyExecKind::GameplayTag:
		case ECrowdyExecKind::SoftPath:
			Out = RustStringLiteral(TextOf(Token));
			return Token.Type == ECrowdyExecToken::String || Malformed();
		default:
			return IntegerLiteral(Token, Out) || Malformed();
		}
	}

	bool FDefaultsReader::FieldValues(int32 StructIndex, const FCrowdyExecToken& Token, TArray<FString>& Out)
	{
		const TArray<FCrowdyExecField>& Fields = Gen.Layout.Structs[StructIndex].Fields;
		if (Token.Type != ECrowdyExecToken::Map || Token.Count != Fields.Num())
		{
			return Malformed();
		}
		for (const FCrowdyExecField& Field : Fields)
		{
			FCrowdyExecToken Key;
			if (!Reader.Next(Key, Error) || !KeyIs(Key, Field.Key))
			{
				return Malformed();
			}
			if (!Next(Field.Type, Out.AddDefaulted_GetRef()))
			{
				return false;
			}
		}
		return true;
	}

	bool FDefaultsReader::StructLiteral(int32 StructIndex, const FCrowdyExecToken& Token, FString& Out)
	{
		TArray<FString> Values;
		if (!FieldValues(StructIndex, Token, Values))
		{
			return false;
		}
		TArray<FString> Parts;
		for (int32 Index = 0; Index < Values.Num(); ++Index)
		{
			Parts.Add(Gen.FieldIdents[StructIndex][Index] + TEXT(": ") + Values[Index]);
		}
		Out = BraceLiteral(Gen.StructNames[StructIndex], Parts);
		return true;
	}

	bool FDefaultsReader::EngineLiteral(const FEngineStruct& Engine, const FCrowdyExecToken& Token, FString& Out)
	{
		if (Token.Type != ECrowdyExecToken::Map || Token.Count != Engine.Parts.Num())
		{
			return Malformed();
		}
		TArray<FString> Parts;
		for (const FEnginePart& Part : Engine.Parts)
		{
			FCrowdyExecToken Key;
			FCrowdyExecToken PartValue;
			FString Literal;
			if (!Reader.Next(Key, Error) || !TextOf(Key).Equals(Part.Name, ESearchCase::CaseSensitive) || !Reader.Next(PartValue, Error)
				|| !Scalar(Part.Kind, PartValue, Literal))
			{
				return Malformed();
			}
			Parts.Add(FString(Part.Name) + TEXT(": ") + Literal);
		}
		Out = BraceLiteral(Engine.Name, Parts);
		return true;
	}

	bool FDefaultsReader::EnumLiteral(int32 EnumIndex, const FCrowdyExecToken& Token, FString& Out)
	{
		if (Token.Type != ECrowdyExecToken::String)
		{
			return Malformed();
		}
		const FString Name = TextOf(Token);
		const int32 Index = Gen.Layout.Enums[EnumIndex].Names.IndexOfByPredicate([&Name](const FString& Candidate) { return Candidate.Equals(Name, ESearchCase::CaseSensitive); });
		if (Index == INDEX_NONE)
		{
			return Malformed();
		}
		Out = Gen.EnumNames[EnumIndex] + TEXT("::") + Gen.VariantIdents[EnumIndex][Index];
		return true;
	}

	bool FDefaultsReader::ListLiteral(int32 Inner, bool bSet, const FCrowdyExecToken& Token, FString& Out)
	{
		if (Token.Type != ECrowdyExecToken::Array)
		{
			return Malformed();
		}
		TArray<FString> Items;
		for (int32 Index = 0; Index < Token.Count; ++Index)
		{
			if (!Next(Inner, Items.AddDefaulted_GetRef()))
			{
				return false;
			}
		}
		const FString Joined = FString::Join(Items, TEXT(", "));
		if (bSet)
		{
			Out = Items.IsEmpty() ? FString(TEXT("BTreeSet::new()")) : TEXT("BTreeSet::from([") + Joined + TEXT("])");
			return true;
		}
		Out = Items.IsEmpty() ? FString(TEXT("Vec::new()")) : TEXT("vec![") + Joined + TEXT("]");
		return true;
	}

	bool FDefaultsReader::MapLiteral(const FCrowdyExecType& Type, const FCrowdyExecToken& Token, FString& Out)
	{
		if (Token.Type != ECrowdyExecToken::Map)
		{
			return Malformed();
		}
		TArray<FString> Entries;
		for (int32 Index = 0; Index < Token.Count; ++Index)
		{
			FString Key;
			FString EntryValue;
			if (!Next(Type.Inner, Key) || !Next(Type.Value, EntryValue))
			{
				return false;
			}
			Entries.Add(FString::Printf(TEXT("(%s, %s)"), *Key, *EntryValue));
		}
		Out = Entries.IsEmpty() ? FString(TEXT("BTreeMap::new()")) : TEXT("BTreeMap::from([") + FString::Join(Entries, TEXT(", ")) + TEXT("])");
		return true;
	}

	bool FDefaultsReader::OptionalLiteral(int32 Inner, const FCrowdyExecToken& Token, FString& Out)
	{
		if (Token.Type == ECrowdyExecToken::Nil)
		{
			Out = TEXT("None");
			return true;
		}
		FString Literal;
		if (!TokenLiteral(Inner, Token, Literal))
		{
			return false;
		}
		Out = TEXT("Some(") + Literal + TEXT(")");
		return true;
	}

	bool FCodegen::AssignNames()
	{
		for (const FCrowdyExecStruct& Struct : Layout.Structs)
		{
			const bool bNamedByLayout = Cast<UUserDefinedStruct>(Struct.Struct) || Cast<UPropertyBag>(Struct.Struct);
			const FString Raw = bNamedByLayout ? Struct.DisplayName : Struct.Struct->GetName();
			StructNames.Add(ClaimTypeName(FilterTypeName(Raw), TakenNames));
			if (!NameFields(Struct))
			{
				return false;
			}
		}
		for (const FCrowdyExecEnum& Enum : Layout.Enums)
		{
			const FString Raw = Enum.Enum->GetName();
			const bool bDropE = Raw.Len() > 1 && Raw[0] == 'E' && IsAsciiUpper(Raw[1]);
			EnumNames.Add(ClaimTypeName(FilterTypeName(bDropE ? Raw.RightChop(1) : Raw), TakenNames));
			if (!NameVariants(Enum))
			{
				return false;
			}
		}
		return true;
	}

	bool FCodegen::NameFields(const FCrowdyExecStruct& Struct)
	{
		TArray<FString>& Idents = FieldIdents.AddDefaulted_GetRef();
		for (const FCrowdyExecField& Field : Struct.Fields)
		{
			if (!RustIdent(Field.Name, Idents.AddDefaulted_GetRef()))
			{
				return Fail(UnwritableNameError(Struct.DisplayName, Field.Name));
			}
		}
		return true;
	}

	bool FCodegen::NameVariants(const FCrowdyExecEnum& Enum)
	{
		if (Enum.Names.IsEmpty())
		{
			return Fail(FString::Printf(TEXT("%s has no values the server code can use"), *Enum.Enum->GetName()));
		}
		TArray<FString>& Idents = VariantIdents.AddDefaulted_GetRef();
		for (const FString& Name : Enum.Names)
		{
			if (!RustIdent(Name, Idents.AddDefaulted_GetRef()))
			{
				return Fail(UnwritableNameError(Enum.Enum->GetName(), Name));
			}
		}
		return true;
	}

	bool FCodegen::FindStructName(const UScriptStruct* Struct, FString& Out) const
	{
		if (!Struct)
		{
			Out.Reset();
			return true;
		}
		const int32 Index = Layout.FindStruct(Struct);
		if (Index == INDEX_NONE)
		{
			return false;
		}
		Out = StructNames[Index];
		return true;
	}

	bool FCodegen::CollectFunctions()
	{
		for (const FCrowdyServerFunction& Function : Definition.Functions)
		{
			FFunctionInfo& Info = FunctionInfos.AddDefaulted_GetRef();
			Info.Name = Function.Name.ToString();
			Info.Method = Function.GetMethodName();
			Info.WhoCanCall = Function.WhoCanCall;
			if (IsOneOf(Info.Method, ReservedMethods) || (KeepsRoster() && IsBuiltinMethod(Info.Method)) || !RustIdent(Info.Method, Info.Ident))
			{
				return Fail(FString::Printf(TEXT("Server Function %s: '%s' is reserved in the server code (Rust); set another server name for it on the definition"),
					*Info.Name, *Info.Method));
			}
			if (!FindStructName(Function.GetParamsStruct(), Info.Params) || !FindStructName(Function.GetReplyStruct(), Info.Reply))
			{
				return Fail(FString::Printf(TEXT("Server Function %s: its structs are missing from the field tables; save the definition again"), *Info.Name));
			}
			if (!NameList(Function, false, Info.Params) || !NameList(Function, true, Info.Reply))
			{
				return false;
			}
			Info.CooldownMs = Function.CooldownSeconds > 0.f ? FMath::Max<int64>(1, FMath::RoundToInt64(Function.CooldownSeconds * 1000.0)) : 0;
			if (!CollectRanges(Function.GetParamsStruct(), Info))
			{
				return false;
			}
		}
		return true;
	}

	bool FCodegen::CollectRanges(const UScriptStruct* Params, FFunctionInfo& Info)
	{
		const int32 StructIndex = Params ? Layout.FindStruct(Params) : INDEX_NONE;
		if (StructIndex == INDEX_NONE)
		{
			return true;
		}
		const FCrowdyExecStruct& Struct = Layout.Structs[StructIndex];
		for (int32 Index = 0; Index < Struct.Fields.Num(); ++Index)
		{
			const FCrowdyExecType& Type = Layout.Types[Struct.Fields[Index].Type];
			FValueRange Range;
			if (!IsRangeKind(Type.Kind) || !ReadValueRange(Struct.Struct, Type.Property, Range))
			{
				continue;
			}
			FString Check;
			if (!RangeCheck(Type.Kind, FieldIdents[StructIndex][Index], Struct.Fields[Index].Name, Range, Check))
			{
				return Fail(FString::Printf(TEXT("Function %s: %s: a whole-number input's Value Range must be whole numbers of at most 38 digits"), *Info.Name,
					*Struct.Fields[Index].Name));
			}
			Info.RangeChecks.Add(MoveTemp(Check));
		}
		return true;
	}

	bool FCodegen::CollectTimers()
	{
		for (const FCrowdyServerTimer& Timer : Definition.Timers)
		{
			FCrowdyServerFunction Named;
			Named.Name = Timer.Name;
			FTimerInfo Info;
			Info.Name = Timer.Name.ToString();
			Info.Method = Named.GetMethodName();
			Info.Constant = Info.Method.ToUpper();
			Info.bEvery = Timer.Repeat == ECrowdyServerTimerRepeat::Every;
			Info.Ms = FMath::Max<int64>(1, FMath::RoundToInt64(Timer.Seconds * 1000.0));
			Info.bAutomatic = Timer.bStartAutomatically;
			auto SameMethod = [&Info](const auto& Other) { return Other.Method.Equals(Info.Method, ESearchCase::CaseSensitive); };
			const bool bTaken = IsOneOf(Info.Method, ReservedMethods) || (KeepsRoster() && IsBuiltinMethod(Info.Method))
				|| FunctionInfos.ContainsByPredicate(SameMethod) || TimerInfos.ContainsByPredicate(SameMethod);
			if (Timer.Name.IsNone() || Info.Method.IsEmpty() || bTaken || !RustIdent(Info.Method, Info.Ident))
			{
				return Fail(FString::Printf(TEXT("Timer %s: '%s' cannot name its function in the server code (Rust); rename the timer"), *Info.Name, *Info.Method));
			}
			if (Info.Constant.Equals(TimerQueueStatic, ESearchCase::CaseSensitive))
			{
				return Fail(FString::Printf(TEXT("Timer %s: the server code (Rust) uses timers::%s itself; rename the timer"), *Info.Name, *Info.Constant));
			}
			TimerInfos.Add(MoveTemp(Info));
		}
		return true;
	}

	/** The built-in members' input structs are written by the glue itself unless the definition uses one of its own accord. */
	bool FCodegen::IsBuiltinInputsOnly(int32 StructIndex) const
	{
		const UScriptStruct* Struct = Layout.Structs[StructIndex].Struct;
		if (Struct != FCrowdyServerMemberInputs::StaticStruct() && Struct != FCrowdyServerOpenInputs::StaticStruct())
		{
			return false;
		}
		const bool bOwnUse = StructIndex == Layout.StateStruct || Definition.Functions.ContainsByPredicate([Struct](const FCrowdyServerFunction& Function)
		{
			return Function.GetParamsStruct() == Struct || Function.GetReplyStruct() == Struct;
		});
		return !bOwnUse && !Layout.Types.ContainsByPredicate([StructIndex](const FCrowdyExecType& Type)
		{
			return Type.Kind == ECrowdyExecKind::Struct && Type.Target == StructIndex;
		});
	}

	/** A List whose struct is named for another List goes by its own name: an alias when it starts with the same values, else a struct of its own. */
	bool FCodegen::NameList(const FCrowdyServerFunction& Function, bool bReply, FString& InOutName)
	{
		const FString ListName = bReply ? Function.GetReplyListName() : Function.GetParamsListName();
		if (ListName.IsEmpty() || InOutName.IsEmpty())
		{
			return true;
		}
		const FString Wanted = FilterTypeName(ListName);
		if (Wanted.Equals(InOutName, ESearchCase::CaseSensitive))
		{
			return true;
		}
		FListStruct List{ClaimTypeName(Wanted, TakenNames), Layout.FindStruct(bReply ? Function.GetReplyStruct() : Function.GetParamsStruct())};
		auto InitializeList = [&Function, bReply](FInstancedStruct& Out) { return bReply ? Function.InitializeReply(Out) : Function.InitializeParams(Out); };
		FDefaultBytes Shared;
		if (!EncodeStructDefaults(List.StructIndex, Shared) || !EncodeDefaults(List.StructIndex, InitializeList, List.Defaults))
		{
			return Fail(FString::Printf(TEXT("Server Function %s: %s: %s"), *Function.Name.ToString(), *ListName, *Error));
		}
		const FString SharedName = InOutName;
		InOutName = List.Name;
		if (List.Defaults.Bytes != Shared.Bytes)
		{
			ListStructs.Add(MoveTemp(List));
			return true;
		}
		Aliases.Add({List.Name, SharedName});
		return true;
	}

	FString FCodegen::RustType(int32 TypeIndex) const
	{
		const FCrowdyExecType& Type = Layout.Types[TypeIndex];
		switch (Type.Kind)
		{
		case ECrowdyExecKind::Enum: return EnumNames[Type.Target];
		case ECrowdyExecKind::Struct: return StructNames[Type.Target];
		case ECrowdyExecKind::Array: return FString::Printf(TEXT("Vec<%s>"), *RustType(Type.Inner));
		case ECrowdyExecKind::Set: return FString::Printf(TEXT("BTreeSet<%s>"), *RustType(Type.Inner));
		case ECrowdyExecKind::Map: return FString::Printf(TEXT("BTreeMap<%s, %s>"), *RustType(Type.Inner), *RustType(Type.Value));
		case ECrowdyExecKind::Optional: return FString::Printf(TEXT("Option<%s>"), *RustType(Type.Inner));
		default: return ScalarRustType(Type.Kind);
		}
	}

	FString FCodegen::TypeComment(int32 TypeIndex) const
	{
		const FCrowdyExecType& Type = Layout.Types[TypeIndex];
		const TCHAR* const Word = KindWord(Type.Kind);
		switch (Type.Kind)
		{
		case ECrowdyExecKind::Enum: return FString::Printf(TEXT("%s %s"), Word, *EnumNames[Type.Target]);
		case ECrowdyExecKind::Struct: return FString::Printf(TEXT("%s %s"), Word, *StructNames[Type.Target]);
		case ECrowdyExecKind::Array:
		case ECrowdyExecKind::Set:
		case ECrowdyExecKind::Optional:
			return FString::Printf(TEXT("%s<%s>"), Word, *TypeComment(Type.Inner));
		case ECrowdyExecKind::Map: return FString::Printf(TEXT("%s<%s, %s>"), Word, *TypeComment(Type.Inner), *TypeComment(Type.Value));
		default: return Word;
		}
	}

	/** ` id=<32 hex digits>` for a List's value, the id it keeps when renamed and the one its bake records; empty for a struct's field. */
	FString FCodegen::ValueIdComment(int32 StructIndex, int32 FieldIndex) const
	{
		const UPropertyBag* Bag = Cast<UPropertyBag>(Layout.Structs[StructIndex].Struct);
		const FProperty* Property = Layout.Types[Layout.Structs[StructIndex].Fields[FieldIndex].Type].Property;
		// The desc's id is what UPropertyBag::FindPropertyGuidFromName, and so the bake, gives the value.
		const FPropertyBagPropertyDesc* Desc = Bag ? Bag->FindPropertyDescByProperty(Property) : nullptr;
		const FGuid Id = Desc ? Desc->ID : FGuid();
		return Id.IsValid() ? TEXT(" id=") + Id.ToString(EGuidFormats::Digits) : FString();
	}

	TArray<int32> FCodegen::WatchedFields() const
	{
		TArray<int32> Watched;
		const TArray<FCrowdyExecField>& Fields = Layout.Structs[Layout.StateStruct].Fields;
		for (int32 Index = 0; Index < Fields.Num(); ++Index)
		{
			if (Fields[Index].bWatched)
			{
				Watched.Add(Index);
			}
		}
		return Watched;
	}

	/** A Rust Checked expression for what the client asks of the kinds Rust holds as String or i64 in Value, a place; empty when there are none. */
	FString FCodegen::KindCheck(int32 TypeIndex, const FString& Value) const
	{
		const FCrowdyExecType& Type = Layout.Types[TypeIndex];
		if (const TCHAR* Function = KindCheckFunction(Type.Kind))
		{
			return FString::Printf(TEXT("%s(&%s)"), Function, *Value);
		}
		const bool bKeyed = Type.Kind == ECrowdyExecKind::Set || Type.Kind == ECrowdyExecKind::Map;
		const bool bNameKeys = bKeyed && Layout.Types[Type.Inner].Kind == ECrowdyExecKind::Name;
		const FString NameKeys = bNameKeys ? FString::Printf(TEXT("check_name_keys(%s.%s())"), *Value, Type.Kind == ECrowdyExecKind::Set ? TEXT("iter") : TEXT("keys")) : FString();
		switch (Type.Kind)
		{
		case ECrowdyExecKind::Array:
		case ECrowdyExecKind::Set:
			return ThenCheck(EachCheck(Value, TEXT("v"), KindCheck(Type.Inner, TEXT("v"))), NameKeys);
		case ECrowdyExecKind::Map:
		{
			const FString KeyCheck = KindCheck(Type.Inner, TEXT("k"));
			const FString ValueCheck = KindCheck(Type.Value, TEXT("v"));
			const TCHAR* Binding = KeyCheck.IsEmpty() ? TEXT("(_, v)") : (ValueCheck.IsEmpty() ? TEXT("(k, _)") : TEXT("(k, v)"));
			return ThenCheck(EachCheck(Value, Binding, ThenCheck(KeyCheck, ValueCheck)), NameKeys);
		}
		case ECrowdyExecKind::Optional:
		{
			const FString Inner = KindCheck(Type.Inner, TEXT("v"));
			return Inner.IsEmpty() ? Inner : FString::Printf(TEXT("%s.as_ref().map_or(Ok(()), |v| %s)"), *Value, *Inner);
		}
		default:
			return FString();
		}
	}

	/** Everything the client asks of a field's value: `Wire::check(&self.Id).and_then(|()| check_guid(&self.Id))`. */
	FString FCodegen::FieldCheck(int32 TypeIndex, const FString& Value) const
	{
		return ThenCheck(FString::Printf(TEXT("Wire::check(&%s)"), *Value), KindCheck(TypeIndex, Value));
	}

	/** Whether a value of the type can hold list, set or map elements, which count against a message's element budget. */
	bool FCodegen::HoldsElements(int32 TypeIndex) const
	{
		const FCrowdyExecType& Type = Layout.Types[TypeIndex];
		switch (Type.Kind)
		{
		case ECrowdyExecKind::Array:
		case ECrowdyExecKind::Set:
		case ECrowdyExecKind::Map:
			return true;
		case ECrowdyExecKind::Optional:
			return HoldsElements(Type.Inner);
		case ECrowdyExecKind::Struct:
			return Layout.Structs[Type.Target].Fields.ContainsByPredicate([this](const FCrowdyExecField& Field) { return HoldsElements(Field.Type); });
		default:
			return false;
		}
	}

	/** `state.Grid.elements() + state.Rows.elements()` over the fields that can hold elements; empty when none can. */
	FString FCodegen::ElementsSum(int32 StructIndex, TConstArrayView<int32> FieldIndexes, const TCHAR* Prefix) const
	{
		TArray<FString> Terms;
		for (const int32 Index : FieldIndexes)
		{
			if (HoldsElements(Layout.Structs[StructIndex].Fields[Index].Type))
			{
				Terms.Add(FString::Printf(TEXT("%s%s.elements()"), Prefix, *FieldIdents[StructIndex][Index]));
			}
		}
		return FString::Join(Terms, TEXT(" + "));
	}

	/** Refuses the watched values together holding more elements than a client reads in one message; empty when they cannot hold any. */
	FString FCodegen::WatchedElementsCheck(const TCHAR* Prefix, const TCHAR* Refusal) const
	{
		const FString Sum = ElementsSum(Layout.StateStruct, WatchedFields(), Prefix);
		if (Sum.IsEmpty())
		{
			return Sum;
		}
		return FString::Printf(TEXT("        if %s > MAX_MESSAGE_ELEMENTS {\n            return Err(%s);\n        }\n"), *Sum, Refusal);
	}

	bool FCodegen::EncodeInstance(const UScriptStruct* Struct, TFunctionRef<void(FInstancedStruct&)> Initialize, TArray<uint8>& OutBytes, FString& OutError) const
	{
		FInstancedStruct Instance;
		Initialize(Instance);
		return CrowdyExec::Encode(Definition, Struct, Instance.GetMemory(), OutBytes, OutError);
	}

	/** Encodes two fresh instances Initialize makes. */
	bool FCodegen::EncodeDefaults(int32 StructIndex, TFunctionRef<void(FInstancedStruct&)> Initialize, FDefaultBytes& Out)
	{
		const UScriptStruct* Struct = Layout.Structs[StructIndex].Struct;
		FString EncodeError;
		if (!EncodeInstance(Struct, Initialize, Out.Bytes, EncodeError))
		{
			return Fail(FString::Printf(TEXT("The defaults cannot be written as server code: %s"), *EncodeError));
		}
		if (!EncodeInstance(Struct, Initialize, Out.Again, EncodeError))
		{
			Out.Again.Reset();
		}
		return true;
	}

	/** The struct's starting values as a fresh instance gets them: the first List holding it, else the struct's own defaults. */
	bool FCodegen::EncodeStructDefaults(int32 StructIndex, FDefaultBytes& Out)
	{
		const UScriptStruct* Struct = Layout.Structs[StructIndex].Struct;
		return EncodeDefaults(StructIndex, [this, Struct](FInstancedStruct& Instance) { Definition.InitializeValues(Struct, Instance); }, Out);
	}

	bool FCodegen::ReadDefaults(int32 StructIndex, TConstArrayView<uint8> Bytes, TArray<FString>& OutValues, FString& OutError) const
	{
		FDefaultsReader Defaults{*this, FCrowdyExecReader(Bytes)};
		FCrowdyExecToken Token;
		const bool bRead = Defaults.Reader.Next(Token, Defaults.Error) && Defaults.FieldValues(StructIndex, Token, OutValues)
			&& (Defaults.Reader.AtEnd() || Defaults.Malformed());
		OutError = Defaults.Error;
		return bRead;
	}

	FString FCodegen::UnstableDefaultsError(int32 StructIndex, const TArray<FString>& Values, TConstArrayView<uint8> Again) const
	{
		const FCrowdyExecStruct& Struct = Layout.Structs[StructIndex];
		FString Owner = Struct.DisplayName;
		TArray<FString> AgainValues;
		FString ReadError;
		const bool bRead = ReadDefaults(StructIndex, Again, AgainValues, ReadError);
		for (int32 Index = 0; bRead && Index < Values.Num(); ++Index)
		{
			if (!Values[Index].Equals(AgainValues[Index], ESearchCase::CaseSensitive))
			{
				Owner += TEXT(".") + Struct.Fields[Index].Name;
				break;
			}
		}
		return FString::Printf(TEXT("%s's default value is not the same each time it is made (a random or time-based initializer); give it a fixed default"), *Owner);
	}

	bool FCodegen::StructItem(int32 StructIndex, const FString& Name, const FDefaultBytes& Defaults, FString& Out)
	{
		TArray<FString> Values;
		FString ReadError;
		if (!ReadDefaults(StructIndex, Defaults.Bytes, Values, ReadError))
		{
			return Fail(FString::Printf(TEXT("%s: its defaults cannot be written as server code: %s"), *Layout.Structs[StructIndex].DisplayName, *ReadError));
		}
		if (Defaults.Again != Defaults.Bytes)
		{
			return Fail(UnstableDefaultsError(StructIndex, Values, Defaults.Again));
		}
		const TArray<FString>& Idents = FieldIdents[StructIndex];
		const TArray<FCrowdyExecField>& Fields = Layout.Structs[StructIndex].Fields;
		Out = TEXT("#[derive(Serialize, Deserialize, Clone, PartialEq, Debug)]\n#[serde(default)]\n");
		Out += FString::Printf(TEXT("pub struct %s {\n"), *Name);
		for (int32 Index = 0; Index < Fields.Num(); ++Index)
		{
			const int32 Type = Fields[Index].Type;
			Out += FString::Printf(TEXT("    pub %s: %s, // %s%s\n"), *Idents[Index], *RustType(Type), *TypeComment(Type), *ValueIdComment(StructIndex, Index));
		}
		Out += TEXT("}\n\n");
		Out += FString::Printf(TEXT("impl Default for %s {\n    fn default() -> Self {\n        Self {\n"), *Name);
		for (int32 Index = 0; Index < Fields.Num(); ++Index)
		{
			Out += FString::Printf(TEXT("            %s: %s,\n"), *Idents[Index], *Values[Index]);
		}
		Out += TEXT("        }\n    }\n}\n");
		return true;
	}

	FString FCodegen::EnumItem(int32 EnumIndex) const
	{
		FString Text = TEXT("#[derive(Serialize, Deserialize, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]\n");
		Text += FString::Printf(TEXT("pub enum %s {\n"), *EnumNames[EnumIndex]);
		for (const FString& Variant : VariantIdents[EnumIndex])
		{
			Text += FString::Printf(TEXT("    %s,\n"), *Variant);
		}
		Text += TEXT("}\n");
		return Text;
	}

	FString FCodegen::EngineItem(const FEngineStruct& Engine) const
	{
		FString Text = TEXT("#[derive(Serialize, Deserialize, Clone, Copy, PartialEq, Debug, Default)]\n#[serde(default)]\n");
		Text += FString::Printf(TEXT("pub struct %s {\n"), Engine.Name);
		for (const FEnginePart& Part : Engine.Parts)
		{
			Text += FString::Printf(TEXT("    pub %s: %s, // %s\n"), Part.Name, ScalarRustType(Part.Kind), KindWord(Part.Kind));
		}
		Text += TEXT("}\n");
		return Text;
	}

	bool FCodegen::TypesItems(TArray<FString>& Items)
	{
		for (int32 Index = 0; Index < Layout.Enums.Num(); ++Index)
		{
			Items.Add(EnumItem(Index));
		}
		for (const FEngineStruct& Engine : EngineStructs)
		{
			if (UsesKind(Engine.Kind))
			{
				Items.Add(EngineItem(Engine));
			}
		}
		for (int32 Index = 0; Index < Layout.Structs.Num(); ++Index)
		{
			FDefaultBytes Defaults;
			if (!IsBuiltinInputsOnly(Index) && (!EncodeStructDefaults(Index, Defaults) || !StructItem(Index, StructNames[Index], Defaults, Items.AddDefaulted_GetRef())))
			{
				return false;
			}
		}
		for (const FListStruct& List : ListStructs)
		{
			if (!StructItem(List.StructIndex, List.Name, List.Defaults, Items.AddDefaulted_GetRef()))
			{
				return false;
			}
		}
		for (const TPair<FString, FString>& Alias : Aliases)
		{
			Items.Add(FString::Printf(TEXT("pub type %s = %s;\n"), *Alias.Key, *Alias.Value));
		}
		return true;
	}

	bool FCodegen::TypesFile(FString& Out)
	{
		TArray<FString> Items;
		if (!TypesItems(Items))
		{
			return false;
		}
		Out = FString::Printf(TEXT("// The Server Object type %s's structs, generated by the Crowdy SDK from its definition. Regenerate it; do not edit.\n"),
			*Definition.TypeName);
		Out += TEXT("use serde::{Deserialize, Serialize};\nuse std::collections::{BTreeMap, BTreeSet};\n\n");
		Out += FString::Join(Items, TEXT("\n"));
		return true;
	}

	FString FCodegen::CargoFile() const
	{
		FString Text = TEXT("[package]\n");
		Text += FString::Printf(TEXT("name = \"%s\"\n"), *Definition.TypeName);
		Text += TEXT("version = \"0.1.0\"\n")
			TEXT("edition = \"2024\"\n")
			TEXT("\n");
		// A definition with no asset, such as one made in memory, records none.
		if (Definition.GetPackage() != GetTransientPackage())
		{
			Text += FString::Printf(TEXT("[package.metadata.crowdy]\ndefinition = \"%s\"\n\n"), *TomlEscape(Definition.GetPathName()));
		}
		Text += TEXT("[lib]\n")
			TEXT("crate-type = [\"cdylib\"]\n")
			TEXT("\n")
			TEXT("[dependencies]\n")
			TEXT("ckx-sdk = \"0.7.0\"\n")
			TEXT("serde = { version = \"1\", features = [\"derive\"] }\n");
		return Text;
	}

	/** The name in a signature `fn name(...)`. */
	FString MethodOf(const FString& Signature)
	{
		return Signature.Mid(3, Signature.Find(TEXT("(")) - 3);
	}

	/** The logic.rs functions a definition's timers and ticked events add, without their ending. */
	TArray<FString> HandlerSignatures(const FCodegen& Gen)
	{
		TArray<FString> Signatures;
		for (const FTimerInfo& Timer : Gen.TimerInfos)
		{
			Signatures.Add(FString::Printf(TEXT("fn %s(&mut self, ctx: &Ctx) -> Result<()>"), *Timer.Ident));
		}
		if (Gen.Definition.bOnPlayerJoined)
		{
			Signatures.Add(TEXT("fn on_player_joined(&mut self, ctx: &Ctx, player: u64) -> Result<()>"));
		}
		if (Gen.Definition.bOnPlayerLeft)
		{
			Signatures.Add(TEXT("fn on_player_left(&mut self, ctx: &Ctx, player: u64) -> Result<()>"));
		}
		return Signatures;
	}

	void FCodegen::AppendTrait(FString& Text) const
	{
		Text += TEXT("/// The Server Functions, implemented in logic.rs. A new function appears here first.\n")
			TEXT("pub trait Functions {\n");
		for (const FFunctionInfo& Function : FunctionInfos)
		{
			Text += TEXT("    ") + FunctionSignature(Function) + TEXT(";\n");
		}
		for (const FString& Handler : HandlerSignatures(*this))
		{
			Text += TEXT("    ") + Handler + TEXT(";\n");
		}
		Text += TEXT("    fn on_timer(&mut self, ctx: &Ctx, name: &str) -> Result<()> {\n")
			TEXT("        Ok(())\n")
			TEXT("    }\n")
			TEXT("    fn on_topic(&mut self, ctx: &Ctx, msg: TopicMsg<'_>) -> Result<()> {\n")
			TEXT("        Ok(())\n")
			TEXT("    }\n")
			TEXT("    fn on_presence(&mut self, ctx: &Ctx, presence: &Presence) -> Result<()> {\n")
			TEXT("        Ok(())\n")
			TEXT("    }\n")
			TEXT("}\n")
			TEXT("\n");
	}

	/** The arm's first line: `authorize(ctx, &call, true)?;` without members, else with the caller rule. */
	FString FCodegen::AuthorizeLine(ECrowdyServerFunctionCaller Caller) const
	{
		if (!HasMembers())
		{
			return FString::Printf(TEXT("authorize(ctx, &call, %s)?;"), Caller == ECrowdyServerFunctionCaller::Players ? TEXT("true") : TEXT("false"));
		}
		return FString::Printf(TEXT("authorize(ctx, &call, %sRule::%s)?;"), KeepsRoster() ? TEXT("&self.roster, ") : TEXT(""), RuleName(Caller));
	}

	/** Keeps the state for a rollback; with members it first lends them to server code, with timers it holds their starts. */
	FString FCodegen::KeepBefore(const TCHAR* Indent) const
	{
		const FString Lend = KeepsRoster() ? FString(Indent) + TEXT("members::lend(&self.roster);\n") : FString();
		const FString Open = TimerInfos.IsEmpty() ? FString() : FString(Indent) + TEXT("timers::open();\n");
		return Lend + Open + Indent + TEXT("let before = Clone::clone(&self.state);\n");
	}

	void FCodegen::AppendDispatch(FString& Text, const FFunctionInfo& Function) const
	{
		Text += FString::Printf(TEXT("            \"%s\" => {\n"), *Function.Method);
		Text += TEXT("                ") + AuthorizeLine(Function.WhoCanCall) + TEXT("\n");
		if (!Function.Params.IsEmpty())
		{
			Text += FString::Printf(TEXT("                let params: %s = call.decode().map_err(bad_params)?;\n"), *Function.Params);
			Text += TEXT("                Wire::check(&params).map_err(|why| Error::new(&format!(\"bad_params: {why}\")))?;\n");
		}
		for (const FString& Check : Function.RangeChecks)
		{
			Text += Check;
		}
		if (Function.CooldownMs > 0)
		{
			Text += FString::Printf(TEXT("                check_cooldown(&self.cooldowns, ctx, &call, %s, %lld)?;\n"), *RustStrLiteral(Function.Name), Function.CooldownMs);
		}
		Text += KeepBefore(TEXT("                "));
		const TCHAR* const Reply = Function.Reply.IsEmpty() ? TEXT(".map(|()| vec![0xc0])") : TEXT(".and_then(|reply| encode_reply(&reply))");
		Text += FString::Printf(TEXT("                let result = Functions::%s(&mut self.state, ctx, &call%s)%s;\n"), *Function.Ident,
			Function.Params.IsEmpty() ? TEXT("") : TEXT(", params"), Reply);
		if (Function.CooldownMs <= 0)
		{
			Text += TEXT("                self.finish(ctx, before, result)\n            }\n");
			return;
		}
		Text += TEXT("                let result = self.finish(ctx, before, result);\n")
			TEXT("                if result.is_ok() {\n");
		Text += FString::Printf(TEXT("                    record_cooldown(&mut self.cooldowns, ctx, &call, %lld);\n"), Function.CooldownMs);
		Text += TEXT("                }\n")
			TEXT("                result\n")
			TEXT("            }\n");
	}

	void FCodegen::AppendBuiltinDispatch(FString& Text) const
	{
		if (!KeepsRoster())
		{
			return;
		}
		for (const FBuiltinMember& BuiltIn : BuiltinMembers)
		{
			Text += FString::Printf(TEXT("            \"%s\" => {\n"), BuiltIn.Method);
			Text += FString::Printf(TEXT("                authorize(ctx, &call, &self.roster, Rule::%s)?;\n"), BuiltIn.Rule);
			if (BuiltIn.Inputs)
			{
				Text += FString::Printf(TEXT("                let params: %s = call.decode().map_err(bad_params)?;\n"), BuiltIn.Inputs);
			}
			else
			{
				Text += FString::Printf(TEXT("                let player = call.player().map_err(|_| Error::new(\"denied: only a player can %s\"))?;\n"), BuiltIn.Method);
			}
			Text += KeepBefore(TEXT("                "));
			if (BuiltIn.Before)
			{
				Text += FString::Printf(TEXT("                %s\n"), BuiltIn.Before);
			}
			Text += FString::Printf(TEXT("                let result = %s;\n"), BuiltIn.Result);
			Text += TEXT("                self.finish(ctx, before, result)\n            }\n");
		}
	}

	void FCodegen::AppendInstanceCheck(FString& Text) const
	{
		if (Definition.bOnlyOneInstance)
		{
			const FString OnlyId = UCrowdyServerObjectDefinition::OnlyInstanceId;
			Text += FString::Printf(TEXT("        if ctx.key != %s {\n"), *RustStrLiteral(OnlyId));
			Text += FString::Printf(TEXT("            return Err(Error::new(%s));\n"),
				*RustStrLiteral(FString::Printf(TEXT("denied: this Server Object has only one instance, whose Instance Id is %s"), *OnlyId)));
			Text += TEXT("        }\n");
		}
		if (IsTeam())
		{
			Text += TEXT("        if !is_user_id(&ctx.key) {\n")
				TEXT("            return Err(Error::new(\"denied: a team Server Object's Instance Id is its team id\"));\n")
				TEXT("        }\n");
		}
		if (!IsOwnerOnly())
		{
			return;
		}
		Text += TEXT("        if !is_user_id(&ctx.key) {\n")
			TEXT("            return Err(Error::new(\"denied: an owner-only Server Object's Instance Id is its owner's user id\"));\n")
			TEXT("        }\n");
	}

	void FCodegen::AppendTimerHook(FString& Text) const
	{
		Text += TEXT("    fn on_timer(&mut self, ctx: &Ctx, name: &str) -> Result<()> {\n");
		Text += KeepBefore(TEXT("        "));
		if (TimerInfos.IsEmpty())
		{
			Text += TEXT("        let result = Functions::on_timer(&mut self.state, ctx, name);\n");
		}
		else
		{
			Text += TEXT("        let result = match name {\n");
			for (const FTimerInfo& Timer : TimerInfos)
			{
				Text += FString::Printf(TEXT("            %s => Functions::%s(&mut self.state, ctx),\n"), *RustStrLiteral(Timer.Name), *Timer.Ident);
			}
			// A timer the logic started under its own name still reaches its on_timer.
			Text += TEXT("            other => Functions::on_timer(&mut self.state, ctx, other),\n")
				TEXT("        };\n");
		}
		Text += TEXT("        self.finish(ctx, before, result)\n")
			TEXT("    }\n")
			TEXT("\n");
	}

	void FCodegen::AppendSessionHook(FString& Text) const
	{
		if (!UsesSessions())
		{
			return;
		}
		Text += TEXT("\n")
			TEXT("    // Connections are counted per player and saved: the first opens a visit, the last closes it; an uncounted player is skipped.\n")
			TEXT("    fn on_session(&mut self, ctx: &Ctx, player: u64, session: ckx_sdk::Session) -> Result<()> {\n")
			TEXT("        let connections = self.sessions.get(&player).copied().unwrap_or(0);\n")
			TEXT("        match session {\n")
			TEXT("            ckx_sdk::Session::Joined => {\n")
			TEXT("                if connections == 0 && self.sessions.len() >= MAX_SESSION_PLAYERS {\n")
			TEXT("                    return Ok(());\n")
			TEXT("                }\n")
			TEXT("                self.sessions.insert(player, connections + 1);\n")
			TEXT("                if connections > 0 {\n")
			TEXT("                    return Ok(());\n")
			TEXT("                }\n");
		if (Definition.bOnPlayerJoined)
		{
			Text += KeepBefore(TEXT("                "));
			Text += TEXT("                let result = Functions::on_player_joined(&mut self.state, ctx, player);\n")
				TEXT("                self.finish(ctx, before, result)\n");
		}
		else
		{
			Text += TEXT("                Ok(())\n");
		}
		Text += TEXT("            }\n")
			TEXT("            ckx_sdk::Session::Left => {\n")
			TEXT("                if connections == 0 {\n")
			TEXT("                    return Ok(());\n")
			TEXT("                }\n")
			TEXT("                if connections > 1 {\n")
			TEXT("                    self.sessions.insert(player, connections - 1);\n")
			TEXT("                    return Ok(());\n")
			TEXT("                }\n")
			TEXT("                self.sessions.remove(&player);\n");
		if (RemovesLeavers())
		{
			Text += KeepBefore(TEXT("                "));
			Text += TEXT("                members::remove(player);\n");
			Text += Definition.bOnPlayerLeft ? TEXT("                self.finish(ctx, before, Ok(()))?;\n")
				TEXT("                // The removal stands even when on_player_left fails.\n") : TEXT("                self.finish(ctx, before, Ok(()))\n");
		}
		if (Definition.bOnPlayerLeft)
		{
			Text += KeepBefore(TEXT("                "));
			Text += TEXT("                let result = Functions::on_player_left(&mut self.state, ctx, player);\n")
				TEXT("                self.finish(ctx, before, result)\n");
		}
		if (!Definition.bOnPlayerLeft && !RemovesLeavers())
		{
			Text += TEXT("                Ok(())\n");
		}
		Text += TEXT("            }\n")
			TEXT("            #[allow(unreachable_patterns)]\n")
			TEXT("            _ => Ok(()),\n")
			TEXT("        }\n")
			TEXT("    }\n");
	}

	void FCodegen::AppendHub(FString& Text) const
	{
		Text += TEXT("impl Hub for ServerObject {\n")
			TEXT("    fn spawn(ctx: &Ctx, seed: &[u8]) -> Result<Self> {\n");
		AppendInstanceCheck(Text);
		Text += FString::Printf(TEXT("        let state = if seed.is_empty() { <%s as Default>::default() } else { decode(seed)? };\n"), *StateName());
		Text += WatchedFields().IsEmpty() ? TEXT("")
			: TEXT("        Self::check_watched(&state).map_err(|(field, why)| Error::new(&format!(\"bad_seed: {field}: the seed holds a value players cannot read: {why}\")))?;\n");
		for (const FTimerInfo& Timer : TimerInfos)
		{
			Text += Timer.bAutomatic ? FString::Printf(TEXT("        timers::arm(ctx, %s)?;\n"), *RustStrLiteral(Timer.Name)) : FString();
		}
		FString SpawnExtra;
		FString LoadExtra;
		FString PersistExtra;
		if (KeepsRoster())
		{
			SpawnExtra += TEXT(", roster: Roster::default()");
			LoadExtra += TEXT(", roster: Roster { members: saved.members, leader: saved.leader, open: saved.open }");
			PersistExtra += TEXT(", members: &self.roster.members, leader: self.roster.leader, open: self.roster.open");
		}
		if (UsesCooldowns())
		{
			SpawnExtra += TEXT(", cooldowns: BTreeMap::new()");
			LoadExtra += TEXT(", cooldowns: saved.cooldowns");
			PersistExtra += TEXT(", cooldowns: &self.cooldowns");
		}
		if (UsesSessions())
		{
			SpawnExtra += TEXT(", sessions: BTreeMap::new()");
			LoadExtra += TEXT(", sessions: saved.sessions");
			PersistExtra += TEXT(", sessions: &self.sessions");
		}
		Text += FString::Printf(TEXT("        Ok(Self { state, epoch: ctx.now_ms() as u64, seq: 0%s })\n"), *SpawnExtra);
		Text += TEXT("    }\n")
			TEXT("\n")
			TEXT("    fn load(ctx: &Ctx, snapshot: &[u8], _from_version: u64) -> Result<Self> {\n");
		AppendInstanceCheck(Text);
		Text += TEXT("        let saved: SnapshotIn = decode(snapshot)?;\n")
			TEXT("        if saved.contract != CONTRACT {\n")
			TEXT("            return Err(Error::new(&format!(\"the snapshot was written for contract {}, this code reads {}\", saved.contract, CONTRACT)));\n")
			TEXT("        }\n");
		Text += FString::Printf(TEXT("        Ok(Self { state: saved.state, epoch: ctx.now_ms() as u64, seq: 0%s })\n"), *LoadExtra);
		Text += TEXT("    }\n")
			TEXT("\n")
			TEXT("    fn persist(&mut self, _ctx: &Ctx) -> Result<Vec<u8>> {\n");
		Text += FString::Printf(TEXT("        encode(&SnapshotOut { contract: CONTRACT, state: &self.state%s })\n"), *PersistExtra);
		Text += TEXT("    }\n")
			TEXT("\n")
			TEXT("    fn handle(&mut self, ctx: &Ctx, call: Call<'_>) -> Result<Vec<u8>> {\n")
			TEXT("        match call.method {\n");
		if (HasRead())
		{
			Text += TEXT("            \"read\" => {\n");
			Text += TEXT("                ") + AuthorizeLine(ECrowdyServerFunctionCaller::Players) + TEXT("\n");
			Text += HasMembers() || WatchedFields().IsEmpty() ? TEXT("") : TEXT("                Self::check_watched(&self.state).map_err(unreadable)?;\n");
			Text += HasMembers() ? TEXT("                self.read_for(ctx, &call)\n") : TEXT("                self.read_reply()\n");
			Text += TEXT("            }\n");
		}
		for (const FFunctionInfo& Function : FunctionInfos)
		{
			AppendDispatch(Text, Function);
		}
		AppendBuiltinDispatch(Text);
		Text += TEXT("            other => Err(Error::new(&format!(\"unknown_method: {other}\"))),\n")
			TEXT("        }\n")
			TEXT("    }\n")
			TEXT("\n");
		AppendTimerHook(Text);
		Text += TEXT("    fn on_topic(&mut self, ctx: &Ctx, msg: TopicMsg<'_>) -> Result<()> {\n");
		Text += KeepBefore(TEXT("        "));
		Text += TEXT("        let result = Functions::on_topic(&mut self.state, ctx, msg);\n")
			TEXT("        self.finish(ctx, before, result)\n")
			TEXT("    }\n")
			TEXT("\n")
			TEXT("    fn on_presence(&mut self, ctx: &Ctx, presence: &Presence) -> Result<()> {\n");
		Text += KeepBefore(TEXT("        "));
		Text += TEXT("        let result = Functions::on_presence(&mut self.state, ctx, presence);\n")
			TEXT("        self.finish(ctx, before, result)\n")
			TEXT("    }\n");
		AppendSessionHook(Text);
		Text += TEXT("}\n")
			TEXT("\n");
	}

	/** Whether the reader is a member and the leader, which read_reply reports and Readable By Members hides the fields by. */
	void FCodegen::AppendReadFor(FString& Text) const
	{
		const TCHAR* const Member = KeepsRoster() ? TEXT("self.roster.members.contains(&player)") : TEXT("team_check(ctx, player, None)?");
		const TCHAR* const Leader = KeepsRoster() ? TEXT("self.roster.leader == Some(player)") : TEXT("team_check(ctx, player, Some(\"manage_group\"))?");
		const bool bMembersRead = Definition.Visibility == ECrowdyServerObjectVisibility::Members;
		const bool bWatched = !WatchedFields().IsEmpty();
		const TCHAR* const Check = TEXT("Self::check_watched(&self.state).map_err(unreadable)?;\n");
		Text += TEXT("    fn read_for(&self, ctx: &Ctx, call: &Call<'_>) -> Result<Vec<u8>> {\n")
			TEXT("        let Ok(player) = call.player() else {\n");
		Text += bWatched ? FString(TEXT("            ")) + Check : FString();
		Text += TEXT("            return self.read_reply(true, false, false);\n")
			TEXT("        };\n");
		Text += FString::Printf(TEXT("        let member = %s;\n"), Member);
		Text += FString::Printf(TEXT("        let leader = %s;\n"), Leader);
		// Only a read that carries the fields is refused for a value in them.
		if (bWatched && bMembersRead)
		{
			Text += FString(TEXT("        if member {\n            ")) + Check + TEXT("        }\n");
		}
		Text += bWatched && !bMembersRead ? FString(TEXT("        ")) + Check : FString();
		Text += FString::Printf(TEXT("        self.read_reply(%s, member, leader)\n"), bMembersRead ? TEXT("member") : TEXT("true"));
		Text += TEXT("    }\n")
			TEXT("\n");
	}

	void FCodegen::AppendReadReply(FString& Text) const
	{
		const TArray<int32> Watched = WatchedFields();
		const TArray<FCrowdyExecField>& Fields = Layout.Structs[Layout.StateStruct].Fields;
		const TArray<FString>& Idents = FieldIdents[Layout.StateStruct];
		const int32 Entries = 4 + (KeepsRoster() ? (ShowsMembers() ? 4 : 3) : 0) + (HasMembers() ? 2 : 0);
		Text += HasMembers() ? TEXT("    fn read_reply(&self, fields: bool, member: bool, leader: bool) -> Result<Vec<u8>> {\n") : TEXT("    fn read_reply(&self) -> Result<Vec<u8>> {\n");
		Text += TEXT("        let mut out = Vec::new();\n");
		Text += FString::Printf(TEXT("        write_map(&mut out, %d);\n"), Entries);
		Text += TEXT("        write_str(&mut out, \"contract\");\n")
			TEXT("        write_uint(&mut out, CONTRACT);\n")
			TEXT("        write_str(&mut out, \"epoch\");\n")
			TEXT("        write_uint(&mut out, self.epoch);\n")
			TEXT("        write_str(&mut out, \"seq\");\n")
			TEXT("        write_uint(&mut out, self.seq);\n")
			TEXT("        write_str(&mut out, \"fields\");\n");
		if (!HasMembers())
		{
			Text += FString::Printf(TEXT("        write_map(&mut out, %d);\n"), Watched.Num());
			for (const int32 Index : Watched)
			{
				Text += FString::Printf(TEXT("        write_field(&mut out, \"%s\", &self.state.%s)?;\n"), *Fields[Index].Name, *Idents[Index]);
			}
		}
		else
		{
			Text += FString::Printf(TEXT("        write_map(&mut out, if fields { %d } else { 0 });\n"), Watched.Num());
			Text += Watched.IsEmpty() ? TEXT("") : TEXT("        if fields {\n");
			for (const int32 Index : Watched)
			{
				Text += FString::Printf(TEXT("            write_field(&mut out, \"%s\", &self.state.%s)?;\n"), *Fields[Index].Name, *Idents[Index]);
			}
			Text += Watched.IsEmpty() ? TEXT("") : TEXT("        }\n");
		}
		if (KeepsRoster())
		{
			Text += TEXT("        write_str(&mut out, \"member_count\");\n")
				TEXT("        write_uint(&mut out, self.roster.members.len() as u64);\n")
				TEXT("        write_field(&mut out, \"leader\", &self.roster.leader)?;\n")
				TEXT("        write_field(&mut out, \"open\", &self.roster.open)?;\n");
			Text += ShowsMembers() ? TEXT("        write_field(&mut out, \"members\", &self.roster.members)?;\n") : TEXT("");
		}
		if (HasMembers())
		{
			Text += TEXT("        write_field(&mut out, \"is_member\", &member)?;\n")
				TEXT("        write_field(&mut out, \"is_leader\", &leader)?;\n");
		}
		Text += TEXT("        Ok(out)\n")
			TEXT("    }\n")
			TEXT("\n");
	}

	void FCodegen::AppendWatchedChanges(FString& Text, const TArray<int32>& Watched) const
	{
		const TArray<FCrowdyExecField>& Fields = Layout.Structs[Layout.StateStruct].Fields;
		const TArray<FString>& Idents = FieldIdents[Layout.StateStruct];
		for (const int32 Index : Watched)
		{
			const FString& Ident = Idents[Index];
			const FString& Name = Fields[Index].Name;
			Text += FString::Printf(TEXT("        if self.state.%s != before.%s {\n"), *Ident, *Ident);
			Text += FString::Printf(TEXT("            %s.map_err(|why| unsendable(\"%s\", why))?;\n"), *FieldCheck(Fields[Index].Type, TEXT("self.state.") + Ident), *Name);
			Text += FString::Printf(TEXT("            changed.push((\"%s\", encode(&self.state.%s)?));\n"), *Name, *Ident);
			Text += TEXT("        }\n");
		}
	}

	/** The first watched value a client could not read, which refuses a seed and a read. */
	void FCodegen::AppendCheckWatched(FString& Text) const
	{
		const TArray<int32> Watched = WatchedFields();
		if (Watched.IsEmpty())
		{
			return;
		}
		const TArray<FCrowdyExecField>& Fields = Layout.Structs[Layout.StateStruct].Fields;
		const TArray<FString>& Idents = FieldIdents[Layout.StateStruct];
		Text += TEXT("    // The first watched value an Unreal client could not read, with the field it is in.\n");
		Text += FString::Printf(TEXT("    fn check_watched(state: &%s) -> std::result::Result<(), (&'static str, &'static str)> {\n"), *StateName());
		for (const int32 Index : Watched)
		{
			Text += FString::Printf(TEXT("        %s.map_err(|why| (\"%s\", why))?;\n"), *FieldCheck(Fields[Index].Type, TEXT("state.") + Idents[Index]), *Fields[Index].Name);
		}
		Text += WatchedElementsCheck(TEXT("state."), TEXT("(\"the watched values\", \"together they hold more than 65536 elements\")"));
		Text += TEXT("        Ok(())\n")
			TEXT("    }\n")
			TEXT("\n");
	}

	/** The member keys a push carries when they changed: member_count, leader, open and, when shown, members. */
	void FCodegen::AppendRosterChanges(FString& Text) const
	{
		Text += TEXT("        let mut roster: Vec<(&str, Vec<u8>)> = Vec::new();\n")
			TEXT("        if self.roster.members.len() != before_roster.members.len() {\n")
			TEXT("            roster.push((\"member_count\", encode(&(self.roster.members.len() as u32))?));\n")
			TEXT("        }\n")
			TEXT("        if self.roster.leader != before_roster.leader {\n")
			TEXT("            roster.push((\"leader\", encode(&self.roster.leader)?));\n")
			TEXT("        }\n")
			TEXT("        if self.roster.open != before_roster.open {\n")
			TEXT("            roster.push((\"open\", encode(&self.roster.open)?));\n")
			TEXT("        }\n");
		if (!ShowsMembers())
		{
			return;
		}
		Text += TEXT("        if self.roster.members != before_roster.members {\n")
			TEXT("            Wire::check(&self.roster.members).map_err(|why| unsendable(\"members\", why))?;\n")
			TEXT("            roster.push((\"members\", encode(&self.roster.members)?));\n")
			TEXT("        }\n");
	}

	void FCodegen::AppendPreparePush(FString& Text) const
	{
		const TArray<int32> Watched = WatchedFields();
		if (!HasMembers())
		{
			AppendPreparePushWithoutMembers(Text, Watched);
			return;
		}
		const bool bRoster = KeepsRoster();
		Text += FString::Printf(TEXT("    fn prepare_push(&self, before: &%s%s) -> Result<Option<Vec<u8>>> {\n"), *StateName(),
			bRoster ? TEXT(", before_roster: &Roster") : TEXT(""));
		Text += Watched.IsEmpty() ? TEXT("        let changed: Vec<(&str, Vec<u8>)> = Vec::new();\n") : TEXT("        let mut changed: Vec<(&str, Vec<u8>)> = Vec::new();\n");
		AppendWatchedChanges(Text, Watched);
		if (bRoster)
		{
			AppendRosterChanges(Text);
		}
		Text += bRoster ? TEXT("        if changed.is_empty() && roster.is_empty() {\n") : TEXT("        if changed.is_empty() {\n");
		Text += TEXT("            return Ok(None);\n")
			TEXT("        }\n")
			TEXT("        // A push carries a subset of what a read carries, so a read that fits means the push fits too.\n")
			TEXT("        if self.read_reply(true, true, true)?.len() > MAX_MESSAGE {\n")
			TEXT("            return Err(unsendable(\"the watched values\", \"together they are larger than 1 MiB\"));\n")
			TEXT("        }\n");
		Text += WatchedElementsCheck(TEXT("self.state."), PushElementsRefusal);
		Text += TEXT("        let mut out = Vec::new();\n");
		// Readable By Members and Owner Only pushes say only that something changed; the reader reads again.
		if (Definition.Visibility != ECrowdyServerObjectVisibility::Public)
		{
			Text += TEXT("        write_map(&mut out, 2);\n")
				TEXT("        write_str(&mut out, \"epoch\");\n")
				TEXT("        write_uint(&mut out, self.epoch);\n")
				TEXT("        write_str(&mut out, \"seq\");\n")
				TEXT("        write_uint(&mut out, self.seq + 1);\n");
		}
		else
		{
			Text += bRoster ? TEXT("        write_map(&mut out, 3 + roster.len());\n") : TEXT("        write_map(&mut out, 3);\n");
			Text += TEXT("        write_str(&mut out, \"epoch\");\n")
				TEXT("        write_uint(&mut out, self.epoch);\n")
				TEXT("        write_str(&mut out, \"seq\");\n")
				TEXT("        write_uint(&mut out, self.seq + 1);\n")
				TEXT("        write_str(&mut out, \"fields\");\n")
				TEXT("        write_map(&mut out, changed.len());\n")
				TEXT("        for (name, bytes) in &changed {\n")
				TEXT("            write_str(&mut out, name);\n")
				TEXT("            out.extend_from_slice(bytes);\n")
				TEXT("        }\n");
			Text += bRoster ? TEXT("        for (name, bytes) in &roster {\n")
				TEXT("            write_str(&mut out, name);\n")
				TEXT("            out.extend_from_slice(bytes);\n")
				TEXT("        }\n") : TEXT("");
		}
		Text += TEXT("        Ok(Some(out))\n")
			TEXT("    }\n");
	}

	void FCodegen::AppendPreparePushWithoutMembers(FString& Text, const TArray<int32>& Watched) const
	{
		if (Watched.IsEmpty())
		{
			Text += FString::Printf(TEXT("    fn prepare_push(&self, _before: &%s) -> Result<Option<Vec<u8>>> {\n"), *StateName());
			Text += TEXT("        Ok(None)\n    }\n");
			return;
		}
		Text += FString::Printf(TEXT("    fn prepare_push(&self, before: &%s) -> Result<Option<Vec<u8>>> {\n"), *StateName());
		Text += TEXT("        let mut changed: Vec<(&str, Vec<u8>)> = Vec::new();\n");
		AppendWatchedChanges(Text, Watched);
		Text += TEXT("        if changed.is_empty() {\n")
			TEXT("            return Ok(None);\n")
			TEXT("        }\n")
			TEXT("        // A push carries a subset of what a read carries, so a read that fits means the push fits too.\n")
			TEXT("        if self.read_reply()?.len() > MAX_MESSAGE {\n")
			TEXT("            return Err(unsendable(\"the watched values\", \"together they are larger than 1 MiB\"));\n")
			TEXT("        }\n");
		Text += WatchedElementsCheck(TEXT("self.state."), PushElementsRefusal);
		Text += TEXT("        let mut out = Vec::new();\n")
			TEXT("        write_map(&mut out, if OWNER_ONLY { 2 } else { 3 });\n")
			TEXT("        write_str(&mut out, \"epoch\");\n")
			TEXT("        write_uint(&mut out, self.epoch);\n")
			TEXT("        write_str(&mut out, \"seq\");\n")
			TEXT("        write_uint(&mut out, self.seq + 1);\n")
			TEXT("        if !OWNER_ONLY {\n")
			TEXT("            write_str(&mut out, \"fields\");\n")
			TEXT("            write_map(&mut out, changed.len());\n")
			TEXT("            for (name, bytes) in &changed {\n")
			TEXT("                write_str(&mut out, name);\n")
			TEXT("                out.extend_from_slice(bytes);\n")
			TEXT("            }\n")
			TEXT("        }\n")
			TEXT("        Ok(Some(out))\n")
			TEXT("    }\n");
	}

	void FCodegen::AppendServerObjectImpl(FString& Text) const
	{
		const bool bTimers = !TimerInfos.IsEmpty();
		const bool bRoster = KeepsRoster();
		Text += TEXT("impl ServerObject {\n");
		Text += bTimers ? TEXT("    // A failed dispatch, an unsendable change or a timer that cannot start leaves the state as it was and publishes nothing.\n")
			: TEXT("    // A failed dispatch, or a change that could not be sent, leaves the state as it was and publishes nothing.\n");
		Text += FString::Printf(TEXT("    fn finish<T>(&mut self, ctx: &Ctx, before: %s, result: Result<T>) -> Result<T> {\n"), *StateName());
		Text += bRoster ? TEXT("        let before_roster = std::mem::replace(&mut self.roster, members::take());\n") : TEXT("");
		Text += bTimers ? TEXT("        let queued = timers::close();\n") : TEXT("");
		Text += TEXT("        // Everything that can fail runs before the change is kept and pushed.\n")
			TEXT("        let prepared = result.and_then(|value| {\n");
		Text += bRoster ? TEXT("            let push = self.prepare_push(&before, &before_roster)?;\n") : TEXT("            let push = self.prepare_push(&before)?;\n");
		Text += bTimers ? TEXT("            timers::arm_queued(ctx, &queued)?;\n") : TEXT("");
		Text += TEXT("            Ok((value, push))\n")
			TEXT("        });\n")
			TEXT("        let (value, push) = match prepared {\n")
			TEXT("            Ok(prepared) => prepared,\n")
			TEXT("            Err(error) => {\n")
			TEXT("                self.state = before;\n");
		Text += bRoster ? TEXT("                self.roster = before_roster;\n") : TEXT("");
		Text += TEXT("                return Err(error);\n")
			TEXT("            }\n")
			TEXT("        };\n")
			TEXT("        if let Some(out) = push {\n")
			TEXT("            self.seq += 1;\n")
			TEXT("            let _ = ctx.publish(\"state\", &out);\n")
			TEXT("        }\n");
		Text += bTimers ? TEXT("        timers::stop_queued(ctx, &queued);\n") : TEXT("");
		Text += TEXT("        Ok(value)\n")
			TEXT("    }\n")
			TEXT("\n");
		AppendCheckWatched(Text);
		if (HasMembers())
		{
			AppendReadFor(Text);
		}
		if (HasRead())
		{
			AppendReadReply(Text);
		}
		AppendPreparePush(Text);
		Text += TEXT("}\n\n");
	}

	const TCHAR* const AuthorizeHelper =
		TEXT("// Players may call only player-callable functions, and on an owner-only type only the owner; developer tools and other\n")
		TEXT("// server code may call everything.\n")
		TEXT("fn authorize(ctx: &Ctx, call: &Call<'_>, player_callable: bool) -> Result<()> {\n")
		TEXT("    let Ok(player) = call.player() else {\n")
		TEXT("        return Ok(());\n")
		TEXT("    };\n")
		TEXT("    if !player_callable {\n")
		TEXT("        return Err(Error::new(\"denied: players may not call this function\"));\n")
		TEXT("    }\n")
		TEXT("    if OWNER_ONLY && player.to_string() != ctx.key {\n")
		TEXT("        return Err(Error::new(\"denied: only the owner may use this Server Object\"));\n")
		TEXT("    }\n")
		TEXT("    Ok(())\n")
		TEXT("}\n")
		TEXT("\n");

	const TCHAR* const UserIdHelper =
		TEXT("fn is_user_id(key: &str) -> bool {\n")
		TEXT("    key.parse::<u64>().map(|id| id.to_string() == key).unwrap_or(false)\n")
		TEXT("}\n")
		TEXT("\n");

	const TCHAR* const RuleItem =
		TEXT("#[derive(Clone, Copy, PartialEq, Eq)]\n")
		TEXT("enum Rule {\n")
		TEXT("    Players,\n")
		TEXT("    Members,\n")
		TEXT("    Leader,\n")
		TEXT("    ServerOnly,\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("// Players may call a function as its Callable By allows, and on an owner-only type only the owner; developer tools and\n")
		TEXT("// other server code may call everything.\n");

	const TCHAR* const RosterItems =
		TEXT("fn open_by_default() -> bool {\n")
		TEXT("    true\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("// The members in the order they joined, the leader, and whether players may join; saved with the state.\n")
		TEXT("#[derive(Serialize, Deserialize, Clone, PartialEq, Debug)]\n")
		TEXT("struct Roster {\n")
		TEXT("    members: Vec<u64>,\n")
		TEXT("    leader: Option<u64>,\n")
		TEXT("    open: bool,\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("impl Default for Roster {\n")
		TEXT("    fn default() -> Self {\n")
		TEXT("        Self { members: Vec::new(), leader: None, open: true }\n")
		TEXT("    }\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("impl Roster {\n")
		TEXT("    fn add(&mut self, player: u64) -> Result<()> {\n")
		TEXT("        if self.members.contains(&player) {\n")
		TEXT("            return Ok(());\n")
		TEXT("        }\n")
		TEXT("        if !self.open {\n")
		TEXT("            return Err(Error::new(\"It is not open for joining\"));\n")
		TEXT("        }\n")
		TEXT("        if MAX_MEMBERS > 0 && self.members.len() >= MAX_MEMBERS {\n")
		TEXT("            return Err(Error::new(\"It is full\"));\n")
		TEXT("        }\n")
		TEXT("        self.members.push(player);\n")
		TEXT("        if self.leader.is_none() {\n")
		TEXT("            self.leader = Some(player);\n")
		TEXT("        }\n")
		TEXT("        Ok(())\n")
		TEXT("    }\n")
		TEXT("\n")
		TEXT("    fn remove(&mut self, player: u64) {\n")
		TEXT("        if !self.members.contains(&player) {\n")
		TEXT("            return;\n")
		TEXT("        }\n")
		TEXT("        self.members.retain(|member| *member != player);\n")
		TEXT("        if self.leader == Some(player) {\n")
		TEXT("            self.leader = self.members.first().copied();\n")
		TEXT("        }\n")
		TEXT("        // The last member leaving opens joining again, so nobody can close an empty object for good.\n")
		TEXT("        if self.members.is_empty() {\n")
		TEXT("            self.open = true;\n")
		TEXT("        }\n")
		TEXT("    }\n")
		TEXT("\n")
		TEXT("    fn make_leader(&mut self, player: u64) -> Result<()> {\n")
		TEXT("        if !self.members.contains(&player) {\n")
		TEXT("            return Err(Error::new(\"Only a member can be made the leader\"));\n")
		TEXT("        }\n")
		TEXT("        self.leader = Some(player);\n")
		TEXT("        Ok(())\n")
		TEXT("    }\n")
		TEXT("}\n")
		TEXT("\n");

	/** The built-in member functions' inputs, in the glue and in a calls module. */
	const TCHAR* const MemberInputItems =
		TEXT("#[derive(Serialize, Deserialize, Clone, PartialEq, Debug, Default)]\n")
		TEXT("#[serde(default)]\n")
		TEXT("pub struct MemberInputs {\n")
		TEXT("    pub Player: u64,\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("#[derive(Serialize, Deserialize, Clone, PartialEq, Debug)]\n")
		TEXT("#[serde(default)]\n")
		TEXT("pub struct OpenInputs {\n")
		TEXT("    pub bOpen: bool,\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("impl Default for OpenInputs {\n")
		TEXT("    fn default() -> Self {\n")
		TEXT("        Self { bOpen: true }\n")
		TEXT("    }\n")
		TEXT("}\n");

	const TCHAR* const MembersModule =
		TEXT("// The members, for logic.rs: the glue lends them to server code for one dispatch and takes them back after, so a failed\n")
		TEXT("// dispatch keeps the old ones.\n")
		TEXT("pub mod members {\n")
		TEXT("    use super::*;\n")
		TEXT("    use std::cell::RefCell;\n")
		TEXT("\n")
		TEXT("    thread_local! {\n")
		TEXT("        static ROSTER: RefCell<Roster> = RefCell::new(Roster::default());\n")
		TEXT("        static OUTER: RefCell<Vec<Roster>> = const { RefCell::new(Vec::new()) };\n")
		TEXT("    }\n")
		TEXT("\n")
		TEXT("    // A dispatch nested in another's call to this type sets the outer one's members aside until it finishes.\n")
		TEXT("    pub(crate) fn lend(roster: &Roster) {\n")
		TEXT("        let outer = ROSTER.with(|cell| std::mem::replace(&mut *cell.borrow_mut(), Clone::clone(roster)));\n")
		TEXT("        OUTER.with(|stack| stack.borrow_mut().push(outer));\n")
		TEXT("    }\n")
		TEXT("\n")
		TEXT("    pub(crate) fn take() -> Roster {\n")
		TEXT("        let outer = OUTER.with(|stack| stack.borrow_mut().pop()).unwrap_or_default();\n")
		TEXT("        ROSTER.with(|cell| std::mem::replace(&mut *cell.borrow_mut(), outer))\n")
		TEXT("    }\n")
		TEXT("\n")
		TEXT("    /// The members in the order they joined.\n")
		TEXT("    pub fn list() -> Vec<u64> {\n")
		TEXT("        ROSTER.with(|cell| cell.borrow().members.clone())\n")
		TEXT("    }\n")
		TEXT("\n")
		TEXT("    pub fn leader() -> Option<u64> {\n")
		TEXT("        ROSTER.with(|cell| cell.borrow().leader)\n")
		TEXT("    }\n")
		TEXT("\n")
		TEXT("    pub fn is_member(player: u64) -> bool {\n")
		TEXT("        ROSTER.with(|cell| cell.borrow().members.contains(&player))\n")
		TEXT("    }\n")
		TEXT("\n")
		TEXT("    pub fn is_open() -> bool {\n")
		TEXT("        ROSTER.with(|cell| cell.borrow().open)\n")
		TEXT("    }\n")
		TEXT("\n")
		TEXT("    pub fn set_open(open: bool) {\n")
		TEXT("        ROSTER.with(|cell| cell.borrow_mut().open = open);\n")
		TEXT("    }\n")
		TEXT("\n")
		TEXT("    /// Refused when joining is closed or every place is taken; the first member leads.\n")
		TEXT("    pub fn add(player: u64) -> Result<()> {\n")
		TEXT("        ROSTER.with(|cell| cell.borrow_mut().add(player))\n")
		TEXT("    }\n")
		TEXT("\n")
		TEXT("    /// When the leader goes, the longest-standing member leads.\n")
		TEXT("    pub fn remove(player: u64) {\n")
		TEXT("        ROSTER.with(|cell| cell.borrow_mut().remove(player));\n")
		TEXT("    }\n")
		TEXT("\n")
		TEXT("    pub fn make_leader(player: u64) -> Result<()> {\n")
		TEXT("        ROSTER.with(|cell| cell.borrow_mut().make_leader(player))\n")
		TEXT("    }\n")
		TEXT("}\n")
		TEXT("\n");

	const TCHAR* const TeamItems =
		TEXT("fn team_id(ctx: &Ctx) -> u64 {\n")
		TEXT("    ctx.key.parse().unwrap_or(0)\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("fn team_check(ctx: &Ctx, player: u64, permission: Option<&str>) -> Result<bool> {\n")
		TEXT("    ctx.players()\n")
		TEXT("        .check_permission(player, team_id(ctx), permission)\n")
		TEXT("        .map_err(|error| Error::new(&format!(\"denied: membership could not be checked ({error})\")))\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("// The members, for logic.rs: the Crowdy Team's whose id is this object's Instance Id.\n")
		TEXT("pub mod members {\n")
		TEXT("    use super::*;\n")
		TEXT("\n")
		TEXT("    pub fn is_member_of_team(ctx: &Ctx, player: u64) -> bool {\n")
		TEXT("        team_check(ctx, player, None).unwrap_or(false)\n")
		TEXT("    }\n")
		TEXT("}\n")
		TEXT("\n");

	const TCHAR* const CooldownItems =
		TEXT("type Cooldowns = BTreeMap<String, BTreeMap<u64, u64>>;\n")
		TEXT("\n")
		TEXT("const MAX_COOLDOWN_PLAYERS: usize = 10000;\n")
		TEXT("\n")
		TEXT("fn check_cooldown(cooldowns: &Cooldowns, ctx: &Ctx, call: &Call<'_>, name: &str, period_ms: u64) -> Result<()> {\n")
		TEXT("    let Ok(player) = call.player() else {\n")
		TEXT("        return Ok(());\n")
		TEXT("    };\n")
		TEXT("    let Some(last) = cooldowns.get(call.method).and_then(|players| players.get(&player)) else {\n")
		TEXT("        return Ok(());\n")
		TEXT("    };\n")
		TEXT("    let now = ctx.now_ms() as u64;\n")
		TEXT("    let ready = last.saturating_add(period_ms);\n")
		TEXT("    if now >= ready {\n")
		TEXT("        return Ok(());\n")
		TEXT("    }\n")
		TEXT("    Err(Error::new(&format!(\"cooldown: {name} can be called again in {} s\", (ready - now).div_ceil(1000))))\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("// Kept after a call that succeeded; entries past their cooldown go, then the oldest past the cap.\n")
		TEXT("fn record_cooldown(cooldowns: &mut Cooldowns, ctx: &Ctx, call: &Call<'_>, period_ms: u64) {\n")
		TEXT("    let Ok(player) = call.player() else {\n")
		TEXT("        return;\n")
		TEXT("    };\n")
		TEXT("    let now = ctx.now_ms() as u64;\n")
		TEXT("    let players = cooldowns.entry(call.method.to_string()).or_default();\n")
		TEXT("    players.retain(|_, last| last.saturating_add(period_ms) > now);\n")
		TEXT("    players.insert(player, now);\n")
		TEXT("    if players.len() <= MAX_COOLDOWN_PLAYERS {\n")
		TEXT("        return;\n")
		TEXT("    }\n")
		TEXT("    if let Some(oldest) = players.iter().min_by_key(|(_, last)| **last).map(|(player, _)| *player) {\n")
		TEXT("        players.remove(&oldest);\n")
		TEXT("    }\n")
		TEXT("}\n")
		TEXT("\n");

	void FCodegen::AppendAuthorize(FString& Text) const
	{
		if (!HasMembers())
		{
			Text += AuthorizeHelper;
			return;
		}
		const bool bRoster = KeepsRoster();
		Text += RuleItem;
		Text += FString::Printf(TEXT("fn authorize(ctx: &Ctx, call: &Call<'_>, %srule: Rule) -> Result<()> {\n"), bRoster ? TEXT("roster: &Roster, ") : TEXT(""));
		Text += TEXT("    let Ok(player) = call.player() else {\n")
			TEXT("        return Ok(());\n")
			TEXT("    };\n")
			TEXT("    if rule == Rule::ServerOnly {\n")
			TEXT("        return Err(Error::new(\"denied: players may not call this function\"));\n")
			TEXT("    }\n")
			TEXT("    if OWNER_ONLY && player.to_string() != ctx.key {\n")
			TEXT("        return Err(Error::new(\"denied: only the owner may use this Server Object\"));\n")
			TEXT("    }\n");
		Text += FString::Printf(TEXT("    if rule == Rule::Members && %s {\n"), bRoster ? TEXT("!roster.members.contains(&player)") : TEXT("!team_check(ctx, player, None)?"));
		Text += TEXT("        return Err(Error::new(&format!(\"denied: only members may call {}\", function_name(call.method))));\n")
			TEXT("    }\n");
		Text += FString::Printf(TEXT("    if rule == Rule::Leader && %s {\n"), bRoster ? TEXT("roster.leader != Some(player)") : TEXT("!team_check(ctx, player, Some(\"manage_group\"))?"));
		Text += TEXT("        return Err(Error::new(&format!(\"denied: only the leader may call {}\", function_name(call.method))));\n")
			TEXT("    }\n")
			TEXT("    Ok(())\n")
			TEXT("}\n")
			TEXT("\n");

		// Refusals name a function as Unreal does, since the player reads them there.
		Text += TEXT("fn function_name(method: &str) -> &str {\n")
			TEXT("    match method {\n");
		TArray<FString> Named;
		for (const FFunctionInfo& Function : FunctionInfos)
		{
			Named.Add(Function.Method);
			Text += FString::Printf(TEXT("        %s => %s,\n"), *RustStrLiteral(Function.Method), *RustStrLiteral(Function.Name));
		}
		for (const FCrowdyServerFunction& BuiltIn : bRoster ? UCrowdyServerObjectDefinition::GetMemberFunctions() : TConstArrayView<FCrowdyServerFunction>())
		{
			const FString Method = BuiltIn.GetMethodName();
			if (Named.Contains(Method))
			{
				continue;
			}
			Text += FString::Printf(TEXT("        %s => %s,\n"), *RustStrLiteral(Method), *RustStrLiteral(BuiltIn.Name.ToString()));
		}
		Text += TEXT("        other => other,\n")
			TEXT("    }\n")
			TEXT("}\n")
			TEXT("\n");
	}

	/** ServerObject and its snapshot, with the members and cooldowns when the definition uses them. */
	void FCodegen::AppendObjectStructs(FString& Text) const
	{
		const bool bRoster = KeepsRoster();
		const bool bCooldowns = UsesCooldowns();
		Text += TEXT("#[derive(Serialize, Deserialize, Default)]\n")
			TEXT("pub struct ServerObject {\n");
		Text += FString::Printf(TEXT("    state: %s,\n"), *StateName());
		Text += TEXT("    epoch: u64,\n")
			TEXT("    seq: u64,\n");
		Text += bRoster ? TEXT("    roster: Roster,\n") : TEXT("");
		Text += bCooldowns ? TEXT("    cooldowns: Cooldowns,\n") : TEXT("");
		Text += UsesSessions() ? TEXT("    sessions: BTreeMap<u64, u32>,\n") : TEXT("");
		Text += TEXT("}\n")
			TEXT("\n")
			TEXT("#[derive(Serialize)]\n")
			TEXT("struct SnapshotOut<'a> {\n")
			TEXT("    contract: u64,\n");
		Text += FString::Printf(TEXT("    state: &'a %s,\n"), *StateName());
		Text += bRoster ? TEXT("    members: &'a Vec<u64>,\n    leader: Option<u64>,\n    open: bool,\n") : TEXT("");
		Text += bCooldowns ? TEXT("    cooldowns: &'a Cooldowns,\n") : TEXT("");
		Text += UsesSessions() ? TEXT("    sessions: &'a BTreeMap<u64, u32>,\n") : TEXT("");
		Text += TEXT("}\n")
			TEXT("\n")
			TEXT("#[derive(Deserialize)]\n")
			TEXT("struct SnapshotIn {\n")
			TEXT("    contract: u64,\n");
		Text += FString::Printf(TEXT("    state: %s,\n"), *StateName());
		Text += bRoster ? TEXT("    #[serde(default)]\n")
			TEXT("    members: Vec<u64>,\n")
			TEXT("    #[serde(default)]\n")
			TEXT("    leader: Option<u64>,\n")
			TEXT("    #[serde(default = \"open_by_default\")]\n")
			TEXT("    open: bool,\n") : TEXT("");
		Text += bCooldowns ? TEXT("    #[serde(default)]\n    cooldowns: Cooldowns,\n") : TEXT("");
		Text += UsesSessions() ? TEXT("    #[serde(default)]\n    sessions: BTreeMap<u64, u32>,\n") : TEXT("");
		Text += TEXT("}\n\n");
	}

	void FCodegen::AppendFeatureItems(FString& Text) const
	{
		if (KeepsRoster())
		{
			Text += FString::Printf(TEXT("const MAX_MEMBERS: usize = %d;\n\n"), FMath::Max(0, Definition.MaxMembers));
			Text += RosterItems;
			Text += MemberInputItems;
			Text += TEXT("\n");
			Text += MembersModule;
		}
		Text += IsTeam() ? TeamItems : TEXT("");
		Text += UsesCooldowns() ? CooldownItems : TEXT("");
		Text += UsesSessions() ? TEXT("const MAX_SESSION_PLAYERS: usize = 100000;\n\n") : TEXT("");
		if (!TimerInfos.IsEmpty())
		{
			AppendTimersModule(Text);
		}
		Text += CallsModule;
	}

	void FCodegen::AppendTimersModule(FString& Text) const
	{
		Text += TEXT("// Starts and stops the definition's timers, with the period set there; from server code, once its dispatch is kept.\n")
			TEXT("pub mod timers {\n")
			TEXT("    use super::*;\n")
			TEXT("    use std::cell::RefCell;\n")
			TEXT("\n")
			TEXT("    // Each timer's Name, so a misspelt one in start or stop does not build.\n");
		for (const FTimerInfo& Timer : TimerInfos)
		{
			Text += FString::Printf(TEXT("    pub const %s: &str = %s;\n"), *Timer.Constant, *RustStrLiteral(Timer.Name));
		}
		Text += TEXT("\n")
			TEXT("    thread_local! {\n")
			TEXT("        static QUEUED: RefCell<Vec<Vec<(String, bool)>>> = const { RefCell::new(Vec::new()) };\n")
			TEXT("    }\n")
			TEXT("\n")
			TEXT("    pub(crate) fn open() {\n")
			TEXT("        QUEUED.with(|stack| stack.borrow_mut().push(Vec::new()));\n")
			TEXT("    }\n")
			TEXT("\n")
			TEXT("    // Only the last start or stop queued for a timer counts, so a stop queued after a start of it still wins.\n")
			TEXT("    pub(crate) fn close() -> Vec<(String, bool)> {\n")
			TEXT("        let queued = QUEUED.with(|stack| stack.borrow_mut().pop()).unwrap_or_default();\n")
			TEXT("        let mut last: Vec<(String, bool)> = Vec::new();\n")
			TEXT("        for (name, start) in queued {\n")
			TEXT("            last.retain(|(earlier, _)| *earlier != name);\n")
			TEXT("            last.push((name, start));\n")
			TEXT("        }\n")
			TEXT("        last\n")
			TEXT("    }\n")
			TEXT("\n")
			TEXT("    // False outside a dispatch, where a start or stop happens at once.\n")
			TEXT("    fn queue(name: &str, start: bool) -> bool {\n")
			TEXT("        QUEUED.with(|stack| stack.borrow_mut().last_mut().map(|queued| queued.push((name.to_string(), start))).is_some())\n")
			TEXT("    }\n")
			TEXT("\n")
			TEXT("    // Arms the queued starts in order; when one fails, those armed before it are cancelled and the dispatch is refused.\n")
			TEXT("    pub(crate) fn arm_queued(ctx: &Ctx, queued: &[(String, bool)]) -> Result<()> {\n")
			TEXT("        let starts: Vec<&str> = queued.iter().filter(|(_, start)| *start).map(|(name, _)| name.as_str()).collect();\n")
			TEXT("        for (index, name) in starts.iter().enumerate() {\n")
			TEXT("            let Err(error) = arm(ctx, name) else {\n")
			TEXT("                continue;\n")
			TEXT("            };\n")
			TEXT("            // A timer that was already running is cancelled here, not given back its earlier deadline.\n")
			TEXT("            for armed in &starts[..index] {\n")
			TEXT("                ctx.cancel_timer(armed);\n")
			TEXT("            }\n")
			TEXT("            return Err(Error::new(&format!(\"the change was not kept: timer {name} could not start: {error}\")));\n")
			TEXT("        }\n")
			TEXT("        Ok(())\n")
			TEXT("    }\n")
			TEXT("\n")
			TEXT("    pub(crate) fn stop_queued(ctx: &Ctx, queued: &[(String, bool)]) {\n")
			TEXT("        for (name, _) in queued.iter().filter(|(_, start)| !*start) {\n")
			TEXT("            ctx.cancel_timer(name);\n")
			TEXT("        }\n")
			TEXT("    }\n")
			TEXT("\n")
			TEXT("    pub(crate) fn arm(ctx: &Ctx, name: &str) -> Result<()> {\n")
			TEXT("        match name {\n");
		TArray<FString> Names;
		for (const FTimerInfo& Timer : TimerInfos)
		{
			const FString Name = RustStrLiteral(Timer.Name);
			Names.Add(Name);
			Text += FString::Printf(TEXT("            %s => ctx.%s(%s, %lld),\n"), *Name, Timer.bEvery ? TEXT("timer_every") : TEXT("timer_after"), *Name, Timer.Ms);
		}
		Text += TEXT("            other => Err(Error::new(&format!(\"no timer is named {other}\"))),\n")
			TEXT("        }\n")
			TEXT("    }\n")
			TEXT("\n")
			TEXT("    pub fn start(ctx: &Ctx, name: &str) -> Result<()> {\n");
		Text += FString::Printf(TEXT("        if !matches!(name, %s) || !queue(name, true) {\n"), *FString::Join(Names, TEXT(" | ")));
		Text += TEXT("            return arm(ctx, name);\n")
			TEXT("        }\n")
			TEXT("        Ok(())\n")
			TEXT("    }\n")
			TEXT("\n")
			TEXT("    /// In a dispatch the stop waits for its success and this answers true; elsewhere, whether the timer was running.\n")
			TEXT("    pub fn stop(ctx: &Ctx, name: &str) -> bool {\n")
			TEXT("        queue(name, false) || ctx.cancel_timer(name)\n")
			TEXT("    }\n")
			TEXT("}\n")
			TEXT("\n");
	}

	/** A typed call to another type's method: its inputs encoded, its reply decoded. */
	FString CallFunction(const FString& TypeName, const FString& Method, const FString& Ident, const FString& Inputs, const FString& Outputs)
	{
		FString Text = FString::Printf(TEXT("pub fn %s(ctx: &Ctx, key: &str%s) -> Result<%s> {\n"), *Ident,
			Inputs.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", inputs: &%s"), *Inputs), Outputs.IsEmpty() ? TEXT("()") : *Outputs);
		Text += Inputs.IsEmpty() ? TEXT("") : TEXT("    let payload = encode(inputs)?;\n");
		// The callee's error follows its name, so a callee's refusal never reads as the caller's own.
		const FString Call = FString::Printf(TEXT("ctx.call(\"%s\", key, \"%s\", %s).map_err(|error| Error::new(&format!(\"%s.%s failed: {error}\")))?"), *TypeName, *Method,
			Inputs.IsEmpty() ? TEXT("&[0x80]") : TEXT("&payload"), *TypeName, *Method);
		if (Outputs.IsEmpty())
		{
			return Text + TEXT("    ") + Call + TEXT(";\n    Ok(())\n}\n");
		}
		return Text + TEXT("    let reply = ") + Call + TEXT(";\n    decode(&reply)\n}\n");
	}

	bool FCodegen::BuildCalls()
	{
		TArray<FString> Types;
		FString Modules;
		for (const TObjectPtr<UCrowdyServerObjectDefinition>& Entry : Definition.CanCall)
		{
			const UCrowdyServerObjectDefinition* Callee = Entry.Get();
			if (!Callee || Callee == &Definition || Callee->TypeName.IsEmpty())
			{
				return Fail(TEXT("Can Call: every entry must be another Server Object definition with a Type Name"));
			}
			if (Types.Contains(Callee->TypeName))
			{
				continue;
			}
			Types.Add(Callee->TypeName);
			FString Module;
			if (!CalleeModule(*Callee, Module))
			{
				return false;
			}
			Modules += (Modules.IsEmpty() ? TEXT("") : TEXT("\n")) + Module;
		}
		if (!Modules.IsEmpty())
		{
			CallsModule = TEXT("// Calls to the Server Object types this one can call; what a call did stays done when the caller's dispatch fails.\n")
				TEXT("pub mod calls {\n") + Modules + TEXT("}\n\n");
		}
		return true;
	}

	/** The callee's inputs and outputs are its own structs, written again inside its module. */
	bool FCodegen::CalleeModule(const UCrowdyServerObjectDefinition& Callee, FString& Out)
	{
		FString LayoutError;
		const FCrowdyExecLayout* CalleeLayout = Callee.ResolveLayout(LayoutError);
		if (!CalleeLayout)
		{
			return Fail(FString::Printf(TEXT("Can Call %s: %s"), *Callee.TypeName, *LayoutError));
		}
		FCodegen Gen{Callee, *CalleeLayout};
		TArray<FString> Items;
		FString Ident;
		if (!Gen.AssignNames() || !Gen.CollectFunctions() || !Gen.TypesItems(Items) || !RustIdent(Callee.TypeName, Ident))
		{
			return Fail(FString::Printf(TEXT("Can Call %s: %s"), *Callee.TypeName, Gen.Error.IsEmpty() ? TEXT("its Type Name cannot be a name in the server code (Rust)") : *Gen.Error));
		}
		if (Gen.KeepsRoster())
		{
			Items.Add(MemberInputItems);
		}
		for (const FFunctionInfo& Function : Gen.FunctionInfos)
		{
			Items.Add(CallFunction(Callee.TypeName, Function.Method, Function.Ident, Function.Params, Function.Reply));
		}
		for (const FBuiltinMember& BuiltIn : BuiltinMembers)
		{
			// Join and Leave take the caller as the player, so only a player can call them.
			if (Gen.KeepsRoster() && BuiltIn.Inputs)
			{
				Items.Add(CallFunction(Callee.TypeName, BuiltIn.Method, BuiltIn.Method, BuiltIn.Inputs, FString()));
			}
		}
		Out = FString::Printf(TEXT("    pub mod %s {\n"), *Ident);
		Out += TEXT("        use ckx_sdk::prelude::*;\n")
			TEXT("        use serde::{Deserialize, Serialize};\n")
			TEXT("        use std::collections::{BTreeMap, BTreeSet};\n");
		for (const FString& Item : Items)
		{
			Out += TEXT("\n") + IndentLines(Item, TEXT("        "));
		}
		Out += TEXT("    }\n");
		return true;
	}

	const TCHAR* const LibHelpers =
		TEXT("fn bad_params(error: Error) -> Error {\n")
		TEXT("    let text = error.to_string();\n")
		TEXT("    Error::new(&format!(\"bad_params: {}\", text.strip_prefix(\"decode: \").unwrap_or(&text)))\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("fn write_field<T: Serialize>(out: &mut Vec<u8>, name: &str, value: &T) -> Result<()> {\n")
		TEXT("    write_str(out, name);\n")
		TEXT("    out.extend_from_slice(&encode(value)?);\n")
		TEXT("    Ok(())\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("fn write_map(out: &mut Vec<u8>, len: usize) {\n")
		TEXT("    if len < 16 {\n")
		TEXT("        out.push(0x80 | len as u8);\n")
		TEXT("    } else {\n")
		TEXT("        out.push(0xde);\n")
		TEXT("        out.extend_from_slice(&(len as u16).to_be_bytes());\n")
		TEXT("    }\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("fn write_str(out: &mut Vec<u8>, text: &str) {\n")
		TEXT("    let len = text.len();\n")
		TEXT("    if len < 32 {\n")
		TEXT("        out.push(0xa0 | len as u8);\n")
		TEXT("    } else if len < 256 {\n")
		TEXT("        out.push(0xd9);\n")
		TEXT("        out.push(len as u8);\n")
		TEXT("    } else {\n")
		TEXT("        out.push(0xda);\n")
		TEXT("        out.extend_from_slice(&(len as u16).to_be_bytes());\n")
		TEXT("    }\n")
		TEXT("    out.extend_from_slice(text.as_bytes());\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("fn write_uint(out: &mut Vec<u8>, value: u64) {\n")
		TEXT("    if value < 128 {\n")
		TEXT("        out.push(value as u8);\n")
		TEXT("    } else if value < 256 {\n")
		TEXT("        out.push(0xcc);\n")
		TEXT("        out.push(value as u8);\n")
		TEXT("    } else if value < 65536 {\n")
		TEXT("        out.push(0xcd);\n")
		TEXT("        out.extend_from_slice(&(value as u16).to_be_bytes());\n")
		TEXT("    } else if value < 4294967296 {\n")
		TEXT("        out.push(0xce);\n")
		TEXT("        out.extend_from_slice(&(value as u32).to_be_bytes());\n")
		TEXT("    } else {\n")
		TEXT("        out.push(0xcf);\n")
		TEXT("        out.extend_from_slice(&value.to_be_bytes());\n")
		TEXT("    }\n")
		TEXT("}\n")
		TEXT("\n");

	const TCHAR* const WireHelpers =
		TEXT("type Checked = std::result::Result<(), &'static str>;\n")
		TEXT("\n")
		TEXT("const MAX_MESSAGE: usize = 1024 * 1024;\n")
		TEXT("const MAX_ELEMENTS: usize = 4096;\n")
		TEXT("const MAX_MESSAGE_ELEMENTS: usize = 65536;\n")
		TEXT("const MAX_TEXT: usize = 65535;\n")
		TEXT("\n")
		TEXT("// Values an Unreal client would refuse to read: the glue keeps them out of watched fields, replies and accepted params.\n")
		TEXT("trait Wire {\n")
		TEXT("    fn check(&self) -> Checked;\n")
		TEXT("\n")
		TEXT("    // Elements of every list, set and map in the value, nested ones included, a map entry once: as an Unreal client counts them.\n")
		TEXT("    fn elements(&self) -> usize {\n")
		TEXT("        0\n")
		TEXT("    }\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("// How two set elements or map keys compare in Unreal, where text ignores case.\n")
		TEXT("trait WireKey: Ord {\n")
		TEXT("    fn folded(&self) -> Option<String> {\n")
		TEXT("        None\n")
		TEXT("    }\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("macro_rules! wire_always_ok {\n")
		TEXT("    ($($t:ty),*) => {\n")
		TEXT("        $(impl Wire for $t {\n")
		TEXT("            fn check(&self) -> Checked {\n")
		TEXT("                Ok(())\n")
		TEXT("            }\n")
		TEXT("        })*\n")
		TEXT("    };\n")
		TEXT("}\n")
		TEXT("wire_always_ok!(bool, i8, i16, i32, i64, u8, u16, u32, u64);\n")
		TEXT("\n")
		TEXT("macro_rules! wire_key_exact {\n")
		TEXT("    ($($t:ty),*) => {\n")
		TEXT("        $(impl WireKey for $t {})*\n")
		TEXT("    };\n")
		TEXT("}\n")
		TEXT("wire_key_exact!(i8, i16, i32, i64, u8, u16, u32, u64);\n")
		TEXT("\n")
		TEXT("impl Wire for f32 {\n")
		TEXT("    fn check(&self) -> Checked {\n")
		TEXT("        if self.is_finite() { Ok(()) } else { Err(\"a number is not finite\") }\n")
		TEXT("    }\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("impl Wire for f64 {\n")
		TEXT("    fn check(&self) -> Checked {\n")
		TEXT("        if self.is_finite() { Ok(()) } else { Err(\"a number is not finite\") }\n")
		TEXT("    }\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("impl Wire for String {\n")
		TEXT("    fn check(&self) -> Checked {\n")
		TEXT("        if self.len() <= MAX_TEXT { Ok(()) } else { Err(\"a text is longer than 65535 bytes\") }\n")
		TEXT("    }\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("impl WireKey for String {\n")
		TEXT("    // Unreal keys two texts as one when they have the same length and the same compared part.\n")
		TEXT("    fn folded(&self) -> Option<String> {\n")
		TEXT("        Some(format!(\"{}\\0{}\", compared_part(self), utf16_len(self)))\n")
		TEXT("    }\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("impl<T: Wire> Wire for Option<T> {\n")
		TEXT("    fn check(&self) -> Checked {\n")
		TEXT("        match self {\n")
		TEXT("            Some(value) => value.check(),\n")
		TEXT("            None => Ok(()),\n")
		TEXT("        }\n")
		TEXT("    }\n")
		TEXT("\n")
		TEXT("    fn elements(&self) -> usize {\n")
		TEXT("        self.as_ref().map_or(0, Wire::elements)\n")
		TEXT("    }\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("impl<T: Wire> Wire for Vec<T> {\n")
		TEXT("    fn check(&self) -> Checked {\n")
		TEXT("        if self.len() > MAX_ELEMENTS {\n")
		TEXT("            return Err(\"a list has more than 4096 elements\");\n")
		TEXT("        }\n")
		TEXT("        self.iter().try_for_each(|value| value.check())\n")
		TEXT("    }\n")
		TEXT("\n")
		TEXT("    fn elements(&self) -> usize {\n")
		TEXT("        self.len() + self.iter().map(Wire::elements).sum::<usize>()\n")
		TEXT("    }\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("impl<T: Wire + WireKey> Wire for BTreeSet<T> {\n")
		TEXT("    fn check(&self) -> Checked {\n")
		TEXT("        if self.len() > MAX_ELEMENTS {\n")
		TEXT("            return Err(\"a set has more than 4096 elements\");\n")
		TEXT("        }\n")
		TEXT("        self.iter().try_for_each(|value| value.check())?;\n")
		TEXT("        check_folded(self.iter())\n")
		TEXT("    }\n")
		TEXT("\n")
		TEXT("    fn elements(&self) -> usize {\n")
		TEXT("        self.len()\n")
		TEXT("    }\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("impl<K: Wire + WireKey, V: Wire> Wire for BTreeMap<K, V> {\n")
		TEXT("    fn check(&self) -> Checked {\n")
		TEXT("        if self.len() > MAX_ELEMENTS {\n")
		TEXT("            return Err(\"a map has more than 4096 entries\");\n")
		TEXT("        }\n")
		TEXT("        self.keys().try_for_each(|key| key.check())?;\n")
		TEXT("        self.values().try_for_each(|value| value.check())?;\n")
		TEXT("        check_folded(self.keys())\n")
		TEXT("    }\n")
		TEXT("\n")
		TEXT("    fn elements(&self) -> usize {\n")
		TEXT("        self.len() + self.values().map(Wire::elements).sum::<usize>()\n")
		TEXT("    }\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("fn check_folded<'a, K: WireKey + 'a>(keys: impl Iterator<Item = &'a K>) -> Checked {\n")
		TEXT("    let mut seen = BTreeSet::new();\n")
		TEXT("    for key in keys {\n")
		TEXT("        let Some(folded) = key.folded() else {\n")
		TEXT("            return Ok(());\n")
		TEXT("        };\n")
		TEXT("        if !seen.insert(folded) {\n")
		TEXT("            return Err(\"two keys are one key in Unreal, which ignores the case of ASCII letters\");\n")
		TEXT("        }\n")
		TEXT("    }\n")
		TEXT("    Ok(())\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("fn unsendable(what: &str, why: &str) -> Error {\n")
		TEXT("    Error::new(&format!(\"the change was not kept: {what}: {why}\"))\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("// Refuses a read until server code replaces the value, such as one restored from a save that players cannot read.\n")
		TEXT("fn unreadable((field, why): (&str, &str)) -> Error {\n")
		TEXT("    Error::new(&format!(\"unreadable: {field}: the server state holds a value players cannot read: {why}\"))\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("fn encode_reply<T: Serialize + Wire>(reply: &T) -> Result<Vec<u8>> {\n")
		TEXT("    Wire::check(reply).map_err(|why| unsendable(\"the reply\", why))?;\n")
		TEXT("    if reply.elements() > MAX_MESSAGE_ELEMENTS {\n")
		TEXT("        return Err(unsendable(\"the reply\", \"it holds more than 65536 elements\"));\n")
		TEXT("    }\n")
		TEXT("    let bytes = encode(reply)?;\n")
		TEXT("    if bytes.len() > MAX_MESSAGE {\n")
		TEXT("        return Err(unsendable(\"the reply\", \"it is larger than 1 MiB\"));\n")
		TEXT("    }\n")
		TEXT("    Ok(bytes)\n")
		TEXT("}\n")
		TEXT("\n");

	/** The Unreal kinds Rust holds as String or i64, checked as the client reads them. Only the client knows whether a gameplay tag exists, whether a path's root is mounted, and how much is left of its 1,048,576-character budget for names it has not seen before in this process (names and soft path parts). */
	const TCHAR* const KindHelpers =
		TEXT("const MAX_NAME: usize = 1023;\n")
		TEXT("\n")
		TEXT("// Unreal holds text as UTF-16, so it counts a text's length in UTF-16 units.\n")
		TEXT("fn utf16_len(text: &str) -> usize {\n")
		TEXT("    text.chars().map(char::len_utf16).sum()\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("// What Unreal compares of a text: the part before any NUL, ignoring the case of ASCII letters only.\n")
		TEXT("fn compared_part(text: &str) -> String {\n")
		TEXT("    text.split('\\0').next().unwrap_or(\"\").to_ascii_lowercase()\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("fn check_name(text: &str) -> Checked {\n")
		TEXT("    if utf16_len(text) <= MAX_NAME { Ok(()) } else { Err(\"a name is longer than 1023 characters\") }\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("// Unreal names that differ only in ASCII case or after a NUL are one name, and an empty name is None.\n")
		TEXT("fn check_name_keys<'a>(keys: impl Iterator<Item = &'a String>) -> Checked {\n")
		TEXT("    let mut seen = BTreeSet::new();\n")
		TEXT("    for key in keys {\n")
		TEXT("        let part = compared_part(key);\n")
		TEXT("        if !seen.insert(if part.is_empty() { String::from(\"none\") } else { part }) {\n")
		TEXT("            return Err(\"two names are one name in Unreal, which ignores the case of ASCII letters and reads an empty name as None\");\n")
		TEXT("        }\n")
		TEXT("    }\n")
		TEXT("    Ok(())\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("// Whether a tag exists only the client knows; one this long cannot.\n")
		TEXT("fn check_tag(text: &str) -> Checked {\n")
		TEXT("    if utf16_len(text) <= MAX_NAME { Ok(()) } else { Err(\"a gameplay tag is longer than 1023 characters\") }\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("// The forms FGuid::Parse reads besides its Base64 and Base36 ones, # standing for a hex digit.\n")
		TEXT("const GUID_FORMS: [&str; 6] = [\n")
		TEXT("    \"################################\",\n")
		TEXT("    \"########-####-####-####-############\",\n")
		TEXT("    \"{########-####-####-####-############}\",\n")
		TEXT("    \"(########-####-####-####-############)\",\n")
		TEXT("    \"########-########-########-########\",\n")
		TEXT("    \"{0x########,0x####,0x####,{0x##,0x##,0x##,0x##,0x##,0x##,0x##,0x##}}\",\n")
		TEXT("];\n")
		TEXT("\n")
		TEXT("fn check_guid(text: &str) -> Checked {\n")
		TEXT("    let bytes = text.as_bytes();\n")
		TEXT("    let fits = |form: &&str| {\n")
		TEXT("        form.len() == bytes.len() && form.bytes().zip(bytes).all(|(want, got)| if want == b'#' { got.is_ascii_hexdigit() } else { *got == want })\n")
		TEXT("    };\n")
		TEXT("    let base64 = bytes.len() == 22 && bytes.iter().all(|got| got.is_ascii_alphanumeric() || b\"+/-_\".contains(got));\n")
		TEXT("    let base36 = bytes.len() == 25 && bytes.iter().all(|got| got.is_ascii_digit() || got.is_ascii_uppercase());\n")
		TEXT("    if base64 || base36 || GUID_FORMS.iter().any(fits) { Ok(()) } else { Err(\"a GUID is not in a form Unreal reads\") }\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("// FPackageName::IsValidObjectPath, less its check that the path's root is mounted, which only the client knows.\n")
		TEXT("fn check_path(text: &str) -> Checked {\n")
		TEXT("    if utf16_len(text) > MAX_NAME {\n")
		TEXT("        return Err(\"an object path is longer than 1023 characters\");\n")
		TEXT("    }\n")
		TEXT("    let (package, object) = text.split_once('.').unwrap_or((text, \"\"));\n")
		TEXT("    let package_ok = utf16_len(package) >= 4 && package.starts_with('/') && !package.ends_with('/') && !package.contains(\"//\")\n")
		TEXT("        && !package.contains(|c: char| \"\\\\:*?\\\"<>|' ,.&!~\\n\\r\\t@#\".contains(c));\n")
		TEXT("    let object_ok = !object.contains(|c: char| \"\\\"' ,|&!~\\n\\r\\t@#(){}[]=;^%$`/\".contains(c)) && !object.ends_with(':');\n")
		TEXT("    if text.is_empty() || (package_ok && object_ok && !text.ends_with('.')) { Ok(()) } else { Err(\"a text is not an object path\") }\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("fn check_date(ms: &i64) -> Checked {\n")
		TEXT("    if (MIN_DATE_MS..=MAX_DATE_MS).contains(ms) { Ok(()) } else { Err(\"a date is outside the years 1 to 9999\") }\n")
		TEXT("}\n")
		TEXT("\n")
		TEXT("fn check_timespan(ms: &i64) -> Checked {\n")
		TEXT("    if (MIN_TIMESPAN_MS..=MAX_TIMESPAN_MS).contains(ms) { Ok(()) } else { Err(\"a timespan is longer than Unreal holds\") }\n")
		TEXT("}\n")
		TEXT("\n");

	/** The milliseconds the client reads into an FDateTime or FTimespan, worked out as it works them out. */
	FString TimeRanges()
	{
		const int64 UnixEpochTicks = FDateTime(1970, 1, 1).GetTicks();
		const int64 TicksPerMs = ETimespan::TicksPerMillisecond;
		return FString::Printf(TEXT("const MIN_DATE_MS: i64 = %lld;\nconst MAX_DATE_MS: i64 = %lld;\nconst MIN_TIMESPAN_MS: i64 = %lld;\nconst MAX_TIMESPAN_MS: i64 = %lld;\n"),
			-UnixEpochTicks / TicksPerMs, (FDateTime::MaxValue().GetTicks() - UnixEpochTicks) / TicksPerMs, MIN_int64 / TicksPerMs, MAX_int64 / TicksPerMs);
	}

	/** Checks are Rust Checked expressions on self; Elements is the sum of its fields' elements, empty when they hold none. */
	void AppendWireImpl(FString& Text, const FString& Name, TConstArrayView<FString> Checks, const FString& Elements = FString())
	{
		Text += FString::Printf(TEXT("impl Wire for %s {\n    fn check(&self) -> Checked {\n"), *Name);
		for (const FString& Check : Checks)
		{
			Text += FString::Printf(TEXT("        %s?;\n"), *Check);
		}
		Text += TEXT("        Ok(())\n    }\n");
		Text += Elements.IsEmpty() ? FString() : FString::Printf(TEXT("\n    fn elements(&self) -> usize {\n        %s\n    }\n"), *Elements);
		Text += TEXT("}\n\n");
	}

	void FCodegen::AppendWireImpls(FString& Text) const
	{
		for (const FString& Name : EnumNames)
		{
			AppendWireImpl(Text, Name, TConstArrayView<FString>());
			Text += FString::Printf(TEXT("impl WireKey for %s {}\n\n"), *Name);
		}
		for (const FEngineStruct& Engine : EngineStructs)
		{
			if (!UsesKind(Engine.Kind))
			{
				continue;
			}
			TArray<FString> Parts;
			for (const FEnginePart& Part : Engine.Parts)
			{
				Parts.Add(FString::Printf(TEXT("Wire::check(&self.%s)"), Part.Name));
			}
			AppendWireImpl(Text, Engine.Name, Parts);
		}
		for (int32 Index = 0; Index < StructNames.Num(); ++Index)
		{
			if (!IsBuiltinInputsOnly(Index))
			{
				AppendStructWireImpl(Text, Index, StructNames[Index]);
			}
		}
		for (const FListStruct& List : ListStructs)
		{
			AppendStructWireImpl(Text, List.StructIndex, List.Name);
		}
	}

	void FCodegen::AppendStructWireImpl(FString& Text, int32 StructIndex, const FString& Name) const
	{
		const TArray<FCrowdyExecField>& Fields = Layout.Structs[StructIndex].Fields;
		TArray<FString> Checks;
		TArray<int32> FieldIndexes;
		for (int32 Field = 0; Field < Fields.Num(); ++Field)
		{
			Checks.Add(FieldCheck(Fields[Field].Type, TEXT("self.") + FieldIdents[StructIndex][Field]));
			FieldIndexes.Add(Field);
		}
		AppendWireImpl(Text, Name, Checks, ElementsSum(StructIndex, FieldIndexes, TEXT("self.")));
	}

	FString FCodegen::LibFile() const
	{
		FString Text = FString::Printf(TEXT("// The Server Object type %s's glue, generated by the Crowdy SDK from its definition. Regenerate it; do not edit.\n"),
			*Definition.TypeName);
		Text += FString::Printf(TEXT("// Your code goes in logic.rs, which implements Functions on %s.\n"), *StateName());
		Text += TEXT("#![allow(non_snake_case, non_camel_case_types, dead_code, unused_imports, unused_variables)]\n")
			TEXT("\n")
			TEXT("mod logic;\n")
			TEXT("mod types;\n")
			TEXT("\n")
			TEXT("use ckx_sdk::prelude::*;\n")
			TEXT("use std::collections::{BTreeMap, BTreeSet};\n")
			TEXT("pub use types::*;\n")
			TEXT("\n")
			TEXT("const CONTRACT: u64 = 1;\n");
		Text += FString::Printf(TEXT("const OWNER_ONLY: bool = %s;\n\n"), IsOwnerOnly() ? TEXT("true") : TEXT("false"));
		AppendTrait(Text);
		AppendObjectStructs(Text);
		AppendFeatureItems(Text);
		AppendHub(Text);
		AppendServerObjectImpl(Text);
		AppendAuthorize(Text);
		Text += IsOwnerOnly() || IsTeam() ? UserIdHelper : TEXT("");
		Text += LibHelpers;
		Text += WireHelpers;
		Text += TimeRanges();
		Text += KindHelpers;
		AppendWireImpls(Text);
		Text += TEXT("ckx_sdk::export_hub!(ServerObject);\n");
		return Text;
	}

	TArray<CrowdyExecCodegen::FLogicStub> FCodegen::LogicStubs() const
	{
		TArray<CrowdyExecCodegen::FLogicStub> Stubs;
		for (const FFunctionInfo& Function : FunctionInfos)
		{
			const FString Body = FString::Printf(TEXT(" {\n        Err(Error::new(\"%s is not written yet\"))\n    }\n"), *Function.Method);
			Stubs.Add({Function.Ident, TEXT("    ") + FunctionSignature(Function) + Body});
		}
		// A timer or event runs on its own, so its stub succeeds rather than fail every time it fires.
		for (const FString& Handler : HandlerSignatures(*this))
		{
			Stubs.Add({MethodOf(Handler), TEXT("    ") + Handler + TEXT(" {\n        Ok(())\n    }\n")});
		}
		return Stubs;
	}

	FString FCodegen::LogicFile() const
	{
		FString Text = FString::Printf(TEXT("// Your server code for the Server Object type %s. The generator writes this file once and never again.\n"), *Definition.TypeName);
		Text += TEXT("use crate::*;\nuse ckx_sdk::prelude::*;\n\n");
		Text += FString::Printf(TEXT("impl Functions for %s {\n"), *StateName());
		TArray<FString> Bodies;
		for (const CrowdyExecCodegen::FLogicStub& Stub : LogicStubs())
		{
			Bodies.Add(Stub.Text);
		}
		Text += FString::Join(Bodies, TEXT("\n"));
		Text += TEXT("}\n");
		return Text;
	}

	struct FTypesItem
	{
		bool bEnum = false;
		FString Name;
		TArray<FString> Members;
		/** Each struct field's Rust type, parallel to Members; empty for an enum. */
		TArray<FString> Types;
		/** Each struct field's Unreal type from its trailing comment, parallel to Members; empty in files written before the comment. */
		TArray<FString> Comments;
		/** Each List value's id from its trailing comment, parallel to Members; empty for a struct's field and in files written before ids. */
		TArray<FString> Ids;
		/** For a `pub type Name = Target;` alias, the struct whose members it carries; empty otherwise. */
		FString Target;
	};

	FString WithoutRaw(const FString& Name)
	{
		return Name.StartsWith(TEXT("r#"), ESearchCase::CaseSensitive) ? Name.RightChop(2) : Name;
	}

	/** Opens an item at a `pub struct Name {` or `pub enum Name {` line; INDEX_NONE for any other line. */
	int32 OpenTypesItem(const FString& Line, TArray<FTypesItem>& Items)
	{
		const bool bEnum = Line.StartsWith(TEXT("pub enum "), ESearchCase::CaseSensitive);
		const bool bStruct = Line.StartsWith(TEXT("pub struct "), ESearchCase::CaseSensitive);
		if ((!bEnum && !bStruct) || !Line.EndsWith(TEXT(" {"), ESearchCase::CaseSensitive))
		{
			return INDEX_NONE;
		}
		const int32 Start = bEnum ? 9 : 11;
		FTypesItem& Item = Items.AddDefaulted_GetRef();
		Item.bEnum = bEnum;
		Item.Name = WithoutRaw(Line.Mid(Start, Line.Len() - Start - 2));
		return Items.Num() - 1;
	}

	/** Reads a `pub type Name = Target;` line as an item; false for any other line. */
	bool AddTypesAlias(const FString& Line, TArray<FTypesItem>& Items)
	{
		FString Alias = Line;
		FString Name;
		FString Target;
		if (!Alias.RemoveFromStart(TEXT("pub type "), ESearchCase::CaseSensitive) || !Alias.RemoveFromEnd(TEXT(";"), ESearchCase::CaseSensitive)
			|| !Alias.Split(TEXT(" = "), &Name, &Target, ESearchCase::CaseSensitive))
		{
			return false;
		}
		FTypesItem& Item = Items.AddDefaulted_GetRef();
		Item.Name = WithoutRaw(Name);
		Item.Target = WithoutRaw(Target);
		return true;
	}

	/** Gives each alias its target struct's members, so a List that shares a struct compares like one that has its own. */
	void ResolveTypesAliases(TArray<FTypesItem>& Items)
	{
		for (FTypesItem& Item : Items)
		{
			const FString& Target = Item.Target;
			const FTypesItem* Struct = Target.IsEmpty() ? nullptr : Items.FindByPredicate([&Target](const FTypesItem& Candidate)
			{
				return !Candidate.bEnum && Candidate.Target.IsEmpty() && Candidate.Name.Equals(Target, ESearchCase::CaseSensitive);
			});
			if (!Struct)
			{
				continue;
			}
			Item.Members = Struct->Members;
			Item.Types = Struct->Types;
			Item.Comments = Struct->Comments;
			Item.Ids = Struct->Ids;
		}
	}

	/** Removes a trailing ` id=<32 hex digits>` from a field comment and returns the digits; empty when there is none. */
	FString TakeValueId(FString& Comment)
	{
		FString UnrealType;
		FString Id;
		FGuid Parsed;
		if (!Comment.Split(TEXT(" id="), &UnrealType, &Id, ESearchCase::CaseSensitive, ESearchDir::FromEnd) || Id.Len() != 32 || !FGuid::ParseExact(Id, EGuidFormats::Digits, Parsed))
		{
			return FString();
		}
		Comment = UnrealType;
		return Id;
	}

	void AddTypesMember(const FString& Line, FTypesItem& Item)
	{
		FString Member = Line.TrimStartAndEnd();
		FString Code;
		FString Comment;
		const bool bCommented = Member.Split(TEXT(" // "), &Code, &Comment, ESearchCase::CaseSensitive);
		if (bCommented)
		{
			Member = Code;
		}
		if (!Member.RemoveFromEnd(TEXT(","), ESearchCase::CaseSensitive))
		{
			return;
		}
		if (Item.bEnum)
		{
			Item.Members.Add(WithoutRaw(Member));
			return;
		}
		FString Name;
		FString Type;
		if (!Member.RemoveFromStart(TEXT("pub "), ESearchCase::CaseSensitive) || !Member.Split(TEXT(": "), &Name, &Type, ESearchCase::CaseSensitive))
		{
			return;
		}
		Item.Members.Add(WithoutRaw(Name));
		Item.Types.Add(Type);
		Item.Ids.Add(bCommented ? TakeValueId(Comment) : FString());
		Item.Comments.Add(bCommented ? Comment : FString());
	}

	TArray<FTypesItem> ParseTypes(const FString& Text)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);
		TArray<FTypesItem> Items;
		int32 Open = INDEX_NONE;
		for (const FString& RawLine : Lines)
		{
			const FString Line = RawLine.TrimEnd();
			if (Open == INDEX_NONE)
			{
				Open = AddTypesAlias(Line, Items) ? INDEX_NONE : OpenTypesItem(Line, Items);
			}
			else if (Line.Equals(TEXT("}"), ESearchCase::CaseSensitive))
			{
				Open = INDEX_NONE;
			}
			else
			{
				AddTypesMember(Line, Items[Open]);
			}
		}
		ResolveTypesAliases(Items);
		return Items;
	}

	int32 FindTypesMember(const FTypesItem& Item, const FString& Name)
	{
		return Item.Members.IndexOfByPredicate([&Name](const FString& Member) { return Member.Equals(Name, ESearchCase::CaseSensitive); });
	}

	/** Where New has Old's List value Index under another name: the same id, its old name gone; INDEX_NONE otherwise. */
	int32 FindRenamedMember(const FTypesItem& Old, int32 Index, const FTypesItem& New)
	{
		if (!Old.Ids.IsValidIndex(Index) || Old.Ids[Index].IsEmpty() || FindTypesMember(New, Old.Members[Index]) != INDEX_NONE)
		{
			return INDEX_NONE;
		}
		const FString& Id = Old.Ids[Index];
		return New.Ids.IndexOfByPredicate([&Id](const FString& Candidate) { return Candidate.Equals(Id, ESearchCase::IgnoreCase); });
	}

	const FTypesItem* FindTypesItem(const TArray<FTypesItem>& Items, const FTypesItem& Old)
	{
		return Items.FindByPredicate([&Old](const FTypesItem& Item) { return Item.bEnum == Old.bEnum && Item.Name.Equals(Old.Name, ESearchCase::CaseSensitive); });
	}

	void AddRenames(const FTypesItem& Old, const FTypesItem& New, TArray<CrowdyExecCodegen::FListValueRename>& Out)
	{
		for (int32 Index = 0; Index < Old.Members.Num(); ++Index)
		{
			const int32 Found = FindRenamedMember(Old, Index, New);
			FGuid ValueId;
			if (Found != INDEX_NONE && FGuid::Parse(Old.Ids[Index], ValueId))
			{
				Out.Add(CrowdyExecCodegen::FListValueRename{Old.Name, Old.Members[Index], New.Members[Found], ValueId});
			}
		}
	}

	void ReportMembers(const FTypesItem& Old, const FTypesItem& New, TArray<FString>& Out)
	{
		for (int32 Index = 0; Index < Old.Members.Num(); ++Index)
		{
			const FString& Member = Old.Members[Index];
			const int32 ByName = FindTypesMember(New, Member);
			// A renamed List value is the same value; only a change of its type is reported.
			const int32 Found = ByName != INDEX_NONE ? ByName : FindRenamedMember(Old, Index, New);
			if (Found == INDEX_NONE)
			{
				Out.Add(Old.bEnum ? FString::Printf(TEXT("%s::%s is gone"), *Old.Name, *Member) : FString::Printf(TEXT("%s.%s is gone"), *Old.Name, *Member));
				continue;
			}
			if (Old.bEnum)
			{
				continue;
			}
			// The Unreal type sees changes the Rust type hides, such as a string that becomes a GUID; an older file has only the Rust type.
			const bool bCommented = !Old.Comments[Index].IsEmpty() && !New.Comments[Found].IsEmpty();
			const FString& OldType = bCommented ? Old.Comments[Index] : Old.Types[Index];
			const FString& NewType = bCommented ? New.Comments[Found] : New.Types[Found];
			if (!OldType.Equals(NewType, ESearchCase::CaseSensitive))
			{
				Out.Add(FString::Printf(TEXT("%s.%s changes type from %s to %s"), *Old.Name, *Member, *OldType, *NewType));
			}
		}
	}

	bool SameText(const TArray<FString>& A, const TArray<FString>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (!A[Index].Equals(B[Index], ESearchCase::CaseSensitive))
			{
				return false;
			}
		}
		return true;
	}

	/** Why New, named as Old was, now stands for another type: it became a struct of its own, or another name for New.Target. */
	FString DescribeIdentityChange(const FTypesItem& Old, const FTypesItem& New, const TArray<FTypesItem>& NewItems)
	{
		if (!New.Target.IsEmpty())
		{
			return FString::Printf(TEXT("%s is now another name for %s, since they now hold the same values with the same starting values; code that treats them as two types no longer builds."),
				*New.Name, *New.Target);
		}
		const FString& Target = Old.Target;
		const FTypesItem* Shared = NewItems.FindByPredicate([&Target](const FTypesItem& Item)
		{
			return !Item.bEnum && Item.Target.IsEmpty() && Item.Name.Equals(Target, ESearchCase::CaseSensitive);
		});
		if (Shared && SameText(Shared->Members, New.Members) && SameText(Shared->Types, New.Types))
		{
			return FString::Printf(TEXT("%s is now its own struct, since its starting values differ from those of %s; code that uses one for the other no longer builds."), *New.Name, *Target);
		}
		return FString::Printf(TEXT("%s is now its own struct rather than another name for %s; code that uses one for the other no longer builds."), *New.Name, *Target);
	}

	struct FPendingFile
	{
		FString Path;
		FString Temp;
	};

	void DeleteTemps(const TArray<FPendingFile>& Pending)
	{
		for (const FPendingFile& File : Pending)
		{
			IFileManager::Get().Delete(*File.Temp, false, false, true);
		}
	}

	/** Writes each file beside its destination with a .tmp suffix; src/logic.rs only when absent, since it holds the user's code. */
	bool WriteTemps(const CrowdyExecCodegen::FGeneratedCrate& Crate, const FString& Directory, TArray<FPendingFile>& OutPending, FString& OutError)
	{
		IFileManager& FileManager = IFileManager::Get();
		for (const CrowdyExecCodegen::FGeneratedFile& File : Crate.Files)
		{
			const FString Path = FPaths::Combine(Directory, File.Path);
			if (File.Path.Equals(TEXT("src/logic.rs"), ESearchCase::CaseSensitive) && FileManager.FileExists(*Path))
			{
				continue;
			}
			const FPendingFile& Pending = OutPending.Add_GetRef(FPendingFile{Path, Path + TEXT(".tmp")});
			const FString Folder = FPaths::GetPath(Path);
			const bool bFolder = FileManager.DirectoryExists(*Folder) || FileManager.MakeDirectory(*Folder, true);
			if (!bFolder || !FFileHelper::SaveStringToFile(File.Text, *Pending.Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutError = FString::Printf(TEXT("Could not write %s"), *Path);
				return false;
			}
		}
		return true;
	}

	bool IsIdentChar(TCHAR Char)
	{
		return IsAsciiDigit(Char) || IsAsciiUpper(Char) || (Char >= 'a' && Char <= 'z') || Char == '_';
	}

	TCHAR CharAt(const FString& Text, int32 Index)
	{
		return Text.IsValidIndex(Index) ? Text[Index] : TCHAR(0);
	}

	/** The end of the block comment opening at Start; Rust's block comments nest. */
	int32 BlockCommentEnd(const FString& Text, int32 Start)
	{
		int32 Depth = 0;
		for (int32 Index = Start; Index + 1 < Text.Len(); ++Index)
		{
			const bool bOpens = Text[Index] == '/' && Text[Index + 1] == '*';
			const bool bCloses = Text[Index] == '*' && Text[Index + 1] == '/';
			Depth += bOpens ? 1 : (bCloses ? -1 : 0);
			Index += bOpens || bCloses ? 1 : 0;
			if (bCloses && Depth == 0)
			{
				return Index + 1;
			}
		}
		return Text.Len();
	}

	/** The end of the string whose opening quote is at Start, past escaped quotes. */
	int32 StringEnd(const FString& Text, int32 Start)
	{
		for (int32 Index = Start + 1; Index < Text.Len(); Index += Text[Index] == '\\' ? 2 : 1)
		{
			if (Text[Index] == '"')
			{
				return Index + 1;
			}
		}
		return Text.Len();
	}

	/** The end of the raw string starting at Start (r"...", r#"..."#, br"..." or cr"..."), where nothing escapes; Start when none does. */
	int32 RawStringEnd(const FString& Text, int32 Start)
	{
		const TCHAR Char = CharAt(Text, Start);
		const int32 RawAt = Char == 'r' ? Start : ((Char == 'b' || Char == 'c') && CharAt(Text, Start + 1) == 'r' ? Start + 1 : INDEX_NONE);
		if (RawAt == INDEX_NONE || IsIdentChar(CharAt(Text, Start - 1)))
		{
			return Start;
		}
		int32 Hashes = 0;
		while (CharAt(Text, RawAt + 1 + Hashes) == '#')
		{
			++Hashes;
		}
		if (CharAt(Text, RawAt + 1 + Hashes) != '"')
		{
			return Start;
		}
		const FString Close = TEXT("\"") + FString::ChrN(Hashes, '#');
		const int32 Found = Text.Find(Close, ESearchCase::CaseSensitive, ESearchDir::FromStart, RawAt + 2 + Hashes);
		return Found == INDEX_NONE ? Text.Len() : Found + Close.Len();
	}

	/** The end of the character literal at Start; a lifetime or label such as '_ or 'outer never closes, so it gives Start. */
	int32 CharEnd(const FString& Text, int32 Start)
	{
		if (CharAt(Text, Start + 1) == '\\')
		{
			const int32 Found = Text.Find(TEXT("'"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start + 3);
			return Found == INDEX_NONE ? Text.Len() : Found + 1;
		}
		const bool bPair = CharAt(Text, Start + 1) >= 0xD800 && CharAt(Text, Start + 1) <= 0xDBFF;
		if (CharAt(Text, Start + 2) == '\'' || (bPair && CharAt(Text, Start + 3) == '\''))
		{
			return Start + (bPair ? 4 : 3);
		}
		return Start;
	}

	/** The end of the comment, string or character literal starting at Start, or Start when none starts there. */
	int32 LiteralEnd(const FString& Text, int32 Start)
	{
		const TCHAR Char = CharAt(Text, Start);
		const TCHAR Next = CharAt(Text, Start + 1);
		if (Char == '/' && Next == '/')
		{
			const int32 Break = Text.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
			return Break == INDEX_NONE ? Text.Len() : Break;
		}
		if (Char == '/' && Next == '*')
		{
			return BlockCommentEnd(Text, Start);
		}
		if (Char == '"')
		{
			return StringEnd(Text, Start);
		}
		return Char == '\'' ? CharEnd(Text, Start) : RawStringEnd(Text, Start);
	}

	/** Text with every comment, string and character literal turned to spaces, line breaks kept, so a search finds only code. */
	FString CodeOnly(const FString& Text)
	{
		FString Code = Text;
		int32 Index = 0;
		while (Index < Text.Len())
		{
			const int32 End = LiteralEnd(Text, Index);
			if (End == Index)
			{
				++Index;
				continue;
			}
			for (; Index < End; ++Index)
			{
				Code[Index] = Text[Index] == '\n' ? TCHAR('\n') : TCHAR(' ');
			}
		}
		return Code;
	}

	FString PlainIdent(const FString& Ident)
	{
		return Ident.StartsWith(TEXT("r#"), ESearchCase::CaseSensitive) ? Ident.RightChop(2) : Ident;
	}

	bool HasName(const TArray<FString>& Names, const FString& Name)
	{
		return Names.ContainsByPredicate([&Name](const FString& Other) { return Other.Equals(Name, ESearchCase::CaseSensitive); });
	}

	/** The function named by the `fn` at Index in Code, without r#; empty when no `fn` starts there. */
	FString FnNameAt(const FString& Code, int32 Index)
	{
		const bool bWord = !IsIdentChar(CharAt(Code, Index - 1)) && CharAt(Code, Index) == 'f' && CharAt(Code, Index + 1) == 'n';
		if (!bWord || !FChar::IsWhitespace(CharAt(Code, Index + 2)))
		{
			return FString();
		}
		int32 From = Index + 2;
		while (From < Code.Len() && FChar::IsWhitespace(Code[From]))
		{
			++From;
		}
		From += CharAt(Code, From) == 'r' && CharAt(Code, From + 1) == '#' ? 2 : 0;
		int32 To = From;
		while (To < Code.Len() && IsIdentChar(Code[To]))
		{
			++To;
		}
		return Code.Mid(From, To - From);
	}

	/** The opening brace of `impl Functions for <State> {` in Code, either name possibly a path; INDEX_NONE when there is none. */
	int32 FindImplOpen(const FString& Code, const FString& State)
	{
		int32 Index = 0;
		auto SkipSpace = [&Code, &Index]()
		{
			while (Index < Code.Len() && FChar::IsWhitespace(Code[Index]))
			{
				++Index;
			}
		};
		auto ReadWord = [&Code, &Index]()
		{
			const int32 From = Index;
			while (Index < Code.Len() && IsIdentChar(Code[Index]))
			{
				++Index;
			}
			return Code.Mid(From, Index - From);
		};
		auto AtPathSeparator = [&Code, &Index]() { return CharAt(Code, Index) == ':' && CharAt(Code, Index + 1) == ':'; };
		// The last segment of a path such as crate::Functions or ::crate::Functions.
		auto ReadPathEnd = [&Index, &SkipSpace, &ReadWord, &AtPathSeparator]()
		{
			SkipSpace();
			Index += AtPathSeparator() ? 2 : 0;
			FString Last = ReadWord();
			SkipSpace();
			while (!Last.IsEmpty() && AtPathSeparator())
			{
				Index += 2;
				SkipSpace();
				Last = ReadWord();
				SkipSpace();
			}
			return Last;
		};
		for (int32 Start = Code.Find(TEXT("impl"), ESearchCase::CaseSensitive); Start != INDEX_NONE; Start = Code.Find(TEXT("impl"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start + 1))
		{
			Index = Start + 4;
			if ((Start > 0 && IsIdentChar(Code[Start - 1])) || Index >= Code.Len() || !FChar::IsWhitespace(Code[Index]))
			{
				continue;
			}
			if (!ReadPathEnd().Equals(TEXT("Functions"), ESearchCase::CaseSensitive) || !ReadWord().Equals(TEXT("for"), ESearchCase::CaseSensitive))
			{
				continue;
			}
			if (!ReadPathEnd().Equals(State, ESearchCase::CaseSensitive))
			{
				continue;
			}
			// A where clause holds no braces, so the block opens at the next one.
			const int32 Open = ReadWord().Equals(TEXT("where"), ESearchCase::CaseSensitive) ? Code.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index) : Index;
			if (CharAt(Code, Open) == '{')
			{
				return Open;
			}
		}
		return INDEX_NONE;
	}

	/** The brace closing the block opened at Open, INDEX_NONE when it never closes; OutMethods gets each function defined directly inside. */
	int32 ScanBlock(const FString& Code, int32 Open, TArray<FString>& OutMethods)
	{
		int32 Depth = 0;
		for (int32 Index = Open; Index < Code.Len(); ++Index)
		{
			Depth += Code[Index] == '{' ? 1 : (Code[Index] == '}' ? -1 : 0);
			if (Depth == 0)
			{
				return Index;
			}
			const FString Name = Depth == 1 ? FnNameAt(Code, Index) : FString();
			if (!Name.IsEmpty() && !HasName(OutMethods, Name))
			{
				OutMethods.Add(Name);
			}
		}
		return INDEX_NONE;
	}

	/** Every function Code defines, at any depth. */
	TArray<FString> AllFnNames(const FString& Code)
	{
		TArray<FString> Names;
		for (int32 Index = 0; Index < Code.Len(); ++Index)
		{
			const FString Name = FnNameAt(Code, Index);
			if (!Name.IsEmpty() && !HasName(Names, Name))
			{
				Names.Add(Name);
			}
		}
		return Names;
	}

	const TCHAR* const CrateDefinitionPrefix = TEXT("definition = \"");

	/** Reads Directory's Cargo.toml into OutText and finds the definition path its [package.metadata.crowdy] records, still escaped, at OutStart for OutLength characters. False when the file or the entry is missing. */
	bool FindCrateDefinition(const FString& Directory, FString& OutText, int32& OutStart, int32& OutLength)
	{
		if (!FFileHelper::LoadFileToString(OutText, *FPaths::Combine(Directory, TEXT("Cargo.toml"))))
		{
			return false;
		}
		const int32 PrefixLength = FCString::Strlen(CrateDefinitionPrefix);
		bool bInSection = false;
		for (int32 LineStart = 0; LineStart < OutText.Len();)
		{
			const int32 Break = OutText.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, LineStart);
			const int32 LineEnd = Break == INDEX_NONE ? OutText.Len() : Break;
			const FString RawLine = OutText.Mid(LineStart, LineEnd - LineStart);
			const FString Line = RawLine.TrimStartAndEnd();
			const int32 ValueStart = LineStart + RawLine.Len() - RawLine.TrimStart().Len() + PrefixLength;
			LineStart = LineEnd + 1;
			if (Line.StartsWith(TEXT("["), ESearchCase::CaseSensitive))
			{
				bInSection = Line.Equals(TEXT("[package.metadata.crowdy]"), ESearchCase::CaseSensitive);
				continue;
			}
			if (bInSection && Line.Len() > PrefixLength && Line.StartsWith(CrateDefinitionPrefix, ESearchCase::CaseSensitive) && Line.EndsWith(TEXT("\""), ESearchCase::CaseSensitive))
			{
				OutStart = ValueStart;
				OutLength = Line.Len() - PrefixLength - 1;
				return true;
			}
		}
		return false;
	}
}

bool CrowdyExecCodegen::Generate(const UCrowdyServerObjectDefinition& Definition, FGeneratedCrate& OutCrate, FString& OutError)
{
	using namespace CrowdyExecCodegenDetail;
	OutCrate = FGeneratedCrate();
	const FCrowdyExecLayout* Layout = Definition.ResolveLayout(OutError);
	if (!Layout)
	{
		return false;
	}
	if (Layout->StateStruct == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("%s has no State struct"), *Definition.GetName());
		return false;
	}
	FCodegen Gen{Definition, *Layout};
	FString Types;
	if (!Gen.AssignNames() || !Gen.CollectFunctions() || !Gen.CollectTimers() || !Gen.TypesFile(Types) || !Gen.BuildCalls())
	{
		OutError = Gen.Error;
		return false;
	}
	OutCrate.Name = Definition.TypeName;
	OutCrate.StateName = Gen.StateName();
	OutCrate.Stubs = Gen.LogicStubs();
	OutCrate.Files.Add(FGeneratedFile{TEXT("Cargo.toml"), Gen.CargoFile()});
	OutCrate.Files.Add(FGeneratedFile{TEXT("src/lib.rs"), Gen.LibFile()});
	OutCrate.Files.Add(FGeneratedFile{TEXT("src/types.rs"), MoveTemp(Types)});
	if (!HasOwnLogicFile(Definition))
	{
		OutCrate.Files.Add(FGeneratedFile{TEXT("src/logic.rs"), Gen.LogicFile()});
	}
	return true;
}

CrowdyExecCodegen::FLogicGaps CrowdyExecCodegen::FindLogicGaps(const FGeneratedCrate& Crate, const FString& Logic)
{
	using namespace CrowdyExecCodegenDetail;
	FLogicGaps Gaps;
	const FString Code = CodeOnly(Logic);
	TArray<FString> Defined;
	const int32 Open = FindImplOpen(Code, Crate.StateName);
	Gaps.InsertAt = Open == INDEX_NONE ? INDEX_NONE : ScanBlock(Code, Open, Defined);
	// Without a whole impl block, any function of the file counts as written.
	if (Gaps.InsertAt == INDEX_NONE)
	{
		Defined = AllFnNames(Code);
	}
	TArray<FString> Asked;
	for (const FLogicStub& Stub : Crate.Stubs)
	{
		Asked.Add(PlainIdent(Stub.Method));
		if (!HasName(Defined, Asked.Last()))
		{
			Gaps.Missing.Add(Stub);
		}
	}
	if (Gaps.InsertAt == INDEX_NONE)
	{
		return Gaps;
	}
	for (const FString& Name : Defined)
	{
		if (!HasName(Asked, Name) && !IsOneOf(Name, ProvidedMethods))
		{
			Gaps.Leftover.Add(Name);
		}
	}
	return Gaps;
}

FString CrowdyExecCodegen::AddLogicStubs(const FString& Logic, const FLogicGaps& Gaps)
{
	using namespace CrowdyExecCodegenDetail;
	if (Gaps.Missing.IsEmpty() || !Logic.IsValidIndex(Gaps.InsertAt))
	{
		return Logic;
	}
	int32 LineStart = Gaps.InsertAt;
	while (LineStart > 0 && (Logic[LineStart - 1] == ' ' || Logic[LineStart - 1] == '\t'))
	{
		--LineStart;
	}
	const bool bOwnLine = LineStart == 0 || Logic[LineStart - 1] == '\n';
	const bool bEmptyBlock = CodeOnly(Logic.Left(Gaps.InsertAt)).TrimEnd().EndsWith(TEXT("{"), ESearchCase::CaseSensitive);
	TArray<FString> Bodies;
	for (const FLogicStub& Stub : Gaps.Missing)
	{
		Bodies.Add(Stub.Text);
	}
	const FString Stubs = (bEmptyBlock ? FString() : FString(TEXT("\n"))) + FString::Join(Bodies, TEXT("\n"));
	const int32 At = bOwnLine ? LineStart : Gaps.InsertAt;
	return Logic.Left(At) + (bOwnLine ? Stubs : TEXT("\n") + Stubs) + Logic.Mid(At);
}

TArray<FString> CrowdyExecCodegen::FindRemovals(const FString& OldTypes, const FString& NewTypes)
{
	using namespace CrowdyExecCodegenDetail;
	TArray<FString> Removals;
	const TArray<FTypesItem> NewItems = ParseTypes(NewTypes);
	for (const FTypesItem& Old : ParseTypes(OldTypes))
	{
		const FTypesItem* New = FindTypesItem(NewItems, Old);
		if (New)
		{
			ReportMembers(Old, *New, Removals);
			continue;
		}
		// An engine struct only disappears when the last field using it does, and that field is reported.
		if (Old.bEnum || !IsEngineStructName(Old.Name))
		{
			Removals.Add(FString::Printf(TEXT("%s %s is gone"), Old.bEnum ? TEXT("enum") : TEXT("struct"), *Old.Name));
		}
	}
	return Removals;
}

TArray<CrowdyExecCodegen::FListValueRename> CrowdyExecCodegen::FindRenames(const FString& OldTypes, const FString& NewTypes)
{
	using namespace CrowdyExecCodegenDetail;
	TArray<FListValueRename> Renames;
	const TArray<FTypesItem> NewItems = ParseTypes(NewTypes);
	for (const FTypesItem& Old : ParseTypes(OldTypes))
	{
		if (const FTypesItem* New = Old.bEnum ? nullptr : FindTypesItem(NewItems, Old))
		{
			AddRenames(Old, *New, Renames);
		}
	}
	return Renames;
}

TArray<FString> CrowdyExecCodegen::FindTypeIdentityChanges(const FString& OldTypes, const FString& NewTypes)
{
	using namespace CrowdyExecCodegenDetail;
	TArray<FString> Changes;
	const TArray<FTypesItem> NewItems = ParseTypes(NewTypes);
	for (const FTypesItem& Old : ParseTypes(OldTypes))
	{
		const FTypesItem* New = Old.bEnum ? nullptr : FindTypesItem(NewItems, Old);
		if (New && !New->Target.Equals(Old.Target, ESearchCase::CaseSensitive))
		{
			Changes.Add(DescribeIdentityChange(Old, *New, NewItems));
		}
	}
	return Changes;
}

bool CrowdyExecCodegen::WriteCrate(const FGeneratedCrate& Crate, const FString& Directory, TArray<FString>& OutWritten, FString& OutError)
{
	using namespace CrowdyExecCodegenDetail;
	OutWritten.Reset();
	if (Crate.Files.IsEmpty())
	{
		OutError = FString::Printf(TEXT("The server code for %s has no files to write"), *Crate.Name);
		return false;
	}
	// Every file is written before any is replaced, so a failed write leaves the old crate whole.
	TArray<FPendingFile> Pending;
	if (!WriteTemps(Crate, Directory, Pending, OutError))
	{
		DeleteTemps(Pending);
		return false;
	}
	IFileManager& FileManager = IFileManager::Get();
	for (const FPendingFile& File : Pending)
	{
		if (!FileManager.Move(*File.Path, *File.Temp, true))
		{
			OutError = FString::Printf(TEXT("Could not replace %s"), *File.Path);
			DeleteTemps(Pending);
			return false;
		}
		OutWritten.Add(File.Path);
	}
	return true;
}

FString CrowdyExecCodegen::GetServerDirectory()
{
	FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Server")));
	FPaths::NormalizeDirectoryName(Directory);
	return Directory;
}

FString CrowdyExecCodegen::GetCrateDirectory(const UCrowdyServerObjectDefinition& Definition)
{
	FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(GetServerDirectory(), Definition.TypeName));
	FPaths::NormalizeDirectoryName(Directory);
	return Directory;
}

FString CrowdyExecCodegen::ReadCrateDefinition(const FString& Directory)
{
	using namespace CrowdyExecCodegenDetail;
	FString Text;
	int32 Start = 0;
	int32 Length = 0;
	return FindCrateDefinition(Directory, Text, Start, Length) ? TomlUnescape(Text.Mid(Start, Length)) : FString();
}

FString CrowdyExecCodegen::WithoutCrateDefinition(const FString& CargoText)
{
	FString Out;
	Out.Reserve(CargoText.Len());
	bool bInSection = false;
	for (int32 LineStart = 0; LineStart < CargoText.Len();)
	{
		const int32 Break = CargoText.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, LineStart);
		const int32 LineEnd = Break == INDEX_NONE ? CargoText.Len() : Break + 1;
		const FString Line = CargoText.Mid(LineStart, LineEnd - LineStart);
		LineStart = LineEnd;
		const FString Trimmed = Line.TrimStartAndEnd();
		if (Trimmed.StartsWith(TEXT("["), ESearchCase::CaseSensitive))
		{
			bInSection = Trimmed.Equals(TEXT("[package.metadata.crowdy]"), ESearchCase::CaseSensitive);
		}
		if (!bInSection)
		{
			Out += Line;
		}
	}
	return Out;
}

int32 CrowdyExecCodegen::RetagCrates(const FString& ServerDirectory, const FString& OldPath, const FString& NewPath)
{
	using namespace CrowdyExecCodegenDetail;
	if (OldPath.IsEmpty())
	{
		return 0;
	}
	TArray<FString> Folders;
	IFileManager::Get().FindFiles(Folders, *FPaths::Combine(ServerDirectory, TEXT("*")), false, true);
	int32 Retagged = 0;
	for (const FString& Folder : Folders)
	{
		const FString Crate = FPaths::Combine(ServerDirectory, Folder);
		FString Text;
		int32 Start = 0;
		int32 Length = 0;
		if (!FindCrateDefinition(Crate, Text, Start, Length) || !TomlUnescape(Text.Mid(Start, Length)).Equals(OldPath, ESearchCase::IgnoreCase))
		{
			continue;
		}
		// Only the path changes; the rest of the file, its line breaks included, is written back as read.
		const FString Retargeted = Text.Left(Start) + TomlEscape(NewPath) + Text.Mid(Start + Length);
		const FString Path = FPaths::Combine(Crate, TEXT("Cargo.toml"));
		const FString Temp = Path + TEXT(".tmp");
		// A failed write leaves the old file whole rather than half written.
		if (!FFileHelper::SaveStringToFile(Retargeted, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) || !IFileManager::Get().Move(*Path, *Temp, true, false, false, true))
		{
			IFileManager::Get().Delete(*Temp, false, false, true);
			UE_LOG(LogCrowdyExec, Warning, TEXT("Could not update %s to name the moved definition %s; it still names %s."), *Path, *NewPath, *OldPath);
			continue;
		}
		++Retagged;
	}
	return Retagged;
}

bool CrowdyExecCodegen::HasOwnLogicFile(const UCrowdyServerObjectDefinition& Definition)
{
	return Definition.CodeSource == ECrowdyServerCodeSource::OwnFile;
}

FString CrowdyExecCodegen::GetLogicFile(const UCrowdyServerObjectDefinition& Definition)
{
	return GetLogicFile(Definition, GetCrateDirectory(Definition));
}

FString CrowdyExecCodegen::GetLogicFile(const UCrowdyServerObjectDefinition& Definition, const FString& CrateDirectory)
{
	if (!HasOwnLogicFile(Definition))
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(CrateDirectory, TEXT("src/logic.rs")));
	}
	const FString& Own = Definition.LogicFile.FilePath;
	if (Own.IsEmpty())
	{
		return FString();
	}
	return FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Own) ? FPaths::Combine(FPaths::ProjectDir(), Own) : Own);
}
