#include "Subsystem/CrowdyModScripts.h"

#include "CrowdyCppClient.h"
#include "CrowdyCppReplication.h"
#include "CrowdyModScriptsPlatform.h"
#include "CrowdyNativeExec.h"
#include "CrowdyServiceApiSupport.h"
#include "CrowdyServicesLog.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Algo/AllOf.h"
#include "Algo/AnyOf.h"
#include "Network/CrowdyCpp/CrowdyCppReplicationSubsystem.h"
#include "Misc/Base64.h"
#include "Misc/DateTime.h"
#include "Misc/Parse.h"
#include "Misc/ScopeLock.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/CrowdyJsonSafety.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystem/CrowdyClaims.h"
#include "Subsystem/CrowdyGameSession.h"
#if WITH_CROWDY_MOD_SCRIPT_RUNTIME
#include "CrowdyModScriptRuntime.h"
#endif

using namespace CrowdyServiceApi;

namespace
{
	const TCHAR* const ScriptsLogName = TEXT("CrowdyModScripts");
	const TCHAR* const ScriptsNoClient = TEXT("The player has no app token yet, or the Crowdy API client is unavailable.");
	const TCHAR* const ScriptsNoAnswer = TEXT("The answer holds nothing.");

	constexpr double NotFoundHoldSeconds = 15.0;
	constexpr double LongHoldSeconds = 60.0;
	constexpr double LookupRetrySeconds = 2.0;
	constexpr int32 MaxCallNameLength = 64;
	constexpr int32 MaxFailedListings = 3;
#if WITH_CROWDY_MOD_SCRIPT_RUNTIME
	constexpr int32 MaxPlatformErrorChars = 500;
	constexpr double HostBudgetMs = 2.0;

	// Enough for one answer, so a run late in the frame's order is never starved.
	constexpr double MinDrainMs = 0.05;
#endif

	// The calls this SDK can offer, by group. bHostAnswered calls need a host set.
	struct FServedCall
	{
		const TCHAR* Name;
		ECrowdyModScriptGroup Group;
		bool bHostAnswered;
		bool bServed;
	};

	const FServedCall ServedCalls[] = {
		{TEXT("user_state_get"), ECrowdyModScriptGroup::State, false, true},
		{TEXT("user_state_set"), ECrowdyModScriptGroup::State, false, true},
		{TEXT("chunk_get"), ECrowdyModScriptGroup::WorldRead, true, true},
		{TEXT("voxels_list"), ECrowdyModScriptGroup::WorldRead, true, true},
		{TEXT("actors_list"), ECrowdyModScriptGroup::WorldRead, true, true},
		{TEXT("actors_list_radius"), ECrowdyModScriptGroup::WorldRead, true, true},
		{TEXT("voxel_set"), ECrowdyModScriptGroup::WorldWrite, true, true},
		{TEXT("grid_info"), ECrowdyModScriptGroup::Meta, false, true},
		{TEXT("grid_permission_check"), ECrowdyModScriptGroup::Meta, false, true},
		{TEXT("clock"), ECrowdyModScriptGroup::Meta, false, true},
		{TEXT("emit_spatial"), ECrowdyModScriptGroup::Egress, false, true},
		{TEXT("emit_channel"), ECrowdyModScriptGroup::Egress, false, true},
		{TEXT("emit_event"), ECrowdyModScriptGroup::Egress, false, true},
		{TEXT("hud_set"), ECrowdyModScriptGroup::Present, false, true},
		{TEXT("overlay_draw"), ECrowdyModScriptGroup::Present, false, true},
		{TEXT("pointer_clicks"), ECrowdyModScriptGroup::Input, true, true}};

	const FServedCall* ScriptsFindCall(const FString& Name)
	{
		for (const FServedCall& Call : ServedCalls)
		{
			if (Name.Equals(Call.Name, ESearchCase::CaseSensitive))
			{
				return &Call;
			}
		}
		return nullptr;
	}

	// One execGridClientMods row.
	struct FModScriptRow
	{
		FString ModId;
		FString Name;
		FString Digest;
		FString Hash;
		FString AuthorHash;
		TArray<FString> Calls;
		TArray<FString> AuthorCalls;
		int64 AuthorId = 0;
		int32 Version = 0;
		int32 TickIntervalMs = 0;
		bool bConsented = false;
		bool bTrustsAuthor = false;
	};

	bool ScriptsText(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FString& Out)
	{
		const TSharedPtr<FJsonValue> Value = Object.IsValid() ? Object->TryGetField(Field) : nullptr;
		if (!Value.IsValid() || Value->Type != EJson::String)
		{
			return false;
		}
		Out = Value->AsString();
		return true;
	}

	// TryGetBoolField reads "yes" or "1" as true; only a JSON boolean is a boolean here.
	bool ScriptsBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, bool& Out)
	{
		const TSharedPtr<FJsonValue> Value = Object.IsValid() ? Object->TryGetField(Field) : nullptr;
		if (!Value.IsValid() || Value->Type != EJson::Boolean)
		{
			return false;
		}
		Out = Value->AsBool();
		return true;
	}

	bool ScriptsNames(const TArray<TSharedPtr<FJsonValue>>& Values, TArray<FString>& Out)
	{
		for (const TSharedPtr<FJsonValue>& Value : Values)
		{
			if (!Value.IsValid() || Value->Type != EJson::String)
			{
				return false;
			}
			Out.Add(Value->AsString());
		}
		return true;
	}

	// A capability summary's hostFunctions.
	bool ScriptsSummary(const FString& Json, TArray<FString>& OutCalls)
	{
		TSharedPtr<FJsonObject> Summary;
		const TArray<TSharedPtr<FJsonValue>>* Calls = nullptr;
		return CrowdyJsonSafety::IsNestingWithinLimit(Json) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Summary)
			&& Summary.IsValid() && Summary->TryGetArrayField(TEXT("hostFunctions"), Calls) && ScriptsNames(*Calls, OutCalls);
	}

	bool ScriptsReadRow(const TSharedPtr<FJsonValue>& Value, int64 GridId, FModScriptRow& Out)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Object))
		{
			return false;
		}
		const TSharedPtr<FJsonObject>& Row = *Object;
		int64 RowGrid = 0;
		int64 Version = 0;
		int64 Tick = 0;
		FString Summary;
		FString AuthorSummary;
		const bool bRead = ScriptsText(Row, TEXT("modId"), Out.ModId) && !Out.ModId.IsEmpty() && ScriptsText(Row, TEXT("name"), Out.Name)
			&& ReadWholeInt64(Row, TEXT("gridId"), RowGrid) && RowGrid == GridId && ReadWholeInt64(Row, TEXT("authorId"), Out.AuthorId)
			&& Out.AuthorId > 0 && ReadWholeInt64(Row, TEXT("clientVersion"), Version) && Version >= 0 && Version <= MAX_int32
			&& ReadWholeInt64(Row, TEXT("tickIntervalMs"), Tick) && Tick >= 16 && Tick <= 1000
			&& ScriptsText(Row, TEXT("digest"), Out.Digest) && ScriptsText(Row, TEXT("capabilityHash"), Out.Hash)
			&& ScriptsText(Row, TEXT("authorCapabilityHash"), Out.AuthorHash) && ScriptsBool(Row, TEXT("callerConsented"), Out.bConsented)
			&& ScriptsBool(Row, TEXT("callerTrustsAuthor"), Out.bTrustsAuthor) && ScriptsText(Row, TEXT("capabilitySummaryJson"), Summary)
			&& ScriptsSummary(Summary, Out.Calls) && ScriptsText(Row, TEXT("authorCapabilitySummaryJson"), AuthorSummary)
			&& ScriptsSummary(AuthorSummary, Out.AuthorCalls);
		Out.Version = static_cast<int32>(Version);
		Out.TickIntervalMs = static_cast<int32>(Tick);
		return bRead;
	}

	bool ScriptsIsCallName(const FString& Name)
	{
		if (Name.IsEmpty() || Name.Len() > MaxCallNameLength)
		{
			return false;
		}
		for (const TCHAR Char : Name)
		{
			if (!(Char >= TEXT('a') && Char <= TEXT('z')) && !(Char >= TEXT('0') && Char <= TEXT('9')) && Char != TEXT('_'))
			{
				return false;
			}
		}
		return true;
	}

	bool ScriptsParseObject(TConstArrayView<uint8> Utf8, TSharedPtr<FJsonObject>& Out)
	{
		if (!CrowdyJsonSafety::IsNestingWithinLimit(Utf8))
		{
			return false;
		}
		const FString Json = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Utf8.GetData()), Utf8.Num());
		return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Out) && Out.IsValid();
	}

	bool ScriptsInBox(const FCrowdyModScriptGrid& Box, const FInt64Vector& Chunk)
	{
		return Chunk.X >= Box.LowChunkX && Chunk.X <= Box.HighChunkX && Chunk.Y >= Box.LowChunkY && Chunk.Y <= Box.HighChunkY
			&& Chunk.Z >= Box.LowChunkZ && Chunk.Z <= Box.HighChunkZ;
	}

	const TCHAR* ScriptsOperation(ECrowdyModScriptsCall Call)
	{
		switch (Call)
		{
		case ECrowdyModScriptsCall::ListMods:      return TEXT("ExecGridClientMods");
		case ECrowdyModScriptsCall::TrustAuthor:   return TEXT("ExecTrustAuthor");
		case ECrowdyModScriptsCall::ConsentMod:    return TEXT("ExecConsentClientMod");
		case ECrowdyModScriptsCall::RevokeConsent: return TEXT("ExecRevokeClientModConsent");
		case ECrowdyModScriptsCall::RevokeTrust:   return TEXT("ExecRevokeAuthorTrust");
		default:                                   return TEXT("");
		}
	}

	const TCHAR* ScriptsAnswerField(ECrowdyModScriptsCall Call)
	{
		switch (Call)
		{
		case ECrowdyModScriptsCall::ListMods:      return TEXT("execGridClientMods");
		case ECrowdyModScriptsCall::TrustAuthor:   return TEXT("execTrustAuthor");
		case ECrowdyModScriptsCall::ConsentMod:    return TEXT("execConsentClientMod");
		case ECrowdyModScriptsCall::RevokeConsent: return TEXT("execRevokeClientModConsent");
		case ECrowdyModScriptsCall::RevokeTrust:   return TEXT("execRevokeAuthorTrust");
		default:                                   return TEXT("");
		}
	}

	TSharedPtr<FJsonObject> ScriptsVariables(const UCrowdyModScripts::FPlatformCall& Call, int64 AppId)
	{
		TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
		Variables->SetStringField(TEXT("appId"), BigInt(AppId));
		if (Call.Call == ECrowdyModScriptsCall::ListMods || Call.Call == ECrowdyModScriptsCall::TrustAuthor || Call.Call == ECrowdyModScriptsCall::RevokeTrust)
		{
			Variables->SetStringField(TEXT("gridId"), BigInt(Call.GridId));
		}
		if (Call.Call == ECrowdyModScriptsCall::TrustAuthor || Call.Call == ECrowdyModScriptsCall::RevokeTrust)
		{
			Variables->SetStringField(TEXT("authorId"), BigInt(Call.AuthorId));
		}
		if (Call.Call == ECrowdyModScriptsCall::ConsentMod || Call.Call == ECrowdyModScriptsCall::RevokeConsent)
		{
			Variables->SetStringField(TEXT("modId"), Call.ModId);
		}
		if (Call.Call == ECrowdyModScriptsCall::TrustAuthor || Call.Call == ECrowdyModScriptsCall::ConsentMod)
		{
			Variables->SetStringField(TEXT("capabilityHash"), Call.Hash);
		}
		return Variables;
	}

	UCrowdyModScripts::FPlatformReply ScriptsFailure(const FString& Message)
	{
		UCrowdyModScripts::FPlatformReply Reply;
		Reply.Message = Message;
		return Reply;
	}

	// As CrowdyServiceApi::NamesCode: the code, its reason, or a message that leads with it.
	bool ScriptsNamesCode(const UCrowdyModScripts::FPlatformReply& Reply, const TCHAR* Code)
	{
		return Reply.Code.Equals(Code, ESearchCase::CaseSensitive) || Reply.Reason.Equals(Code, ESearchCase::CaseSensitive)
			|| Reply.Message.StartsWith(FString::Printf(TEXT("%s:"), Code), ESearchCase::CaseSensitive);
	}

	TArray<uint8> ScriptsUtf8(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text, Text.Len());
		return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}

	FString ScriptsCondensed(const TSharedRef<FJsonObject>& Object)
	{
		FString Json;
		FJsonSerializer::Serialize(Object, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
		return Json;
	}

	// Each group's calls a second, in ECrowdyModScriptGroup order, as the browser's broker.
	constexpr int32 GroupCaps[] = {100, 400, 200, 100, 60, 120, 400};
	static_assert(UE_ARRAY_COUNT(GroupCaps) == CrowdyModScriptsHostCalls::NumGroups, "one cap per ECrowdyModScriptGroup");

	// Compared without case, as FJsonObject stores keys, so no spelling of them slips through.
	const TCHAR* const ForbiddenArgKeys[] = {TEXT("authority"), TEXT("bindingKind"), TEXT("binding_kind")};

	constexpr double VoxelsPerChunk = 16.0;
	constexpr double DefaultOverlaySize = 0.2;
	constexpr double MinOverlaySize = 0.04;
	constexpr double MaxOverlaySize = 8.0;
	constexpr int32 MaxOverlayIdChars = 32;

	// Stricter than the web, which reads every row: a payload of rows that are all dropped costs no more than this.
	constexpr int32 MaxOverlayRowsRead = 4 * CrowdyModScriptsHostCalls::MaxOverlayShapes;
	constexpr uint32 DefaultOverlayColor = 0xf8fafc;

	TSharedPtr<FJsonValue> OverlayPresent(const FJsonObject& Object, std::initializer_list<const TCHAR*> Keys)
	{
		for (const TCHAR* Key : Keys)
		{
			const TSharedPtr<FJsonValue> Value = Object.TryGetField(Key);
			if (Value.IsValid() && Value->Type != EJson::Null)
			{
				return Value;
			}
		}
		return nullptr;
	}

	// The first of Keys present and not null, then of NestedKeys on Nested, as the web's `a ?? b ?? c`.
	TSharedPtr<FJsonValue> OverlayField(const FJsonObject& Row, std::initializer_list<const TCHAR*> Keys, const FJsonObject* Nested = nullptr,
		std::initializer_list<const TCHAR*> NestedKeys = {})
	{
		const TSharedPtr<FJsonValue> Value = OverlayPresent(Row, Keys);
		return Value.IsValid() || !Nested ? Value : OverlayPresent(*Nested, NestedKeys);
	}

	const FJsonObject* OverlayRecord(const FJsonObject& Row, std::initializer_list<const TCHAR*> Keys)
	{
		for (const TCHAR* Key : Keys)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (Row.TryGetObjectField(Key, Object))
			{
				return Object->Get();
			}
		}
		return nullptr;
	}

	// FChar::IsDigit also takes other scripts' digits, which JavaScript's numbers do not.
	bool ScriptsIsAsciiDigit(TCHAR Char)
	{
		return Char >= TEXT('0') && Char <= TEXT('9');
	}

	int32 OverlayDigits(const FString& Text, int32 Index)
	{
		int32 End = Index;
		while (End < Text.Len() && ScriptsIsAsciiDigit(Text[End]))
		{
			++End;
		}
		return End - Index;
	}

	// Whether Text is wholly a decimal number: sign, digits with an optional fraction, then an optional exponent.
	bool OverlayIsDecimal(const FString& Text)
	{
		int32 Index = Text.Len() > 0 && (Text[0] == TEXT('+') || Text[0] == TEXT('-')) ? 1 : 0;
		const int32 Whole = OverlayDigits(Text, Index);
		Index += Whole;
		const int32 Fraction = Index < Text.Len() && Text[Index] == TEXT('.') ? OverlayDigits(Text, Index + 1) : 0;
		Index += Index < Text.Len() && Text[Index] == TEXT('.') ? 1 + Fraction : 0;
		if (Whole + Fraction == 0)
		{
			return false;
		}
		if (Index < Text.Len() && (Text[Index] == TEXT('e') || Text[Index] == TEXT('E')))
		{
			Index += Index + 1 < Text.Len() && (Text[Index + 1] == TEXT('+') || Text[Index + 1] == TEXT('-')) ? 2 : 1;
			const int32 Exponent = OverlayDigits(Text, Index);
			Index += Exponent;
			return Exponent > 0 && Index == Text.Len();
		}
		return Index == Text.Len();
	}

	// A finite number, or a string that is wholly a decimal one, as JavaScript's Number() reads it.
	bool OverlayNumber(const TSharedPtr<FJsonValue>& Value, double& Out)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::Number)
		{
			Out = Value->AsNumber();
			return FMath::IsFinite(Out);
		}
		const FString Text = Value->Type == EJson::String ? Value->AsString().TrimStartAndEnd() : FString();
		Out = OverlayIsDecimal(Text) ? FCString::Atod(*Text) : 0.0;
		return OverlayIsDecimal(Text) && FMath::IsFinite(Out);
	}

	double OverlayNumberOr(const TSharedPtr<FJsonValue>& Value, double Fallback)
	{
		double Number = 0.0;
		return OverlayNumber(Value, Number) ? Number : Fallback;
	}

	double OverlayAxis(const FJsonObject& Row, std::initializer_list<const TCHAR*> Keys, const FJsonObject* Scale, const TCHAR* ScaleKey, double Uniform)
	{
		return FMath::Clamp(OverlayNumberOr(OverlayField(Row, Keys, Scale, {ScaleKey}), Uniform), MinOverlaySize, MaxOverlaySize);
	}

	bool OverlayIs(const FJsonObject& Row, const TCHAR* Key, bool bValue)
	{
		const TSharedPtr<FJsonValue> Value = Row.TryGetField(Key);
		return Value.IsValid() && Value->Type == EJson::Boolean && Value->AsBool() == bValue;
	}

	bool OverlayIsText(const TSharedPtr<FJsonValue>& Value, const TCHAR* Text)
	{
		return Value.IsValid() && Value->Type == EJson::String && Value->AsString().Equals(Text, ESearchCase::CaseSensitive);
	}

	FLinearColor OverlayColor(const TSharedPtr<FJsonValue>& Value)
	{
		uint32 Rgb = DefaultOverlayColor;
		if (Value.IsValid() && Value->Type == EJson::Number && FMath::IsFinite(Value->AsNumber()))
		{
			Rgb = static_cast<uint32>(FMath::Clamp(FMath::FloorToDouble(Value->AsNumber()), 0.0, 16777215.0));
		}
		else if (Value.IsValid() && Value->Type == EJson::String)
		{
			FString Hex = Value->AsString().TrimStartAndEnd();
			Hex.RemoveFromStart(TEXT("#"));
			bool bHex = Hex.Len() == 6;
			for (const TCHAR Char : Hex)
			{
				bHex &= FChar::IsHexDigit(Char);
			}
			Rgb = bHex ? FParse::HexNumber(*Hex) : Rgb;
		}
		return FLinearColor(FColor(static_cast<uint8>(Rgb >> 16), static_cast<uint8>(Rgb >> 8), static_cast<uint8>(Rgb)));
	}

	// False for a mesh row, which takes its place and id but is not drawn.
	bool OverlayKind(const TSharedPtr<FJsonValue>& Value, ECrowdyModScriptShapeKind& Out)
	{
		struct FKindName
		{
			const TCHAR* Name;
			ECrowdyModScriptShapeKind Kind;
		};
		const FKindName Kinds[] = {
			{TEXT("sphere"), ECrowdyModScriptShapeKind::Sphere}, {TEXT("cylinder"), ECrowdyModScriptShapeKind::Cylinder},
			{TEXT("capsule"), ECrowdyModScriptShapeKind::Capsule}, {TEXT("plane"), ECrowdyModScriptShapeKind::Plane}};
		Out = ECrowdyModScriptShapeKind::Box;
		for (const FKindName& Kind : Kinds)
		{
			Out = OverlayIsText(Value, Kind.Name) ? Kind.Kind : Out;
		}
		return !OverlayIsText(Value, TEXT("mesh"));
	}

	// One construct.scene.v1 row, read as the web Construct's parseNode; false drops it.
	bool OverlayShape(const TSharedPtr<FJsonValue>& Value, int32 Index, const FVector& Low, const FVector& High, FCrowdyModScriptShape& Out, bool& bOutMesh)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Object))
		{
			return false;
		}
		const FJsonObject& Row = **Object;
		const FJsonObject* Position = OverlayRecord(Row, {TEXT("p"), TEXT("pos"), TEXT("position")});
		double X = 0.0;
		double Z = 0.0;
		if (!OverlayNumber(OverlayField(Row, {TEXT("x"), TEXT("0")}, Position, {TEXT("x")}), X)
			|| !OverlayNumber(OverlayField(Row, {TEXT("z"), TEXT("2")}, Position, {TEXT("z")}), Z))
		{
			return false;
		}
		bOutMesh = !OverlayKind(OverlayField(Row, {TEXT("kind"), TEXT("shape")}), Out.Kind);
		const double Y = OverlayNumberOr(OverlayField(Row, {TEXT("y"), TEXT("1")}, Position, {TEXT("y")}), 0.5);
		Out.Location = FVector(FMath::Clamp(X, Low.X, High.X), FMath::Clamp(Y, Low.Y, High.Y), FMath::Clamp(Z, Low.Z, High.Z));

		const FJsonObject* Scale = OverlayRecord(Row, {TEXT("s"), TEXT("scale")});
		const double Uniform = OverlayNumberOr(OverlayField(Row, {TEXT("r"), TEXT("radius"), TEXT("size")}), DefaultOverlaySize);
		Out.Size = FVector(OverlayAxis(Row, {TEXT("sx"), TEXT("w")}, Scale, TEXT("x"), Uniform), OverlayAxis(Row, {TEXT("sy"), TEXT("h")}, Scale, TEXT("y"), Uniform),
			OverlayAxis(Row, {TEXT("sz"), TEXT("d")}, Scale, TEXT("z"), Uniform));

		const FJsonObject* Quat = OverlayRecord(Row, {TEXT("q"), TEXT("quat"), TEXT("quaternion")});
		const FQuat Rotation(OverlayNumberOr(OverlayField(Row, {TEXT("qx"), TEXT("qi")}, Quat, {TEXT("x"), TEXT("qx")}), 0.0),
			OverlayNumberOr(OverlayField(Row, {TEXT("qy"), TEXT("qj")}, Quat, {TEXT("y"), TEXT("qy")}), 0.0),
			OverlayNumberOr(OverlayField(Row, {TEXT("qz"), TEXT("qk")}, Quat, {TEXT("z"), TEXT("qz")}), 0.0),
			OverlayNumberOr(OverlayField(Row, {TEXT("qw"), TEXT("qr")}, Quat, {TEXT("w"), TEXT("qw")}), 1.0));
		// Scaled by the largest part first, so a huge but finite quaternion still normalizes, as Math.hypot does.
		const double Largest = FMath::Max(FMath::Max(FMath::Abs(Rotation.X), FMath::Abs(Rotation.Y)), FMath::Max(FMath::Abs(Rotation.Z), FMath::Abs(Rotation.W)));
		const FQuat Scaled = Largest > 0.0 ? Rotation / Largest : Rotation;
		const double Length = Largest * Scaled.Size();
		Out.Rotation = FMath::IsFinite(Length) && Length >= 1e-8 ? Scaled / Scaled.Size() : FQuat::Identity;

		Out.Color = OverlayColor(OverlayField(Row, {TEXT("color"), TEXT("colour")}));
		Out.Opacity = static_cast<float>(FMath::Clamp(OverlayNumberOr(Row.TryGetField(TEXT("opacity")), 1.0), 0.0, 1.0));
		Out.bVisible = !OverlayIs(Row, TEXT("visible"), false) && !OverlayIs(Row, TEXT("hidden"), true);
		Out.bUnlit = OverlayIs(Row, TEXT("unlit"), true) || OverlayIs(Row, TEXT("basic"), true) || OverlayIsText(Row.TryGetField(TEXT("shading")), TEXT("basic"));

		const TSharedPtr<FJsonValue> IdValue = OverlayField(Row, {TEXT("id"), TEXT("name")});
		const FString Id = IdValue.IsValid() && IdValue->Type == EJson::String ? IdValue->AsString().TrimStartAndEnd() : FString();
		Out.Id = Id.IsEmpty() ? FString::Printf(TEXT("obj-%d"), Index) : Id.Left(MaxOverlayIdChars);
		return true;
	}

	int32 ScriptsSerializedBytes(const TSharedRef<FJsonObject>& Object)
	{
		const FString Json = ScriptsCondensed(Object);
		return FTCHARToUTF8(*Json, Json.Len()).Length();
	}

	const TCHAR* ScriptsForbiddenKey(const FJsonObject& Args)
	{
		for (const TCHAR* Key : ForbiddenArgKeys)
		{
			if (Args.Values.Contains(Key))
			{
				return Key;
			}
		}
		return nullptr;
	}

	bool ScriptsIsEnvelopeKey(const FString& Key)
	{
		return Key.Equals(TEXT("fn"), ESearchCase::CaseSensitive) || Key.Equals(TEXT("args"), ESearchCase::CaseSensitive);
	}

	using CrowdyModScriptsHostCalls::ECall;

	constexpr int32 ChunkCells = 4096;
	constexpr int32 MaxQueuedClicks = 64;
	constexpr int32 MaxUnsentEdits = 64;
	constexpr int32 VoxelSetsPerSecond = 100;
	constexpr int32 MaxPlayers = 64;
	constexpr int32 MaxPlayerNameChars = 64;
	constexpr int32 MaxVoxelStateBytes = 1024;
	constexpr int32 MaxActorsRadiusXz = 3;
	constexpr int32 MaxActorsRadiusY = 1;
	constexpr int32 MaxCoordinateDigits = 18;

	// Other spellings of a call's chunk, refused so nothing can be pointed outside the grid through them, as the browser's broker.
	const TCHAR* const ReadChunkAliases[] = {TEXT("chunk"), TEXT("chunkX"), TEXT("chunkY"), TEXT("chunkZ"), TEXT("chunk_x"), TEXT("chunk_y"), TEXT("chunk_z")};
	const TCHAR* const WriteChunkAliases[] = {TEXT("chunk"), TEXT("chunk_x"), TEXT("chunk_y"), TEXT("chunk_z")};

	struct FCallName
	{
		const TCHAR* Name;
		ECall Call;
	};

	const FCallName CallNames[] = {
		{TEXT("hud_set"), ECall::Hud}, {TEXT("overlay_draw"), ECall::Overlay}, {TEXT("pointer_clicks"), ECall::Clicks},
		{TEXT("chunk_get"), ECall::ChunkGet}, {TEXT("voxels_list"), ECall::VoxelsList}, {TEXT("actors_list"), ECall::Actors},
		{TEXT("actors_list_radius"), ECall::Actors}, {TEXT("voxel_set"), ECall::VoxelSet}, {TEXT("emit_event"), ECall::Event},
		{TEXT("grid_info"), ECall::GridInfo}, {TEXT("clock"), ECall::Clock}, {TEXT("grid_permission_check"), ECall::Permission},
		{TEXT("user_state_get"), ECall::UserStateGet}, {TEXT("user_state_set"), ECall::UserStateSet}, {TEXT("emit_spatial"), ECall::Spatial},
		{TEXT("emit_channel"), ECall::Channel}};

	ECall WorldCallOf(const FString& Name)
	{
		for (const FCallName& Entry : CallNames)
		{
			if (Name.Equals(Entry.Name, ESearchCase::CaseSensitive))
			{
				return Entry.Call;
			}
		}
		return ECall::None;
	}

	TArray<uint8> WorldRefusal(TConstArrayView<const TCHAR* const> Aliases, const FJsonObject& Args)
	{
		for (const TCHAR* Alias : Aliases)
		{
			if (Args.Values.Contains(Alias))
			{
				return CrowdyModScriptsHostCalls::Denied(
					FString::Printf(TEXT("host call cannot carry '%s': the grid is checked on the call's own chunk fields"), Alias));
			}
		}
		return TArray<uint8>();
	}

	bool WorldSignedDigits(const FString& Text, bool bNoLeadingZero)
	{
		const int32 First = Text.StartsWith(TEXT("-")) ? 1 : 0;
		const int32 Digits = Text.Len() - First;
		if (Digits <= 0 || (bNoLeadingZero && Digits > 1 && Text[First] == TEXT('0')))
		{
			return false;
		}
		for (int32 Index = First; Index < Text.Len(); ++Index)
		{
			if (!ScriptsIsAsciiDigit(Text[Index]))
			{
				return false;
			}
		}
		return true;
	}

	// A chunk coordinate as the browser's broker reads it: a safe integer, or a decimal integer string.
	bool WorldCoordinate(const TSharedPtr<FJsonValue>& Value, int64& Out, FString& OutError)
	{
		if (Value.IsValid() && Value->Type == EJson::String)
		{
			const FString& Text = Value->AsString();
			if (!WorldSignedDigits(Text, true))
			{
				OutError = TEXT("chunk coordinates must be decimal integer strings");
				return false;
			}
			// Longer ones could overflow int64; they lie outside any grid, so their sign's extreme stands in.
			const bool bNegative = Text.StartsWith(TEXT("-"));
			const bool bLong = Text.Len() - (bNegative ? 1 : 0) > MaxCoordinateDigits;
			Out = !bLong ? FCString::Atoi64(*Text) : bNegative ? MIN_int64 : MAX_int64;
			return true;
		}
		const double Number = Value.IsValid() && Value->Type == EJson::Number ? Value->AsNumber() : 0.0;
		if (!Value.IsValid() || Value->Type != EJson::Number)
		{
			OutError = TEXT("chunk coordinates must be integers");
			return false;
		}
		if (!FMath::IsFinite(Number) || FMath::Abs(Number) > 9007199254740991.0 || Number != FMath::FloorToDouble(Number))
		{
			OutError = TEXT("chunk coordinates must be safe integers or decimal strings");
			return false;
		}
		Out = static_cast<int64>(Number);
		return true;
	}

	bool WorldChunk(const FJsonObject& Args, const TCHAR* X, const TCHAR* Y, const TCHAR* Z, FInt64Vector& Out, FString& OutError)
	{
		return WorldCoordinate(Args.TryGetField(X), Out.X, OutError) && WorldCoordinate(Args.TryGetField(Y), Out.Y, OutError)
			&& WorldCoordinate(Args.TryGetField(Z), Out.Z, OutError);
	}

	// As the web's clampInt: a number, a numeric string or a boolean, truncated into Low to High; anything else is Low.
	int64 WorldClamp(const TSharedPtr<FJsonValue>& Value, int64 Low, int64 High)
	{
		double Number = static_cast<double>(Low);
		if (Value.IsValid() && Value->Type == EJson::Boolean)
		{
			Number = Value->AsBool() ? 1.0 : 0.0;
		}
		else if (Value.IsValid())
		{
			Number = OverlayNumberOr(Value, static_cast<double>(Low));
		}
		if (!FMath::IsFinite(Number))
		{
			return Low;
		}
		return static_cast<int64>(FMath::Clamp(FMath::TruncToDouble(Number), static_cast<double>(Low), static_cast<double>(High)));
	}

	// Chunk moved by up to Reach toward Limit, never past it. Chunk lies inside the box, so the room between fits uint64.
	int64 WorldStep(int64 Chunk, int64 Limit, int64 Reach)
	{
		const bool bUp = Limit >= Chunk;
		const uint64 Room = bUp ? static_cast<uint64>(Limit) - static_cast<uint64>(Chunk) : static_cast<uint64>(Chunk) - static_cast<uint64>(Limit);
		const int64 Step = static_cast<int64>(FMath::Min(Room, static_cast<uint64>(Reach)));
		return bUp ? Chunk + Step : Chunk - Step;
	}

	// An integer in Low to High, as a number or a string of digits; an absent or null one is 0, as the web's `?? 0`.
	bool WorldInt(const TSharedPtr<FJsonValue>& Value, int32 Low, int32 High, int32& Out)
	{
		double Number = 0.0;
		if (Value.IsValid() && Value->Type == EJson::Number)
		{
			Number = Value->AsNumber();
		}
		else if (Value.IsValid() && Value->Type == EJson::String)
		{
			if (!WorldSignedDigits(Value->AsString(), false))
			{
				return false;
			}
			Number = FCString::Atod(*Value->AsString());
		}
		else if (Value.IsValid() && Value->Type != EJson::Null)
		{
			return false;
		}
		if (!FMath::IsFinite(Number) || Number != FMath::FloorToDouble(Number) || Number < Low || Number > High)
		{
			return false;
		}
		Out = static_cast<int32>(Number);
		return true;
	}

	TArray<uint8> WorldOutsideGrid()
	{
		return CrowdyModScriptsHostCalls::Denied(TEXT("target is outside the player grid"));
	}

	bool PrepareRead(const FString& Name, const FJsonObject& Args, const FCrowdyModScriptGrid& Box, CrowdyModScriptsHostCalls::FPrepared& Out,
		TArray<uint8>& OutImmediate)
	{
		OutImmediate = WorldRefusal(ReadChunkAliases, Args);
		FString Error;
		if (!OutImmediate.IsEmpty() || !WorldChunk(Args, TEXT("x"), TEXT("y"), TEXT("z"), Out.Chunk, Error))
		{
			OutImmediate = OutImmediate.IsEmpty() ? CrowdyModScriptsHostCalls::Denied(Error) : OutImmediate;
			return false;
		}
		if (!ScriptsInBox(Box, Out.Chunk))
		{
			OutImmediate = WorldOutsideGrid();
			return false;
		}
		const bool bRadius = Name.Equals(TEXT("actors_list_radius"), ESearchCase::CaseSensitive);
		const int64 Sideways = bRadius ? WorldClamp(OverlayField(Args, {TEXT("radiusXz"), TEXT("radius")}), 0, MaxActorsRadiusXz) : 0;
		const int64 Vertical = bRadius ? WorldClamp(Args.TryGetField(TEXT("radiusY")), 0, MaxActorsRadiusY) : 0;
		Out.Area.GridId = Box.GridId;
		Out.Area.LowChunkX = WorldStep(Out.Chunk.X, Box.LowChunkX, Sideways);
		Out.Area.LowChunkY = WorldStep(Out.Chunk.Y, Box.LowChunkY, Vertical);
		Out.Area.LowChunkZ = WorldStep(Out.Chunk.Z, Box.LowChunkZ, Sideways);
		Out.Area.HighChunkX = WorldStep(Out.Chunk.X, Box.HighChunkX, Sideways);
		Out.Area.HighChunkY = WorldStep(Out.Chunk.Y, Box.HighChunkY, Vertical);
		Out.Area.HighChunkZ = WorldStep(Out.Chunk.Z, Box.HighChunkZ, Sideways);
		return true;
	}

	TArray<uint8> WorldVoxelAnswer(bool bOk, const FString& Error)
	{
		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("ok"), bOk);
		if (!bOk)
		{
			Data->SetStringField(TEXT("error"), Error);
		}
		const TSharedRef<FJsonObject> Answer = MakeShared<FJsonObject>();
		Answer->SetBoolField(TEXT("ok"), true);
		Answer->SetObjectField(TEXT("data"), Data);
		return ScriptsUtf8(ScriptsCondensed(Answer));
	}

	bool PrepareVoxelSet(const FJsonObject& Args, const FCrowdyModScriptGrid& Box, CrowdyModScriptsHostCalls::FPrepared& Out, TArray<uint8>& OutImmediate)
	{
		OutImmediate = WorldRefusal(WriteChunkAliases, Args);
		FString Error;
		FInt64Vector Chunk;
		if (!OutImmediate.IsEmpty() || !WorldChunk(Args, TEXT("chunkX"), TEXT("chunkY"), TEXT("chunkZ"), Chunk, Error))
		{
			OutImmediate = OutImmediate.IsEmpty() ? CrowdyModScriptsHostCalls::Denied(Error) : OutImmediate;
			return false;
		}
		// Answered, not refused: the script is told where it may write, as the web's host does.
		if (!ScriptsInBox(Box, Chunk))
		{
			OutImmediate = WorldVoxelAnswer(false, TEXT("out_of_grid"));
			return false;
		}
		FCrowdyVoxel& Voxel = Out.Write.Voxel;
		if (!WorldInt(Args.TryGetField(TEXT("voxelX")), 0, 15, Voxel.Position.X) || !WorldInt(Args.TryGetField(TEXT("voxelY")), 0, 15, Voxel.Position.Y)
			|| !WorldInt(Args.TryGetField(TEXT("voxelZ")), 0, 15, Voxel.Position.Z) || !WorldInt(Args.TryGetField(TEXT("voxelType")), 0, 255, Voxel.Type))
		{
			OutImmediate = CrowdyModScriptsHostCalls::Denied(TEXT("host call 'voxel_set' needs a voxel inside its chunk (0-15) and a voxel type 0-255"));
			return false;
		}
		const TSharedPtr<FJsonValue> State = Args.TryGetField(TEXT("stateBase64"));
		if (State.IsValid() && State->Type == EJson::String && (!FBase64::Decode(State->AsString(), Voxel.State) || Voxel.State.Num() > MaxVoxelStateBytes))
		{
			OutImmediate = CrowdyModScriptsHostCalls::Denied(TEXT("host call 'voxel_set' needs a base64 state of at most 1024 bytes"));
			return false;
		}
		Out.Write.ChunkX = Chunk.X;
		Out.Write.ChunkY = Chunk.Y;
		Out.Write.ChunkZ = Chunk.Z;
		return true;
	}

	TArray<uint8> SdkAnswer(const TSharedPtr<FJsonValue>& Data)
	{
		const TSharedRef<FJsonObject> Answer = MakeShared<FJsonObject>();
		Answer->SetBoolField(TEXT("ok"), true);
		Answer->SetField(TEXT("data"), Data);
		return ScriptsUtf8(ScriptsCondensed(Answer));
	}

	TSharedRef<FJsonObject> SdkChunk(int64 X, int64 Y, int64 Z)
	{
		const TSharedRef<FJsonObject> Chunk = MakeShared<FJsonObject>();
		Chunk->SetStringField(TEXT("x"), FString::Printf(TEXT("%lld"), X));
		Chunk->SetStringField(TEXT("y"), FString::Printf(TEXT("%lld"), Y));
		Chunk->SetStringField(TEXT("z"), FString::Printf(TEXT("%lld"), Z));
		return Chunk;
	}

	// Chunk coordinates cross as decimal strings, since they may pass 2^53.
	TArray<uint8> SdkGridInfo(const FCrowdyModScriptGrid& Box)
	{
		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("gridId"), FString::Printf(TEXT("%lld"), Box.GridId));
		Data->SetObjectField(TEXT("low"), SdkChunk(Box.LowChunkX, Box.LowChunkY, Box.LowChunkZ));
		Data->SetObjectField(TEXT("high"), SdkChunk(Box.HighChunkX, Box.HighChunkY, Box.HighChunkZ));
		return SdkAnswer(MakeShared<FJsonValueObject>(Data));
	}

	TArray<uint8> SdkClock()
	{
		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("ms"), static_cast<double>((FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTicks() / ETimespan::TicksPerMillisecond));
		return SdkAnswer(MakeShared<FJsonValueObject>(Data));
	}

	// As the browser's String(value ?? ''): a string as it is, a whole number in decimal.
	FString SdkText(const TSharedPtr<FJsonValue>& Value)
	{
		if (Value.IsValid() && Value->Type == EJson::String)
		{
			return Value->AsString();
		}
		const bool bWhole = Value.IsValid() && Value->Type == EJson::Number && FMath::Abs(Value->AsNumber()) <= 9007199254740991.0
			&& Value->AsNumber() == FMath::FloorToDouble(Value->AsNumber());
		return bWhole ? FString::Printf(TEXT("%lld"), static_cast<int64>(Value->AsNumber())) : FString();
	}

	// A positive decimal id, as the web's decimalId; empty when it is not one.
	FString SdkDecimalId(const TSharedPtr<FJsonValue>& Value)
	{
		const FString Text = SdkText(Value);
		const bool bId = !Text.IsEmpty() && Text.Len() <= 20 && Text[0] != TEXT('0') && WorldSignedDigits(Text, true) && !Text.StartsWith(TEXT("-"));
		return bId ? Text : FString();
	}

	bool SdkIsPermissionKey(const FString& Key)
	{
		if (Key.IsEmpty() || Key.Len() > MaxCallNameLength || !(Key[0] >= TEXT('a') && Key[0] <= TEXT('z')))
		{
			return false;
		}
		return ScriptsIsCallName(Key);
	}

	const TCHAR* const CodePermissionKeys[] = {TEXT("write_server_code"), TEXT("run_server_code"), TEXT("write_client_code"), TEXT("run_client_code")};

	TArray<uint8> SdkPermissionCheck(const FJsonObject& Args, CrowdyModScriptsHostCalls::FGate& Gate)
	{
		using CrowdyModScriptsHostCalls::Denied;
		if (!SdkText(Args.TryGetField(TEXT("gridId"))).Equals(FString::Printf(TEXT("%lld"), Gate.Box.GridId), ESearchCase::CaseSensitive))
		{
			return WorldOutsideGrid();
		}
		const FString Player = Gate.PlayerId > 0 ? FString::Printf(TEXT("%lld"), Gate.PlayerId) : FString();
		if (Player.IsEmpty())
		{
			return Denied(TEXT("host call 'grid_permission_check' is not offered by this game"));
		}
		if (!SdkDecimalId(Args.TryGetField(TEXT("userId"))).Equals(Player, ESearchCase::CaseSensitive))
		{
			return Denied(TEXT("host call 'grid_permission_check' asks about another player"));
		}
		const TSharedPtr<FJsonValue> KeyValue = Args.TryGetField(TEXT("permissionKey"));
		const FString Key = KeyValue.IsValid() && KeyValue->Type == EJson::String ? KeyValue->AsString() : FString();
		if (!SdkIsPermissionKey(Key))
		{
			return Denied(TEXT("host call 'grid_permission_check' needs a permission key"));
		}
		const auto IsKey = [&Key](const FString& Held) { return Held.Equals(Key, ESearchCase::CaseSensitive); };
		if (!Algo::AnyOf(CodePermissionKeys, [&Key](const TCHAR* Code) { return Key.Equals(Code, ESearchCase::CaseSensitive); }))
		{
			return Denied(FString::Printf(TEXT("host call 'grid_permission_check' cannot answer for '%s': the page knows only the player's code-permission keys ")
				TEXT("(write_server_code, run_server_code, write_client_code, run_client_code)"), *Key));
		}
		bool bHeld = false;
		{
			FScopeLock Guard(&Gate.Lock);
			bHeld = Gate.Keys.ContainsByPredicate(IsKey);
		}
		return SdkAnswer(MakeShared<FJsonValueBoolean>(bHeld));
	}

	constexpr int32 MaxEventNameBytes = 128;

	// The event's JSON as the browser's broker writes it, all but its cascade depth, which only the game thread knows.
	bool PrepareEvent(const FJsonObject& Args, const CrowdyModScriptsHostCalls::FGate& Gate, CrowdyModScriptsHostCalls::FPrepared& Out, TArray<uint8>& OutImmediate)
	{
		const TSharedPtr<FJsonValue> NameValue = Args.TryGetField(TEXT("name"));
		const FString Name = NameValue.IsValid() && NameValue->Type == EJson::String ? NameValue->AsString() : FString();
		if (Name.IsEmpty() || FTCHARToUTF8(*Name, Name.Len()).Length() > MaxEventNameBytes)
		{
			OutImmediate = CrowdyModScriptsHostCalls::Denied(TEXT("emit_event name must be a non-empty string"));
			return false;
		}
		const TSharedPtr<FJsonValue> TargetValue = Args.TryGetField(TEXT("target"));
		Out.bEventTargeted = TargetValue.IsValid() && TargetValue->Type != EJson::Null;
		Out.EventTarget = Out.bEventTargeted && TargetValue->Type == EJson::String ? TargetValue->AsString() : FString();
		if (Out.bEventTargeted && Out.EventTarget.IsEmpty())
		{
			OutImmediate = CrowdyModScriptsHostCalls::Denied(TEXT("emit_event target must be a module name"));
			return false;
		}
		const TSharedPtr<FJsonValue> Payload = Args.TryGetField(TEXT("payload"));
		const TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
		Event->SetStringField(TEXT("kind"), TEXT("grid_event"));
		Event->SetStringField(TEXT("gridId"), FString::Printf(TEXT("%lld"), Gate.Box.GridId));
		Event->SetStringField(TEXT("eventName"), Name);
		Event->SetField(TEXT("payload"), Payload.IsValid() ? Payload : MakeShared<FJsonValueNull>());
		Event->SetStringField(TEXT("sourceModule"), Gate.ModuleName);
		Event->SetField(TEXT("target"), Out.bEventTargeted ? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueString>(Out.EventTarget))
			: StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>()));
		// Written without its closing brace, so the game thread can add the depth last, where the browser writes it.
		Out.EventPrefix = ScriptsUtf8(ScriptsCondensed(Event));
		Out.EventPrefix.Pop();
		return true;
	}

	// The sender id a script's sends carry: for the actor it names by a 64-hex uuidHex, else for the game's, which is none.
	TArray<uint8> SendSender(const FJsonObject& Args, int64 GridId)
	{
		const TSharedPtr<FJsonValue> Value = Args.TryGetField(TEXT("uuidHex"));
		const FString Hex = Value.IsValid() && Value->Type == EJson::String ? Value->AsString() : FString();
		TArray<uint8> Name;
		// ASCII only: FChar::IsHexDigit also takes fullwidth digits, which HexToBytes asserts on.
		const auto IsHex = [](TCHAR Char) { return (Char >= TEXT('0') && Char <= TEXT('9')) || (Char >= TEXT('a') && Char <= TEXT('f')) || (Char >= TEXT('A') && Char <= TEXT('F')); };
		if (Hex.Len() == 64 && Algo::AllOf(Hex, IsHex))
		{
			Name.SetNumUninitialized(32);
			HexToBytes(Hex, Name.GetData());
		}
		return ScriptsUtf8(CrowdyModScriptsHostCalls::ClientHalfActorUuid(GridId, Name));
	}

	// Absent or null is no payload; a value that reads as no base64 text is refused.
	bool SendPayload(const FJsonObject& Args, TArray<uint8>& Out)
	{
		const TSharedPtr<FJsonValue> Value = Args.TryGetField(TEXT("payloadBase64"));
		const FString Text = SdkText(Value);
		if (Text.IsEmpty())
		{
			return !Value.IsValid() || Value->Type == EJson::Null || Value->Type == EJson::String;
		}
		return FBase64::Decode(Text, Out);
	}

	bool PrepareSpatial(const FJsonObject& Args, const FCrowdyModScriptGrid& Box, CrowdyModScriptsHostCalls::FPrepared& Out, TArray<uint8>& OutImmediate)
	{
		using CrowdyModScriptsHostCalls::Denied;
		UCrowdyModScripts::FRealtimeSend& Send = Out.Send;
		OutImmediate = WorldRefusal(WriteChunkAliases, Args);
		FString Error;
		if (!OutImmediate.IsEmpty() || !WorldChunk(Args, TEXT("chunkX"), TEXT("chunkY"), TEXT("chunkZ"), Send.Chunk, Error))
		{
			OutImmediate = OutImmediate.IsEmpty() ? Denied(Error) : OutImmediate;
			return false;
		}
		if (!ScriptsInBox(Box, Send.Chunk))
		{
			OutImmediate = WorldOutsideGrid();
			return false;
		}
		const FString Kind = SdkText(Args.TryGetField(TEXT("kind")));
		const auto IsKind = [&Kind](const TCHAR* Name) { return Kind.Equals(Name, ESearchCase::CaseSensitive); };
		const bool bEvent = IsKind(TEXT("client_event")) || IsKind(TEXT("server_event"));
		if (!bEvent && !IsKind(TEXT("actor")) && !IsKind(TEXT("text")))
		{
			OutImmediate = Denied(TEXT("host call 'emit_spatial' has an unknown kind"));
			return false;
		}
		// Another client would draw a pawn or a chat line from these.
		if (!bEvent)
		{
			OutImmediate = Denied(FString::Printf(TEXT("host call 'emit_spatial' cannot send kind '%s' in this game"), *Kind));
			return false;
		}
		if (!SendPayload(Args, Send.Payload) || Send.Payload.Num() < 2)
		{
			OutImmediate = Denied(TEXT("host call 'emit_spatial' needs a uint16 eventType prefix"));
			return false;
		}
		Send.Distance = static_cast<uint8>(WorldClamp(Args.TryGetField(TEXT("distance")), 0, CrowdyModScriptsHostCalls::MaxSpatialDistance));
		Send.Sender = SendSender(Args, Box.GridId);
		return true;
	}

	// A positive decimal id that fits int64, else 0.
	int64 SendChannelId(const TSharedPtr<FJsonValue>& Value)
	{
		const FString Id = SdkDecimalId(Value);
		const bool bFits = Id.Len() < 19 || (Id.Len() == 19 && Id.Compare(TEXT("9223372036854775807"), ESearchCase::CaseSensitive) <= 0);
		return !Id.IsEmpty() && bFits ? FCString::Atoi64(*Id) : 0;
	}

	TArray<uint8> ChannelOutsideGrid()
	{
		return CrowdyModScriptsHostCalls::Denied(TEXT("host call 'emit_channel' targets a channel outside this grid"));
	}

	bool PrepareChannel(const FJsonObject& Args, const FCrowdyModScriptGrid& Box, CrowdyModScriptsHostCalls::FPrepared& Out, TArray<uint8>& OutImmediate)
	{
		UCrowdyModScripts::FRealtimeSend& Send = Out.Send;
		Send.bChannel = true;
		// An id spelled any other way matches none of the grid's channels.
		Send.ChannelId = SendChannelId(Args.TryGetField(TEXT("channelId")));
		if (Send.ChannelId == 0)
		{
			OutImmediate = ChannelOutsideGrid();
			return false;
		}
		if (!SendPayload(Args, Send.Payload))
		{
			OutImmediate = CrowdyModScriptsHostCalls::Denied(TEXT("host call 'emit_channel' needs a base64 payload"));
			return false;
		}
		if (Send.Payload.Num() > CrowdyModScriptsHostCalls::MaxChannelPayloadBytes)
		{
			OutImmediate = CrowdyModScriptsHostCalls::Denied(FString::Printf(TEXT("Channel payload exceeds 1024 bytes: %d"), Send.Payload.Num()));
			return false;
		}
		Send.Sender = SendSender(Args, Box.GridId);
		return true;
	}

	bool PrepareCall(const CrowdyModScriptsHostCalls::FChecked& Call, CrowdyModScriptsHostCalls::FGate& Gate, CrowdyModScriptsHostCalls::FPrepared& Out,
		TArray<uint8>& OutImmediate)
	{
		const FJsonObject& Args = *Call.Args;
		const FCrowdyModScriptGrid& Box = Gate.Box;
		Out.Call = WorldCallOf(Call.Name);
		switch (Out.Call)
		{
		case ECall::Hud:
			Out.HudText = CrowdyModScriptsHostCalls::HudText(Args);
			return true;
		case ECall::Overlay:
			CrowdyModScriptsHostCalls::OverlayShapes(Args, Box, Out.Shapes);
			return true;
		case ECall::Clicks:
			return true;
		case ECall::VoxelSet:
			return PrepareVoxelSet(Args, Box, Out, OutImmediate);
		case ECall::ChunkGet:
		case ECall::VoxelsList:
		case ECall::Actors:
			return PrepareRead(Call.Name, Args, Box, Out, OutImmediate);
		case ECall::Event:
			return PrepareEvent(Args, Gate, Out, OutImmediate);
		case ECall::GridInfo:
			OutImmediate = SdkGridInfo(Box);
			return false;
		case ECall::Clock:
			OutImmediate = SdkClock();
			return false;
		case ECall::Permission:
			OutImmediate = SdkPermissionCheck(Args, Gate);
			return false;
		case ECall::UserStateGet:
			return true;
		case ECall::UserStateSet:
		{
			// Any state that is not a string clears it, as the browser's.
			const TSharedPtr<FJsonValue> State = Args.TryGetField(TEXT("stateBase64"));
			Out.bClearState = !State.IsValid() || State->Type != EJson::String;
			Out.State = Out.bClearState ? FString() : State->AsString();
			return true;
		}
		case ECall::Spatial:
			return PrepareSpatial(Args, Box, Out, OutImmediate);
		case ECall::Channel:
			return PrepareChannel(Args, Box, Out, OutImmediate);
		default:
			OutImmediate = CrowdyModScriptsHostCalls::Denied(TEXT("host call has no answer in this build"));
			return false;
		}
	}

	// The host's numbers, written as JSON can carry them.
	double WorldFinite(double Value)
	{
		return FMath::IsFinite(Value) ? Value : 0.0;
	}

	TSharedRef<FJsonObject> WorldClicks(const CrowdyModScriptsHostCalls::FWorldAnswer& Answer)
	{
		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("nowMs"), static_cast<double>(Answer.NowMs));
		Data->SetNumberField(TEXT("buttons"), Answer.Clicks.Buttons);
		const TSharedRef<FJsonObject> Holding = MakeShared<FJsonObject>();
		Holding->SetNumberField(TEXT("0"), Answer.Clicks.LeftHeldMs);
		Holding->SetNumberField(TEXT("1"), Answer.Clicks.MiddleHeldMs);
		Holding->SetNumberField(TEXT("2"), Answer.Clicks.RightHeldMs);
		Data->SetObjectField(TEXT("holdingMs"), Holding);
		TArray<TSharedPtr<FJsonValue>> Clicks;
		Clicks.Reserve(Answer.Clicks.Clicks.Num());
		for (const FCrowdyModScriptClick& Click : Answer.Clicks.Clicks)
		{
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("t"), Click.bDown ? TEXT("down") : TEXT("up"));
			Row->SetNumberField(TEXT("button"), Click.Button);
			Row->SetNumberField(TEXT("atMs"), static_cast<double>(Click.AtMs));
			if (!Click.bDown)
			{
				Row->SetNumberField(TEXT("heldMs"), Click.HeldMs);
			}
			Row->SetNumberField(TEXT("nx"), WorldFinite(Click.X));
			Row->SetNumberField(TEXT("ny"), WorldFinite(Click.Y));
			Clicks.Add(MakeShared<FJsonValueObject>(Row));
		}
		Data->SetArrayField(TEXT("clicks"), Clicks);
		return Data;
	}

	// The filled cells and the cells with a state, in index order.
	TArray<TSharedPtr<FJsonValue>> WorldVoxels(const FCrowdyModScriptChunk* Chunk)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		if (!Chunk)
		{
			return Rows;
		}
		TMap<int32, const FString*> States;
		for (const FCrowdyModScriptCellState& Cell : Chunk->States)
		{
			const bool bInside = Cell.X >= 0 && Cell.X < 16 && Cell.Y >= 0 && Cell.Y < 16 && Cell.Z >= 0 && Cell.Z < 16;
			if (bInside)
			{
				States.Add(Cell.X + 16 * Cell.Y + 256 * Cell.Z, &Cell.State);
			}
		}
		for (int32 Index = 0; Index < ChunkCells; ++Index)
		{
			const FString* const* State = States.Find(Index);
			if (Chunk->Types[Index] == 0 && !State)
			{
				continue;
			}
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("x"), Index % 16);
			Row->SetNumberField(TEXT("y"), (Index / 16) % 16);
			Row->SetNumberField(TEXT("z"), Index / 256);
			Row->SetNumberField(TEXT("voxelType"), Chunk->Types[Index]);
			if (State)
			{
				Row->SetStringField(TEXT("state"), **State);
			}
			else
			{
				Row->SetField(TEXT("state"), MakeShared<FJsonValueNull>());
			}
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Rows;
	}

	// The player's state row as the platform answered it, packed on the game thread: the row, or null when they have none.
	TArray<uint8> WorldStateRow(const CrowdyModScriptsHostCalls::FWorldAnswer& Answer)
	{
		FString Json;
		TSharedPtr<FJsonObject> Data;
		const bool bRead = CrowdyNativeExec::MsgpackToJson(Answer.Packed, Json) && CrowdyJsonSafety::IsNestingWithinLimit(Json)
			&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Data) && Data.IsValid();
		const TCHAR* Field = Answer.Call == ECall::UserStateGet ? TEXT("userAppState") : TEXT("updateUserAppState");
		const TSharedPtr<FJsonValue> Row = bRead ? Data->TryGetField(Field) : nullptr;
		if (!Row.IsValid())
		{
			return CrowdyModScriptsHostCalls::Denied(TEXT("host call answer could not be read"));
		}
		// A state may be larger than any argument, up to the browser's reply limit.
		TArray<uint8> Bytes = SdkAnswer(Row);
		return Bytes.Num() <= CrowdyModScriptsHostCalls::MaxReplyBytes ? Bytes
			: ScriptsUtf8(TEXT("{\"ok\":false,\"error\":{\"kind\":\"response_too_large\",\"message\":\"host response exceeds reply limit\"}}"));
	}

	TArray<TSharedPtr<FJsonValue>> WorldActors(const TArray<FCrowdyModScriptPlayer>& Players)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(Players.Num());
		for (const FCrowdyModScriptPlayer& Player : Players)
		{
			const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("uuid"), Player.ActorId);
			if (Player.bSelf)
			{
				Row->SetBoolField(TEXT("self"), true);
			}
			Row->SetStringField(TEXT("name"), Player.Name);
			Row->SetNumberField(TEXT("x"), WorldFinite(Player.Location.X));
			Row->SetNumberField(TEXT("y"), WorldFinite(Player.Location.Y));
			Row->SetNumberField(TEXT("z"), WorldFinite(Player.Location.Z));
			Row->SetNumberField(TEXT("yaw"), WorldFinite(Player.Yaw));
			Row->SetNumberField(TEXT("pitch"), WorldFinite(Player.Pitch));
			Row->SetNumberField(TEXT("program"), Player.Program);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Rows;
	}
}

bool CrowdyModScriptsHostCalls::IsServed(const FString& Name, bool bHostSet)
{
	const FServedCall* Call = ScriptsFindCall(Name);
	return Call && Call->bServed && (bHostSet || !Call->bHostAnswered);
}

bool CrowdyModScriptsHostCalls::AdmitWindow(TArray<double>& Times, int32& Oldest, double NowSeconds, int32 Cap)
{
	if (Times.Num() < Cap)
	{
		Times.Reserve(Cap);
		Times.Add(NowSeconds);
		return true;
	}
	if (NowSeconds - Times[Oldest] < 1.0)
	{
		return false;
	}
	Times[Oldest] = NowSeconds;
	Oldest = (Oldest + 1) % Cap;
	return true;
}

bool CrowdyModScriptsHostCalls::FWindow::Admit(double NowSeconds, int32 Cap)
{
	return AdmitWindow(Times, Oldest, NowSeconds, Cap);
}

bool CrowdyModScriptsHostCalls::Check(TConstArrayView<uint8> RequestUtf8, const TArray<FString>& Agreed, bool bHostSet, FBudget& Budget, double NowSeconds,
	FChecked& Out, TArray<uint8>& OutDenied)
{
	if (Budget.bTripped || !Budget.All.Admit(NowSeconds, MaxCallsPerSecond))
	{
		Budget.bTripped = true;
		OutDenied = Denied(TEXT("global host-call rate exceeded"));
		return false;
	}
	if (RequestUtf8.Num() > MaxRequestBytes)
	{
		OutDenied = Denied(TEXT("host call request too large"));
		return false;
	}
	TSharedPtr<FJsonObject> Request;
	FString Name;
	if (!ScriptsParseObject(RequestUtf8, Request) || !ScriptsText(Request, TEXT("fn"), Name) || !ScriptsIsCallName(Name))
	{
		OutDenied = Denied(TEXT("malformed host call"));
		return false;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Request->Values)
	{
		if (!ScriptsIsEnvelopeKey(Field.Key))
		{
			OutDenied = Denied(FString::Printf(TEXT("unexpected hostcall envelope field '%s'"), *Field.Key.Left(MaxCallNameLength)));
			return false;
		}
	}
	if (!IsServed(Name, bHostSet))
	{
		OutDenied = Denied(FString::Printf(TEXT("host call '%s' is not offered by this game"), *Name));
		return false;
	}
	if (!Agreed.ContainsByPredicate([&Name](const FString& Call) { return Call.Equals(Name, ESearchCase::CaseSensitive); }))
	{
		OutDenied = Denied(TEXT("host call is outside the capabilities the player consented to"));
		return false;
	}
	const TSharedPtr<FJsonValue> ArgsValue = Request->TryGetField(TEXT("args"));
	if (!ArgsValue.IsValid() || ArgsValue->Type != EJson::Object)
	{
		OutDenied = Denied(TEXT("host call args must be a plain object"));
		return false;
	}
	const TSharedRef<FJsonObject> Args = ArgsValue->AsObject().ToSharedRef();
	if (const TCHAR* Forbidden = ScriptsForbiddenKey(*Args))
	{
		OutDenied = Denied(FString::Printf(TEXT("host call cannot override '%s'"), Forbidden));
		return false;
	}
	// Measured as serialized, as the browser does, since a short spelling such as 1e9 can serialize long.
	if (ScriptsSerializedBytes(Args) > MaxArgsBytes)
	{
		OutDenied = Denied(TEXT("host call args exceed the browser sandbox limit"));
		return false;
	}
	const int32 Group = static_cast<int32>(ScriptsFindCall(Name)->Group);
	if (!Budget.Groups[Group].Admit(NowSeconds, GroupCaps[Group]))
	{
		OutDenied = Denied(FString::Printf(TEXT("rate cap exceeded for '%s'"), *Name));
		return false;
	}
	Out.Name = MoveTemp(Name);
	Out.Args = Args;
	return true;
}

bool CrowdyModScriptsHostCalls::Prepare(FGate& Gate, TArray<uint8>& InOutRequest, double NowSeconds)
{
	FChecked Call;
	TArray<uint8> Refusal;
	if (!Check(InOutRequest, Gate.Agreed, true, Gate.Budget, NowSeconds, Call, Refusal))
	{
		Gate.bTripped = Gate.Budget.bTripped;
		InOutRequest = MoveTemp(Refusal);
		return false;
	}
	FPrepared Prepared;
	TArray<uint8> Immediate;
	if (!PrepareCall(Call, Gate, Prepared, Immediate))
	{
		InOutRequest = MoveTemp(Immediate);
		return false;
	}
	{
		FScopeLock Guard(&Gate.Lock);
		Gate.Prepared = MoveTemp(Prepared);
	}
	InOutRequest = ScriptsUtf8(Call.Name);
	return true;
}

void CrowdyModScriptsHostCalls::Finish(FGate& Gate, TArray<uint8>& InOutAnswer)
{
	if (InOutAnswer.Num() != 1 || InOutAnswer[0] != WorldAnswerMarker)
	{
		return;
	}
	FWorldAnswer Answer;
	{
		FScopeLock Guard(&Gate.Lock);
		Answer = MoveTemp(Gate.Answer);
		Gate.Answer = FWorldAnswer();
	}
	InOutAnswer = WorldAnswer(Answer);
}

TArray<uint8> CrowdyModScriptsHostCalls::WorldAnswer(const FWorldAnswer& Answer)
{
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	switch (Answer.Call)
	{
	case ECall::Clicks:
		Data = WorldClicks(Answer);
		break;
	case ECall::ChunkGet:
		if (Answer.Chunk.IsValid())
		{
			Data->SetStringField(TEXT("voxelsBase64"), FBase64::Encode(Answer.Chunk->Types));
			break;
		}
		Data->SetField(TEXT("voxelsBase64"), MakeShared<FJsonValueNull>());
		break;
	case ECall::VoxelsList:
		Data->SetArrayField(TEXT("voxels"), WorldVoxels(Answer.Chunk.Get()));
		break;
	case ECall::Actors:
		Data->SetArrayField(TEXT("actors"), WorldActors(Answer.Players));
		break;
	case ECall::VoxelSet:
		return WorldVoxelAnswer(Answer.bVoxelOk, Answer.VoxelError);
	case ECall::UserStateGet:
	case ECall::UserStateSet:
		return WorldStateRow(Answer);
	default:
		return Denied(TEXT("host call has no answer in this build"));
	}
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetObjectField(TEXT("data"), Data);
	TArray<uint8> Bytes = ScriptsUtf8(ScriptsCondensed(Root));
	return Bytes.Num() > MaxArgsBytes ? Denied(TEXT("host call answer exceeds the browser sandbox limit")) : Bytes;
}

FString CrowdyModScriptsHostCalls::ClientHalfActorUuid(int64 GridId, TConstArrayView<uint8> Name)
{
#if WITH_CROWDY_MOD_SCRIPT_RUNTIME
	TArray<uint8> Input = ScriptsUtf8(TEXT("crowdy/client-half-actor/v1"));
	Input.Add(0);
	Input.Append(ScriptsUtf8(FString::Printf(TEXT("%lld"), GridId)));
	Input.Add(0);
	// Each octet is one character, so one past 0x7f takes two octets in UTF-8.
	for (const uint8 Octet : Name)
	{
		if (Octet < 0x80)
		{
			Input.Add(Octet);
			continue;
		}
		Input.Add(static_cast<uint8>(0xC0 | (Octet >> 6)));
		Input.Add(static_cast<uint8>(0x80 | (Octet & 0x3F)));
	}
	return FCrowdyModScriptModuleCache::Sha256Hex(Input).Left(32);
#else
	return FString();
#endif
}

TArray<uint8> CrowdyModScriptsHostCalls::Denied(const FString& Message)
{
	const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	Error->SetStringField(TEXT("kind"), TEXT("denied"));
	Error->SetStringField(TEXT("message"), Message);
	const TSharedRef<FJsonObject> Answer = MakeShared<FJsonObject>();
	Answer->SetBoolField(TEXT("ok"), false);
	Answer->SetObjectField(TEXT("error"), Error);
	return ScriptsUtf8(ScriptsCondensed(Answer));
}

TArray<uint8> CrowdyModScriptsHostCalls::Delivered(bool bDelivered)
{
	return ScriptsUtf8(bDelivered ? TEXT("{\"ok\":true,\"data\":{\"delivered\":true}}") : TEXT("{\"ok\":true,\"data\":{\"delivered\":false}}"));
}

FString CrowdyModScriptsHostCalls::HudText(const FJsonObject& Args)
{
	const TSharedPtr<FJsonObject>* Payload = nullptr;
	FString Text;
	if (!Args.TryGetObjectField(TEXT("payload"), Payload) || !ScriptsText(*Payload, TEXT("text"), Text))
	{
		return FString();
	}
	// Every break Slate wraps on counts, with CR LF as one.
	int32 Lines = 1;
	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		const bool bBreak = FChar::IsLinebreak(Text[Index]) && !(Text[Index] == TEXT('\r') && Index + 1 < Text.Len() && Text[Index + 1] == TEXT('\n'));
		if (bBreak && ++Lines > MaxHudLines)
		{
			Text.LeftInline(Text[Index] == TEXT('\n') && Index > 0 && Text[Index - 1] == TEXT('\r') ? Index - 1 : Index);
			break;
		}
	}
	return Text.Left(MaxHudChars);
}

void CrowdyModScriptsHostCalls::OverlayShapes(const FJsonObject& Args, const FCrowdyModScriptGrid& Box, TArray<FCrowdyModScriptShape>& Out)
{
	Out.Reset();
	const TSharedPtr<FJsonValue> Payload = Args.TryGetField(TEXT("payload"));
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	TArray<TSharedPtr<FJsonValue>> Single;
	if (Payload.IsValid() && Payload->Type == EJson::Array)
	{
		Rows = &Payload->AsArray();
	}
	else if (Payload.IsValid() && Payload->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> Record = Payload->AsObject();
		if (!Record->TryGetArrayField(TEXT("objects"), Rows) && !Record->TryGetArrayField(TEXT("nodes"), Rows) && !Record->TryGetArrayField(TEXT("shapes"), Rows)
			&& (Record->HasField(TEXT("x")) || Record->HasField(TEXT("z"))))
		{
			Single.Add(Payload);
			Rows = &Single;
		}
	}
	if (!Rows)
	{
		return;
	}
	const FVector Low(static_cast<double>(Box.LowChunkX) * VoxelsPerChunk, static_cast<double>(Box.LowChunkY) * VoxelsPerChunk,
		static_cast<double>(Box.LowChunkZ) * VoxelsPerChunk);
	const FVector High((static_cast<double>(Box.HighChunkX) + 1.0) * VoxelsPerChunk, (static_cast<double>(Box.HighChunkY) + 1.0) * VoxelsPerChunk,
		(static_cast<double>(Box.HighChunkZ) + 1.0) * VoxelsPerChunk);
	// Mesh rows count toward the 64 and claim their ids, as on the web, though they are not drawn.
	TArray<FString, TInlineAllocator<MaxOverlayShapes>> Seen;
	const int32 NumRead = FMath::Min(Rows->Num(), MaxOverlayRowsRead);
	for (int32 Index = 0; Index < NumRead && Seen.Num() < MaxOverlayShapes; ++Index)
	{
		FCrowdyModScriptShape Shape;
		bool bMesh = false;
		if (!OverlayShape((*Rows)[Index], Seen.Num(), Low, High, Shape, bMesh)
			|| Seen.ContainsByPredicate([&Shape](const FString& Id) { return Id.Equals(Shape.Id, ESearchCase::CaseSensitive); }))
		{
			continue;
		}
		Seen.Add(Shape.Id);
		if (!bMesh)
		{
			Out.Add(MoveTemp(Shape));
		}
	}
}

struct UCrowdyModScripts::FScript
{
	FModScriptRow Row;
	TSharedPtr<FCrowdyModScriptRun> Run;
	FCrowdyModScript Info;

	/** The running run's checks, shared with its worker. */
	TSharedPtr<CrowdyModScriptsHostCalls::FGate> Gate;

	/** The clicks since this script's last pointer_clicks, at most 64. */
	TArray<FCrowdyModScriptClick> Clicks;
	double HoldUntil = 0.0;

	/** Advances whenever an answer in flight for this script stops being wanted. */
	int32 Attempt = 0;
	int32 HostCalls = 0;
	ECrowdyModScriptStop EndReason = ECrowdyModScriptStop::Failed;
	bool bRunning = false;
	bool bFetching = false;

	/** The run stopped itself; the next frame reports it. */
	bool bEnded = false;

	/** The player took it back here, or elsewhere while it ran: not run or asked about again until it changes. */
	bool bTakenBack = false;

	/** The platform refused an agreement call for it: stopped until the player enters the grid again. */
	bool bRefused = false;
};

struct UCrowdyModScripts::FAuthor
{
	int32 PromptId = 0;
	FString PromptHash;

	/** The union hash the player said not now to, or took their trust back at. */
	FString DeclinedHash;
	bool bTrusting = false;
};

bool UCrowdyModScripts::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}
	const UWorld* World = Outer ? Outer->GetWorld() : nullptr;
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UCrowdyModScripts::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	LiveToken = MakeShared<uint8>(0);
	TWeakObjectPtr<UCrowdyModScripts> WeakThis(this);
	Platform = [WeakThis](const FPlatformCall& Call, TFunction<void(const FPlatformReply&)> OnReply)
	{
		if (UCrowdyModScripts* Self = WeakThis.Get())
		{
			Self->SendToPlatform(Call, MoveTemp(OnReply));
		}
	};
	// Null in the transient world the engine makes at startup.
	const UWorld* World = GetWorld();
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	if (GameInstance)
	{
		Session = GameInstance->GetSubsystem<UCrowdyGameSession>();
	}
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UCrowdyModScripts::HandleTicker));
}

void UCrowdyModScripts::Deinitialize()
{
	FTSTicker::RemoveTicker(TickHandle);
	TickHandle.Reset();
	LiveToken.Reset();
	++EnterSerial;
	bAskedChunk = false;
	LeaveGridFor(ECrowdyModScriptStop::Stopped);
	Super::Deinitialize();
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyModScripts::InitializeForTest(FPlatform InPlatform, UCrowdyGameSession* InSession)
{
	LiveToken = MakeShared<uint8>(0);
	Platform = MoveTemp(InPlatform);
	Session = InSession;
	NowForTest = 0.0;
}

void UCrowdyModScripts::TickForTest(double NowSeconds)
{
	NowForTest = NowSeconds;
	TickAt(NowSeconds);
}

int32 UCrowdyModScripts::GetHostCallsForTest(const FString& ModId) const
{
	const TSharedRef<FScript>* Found = Scripts.Find(ModId);
	return Found ? (*Found)->HostCalls : 0;
}
#endif

double UCrowdyModScripts::Now() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (NowForTest >= 0.0)
	{
		return NowForTest;
	}
#endif
	return FPlatformTime::Seconds();
}

bool UCrowdyModScripts::IsAvailable() const
{
#if WITH_CROWDY_MOD_SCRIPT_RUNTIME
	const TSharedPtr<FCrowdyModScriptRuntime> Runtime = FCrowdyModScriptRuntime::Get();
	return Runtime.IsValid() && Runtime->IsAvailable();
#else
	return false;
#endif
}

void UCrowdyModScripts::EnterGrid(int64 ChunkX, int64 ChunkY, int64 ChunkZ)
{
	if (!IsAvailable())
	{
		return;
	}
	const FInt64Vector Chunk(ChunkX, ChunkY, ChunkZ);
	if (Grid.GridId != 0 && ScriptsInBox(Grid, Chunk))
	{
		++EnterSerial;
		bAskedChunk = false;
		return;
	}
	if (bAskedChunk && AskedChunk == Chunk && Now() < AskAgainAt)
	{
		return;
	}
	const int32 Serial = ++EnterSerial;
	bAskedChunk = true;
	AskedChunk = Chunk;
	AskAgainAt = TNumericLimits<double>::Max();
	FPlatformCall Call;
	Call.Call = ECrowdyModScriptsCall::GridPermissions;
	Call.Chunk = Chunk;
	Send(Call, [this, Serial](const FPlatformReply& Reply) { ReadGrid(Serial, Reply); });
}

void UCrowdyModScripts::ReadGrid(int32 Serial, const FPlatformReply& Reply)
{
	if (Serial != EnterSerial)
	{
		return;
	}
	// A failed lookup answers nothing about the chunk, so the same chunk is asked again after a short hold.
	if (!Reply.bOk)
	{
		AskAgainAt = Now() + LookupRetrySeconds;
	}
	const FCrowdyGridPermissionRow* Deciding = Reply.bOk
		? Reply.Grids.FindByPredicate([](const FCrowdyGridPermissionRow& Row) { return Row.bDecidesEdits && Row.GridId > 0; }) : nullptr;
	if (Deciding && Deciding->GridId == Grid.GridId)
	{
		GridKeys = Deciding->PermissionKeys;
		// Running scripts answer grid_permission_check from these from now on.
		for (const TPair<FString, TSharedRef<FScript>>& Pair : Scripts)
		{
			if (Pair.Value->Gate.IsValid())
			{
				FScopeLock Guard(&Pair.Value->Gate->Lock);
				Pair.Value->Gate->Keys = GridKeys;
			}
		}
		return;
	}
	if (Grid.GridId != 0)
	{
		LeaveGridFor(ECrowdyModScriptStop::LeftGrid);
	}
	// A listener of the scripts the leave stopped may have moved the player on.
	if (!Deciding || Serial != EnterSerial)
	{
		return;
	}
	Grid.GridId = Deciding->GridId;
	Grid.LowChunkX = Deciding->LowChunkX;
	Grid.LowChunkY = Deciding->LowChunkY;
	Grid.LowChunkZ = Deciding->LowChunkZ;
	Grid.HighChunkX = Deciding->HighChunkX;
	Grid.HighChunkY = Deciding->HighChunkY;
	Grid.HighChunkZ = Deciding->HighChunkZ;
	GridKeys = Deciding->PermissionKeys;
	const UCrowdyGameSession* Signed = Session.Get();
	GridUserId = Signed ? Signed->GetUserID() : 0;
	AdvanceGrid();
	List();
}

void UCrowdyModScripts::AdvanceGrid()
{
	++GridSerial;
	++AgreementEpoch;
	PendingAgreements = 0;
	FailedListings = 0;
	bListing = false;
	bListingLost = false;
	bListAgain = false;
	// The sends waiting here belong to scripts the change of grid stops.
	GridChannels.Reset();
	ChannelWaiting.Reset();
	GridChannelsRetryAt = 0.0;
	bGridChannelsRead = false;
	bGridChannelsReading = false;
}

void UCrowdyModScripts::LeaveGrid()
{
	++EnterSerial;
	bAskedChunk = false;
	LeaveGridFor(ECrowdyModScriptStop::LeftGrid);
}

void UCrowdyModScripts::LeaveGridFor(ECrowdyModScriptStop Reason)
{
	AdvanceGrid();
	Grid = FCrowdyModScriptGrid();
	GridKeys.Reset();
	const TMap<FString, TSharedRef<FScript>> Leaving = MoveTemp(Scripts);
	const TMap<int64, TSharedRef<FAuthor>> Forgotten = MoveTemp(Authors);
	Scripts.Reset();
	Authors.Reset();
	for (const TPair<FString, TSharedRef<FScript>>& Pair : Leaving)
	{
		StopRun(Pair.Value, Reason);
	}
	for (const TPair<int64, TSharedRef<FAuthor>>& Pair : Forgotten)
	{
		if (Pair.Value->PromptId != 0)
		{
			Withdraw(*Pair.Value);
		}
	}
}

FCrowdyModScriptGrid UCrowdyModScripts::GetGrid() const
{
	return Grid;
}

void UCrowdyModScripts::SetHost(TScriptInterface<ICrowdyModScriptHost> Host)
{
	if (HostObject.Get() != Host.GetObject())
	{
		ClearFromHost();
	}
	HostObject = Host.GetObject();
}

void UCrowdyModScripts::ClearHost(TScriptInterface<ICrowdyModScriptHost> Host)
{
	if (HostObject.Get() == Host.GetObject())
	{
		ClearFromHost();
		HostObject.Reset();
	}
}

void UCrowdyModScripts::ClearFromHost()
{
	UObject* Host = HostObject.Get();
	if (!Host || !Host->GetClass()->ImplementsInterface(UCrowdyModScriptHost::StaticClass()))
	{
		return;
	}
	// Copied first: the host may stop scripts from inside ClearScript.
	TArray<FCrowdyModScript, TInlineAllocator<8>> Shown;
	for (const TPair<FString, TSharedRef<FScript>>& Pair : Scripts)
	{
		if (Pair.Value->bRunning)
		{
			Shown.Add(Pair.Value->Info);
		}
	}
	for (const FCrowdyModScript& Script : Shown)
	{
		ICrowdyModScriptHost::Execute_ClearScript(Host, Script);
	}
}

void UCrowdyModScripts::Send(const FPlatformCall& Call, TFunction<void(const FPlatformReply&)> OnReply)
{
	if (!Platform || !LiveToken.IsValid())
	{
		return;
	}
	TWeakObjectPtr<UCrowdyModScripts> WeakThis(this);
	TWeakPtr<uint8> Live = LiveToken;
	Platform(Call, [WeakThis, Live, OnReply = MoveTemp(OnReply)](const FPlatformReply& Reply)
	{
		if (WeakThis.IsValid() && Live.IsValid())
		{
			OnReply(Reply);
		}
	});
}

void UCrowdyModScripts::SendAgreement(const FPlatformCall& Call, TFunction<void(const FPlatformReply&, bool bSameGrid)> OnReply)
{
	if (!Platform || !LiveToken.IsValid())
	{
		return;
	}
	++PendingAgreements;
	++AgreementEpoch;
	const int32 Serial = GridSerial;
	Send(Call, [this, Serial, OnReply = MoveTemp(OnReply)](const FPlatformReply& Reply)
	{
		// A change of grid already reset the count this call was part of.
		const bool bSameGrid = Serial == GridSerial;
		if (bSameGrid)
		{
			--PendingAgreements;
			++AgreementEpoch;
		}
		OnReply(Reply, bSameGrid);
	});
}

void UCrowdyModScripts::SendToPlatform(const FPlatformCall& Call, TFunction<void(const FPlatformReply&)> OnReply)
{
	const UWorld* World = GetWorld();
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	if (Call.Call == ECrowdyModScriptsCall::GridPermissions)
	{
		UCrowdyClaims* Claims = GameInstance ? GameInstance->GetSubsystem<UCrowdyClaims>() : nullptr;
		if (!Claims)
		{
			DeferWhileLive(LiveToken, [OnReply = MoveTemp(OnReply)]() { OnReply(ScriptsFailure(ScriptsNoClient)); });
			return;
		}
		Claims->GetGridPermissions(Call.Chunk.X, Call.Chunk.Y, Call.Chunk.Z, [OnReply = MoveTemp(OnReply)](const FCrowdyGridPermissions& Answer)
		{
			FPlatformReply Reply;
			Reply.bOk = Answer.Result == ECrowdyClaimResult::Success;
			Reply.Message = Answer.Message;
			Reply.Grids = Answer.Grids;
			OnReply(Reply);
		});
		return;
	}

	const UCrowdyGameSession* Signed = GameInstance ? GameInstance->GetSubsystem<UCrowdyGameSession>() : nullptr;
	FCrowdyCppClient* Client = Signed && !Signed->GetGameToken().IsEmpty() ? ResolveApiClient(GameInstance, ScriptsLogName) : nullptr;
	if (!Client)
	{
		DeferWhileLive(LiveToken, [OnReply = MoveTemp(OnReply)]() { OnReply(ScriptsFailure(ScriptsNoClient)); });
		return;
	}
	const int64 AppId = ResolveAppId(GameInstance);

	if (Call.Call == ECrowdyModScriptsCall::FetchArtifact)
	{
		const TSharedRef<bool> bInCall = MakeShared<bool>(true);
		const TWeakPtr<uint8> Live = LiveToken;
		Client->FetchModClientArtifact(AppId, Call.ModId, [bInCall, Live, ModId = Call.ModId, OnReply = MoveTemp(OnReply)](FCrowdyCppModClientArtifact Artifact)
		{
			FPlatformReply Reply;
			const bool bOtherMod = Artifact.bOk && !Artifact.ModId.Equals(ModId, ESearchCase::CaseSensitive);
			Reply.bOk = Artifact.bOk && !bOtherMod;
			Reply.Code = Artifact.ErrorCode;
			Reply.Reason = Artifact.ErrorReason;
			Reply.Message = bOtherMod ? FString(TEXT("The platform answered with another Mod Script.")) : Artifact.ErrorMessage;
			Reply.bCheckRefused = Artifact.bCheckRefused || bOtherMod;
			Reply.Bytes = MoveTemp(Artifact.Bytes);
			Reply.Digest = Artifact.Digest;
			Reply.FuelPerDispatch = Artifact.FuelPerDispatch;
			Reply.TickIntervalMs = Artifact.TickIntervalMs;
			UE_CLOG(!Reply.bOk, LogCrowdyServices, Warning, TEXT("[%s] ExecModClientArtifact ended %s"), ScriptsLogName,
				Reply.Code.IsEmpty() ? TEXT("without a code") : *Reply.Code);
			if (!*bInCall)
			{
				OnReply(Reply);
				return;
			}
			DeferWhileLive(Live.Pin(), [OnReply, Deferred = MoveTemp(Reply)]() { OnReply(Deferred); });
		});
		*bInCall = false;
		return;
	}

	const bool bChannels = Call.Call == ECrowdyModScriptsCall::GridChannels;
	if (bChannels || Call.Call == ECrowdyModScriptsCall::UserStateGet || Call.Call == ECrowdyModScriptsCall::UserStateSet)
	{
		SendStateOrChannels(*Client, Call, AppId, MoveTemp(OnReply));
		return;
	}

	const TCHAR* Operation = ScriptsOperation(Call.Call);
	const TCHAR* Field = ScriptsAnswerField(Call.Call);
	const bool bList = Call.Call == ECrowdyModScriptsCall::ListMods;
	RunOpDeferred(*Client, ECrowdyCppApiDomain::Exec, Operation, ScriptsVariables(Call, AppId), LiveToken,
		[Operation, Field, bList, OnReply = MoveTemp(OnReply)](FCrowdyCppJsonResult Result)
		{
			FPlatformReply Reply;
			Reply.Code = Result.ErrorCode;
			Reply.Reason = Result.ErrorReason;
			const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
			const bool bAnswered = Result.bTransportOk && Result.Data.IsValid();
			if (bAnswered && bList && Result.Data->TryGetArrayField(Field, Rows))
			{
				Reply.Rows = *Rows;
			}
			// A mutation's boolean is not its verdict: a refusal arrives as an error.
			Reply.bOk = bAnswered && (bList ? Rows != nullptr : Result.Data->HasField(Field));
			if (!Reply.bOk)
			{
				Reply.Message = Result.bTransportOk ? FString(ScriptsNoAnswer) : Result.ErrorMessage;
				UE_LOG(LogCrowdyServices, Warning, TEXT("[%s] %s ended %s"), ScriptsLogName, Operation,
					Reply.Code.IsEmpty() ? TEXT("without a code") : *Reply.Code);
			}
			OnReply(Reply);
		});
}

void UCrowdyModScripts::SendStateOrChannels(FCrowdyCppClient& Client, const FPlatformCall& Call, int64 AppId, TFunction<void(const FPlatformReply&)> OnReply)
{
	const bool bChannels = Call.Call == ECrowdyModScriptsCall::GridChannels;
	const bool bGet = Call.Call == ECrowdyModScriptsCall::UserStateGet;
	const TCHAR* Operation = bChannels ? TEXT("GridChannels") : bGet ? TEXT("UserAppState") : TEXT("UpdateUserAppState");
	const TCHAR* Field = bChannels ? TEXT("gridChannels") : bGet ? TEXT("userAppState") : TEXT("updateUserAppState");
	const TSharedPtr<FJsonObject> Variables = MakeShared<FJsonObject>();
	if (Call.Call == ECrowdyModScriptsCall::UserStateSet)
	{
		const TSharedRef<FJsonObject> Input = MakeShared<FJsonObject>();
		Input->SetStringField(TEXT("appId"), BigInt(AppId));
		if (Call.bClearState)
		{
			Input->SetField(TEXT("state"), MakeShared<FJsonValueNull>());
		}
		else
		{
			Input->SetStringField(TEXT("state"), Call.State);
		}
		Variables->SetObjectField(TEXT("input"), Input);
	}
	else
	{
		Variables->SetStringField(TEXT("appId"), BigInt(AppId));
	}
	if (bChannels)
	{
		Variables->SetStringField(TEXT("gridId"), BigInt(Call.GridId));
	}
	RunOpDeferred(Client, bChannels ? ECrowdyCppApiDomain::Grids : ECrowdyCppApiDomain::State, Operation, Variables, LiveToken,
		[Operation, Field, bChannels, OnReply = MoveTemp(OnReply)](FCrowdyCppJsonResult Result)
		{
			FPlatformReply Reply;
			Reply.Code = Result.ErrorCode;
			Reply.Reason = Result.ErrorReason;
			// A state row can be as large as the state, so it stays packed for the script's worker to read.
			Reply.Packed = MoveTemp(Result.Msgpack);
			const TSharedPtr<FJsonValue> Rows = bChannels && Result.bTransportOk && Result.Data.IsValid() ? Result.Data->TryGetField(Field) : nullptr;
			Reply.bOk = Result.bTransportOk && (bChannels ? Rows.IsValid() && Rows->Type == EJson::Array : !Reply.Packed.IsEmpty());
			const TArray<TSharedPtr<FJsonValue>> NoRows;
			for (const TSharedPtr<FJsonValue>& Row : Reply.bOk && bChannels ? Rows->AsArray() : NoRows)
			{
				const TSharedPtr<FJsonObject> Channel = Row.IsValid() && Row->Type == EJson::Object ? Row->AsObject() : nullptr;
				const int64 Id = Channel.IsValid() ? SendChannelId(Channel->TryGetField(TEXT("groupId"))) : 0;
				if (Id != 0)
				{
					Reply.ChannelIds.Add(Id);
				}
			}
			if (!Reply.bOk)
			{
				Reply.Message = !Result.ErrorMessage.IsEmpty() ? Result.ErrorMessage : FString(ScriptsNoAnswer);
				// A state call's failure goes to the script that made it, which may make one every round trip.
				UE_CLOG(bChannels, LogCrowdyServices, Warning, TEXT("[%s] %s ended %s"), ScriptsLogName, Operation,
					Reply.Code.IsEmpty() ? TEXT("without a code") : *Reply.Code);
			}
			OnReply(Reply);
		}, !bChannels);
}

bool UCrowdyModScripts::HandleTicker(float DeltaSeconds)
{
	TickAt(Now());
	return true;
}

void UCrowdyModScripts::TickAt(double NowSeconds)
{
	// A chunk read once is answered from FrameReads for the rest of the frame.
	FrameReads.Reset();
	DrainClicks();
	if (Grid.GridId == 0)
	{
		return;
	}
	const UCrowdyGameSession* Signed = Session.Get();
	if (Signed && Signed->GetUserID() != GridUserId)
	{
		++EnterSerial;
		bAskedChunk = false;
		LeaveGridFor(ECrowdyModScriptStop::Stopped);
		return;
	}
	if (NowSeconds >= NextListAt)
	{
		List();
	}
	StartAgreed();

#if WITH_CROWDY_MOD_SCRIPT_RUNTIME
	// Copied first: a run may end itself inside its own tick.
	TickRuns.Reset();
	for (const TPair<FString, TSharedRef<FScript>>& Pair : Scripts)
	{
		if (Pair.Value->bRunning && !Pair.Value->bEnded && Pair.Value->Run.IsValid())
		{
			TickRuns.Add(Pair.Value->Run);
		}
	}
	for (const TSharedPtr<FCrowdyModScriptRun>& Run : TickRuns)
	{
		Run->Tick(NowSeconds);
	}
	// The first run to drain rotates each frame, so no run always gets what the others left.
	const int32 NumRuns = TickRuns.Num();
	const int32 First = NumRuns > 0 ? static_cast<int32>(DrainStart++ % static_cast<uint32>(NumRuns)) : 0;
	double BudgetMs = HostBudgetMs;
#if WITH_DEV_AUTOMATION_TESTS
	BudgetMs = HostBudgetMsForTest >= 0.0 ? HostBudgetMsForTest : BudgetMs;
#endif
	for (int32 Offset = 0; Offset < NumRuns; ++Offset)
	{
		const double DrainStarted = FPlatformTime::Seconds();
		TickRuns[(First + Offset) % NumRuns]->DrainHostCalls(FMath::Max(BudgetMs, MinDrainMs));
		BudgetMs -= (FPlatformTime::Seconds() - DrainStarted) * 1000.0;
	}
	TickRuns.Reset();
#endif

	TArray<TSharedRef<FScript>, TInlineAllocator<8>> Ended;
	for (const TPair<FString, TSharedRef<FScript>>& Pair : Scripts)
	{
		FScript& Script = *Pair.Value;
		if (Script.bRunning && !Script.bEnded && Script.Gate.IsValid() && Script.Gate->bTripped)
		{
			UE_LOG(LogCrowdyServices, Warning, TEXT("[%s] %s made more than %d host calls in a second; its circuit is open"), ScriptsLogName, *Script.Info.Name,
				CrowdyModScriptsHostCalls::MaxCallsPerSecond);
			Script.bEnded = true;
			Script.EndReason = ECrowdyModScriptStop::CircuitOpen;
		}
		if (Script.bRunning && Script.bEnded)
		{
			Ended.Add(Pair.Value);
		}
	}
	const int32 Serial = GridSerial;
	for (const TSharedRef<FScript>& Script : Ended)
	{
		if (Serial != GridSerial)
		{
			return;
		}
		Script->HoldUntil = NowSeconds + LongHoldSeconds;
		StopRun(Script, Script->EndReason);
	}
}

void UCrowdyModScripts::List()
{
	if (Grid.GridId == 0 || bListing)
	{
		return;
	}
	bListing = true;
	bListAgain = false;
	NextListAt = Now() + ListSeconds;
	const int32 Serial = GridSerial;
	const int32 Epoch = AgreementEpoch;
	FPlatformCall Call;
	Call.Call = ECrowdyModScriptsCall::ListMods;
	Call.GridId = Grid.GridId;
	Send(Call, [this, Serial, Epoch](const FPlatformReply& Reply)
	{
		if (Serial != GridSerial)
		{
			return;
		}
		bListing = false;
		NextListAt = bListAgain ? 0.0 : Now() + ListSeconds;
		if (!Reply.bOk)
		{
			// A refusal says the grid serves this player nothing; a run of unreadable answers says nothing is known.
			++FailedListings;
			if (!Reply.Code.IsEmpty() || FailedListings >= MaxFailedListings)
			{
				LoseListing();
			}
			return;
		}
		FailedListings = 0;
		// A listing that raced an agreement call may show the agreement before it changed.
		if (Epoch != AgreementEpoch || PendingAgreements > 0)
		{
			return;
		}
		bListingLost = false;
		ReadListing(Reply.Rows);
	});
}

void UCrowdyModScripts::ListAgain()
{
	bListAgain = true;
	if (!bListing)
	{
		NextListAt = 0.0;
	}
}

void UCrowdyModScripts::LoseListing()
{
	bListingLost = true;
	TArray<TSharedRef<FScript>, TInlineAllocator<8>> Running;
	for (const TPair<FString, TSharedRef<FScript>>& Pair : Scripts)
	{
		if (Pair.Value->bRunning)
		{
			Running.Add(Pair.Value);
		}
	}
	const int32 Serial = GridSerial;
	for (const TSharedRef<FScript>& Script : Running)
	{
		if (Serial != GridSerial)
		{
			return;
		}
		StopRun(Script, ECrowdyModScriptStop::Removed);
	}
}

void UCrowdyModScripts::ReadListing(const TArray<TSharedPtr<FJsonValue>>& Rows)
{
	// A row that does not read is left out, so its script counts as no longer listed.
	TArray<FModScriptRow> Served;
	for (const TSharedPtr<FJsonValue>& Value : Rows)
	{
		FModScriptRow Row;
		if (ScriptsReadRow(Value, Grid.GridId, Row))
		{
			Served.Add(MoveTemp(Row));
		}
	}
	UE_CLOG(Served.Num() != Rows.Num(), LogCrowdyServices, Warning, TEXT("[%s] %d of the grid's Mod Script rows did not read."), ScriptsLogName,
		Rows.Num() - Served.Num());

	const int32 Serial = GridSerial;
	TArray<TSharedRef<FScript>> Gone;
	for (const TPair<FString, TSharedRef<FScript>>& Pair : Scripts)
	{
		if (!Served.ContainsByPredicate([&Pair](const FModScriptRow& Row) { return Row.ModId.Equals(Pair.Key, ESearchCase::CaseSensitive); }))
		{
			Gone.Add(Pair.Value);
		}
	}
	for (const TSharedRef<FScript>& Script : Gone)
	{
		Scripts.Remove(Script->Row.ModId);
		StopRun(Script, ECrowdyModScriptStop::Removed);
		if (Serial != GridSerial)
		{
			return;
		}
	}

	for (FModScriptRow& Row : Served)
	{
		const TSharedRef<FScript>* Found = Scripts.Find(Row.ModId);
		if (!Found)
		{
			const TSharedRef<FScript> Added = MakeShared<FScript>();
			Added->Row = MoveTemp(Row);
			Scripts.Add(Added->Row.ModId, Added);
			continue;
		}
		const TSharedRef<FScript> Script = *Found;
		const bool bChanged = !Script->Row.Digest.Equals(Row.Digest, ESearchCase::CaseSensitive)
			|| !Script->Row.Hash.Equals(Row.Hash, ESearchCase::CaseSensitive) || Script->Row.TickIntervalMs != Row.TickIntervalMs;
		const bool bAgreed = Row.bConsented || Row.bTrustsAuthor;
		Script->Row = MoveTemp(Row);
		if (bChanged)
		{
			Script->bTakenBack = false;
			Script->bRefused = false;
			Script->bFetching = false;
			Script->HoldUntil = 0.0;
			++Script->Attempt;
			StopRun(Script, ECrowdyModScriptStop::Changed);
		}
		else if (Script->bRunning && !bAgreed)
		{
			Script->bTakenBack = true;
			StopRun(Script, ECrowdyModScriptStop::Unconsented);
		}
		if (Serial != GridSerial)
		{
			return;
		}
	}

	Ask();
	if (Serial == GridSerial)
	{
		StartAgreed();
	}
}

void UCrowdyModScripts::Ask()
{
	const int32 Serial = GridSerial;
	TArray<int64, TInlineAllocator<8>> AuthorIds;
	for (const TPair<FString, TSharedRef<FScript>>& Pair : Scripts)
	{
		AuthorIds.AddUnique(Pair.Value->Row.AuthorId);
	}
	TArray<TSharedRef<FAuthor>, TInlineAllocator<8>> Unlisted;
	for (const TPair<int64, TSharedRef<FAuthor>>& Pair : Authors)
	{
		if (Pair.Value->PromptId != 0 && !AuthorIds.Contains(Pair.Key))
		{
			Unlisted.Add(Pair.Value);
		}
	}
	for (const TSharedRef<FAuthor>& Author : Unlisted)
	{
		Withdraw(*Author);
	}

	for (const int64 AuthorId : AuthorIds)
	{
		if (Serial != GridSerial)
		{
			return;
		}
		FCrowdyModScriptPrompt Prompt;
		const FModScriptRow* Union = nullptr;
		bool bUnagreed = false;
		// A trust covers every current script of the author, so the prompt names them all; only those not taken back ask.
		for (const TPair<FString, TSharedRef<FScript>>& Pair : Scripts)
		{
			const FScript& Script = *Pair.Value;
			if (Script.Row.AuthorId != AuthorId)
			{
				continue;
			}
			Union = &Script.Row;
			Prompt.Scripts.Add(Script.Row.Name);
			bUnagreed |= !Script.bTakenBack && !Script.bRefused && !Script.Row.bConsented && !Script.Row.bTrustsAuthor;
		}
		const TSharedRef<FAuthor> Author = FindOrAddAuthor(AuthorId);
		const bool bAsk = Union && bUnagreed && !Author->bTrusting && !Union->AuthorHash.Equals(Author->DeclinedHash, ESearchCase::CaseSensitive);
		if (Author->PromptId != 0 && (!bAsk || !Union->AuthorHash.Equals(Author->PromptHash, ESearchCase::CaseSensitive)))
		{
			Withdraw(*Author);
		}
		if (!bAsk || Author->PromptId != 0 || Serial != GridSerial)
		{
			continue;
		}
		Prompt.PromptId = ++NextPromptId;
		Prompt.GridId = Grid.GridId;
		Prompt.AuthorId = AuthorId;
		Prompt.Calls = Union->AuthorCalls;
		const bool bHostSet = HostObject.IsValid();
		for (const FString& Call : Prompt.Calls)
		{
			if (!CrowdyModScriptsHostCalls::IsServed(Call, bHostSet))
			{
				Prompt.NotOffered.Add(Call);
				continue;
			}
			Prompt.Groups.AddUnique(ScriptsFindCall(Call)->Group);
		}
		Prompt.Groups.Sort();
		Author->PromptId = Prompt.PromptId;
		Author->PromptHash = Union->AuthorHash;
		OnModScriptPrompt.Broadcast(Prompt);
	}
}

void UCrowdyModScripts::Withdraw(FAuthor& Author)
{
	const int32 PromptId = Author.PromptId;
	Author.PromptId = 0;
	Author.PromptHash.Reset();
	OnModScriptPromptWithdrawn.Broadcast(PromptId);
}

TSharedRef<UCrowdyModScripts::FAuthor> UCrowdyModScripts::FindOrAddAuthor(int64 AuthorId)
{
	if (const TSharedRef<FAuthor>* Found = Authors.Find(AuthorId))
	{
		return *Found;
	}
	return Authors.Add(AuthorId, MakeShared<FAuthor>());
}

TArray<TSharedRef<UCrowdyModScripts::FScript>> UCrowdyModScripts::ScriptsOf(int64 AuthorId) const
{
	TArray<TSharedRef<FScript>> Theirs;
	for (const TPair<FString, TSharedRef<FScript>>& Pair : Scripts)
	{
		if (Pair.Value->Row.AuthorId == AuthorId)
		{
			Theirs.Add(Pair.Value);
		}
	}
	return Theirs;
}

void UCrowdyModScripts::AnswerPrompt(int32 PromptId, bool bRun)
{
	int64 AuthorId = 0;
	for (const TPair<int64, TSharedRef<FAuthor>>& Pair : Authors)
	{
		if (PromptId != 0 && Pair.Value->PromptId == PromptId)
		{
			AuthorId = Pair.Key;
			break;
		}
	}
	if (AuthorId == 0)
	{
		return;
	}
	const TSharedRef<FAuthor> Author = Authors.FindChecked(AuthorId);
	const FString Hash = Author->PromptHash;
	Author->PromptId = 0;
	Author->PromptHash.Reset();
	if (!bRun)
	{
		Author->DeclinedHash = Hash;
		return;
	}
	Author->bTrusting = true;
	FPlatformCall Call;
	Call.Call = ECrowdyModScriptsCall::TrustAuthor;
	Call.GridId = Grid.GridId;
	Call.AuthorId = AuthorId;
	Call.Hash = Hash;
	SendAgreement(Call, [this, AuthorId](const FPlatformReply& Reply, bool bSameGrid)
	{
		if (!bSameGrid)
		{
			return;
		}
		if (const TSharedRef<FAuthor>* Found = Authors.Find(AuthorId))
		{
			(*Found)->bTrusting = false;
		}
		// A stale hash means the union changed since it was shown: list again and ask again.
		if (!Reply.bOk && ScriptsNamesCode(Reply, TEXT("CONFLICT")))
		{
			ListAgain();
			return;
		}
		if (!Reply.bOk)
		{
			RefuseAuthor(AuthorId, Reply);
			return;
		}
		for (const TSharedRef<FScript>& Script : ScriptsOf(AuthorId))
		{
			Script->Row.bTrustsAuthor = true;
		}
		StartAgreed();
	});
}

void UCrowdyModScripts::StartAgreed()
{
	if (Grid.GridId == 0 || bListingLost)
	{
		return;
	}
	const double NowSeconds = Now();
	TArray<TSharedRef<FScript>, TInlineAllocator<8>> Due;
	for (const TPair<FString, TSharedRef<FScript>>& Pair : Scripts)
	{
		const FScript& Script = *Pair.Value;
		if (!Script.bRunning && !Script.bFetching && !Script.bTakenBack && !Script.bRefused && NowSeconds >= Script.HoldUntil
			&& (Script.Row.bConsented || Script.Row.bTrustsAuthor))
		{
			Due.Add(Pair.Value);
		}
	}
	for (const TSharedRef<FScript>& Script : Due)
	{
		Fetch(Script);
	}
}

void UCrowdyModScripts::Fetch(const TSharedRef<FScript>& Script)
{
	Script->bFetching = true;
	const int32 Attempt = ++Script->Attempt;
	const int32 Serial = GridSerial;
	const TWeakPtr<FScript> Weak = Script;
	FPlatformCall Call;
	Call.Call = ECrowdyModScriptsCall::FetchArtifact;
	Call.ModId = Script->Row.ModId;
	Send(Call, [this, Weak, Attempt, Serial](const FPlatformReply& Reply)
	{
		const TSharedPtr<FScript> Pinned = Weak.Pin();
		const TSharedRef<FScript>* Listed = Pinned ? Scripts.Find(Pinned->Row.ModId) : nullptr;
		if (!Listed || *Listed != Pinned.ToSharedRef() || Serial != GridSerial || Pinned->Attempt != Attempt || !Pinned->bFetching)
		{
			return;
		}
		ReadArtifact(Pinned.ToSharedRef(), Reply);
	});
}

void UCrowdyModScripts::ReadArtifact(const TSharedRef<FScript>& Script, const FPlatformReply& Reply)
{
	// Still fetching until the module is compiled, so nothing fetches it twice meanwhile.
	if (Reply.bOk && Reply.Digest.Equals(Script->Row.Digest, ESearchCase::IgnoreCase) && (Script->Row.bConsented || Script->Row.bTrustsAuthor))
	{
		Start(Script, Reply);
		return;
	}
	Script->bFetching = false;
	if (!Script->Row.bConsented && !Script->Row.bTrustsAuthor)
	{
		return;
	}
	const bool bLong = Reply.bOk || Reply.bCheckRefused || ScriptsNamesCode(Reply, TEXT("RATE_LIMITED"));
	Fail(Script, bLong ? LongHoldSeconds : NotFoundHoldSeconds);
}

void UCrowdyModScripts::Start(const TSharedRef<FScript>& Script, const FPlatformReply& Artifact)
{
#if WITH_CROWDY_MOD_SCRIPT_RUNTIME
	const TSharedPtr<FCrowdyModScriptRuntime> Runtime = FCrowdyModScriptRuntime::Get();
	if (!Runtime.IsValid())
	{
		Script->bFetching = false;
		Fail(Script, LongHoldSeconds);
		return;
	}
	const TWeakObjectPtr<UCrowdyModScripts> WeakThis(this);
	const TWeakPtr<FScript> Weak = Script;
	const int32 Attempt = Script->Attempt;
	const int32 Serial = GridSerial;
	const FString Digest = Script->Row.Digest;
	const int64 FuelPerDispatch = Artifact.FuelPerDispatch;
	const int32 TickIntervalMs = Artifact.TickIntervalMs > 0 ? Artifact.TickIntervalMs : Script->Row.TickIntervalMs;
	FCrowdyModScriptModuleCache::GetAsync(Runtime.ToSharedRef(), Artifact.Digest, Artifact.Bytes,
		[WeakThis, Weak, Attempt, Serial, Digest, FuelPerDispatch, TickIntervalMs](TSharedPtr<FCrowdyModScriptModule> Module, const FString& Error)
		{
			UCrowdyModScripts* Self = WeakThis.Get();
			const TSharedPtr<FScript> Pinned = Weak.Pin();
			const TSharedRef<FScript>* Listed = Self && Self->LiveToken.IsValid() && Pinned ? Self->Scripts.Find(Pinned->Row.ModId) : nullptr;
			// Anything that changed or took the script back while it compiled advanced its attempt.
			if (!Listed || *Listed != Pinned.ToSharedRef() || Serial != Self->GridSerial || Pinned->Attempt != Attempt || !Pinned->bFetching
				|| Pinned->bTakenBack || !Pinned->Row.Digest.Equals(Digest, ESearchCase::CaseSensitive))
			{
				return;
			}
			Pinned->bFetching = false;
			if (!Module.IsValid())
			{
				Self->Fail(Pinned.ToSharedRef(), LongHoldSeconds);
				return;
			}
			Self->StartRun(Pinned.ToSharedRef(), Module.ToSharedRef(), FuelPerDispatch, TickIntervalMs);
		});
#else
	Script->bFetching = false;
	Fail(Script, LongHoldSeconds);
#endif
}

#if WITH_CROWDY_MOD_SCRIPT_RUNTIME
void UCrowdyModScripts::StartRun(const TSharedRef<FScript>& Script, const TSharedRef<FCrowdyModScriptModule>& Module, int64 FuelPerDispatch,
	int32 TickIntervalMs)
{
	const int32 Generation = ++NextGeneration;
	Script->Info.ModId = Script->Row.ModId;
	Script->Info.Name = Script->Row.Name;
	Script->Info.AuthorId = Script->Row.AuthorId;
	Script->Info.GridId = Grid.GridId;
	Script->Info.Version = Script->Row.Version;
	Script->Info.Generation = Generation;
	Script->Info.Calls = Script->Row.Calls;
	Script->Clicks.Reset();
	Script->bRunning = true;
	Script->bEnded = false;
	const TSharedRef<CrowdyModScriptsHostCalls::FGate> Gate = MakeShared<CrowdyModScriptsHostCalls::FGate>();
	Gate->Agreed = Script->Info.Calls;
	Gate->Box = Grid;
	Gate->ModuleName = Script->Info.Name;
	Gate->PlayerId = GridUserId;
	Gate->Keys = GridKeys;
	Script->Gate = Gate;

	FCrowdyModScriptRunHost Host;
	const TWeakObjectPtr<UCrowdyModScripts> WeakThis(this);
	const TWeakPtr<FScript> Weak = Script;
	Host.OnHostCallWorker = [Gate](TArray<uint8>& Request)
	{
		return CrowdyModScriptsHostCalls::Prepare(*Gate, Request, FPlatformTime::Seconds());
	};
	Host.OnHostAnswerWorker = [Gate](TArray<uint8>& Answer)
	{
		CrowdyModScriptsHostCalls::Finish(*Gate, Answer);
	};
	Host.OnHostCall = [WeakThis, Weak, Generation](TConstArrayView<uint8> Call)
	{
		UCrowdyModScripts* Self = WeakThis.Get();
		const TSharedPtr<FScript> Pinned = Weak.Pin();
		return Self && Pinned ? Self->AnswerHostCall(*Pinned, Generation, Call) : CrowdyModScriptsHostCalls::Denied(TEXT("the script stopped"));
	};
	Host.OnStopped = [Weak, Generation](ECrowdyModScriptRunEnd End, const FString& Message)
	{
		const TSharedPtr<FScript> Pinned = Weak.Pin();
		if (!Pinned || !Pinned->bRunning || Pinned->Info.Generation != Generation)
		{
			return;
		}
		Pinned->bEnded = true;
		Pinned->EndReason = End == ECrowdyModScriptRunEnd::CircuitOpen ? ECrowdyModScriptStop::CircuitOpen : ECrowdyModScriptStop::Failed;
	};
	FString Error;
	const TSharedPtr<FCrowdyModScriptRun> Run = FCrowdyModScriptRun::Start(Module, MoveTemp(Host), FuelPerDispatch, TickIntervalMs, Script->Row.Name, Error);
	if (!Run.IsValid())
	{
		Script->bRunning = false;
		Fail(Script, LongHoldSeconds);
		return;
	}
	Script->Run = Run;
	// A run that ended inside Start is reported on the next frame, never as started.
	if (!Script->bEnded)
	{
		OnModScriptStarted.Broadcast(Script->Info);
	}
}
#endif

TArray<uint8> UCrowdyModScripts::AnswerHostCall(FScript& Script, int32 Generation, TConstArrayView<uint8> Call)
{
	++Script.HostCalls;
	if (!Script.bRunning || Script.bEnded || Script.Info.Generation != Generation || !Script.Gate.IsValid())
	{
		return CrowdyModScriptsHostCalls::Denied(TEXT("the script stopped"));
	}
	// The worker already checked the call and left its name here, with what it read on the gate.
	using CrowdyModScriptsHostCalls::ECall;
	const FString Name = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Call.GetData()), Call.Num());
	UObject* Host = HostObject.Get();
	const bool bHost = Host && Host->GetClass()->ImplementsInterface(UCrowdyModScriptHost::StaticClass());
	CrowdyModScriptsHostCalls::FGate& Gate = *Script.Gate;
	CrowdyModScriptsHostCalls::FPrepared Prepared;
	{
		FScopeLock Guard(&Gate.Lock);
		Prepared = MoveTemp(Gate.Prepared);
		Gate.Prepared = CrowdyModScriptsHostCalls::FPrepared();
	}
	if (Prepared.Call == ECall::Hud || Prepared.Call == ECall::Overlay)
	{
		if (bHost && Prepared.Call == ECall::Hud)
		{
			ICrowdyModScriptHost::Execute_ShowHud(Host, Script.Info, Prepared.HudText);
		}
		if (bHost && Prepared.Call == ECall::Overlay)
		{
			ICrowdyModScriptHost::Execute_DrawOverlay(Host, Script.Info, Prepared.Shapes);
		}
		return CrowdyModScriptsHostCalls::Delivered(bHost);
	}
	if (Prepared.Call == ECall::Event)
	{
		const int32 Delivered = SendEvent(Script, Prepared.EventPrefix, Prepared.EventTarget, Prepared.bEventTargeted);
		return ScriptsUtf8(FString::Printf(TEXT("{\"ok\":true,\"data\":{\"delivered\":%d}}"), Delivered));
	}
	if (Prepared.Call == ECall::Spatial)
	{
		return AnswerSend(Prepared.Send, TEXT("emit_spatial"));
	}
	const bool bChannelsHeld = !bGridChannelsReading && Now() < GridChannelsRetryAt;
	if (Prepared.Call == ECall::Channel && (bGridChannelsRead || bChannelsHeld))
	{
		return GridChannels.Contains(Prepared.Send.ChannelId) ? AnswerSend(Prepared.Send, TEXT("emit_channel")) : ChannelOutsideGrid();
	}
	// The rest wait on the platform: once deferred, what is returned here is not read.
	if (Prepared.Call == ECall::Channel)
	{
		return AnswerChannelLater(Script, MoveTemp(Prepared.Send));
	}
	if (Prepared.Call == ECall::UserStateGet || Prepared.Call == ECall::UserStateSet)
	{
		FPlatformCall State;
		State.Call = Prepared.Call == ECall::UserStateGet ? ECrowdyModScriptsCall::UserStateGet : ECrowdyModScriptsCall::UserStateSet;
		State.State = MoveTemp(Prepared.State);
		State.bClearState = Prepared.bClearState;
		return AnswerState(Script, State);
	}
	if (!bHost || Prepared.Call == ECall::None)
	{
		return CrowdyModScriptsHostCalls::Denied(FString::Printf(TEXT("host call '%s' is not offered by this game"), *Name.Left(MaxCallNameLength)));
	}
	CrowdyModScriptsHostCalls::FWorldAnswer Answer;
	Answer.Call = Prepared.Call;
	switch (Prepared.Call)
	{
	case ECall::Clicks:
		Answer.Clicks = HeldClicks;
		Answer.Clicks.Clicks = MoveTemp(Script.Clicks);
		Script.Clicks.Reset();
		Answer.NowMs = (FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTicks() / ETimespan::TicksPerMillisecond;
		break;
	case ECall::ChunkGet:
	case ECall::VoxelsList:
		Answer.Chunk = ReadChunkOnce(*Host, Prepared.Chunk);
		break;
	case ECall::Actors:
		ICrowdyModScriptHost::Execute_ListPlayers(Host, Prepared.Area, Answer.Players);
		Answer.Players.SetNum(FMath::Min(Answer.Players.Num(), MaxPlayers));
		for (FCrowdyModScriptPlayer& Player : Answer.Players)
		{
			// Never between the two halves of a surrogate pair.
			const bool bSplit = Player.Name.Len() > MaxPlayerNameChars && StringConv::IsHighSurrogate(static_cast<uint32>(Player.Name[MaxPlayerNameChars - 1]));
			Player.Name.LeftInline(bSplit ? MaxPlayerNameChars - 1 : MaxPlayerNameChars);
		}
		break;
	case ECall::VoxelSet:
		Answer.bVoxelOk = AnswerVoxelSet(*Host, Script, Prepared.Write, Answer.VoxelError);
		break;
	default:
		return CrowdyModScriptsHostCalls::Denied(TEXT("host call has no answer in this build"));
	}
	{
		FScopeLock Guard(&Gate.Lock);
		Gate.Answer = MoveTemp(Answer);
	}
	return {CrowdyModScriptsHostCalls::WorldAnswerMarker};
}

int32 UCrowdyModScripts::SendEvent(const FScript& Sender, const TArray<uint8>& EventPrefix, const FString& Target, bool bTargeted)
{
#if WITH_CROWDY_MOD_SCRIPT_RUNTIME
	// The sender is inside the dispatch that emits, so its run names that dispatch's depth.
	const int32 Depth = (Sender.Run.IsValid() ? Sender.Run->GetEventDepth() : 0) + 1;
	const FTCHARToUTF8 Tail(*FString::Printf(TEXT(",\"cascadeDepth\":%d}"), Depth));
	if (Depth > CrowdyModScriptsHostCalls::MaxEventCascade || EventPrefix.Num() + Tail.Length() > CrowdyModScriptsHostCalls::MaxArgsBytes)
	{
		return 0;
	}
	// One copy, shared by every run it is queued on.
	TArray<uint8> Bytes;
	Bytes.Reserve(EventPrefix.Num() + Tail.Length());
	Bytes.Append(EventPrefix);
	Bytes.Append(reinterpret_cast<const uint8*>(Tail.Get()), Tail.Length());
	const TSharedRef<const TArray<uint8>, ESPMode::ThreadSafe> Event = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>(MoveTemp(Bytes));
	// Every running script of this grid but the sender, or the one it names, which may be itself.
	int32 Delivered = 0;
	for (const TPair<FString, TSharedRef<FScript>>& Pair : Scripts)
	{
		const FScript& To = *Pair.Value;
		const bool bAddressed = bTargeted ? To.Info.Name.Equals(Target, ESearchCase::CaseSensitive) : &To != &Sender;
		if (bAddressed && To.bRunning && !To.bEnded && To.Run.IsValid())
		{
			Delivered += To.Run->QueueEvent(Event, Depth) ? 1 : 0;
		}
	}
	return Delivered;
#else
	return 0;
#endif
}

TArray<uint8> UCrowdyModScripts::AnswerState(FScript& Script, const FPlatformCall& Call)
{
	const bool bGet = Call.Call == ECrowdyModScriptsCall::UserStateGet;
	const TCHAR* Name = bGet ? TEXT("user_state_get") : TEXT("user_state_set");
#if WITH_CROWDY_MOD_SCRIPT_RUNTIME
	FCrowdyModScriptAnswer Answer = Script.Run.IsValid() ? Script.Run->DeferHostCall() : FCrowdyModScriptAnswer();
	if (!Answer.IsWaiting())
	{
		return CrowdyModScriptsHostCalls::Denied(FString::Printf(TEXT("host call '%s' cannot wait for the platform here"), Name));
	}
	const TWeakPtr<CrowdyModScriptsHostCalls::FGate> WeakGate = Script.Gate;
	const CrowdyModScriptsHostCalls::ECall Kind = bGet ? CrowdyModScriptsHostCalls::ECall::UserStateGet : CrowdyModScriptsHostCalls::ECall::UserStateSet;
	Send(Call, [Answer, WeakGate, Kind, Name](const FPlatformReply& Reply) mutable
	{
		const TSharedPtr<CrowdyModScriptsHostCalls::FGate> Gate = WeakGate.Pin();
		// Only the call still waiting may use the gate's answer: a later call of the same run owns it otherwise.
		if (!Gate.IsValid() || !Answer.IsWaiting())
		{
			return;
		}
		if (!Reply.bOk)
		{
			const FString& Why = !Reply.Message.IsEmpty() ? Reply.Message : Reply.Code;
			Answer.Send(CrowdyModScriptsHostCalls::Denied(FString::Printf(TEXT("host call '%s' failed: %s"), Name, *Why.Left(MaxPlatformErrorChars))));
			return;
		}
		{
			FScopeLock Guard(&Gate->Lock);
			Gate->Answer = CrowdyModScriptsHostCalls::FWorldAnswer();
			Gate->Answer.Call = Kind;
			Gate->Answer.Packed = Reply.Packed;
		}
		Answer.Send({CrowdyModScriptsHostCalls::WorldAnswerMarker});
	});
	return TArray<uint8>();
#else
	return CrowdyModScriptsHostCalls::Denied(FString::Printf(TEXT("host call '%s' is not offered by this game"), Name));
#endif
}

TArray<uint8> UCrowdyModScripts::AnswerSend(const FRealtimeSend& Message, const TCHAR* Name)
{
	FString Error;
	const bool bSent = Realtime ? Realtime(Message, Error) : SendRealtime(Message, Error);
	return bSent ? SdkAnswer(MakeShared<FJsonValueBoolean>(true))
		: CrowdyModScriptsHostCalls::Denied(FString::Printf(TEXT("host call '%s' was not sent: %s"), Name, *Error));
}

TArray<uint8> UCrowdyModScripts::AnswerChannelLater(FScript& Script, FRealtimeSend&& Message)
{
#if WITH_CROWDY_MOD_SCRIPT_RUNTIME
	FCrowdyModScriptAnswer Answer = Script.Run.IsValid() ? Script.Run->DeferHostCall() : FCrowdyModScriptAnswer();
	if (!Answer.IsWaiting())
	{
		return CrowdyModScriptsHostCalls::Denied(TEXT("host call 'emit_channel' cannot wait for the platform here"));
	}
	ChannelWaiting.Add([this, Answer, Waiting = MoveTemp(Message)]() mutable
	{
		if (Answer.IsWaiting())
		{
			Answer.Send(GridChannels.Contains(Waiting.ChannelId) ? AnswerSend(Waiting, TEXT("emit_channel")) : ChannelOutsideGrid());
		}
	});
	if (bGridChannelsReading)
	{
		return TArray<uint8>();
	}
	bGridChannelsReading = true;
	FPlatformCall Call;
	Call.Call = ECrowdyModScriptsCall::GridChannels;
	Call.GridId = Grid.GridId;
	const int32 Serial = GridSerial;
	Send(Call, [this, Serial](const FPlatformReply& Reply) { ReadGridChannels(Serial, Reply); });
	return TArray<uint8>();
#else
	return CrowdyModScriptsHostCalls::Denied(TEXT("host call 'emit_channel' is not offered by this game"));
#endif
}

void UCrowdyModScripts::ReadGridChannels(int32 Serial, const FPlatformReply& Reply)
{
	if (Serial != GridSerial)
	{
		return;
	}
	// A failed read refuses the sends waiting on it, as the browser's; unlike the browser's, it is read again only after a short hold.
	bGridChannelsReading = false;
	bGridChannelsRead = Reply.bOk;
	GridChannelsRetryAt = Reply.bOk ? 0.0 : Now() + LookupRetrySeconds;
	GridChannels.Reset();
	if (Reply.bOk)
	{
		GridChannels.Append(Reply.ChannelIds);
	}
	TArray<TFunction<void()>> Waiting = MoveTemp(ChannelWaiting);
	ChannelWaiting.Reset();
	for (TFunction<void()>& Next : Waiting)
	{
		Next();
	}
}

bool UCrowdyModScripts::SendRealtime(const FRealtimeSend& Message, FString& OutError)
{
	const UWorld* World = GetWorld();
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	const UCrowdyCppReplicationSubsystem* Replication = GameInstance ? GameInstance->GetSubsystem<UCrowdyCppReplicationSubsystem>() : nullptr;
	const TSharedPtr<FCrowdyCppReplication> Connection = Replication && Replication->IsRouting() ? Replication->GetConnection() : nullptr;
	if (!Connection.IsValid())
	{
		OutError = TEXT("the realtime connection is not open");
		return false;
	}
	if (Message.bChannel)
	{
		return Connection->SendChannelMessage(Message.ChannelId, Message.Sender, Message.Payload, OutError);
	}
	return Connection->SendSpatial(static_cast<uint8>(ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION), Message.Chunk.X, Message.Chunk.Y, Message.Chunk.Z,
		Message.Sender, Message.Payload, Message.Distance, 0, OutError);
}

TSharedPtr<const FCrowdyModScriptChunk, ESPMode::ThreadSafe> UCrowdyModScripts::ReadChunkOnce(UObject& Host, const FInt64Vector& Chunk)
{
	if (const TSharedPtr<const FCrowdyModScriptChunk, ESPMode::ThreadSafe>* Read = FrameReads.Find(Chunk))
	{
		return *Read;
	}
	const TSharedRef<FCrowdyModScriptChunk, ESPMode::ThreadSafe> Fresh = MakeShared<FCrowdyModScriptChunk, ESPMode::ThreadSafe>();
	const bool bRead = ICrowdyModScriptHost::Execute_ReadChunk(&Host, Chunk.X, Chunk.Y, Chunk.Z, *Fresh);
	const bool bLoaded = bRead && Fresh->Types.Num() == ChunkCells;
	UE_CLOG(bRead && !bLoaded && !bWarnedChunkSize, LogCrowdyServices, Warning,
		TEXT("[%s] The Mod Script host's ReadChunk gave %d voxel types, not 4096; the chunk is answered as not loaded."), ScriptsLogName, Fresh->Types.Num());
	bWarnedChunkSize |= bRead && !bLoaded;
	const TSharedPtr<const FCrowdyModScriptChunk, ESPMode::ThreadSafe> Read = bLoaded ? TSharedPtr<const FCrowdyModScriptChunk, ESPMode::ThreadSafe>(Fresh) : nullptr;
	FrameReads.Add(Chunk, Read);
	return Read;
}

bool UCrowdyModScripts::AnswerVoxelSet(UObject& Host, const FScript& Script, const FCrowdyVoxelWrite& Write, FString& OutError)
{
	// The voxel world's queue is shared with the game's own edits, so scripts back off before it grows.
	if (UnsentEdits() >= MaxUnsentEdits || !CrowdyModScriptsHostCalls::AdmitWindow(VoxelSetTimes, VoxelSetOldest, Now(), VoxelSetsPerSecond))
	{
		OutError = TEXT("busy");
		return false;
	}
	const bool bOk = ICrowdyModScriptHost::Execute_SetVoxel(&Host, Script.Info, Write, OutError);
	OutError = bOk ? FString() : OutError.IsEmpty() ? FString(TEXT("refused")) : OutError;
	return bOk;
}

int32 UCrowdyModScripts::UnsentEdits() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (UnsentEditsForTest >= 0)
	{
		return UnsentEditsForTest;
	}
#endif
	const UWorld* World = GetWorld();
	const UCrowdyVoxelWorld* Voxels = World ? World->GetSubsystem<UCrowdyVoxelWorld>() : nullptr;
	return Voxels ? Voxels->GetUnsentEditCount() : 0;
}

void UCrowdyModScripts::DrainClicks()
{
	UObject* Host = HostObject.Get();
	if (!Host || !Host->GetClass()->ImplementsInterface(UCrowdyModScriptHost::StaticClass()))
	{
		return;
	}
	TArray<TSharedRef<FScript>, TInlineAllocator<8>> Listening;
	for (const TPair<FString, TSharedRef<FScript>>& Pair : Scripts)
	{
		const bool bAgreed = Pair.Value->Info.Calls.ContainsByPredicate([](const FString& Call) { return Call.Equals(TEXT("pointer_clicks"), ESearchCase::CaseSensitive); });
		if (Pair.Value->bRunning && bAgreed)
		{
			Listening.Add(Pair.Value);
		}
	}
	// Drained every frame, so the host never holds clicks for a script that starts listening later.
	FCrowdyModScriptClicks Drained;
	ICrowdyModScriptHost::Execute_DrainClicks(Host, Drained);
	HeldClicks = Drained;
	HeldClicks.Clicks.Reset();
	// Each script gets every click since its own last call, the oldest dropped past 64.
	for (const TSharedRef<FScript>& Script : Listening)
	{
		if (!Script->bRunning)
		{
			continue;
		}
		Script->Clicks.Append(Drained.Clicks);
		Script->Clicks.RemoveAt(0, FMath::Max(0, Script->Clicks.Num() - MaxQueuedClicks));
	}
}

void UCrowdyModScripts::Fail(const TSharedRef<FScript>& Script, double HoldSeconds)
{
	Script->HoldUntil = Now() + HoldSeconds;
	FCrowdyModScript Failed = Script->Info;
	Failed.ModId = Script->Row.ModId;
	Failed.Name = Script->Row.Name;
	Failed.AuthorId = Script->Row.AuthorId;
	Failed.GridId = Grid.GridId;
	Failed.Version = Script->Row.Version;
	Failed.Calls = Script->Row.Calls;
	OnModScriptStopped.Broadcast(Failed, ECrowdyModScriptStop::Failed);
}

void UCrowdyModScripts::StopRun(const TSharedRef<FScript>& Script, ECrowdyModScriptStop Reason)
{
	const TSharedPtr<FCrowdyModScriptRun> Run = MoveTemp(Script->Run);
	Script->Run.Reset();
	const bool bWasRunning = Script->bRunning;
	Script->bRunning = false;
	Script->bEnded = false;
	Script->Clicks.Reset();
#if WITH_CROWDY_MOD_SCRIPT_RUNTIME
	if (Run.IsValid())
	{
		Run->Stop();
	}
#endif
	if (!bWasRunning)
	{
		return;
	}
	UObject* Host = HostObject.Get();
	if (Host && Host->GetClass()->ImplementsInterface(UCrowdyModScriptHost::StaticClass()))
	{
		ICrowdyModScriptHost::Execute_ClearScript(Host, Script->Info);
	}
	OnModScriptStopped.Broadcast(Script->Info, Reason);
}

TArray<FCrowdyModScript> UCrowdyModScripts::GetRunning() const
{
	TArray<FCrowdyModScript> Running;
	for (const TPair<FString, TSharedRef<FScript>>& Pair : Scripts)
	{
		if (Pair.Value->bRunning)
		{
			Running.Add(Pair.Value->Info);
		}
	}
	return Running;
}

void UCrowdyModScripts::Revoke(const FString& ModId)
{
	if (ModId.IsEmpty())
	{
		return;
	}
	FPlatformCall Call;
	Call.Call = ECrowdyModScriptsCall::RevokeConsent;
	Call.ModId = ModId;
	// A revoke works from anywhere, so its refusal is told even after the player left the grid.
	SendAgreement(Call, [this, ModId](const FPlatformReply& Reply, bool bSameGrid)
	{
		if (!Reply.bOk)
		{
			RefuseScript(ModId, Reply, bSameGrid);
		}
	});

	const TSharedRef<FScript>* Found = Scripts.Find(ModId);
	if (!Found)
	{
		return;
	}
	const TSharedRef<FScript> Script = *Found;
	const int64 AuthorId = Script->Row.AuthorId;
	const bool bByTrust = Script->Row.bTrustsAuthor;
	Script->bTakenBack = true;
	Script->bFetching = false;
	++Script->Attempt;
	Script->Row.bConsented = false;
	if (bByTrust)
	{
		// The trust goes too, and the author's other running scripts are consented to one by one so they keep running.
		const TSharedRef<FAuthor> Author = FindOrAddAuthor(AuthorId);
		Author->DeclinedHash = Script->Row.AuthorHash;
		for (const TSharedRef<FScript>& Theirs : ScriptsOf(AuthorId))
		{
			Theirs->Row.bTrustsAuthor = false;
		}
		FPlatformCall Untrust;
		Untrust.Call = ECrowdyModScriptsCall::RevokeTrust;
		Untrust.GridId = Grid.GridId;
		Untrust.AuthorId = AuthorId;
		SendAgreement(Untrust, [this, ModId, AuthorId](const FPlatformReply& Reply, bool bSameGrid)
		{
			if (!Reply.bOk)
			{
				RefuseScript(ModId, Reply, bSameGrid);
				return;
			}
			if (!bSameGrid)
			{
				return;
			}
			for (const TSharedRef<FScript>& Other : ScriptsOf(AuthorId))
			{
				if (!Other->bRunning)
				{
					continue;
				}
				FPlatformCall Consent;
				Consent.Call = ECrowdyModScriptsCall::ConsentMod;
				Consent.ModId = Other->Row.ModId;
				Consent.Hash = Other->Row.Hash;
				const TWeakPtr<FScript> Weak = Other;
				SendAgreement(Consent, [this, Weak, OtherId = Other->Row.ModId](const FPlatformReply& ConsentReply, bool bConsentSameGrid)
				{
					const TSharedPtr<FScript> Pinned = Weak.Pin();
					if (!bConsentSameGrid || !Pinned)
					{
						return;
					}
					if (ConsentReply.bOk)
					{
						Pinned->Row.bConsented = true;
						return;
					}
					// Its hash changed since it was listed: the next listing stops it as Changed and asks again.
					if (ScriptsNamesCode(ConsentReply, TEXT("CONFLICT")))
					{
						ListAgain();
						return;
					}
					RefuseScript(OtherId, ConsentReply, true);
				});
			}
		});
	}
	StopRun(Script, ECrowdyModScriptStop::Revoked);
}

void UCrowdyModScripts::ForgetAuthor(int64 AuthorId)
{
	if (Grid.GridId == 0 || AuthorId <= 0)
	{
		return;
	}
	const TArray<TSharedRef<FScript>> Theirs = ScriptsOf(AuthorId);
	const TSharedRef<FAuthor> Author = FindOrAddAuthor(AuthorId);
	for (const TSharedRef<FScript>& Script : Theirs)
	{
		Script->bTakenBack = true;
		Script->bFetching = false;
		++Script->Attempt;
		Script->Row.bConsented = false;
		Script->Row.bTrustsAuthor = false;
		Author->DeclinedHash = Script->Row.AuthorHash;
	}
	TArray<FString> ModIds;
	for (const TSharedRef<FScript>& Script : Theirs)
	{
		ModIds.Add(Script->Row.ModId);
	}
	FPlatformCall Call;
	Call.Call = ECrowdyModScriptsCall::RevokeTrust;
	Call.GridId = Grid.GridId;
	Call.AuthorId = AuthorId;
	SendAgreement(Call, [this, ModIds](const FPlatformReply& Reply, bool bSameGrid)
	{
		if (Reply.bOk)
		{
			return;
		}
		for (const FString& ModId : ModIds)
		{
			RefuseScript(ModId, Reply, bSameGrid);
		}
	});

	const int32 Serial = GridSerial;
	if (Author->PromptId != 0)
	{
		Withdraw(*Author);
	}
	for (const TSharedRef<FScript>& Script : Theirs)
	{
		if (Serial != GridSerial)
		{
			return;
		}
		StopRun(Script, ECrowdyModScriptStop::Revoked);
	}
}

void UCrowdyModScripts::RefuseScript(const FString& ModId, const FPlatformReply& Reply, bool bSameGrid)
{
	const TSharedRef<FScript>* Found = bSameGrid ? Scripts.Find(ModId) : nullptr;
	if (Found)
	{
		const TSharedRef<FScript> Script = *Found;
		Script->bRefused = true;
		Script->bFetching = false;
		++Script->Attempt;
		StopRun(Script, ECrowdyModScriptStop::Unconsented);
	}
	FCrowdyModScriptError Error;
	Error.ModId = ModId;
	Error.Code = Reply.Code;
	Error.Message = Reply.Message;
	OnModScriptError.Broadcast(Error);
}

void UCrowdyModScripts::RefuseAuthor(int64 AuthorId, const FPlatformReply& Reply)
{
	// Scripts already running under their own consent keep running.
	TArray<FString> Refused;
	for (const TSharedRef<FScript>& Script : ScriptsOf(AuthorId))
	{
		if (Script->bRunning)
		{
			continue;
		}
		Script->bRefused = true;
		Script->bFetching = false;
		++Script->Attempt;
		Refused.Add(Script->Row.ModId);
	}
	for (const FString& ModId : Refused)
	{
		FCrowdyModScriptError Error;
		Error.ModId = ModId;
		Error.Code = Reply.Code;
		Error.Message = Reply.Message;
		OnModScriptError.Broadcast(Error);
	}
}
