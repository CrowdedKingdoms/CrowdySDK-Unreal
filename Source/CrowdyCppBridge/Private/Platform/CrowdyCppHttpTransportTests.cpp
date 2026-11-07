#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && !NO_LOGGING

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Http.h"
#include "HttpConstants.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Misc/OutputDeviceRedirector.h"
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

	// Sets LogHttp for one test and puts the previous value back.
	struct FScopedHttpVerbosity
	{
		ELogVerbosity::Type Previous = LogHttp.GetVerbosity();

		explicit FScopedHttpVerbosity(const ELogVerbosity::Type Verbosity) { LogHttp.SetVerbosity(Verbosity); }
		~FScopedHttpVerbosity() { LogHttp.SetVerbosity(Previous); }
	};

	class FHttpWarningCount final : public FOutputDevice
	{
	public:
		std::atomic<int32> Lines{0};

		virtual void Serialize(const TCHAR*, const ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Category == LogHttp.GetCategoryName() && (Verbosity & ELogVerbosity::VerbosityMask) <= ELogVerbosity::Warning)
			{
				++Lines;
			}
		}

		virtual bool CanBeUsedOnAnyThread() const override { return true; }
	};

	// Nothing listens on port 1, so the connection is refused and curl reports the failure.
	int32 CountHttpWarningsFromAFailedRequest(const bool bThroughTransport)
	{
		FHttpWarningCount Count;
		GLog->AddOutputDevice(&Count);
		GLog->Flush();
		Count.Lines = 0;

		crowdy::graphql::HttpRequest Request;
		Request.method = "POST";
		Request.url = "https://127.0.0.1:1/graphql";
		Request.headers.emplace_back("Content-Type", "application/json");
		Request.body = "{}";
		Request.timeoutMs = 10000;

		bool bDone = false;
		if (bThroughTransport)
		{
			CrowdyCppTransport::MakeFHttpTransport()->sendAsync(Request,
				[&bDone](crowdy::graphql::HttpOutcome) { bDone = true; });
		}
		else
		{
			const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Direct = CrowdyCppTransport::BuildRequest(Request);
			Direct->OnProcessRequestComplete().BindLambda(
				[&bDone](FHttpRequestPtr, FHttpResponsePtr, bool) { bDone = true; });
			Direct->ProcessRequest();
		}

		const double Deadline = FPlatformTime::Seconds() + 15.0;
		while (!bDone && FPlatformTime::Seconds() < Deadline)
		{
			FHttpModule::Get().GetHttpManager().Tick(0.016f);
			FPlatformProcess::Sleep(0.016f);
		}

		GLog->Flush();
		GLog->RemoveOutputDevice(&Count);
		return Count.Lines;
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

// LogHttp stays quiet until the last of several overlapping requests ends, then goes back to what it was.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppQuietHttpLogOverlapsTest,
	"CrowdySDK.CrowdyCppBridge.QuietHttpLogOverlaps", CrowdyCppHttpTransportTestFlags)
bool FCrowdyCppQuietHttpLogOverlapsTest::RunTest(const FString& Parameters)
{
	const FScopedHttpVerbosity Restore(ELogVerbosity::Display);

	CrowdyCppTransport::BeginQuietHttpLog();
	TestEqual(TEXT("quiet with one request in flight"), LogHttp.GetVerbosity(), ELogVerbosity::Error);

	CrowdyCppTransport::BeginQuietHttpLog();
	CrowdyCppTransport::EndQuietHttpLog();
	TestEqual(TEXT("still quiet with one of two left"), LogHttp.GetVerbosity(), ELogVerbosity::Error);

	CrowdyCppTransport::EndQuietHttpLog();
	TestEqual(TEXT("back when the last one ends"), LogHttp.GetVerbosity(), ELogVerbosity::Display);
	return true;
}

// Each setting someone chose stands: VeryVerbose, anything quieter than Error, and a change made mid-flight.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppQuietHttpLogRespectsAChoiceTest,
	"CrowdySDK.CrowdyCppBridge.QuietHttpLogRespectsAChoice", CrowdyCppHttpTransportTestFlags)
bool FCrowdyCppQuietHttpLogRespectsAChoiceTest::RunTest(const FString& Parameters)
{
	const FScopedHttpVerbosity Restore(ELogVerbosity::VeryVerbose);

	CrowdyCppTransport::BeginQuietHttpLog();
	TestEqual(TEXT("VeryVerbose is left alone"), LogHttp.GetVerbosity(), ELogVerbosity::VeryVerbose);
	CrowdyCppTransport::EndQuietHttpLog();
	TestEqual(TEXT("and stays"), LogHttp.GetVerbosity(), ELogVerbosity::VeryVerbose);

	LogHttp.SetVerbosity(ELogVerbosity::Fatal);
	CrowdyCppTransport::BeginQuietHttpLog();
	TestEqual(TEXT("a quieter setting is not made louder"), LogHttp.GetVerbosity(), ELogVerbosity::Fatal);
	CrowdyCppTransport::EndQuietHttpLog();
	TestEqual(TEXT("and stays"), LogHttp.GetVerbosity(), ELogVerbosity::Fatal);

	LogHttp.SetVerbosity(ELogVerbosity::Display);
	CrowdyCppTransport::BeginQuietHttpLog();
	LogHttp.SetVerbosity(ELogVerbosity::Verbose);
	CrowdyCppTransport::EndQuietHttpLog();
	TestEqual(TEXT("a change made while quiet wins"), LogHttp.GetVerbosity(), ELogVerbosity::Verbose);
	return true;
}

// A failed start can end a request twice, once by itself and once through its completion, and it may only count once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppQuietHttpLogCountsAFailedStartOnceTest,
	"CrowdySDK.CrowdyCppBridge.QuietHttpLogCountsAFailedStartOnce", CrowdyCppHttpTransportTestFlags)
bool FCrowdyCppQuietHttpLogCountsAFailedStartOnceTest::RunTest(const FString& Parameters)
{
	const FScopedHttpVerbosity Restore(ELogVerbosity::Display);

	crowdy::graphql::HttpRequest Request;
	Request.method = "POST";
	Request.headers.emplace_back("Content-Type", "application/json");
	Request.body = "{}";

	int32 Outcomes = 0;
	CrowdyCppTransport::MakeFHttpTransport()->sendAsync(Request, [&Outcomes](crowdy::graphql::HttpOutcome) { ++Outcomes; });
	for (int32 Tick = 0; Tick < 10; ++Tick)
	{
		FHttpModule::Get().GetHttpManager().Tick(0.016f);
	}
	TestTrue(TEXT("a request with no address fails"), Outcomes > 0);

	CrowdyCppTransport::BeginQuietHttpLog();
	CrowdyCppTransport::BeginQuietHttpLog();
	CrowdyCppTransport::EndQuietHttpLog();
	TestEqual(TEXT("one of two left is still quiet"), LogHttp.GetVerbosity(), ELogVerbosity::Error);
	CrowdyCppTransport::EndQuietHttpLog();
	TestEqual(TEXT("and the last one puts it back"), LogHttp.GetVerbosity(), ELogVerbosity::Display);
	return true;
}

// A real failed request. Sent straight to the engine it is reported, which shows the count can see those lines. Through
// the transport it is not, which holds only while the engine prints before the completion runs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCppFailedRequestStaysOutOfTheLogTest,
	"CrowdySDK.CrowdyCppBridge.FailedRequestStaysOutOfTheLog", CrowdyCppHttpTransportTestFlags)
bool FCrowdyCppFailedRequestStaysOutOfTheLogTest::RunTest(const FString& Parameters)
{
	const FScopedHttpVerbosity Restore(ELogVerbosity::Display);

	AddExpectedMessagePlain(TEXT("HTTP request to https://127.0.0.1:1/graphql failed"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);

	TestTrue(TEXT("sent straight to the engine it is reported"), CountHttpWarningsFromAFailedRequest(false) > 0);
	TestEqual(TEXT("through the transport it is not"), CountHttpWarningsFromAFailedRequest(true), 0);
	TestEqual(TEXT("and LogHttp is back afterwards"), LogHttp.GetVerbosity(), ELogVerbosity::Display);
	return true;
}

#endif
