#include "Network/GraphQL/FCrowdyGameApiCodec.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyGameModelPhase2BTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Uniquely named (never "ParseObject") so a unity build cannot collide it with a sibling test file's helper.
	TSharedPtr<FJsonObject> ParsePhase2BJson(const FString& Json)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		TSharedPtr<FJsonObject> Obj;
		FJsonSerializer::Deserialize(Reader, Obj);
		return Obj;
	}

	// The "input" object of a mutation's variables, or null when the wrapper is missing.
	TSharedPtr<FJsonObject> Phase2BInput(const TSharedPtr<FJsonObject>& Vars)
	{
		const TSharedPtr<FJsonObject>* InputPtr = nullptr;
		if (!Vars.IsValid() || !Vars->TryGetObjectField(TEXT("input"), InputPtr) || !InputPtr)
		{
			return nullptr;
		}
		return *InputPtr;
	}

	// The JSON type of a field as an int (EJson::None when absent), so a test asserts the wire TYPE and not just
	// presence: TryGetStringField also accepts a number and TryGetNumberField also accepts a numeric string.
	int32 Phase2BFieldType(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		const TSharedPtr<FJsonValue> Value = Obj.IsValid() ? Obj->TryGetField(Field) : nullptr;
		return static_cast<int32>(Value.IsValid() ? Value->Type : EJson::None);
	}

	constexpr int32 Phase2BString = static_cast<int32>(EJson::String);
	constexpr int32 Phase2BNumber = static_cast<int32>(EJson::Number);
	constexpr int32 Phase2BNull = static_cast<int32>(EJson::Null);
	constexpr int32 Phase2BAbsent = static_cast<int32>(EJson::None);
}

// The session request builders encode every BigInt id (appId, each participant, the turn holder) as a JSON
// string and omit nullable inputs (name, metadata, participants, a cleared turn) entirely when empty.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelSessionBuildsRequestTest,
	"CrowdySDK.GameModel.SessionBuildsRequest", CrowdyGameModelPhase2BTestFlags)
bool FCrowdyGameModelSessionBuildsRequestTest::RunTest(const FString& Parameters)
{
	// CreateSession: appId string, participantUserIds an array of BigInt STRINGS, name present, metadata omitted.
	{
		TArray<int64> Participants;
		Participants.Add(90001);
		Participants.Add(90002);
		const TSharedPtr<FJsonObject> Vars =
			FCrowdyGameApiCodec::BuildCreateSessionVariables(1, TEXT("Skirmish"), Participants, FString());
		const TSharedPtr<FJsonObject>* InputPtr = nullptr;
		if (!TestTrue(TEXT("input present"), Vars.IsValid() && Vars->TryGetObjectField(TEXT("input"), InputPtr) && InputPtr))
		{
			return false;
		}
		const TSharedPtr<FJsonObject>& Input = *InputPtr;

		const TSharedPtr<FJsonValue> AppIdValue = Input->TryGetField(TEXT("appId"));
		if (TestNotNull(TEXT("appId present"), AppIdValue.Get()))
		{
			TestEqual(TEXT("appId is a JSON string"), static_cast<int32>(AppIdValue->Type), static_cast<int32>(EJson::String));
			TestEqual(TEXT("appId string value"), AppIdValue->AsString(), FString(TEXT("1")));
		}

		FString Name;
		TestTrue(TEXT("name present when set"), Input->TryGetStringField(TEXT("name"), Name));
		TestEqual(TEXT("name round-trips"), Name, FString(TEXT("Skirmish")));
		TestFalse(TEXT("metadataJson omitted when empty"), Input->HasField(TEXT("metadataJson")));

		const TArray<TSharedPtr<FJsonValue>>* Ids = nullptr;
		if (TestTrue(TEXT("participantUserIds present"), Input->TryGetArrayField(TEXT("participantUserIds"), Ids) && Ids))
		{
			if (TestEqual(TEXT("two participants"), Ids->Num(), 2))
			{
				// The load-bearing detail: each participant id is a JSON STRING, not a number.
				TestEqual(TEXT("participant[0] is a string"), static_cast<int32>((*Ids)[0]->Type), static_cast<int32>(EJson::String));
				TestEqual(TEXT("participant[0] value"), (*Ids)[0]->AsString(), FString(TEXT("90001")));
				TestEqual(TEXT("participant[1] is a string"), static_cast<int32>((*Ids)[1]->Type), static_cast<int32>(EJson::String));
				TestEqual(TEXT("participant[1] value"), (*Ids)[1]->AsString(), FString(TEXT("90002")));
			}
		}
	}

	// Empty name + empty participants: both nullable inputs vanish (not written as "" or []).
	{
		const TSharedPtr<FJsonObject> Vars =
			FCrowdyGameApiCodec::BuildCreateSessionVariables(1, FString(), TArray<int64>(), FString());
		const TSharedPtr<FJsonObject>* InputPtr = nullptr;
		Vars->TryGetObjectField(TEXT("input"), InputPtr);
		if (TestTrue(TEXT("input present"), InputPtr != nullptr))
		{
			TestFalse(TEXT("name omitted when empty"), (*InputPtr)->HasField(TEXT("name")));
			TestFalse(TEXT("participantUserIds omitted when empty"), (*InputPtr)->HasField(TEXT("participantUserIds")));
		}
	}

	// Default options: every optional create input vanishes, so the server applies its own defaults.
	{
		const TSharedPtr<FJsonObject> Input = Phase2BInput(
			FCrowdyGameApiCodec::BuildCreateSessionVariables(1, FString(), TArray<int64>(), FString()));
		if (TestTrue(TEXT("input present (default options)"), Input.IsValid()))
		{
			TestEqual(TEXT("maxParticipants omitted at 0"), Phase2BFieldType(Input, TEXT("maxParticipants")), Phase2BAbsent);
			TestEqual(TEXT("admission omitted when empty"), Phase2BFieldType(Input, TEXT("admission")), Phase2BAbsent);
			TestEqual(TEXT("emptyTimeoutSec omitted at -1"), Phase2BFieldType(Input, TEXT("emptyTimeoutSec")), Phase2BAbsent);
			TestEqual(TEXT("presence omitted when empty"), Phase2BFieldType(Input, TEXT("presence")), Phase2BAbsent);
			TestEqual(TEXT("idempotencyKey omitted when empty"), Phase2BFieldType(Input, TEXT("idempotencyKey")), Phase2BAbsent);
		}
	}

	// Set options: the Int inputs are JSON NUMBERS (never strings), the enums and key are strings.
	{
		FCrowdyCreateSessionOptions Options;
		Options.MaxParticipants = 4;
		Options.Admission = TEXT("locked");
		Options.EmptyTimeoutSec = 30;
		Options.Presence = TEXT("none");
		Options.IdempotencyKey = TEXT("k-1");
		const TSharedPtr<FJsonObject> Input = Phase2BInput(
			FCrowdyGameApiCodec::BuildCreateSessionVariables(1, FString(), TArray<int64>(), FString(), Options));
		if (TestTrue(TEXT("input present (options set)"), Input.IsValid()))
		{
			TestEqual(TEXT("maxParticipants is a JSON number"), Phase2BFieldType(Input, TEXT("maxParticipants")), Phase2BNumber);
			TestEqual(TEXT("maxParticipants value"), static_cast<int32>(Input->GetNumberField(TEXT("maxParticipants"))), 4);
			TestEqual(TEXT("admission is a JSON string"), Phase2BFieldType(Input, TEXT("admission")), Phase2BString);
			TestEqual(TEXT("admission value"), Input->GetStringField(TEXT("admission")), FString(TEXT("locked")));
			TestEqual(TEXT("emptyTimeoutSec is a JSON number"), Phase2BFieldType(Input, TEXT("emptyTimeoutSec")), Phase2BNumber);
			TestEqual(TEXT("emptyTimeoutSec value"), static_cast<int32>(Input->GetNumberField(TEXT("emptyTimeoutSec"))), 30);
			TestEqual(TEXT("presence is a JSON string"), Phase2BFieldType(Input, TEXT("presence")), Phase2BString);
			TestEqual(TEXT("presence value"), Input->GetStringField(TEXT("presence")), FString(TEXT("none")));
			TestEqual(TEXT("idempotencyKey is a JSON string"), Phase2BFieldType(Input, TEXT("idempotencyKey")), Phase2BString);
			TestEqual(TEXT("idempotencyKey value"), Input->GetStringField(TEXT("idempotencyKey")), FString(TEXT("k-1")));
		}
	}

	// emptyTimeoutSec 0 is a real instruction (disable the timeout) and must be WRITTEN, unlike -1.
	{
		FCrowdyCreateSessionOptions Options;
		Options.EmptyTimeoutSec = 0;
		const TSharedPtr<FJsonObject> Input = Phase2BInput(
			FCrowdyGameApiCodec::BuildCreateSessionVariables(1, FString(), TArray<int64>(), FString(), Options));
		if (TestTrue(TEXT("input present (timeout 0)"), Input.IsValid()))
		{
			TestEqual(TEXT("emptyTimeoutSec 0 is written as a number"), Phase2BFieldType(Input, TEXT("emptyTimeoutSec")), Phase2BNumber);
			TestEqual(TEXT("emptyTimeoutSec 0 value"), static_cast<int32>(Input->GetNumberField(TEXT("emptyTimeoutSec"))), 0);
		}
	}

	// SetSessionTurn: userId written as a BigInt string when present; expectedHostTerm omitted at 0...
	{
		const TSharedPtr<FJsonObject> Input = Phase2BInput(
			FCrowdyGameApiCodec::BuildSetSessionTurnVariables(1, TEXT("s"), 90002, true));
		if (TestTrue(TEXT("input present"), Input.IsValid()))
		{
			TestEqual(TEXT("userId is a JSON string"), Phase2BFieldType(Input, TEXT("userId")), Phase2BString);
			TestEqual(TEXT("userId string value"), Input->GetStringField(TEXT("userId")), FString(TEXT("90002")));
			TestEqual(TEXT("expectedHostTerm omitted at 0"), Phase2BFieldType(Input, TEXT("expectedHostTerm")), Phase2BAbsent);
		}
	}

	// ...and written as a JSON NUMBER when the caller asserts a host term.
	{
		const TSharedPtr<FJsonObject> Input = Phase2BInput(
			FCrowdyGameApiCodec::BuildSetSessionTurnVariables(1, TEXT("s"), 90002, true, 3));
		if (TestTrue(TEXT("input present (host term)"), Input.IsValid()))
		{
			TestEqual(TEXT("expectedHostTerm is a JSON number"), Phase2BFieldType(Input, TEXT("expectedHostTerm")), Phase2BNumber);
			TestEqual(TEXT("expectedHostTerm value"), static_cast<int32>(Input->GetNumberField(TEXT("expectedHostTerm"))), 3);
		}
	}

	// Clearing the turn (bHasUserId=false) still writes an EXPLICIT JSON null. The schema clears a turn on null,
	// NOT on an absent field, so the key must be present and null, while sessionId stays.
	{
		const TSharedPtr<FJsonObject> Input = Phase2BInput(
			FCrowdyGameApiCodec::BuildSetSessionTurnVariables(1, TEXT("s"), 90002, false, 3));
		if (TestTrue(TEXT("input present"), Input.IsValid()))
		{
			TestEqual(TEXT("userId is JSON null when clearing the turn"), Phase2BFieldType(Input, TEXT("userId")), Phase2BNull);
			TestEqual(TEXT("sessionId is a JSON string"), Phase2BFieldType(Input, TEXT("sessionId")), Phase2BString);
			TestEqual(TEXT("sessionId value"), Input->GetStringField(TEXT("sessionId")), FString(TEXT("s")));
			TestEqual(TEXT("expectedHostTerm written alongside a clear"), Phase2BFieldType(Input, TEXT("expectedHostTerm")), Phase2BNumber);
		}
	}

	// JoinSession: role, actorUuid and idempotencyKey omitted when empty, present as strings when set.
	{
		const TSharedPtr<FJsonObject> Input = Phase2BInput(
			FCrowdyGameApiCodec::BuildJoinSessionVariables(1, TEXT("s"), FString()));
		if (TestTrue(TEXT("input present"), Input.IsValid()))
		{
			TestEqual(TEXT("role omitted when empty"), Phase2BFieldType(Input, TEXT("role")), Phase2BAbsent);
			TestEqual(TEXT("actorUuid omitted when empty"), Phase2BFieldType(Input, TEXT("actorUuid")), Phase2BAbsent);
			TestEqual(TEXT("idempotencyKey omitted when empty"), Phase2BFieldType(Input, TEXT("idempotencyKey")), Phase2BAbsent);
		}

		const FString Uuid = TEXT("0123456789abcdef0123456789abcdef");
		const TSharedPtr<FJsonObject> Input2 = Phase2BInput(
			FCrowdyGameApiCodec::BuildJoinSessionVariables(1, TEXT("s"), TEXT("gm"), Uuid, TEXT("k-2")));
		if (TestTrue(TEXT("input present (role set)"), Input2.IsValid()))
		{
			TestEqual(TEXT("role is a JSON string"), Phase2BFieldType(Input2, TEXT("role")), Phase2BString);
			TestEqual(TEXT("role value"), Input2->GetStringField(TEXT("role")), FString(TEXT("gm")));
			TestEqual(TEXT("actorUuid is a JSON string"), Phase2BFieldType(Input2, TEXT("actorUuid")), Phase2BString);
			TestEqual(TEXT("actorUuid value"), Input2->GetStringField(TEXT("actorUuid")), Uuid);
			TestEqual(TEXT("idempotencyKey value"), Input2->GetStringField(TEXT("idempotencyKey")), FString(TEXT("k-2")));
		}
	}

	return true;
}

// The remaining session builders: leave sends incarnation as a number; admission / transfer / end are host-gated
// mutations whose expectedHostTerm is omitted at 0 and a number otherwise (toUserId a BigInt string, reason omitted
// when empty); list filters vanish when unset; snapshot / events / changed are flat queries with afterRevision a
// STRING and limit a number omitted at 0.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelSessionLifecycleBuildsRequestTest,
	"CrowdySDK.GameModel.SessionLifecycleBuildsRequest", CrowdyGameModelPhase2BTestFlags)
bool FCrowdyGameModelSessionLifecycleBuildsRequestTest::RunTest(const FString& Parameters)
{
	// LeaveSession: appId a string, incarnation a JSON NUMBER, idempotencyKey omitted when empty.
	{
		const TSharedPtr<FJsonObject> Input = Phase2BInput(FCrowdyGameApiCodec::BuildLeaveSessionVariables(1, TEXT("s"), 2));
		if (TestTrue(TEXT("leave input present"), Input.IsValid()))
		{
			TestEqual(TEXT("leave appId is a JSON string"), Phase2BFieldType(Input, TEXT("appId")), Phase2BString);
			TestEqual(TEXT("leave appId value"), Input->GetStringField(TEXT("appId")), FString(TEXT("1")));
			TestEqual(TEXT("leave sessionId value"), Input->GetStringField(TEXT("sessionId")), FString(TEXT("s")));
			TestEqual(TEXT("incarnation is a JSON number"), Phase2BFieldType(Input, TEXT("incarnation")), Phase2BNumber);
			TestEqual(TEXT("incarnation value"), static_cast<int32>(Input->GetNumberField(TEXT("incarnation"))), 2);
			TestEqual(TEXT("leave idempotencyKey omitted"), Phase2BFieldType(Input, TEXT("idempotencyKey")), Phase2BAbsent);
		}
		const TSharedPtr<FJsonObject> Input2 = Phase2BInput(
			FCrowdyGameApiCodec::BuildLeaveSessionVariables(1, TEXT("s"), 0, TEXT("k-3")));
		if (TestTrue(TEXT("leave input present (key)"), Input2.IsValid()))
		{
			// incarnation is required, so 0 is still written.
			TestEqual(TEXT("incarnation 0 still written"), Phase2BFieldType(Input2, TEXT("incarnation")), Phase2BNumber);
			TestEqual(TEXT("leave idempotencyKey value"), Input2->GetStringField(TEXT("idempotencyKey")), FString(TEXT("k-3")));
		}
	}

	// SetSessionAdmission: admission a string; expectedHostTerm omitted at 0, a JSON number otherwise.
	{
		const TSharedPtr<FJsonObject> Input = Phase2BInput(
			FCrowdyGameApiCodec::BuildSetSessionAdmissionVariables(1, TEXT("s"), TEXT("locked"), 0));
		if (TestTrue(TEXT("admission input present"), Input.IsValid()))
		{
			TestEqual(TEXT("admission appId is a JSON string"), Phase2BFieldType(Input, TEXT("appId")), Phase2BString);
			TestEqual(TEXT("admission is a JSON string"), Phase2BFieldType(Input, TEXT("admission")), Phase2BString);
			TestEqual(TEXT("admission value"), Input->GetStringField(TEXT("admission")), FString(TEXT("locked")));
			TestEqual(TEXT("admission expectedHostTerm omitted at 0"), Phase2BFieldType(Input, TEXT("expectedHostTerm")), Phase2BAbsent);
		}
		const TSharedPtr<FJsonObject> Input2 = Phase2BInput(
			FCrowdyGameApiCodec::BuildSetSessionAdmissionVariables(1, TEXT("s"), TEXT("closed"), 2));
		if (TestTrue(TEXT("admission input present (term)"), Input2.IsValid()))
		{
			TestEqual(TEXT("admission expectedHostTerm is a JSON number"), Phase2BFieldType(Input2, TEXT("expectedHostTerm")), Phase2BNumber);
			TestEqual(TEXT("admission expectedHostTerm value"), static_cast<int32>(Input2->GetNumberField(TEXT("expectedHostTerm"))), 2);
		}
	}

	// TransferSessionHost: toUserId is a BigInt STRING, never a number.
	{
		const TSharedPtr<FJsonObject> Input = Phase2BInput(
			FCrowdyGameApiCodec::BuildTransferSessionHostVariables(1, TEXT("s"), 90003, 0));
		if (TestTrue(TEXT("transfer input present"), Input.IsValid()))
		{
			TestEqual(TEXT("toUserId is a JSON string"), Phase2BFieldType(Input, TEXT("toUserId")), Phase2BString);
			TestEqual(TEXT("toUserId value"), Input->GetStringField(TEXT("toUserId")), FString(TEXT("90003")));
			TestEqual(TEXT("transfer expectedHostTerm omitted at 0"), Phase2BFieldType(Input, TEXT("expectedHostTerm")), Phase2BAbsent);
		}
		const TSharedPtr<FJsonObject> Input2 = Phase2BInput(
			FCrowdyGameApiCodec::BuildTransferSessionHostVariables(1, TEXT("s"), 90003, 5));
		if (TestTrue(TEXT("transfer input present (term)"), Input2.IsValid()))
		{
			TestEqual(TEXT("transfer expectedHostTerm is a JSON number"), Phase2BFieldType(Input2, TEXT("expectedHostTerm")), Phase2BNumber);
			TestEqual(TEXT("transfer expectedHostTerm value"), static_cast<int32>(Input2->GetNumberField(TEXT("expectedHostTerm"))), 5);
		}
	}

	// EndSession: reason omitted when empty, a string when set; expectedHostTerm as above.
	{
		const TSharedPtr<FJsonObject> Input = Phase2BInput(
			FCrowdyGameApiCodec::BuildEndSessionVariables(1, TEXT("s"), FString(), 0));
		if (TestTrue(TEXT("end input present"), Input.IsValid()))
		{
			TestEqual(TEXT("end sessionId value"), Input->GetStringField(TEXT("sessionId")), FString(TEXT("s")));
			TestEqual(TEXT("reason omitted when empty"), Phase2BFieldType(Input, TEXT("reason")), Phase2BAbsent);
			TestEqual(TEXT("end expectedHostTerm omitted at 0"), Phase2BFieldType(Input, TEXT("expectedHostTerm")), Phase2BAbsent);
		}
		const TSharedPtr<FJsonObject> Input2 = Phase2BInput(
			FCrowdyGameApiCodec::BuildEndSessionVariables(1, TEXT("s"), TEXT("abandoned"), 1));
		if (TestTrue(TEXT("end input present (reason)"), Input2.IsValid()))
		{
			TestEqual(TEXT("reason is a JSON string"), Phase2BFieldType(Input2, TEXT("reason")), Phase2BString);
			TestEqual(TEXT("reason value"), Input2->GetStringField(TEXT("reason")), FString(TEXT("abandoned")));
			TestEqual(TEXT("end expectedHostTerm is a JSON number"), Phase2BFieldType(Input2, TEXT("expectedHostTerm")), Phase2BNumber);
			TestEqual(TEXT("end expectedHostTerm value"), static_cast<int32>(Input2->GetNumberField(TEXT("expectedHostTerm"))), 1);
		}
	}

	// ListSessions is a QUERY (flat variables): every filter omitted when unset...
	{
		const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildListSessionsVariables(1, FString());
		if (TestTrue(TEXT("list vars present"), Vars.IsValid()))
		{
			TestEqual(TEXT("list has no input wrapper"), Phase2BFieldType(Vars, TEXT("input")), Phase2BAbsent);
			TestEqual(TEXT("list appId is a JSON string"), Phase2BFieldType(Vars, TEXT("appId")), Phase2BString);
			TestEqual(TEXT("status omitted when empty"), Phase2BFieldType(Vars, TEXT("status")), Phase2BAbsent);
			TestEqual(TEXT("admission filter omitted when empty"), Phase2BFieldType(Vars, TEXT("admission")), Phase2BAbsent);
			TestEqual(TEXT("hostUserId omitted at 0"), Phase2BFieldType(Vars, TEXT("hostUserId")), Phase2BAbsent);
			TestEqual(TEXT("limit omitted at 0"), Phase2BFieldType(Vars, TEXT("limit")), Phase2BAbsent);
		}
	}

	// ...and each written with its wire type when set (hostUserId a BigInt string, limit a number).
	{
		const TSharedPtr<FJsonObject> Vars =
			FCrowdyGameApiCodec::BuildListSessionsVariables(1, TEXT("active"), TEXT("open"), 90001, 25);
		if (TestTrue(TEXT("list vars present (filters)"), Vars.IsValid()))
		{
			TestEqual(TEXT("status value"), Vars->GetStringField(TEXT("status")), FString(TEXT("active")));
			TestEqual(TEXT("admission filter value"), Vars->GetStringField(TEXT("admission")), FString(TEXT("open")));
			TestEqual(TEXT("hostUserId is a JSON string"), Phase2BFieldType(Vars, TEXT("hostUserId")), Phase2BString);
			TestEqual(TEXT("hostUserId value"), Vars->GetStringField(TEXT("hostUserId")), FString(TEXT("90001")));
			TestEqual(TEXT("limit is a JSON number"), Phase2BFieldType(Vars, TEXT("limit")), Phase2BNumber);
			TestEqual(TEXT("limit value"), static_cast<int32>(Vars->GetNumberField(TEXT("limit"))), 25);
		}
	}

	// SessionSnapshot: flat { appId (string), sessionId }.
	{
		const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildSessionSnapshotVariables(1, TEXT("s"));
		if (TestTrue(TEXT("snapshot vars present"), Vars.IsValid()))
		{
			TestEqual(TEXT("snapshot has no input wrapper"), Phase2BFieldType(Vars, TEXT("input")), Phase2BAbsent);
			TestEqual(TEXT("snapshot appId is a JSON string"), Phase2BFieldType(Vars, TEXT("appId")), Phase2BString);
			TestEqual(TEXT("snapshot appId value"), Vars->GetStringField(TEXT("appId")), FString(TEXT("1")));
			TestEqual(TEXT("snapshot sessionId value"), Vars->GetStringField(TEXT("sessionId")), FString(TEXT("s")));
		}
	}

	// SessionEvents: afterRevision is a STRING holding the integer (never a number); limit omitted at 0.
	{
		const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildSessionEventsVariables(1, TEXT("s"), 0, 0);
		if (TestTrue(TEXT("events vars present"), Vars.IsValid()))
		{
			TestEqual(TEXT("afterRevision is a JSON string"), Phase2BFieldType(Vars, TEXT("afterRevision")), Phase2BString);
			TestEqual(TEXT("afterRevision value"), Vars->GetStringField(TEXT("afterRevision")), FString(TEXT("0")));
			TestEqual(TEXT("events limit omitted at 0"), Phase2BFieldType(Vars, TEXT("limit")), Phase2BAbsent);
		}
		const TSharedPtr<FJsonObject> Vars2 = FCrowdyGameApiCodec::BuildSessionEventsVariables(1, TEXT("s"), 41, 100);
		if (TestTrue(TEXT("events vars present (limit)"), Vars2.IsValid()))
		{
			TestEqual(TEXT("afterRevision 41 as a string"), Vars2->GetStringField(TEXT("afterRevision")), FString(TEXT("41")));
			TestEqual(TEXT("events limit is a JSON number"), Phase2BFieldType(Vars2, TEXT("limit")), Phase2BNumber);
			TestEqual(TEXT("events limit value"), static_cast<int32>(Vars2->GetNumberField(TEXT("limit"))), 100);
		}
	}

	// SessionChanged (subscription): afterRevision omitted when the caller has none, a string otherwise.
	{
		const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildSessionChangedVariables(1, TEXT("s"), 0, false);
		if (TestTrue(TEXT("changed vars present"), Vars.IsValid()))
		{
			TestEqual(TEXT("changed appId is a JSON string"), Phase2BFieldType(Vars, TEXT("appId")), Phase2BString);
			TestEqual(TEXT("changed afterRevision omitted"), Phase2BFieldType(Vars, TEXT("afterRevision")), Phase2BAbsent);
		}
		const TSharedPtr<FJsonObject> Vars2 = FCrowdyGameApiCodec::BuildSessionChangedVariables(1, TEXT("s"), 7, true);
		if (TestTrue(TEXT("changed vars present (revision)"), Vars2.IsValid()))
		{
			TestEqual(TEXT("changed afterRevision is a JSON string"), Phase2BFieldType(Vars2, TEXT("afterRevision")), Phase2BString);
			TestEqual(TEXT("changed afterRevision value"), Vars2->GetStringField(TEXT("afterRevision")), FString(TEXT("7")));
		}
	}

	return true;
}

// The session parsers read BigInt ids from wire strings into int64 and use bHasCurrentTurn to tell a real
// turn holder apart from a null turn, across the single-session, list, and join envelopes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelParseSessionTest,
	"CrowdySDK.GameModel.ParseSession", CrowdyGameModelPhase2BTestFlags)
bool FCrowdyGameModelParseSessionTest::RunTest(const FString& Parameters)
{
	// A single-session envelope: BigInt ids arrive as strings and parse to int64; a real currentTurnUserId
	// sets bHasCurrentTurn so a caller distinguishes "someone's turn" from user id 0.
	{
		const TSharedPtr<FJsonObject> Env = ParsePhase2BJson(TEXT(
			"{\"data\":{\"gameModelCreateSession\":{\"sessionId\":\"sess1\",\"appId\":\"1\",\"name\":\"Skirmish\","
			"\"status\":\"active\",\"createdByUserId\":\"90001\",\"currentTurnUserId\":\"90002\","
			"\"metadataJson\":\"{}\"}}}"));
		FCrowdyGameSessionData Session;
		const bool bOk = FCrowdyGameApiCodec::ParseSessionEnvelope(
			Env, true, TArray<FString>(), TEXT("gameModelCreateSession"), Session);
		TestTrue(TEXT("session parsed"), bOk);
		TestEqual(TEXT("sessionId"), Session.SessionId, FString(TEXT("sess1")));
		TestEqual(TEXT("appId parsed from string"), Session.AppId, static_cast<int64>(1));
		TestEqual(TEXT("name"), Session.Name, FString(TEXT("Skirmish")));
		TestEqual(TEXT("status"), Session.Status, FString(TEXT("active")));
		TestEqual(TEXT("createdByUserId"), Session.CreatedByUserId, static_cast<int64>(90001));
		TestEqual(TEXT("currentTurnUserId"), Session.CurrentTurnUserId, static_cast<int64>(90002));
		TestTrue(TEXT("bHasCurrentTurn true"), Session.bHasCurrentTurn);
	}

	// currentTurnUserId null => bHasCurrentTurn stays false (no one's turn), never a spurious 0.
	{
		const TSharedPtr<FJsonObject> Env = ParsePhase2BJson(TEXT(
			"{\"data\":{\"gameModelCreateSession\":{\"sessionId\":\"sess1\",\"appId\":\"1\",\"status\":\"active\","
			"\"currentTurnUserId\":null,\"metadataJson\":\"{}\"}}}"));
		FCrowdyGameSessionData Session;
		const bool bOk = FCrowdyGameApiCodec::ParseSessionEnvelope(
			Env, true, TArray<FString>(), TEXT("gameModelCreateSession"), Session);
		TestTrue(TEXT("session parsed"), bOk);
		TestFalse(TEXT("bHasCurrentTurn false when null"), Session.bHasCurrentTurn);
	}

	// A sessions LIST envelope parses each row.
	{
		const TSharedPtr<FJsonObject> Env = ParsePhase2BJson(TEXT(
			"{\"data\":{\"gameModelSessions\":["
			"{\"sessionId\":\"s1\",\"appId\":\"1\",\"status\":\"active\",\"metadataJson\":\"{}\"},"
			"{\"sessionId\":\"s2\",\"appId\":\"1\",\"status\":\"ended\",\"metadataJson\":\"{}\"}]}}"));
		TArray<FCrowdyGameSessionData> Sessions;
		const bool bOk = FCrowdyGameApiCodec::ParseSessionsEnvelope(Env, true, TArray<FString>(), Sessions);
		TestTrue(TEXT("sessions parsed"), bOk);
		if (TestEqual(TEXT("two sessions"), Sessions.Num(), 2))
		{
			TestEqual(TEXT("session[0] id"), Sessions[0].SessionId, FString(TEXT("s1")));
			TestEqual(TEXT("session[1] id"), Sessions[1].SessionId, FString(TEXT("s2")));
		}
	}

	// The full GmSession shape: revision arrives as a STRING and parses to int64; hostTerm / participantCount /
	// maxParticipants are numbers; hostUserId is a BigInt string; the bHas flags are set for present values.
	{
		const TSharedPtr<FJsonObject> Env = ParsePhase2BJson(TEXT(
			"{\"data\":{\"gameModelSession\":{\"sessionId\":\"sess1\",\"appId\":\"1\",\"name\":\"Skirmish\","
			"\"status\":\"ended\",\"createdByUserId\":\"90001\",\"currentTurnUserId\":null,\"metadataJson\":\"{}\","
			"\"admission\":\"locked\",\"maxParticipants\":4,\"participantCount\":3,\"hostUserId\":\"90002\","
			"\"hostTerm\":2,\"revision\":\"17\",\"endedAt\":\"2026-01-02T03:04:05Z\",\"endReason\":\"abandoned\","
			"\"createdAt\":\"2026-01-01T00:00:00Z\",\"presence\":\"none\"}}}"));
		FCrowdyGameSessionData Session;
		const bool bOk = FCrowdyGameApiCodec::ParseSessionEnvelope(
			Env, true, TArray<FString>(), TEXT("gameModelSession"), Session);
		TestTrue(TEXT("full session parsed"), bOk);
		TestEqual(TEXT("admission"), Session.Admission, FString(TEXT("locked")));
		TestTrue(TEXT("bHasMaxParticipants when present"), Session.bHasMaxParticipants);
		TestEqual(TEXT("maxParticipants"), Session.MaxParticipants, 4);
		TestEqual(TEXT("participantCount"), Session.ParticipantCount, 3);
		TestTrue(TEXT("bHasHost when present"), Session.bHasHost);
		TestEqual(TEXT("hostUserId parsed from string"), Session.HostUserId, static_cast<int64>(90002));
		TestEqual(TEXT("hostTerm"), Session.HostTerm, 2);
		TestEqual(TEXT("revision parsed from string"), Session.Revision, static_cast<int64>(17));
		TestEqual(TEXT("endedAt"), Session.EndedAt, FString(TEXT("2026-01-02T03:04:05Z")));
		TestEqual(TEXT("endReason"), Session.EndReason, FString(TEXT("abandoned")));
		TestEqual(TEXT("createdAt"), Session.CreatedAt, FString(TEXT("2026-01-01T00:00:00Z")));
		TestEqual(TEXT("presence"), Session.Presence, FString(TEXT("none")));
	}

	// maxParticipants / hostUserId null (unbounded, nobody joined) leave their bHas flags false and never read as
	// a real 0; a real 0 maxParticipants sets the flag. A numeric revision is accepted defensively.
	{
		const TSharedPtr<FJsonObject> Env = ParsePhase2BJson(TEXT(
			"{\"data\":{\"gameModelSession\":{\"sessionId\":\"sess1\",\"appId\":\"1\",\"status\":\"active\","
			"\"maxParticipants\":null,\"hostUserId\":null,\"participantCount\":0,\"hostTerm\":0,\"revision\":3,"
			"\"endedAt\":null,\"endReason\":null}}}"));
		FCrowdyGameSessionData Session;
		const bool bOk = FCrowdyGameApiCodec::ParseSessionEnvelope(
			Env, true, TArray<FString>(), TEXT("gameModelSession"), Session);
		TestTrue(TEXT("null-field session parsed"), bOk);
		TestFalse(TEXT("bHasMaxParticipants false when null"), Session.bHasMaxParticipants);
		TestFalse(TEXT("bHasHost false when null"), Session.bHasHost);
		TestEqual(TEXT("revision accepted as a number"), Session.Revision, static_cast<int64>(3));
		TestTrue(TEXT("endedAt empty when null"), Session.EndedAt.IsEmpty());
		TestTrue(TEXT("endReason empty when null"), Session.EndReason.IsEmpty());

		const TSharedPtr<FJsonObject> ZeroEnv = ParsePhase2BJson(TEXT(
			"{\"data\":{\"gameModelSession\":{\"sessionId\":\"sess1\",\"appId\":\"1\",\"maxParticipants\":0,"
			"\"hostUserId\":\"0\",\"hostTerm\":\"5\"}}}"));
		FCrowdyGameSessionData ZeroSession;
		FCrowdyGameApiCodec::ParseSessionEnvelope(ZeroEnv, true, TArray<FString>(), TEXT("gameModelSession"), ZeroSession);
		TestTrue(TEXT("bHasMaxParticipants true for a real 0"), ZeroSession.bHasMaxParticipants);
		TestEqual(TEXT("maxParticipants 0"), ZeroSession.MaxParticipants, 0);
		TestTrue(TEXT("bHasHost true for a real 0"), ZeroSession.bHasHost);
		TestEqual(TEXT("hostTerm accepted as a string"), ZeroSession.HostTerm, 5);
	}

	// Join returns a participant row; userId is a BigInt string parsed to int64 and incarnation a number.
	{
		const TSharedPtr<FJsonObject> Env = ParsePhase2BJson(TEXT(
			"{\"data\":{\"gameModelJoinSession\":{\"sessionId\":\"s\",\"userId\":\"90002\",\"role\":\"player\","
			"\"state\":\"joined\",\"incarnation\":2,\"actorUuid\":\"0123456789abcdef0123456789abcdef\","
			"\"joinedAt\":\"2026-01-01T00:00:00Z\",\"leftAt\":null,\"leftReason\":null}}}"));
		FCrowdyGameSessionParticipantData Participant;
		const bool bOk = FCrowdyGameApiCodec::ParseJoinSessionEnvelope(Env, true, TArray<FString>(), Participant);
		TestTrue(TEXT("join parsed"), bOk);
		TestEqual(TEXT("sessionId"), Participant.SessionId, FString(TEXT("s")));
		TestEqual(TEXT("userId parsed from string"), Participant.UserId, static_cast<int64>(90002));
		TestEqual(TEXT("role"), Participant.Role, FString(TEXT("player")));
		TestEqual(TEXT("state"), Participant.State, FString(TEXT("joined")));
		TestEqual(TEXT("incarnation"), Participant.Incarnation, 2);
		TestEqual(TEXT("actorUuid"), Participant.ActorUuid, FString(TEXT("0123456789abcdef0123456789abcdef")));
		TestEqual(TEXT("joinedAt"), Participant.JoinedAt, FString(TEXT("2026-01-01T00:00:00Z")));
		TestTrue(TEXT("leftAt empty when null"), Participant.LeftAt.IsEmpty());
	}

	// A null join (the server returned nothing) fails the parse and leaves the participant reset.
	{
		const TSharedPtr<FJsonObject> Env = ParsePhase2BJson(TEXT("{\"data\":{\"gameModelJoinSession\":null}}"));
		FCrowdyGameSessionParticipantData Participant;
		Participant.UserId = 5;
		const bool bOk = FCrowdyGameApiCodec::ParseJoinSessionEnvelope(Env, true, TArray<FString>(), Participant);
		TestFalse(TEXT("null join fails"), bOk);
		TestEqual(TEXT("participant reset on failure"), Participant.UserId, static_cast<int64>(0));
	}

	return true;
}

// The participant, event, snapshot and event-log envelopes: leave reads a participant row by field name, a
// snapshot carries its own revision plus the session and every participant, and the event log is an ordered
// array whose revisions (wire strings) parse to int64.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelParseSessionSnapshotAndEventsTest,
	"CrowdySDK.GameModel.ParseSessionSnapshotAndEvents", CrowdyGameModelPhase2BTestFlags)
bool FCrowdyGameModelParseSessionSnapshotAndEventsTest::RunTest(const FString& Parameters)
{
	// Leave reads the same participant shape under its own field; a left row carries leftAt / leftReason.
	{
		const TSharedPtr<FJsonObject> Env = ParsePhase2BJson(TEXT(
			"{\"data\":{\"gameModelLeaveSession\":{\"sessionId\":\"s\",\"userId\":\"90002\",\"role\":\"player\","
			"\"state\":\"left\",\"incarnation\":\"3\",\"actorUuid\":null,\"joinedAt\":\"2026-01-01T00:00:00Z\","
			"\"leftAt\":\"2026-01-01T00:10:00Z\",\"leftReason\":\"left\"}}}"));
		FCrowdyGameSessionParticipantData Participant;
		const bool bOk = FCrowdyGameApiCodec::ParseSessionParticipantEnvelope(
			Env, true, TArray<FString>(), TEXT("gameModelLeaveSession"), Participant);
		TestTrue(TEXT("leave parsed"), bOk);
		TestEqual(TEXT("state left"), Participant.State, FString(TEXT("left")));
		TestEqual(TEXT("incarnation accepted as a string"), Participant.Incarnation, 3);
		TestTrue(TEXT("actorUuid empty when null"), Participant.ActorUuid.IsEmpty());
		TestEqual(TEXT("leftAt"), Participant.LeftAt, FString(TEXT("2026-01-01T00:10:00Z")));
		TestEqual(TEXT("leftReason"), Participant.LeftReason, FString(TEXT("left")));

		// The wrong field name is a miss, not a stale read.
		FCrowdyGameSessionParticipantData Miss;
		TestFalse(TEXT("other field name misses"), FCrowdyGameApiCodec::ParseSessionParticipantEnvelope(
			Env, true, TArray<FString>(), TEXT("gameModelJoinSession"), Miss));
	}

	// A single event object: appId and revision are wire strings parsed to int64.
	{
		const TSharedPtr<FJsonObject> Obj = ParsePhase2BJson(TEXT(
			"{\"appId\":\"1\",\"sessionId\":\"s\",\"revision\":\"9\",\"kind\":\"host_changed\","
			"\"payloadJson\":\"{\\\"hostUserId\\\":\\\"90002\\\"}\",\"createdAt\":\"2026-01-01T00:00:00Z\"}"));
		const FCrowdyGameSessionEventData Event = FCrowdyGameApiCodec::ParseSessionEventObject(Obj);
		TestEqual(TEXT("event appId"), Event.AppId, static_cast<int64>(1));
		TestEqual(TEXT("event sessionId"), Event.SessionId, FString(TEXT("s")));
		TestEqual(TEXT("event revision parsed from string"), Event.Revision, static_cast<int64>(9));
		TestEqual(TEXT("event kind"), Event.Kind, FString(TEXT("host_changed")));
		TestEqual(TEXT("event payloadJson verbatim"), Event.PayloadJson, FString(TEXT("{\"hostUserId\":\"90002\"}")));
		TestEqual(TEXT("event createdAt"), Event.CreatedAt, FString(TEXT("2026-01-01T00:00:00Z")));
	}

	// Snapshot: its own revision (a wire string), the session, and every participant row.
	{
		const TSharedPtr<FJsonObject> Env = ParsePhase2BJson(TEXT(
			"{\"data\":{\"gameModelSessionSnapshot\":{\"revision\":\"12\","
			"\"session\":{\"sessionId\":\"s\",\"appId\":\"1\",\"status\":\"active\",\"admission\":\"open\","
			"\"hostUserId\":\"90001\",\"hostTerm\":1,\"participantCount\":2,\"revision\":\"12\"},"
			"\"participants\":["
			"{\"sessionId\":\"s\",\"userId\":\"90001\",\"role\":\"host\",\"state\":\"joined\",\"incarnation\":1},"
			"{\"sessionId\":\"s\",\"userId\":\"90002\",\"role\":\"player\",\"state\":\"joined\",\"incarnation\":4}]}}}"));
		FCrowdyGameSessionSnapshotData Snapshot;
		const bool bOk = FCrowdyGameApiCodec::ParseSessionSnapshotEnvelope(Env, true, TArray<FString>(), Snapshot);
		TestTrue(TEXT("snapshot parsed"), bOk);
		TestEqual(TEXT("snapshot revision parsed from string"), Snapshot.Revision, static_cast<int64>(12));
		TestEqual(TEXT("snapshot session id"), Snapshot.Session.SessionId, FString(TEXT("s")));
		TestEqual(TEXT("snapshot session admission"), Snapshot.Session.Admission, FString(TEXT("open")));
		TestTrue(TEXT("snapshot session has host"), Snapshot.Session.bHasHost);
		TestEqual(TEXT("snapshot session host"), Snapshot.Session.HostUserId, static_cast<int64>(90001));
		if (TestEqual(TEXT("two participants"), Snapshot.Participants.Num(), 2))
		{
			TestEqual(TEXT("participant[0] userId"), Snapshot.Participants[0].UserId, static_cast<int64>(90001));
			TestEqual(TEXT("participant[0] role"), Snapshot.Participants[0].Role, FString(TEXT("host")));
			TestEqual(TEXT("participant[1] userId"), Snapshot.Participants[1].UserId, static_cast<int64>(90002));
			TestEqual(TEXT("participant[1] incarnation"), Snapshot.Participants[1].Incarnation, 4);
		}
	}

	// A GraphQL error fails the snapshot parse and resets the output.
	{
		const TSharedPtr<FJsonObject> Env = ParsePhase2BJson(TEXT(
			"{\"data\":{\"gameModelSessionSnapshot\":{\"revision\":\"12\",\"session\":{\"sessionId\":\"s\"},\"participants\":[]}}}"));
		TArray<FString> Errors;
		Errors.Add(TEXT("SESSION_NOT_PARTICIPANT"));
		FCrowdyGameSessionSnapshotData Snapshot;
		Snapshot.Revision = 99;
		TestFalse(TEXT("errors[] fail the snapshot"), FCrowdyGameApiCodec::ParseSessionSnapshotEnvelope(Env, true, Errors, Snapshot));
		TestEqual(TEXT("snapshot reset on failure"), Snapshot.Revision, static_cast<int64>(0));
	}

	// The event log: an ordered array, each revision parsed from its wire string.
	{
		const TSharedPtr<FJsonObject> Env = ParsePhase2BJson(TEXT(
			"{\"data\":{\"gameModelSessionEvents\":["
			"{\"appId\":\"1\",\"sessionId\":\"s\",\"revision\":\"1\",\"kind\":\"created\",\"payloadJson\":\"{}\"},"
			"{\"appId\":\"1\",\"sessionId\":\"s\",\"revision\":\"2\",\"kind\":\"participant_joined\",\"payloadJson\":\"{}\"},"
			"{\"appId\":\"1\",\"sessionId\":\"s\",\"revision\":\"3\",\"kind\":\"turn_changed\",\"payloadJson\":\"{}\"}]}}"));
		TArray<FCrowdyGameSessionEventData> Events;
		const bool bOk = FCrowdyGameApiCodec::ParseSessionEventsEnvelope(Env, true, TArray<FString>(), Events);
		TestTrue(TEXT("events parsed"), bOk);
		if (TestEqual(TEXT("three events"), Events.Num(), 3))
		{
			TestEqual(TEXT("event[0] revision"), Events[0].Revision, static_cast<int64>(1));
			TestEqual(TEXT("event[0] kind"), Events[0].Kind, FString(TEXT("created")));
			TestEqual(TEXT("event[2] revision"), Events[2].Revision, static_cast<int64>(3));
			TestEqual(TEXT("event[2] kind"), Events[2].Kind, FString(TEXT("turn_changed")));
		}

		// An empty log is a clean read, not a failure; a missing field is a failure.
		TArray<FCrowdyGameSessionEventData> Empty;
		TestTrue(TEXT("empty log parses"), FCrowdyGameApiCodec::ParseSessionEventsEnvelope(
			ParsePhase2BJson(TEXT("{\"data\":{\"gameModelSessionEvents\":[]}}")), true, TArray<FString>(), Empty));
		TestEqual(TEXT("empty log has no events"), Empty.Num(), 0);
		TestFalse(TEXT("missing field fails"), FCrowdyGameApiCodec::ParseSessionEventsEnvelope(
			ParsePhase2BJson(TEXT("{\"data\":{}}")), true, TArray<FString>(), Empty));
	}

	return true;
}

// The graph builders emit appId as a string, write weight only as a real JSON number when present, and clamp
// the traverse depth into the server's [1,5] range as a JSON number on the flat (non-input) query variables.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelGraphBuildsRequestTest,
	"CrowdySDK.GameModel.GraphBuildsRequest", CrowdyGameModelPhase2BTestFlags)
bool FCrowdyGameModelGraphBuildsRequestTest::RunTest(const FString& Parameters)
{
	// AddEdge: appId a string, edge endpoints round-trip, weight/metadata omitted when absent.
	{
		const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildAddEdgeVariables(
			1, TEXT("a"), TEXT("b"), TEXT("contains"), 0.0, false, FString());
		const TSharedPtr<FJsonObject>* InputPtr = nullptr;
		if (TestTrue(TEXT("input present"), Vars.IsValid() && Vars->TryGetObjectField(TEXT("input"), InputPtr) && InputPtr))
		{
			const TSharedPtr<FJsonValue> AppIdValue = (*InputPtr)->TryGetField(TEXT("appId"));
			if (TestNotNull(TEXT("appId present"), AppIdValue.Get()))
			{
				TestEqual(TEXT("appId is a JSON string"), static_cast<int32>(AppIdValue->Type), static_cast<int32>(EJson::String));
				TestEqual(TEXT("appId value"), AppIdValue->AsString(), FString(TEXT("1")));
			}
			FString From, To, Rel;
			TestTrue(TEXT("fromContainerId present"), (*InputPtr)->TryGetStringField(TEXT("fromContainerId"), From));
			TestEqual(TEXT("fromContainerId"), From, FString(TEXT("a")));
			TestTrue(TEXT("toContainerId present"), (*InputPtr)->TryGetStringField(TEXT("toContainerId"), To));
			TestEqual(TEXT("toContainerId"), To, FString(TEXT("b")));
			TestTrue(TEXT("relationshipType present"), (*InputPtr)->TryGetStringField(TEXT("relationshipType"), Rel));
			TestEqual(TEXT("relationshipType"), Rel, FString(TEXT("contains")));
			TestFalse(TEXT("weight omitted when !bHasWeight"), (*InputPtr)->HasField(TEXT("weight")));
			TestFalse(TEXT("metadataJson omitted when empty"), (*InputPtr)->HasField(TEXT("metadataJson")));
		}
	}

	// AddEdge with a weight: it is a JSON NUMBER (Float), not a string.
	{
		const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildAddEdgeVariables(
			1, TEXT("a"), TEXT("b"), TEXT("contains"), 2.5, true, FString());
		const TSharedPtr<FJsonObject>* InputPtr = nullptr;
		Vars->TryGetObjectField(TEXT("input"), InputPtr);
		if (TestTrue(TEXT("input present"), InputPtr != nullptr))
		{
			const TSharedPtr<FJsonValue> WeightValue = (*InputPtr)->TryGetField(TEXT("weight"));
			if (TestNotNull(TEXT("weight present"), WeightValue.Get()))
			{
				TestEqual(TEXT("weight is a JSON number"), static_cast<int32>(WeightValue->Type), static_cast<int32>(EJson::Number));
				TestEqual(TEXT("weight value"), WeightValue->AsNumber(), 2.5);
			}
		}
	}

	// Traverse is a QUERY: flat variables (no input wrapper), appId a string, depth a JSON number clamped to 5.
	{
		const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildTraverseVariables(1, TEXT("root"), TEXT("contains"), 9);
		if (TestNotNull(TEXT("variables built"), Vars.Get()))
		{
			const TSharedPtr<FJsonValue> AppIdValue = Vars->TryGetField(TEXT("appId"));
			if (TestNotNull(TEXT("appId present"), AppIdValue.Get()))
			{
				TestEqual(TEXT("appId is a JSON string"), static_cast<int32>(AppIdValue->Type), static_cast<int32>(EJson::String));
				TestEqual(TEXT("appId value"), AppIdValue->AsString(), FString(TEXT("1")));
			}
			FString RootId, Rel;
			TestTrue(TEXT("rootId present"), Vars->TryGetStringField(TEXT("rootId"), RootId));
			TestEqual(TEXT("rootId"), RootId, FString(TEXT("root")));
			TestTrue(TEXT("relationshipType present"), Vars->TryGetStringField(TEXT("relationshipType"), Rel));
			TestEqual(TEXT("relationshipType"), Rel, FString(TEXT("contains")));

			const TSharedPtr<FJsonValue> DepthValue = Vars->TryGetField(TEXT("depth"));
			if (TestNotNull(TEXT("depth present"), DepthValue.Get()))
			{
				TestEqual(TEXT("depth is a JSON number"), static_cast<int32>(DepthValue->Type), static_cast<int32>(EJson::Number));
				TestEqual(TEXT("depth clamped to 5"), static_cast<int32>(DepthValue->AsNumber()), 5);
			}
		}
	}

	// Depth below the floor clamps up to 1 (the server's minimum), never 0.
	{
		const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildTraverseVariables(1, TEXT("root"), TEXT("contains"), 0);
		if (TestNotNull(TEXT("variables built"), Vars.Get()))
		{
			const TSharedPtr<FJsonValue> DepthValue = Vars->TryGetField(TEXT("depth"));
			if (TestNotNull(TEXT("depth present"), DepthValue.Get()))
			{
				TestEqual(TEXT("depth clamped to 1"), static_cast<int32>(DepthValue->AsNumber()), 1);
			}
		}
	}

	return true;
}

// The graph parsers read a GmEdge's fields (including the optional weight) and a traversal's root, its raw
// container nodes, and the walked edges.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelParseGraphTest,
	"CrowdySDK.GameModel.ParseGraph", CrowdyGameModelPhase2BTestFlags)
bool FCrowdyGameModelParseGraphTest::RunTest(const FString& Parameters)
{
	// AddEdge parsing reads the GmEdge fields and a present weight.
	{
		const TSharedPtr<FJsonObject> Env = ParsePhase2BJson(TEXT(
			"{\"data\":{\"gameModelAddEdge\":{\"edgeId\":\"e1\",\"fromContainerId\":\"a\",\"toContainerId\":\"b\","
			"\"relationshipType\":\"contains\",\"weight\":2.5}}}"));
		FCrowdyEdgeData Edge;
		const bool bOk = FCrowdyGameApiCodec::ParseAddEdgeEnvelope(Env, true, TArray<FString>(), Edge);
		TestTrue(TEXT("edge parsed"), bOk);
		TestEqual(TEXT("edgeId"), Edge.EdgeId, FString(TEXT("e1")));
		TestEqual(TEXT("fromContainerId"), Edge.FromContainerId, FString(TEXT("a")));
		TestEqual(TEXT("toContainerId"), Edge.ToContainerId, FString(TEXT("b")));
		TestEqual(TEXT("relationshipType"), Edge.RelationshipType, FString(TEXT("contains")));
		TestTrue(TEXT("bHasWeight when present"), Edge.bHasWeight);
		TestEqual(TEXT("weight value"), Edge.Weight, 2.5);
	}

	// Traverse parsing: rootId, the reachable nodes (raw GmContainer objects), and the walked edges.
	{
		const TSharedPtr<FJsonObject> Env = ParsePhase2BJson(TEXT(
			"{\"data\":{\"gameModelTraverse\":{\"rootId\":\"root\",\"nodes\":["
			"{\"containerId\":\"n1\",\"typeName\":\"Item\",\"ownerUserId\":\"90001\",\"metadataJson\":\"{}\"},"
			"{\"containerId\":\"n2\",\"typeName\":\"Item\",\"ownerUserId\":\"90001\",\"metadataJson\":\"{}\"}],"
			"\"edges\":[{\"edgeId\":\"e1\",\"fromContainerId\":\"root\",\"toContainerId\":\"n1\","
			"\"relationshipType\":\"contains\"}]}}}"));
		FCrowdyTraverseData Result;
		const bool bOk = FCrowdyGameApiCodec::ParseTraverseEnvelope(Env, true, TArray<FString>(), Result);
		TestTrue(TEXT("traverse parsed"), bOk);
		TestEqual(TEXT("rootId"), Result.RootId, FString(TEXT("root")));
		if (TestEqual(TEXT("two nodes"), Result.Nodes.Num(), 2))
		{
			if (TestTrue(TEXT("first node valid"), Result.Nodes[0].IsValid()))
			{
				FString FirstNodeId;
				TestTrue(TEXT("first node has containerId"), Result.Nodes[0]->TryGetStringField(TEXT("containerId"), FirstNodeId));
				TestEqual(TEXT("first node containerId"), FirstNodeId, FString(TEXT("n1")));
			}
		}
		TestEqual(TEXT("one edge"), Result.Edges.Num(), 1);
	}

	return true;
}

// The set-property builder passes the already-JSON-encoded valueJson through verbatim, quotes intact, rather
// than re-encoding it into a double-escaped string.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelSetPropertyBuildsRequestTest,
	"CrowdySDK.GameModel.SetPropertyBuildsRequest", CrowdyGameModelPhase2BTestFlags)
bool FCrowdyGameModelSetPropertyBuildsRequestTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildSetPropertyVariables(
		1, TEXT("c1"), TEXT("display_name"), TEXT("string"), TEXT("\"Aria\""));
	const TSharedPtr<FJsonObject>* InputPtr = nullptr;
	if (!TestTrue(TEXT("input present"), Vars.IsValid() && Vars->TryGetObjectField(TEXT("input"), InputPtr) && InputPtr))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>& Input = *InputPtr;

	const TSharedPtr<FJsonValue> AppIdValue = Input->TryGetField(TEXT("appId"));
	if (TestNotNull(TEXT("appId present"), AppIdValue.Get()))
	{
		TestEqual(TEXT("appId is a JSON string"), static_cast<int32>(AppIdValue->Type), static_cast<int32>(EJson::String));
		TestEqual(TEXT("appId value"), AppIdValue->AsString(), FString(TEXT("1")));
	}

	FString ContainerId, Key, ValueType, ValueJson;
	TestTrue(TEXT("containerId present"), Input->TryGetStringField(TEXT("containerId"), ContainerId));
	TestEqual(TEXT("containerId"), ContainerId, FString(TEXT("c1")));
	TestTrue(TEXT("key present"), Input->TryGetStringField(TEXT("key"), Key));
	TestEqual(TEXT("key"), Key, FString(TEXT("display_name")));
	TestTrue(TEXT("valueType present"), Input->TryGetStringField(TEXT("valueType"), ValueType));
	TestEqual(TEXT("valueType"), ValueType, FString(TEXT("string")));

	// The verbatim contract: the stored field value equals the exact JSON string "Aria" WITH its surrounding
	// quotes, not stripped and not re-escaped to a double-encoded form.
	TestTrue(TEXT("valueJson present"), Input->TryGetStringField(TEXT("valueJson"), ValueJson));
	TestEqual(TEXT("valueJson passed through verbatim (quotes preserved)"), ValueJson, FString(TEXT("\"Aria\"")));

	return true;
}

// Server deletes: the wire builders emit appId as a JSON string with the id as a TOP-LEVEL variable (these ops
// take flat args, not an input object).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelDeleteBuildsRequestTest,
	"CrowdySDK.GameModel.DeleteBuildsRequest", CrowdyGameModelPhase2BTestFlags)
bool FCrowdyGameModelDeleteBuildsRequestTest::RunTest(const FString& Parameters)
{
	// deleteContainer: top-level { appId (string), containerId } -- no "input" wrapper.
	{
		const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildDeleteContainerVariables(1, TEXT("c1"));
		if (TestTrue(TEXT("delete-container vars present"), Vars.IsValid()))
		{
			const TSharedPtr<FJsonObject>* Unused = nullptr;
			TestFalse(TEXT("no input wrapper (flat args)"), Vars->TryGetObjectField(TEXT("input"), Unused));

			const TSharedPtr<FJsonValue> AppIdValue = Vars->TryGetField(TEXT("appId"));
			if (TestNotNull(TEXT("appId present"), AppIdValue.Get()))
			{
				TestEqual(TEXT("appId is a JSON string"), static_cast<int32>(AppIdValue->Type), static_cast<int32>(EJson::String));
				TestEqual(TEXT("appId value"), AppIdValue->AsString(), FString(TEXT("1")));
			}
			FString ContainerId;
			TestTrue(TEXT("containerId present"), Vars->TryGetStringField(TEXT("containerId"), ContainerId));
			TestEqual(TEXT("containerId"), ContainerId, FString(TEXT("c1")));
		}
	}
	// deleteEdge: top-level { appId (string), edgeId }.
	{
		const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildDeleteEdgeVariables(1, TEXT("e1"));
		if (TestTrue(TEXT("delete-edge vars present"), Vars.IsValid()))
		{
			const TSharedPtr<FJsonValue> AppIdValue = Vars->TryGetField(TEXT("appId"));
			if (TestNotNull(TEXT("appId present"), AppIdValue.Get()))
			{
				TestEqual(TEXT("appId is a JSON string"), static_cast<int32>(AppIdValue->Type), static_cast<int32>(EJson::String));
			}
			FString EdgeId;
			TestTrue(TEXT("edgeId present"), Vars->TryGetStringField(TEXT("edgeId"), EdgeId));
			TestEqual(TEXT("edgeId"), EdgeId, FString(TEXT("e1")));
		}
	}
	return true;
}

// The delete parsers read the Boolean! result (true = deleted, false = idempotent no-op) and route a transport
// failure or a GraphQL error (an authorization/refusal) to bOk=false.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelParseDeleteTest,
	"CrowdySDK.GameModel.ParseDelete", CrowdyGameModelPhase2BTestFlags)
bool FCrowdyGameModelParseDeleteTest::RunTest(const FString& Parameters)
{
	// A committed delete: Boolean! true -> bOk, bDeleted true.
	{
		const TSharedPtr<FJsonObject> Env = ParsePhase2BJson(TEXT("{\"data\":{\"gameModelDeleteContainer\":true}}"));
		bool bDeleted = false;
		const bool bOk = FCrowdyGameApiCodec::ParseDeleteContainerEnvelope(Env, true, TArray<FString>(), bDeleted);
		TestTrue(TEXT("clean transport"), bOk);
		TestTrue(TEXT("bDeleted true"), bDeleted);
	}
	// An idempotent no-op (it did not exist): Boolean! false -> bOk, bDeleted false.
	{
		const TSharedPtr<FJsonObject> Env = ParsePhase2BJson(TEXT("{\"data\":{\"gameModelDeleteEdge\":false}}"));
		bool bDeleted = true;
		const bool bOk = FCrowdyGameApiCodec::ParseDeleteEdgeEnvelope(Env, true, TArray<FString>(), bDeleted);
		TestTrue(TEXT("clean transport"), bOk);
		TestFalse(TEXT("bDeleted false (did not exist)"), bDeleted);
	}
	// A GraphQL error (e.g. an authorization/refusal) -> bOk false, OutDeleted reset.
	{
		const TSharedPtr<FJsonObject> Env = ParsePhase2BJson(TEXT("{\"data\":{\"gameModelDeleteContainer\":true}}"));
		bool bDeleted = false;
		TArray<FString> Errors;
		Errors.Add(TEXT("not authorized"));
		const bool bOk = FCrowdyGameApiCodec::ParseDeleteContainerEnvelope(Env, true, Errors, bDeleted);
		TestFalse(TEXT("errors[] -> transport failure"), bOk);
		TestFalse(TEXT("bDeleted reset on failure"), bDeleted);
	}
	// A non-2xx HTTP -> bOk false.
	{
		const TSharedPtr<FJsonObject> Env = ParsePhase2BJson(TEXT("{\"data\":{\"gameModelDeleteContainer\":true}}"));
		bool bDeleted = false;
		const bool bOk = FCrowdyGameApiCodec::ParseDeleteContainerEnvelope(Env, false, TArray<FString>(), bDeleted);
		TestFalse(TEXT("!httpOk -> failure"), bOk);
	}
	return true;
}

// A free/data container's apply seam keeps a per-containerId cache and broadcasts OnDataContainerChanged
// exactly once per changed apply, with no cross-container cache bleed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelDataContainerCacheAndDelegateTest,
	"CrowdySDK.GameModel.DataContainerCacheAndDelegate", CrowdyGameModelPhase2BTestFlags)
bool FCrowdyGameModelDataContainerCacheAndDelegateTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	UCrowdyGameModelTestTarget* Target = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("target created"), Target))
	{
		return false;
	}

	Model->OnDataContainerChanged.AddDynamic(Target, &UCrowdyGameModelTestTarget::HandleDataContainerChanged);

	// First apply to "c1": every value is new, so the delegate fires exactly once for "c1" and the by-id cache
	// holds each canonical value.
	{
		const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
		State->SetNumberField(TEXT("gold"), 10);
		State->SetNumberField(TEXT("potions"), 3);
		Model->ApplyDataContainerState(TEXT("c1"), State);
	}
	TestEqual(TEXT("delegate fired once"), Target->DataChangedCount, 1);
	TestEqual(TEXT("fired for c1"), Target->LastChangedContainerId, FString(TEXT("c1")));
	FString GoldJson;
	if (TestTrue(TEXT("c1 gold cached"), Model->TryGetContainerValueJson(TEXT("c1"), FName(TEXT("gold")), GoldJson)))
	{
		TestEqual(TEXT("c1 gold value"), GoldJson, FString(TEXT("10")));
	}

	// Re-apply identical state: nothing changed, so no further broadcast.
	{
		const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
		State->SetNumberField(TEXT("gold"), 10);
		State->SetNumberField(TEXT("potions"), 3);
		Model->ApplyDataContainerState(TEXT("c1"), State);
	}
	TestEqual(TEXT("no fire on unchanged re-apply"), Target->DataChangedCount, 1);

	// Change gold 10 -> 7: the container changed, so it fires again (once), and the cache reflects the new value.
	{
		const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
		State->SetNumberField(TEXT("gold"), 7);
		State->SetNumberField(TEXT("potions"), 3);
		Model->ApplyDataContainerState(TEXT("c1"), State);
	}
	TestEqual(TEXT("fire on change"), Target->DataChangedCount, 2);
	FString GoldJson2;
	if (TestTrue(TEXT("c1 gold still cached"), Model->TryGetContainerValueJson(TEXT("c1"), FName(TEXT("gold")), GoldJson2)))
	{
		TestEqual(TEXT("c1 gold updated"), GoldJson2, FString(TEXT("7")));
	}

	// A DIFFERENT container "c2" has its own cache: applying to it fires for "c2" and never disturbs c1.
	{
		const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
		State->SetNumberField(TEXT("gold"), 99);
		State->SetNumberField(TEXT("potions"), 1);
		Model->ApplyDataContainerState(TEXT("c2"), State);
	}
	TestEqual(TEXT("fire for c2"), Target->DataChangedCount, 3);
	TestEqual(TEXT("fired for c2"), Target->LastChangedContainerId, FString(TEXT("c2")));

	// The caches do not cross: c1 keeps 7 while c2 holds 99.
	FString C1Gold, C2Gold;
	if (TestTrue(TEXT("c1 gold present"), Model->TryGetContainerValueJson(TEXT("c1"), FName(TEXT("gold")), C1Gold)))
	{
		TestEqual(TEXT("c1 gold unchanged by c2 apply"), C1Gold, FString(TEXT("7")));
	}
	if (TestTrue(TEXT("c2 gold present"), Model->TryGetContainerValueJson(TEXT("c2"), FName(TEXT("gold")), C2Gold)))
	{
		TestEqual(TEXT("c2 gold value"), C2Gold, FString(TEXT("99")));
	}

	// Per-container potions stay independent too.
	FString C1Potions, C2Potions;
	if (TestTrue(TEXT("c1 potions present"), Model->TryGetContainerValueJson(TEXT("c1"), FName(TEXT("potions")), C1Potions)))
	{
		TestEqual(TEXT("c1 potions"), C1Potions, FString(TEXT("3")));
	}
	if (TestTrue(TEXT("c2 potions present"), Model->TryGetContainerValueJson(TEXT("c2"), FName(TEXT("potions")), C2Potions)))
	{
		TestEqual(TEXT("c2 potions"), C2Potions, FString(TEXT("1")));
	}

	// A re-pull that DROPS a key (potions taken from the inventory) reconciles the removal: potions leaves the
	// cache, and the removal alone (gold unchanged) is still a change the bound UI must see. A free/data
	// container's key set is dynamic, unlike an actor's fixed attribute set.
	{
		const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
		State->SetNumberField(TEXT("gold"), 7);
		Model->ApplyDataContainerState(TEXT("c1"), State);
	}
	TestEqual(TEXT("removal alone fires the delegate"), Target->DataChangedCount, 4);
	FString RemovedPotions;
	TestFalse(TEXT("dropped key is evicted from the cache"),
		Model->TryGetContainerValueJson(TEXT("c1"), FName(TEXT("potions")), RemovedPotions));
	FString GoldAfterRemoval;
	if (TestTrue(TEXT("surviving key stays cached"), Model->TryGetContainerValueJson(TEXT("c1"), FName(TEXT("gold")), GoldAfterRemoval)))
	{
		TestEqual(TEXT("surviving gold value"), GoldAfterRemoval, FString(TEXT("7")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
