#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/CrowdyGameModelTestSupport.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyBulkResolveTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// The list variables name the type, always bound the page, and omit an empty session (the server reads that as
// every scope, which is why the rows are filtered again on the way back).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBulkResolveListVariablesTest,
	"CrowdySDK.GameModel.BulkResolveListVariables", CrowdyBulkResolveTestFlags)
bool FCrowdyBulkResolveListVariablesTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> AppScope = FCrowdyGameApiCodec::BuildListContainersByTypeVariables(42, TEXT("Node"), FString(), 200, 400);
	TestEqual(TEXT("appId is a string"), AppScope->GetStringField(TEXT("appId")), FString(TEXT("42")));
	TestEqual(TEXT("typeName"), AppScope->GetStringField(TEXT("typeName")), FString(TEXT("Node")));
	TestFalse(TEXT("an empty session is omitted"), AppScope->HasField(TEXT("sessionId")));
	TestEqual(TEXT("limit is a number"), AppScope->GetNumberField(TEXT("limit")), 200.0);
	TestEqual(TEXT("offset is a number"), AppScope->GetNumberField(TEXT("offset")), 400.0);

	const TSharedPtr<FJsonObject> Scoped = FCrowdyGameApiCodec::BuildListContainersByTypeVariables(42, TEXT("Node"), TEXT("s1"), 0, -5);
	TestEqual(TEXT("a session is sent"), Scoped->GetStringField(TEXT("sessionId")), FString(TEXT("s1")));
	TestEqual(TEXT("a zero limit is still bounded"), Scoped->GetNumberField(TEXT("limit")), 1.0);
	TestEqual(TEXT("a negative offset is clamped"), Scoped->GetNumberField(TEXT("offset")), 0.0);

	// The server refuses a limit above 1,000, so the builder clamps rather than let the page be refused.
	TestEqual(TEXT("the page cap is 1000"), FCrowdyGameApiCodec::MaxContainersPerPage, 1000);
	const TSharedPtr<FJsonObject> Oversized = FCrowdyGameApiCodec::BuildListContainersByTypeVariables(42, TEXT("Node"), FString(), 5000, 0);
	TestEqual(TEXT("an oversized limit is clamped to 1000"), Oversized->GetNumberField(TEXT("limit")), 1000.0);
	const TSharedPtr<FJsonObject> AtCap = FCrowdyGameApiCodec::BuildListContainersByTypeVariables(42, TEXT("Node"), FString(), 1000, 0);
	TestEqual(TEXT("a limit at the cap is sent as is"), AtCap->GetNumberField(TEXT("limit")), 1000.0);

	const TSharedPtr<FJsonObject> AnyType = FCrowdyGameApiCodec::BuildListContainersByTypeVariables(42, FString(), FString(), 1000, 0);
	TestFalse(TEXT("an empty type is omitted, which lists every type"), AnyType->HasField(TEXT("typeName")));
	return true;
}

// A page parses into typed rows; a row without an id or a key is dropped; a broken envelope is a failed read.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBulkResolveParseRowsTest,
	"CrowdySDK.GameModel.BulkResolveParseRows", CrowdyBulkResolveTestFlags)
bool FCrowdyBulkResolveParseRowsTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Env = ParseObject(TEXT(
		"{\"data\":{\"gameModelContainers\":["
		"{\"containerId\":\"c1\",\"typeName\":\"Node\",\"bindingKey\":\"k1\",\"sessionId\":\"\",\"ownerUserId\":\"77\"},"
		"{\"containerId\":\"c2\",\"typeName\":\"Node\",\"bindingKey\":\"k2\",\"sessionId\":\"s1\",\"ownerUserId\":null},"
		"{\"containerId\":\"\",\"bindingKey\":\"k3\"},"
		"{\"containerId\":\"c4\"}"
		"]}}"));
	TArray<FCrowdyGameApiCodec::FContainerRow> Rows;
	TestTrue(TEXT("parses"), FCrowdyGameApiCodec::ParseContainerRowsEnvelope(Env, true, TArray<FString>(), Rows));
	TestEqual(TEXT("two usable rows"), Rows.Num(), 2);
	if (Rows.Num() == 2)
	{
		TestEqual(TEXT("row 1 id"), Rows[0].ContainerId, FString(TEXT("c1")));
		TestEqual(TEXT("row 1 key"), Rows[0].BindingKey, FString(TEXT("k1")));
		TestEqual(TEXT("row 1 owner"), Rows[0].OwnerUserId, static_cast<int64>(77));
		TestTrue(TEXT("row 1 is app-global"), Rows[0].SessionId.IsEmpty());
		TestEqual(TEXT("row 2 session"), Rows[1].SessionId, FString(TEXT("s1")));
		TestEqual(TEXT("row 2 owner null reads as zero"), Rows[1].OwnerUserId, static_cast<int64>(0));
	}

	TArray<FString> Errors;
	Errors.Add(TEXT("boom"));
	TestFalse(TEXT("a transport error is a failed read"), FCrowdyGameApiCodec::ParseContainerRowsEnvelope(Env, true, Errors, Rows));
	TestFalse(TEXT("a missing envelope is a failed read"), FCrowdyGameApiCodec::ParseContainerRowsEnvelope(nullptr, true, TArray<FString>(), Rows));
	return true;
}

#if WITH_METADATA
// The scope is read off the class declaration: CrowdyScope="App" is app-scoped, an absent tag is not, and a subclass
// that restates nothing follows the nearest base that declares a scope.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContainerScopeReadsClassMetaTest,
	"CrowdySDK.GameModel.ContainerScopeReadsClassMeta", CrowdyBulkResolveTestFlags)
bool FCrowdyContainerScopeReadsClassMetaTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("CrowdyScope=App reads as app-scoped"),
		FCrowdyAttributeRegistry::IsContainerAppScoped(UCrowdyGameModelAppScopedTarget::StaticClass()));
	TestTrue(TEXT("a subclass with no scope tag of its own follows its app-scoped base"),
		FCrowdyAttributeRegistry::IsContainerAppScoped(UCrowdyGameModelAppScopedChildTarget::StaticClass()));
	TestFalse(TEXT("a container with no scope tag reads as session-scoped"),
		FCrowdyAttributeRegistry::IsContainerAppScoped(UCrowdyGameModelTestComponent::StaticClass()));
	TestFalse(TEXT("a null class reads as session-scoped"), FCrowdyAttributeRegistry::IsContainerAppScoped(nullptr));
	return true;
}
#endif

#endif
