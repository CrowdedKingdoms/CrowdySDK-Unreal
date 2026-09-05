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

	// SetSessionTurn: userId written as a BigInt string when present...
	{
		const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildSetSessionTurnVariables(1, TEXT("s"), 90002, true);
		const TSharedPtr<FJsonObject>* InputPtr = nullptr;
		if (TestTrue(TEXT("input present"), Vars.IsValid() && Vars->TryGetObjectField(TEXT("input"), InputPtr) && InputPtr))
		{
			const TSharedPtr<FJsonValue> UserIdValue = (*InputPtr)->TryGetField(TEXT("userId"));
			if (TestNotNull(TEXT("userId present"), UserIdValue.Get()))
			{
				TestEqual(TEXT("userId is a JSON string"), static_cast<int32>(UserIdValue->Type), static_cast<int32>(EJson::String));
				TestEqual(TEXT("userId string value"), UserIdValue->AsString(), FString(TEXT("90002")));
			}
		}
	}

	// ...and an EXPLICIT JSON null when clearing the turn (bHasUserId=false). The schema clears a turn on null,
	// NOT on an absent field, so the key must be present and null, while sessionId stays.
	{
		const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildSetSessionTurnVariables(1, TEXT("s"), 90002, false);
		const TSharedPtr<FJsonObject>* InputPtr = nullptr;
		Vars->TryGetObjectField(TEXT("input"), InputPtr);
		if (TestTrue(TEXT("input present"), InputPtr != nullptr))
		{
			const TSharedPtr<FJsonValue> UserIdValue = (*InputPtr)->TryGetField(TEXT("userId"));
			if (TestNotNull(TEXT("userId present when clearing"), UserIdValue.Get()))
			{
				TestEqual(TEXT("userId is JSON null when clearing the turn"),
					static_cast<int32>(UserIdValue->Type), static_cast<int32>(EJson::Null));
			}
			FString SessionId;
			TestTrue(TEXT("sessionId present"), (*InputPtr)->TryGetStringField(TEXT("sessionId"), SessionId));
			TestEqual(TEXT("sessionId value"), SessionId, FString(TEXT("s")));
		}
	}

	// JoinSession: role omitted when empty, present when set.
	{
		const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildJoinSessionVariables(1, TEXT("s"), FString());
		const TSharedPtr<FJsonObject>* InputPtr = nullptr;
		Vars->TryGetObjectField(TEXT("input"), InputPtr);
		if (TestTrue(TEXT("input present"), InputPtr != nullptr))
		{
			TestFalse(TEXT("role omitted when empty"), (*InputPtr)->HasField(TEXT("role")));
		}

		const TSharedPtr<FJsonObject> Vars2 = FCrowdyGameApiCodec::BuildJoinSessionVariables(1, TEXT("s"), TEXT("gm"));
		const TSharedPtr<FJsonObject>* Input2Ptr = nullptr;
		Vars2->TryGetObjectField(TEXT("input"), Input2Ptr);
		if (TestTrue(TEXT("input present (role set)"), Input2Ptr != nullptr))
		{
			FString Role;
			TestTrue(TEXT("role present when set"), (*Input2Ptr)->TryGetStringField(TEXT("role"), Role));
			TestEqual(TEXT("role value"), Role, FString(TEXT("gm")));
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

	// Join returns the participant triple; userId is a BigInt string parsed to int64.
	{
		const TSharedPtr<FJsonObject> Env = ParsePhase2BJson(TEXT(
			"{\"data\":{\"gameModelJoinSession\":{\"sessionId\":\"s\",\"userId\":\"90002\",\"role\":\"player\"}}}"));
		FString SessionId;
		int64 UserId = 0;
		FString Role;
		const bool bOk = FCrowdyGameApiCodec::ParseJoinSessionEnvelope(
			Env, true, TArray<FString>(), SessionId, UserId, Role);
		TestTrue(TEXT("join parsed"), bOk);
		TestEqual(TEXT("sessionId"), SessionId, FString(TEXT("s")));
		TestEqual(TEXT("userId parsed from string"), UserId, static_cast<int64>(90002));
		TestEqual(TEXT("role"), Role, FString(TEXT("player")));
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
