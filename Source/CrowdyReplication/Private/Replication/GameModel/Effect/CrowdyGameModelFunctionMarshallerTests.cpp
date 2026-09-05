// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Replication/GameModel/Effect/CrowdyGameModelFunctionInput.h"
#include "Replication/GameModel/Effect/CrowdyGameModelFunctionMarshaller.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyMarshallerTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Serialize an upsert input to the same condensed JSON the Studio callers hand to the GraphQL client. Field order is
	// the marshaller's insertion order, which the callers rely on.
	FString ToCondensedJson(const TSharedPtr<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		Writer->Close();
		return Out;
	}
}

// A fully populated function input exercises every top-level branch: an owned string kept, params with and without the
// optional default/description, a mutation, and a preserved notification with an arg. AppId must serialize as a string.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelFunctionMarshallerFullGoldenTest,
	"CrowdySDK.GameModel.FunctionMarshallerFullGolden", CrowdyMarshallerTestFlags)
bool FCrowdyGameModelFunctionMarshallerFullGoldenTest::RunTest(const FString& Parameters)
{
	FCrowdyGameModelFunctionInput Fn;
	Fn.Name = TEXT("heal");
	Fn.ContainerTypeName = TEXT("Hero");
	Fn.InvokeScope = TEXT("player");
	Fn.bAutonomousInvocable = false;
	Fn.Description = TEXT("Restores health");
	Fn.ReturnType = TEXT("");
	Fn.ReturnExpression = TEXT("");
	Fn.InvokePolicyJson = TEXT("{\"type\":\"any\"}");

	FCrowdyGameModelFunctionParam Amount;
	Amount.Name = TEXT("amount");
	Amount.ValueType = TEXT("int");
	Amount.bRequired = true;
	Amount.SortOrder = 0;
	Fn.Parameters.Add(Amount);

	FCrowdyGameModelFunctionParam Note;
	Note.Name = TEXT("note");
	Note.ValueType = TEXT("string");
	Note.bRequired = false;
	Note.DefaultValueJson = TEXT("\"hi\"");
	Note.Description = TEXT("a note");
	Note.SortOrder = 1;
	Fn.Parameters.Add(Note);

	FCrowdyGameModelMutation Mutation;
	Mutation.Target = TEXT("self");
	Mutation.Property = TEXT("hp");
	Mutation.Expression = TEXT("clamp(self.hp + $amount, 0, 100)");
	Fn.Mutations.Add(Mutation);

	FCrowdyGameModelNotification Notification;
	Notification.Kind = TEXT("channel");
	Notification.EmitAs = TEXT("model_changed");
	FCrowdyGameModelNotificationArg Arg;
	Arg.Name = TEXT("channelId");
	Arg.Expression = TEXT("$self_container_id");
	Notification.Args.Add(Arg);
	Fn.Notifications.Add(Notification);

	const TSharedPtr<FJsonObject> Input = CrowdyGameModelMarshalling::BuildFunctionUpsertInput(Fn, 12345);
	if (!TestNotNull(TEXT("input built"), Input.Get()))
	{
		return false;
	}

	const FString Expected =
		TEXT("{\"appId\":\"12345\",\"name\":\"heal\",\"containerTypeName\":\"Hero\",\"invokeScope\":\"player\",")
		TEXT("\"autonomousInvocable\":false,\"description\":\"Restores health\",\"returnType\":null,")
		TEXT("\"returnExpression\":null,\"invokePolicyJson\":\"{\\\"type\\\":\\\"any\\\"}\",")
		TEXT("\"parameters\":[{\"name\":\"amount\",\"valueType\":\"int\",\"required\":true,\"sortOrder\":0},")
		TEXT("{\"name\":\"note\",\"valueType\":\"string\",\"required\":false,\"defaultValueJson\":\"\\\"hi\\\"\",")
		TEXT("\"description\":\"a note\",\"sortOrder\":1}],")
		TEXT("\"mutations\":[{\"target\":\"self\",\"property\":\"hp\",\"expression\":\"clamp(self.hp + $amount, 0, 100)\"}],")
		TEXT("\"notifications\":[{\"kind\":\"channel\",\"emitAs\":\"model_changed\",")
		TEXT("\"args\":[{\"name\":\"channelId\",\"expression\":\"$self_container_id\"}]}]}");

	TestEqual(TEXT("condensed JSON matches golden"), ToCondensedJson(Input), Expected);

	// AppId is a BigInt string, never a JSON number.
	TestTrue(TEXT("appId is a string field"), Input->HasTypedField<EJson::String>(TEXT("appId")));
	return true;
}

// The empty case locks the owned-field-null and omission behavior: empty owned strings emit explicit null; empty
// optional container/scope are dropped; empty parameter/mutation arrays emit as []; no notifications means the key is
// omitted entirely.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelFunctionMarshallerMinimalGoldenTest,
	"CrowdySDK.GameModel.FunctionMarshallerMinimalGolden", CrowdyMarshallerTestFlags)
bool FCrowdyGameModelFunctionMarshallerMinimalGoldenTest::RunTest(const FString& Parameters)
{
	FCrowdyGameModelFunctionInput Fn;
	Fn.Name = TEXT("poke");
	Fn.ContainerTypeName = TEXT("");
	Fn.InvokeScope = TEXT("");
	Fn.bAutonomousInvocable = false;
	Fn.Description = TEXT("");
	Fn.ReturnType = TEXT("");
	Fn.ReturnExpression = TEXT("");
	Fn.InvokePolicyJson = TEXT("");

	const TSharedPtr<FJsonObject> Input = CrowdyGameModelMarshalling::BuildFunctionUpsertInput(Fn, 7);
	if (!TestNotNull(TEXT("input built"), Input.Get()))
	{
		return false;
	}

	const FString Expected =
		TEXT("{\"appId\":\"7\",\"name\":\"poke\",\"autonomousInvocable\":false,\"description\":null,")
		TEXT("\"returnType\":null,\"returnExpression\":null,\"invokePolicyJson\":null,")
		TEXT("\"parameters\":[],\"mutations\":[]}");

	TestEqual(TEXT("condensed JSON matches golden"), ToCondensedJson(Input), Expected);
	TestFalse(TEXT("containerTypeName omitted"), Input->HasField(TEXT("containerTypeName")));
	TestFalse(TEXT("invokeScope omitted"), Input->HasField(TEXT("invokeScope")));
	TestFalse(TEXT("notifications omitted"), Input->HasField(TEXT("notifications")));
	return true;
}

#endif
