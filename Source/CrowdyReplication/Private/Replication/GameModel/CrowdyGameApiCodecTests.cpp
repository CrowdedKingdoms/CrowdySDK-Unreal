#include "Network/GraphQL/FCrowdyGameApiCodec.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Replication/GameModel/CrowdyGameModelTestSupport.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyGameApiTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// The invoke request builder emits appId as a JSON STRING (BigInt! scalar), never a number, and lays out
// the { input: { appId, functionName, selfContainerId, sessionId?, paramsJson } } shape. sessionId is
// omitted for app-global scope and present when set. This is the regression guard for the BigInt
// encoding the single most load-bearing wire detail in the whole Game Model client.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelInvokeBuildsRequestTest,
	"CrowdySDK.GameModel.InvokeBuildsRequest", CrowdyGameApiTestFlags)
bool FCrowdyGameModelInvokeBuildsRequestTest::RunTest(const FString& Parameters)
{
	FCrowdyInvokeRequest Req;
	Req.AppId = 1;
	Req.FunctionName = TEXT("attack");
	Req.SelfContainerId = TEXT("container-uuid");
	Req.Params = MakeShared<FJsonObject>();
	Req.Params->SetStringField(TEXT("target_id"), TEXT("enemy-uuid"));

	const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildInvokeVariables(Req);
	if (!TestNotNull(TEXT("variables built"), Vars.Get()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* InputPtr = nullptr;
	if (!TestTrue(TEXT("input object present"), Vars->TryGetObjectField(TEXT("input"), InputPtr) && InputPtr != nullptr))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>& Input = *InputPtr;

	// The critical assertion: appId is a JSON STRING value "1", not the number 1.
	const TSharedPtr<FJsonValue> AppIdValue = Input->TryGetField(TEXT("appId"));
	if (!TestNotNull(TEXT("appId present"), AppIdValue.Get()))
	{
		return false;
	}
	TestEqual(TEXT("appId is a JSON string, not a number"), static_cast<int32>(AppIdValue->Type), static_cast<int32>(EJson::String));
	TestEqual(TEXT("appId string value is \"1\""), AppIdValue->AsString(), FString(TEXT("1")));

	FString FunctionName;
	TestTrue(TEXT("functionName present"), Input->TryGetStringField(TEXT("functionName"), FunctionName));
	TestEqual(TEXT("functionName round-trips"), FunctionName, FString(TEXT("attack")));

	FString SelfId;
	TestTrue(TEXT("selfContainerId present"), Input->TryGetStringField(TEXT("selfContainerId"), SelfId));
	TestEqual(TEXT("selfContainerId round-trips"), SelfId, FString(TEXT("container-uuid")));

	// paramsJson is a compact JSON string carrying the params object.
	FString ParamsJson;
	TestTrue(TEXT("paramsJson present"), Input->TryGetStringField(TEXT("paramsJson"), ParamsJson));
	TestTrue(TEXT("paramsJson carries the param"), ParamsJson.Contains(TEXT("target_id")));

	// sessionId omitted for app-global scope.
	TestFalse(TEXT("sessionId omitted when empty"), Input->HasField(TEXT("sessionId")));

	// ...present when set.
	Req.SessionId = TEXT("session-uuid");
	const TSharedPtr<FJsonObject> Vars2 = FCrowdyGameApiCodec::BuildInvokeVariables(Req);
	const TSharedPtr<FJsonObject>* Input2Ptr = nullptr;
	Vars2->TryGetObjectField(TEXT("input"), Input2Ptr);
	FString SessionId;
	TestTrue(TEXT("sessionId present when set"), (*Input2Ptr)->TryGetStringField(TEXT("sessionId"), SessionId));
	TestEqual(TEXT("sessionId round-trips"), SessionId, FString(TEXT("session-uuid")));

	return true;
}

// Parsing discriminates the three outcomes that must never be confused: committed success (transport ok +
// success + mutations), a rolled-back logic/authority failure (transport ok + success=false + errorMessage,
// NO mutations so a caller's cache stays untouched), and a transport/GraphQL failure (errors[] or !2xx ->
// NOT transport ok). Conflating the last two would make a caller retry a rolled-back invoke as a network error.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelParseInvokeResultTest,
	"CrowdySDK.GameModel.ParseInvokeResult", CrowdyGameApiTestFlags)
bool FCrowdyGameModelParseInvokeResultTest::RunTest(const FString& Parameters)
{
	// (1) success + mutations
	{
		const TSharedPtr<FJsonObject> Env = ParseObject(TEXT(
			"{\"data\":{\"gameModelInvoke\":{\"eventId\":\"e1\",\"functionName\":\"attack\",\"success\":true,"
			"\"returnValueJson\":\"87\",\"errorMessage\":null,"
			"\"mutationsApplied\":[{\"containerId\":\"c-victim\",\"key\":\"hp\",\"oldValueJson\":\"100\","
			"\"newValueJson\":\"87\"},"
			"{\"containerId\":\"c-attacker\",\"key\":\"damagedealt\",\"oldValueJson\":\"0\","
			"\"newValueJson\":\"13\"}]}}}"));
		const FCrowdyInvokeResult R = FCrowdyGameApiCodec::ParseInvokeEnvelope(Env, true, TArray<FString>());
		TestTrue(TEXT("success: transport ok"), R.bTransportOk);
		TestTrue(TEXT("success: success true"), R.bSuccess);
		TestEqual(TEXT("success: returnValueJson"), R.ReturnValueJson, FString(TEXT("87")));
		if (TestEqual(TEXT("success: two mutations"), R.Mutations.Num(), 2))
		{
			TestEqual(TEXT("mutation key"), R.Mutations[0].Key, FString(TEXT("hp")));
			TestEqual(TEXT("mutation old"), R.Mutations[0].OldValueJson, FString(TEXT("100")));
			TestEqual(TEXT("mutation new"), R.Mutations[0].NewValueJson, FString(TEXT("87")));
			// The destination each write names. One invoke reports writes to TWO containers when an effect writes
			// source.<attr> alongside self.<attr>, and dropping this field is what routed the source's value onto
			// the target, where it was silently discarded.
			TestEqual(TEXT("mutation container id"), R.Mutations[0].ContainerId, FString(TEXT("c-victim")));
			TestEqual(TEXT("cross-container mutation key"), R.Mutations[1].Key, FString(TEXT("damagedealt")));
			TestEqual(TEXT("cross-container mutation container id"), R.Mutations[1].ContainerId,
				FString(TEXT("c-attacker")));
		}
	}

	// (2) logic/authority failure transport ok, success false, errorMessage set, NO mutations
	{
		const TSharedPtr<FJsonObject> Env = ParseObject(TEXT(
			"{\"data\":{\"gameModelInvoke\":{\"eventId\":\"e2\",\"functionName\":\"attack\",\"success\":false,"
			"\"returnValueJson\":null,\"errorMessage\":\"not your turn\",\"mutationsApplied\":[]}}}"));
		const FCrowdyInvokeResult R = FCrowdyGameApiCodec::ParseInvokeEnvelope(Env, true, TArray<FString>());
		TestTrue(TEXT("logic: transport ok"), R.bTransportOk);
		TestFalse(TEXT("logic: success false"), R.bSuccess);
		TestEqual(TEXT("logic: errorMessage"), R.ErrorMessage, FString(TEXT("not your turn")));
		TestEqual(TEXT("logic: no mutations"), R.Mutations.Num(), 0);
	}

	// (3) transport error errors[] present -> NOT transport ok, message surfaced
	{
		const TSharedPtr<FJsonObject> Env = ParseObject(TEXT("{\"errors\":[{\"message\":\"boom\"}],\"data\":null}"));
		TArray<FString> Errors;
		Errors.Add(TEXT("boom"));
		const FCrowdyInvokeResult R = FCrowdyGameApiCodec::ParseInvokeEnvelope(Env, true, Errors);
		TestFalse(TEXT("transport-error: not transport ok"), R.bTransportOk);
		TestFalse(TEXT("transport-error: not success"), R.bSuccess);
		TestEqual(TEXT("transport-error: message surfaced"), R.ErrorMessage, FString(TEXT("boom")));
	}

	// (4) http failure bHttpOk false -> NOT transport ok
	{
		const FCrowdyInvokeResult R = FCrowdyGameApiCodec::ParseInvokeEnvelope(nullptr, false, TArray<FString>());
		TestFalse(TEXT("http-failure: not transport ok"), R.bTransportOk);
	}

	return true;
}

// gameModelContainerState returns the visible properties as a JSON-ENCODED STRING (propertiesJson) nested
// in the envelope, not a raw JSON object the parser must decode that inner string. A null container
// (not found / not visible) resolves to false, not a crash.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelParseContainerStateTest,
	"CrowdySDK.GameModel.ParseContainerState", CrowdyGameApiTestFlags)
bool FCrowdyGameModelParseContainerStateTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Env = ParseObject(TEXT(
		"{\"data\":{\"gameModelContainerState\":{\"containerId\":\"c1\",\"typeName\":\"Character\","
		"\"ownerUserId\":\"90001\",\"propertiesJson\":\"{\\\"hp\\\":87,\\\"str\\\":10}\"}}}"));
	TSharedPtr<FJsonObject> State;
	const bool bOk = FCrowdyGameApiCodec::ParseContainerStateEnvelope(Env, true, TArray<FString>(), State);
	TestTrue(TEXT("state parsed"), bOk);
	if (State.IsValid())
	{
		double Hp = 0.0;
		TestTrue(TEXT("hp present"), State->TryGetNumberField(TEXT("hp"), Hp));
		TestEqual(TEXT("hp value"), static_cast<int32>(Hp), 87);
		double Str = 0.0;
		TestTrue(TEXT("str present"), State->TryGetNumberField(TEXT("str"), Str));
		TestEqual(TEXT("str value"), static_cast<int32>(Str), 10);
	}

	// null container -> false
	const TSharedPtr<FJsonObject> Env2 = ParseObject(TEXT("{\"data\":{\"gameModelContainerState\":null}}"));
	TSharedPtr<FJsonObject> S2;
	TestFalse(TEXT("null container -> false"),
		FCrowdyGameApiCodec::ParseContainerStateEnvelope(Env2, true, TArray<FString>(), S2));

	return true;
}

// The bulk read: the variables are top-level (appId a BigInt STRING, containerIds an array of strings with empty
// ids dropped), and the reply parses per row, keeping a row whose propertiesJson is unusable (null State) so the
// caller can still tell it apart from an id the server omitted, and dropping only a row with no containerId.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelParseContainerStatesTest,
	"CrowdySDK.GameModel.ParseContainerStates", CrowdyGameApiTestFlags)
bool FCrowdyGameModelParseContainerStatesTest::RunTest(const FString& Parameters)
{
	// Builder: appId is a string, containerIds an array of strings, the empty id dropped, duplicates kept.
	{
		TArray<FString> Ids;
		Ids.Add(TEXT("c1"));
		Ids.Add(FString());
		Ids.Add(TEXT("c2"));
		Ids.Add(TEXT("c1"));
		const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildContainerStatesVariables(7, Ids);
		if (!TestTrue(TEXT("variables built"), Vars.IsValid()))
		{
			return false;
		}
		TestFalse(TEXT("variables are top-level, not wrapped in input"), Vars->HasField(TEXT("input")));
		const TSharedPtr<FJsonValue> AppIdValue = Vars->TryGetField(TEXT("appId"));
		if (TestNotNull(TEXT("appId present"), AppIdValue.Get()))
		{
			TestEqual(TEXT("appId is a JSON string"), static_cast<int32>(AppIdValue->Type), static_cast<int32>(EJson::String));
			TestEqual(TEXT("appId string value"), AppIdValue->AsString(), FString(TEXT("7")));
		}
		const TSharedPtr<FJsonValue> IdsValue = Vars->TryGetField(TEXT("containerIds"));
		if (TestNotNull(TEXT("containerIds present"), IdsValue.Get()))
		{
			TestEqual(TEXT("containerIds is a JSON array"), static_cast<int32>(IdsValue->Type), static_cast<int32>(EJson::Array));
			const TArray<TSharedPtr<FJsonValue>>& Arr = IdsValue->AsArray();
			if (TestEqual(TEXT("empty id dropped, duplicate kept"), Arr.Num(), 3))
			{
				TestEqual(TEXT("containerIds[0] is a string"), static_cast<int32>(Arr[0]->Type), static_cast<int32>(EJson::String));
				TestEqual(TEXT("containerIds[0]"), Arr[0]->AsString(), FString(TEXT("c1")));
				TestEqual(TEXT("containerIds[1]"), Arr[1]->AsString(), FString(TEXT("c2")));
				TestEqual(TEXT("containerIds[2]"), Arr[2]->AsString(), FString(TEXT("c1")));
			}
		}
		TestEqual(TEXT("per-call limit"), FCrowdyGameApiCodec::MaxContainerStatesPerCall, 500);
	}

	// Parser: three rows; the second has a null owner and unparseable propertiesJson; the third has no containerId.
	{
		const TSharedPtr<FJsonObject> Env = ParseObject(TEXT(
			"{\"data\":{\"gameModelContainerStates\":["
			"{\"containerId\":\"c1\",\"appId\":\"7\",\"sessionId\":\"s1\",\"typeName\":\"Character\",\"displayName\":\"A\","
			"\"ownerUserId\":\"90001\",\"propertiesJson\":\"{\\\"hp\\\":87}\"},"
			"{\"containerId\":\"c2\",\"appId\":\"7\",\"sessionId\":null,\"typeName\":\"Landmark\",\"displayName\":\"B\","
			"\"ownerUserId\":null,\"propertiesJson\":\"not json\"},"
			"{\"appId\":\"7\",\"typeName\":\"Character\",\"ownerUserId\":\"90002\",\"propertiesJson\":\"{}\"}"
			"]}}"));
		TArray<FCrowdyGameApiCodec::FContainerStateRow> Rows;
		TestTrue(TEXT("rows parsed"), FCrowdyGameApiCodec::ParseContainerStatesEnvelope(Env, true, TArray<FString>(), Rows));
		if (TestEqual(TEXT("the row without a containerId is dropped"), Rows.Num(), 2))
		{
			TestEqual(TEXT("row 1 id"), Rows[0].ContainerId, FString(TEXT("c1")));
			TestEqual(TEXT("row 1 type"), Rows[0].TypeName, FString(TEXT("Character")));
			TestEqual(TEXT("row 1 session"), Rows[0].SessionId, FString(TEXT("s1")));
			TestEqual(TEXT("row 1 owner parsed from string"), Rows[0].OwnerUserId, static_cast<int64>(90001));
			if (TestTrue(TEXT("row 1 state decoded"), Rows[0].State.IsValid()))
			{
				double Hp = 0.0;
				TestTrue(TEXT("row 1 hp present"), Rows[0].State->TryGetNumberField(TEXT("hp"), Hp));
				TestEqual(TEXT("row 1 hp value"), static_cast<int32>(Hp), 87);
			}
			TestEqual(TEXT("row 2 id"), Rows[1].ContainerId, FString(TEXT("c2")));
			TestTrue(TEXT("row 2 app-scoped session is empty"), Rows[1].SessionId.IsEmpty());
			TestEqual(TEXT("row 2 null owner reads as 0"), Rows[1].OwnerUserId, static_cast<int64>(0));
			TestFalse(TEXT("row 2 unparseable propertiesJson leaves State null"), Rows[1].State.IsValid());
		}
	}

	// An empty list is a clean read of nothing, not a failure.
	{
		const TSharedPtr<FJsonObject> Env = ParseObject(TEXT("{\"data\":{\"gameModelContainerStates\":[]}}"));
		TArray<FCrowdyGameApiCodec::FContainerStateRow> Rows;
		TestTrue(TEXT("empty list parses"), FCrowdyGameApiCodec::ParseContainerStatesEnvelope(Env, true, TArray<FString>(), Rows));
		TestEqual(TEXT("no rows"), Rows.Num(), 0);
	}

	// Transport failure, a GraphQL error and a missing envelope each fail the read.
	{
		const TSharedPtr<FJsonObject> Env = ParseObject(TEXT("{\"data\":{\"gameModelContainerStates\":[{\"containerId\":\"c1\",\"propertiesJson\":\"{}\"}]}}"));
		TArray<FCrowdyGameApiCodec::FContainerStateRow> Rows;
		TestFalse(TEXT("transport failure -> false"), FCrowdyGameApiCodec::ParseContainerStatesEnvelope(Env, false, TArray<FString>(), Rows));
		TestEqual(TEXT("no rows on a transport failure"), Rows.Num(), 0);
		TArray<FString> Errors;
		Errors.Add(TEXT("boom"));
		TestFalse(TEXT("errors -> false"), FCrowdyGameApiCodec::ParseContainerStatesEnvelope(Env, true, Errors, Rows));
		TestFalse(TEXT("missing envelope -> false"), FCrowdyGameApiCodec::ParseContainerStatesEnvelope(nullptr, true, TArray<FString>(), Rows));
	}

	return true;
}

// The create-container request builder emits appId as a JSON string, NEVER writes ownerUserId (the server
// pins the owner to the caller -- fail-safe by omission, the whole trust argument for player creates), falls
// displayName back to the type name, and omits sessionId/metadataJson when empty. This guards the two
// load-bearing wire details of container creation: appId-as-string and owner-by-omission.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelCreateContainerBuildsRequestTest,
	"CrowdySDK.GameModel.CreateContainerBuildsRequest", CrowdyGameApiTestFlags)
bool FCrowdyGameModelCreateContainerBuildsRequestTest::RunTest(const FString& Parameters)
{
	// Minimal: appId + typeName, empty displayName/session/metadata.
	const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildCreateContainerVariables(
		7, TEXT("Hero"), FString(), FString(), FString());
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
		TestEqual(TEXT("appId string value"), AppIdValue->AsString(), FString(TEXT("7")));
	}

	FString TypeName;
	TestTrue(TEXT("typeName present"), Input->TryGetStringField(TEXT("typeName"), TypeName));
	TestEqual(TEXT("typeName"), TypeName, FString(TEXT("Hero")));

	// displayName falls back to the type name (the field is non-null on the server).
	FString DisplayName;
	TestTrue(TEXT("displayName present"), Input->TryGetStringField(TEXT("displayName"), DisplayName));
	TestEqual(TEXT("displayName defaults to typeName"), DisplayName, FString(TEXT("Hero")));

	// The trust guarantee: ownerUserId is NEVER written (server pins the caller).
	TestFalse(TEXT("ownerUserId omitted (server pins the caller)"), Input->HasField(TEXT("ownerUserId")));
	TestFalse(TEXT("sessionId omitted when empty"), Input->HasField(TEXT("sessionId")));
	TestFalse(TEXT("metadataJson omitted when empty"), Input->HasField(TEXT("metadataJson")));

	// Full: explicit displayName + session + metadata carried; still no ownerUserId.
	const TSharedPtr<FJsonObject> Vars2 = FCrowdyGameApiCodec::BuildCreateContainerVariables(
		7, TEXT("Hero"), TEXT("Aria"), TEXT("sess-1"), TEXT("{\"note\":\"x\"}"));
	const TSharedPtr<FJsonObject>* Input2Ptr = nullptr;
	Vars2->TryGetObjectField(TEXT("input"), Input2Ptr);
	FString D2, S2, M2;
	TestTrue(TEXT("displayName carried"), (*Input2Ptr)->TryGetStringField(TEXT("displayName"), D2));
	TestEqual(TEXT("displayName value"), D2, FString(TEXT("Aria")));
	TestTrue(TEXT("sessionId carried"), (*Input2Ptr)->TryGetStringField(TEXT("sessionId"), S2));
	TestEqual(TEXT("sessionId value"), S2, FString(TEXT("sess-1")));
	TestTrue(TEXT("metadataJson carried"), (*Input2Ptr)->TryGetStringField(TEXT("metadataJson"), M2));
	TestTrue(TEXT("metadataJson passed through verbatim"), M2.Contains(TEXT("note")));
	TestFalse(TEXT("ownerUserId still omitted"), (*Input2Ptr)->HasField(TEXT("ownerUserId")));

	return true;
}

// Create parsing reads containerId (+ ownerUserId as a BigInt string) and rejects a null/errored response so
// a caller never binds to an empty id.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelParseCreateContainerTest,
	"CrowdySDK.GameModel.ParseCreateContainer", CrowdyGameApiTestFlags)
bool FCrowdyGameModelParseCreateContainerTest::RunTest(const FString& Parameters)
{
	// success
	{
		const TSharedPtr<FJsonObject> Env = ParseObject(TEXT(
			"{\"data\":{\"gameModelCreateContainer\":{\"containerId\":\"c9\",\"ownerUserId\":\"90001\","
			"\"metadataJson\":\"{}\"}}}"));
		FString Id;
		int64 Owner = 0;
		const bool bOk = FCrowdyGameApiCodec::ParseCreateContainerEnvelope(Env, true, TArray<FString>(), Id, Owner);
		TestTrue(TEXT("create parsed"), bOk);
		TestEqual(TEXT("containerId"), Id, FString(TEXT("c9")));
		TestEqual(TEXT("ownerUserId pinned by server"), Owner, static_cast<int64>(90001));
	}

	// null container -> false
	{
		const TSharedPtr<FJsonObject> Env = ParseObject(TEXT("{\"data\":{\"gameModelCreateContainer\":null}}"));
		FString Id;
		int64 Owner = 0;
		TestFalse(TEXT("null -> false"), FCrowdyGameApiCodec::ParseCreateContainerEnvelope(Env, true, TArray<FString>(), Id, Owner));
	}

	// GraphQL error -> false
	{
		TArray<FString> Errors;
		Errors.Add(TEXT("not permitted"));
		FString Id;
		int64 Owner = 0;
		TestFalse(TEXT("errors -> false"), FCrowdyGameApiCodec::ParseCreateContainerEnvelope(nullptr, true, Errors, Id, Owner));
	}

	return true;
}

// Ensure builds the atomic get-or-create request: appId as a JSON string, the required bindingKey + displayName
// carried, session/metadata omitted when empty, and ownerUserId NEVER written (server pins the caller/null).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelEnsureContainerBuildsRequestTest,
	"CrowdySDK.GameModel.EnsureContainerBuildsRequest", CrowdyGameApiTestFlags)
bool FCrowdyGameModelEnsureContainerBuildsRequestTest::RunTest(const FString& Parameters)
{
	// Minimal: appId + typeName + bindingKey, empty displayName/session/metadata.
	const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildEnsureContainerVariables(
		7, TEXT("Hero"), TEXT("abc123"), FString(), FString(), FString());
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
		TestEqual(TEXT("appId string value"), AppIdValue->AsString(), FString(TEXT("7")));
	}

	FString TypeName, BindingKey;
	TestTrue(TEXT("typeName present"), Input->TryGetStringField(TEXT("typeName"), TypeName));
	TestEqual(TEXT("typeName"), TypeName, FString(TEXT("Hero")));
	TestTrue(TEXT("bindingKey present"), Input->TryGetStringField(TEXT("bindingKey"), BindingKey));
	TestEqual(TEXT("bindingKey"), BindingKey, FString(TEXT("abc123")));

	// displayName falls back to the type name (the field is non-null on the server).
	FString DisplayName;
	TestTrue(TEXT("displayName present"), Input->TryGetStringField(TEXT("displayName"), DisplayName));
	TestEqual(TEXT("displayName defaults to typeName"), DisplayName, FString(TEXT("Hero")));

	TestFalse(TEXT("ownerUserId omitted (server pins the caller/null)"), Input->HasField(TEXT("ownerUserId")));
	TestFalse(TEXT("sessionId omitted when empty"), Input->HasField(TEXT("sessionId")));
	TestFalse(TEXT("metadataJson omitted when empty"), Input->HasField(TEXT("metadataJson")));

	// Full: explicit displayName + session + metadata carried; still no ownerUserId.
	const TSharedPtr<FJsonObject> Vars2 = FCrowdyGameApiCodec::BuildEnsureContainerVariables(
		7, TEXT("Hero"), TEXT("abc123"), TEXT("Boss"), TEXT("sess-1"), TEXT("{\"k\":\"v\"}"));
	const TSharedPtr<FJsonObject>* Input2Ptr = nullptr;
	Vars2->TryGetObjectField(TEXT("input"), Input2Ptr);
	FString D2, S2, M2;
	TestTrue(TEXT("displayName carried"), (*Input2Ptr)->TryGetStringField(TEXT("displayName"), D2));
	TestEqual(TEXT("displayName value"), D2, FString(TEXT("Boss")));
	TestTrue(TEXT("sessionId carried"), (*Input2Ptr)->TryGetStringField(TEXT("sessionId"), S2));
	TestEqual(TEXT("sessionId value"), S2, FString(TEXT("sess-1")));
	TestTrue(TEXT("metadataJson carried"), (*Input2Ptr)->TryGetStringField(TEXT("metadataJson"), M2));
	TestFalse(TEXT("ownerUserId still omitted"), (*Input2Ptr)->HasField(TEXT("ownerUserId")));

	return true;
}

// Ensure parsing reads container.containerId (+ ownerUserId) and the created flag, and rejects a null/errored
// response so a caller never binds to an empty id.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelParseEnsureContainerTest,
	"CrowdySDK.GameModel.ParseEnsureContainer", CrowdyGameApiTestFlags)
bool FCrowdyGameModelParseEnsureContainerTest::RunTest(const FString& Parameters)
{
	// created:true
	{
		const TSharedPtr<FJsonObject> Env = ParseObject(TEXT(
			"{\"data\":{\"gameModelEnsureContainer\":{\"container\":{\"containerId\":\"c9\",\"ownerUserId\":\"90001\","
			"\"metadataJson\":\"{}\",\"bindingKey\":\"abc123\"},\"created\":true}}}"));
		FString Id;
		int64 Owner = 0;
		bool bCreated = false;
		const bool bOk = FCrowdyGameApiCodec::ParseEnsureContainerEnvelope(Env, true, TArray<FString>(), Id, Owner, bCreated);
		TestTrue(TEXT("ensure parsed"), bOk);
		TestEqual(TEXT("containerId"), Id, FString(TEXT("c9")));
		TestEqual(TEXT("ownerUserId"), Owner, static_cast<int64>(90001));
		TestTrue(TEXT("created flag true"), bCreated);
	}

	// created:false (resolved an existing row) - a shared/admin row can carry a null ownerUserId.
	{
		const TSharedPtr<FJsonObject> Env = ParseObject(TEXT(
			"{\"data\":{\"gameModelEnsureContainer\":{\"container\":{\"containerId\":\"c9\",\"ownerUserId\":null,"
			"\"metadataJson\":\"{}\",\"bindingKey\":\"abc123\"},\"created\":false}}}"));
		FString Id;
		int64 Owner = 0;
		bool bCreated = true;
		const bool bOk = FCrowdyGameApiCodec::ParseEnsureContainerEnvelope(Env, true, TArray<FString>(), Id, Owner, bCreated);
		TestTrue(TEXT("ensure parsed (existing)"), bOk);
		TestEqual(TEXT("containerId"), Id, FString(TEXT("c9")));
		TestEqual(TEXT("null owner -> 0"), Owner, static_cast<int64>(0));
		TestFalse(TEXT("created flag false"), bCreated);
	}

	// null container -> false
	{
		const TSharedPtr<FJsonObject> Env = ParseObject(TEXT(
			"{\"data\":{\"gameModelEnsureContainer\":{\"container\":null,\"created\":true}}}"));
		FString Id;
		int64 Owner = 0;
		bool bCreated = false;
		TestFalse(TEXT("null container -> false"),
			FCrowdyGameApiCodec::ParseEnsureContainerEnvelope(Env, true, TArray<FString>(), Id, Owner, bCreated));
	}

	// GraphQL error -> false (e.g. not permitted to create a not-yet-existing shared admin row)
	{
		TArray<FString> Errors;
		Errors.Add(TEXT("not permitted"));
		FString Id;
		int64 Owner = 0;
		bool bCreated = false;
		TestFalse(TEXT("errors -> false"),
			FCrowdyGameApiCodec::ParseEnsureContainerEnvelope(nullptr, true, Errors, Id, Owner, bCreated));
	}

	return true;
}

// The get-by-key read (a remote proxy path): appId as a JSON string, bindingKey always carried, typeName/session
// carried when present. The parser returns the single row (or a clean not-found) and rejects a transport error.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelReadByKeyTest,
	"CrowdySDK.GameModel.ReadContainerByKey", CrowdyGameApiTestFlags)
bool FCrowdyGameModelReadByKeyTest::RunTest(const FString& Parameters)
{
	// Builder: minimal (no session) carries bindingKey + typeName.
	{
		const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildReadContainerByKeyVariables(
			7, TEXT("Hero"), FString(), TEXT("abc123"));
		const TSharedPtr<FJsonValue> AppIdValue = Vars->TryGetField(TEXT("appId"));
		if (TestNotNull(TEXT("appId present"), AppIdValue.Get()))
		{
			TestEqual(TEXT("appId is a JSON string"), static_cast<int32>(AppIdValue->Type), static_cast<int32>(EJson::String));
		}
		FString BindingKey, TypeName;
		TestTrue(TEXT("bindingKey carried"), Vars->TryGetStringField(TEXT("bindingKey"), BindingKey));
		TestEqual(TEXT("bindingKey value"), BindingKey, FString(TEXT("abc123")));
		TestTrue(TEXT("typeName carried"), Vars->TryGetStringField(TEXT("typeName"), TypeName));
		TestFalse(TEXT("sessionId omitted when empty"), Vars->HasField(TEXT("sessionId")));
	}

	// Builder: with a session carries it.
	{
		const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildReadContainerByKeyVariables(
			7, TEXT("Hero"), TEXT("sess-1"), TEXT("abc123"));
		FString S;
		TestTrue(TEXT("sessionId carried"), Vars->TryGetStringField(TEXT("sessionId"), S));
		TestEqual(TEXT("sessionId value"), S, FString(TEXT("sess-1")));
	}

	// Parse: one row whose bindingKey matches -> found.
	{
		const TSharedPtr<FJsonObject> Env = ParseObject(TEXT(
			"{\"data\":{\"gameModelContainers\":[{\"containerId\":\"c9\",\"ownerUserId\":\"90001\",\"bindingKey\":\"abc123\"}]}}"));
		bool bFound = false;
		FString Id;
		int64 Owner = 0;
		const bool bOk = FCrowdyGameApiCodec::ParseReadContainerByKeyEnvelope(Env, true, TArray<FString>(), TEXT("abc123"), bFound, Id, Owner);
		TestTrue(TEXT("read ok"), bOk);
		TestTrue(TEXT("found"), bFound);
		TestEqual(TEXT("containerId"), Id, FString(TEXT("c9")));
		TestEqual(TEXT("ownerUserId"), Owner, static_cast<int64>(90001));
	}

	// Parse: a drifted server ignored the filter and returned a row with a DIFFERENT bindingKey -> not found (never
	// bind the wrong player's container).
	{
		const TSharedPtr<FJsonObject> Env = ParseObject(TEXT(
			"{\"data\":{\"gameModelContainers\":[{\"containerId\":\"c-other\",\"ownerUserId\":\"90002\",\"bindingKey\":\"zzz999\"}]}}"));
		bool bFound = true;
		FString Id;
		int64 Owner = 0;
		const bool bOk = FCrowdyGameApiCodec::ParseReadContainerByKeyEnvelope(Env, true, TArray<FString>(), TEXT("abc123"), bFound, Id, Owner);
		TestTrue(TEXT("read ok (mismatched key)"), bOk);
		TestFalse(TEXT("mismatched bindingKey not bound"), bFound);
		TestTrue(TEXT("no id on mismatch"), Id.IsEmpty());
	}

	// Parse: empty list -> ok but not found (a clean "owner has not created it yet").
	{
		const TSharedPtr<FJsonObject> Env = ParseObject(TEXT("{\"data\":{\"gameModelContainers\":[]}}"));
		bool bFound = true;
		FString Id;
		int64 Owner = 0;
		const bool bOk = FCrowdyGameApiCodec::ParseReadContainerByKeyEnvelope(Env, true, TArray<FString>(), TEXT("abc123"), bFound, Id, Owner);
		TestTrue(TEXT("read ok (empty)"), bOk);
		TestFalse(TEXT("not found"), bFound);
		TestTrue(TEXT("empty id"), Id.IsEmpty());
	}

	// Parse: transport error -> false.
	{
		TArray<FString> Errors;
		Errors.Add(TEXT("boom"));
		bool bFound = true;
		FString Id;
		int64 Owner = 0;
		TestFalse(TEXT("errors -> false"),
			FCrowdyGameApiCodec::ParseReadContainerByKeyEnvelope(nullptr, true, Errors, TEXT("abc123"), bFound, Id, Owner));
		TestFalse(TEXT("not found on error"), bFound);
	}

	return true;
}

// The retry decision: an in-band PlayerFaultInfo (fault{code, blame, retryable}) on a rolled-back invoke (success:
// false) is read into Blame/bRetryable. Platform + retryable:true is the one shape a caller may retry unchanged (an
// overload or a transient platform hiccup); Author is a guaranteed repeat failure and must come out NOT retryable
// even when the field is simply absent, since defaulting to true would retry an authored refusal forever.
//
// NOTE: the server's other retry-relevant channel is a THROWN GraphQL error carrying blame/retryable in
// errors[].extensions (the overload-refusal path, which arrives as a transport failure with no gameModelInvoke
// object at all). ParseInvokeEnvelope cannot see that today: TransportErrors is a flat TArray<FString> of error
// messages with no extensions carried alongside them (see the comment on the TransportErrors.Num() > 0 branch in
// FCrowdyGameApiCodec.cpp), so a thrown-channel fault always parses as Blame::Unknown / bRetryable false regardless
// of what the real server sent. That is pinned below as current, correct-for-today behavior, not as the desired
// end state; widening TransportErrors to carry extensions is a tracked follow-up.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelInvokeRetryableTest,
	"CrowdySDK.GameModel.InvokeRetryable", CrowdyGameApiTestFlags)
bool FCrowdyGameModelInvokeRetryableTest::RunTest(const FString& Parameters)
{
	// In-band success:false + fault{blame:PLATFORM, retryable:true} -> bRetryable true. Losing this would mean a
	// caller never retries a genuine platform overload, silently dropping the player's action.
	{
		const TSharedPtr<FJsonObject> Env = ParseObject(TEXT(
			"{\"data\":{\"gameModelInvoke\":{\"eventId\":\"e3\",\"functionName\":\"attack\",\"success\":false,"
			"\"returnValueJson\":null,\"errorMessage\":null,\"mutationsApplied\":[],"
			"\"fault\":{\"code\":\"OVERLOADED\",\"blame\":\"PLATFORM\",\"retryable\":true}}}}"));
		const FCrowdyInvokeResult R = FCrowdyGameApiCodec::ParseInvokeEnvelope(Env, true, TArray<FString>());
		TestTrue(TEXT("platform fault: transport ok"), R.bTransportOk);
		TestFalse(TEXT("platform fault: success false"), R.bSuccess);
		TestTrue(TEXT("platform fault: blame is Platform"), R.Blame == ECrowdyPlayerFaultBlame::Platform);
		TestTrue(TEXT("platform fault: retryable"), R.bRetryable);
		TestEqual(TEXT("platform fault: code carried"), R.FaultCode, FString(TEXT("OVERLOADED")));
	}

	// The mirror: in-band success:false + fault{blame:AUTHOR} (retryable omitted, so it keeps its false default) ->
	// NOT retryable. Retrying an author fault repeats the identical failure, so this is a caller-safety guard, not a
	// cosmetic distinction.
	{
		const TSharedPtr<FJsonObject> Env = ParseObject(TEXT(
			"{\"data\":{\"gameModelInvoke\":{\"eventId\":\"e4\",\"functionName\":\"attack\",\"success\":false,"
			"\"returnValueJson\":null,\"errorMessage\":\"not your turn\",\"mutationsApplied\":[],"
			"\"fault\":{\"code\":\"NOT_YOUR_TURN\",\"blame\":\"AUTHOR\"}}}}"));
		const FCrowdyInvokeResult R = FCrowdyGameApiCodec::ParseInvokeEnvelope(Env, true, TArray<FString>());
		TestTrue(TEXT("author fault: transport ok"), R.bTransportOk);
		TestFalse(TEXT("author fault: success false"), R.bSuccess);
		TestTrue(TEXT("author fault: blame is Author"), R.Blame == ECrowdyPlayerFaultBlame::Author);
		TestFalse(TEXT("author fault: NOT retryable"), R.bRetryable);
	}

	// No fault object at all (an older server, or a plain logic failure with no PlayerFaultInfo attached) leaves
	// Blame/bRetryable at their Unknown/false default rather than crashing or guessing.
	{
		const TSharedPtr<FJsonObject> Env = ParseObject(TEXT(
			"{\"data\":{\"gameModelInvoke\":{\"eventId\":\"e5\",\"functionName\":\"attack\",\"success\":false,"
			"\"returnValueJson\":null,\"errorMessage\":\"not your turn\",\"mutationsApplied\":[]}}}"));
		const FCrowdyInvokeResult R = FCrowdyGameApiCodec::ParseInvokeEnvelope(Env, true, TArray<FString>());
		TestTrue(TEXT("no fault: blame stays Unknown"), R.Blame == ECrowdyPlayerFaultBlame::Unknown);
		TestFalse(TEXT("no fault: NOT retryable"), R.bRetryable);
	}

	// The thrown-error / overload-refusal channel: a canned platform-refusal delivered as a GraphQL errors[] entry.
	// See the file-level note above -- TransportErrors carries only the message string today, so this pins the
	// current (gap-exposing) behavior: the codec cannot see blame/retryable on this channel and a caller must treat
	// EVERY thrown GraphQL error as non-retryable until extensions are plumbed through.
	{
		TArray<FString> Errors;
		Errors.Add(TEXT("server overloaded, try again"));
		const FCrowdyInvokeResult R = FCrowdyGameApiCodec::ParseInvokeEnvelope(nullptr, true, Errors);
		TestFalse(TEXT("thrown error: not transport ok"), R.bTransportOk);
		TestTrue(TEXT("thrown error: blame cannot be read from a flat message array, so it stays Unknown"),
			R.Blame == ECrowdyPlayerFaultBlame::Unknown);
		TestFalse(TEXT("thrown error: Unknown must never be treated as retryable"), R.bRetryable);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
