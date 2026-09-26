#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"
#include "HttpConstants.h"
#include "Interfaces/IHttpRequest.h"
#include "Platform/CrowdyCppHttpTransport.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyCppHttpTransportTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	crowdy::graphql::HttpRequest CrowdyCppTestRequest()
	{
		crowdy::graphql::HttpRequest Request;
		Request.method = "POST";
		Request.url = "https://example.invalid/graphql";
		Request.headers.emplace_back("Content-Type", "application/json");
		Request.body = "{}";
		Request.timeoutMs = 2500;
		return Request;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppHttpTransportAsksForHttp2Test,
	"CrowdySDK.CrowdyCppBridge.HttpTransportAsksForHttp2", CrowdyCppHttpTransportTestFlags)
bool FCrowdyCppHttpTransportAsksForHttp2Test::RunTest(const FString& Parameters)
{
	IConsoleVariable* const Cvar = IConsoleManager::Get().FindConsoleVariable(TEXT("crowdy.net.http2"));
	if (!TestNotNull(TEXT("crowdy.net.http2 exists"), Cvar))
	{
		return false;
	}
	const int32 Saved = Cvar->GetInt();
	const EConsoleVariableFlags Priority = static_cast<EConsoleVariableFlags>(Cvar->GetFlags() & ECVF_SetByMask);
	if (Priority == ECVF_SetByConstructor)
	{
		TestEqual(TEXT("on by default"), Saved, 1);
	}

	Cvar->Set(1, Priority);
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> On = CrowdyCppTransport::BuildRequest(CrowdyCppTestRequest());
	TestEqual(TEXT("switched on, a built request asks for HTTP/2 over TLS"), On->GetOption(HttpRequestOptions::HttpVersion),
		FString(FHttpConstants::VERSION_2TLS));
	TestEqual(TEXT("verb"), On->GetVerb(), FString(TEXT("POST")));
	TestEqual(TEXT("url"), On->GetURL(), FString(TEXT("https://example.invalid/graphql")));
	TestEqual(TEXT("header"), On->GetHeader(TEXT("Content-Type")), FString(TEXT("application/json")));
	TestEqual(TEXT("body bytes"), On->GetContent().Num(), 2);
	TestEqual(TEXT("timeout in seconds"), On->GetTimeout().Get(0.f), 2.5f);

	Cvar->Set(0, Priority);
	TestEqual(TEXT("switched off, a built request keeps the engine's HTTP version"),
		CrowdyCppTransport::BuildRequest(CrowdyCppTestRequest())->GetOption(HttpRequestOptions::HttpVersion), FString());

	Cvar->Set(Saved, Priority);
	TestEqual(TEXT("the switch is back to its value"), Cvar->GetInt(), Saved);
	TestEqual(TEXT("and to its priority"), static_cast<int32>(Cvar->GetFlags() & ECVF_SetByMask), static_cast<int32>(Priority));
	return true;
}

#endif
