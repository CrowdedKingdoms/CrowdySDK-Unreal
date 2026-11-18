#include "Subsystem/CrowdyStudioAgent.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "CrowdyCppClient.h"
#include "CrowdyStudioAgentTestTypes.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystem/CrowdyAuthentication.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include <type_traits>

namespace CrowdyStudioAgentTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
	constexpr int64 AgentTestApp = 31337;
	constexpr int64 AgentTestUser = 7;
	constexpr int32 AgentIdlePolls = 16;

	const TCHAR* const AgentModelList = TEXT("{\"object\":\"list\",\"data\":["
		"{\"id\":\"openai/gpt-oss-120b\",\"name\":\"GPT OSS 120B\",\"context_window\":null,\"input_modalities\":[\"text\"]},"
		"{\"id\":\"vision/model\",\"name\":\"Vision\",\"context_window\":131072,\"input_modalities\":[\"text\",\"image\"]}]}");

	FCrowdyCppClientConfig AgentClientConfig(const TCHAR* ApiUrl = TEXT("https://game.test"))
	{
		FCrowdyCppClientConfig Config;
		Config.ApiUrl = ApiUrl;
		return Config;
	}

	FString AgentErrorBody(const TCHAR* Code)
	{
		return FString::Printf(TEXT("{\"errors\":[{\"message\":\"No.\",\"extensions\":{\"code\":\"%s\"}}],\"data\":null}"), Code);
	}

	FString AgentModelError(const TCHAR* Code)
	{
		return FString::Printf(TEXT("{\"error\":{\"message\":\"Refused.\",\"code\":\"%s\"}}"), Code);
	}

	FString AgentConsentBody(const TCHAR* Field, const TCHAR* Consented, const TCHAR* ConsentedAt, const TCHAR* AppId = TEXT("\"31337\""))
	{
		return FString::Printf(TEXT("{\"data\":{\"%s\":{\"appId\":%s,\"consented\":%s,\"consentedAt\":%s}}}"), Field, AppId, Consented, ConsentedAt);
	}

	FString AgentUsageBody(const TCHAR* Payer, const TCHAR* Requests, const TCHAR* Charge, const TCHAR* Limit)
	{
		return FString::Printf(TEXT("{\"data\":{\"crowdyStudioModelUsage\":{\"appId\":\"31337\",\"payerKind\":\"%s\",\"todayRequests\":%s,\"todayChargeMicrousd\":%s,")
			TEXT("\"dayLimitMicrousd\":%s,\"recent\":[]}}}"), Payer, Requests, Charge, Limit);
	}

	FString AgentLiveBody(const TCHAR* Verdict)
	{
		return FString::Printf(TEXT("{\"data\":{\"crowdyStudioLiveModding\":%s}}"), Verdict);
	}

	int64 AgentTimeMs(int32 Year, int32 Month, int32 Day, int32 Hour, int32 Millisecond)
	{
		return (FDateTime(Year, Month, Day, Hour, 0, 0, Millisecond) - FDateTime(1970, 1, 1)).GetTicks() / ETimespan::TicksPerMillisecond;
	}

	struct FAgentTokenEvent
	{
		int64 UserId = 0;
		FString Token;
	};

	FString AgentLinksBody(const TCHAR* Origin, const TCHAR* Wallet)
	{
		return FString::Printf(TEXT("{\"data\":{\"crowdyStudioLinks\":{\"origin\":\"%s\",\"walletUrl\":\"%s\"}}}"), Origin, Wallet);
	}

	struct FAgentRig
	{
		TStrongObjectPtr<UGameInstance> Instance;
		TStrongObjectPtr<UCrowdyGameSession> Session;
		TStrongObjectPtr<UCrowdyAuthentication> Auth;
		TStrongObjectPtr<UCrowdyStudioAgent> Agent;
		TSharedPtr<FCrowdyCppClient> Client;
		int32 Answers = 0;
		FCrowdyStudioAgentBoot Boot;
		FCrowdyStudioConsent Consent;
		FCrowdyStudioModelUsage Usage;
		FCrowdyStudioLiveModding Live;
		TArray<FAgentTokenEvent> TokenEvents;
		/** Every request's URL, in the order sent. */
		TArray<FString> Urls;

		explicit FAgentRig(const FString& Body, int32 Status = 200, bool bSignedIn = true, const TCHAR* ApiUrl = TEXT("https://game.test"))
			: Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient))
			, Session(NewObject<UCrowdyGameSession>(Instance.Get()))
			, Auth(NewObject<UCrowdyAuthentication>(Instance.Get()))
			, Agent(NewObject<UCrowdyStudioAgent>(Instance.Get()))
			, Client(FCrowdyCppClient::MakeForTest(Body, Status, AgentClientConfig(ApiUrl)))
		{
			Session->SetAppID(AgentTestApp);
			Session->SetUserID(AgentTestUser);
			if (bSignedIn)
			{
				Session->SetGameToken(TEXT("app-token"));
				Session->SetSessionToken(TEXT("session-token"));
				Client->SetGameToken(TEXT("app-token"));
			}
			Auth->InitializeForTest(Client, Session.Get());
			Agent->InitializeForTest(Client, Session.Get(), Auth.Get());
			Agent->OnAgentTokenChanged.AddLambda([this](int64 UserId, const FString& Token)
			{
				FAgentTokenEvent& Event = TokenEvents.AddDefaulted_GetRef();
				Event.UserId = UserId;
				Event.Token = Token;
			});
			Client->SetTestOnRequest([this](const FString& Url) { Urls.Add(Url); });
		}

		~FAgentRig()
		{
			Client->SetTestOnRequest(nullptr);
		}

		void Script(TArray<TPair<int32, FString>> Responses)
		{
			Client->SetTestResponseScript(MoveTemp(Responses));
		}

		bool PumpUntil(TFunctionRef<bool()> Done, int32 Steps = 400)
		{
			for (int32 Step = 0; Step < Steps && !Done(); ++Step)
			{
				Client->Poll();
				FTSTicker::GetCoreTicker().Tick(0.f);
			}
			return Done();
		}

		void Pump()
		{
			PumpUntil([this]() { return Answers > 0; });
		}

		void Idle()
		{
			for (int32 Tick = 0; Tick < AgentIdlePolls; ++Tick)
			{
				Client->Poll();
				FTSTicker::GetCoreTicker().Tick(0.f);
			}
		}

		void GetBoot()
		{
			Answers = 0;
			Agent->GetAgentBoot([this](const FCrowdyStudioAgentBoot& Answer) { Boot = Answer; ++Answers; });
			Pump();
		}

		void GetConsent()
		{
			Answers = 0;
			Agent->GetProviderConsent([this](const FCrowdyStudioConsent& Answer) { Consent = Answer; ++Answers; });
			Pump();
		}

		void RecordConsent(bool bConsented)
		{
			Answers = 0;
			Agent->RecordProviderConsent([this](const FCrowdyStudioConsent& Answer) { Consent = Answer; ++Answers; }, bConsented);
			Pump();
		}

		void GetUsage()
		{
			Answers = 0;
			Agent->GetModelUsage([this](const FCrowdyStudioModelUsage& Answer) { Usage = Answer; ++Answers; });
			Pump();
		}

		void Authorize(ECrowdyStudioRunMode Action)
		{
			Answers = 0;
			Agent->AuthorizeLiveModding(Action, [this](const FCrowdyStudioLiveModding& Answer) { Live = Answer; ++Answers; });
			Pump();
		}

		void ApplyToken(const TCHAR* Token, bool bRefresh)
		{
			FCrowdyAppTokenFields Fields;
			Fields.AppToken = Token;
			Auth->ApplyAppTokenForTest(Fields, bRefresh, nullptr);
		}

		bool SentAnything() const
		{
			FString Url;
			FString Authorization;
			return Client->GetLastTestRequest(Url, Authorization);
		}

		FString SentUrl() const
		{
			FString Url;
			FString Authorization;
			Client->GetLastTestRequest(Url, Authorization);
			return Url;
		}

		FString SentAuthorization() const
		{
			FString Url;
			FString Authorization;
			Client->GetLastTestRequest(Url, Authorization);
			return Authorization;
		}

		TSharedPtr<FJsonObject> SentRequest() const
		{
			FString Body;
			TSharedPtr<FJsonObject> Request;
			if (!Client->GetLastTestRequestBody(Body))
			{
				return nullptr;
			}
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), Request);
			return Request;
		}

		FString SentOperation() const
		{
			const TSharedPtr<FJsonObject> Request = SentRequest();
			FString Name;
			if (Request.IsValid())
			{
				Request->TryGetStringField(TEXT("operationName"), Name);
			}
			return Name;
		}

		TSharedPtr<FJsonObject> SentVariables() const
		{
			const TSharedPtr<FJsonObject> Request = SentRequest();
			const TSharedPtr<FJsonObject>* Variables = nullptr;
			return Request.IsValid() && Request->TryGetObjectField(TEXT("variables"), Variables) ? *Variables : nullptr;
		}
	};
}

using namespace CrowdyStudioAgentTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAgentBootTest, "CrowdySDK.CrowdyServices.StudioAgent.BootFillsEveryFieldAndTheModels", Flags)
bool FCrowdyStudioAgentBootTest::RunTest(const FString& Parameters)
{
	FAgentRig Rig(AgentModelList);
	Rig.Script({{200, AgentModelList}, {200, AgentLinksBody(TEXT("https://studio.test"), TEXT("https://studio.test/account/wallet"))}});
	Rig.GetBoot();
	if (!TestEqual(TEXT("the boot is answered once"), Rig.Answers, 1))
	{
		return false;
	}
	const FCrowdyStudioAgentBoot& Boot = Rig.Boot;
	TestEqual(TEXT("the boot succeeds"), Boot.Result, ECrowdyStudioResult::Success);
	TestEqual(TEXT("the app id is the session's"), Boot.AppId, AgentTestApp);
	TestEqual(TEXT("the GraphQL URL is the client's endpoint"), Boot.GraphQLUrl, Rig.Client->GetApiEndpoint());
	TestTrue(TEXT("and it is a GraphQL path"), Boot.GraphQLUrl.EndsWith(TEXT("/graphql")));
	TestEqual(TEXT("the model base URL is that URL's origin"), Boot.ModelBaseUrl, FString(TEXT("https://game.test")));
	TestTrue(TEXT("the page may reach the API origin and nothing else"), Boot.ConnectOrigins == TArray<FString>({TEXT("https://game.test")}));
	TestEqual(TEXT("the Studio origin comes from the platform"), Boot.StudioOrigin, FString(TEXT("https://studio.test")));
	TestEqual(TEXT("the wallet URL comes from the platform"), Boot.WalletUrl, FString(TEXT("https://studio.test/account/wallet")));
	if (TestEqual(TEXT("both models are listed"), Boot.Models.Num(), 2))
	{
		TestEqual(TEXT("first id"), Boot.Models[0].ModelId, FString(TEXT("openai/gpt-oss-120b")));
		TestEqual(TEXT("first name"), Boot.Models[0].Name, FString(TEXT("GPT OSS 120B")));
		TestEqual(TEXT("an unstated window is 0"), Boot.Models[0].ContextWindow, static_cast<int64>(0));
		TestEqual(TEXT("a stated window is kept"), Boot.Models[1].ContextWindow, static_cast<int64>(131072));
		TestTrue(TEXT("modalities are kept"), Boot.Models[1].InputModalities == TArray<FString>({TEXT("text"), TEXT("image")}));
	}
	TestTrue(TEXT("the list is asked for this app first"), Rig.Urls.Num() == 2 && Rig.Urls[0] == TEXT("https://game.test/v1/model/models?appId=31337"));
	TestEqual(TEXT("then the Studio links"), Rig.SentOperation(), FString(TEXT("CrowdyStudioLinks")));
	TestEqual(TEXT("under the app token, not the session token"), Rig.SentAuthorization(), FString(TEXT("Bearer app-token")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAgentSignedOutTest, "CrowdySDK.CrowdyServices.StudioAgent.EveryCallRefusesWhenSignedOut", Flags)
bool FCrowdyStudioAgentSignedOutTest::RunTest(const FString& Parameters)
{
	FAgentRig Rig(AgentModelList, 200, false);
	Rig.GetBoot();
	TestEqual(TEXT("the boot refuses"), Rig.Boot.Result, ECrowdyStudioResult::NotSignedIn);
	TestTrue(TEXT("and carries no material"), Rig.Boot.AppId == 0 && Rig.Boot.GraphQLUrl.IsEmpty() && Rig.Boot.Models.IsEmpty());
	Rig.GetConsent();
	TestEqual(TEXT("the consent read refuses"), Rig.Consent.Result, ECrowdyStudioResult::NotSignedIn);
	Rig.RecordConsent(true);
	TestEqual(TEXT("the consent record refuses"), Rig.Consent.Result, ECrowdyStudioResult::NotSignedIn);
	Rig.GetUsage();
	TestEqual(TEXT("the usage read refuses"), Rig.Usage.Result, ECrowdyStudioResult::NotSignedIn);
	Rig.Authorize(ECrowdyStudioRunMode::Live);
	TestEqual(TEXT("the live check refuses"), Rig.Live.Result, ECrowdyStudioResult::NotSignedIn);
	TestFalse(TEXT("and allows nothing"), Rig.Live.bAllowed);
	TestFalse(TEXT("nothing was sent"), Rig.SentAnything());
	TestTrue(TEXT("no token leaves the SDK"), Rig.Agent->GetAgentToken().IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAgentModelFailureTest, "CrowdySDK.CrowdyServices.StudioAgent.ModelListFailuresMapToTypedResults", Flags)
bool FCrowdyStudioAgentModelFailureTest::RunTest(const FString& Parameters)
{
	FAgentRig Rig(AgentModelList);
	Rig.Script({
		TPair<int32, FString>(403, AgentModelError(TEXT("AGENT_PERMISSION_DENIED"))),
		TPair<int32, FString>(403, AgentModelError(TEXT("AGENT_MODEL_NOT_ALLOWED"))),
		TPair<int32, FString>(404, AgentModelError(TEXT("NOT_FOUND"))),
		TPair<int32, FString>(402, AgentModelError(TEXT("AGENT_FUNDS_NEEDED"))),
		TPair<int32, FString>(429, AgentModelError(TEXT("RATE_LIMITED"))),
		TPair<int32, FString>(503, AgentModelError(TEXT("AGENT_PROVIDER_UNAVAILABLE"))),
		TPair<int32, FString>(200, TEXT("{\"nope\":1}")),
		TPair<int32, FString>(200, TEXT("{\"object\":\"list\",\"data\":[]}")) });

	Rig.GetBoot();
	TestEqual(TEXT("a 403 is denied"), Rig.Boot.Result, ECrowdyStudioResult::Denied);
	TestTrue(TEXT("and carries no models"), Rig.Boot.Models.IsEmpty() && Rig.Boot.GraphQLUrl.IsEmpty());
	Rig.GetBoot();
	TestEqual(TEXT("a policy that allows no model is NoModel, not Denied"), Rig.Boot.Result, ECrowdyStudioResult::NoModel);
	Rig.GetBoot();
	TestEqual(TEXT("a 404 stays NotFound"), Rig.Boot.Result, ECrowdyStudioResult::NotFound);
	Rig.GetBoot();
	TestEqual(TEXT("a 402 is refused"), Rig.Boot.Result, ECrowdyStudioResult::Refused);
	Rig.GetBoot();
	TestEqual(TEXT("a 429 is refused"), Rig.Boot.Result, ECrowdyStudioResult::Refused);
	Rig.GetBoot();
	TestEqual(TEXT("a 503 fails"), Rig.Boot.Result, ECrowdyStudioResult::Failed);
	Rig.GetBoot();
	TestEqual(TEXT("a body with no list is a bad reply"), Rig.Boot.Result, ECrowdyStudioResult::BadReply);
	Rig.GetBoot();
	TestEqual(TEXT("an empty list is NoModel"), Rig.Boot.Result, ECrowdyStudioResult::NoModel);
	TestFalse(TEXT("with a message that says why"), Rig.Boot.Message.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAgentLinksTest, "CrowdySDK.CrowdyServices.StudioAgent.BootTakesTheStudioLinksFromThePlatform", Flags)
bool FCrowdyStudioAgentLinksTest::RunTest(const FString& Parameters)
{
	const TCHAR* const TierApi = TEXT("https://ck.dev.example.test");
	const auto Boot = [](const TCHAR* ApiUrl, const TCHAR* Origin, const TCHAR* Wallet, FCrowdyStudioAgentBoot& Out)
	{
		FAgentRig Rig(AgentModelList, 200, true, ApiUrl);
		Rig.Script({{200, AgentModelList}, {200, AgentLinksBody(Origin, Wallet)}});
		Rig.GetBoot();
		Out = Rig.Boot;
	};
	FCrowdyStudioAgentBoot Read;
	Boot(TEXT("https://game.test"), TEXT("https://studio.platform.test/"), TEXT("https://studio.platform.test/account/wallet"), Read);
	TestTrue(TEXT("the boot hands out the platform's links, a trailing slash dropped"), Read.Result == ECrowdyStudioResult::Success
		&& Read.StudioOrigin == TEXT("https://studio.platform.test") && Read.WalletUrl == TEXT("https://studio.platform.test/account/wallet"));
	{
		FAgentRig Rig(AgentModelList, 200, true, TierApi);
		Rig.Script({{200, AgentModelList}, {200, AgentErrorBody(TEXT("GRAPHQL_VALIDATION_FAILED"))}});
		Rig.GetBoot();
		TestEqual(TEXT("an API that cannot answer the links still boots"), Rig.Boot.Result, ECrowdyStudioResult::Success);
		TestEqual(TEXT("with the Studio its address implies"), Rig.Boot.StudioOrigin, FString(TEXT("https://studio.dev.example.test")));
		TestEqual(TEXT("and that Studio's wallet page"), Rig.Boot.WalletUrl, FString(TEXT("https://studio.dev.example.test/account/wallet")));
	}
	const TCHAR* const NotHanded[] = {TEXT("http://studio.platform.test"), TEXT("https://studio.platform.test/path"), TEXT("https://user@studio.platform.test")};
	for (const TCHAR* Origin : NotHanded)
	{
		Boot(TierApi, Origin, TEXT("https://studio.platform.test/account/wallet"), Read);
		TestEqual(*FString::Printf(TEXT("a platform origin %s is not handed out; the API address's Studio is"), Origin), Read.StudioOrigin,
			FString(TEXT("https://studio.dev.example.test")));
	}
	Boot(TierApi, TEXT("https://studio.platform.test"), TEXT("ftp://studio.platform.test/account/wallet"), Read);
	TestEqual(TEXT("a platform wallet that is not https is not handed out"), Read.WalletUrl, FString(TEXT("https://studio.dev.example.test/account/wallet")));
	Boot(TEXT("https://game.test"), TEXT("https://studio.platform.test"), TEXT("http://studio.platform.test/account/wallet"), Read);
	TestEqual(TEXT("an http wallet with no Studio implied by the API address refuses the boot"), Read.Result, ECrowdyStudioResult::Refused);
	TestTrue(TEXT("and hands out no link"), Read.StudioOrigin.IsEmpty() && Read.WalletUrl.IsEmpty());
#if !UE_BUILD_SHIPPING
	{
		FAgentRig Rig(AgentModelList, 200, true, TEXT("http://localhost:8080"));
		Rig.Script({{200, AgentModelList}, {200, AgentErrorBody(TEXT("GRAPHQL_VALIDATION_FAILED"))}});
		Rig.GetBoot();
		TestEqual(TEXT("a local API's Studio is on port 3001"), Rig.Boot.StudioOrigin, FString(TEXT("http://localhost:3001")));
	}
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAgentLiveModdingCodesTest, "CrowdySDK.CrowdyServices.StudioAgent.LiveModdingRefusalsMapByCode", Flags)
bool FCrowdyStudioAgentLiveModdingCodesTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		const TCHAR* Code;
		ECrowdyStudioResult Expected;
	};
	const FCase Cases[] = {
		{TEXT("AGENT_SCOPE_DENIED"), ECrowdyStudioResult::Denied},
		{TEXT("AGENT_PERMISSION_DENIED"), ECrowdyStudioResult::Denied},
		{TEXT("AGENT_DISABLED"), ECrowdyStudioResult::Denied},
		{TEXT("AGENT_OPERATOR_KILLED"), ECrowdyStudioResult::Denied},
		{TEXT("AGENT_PROVIDER_POLICY_UNSATISFIED"), ECrowdyStudioResult::Failed},
	};
	for (const FCase& Case : Cases)
	{
		FAgentRig Rig(AgentErrorBody(Case.Code));
		Rig.Authorize(ECrowdyStudioRunMode::Live);
		TestTrue(*FString::Printf(TEXT("%s ends %d and allows nothing"), Case.Code, static_cast<int32>(Case.Expected)),
			Rig.Live.Result == Case.Expected && !Rig.Live.bAllowed);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAgentTokenTest,"CrowdySDK.CrowdyServices.StudioAgent.TokenIsTheAppTokenNeverTheSessionToken", Flags)
bool FCrowdyStudioAgentTokenTest::RunTest(const FString& Parameters)
{
	FAgentRig Rig(AgentModelList);
	TestEqual(TEXT("the agent gets the app token"), Rig.Agent->GetAgentToken(), FString(TEXT("app-token")));
	TestNotEqual(TEXT("and not the session token"), Rig.Agent->GetAgentToken(), Rig.Session->GetSessionToken());
	Rig.Session->ClearCurrentSessionData();
	TestTrue(TEXT("a signed-out session hands out nothing"), Rig.Agent->GetAgentToken().IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAgentTokenEventTest, "CrowdySDK.CrowdyServices.StudioAgent.TokenChangeFiresOnRefreshSignInAndSignOut", Flags)
bool FCrowdyStudioAgentTokenEventTest::RunTest(const FString& Parameters)
{
	FAgentRig Rig(AgentModelList);
	Rig.ApplyToken(TEXT("app-token-2"), true);
	if (!TestEqual(TEXT("a refresh fires the event"), Rig.TokenEvents.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("with the rotated app token"), Rig.TokenEvents[0].Token, FString(TEXT("app-token-2")));
	TestEqual(TEXT("and the signed-in user"), Rig.TokenEvents[0].UserId, AgentTestUser);

	Rig.Session->SetUserID(8);
	Rig.ApplyToken(TEXT("app-token-3"), false);
	if (!TestEqual(TEXT("a sign-in as another user fires the event"), Rig.TokenEvents.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("with the new user's token"), Rig.TokenEvents[1].Token, FString(TEXT("app-token-3")));
	TestEqual(TEXT("and the new user's id"), Rig.TokenEvents[1].UserId, static_cast<int64>(8));

	Rig.Auth->OnRegister.Broadcast(FCrowdyAuthResult());
	Rig.Auth->OnSessionRestored.Broadcast(FCrowdyAuthResult());
	TestEqual(TEXT("a registration and a restored session fire it too"), Rig.TokenEvents.Num(), 4);

	Rig.Session->ClearCurrentSessionData();
	Rig.Agent->HandleSignedOut();
	if (!TestEqual(TEXT("a sign-out fires the event"), Rig.TokenEvents.Num(), 5))
	{
		return false;
	}
	TestTrue(TEXT("with an empty token"), Rig.TokenEvents[4].Token.IsEmpty());
	TestEqual(TEXT("and the user who left"), Rig.TokenEvents[4].UserId, static_cast<int64>(8));

	Rig.Auth->OnAppTokenRefreshed.Broadcast();
	TestEqual(TEXT("a refresh with no app token hands out nothing"), Rig.TokenEvents.Num(), 5);

	for (const FAgentTokenEvent& Event : Rig.TokenEvents)
	{
		TestNotEqual(TEXT("no event carries the session token"), Event.Token, FString(TEXT("session-token")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAgentTokenIsNativeOnlyTest, "CrowdySDK.CrowdyServices.StudioAgent.TokenHandOffIsCppOnly", Flags)
bool FCrowdyStudioAgentTokenIsNativeOnlyTest::RunTest(const FString& Parameters)
{
	static_assert(std::is_same_v<decltype(UCrowdyStudioAgent::OnAgentTokenChanged), FOnCrowdyAgentTokenChanged>, "the token event is the native delegate");
	const UClass* AgentClass = UCrowdyStudioAgent::StaticClass();
	TestNull(TEXT("no Blueprint function reads the token"), AgentClass->FindFunctionByName(TEXT("GetAgentToken")));
	TestNull(TEXT("no Blueprint event carries the token"), AgentClass->FindPropertyByName(TEXT("OnAgentTokenChanged")));
	TestNotNull(TEXT("the boot event stays bindable from Blueprint"), AgentClass->FindPropertyByName(TEXT("OnAgentBootChanged")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAgentMovedEndpointTest, "CrowdySDK.CrowdyServices.StudioAgent.BootChangedFiresWhenTheApiOriginMoves", Flags)
bool FCrowdyStudioAgentMovedEndpointTest::RunTest(const FString& Parameters)
{
	FAgentRig Rig(AgentModelList);
	const TStrongObjectPtr<UCrowdyStudioAgentTestListener> Listener(NewObject<UCrowdyStudioAgentTestListener>(GetTransientPackage()));
	Rig.Agent->OnAgentBootChanged.AddDynamic(Listener.Get(), &UCrowdyStudioAgentTestListener::HandleBootChanged);
	Rig.ApplyToken(TEXT("app-token-2"), true);
	TestEqual(TEXT("an unmoved origin fires nothing"), Listener->BootChanges, 0);

	Rig.Agent->SetApiEndpointForTest(TEXT("https://moved.game.test/graphql"));
	Rig.ApplyToken(TEXT("app-token-3"), true);
	TestEqual(TEXT("a moved origin fires once on the next token change"), Listener->BootChanges, 1);
	Rig.ApplyToken(TEXT("app-token-4"), true);
	TestEqual(TEXT("and not again while it stays put"), Listener->BootChanges, 1);

	Rig.Agent->SetApiEndpointForTest(TEXT("https://ck.third.game.test/graphql"));
	Rig.GetBoot();
	TestEqual(TEXT("the boot reads the moved endpoint"), Rig.Boot.GraphQLUrl, FString(TEXT("https://ck.third.game.test/graphql")));
	TestEqual(TEXT("and the Studio that endpoint implies"), Rig.Boot.StudioOrigin, FString(TEXT("https://studio.third.game.test")));
	TestEqual(TEXT("and a move seen on a boot read fires too"), Listener->BootChanges, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAgentNoticeTest, "CrowdySDK.CrowdyServices.StudioAgent.NoticeIsWritten", Flags)
bool FCrowdyStudioAgentNoticeTest::RunTest(const FString& Parameters)
{
	FAgentRig Rig(AgentModelList);
	const FString Notice = Rig.Agent->GetConsentNotice().ToString();
	TestTrue(TEXT("the notice names the model provider"), Notice.Contains(TEXT("model provider")));
	TestTrue(TEXT("the notice names the project source and messages"), Notice.Contains(TEXT("source")) && Notice.Contains(TEXT("messages")));
	TestTrue(TEXT("the notice names the screenshots"), Notice.Contains(TEXT("screenshots")));
	TestTrue(TEXT("the notice names other players"), Notice.Contains(TEXT("other players")));
	TestTrue(TEXT("the notice names the player's own position and view"), Notice.Contains(TEXT("Your own position and view")));
	TestTrue(TEXT("the notice says requests are charged and who pays"), Notice.Contains(TEXT("charged")) && Notice.Contains(TEXT("who pays")));
	TestTrue(TEXT("the notice says where sessions stay"), Notice.Contains(TEXT("this computer")));
	TestTrue(TEXT("the notice says another account can read them"), Notice.Contains(TEXT("another account")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAgentConsentReadTest, "CrowdySDK.CrowdyServices.StudioAgent.ConsentReadParsesTheAnswer", Flags)
bool FCrowdyStudioAgentConsentReadTest::RunTest(const FString& Parameters)
{
	FAgentRig Rig(AgentConsentBody(TEXT("crowdyStudioProviderConsent"), TEXT("true"), TEXT("\"2026-11-11T10:00:00.250Z\"")));
	Rig.GetConsent();
	TestEqual(TEXT("the read succeeds"), Rig.Consent.Result, ECrowdyStudioResult::Success);
	TestTrue(TEXT("consent is read"), Rig.Consent.bConsented);
	TestEqual(TEXT("consentedAt is read as Unix milliseconds"), Rig.Consent.ConsentedAtMs, AgentTimeMs(2026, 11, 11, 10, 250));
	TestEqual(TEXT("the operation is the consent read"), Rig.SentOperation(), FString(TEXT("CrowdyStudioProviderConsent")));
	TestEqual(TEXT("under the app token"), Rig.SentAuthorization(), FString(TEXT("Bearer app-token")));
	const TSharedPtr<FJsonObject> Variables = Rig.SentVariables();
	if (!TestTrue(TEXT("variables were sent"), Variables.IsValid()))
	{
		return false;
	}
	FString AppId;
	TestTrue(TEXT("appId travels as a string"), Variables->TryGetStringField(TEXT("appId"), AppId) && AppId == TEXT("31337"));

	FAgentRig Never(AgentConsentBody(TEXT("crowdyStudioProviderConsent"), TEXT("false"), TEXT("null")));
	Never.GetConsent();
	TestTrue(TEXT("a null consentedAt is never consented"), Never.Consent.Result == ECrowdyStudioResult::Success && !Never.Consent.bConsented && Never.Consent.ConsentedAtMs == 0);

	FAgentRig Wrong(AgentConsentBody(TEXT("crowdyStudioProviderConsent"), TEXT("true"), TEXT("null"), TEXT("\"99\"")));
	Wrong.GetConsent();
	TestEqual(TEXT("an answer for another app is a bad reply"), Wrong.Consent.Result, ECrowdyStudioResult::BadReply);
	TestFalse(TEXT("and is not consent"), Wrong.Consent.bConsented);

	FAgentRig Garbled(AgentConsentBody(TEXT("crowdyStudioProviderConsent"), TEXT("true"), TEXT("\"yesterday\"")));
	Garbled.GetConsent();
	TestEqual(TEXT("a time that does not parse is a bad reply"), Garbled.Consent.Result, ECrowdyStudioResult::BadReply);

	FAgentRig Forbidden(AgentErrorBody(TEXT("FORBIDDEN")));
	Forbidden.GetConsent();
	TestEqual(TEXT("a FORBIDDEN refusal is denied"), Forbidden.Consent.Result, ECrowdyStudioResult::Denied);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAgentConsentRecordTest, "CrowdySDK.CrowdyServices.StudioAgent.ConsentRecordSendsTheInput", Flags)
bool FCrowdyStudioAgentConsentRecordTest::RunTest(const FString& Parameters)
{
	FAgentRig Rig(AgentConsentBody(TEXT("crowdyStudioSetProviderConsent"), TEXT("true"), TEXT("\"2026-11-11T10:00:00.250Z\"")));
	Rig.RecordConsent(true);
	TestEqual(TEXT("the record succeeds"), Rig.Consent.Result, ECrowdyStudioResult::Success);
	TestTrue(TEXT("and reads back consent"), Rig.Consent.bConsented && Rig.Consent.ConsentedAtMs == AgentTimeMs(2026, 11, 11, 10, 250));
	TestEqual(TEXT("the operation is the consent mutation"), Rig.SentOperation(), FString(TEXT("CrowdyStudioSetProviderConsent")));
	const TSharedPtr<FJsonObject> Variables = Rig.SentVariables();
	const TSharedPtr<FJsonObject>* Input = nullptr;
	if (!TestTrue(TEXT("an input object was sent"), Variables.IsValid() && Variables->TryGetObjectField(TEXT("input"), Input) && Input))
	{
		return false;
	}
	FString AppId;
	bool bConsented = false;
	TestTrue(TEXT("appId travels as a string"), (*Input)->TryGetStringField(TEXT("appId"), AppId) && AppId == TEXT("31337"));
	TestTrue(TEXT("consented is true"), (*Input)->TryGetBoolField(TEXT("consented"), bConsented) && bConsented);

	FAgentRig Revoke(AgentConsentBody(TEXT("crowdyStudioSetProviderConsent"), TEXT("false"), TEXT("null")));
	Revoke.RecordConsent(false);
	TestTrue(TEXT("a revoke succeeds and is not consent"), Revoke.Consent.Result == ECrowdyStudioResult::Success && !Revoke.Consent.bConsented);
	const TSharedPtr<FJsonObject> RevokeVariables = Revoke.SentVariables();
	const TSharedPtr<FJsonObject>* RevokeInput = nullptr;
	if (TestTrue(TEXT("a revoke sends its input"), RevokeVariables.IsValid() && RevokeVariables->TryGetObjectField(TEXT("input"), RevokeInput) && RevokeInput))
	{
		bool bSent = true;
		TestTrue(TEXT("consented is false"), (*RevokeInput)->TryGetBoolField(TEXT("consented"), bSent) && !bSent);
	}

	FAgentRig Ignored(AgentConsentBody(TEXT("crowdyStudioSetProviderConsent"), TEXT("false"), TEXT("null")));
	Ignored.RecordConsent(true);
	TestEqual(TEXT("an answer that did not record the change is refused"), Ignored.Consent.Result, ECrowdyStudioResult::Refused);
	TestFalse(TEXT("and is not consent"), Ignored.Consent.bConsented);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAgentUsageTest, "CrowdySDK.CrowdyServices.StudioAgent.UsageParsesBigIntStringsAndAPayer", Flags)
bool FCrowdyStudioAgentUsageTest::RunTest(const FString& Parameters)
{
	FAgentRig Rig(AgentUsageBody(TEXT("ORG"), TEXT("\"12\""), TEXT("\"9007199254740995\""), TEXT("\"5000000\"")));
	Rig.GetUsage();
	TestEqual(TEXT("the read succeeds"), Rig.Usage.Result, ECrowdyStudioResult::Success);
	TestEqual(TEXT("ORG is the app paying"), Rig.Usage.Payer, ECrowdyStudioPayer::App);
	TestEqual(TEXT("requests"), Rig.Usage.TodayRequests, static_cast<int64>(12));
	TestEqual(TEXT("a charge past 2^53 keeps every digit"), Rig.Usage.TodayChargeMicroUsd, static_cast<int64>(9007199254740995LL));
	TestEqual(TEXT("the day limit"), Rig.Usage.DayLimitMicroUsd, static_cast<int64>(5000000));
	TestEqual(TEXT("the operation is the usage read"), Rig.SentOperation(), FString(TEXT("CrowdyStudioModelUsage")));
	const TSharedPtr<FJsonObject> Variables = Rig.SentVariables();
	if (!TestTrue(TEXT("variables were sent"), Variables.IsValid()))
	{
		return false;
	}
	double Limit = 0.0;
	TestTrue(TEXT("only the newest row is asked for"), Variables->TryGetNumberField(TEXT("limit"), Limit) && Limit == 1.0);
	FString AppId;
	TestTrue(TEXT("appId travels as a string"), Variables->TryGetStringField(TEXT("appId"), AppId) && AppId == TEXT("31337"));

	FAgentRig Player(AgentUsageBody(TEXT("PLAYER"), TEXT("\"0\""), TEXT("\"0\""), TEXT("null")));
	Player.GetUsage();
	TestTrue(TEXT("PLAYER is the player paying and a null limit is 0"), Player.Usage.Result == ECrowdyStudioResult::Success
		&& Player.Usage.Payer == ECrowdyStudioPayer::Player && Player.Usage.DayLimitMicroUsd == 0);

	FAgentRig Platform(AgentUsageBody(TEXT("PLATFORM"), TEXT("\"1\""), TEXT("\"2\""), TEXT("\"3\"")));
	Platform.GetUsage();
	TestEqual(TEXT("PLATFORM is the platform paying"), Platform.Usage.Payer, ECrowdyStudioPayer::Platform);

	FAgentRig Unknown(AgentUsageBody(TEXT("SPONSOR"), TEXT("\"1\""), TEXT("\"2\""), TEXT("\"3\"")));
	Unknown.GetUsage();
	TestTrue(TEXT("an unknown payer reads as Unknown without failing"), Unknown.Usage.Result == ECrowdyStudioResult::Success
		&& Unknown.Usage.Payer == ECrowdyStudioPayer::Unknown);

	FAgentRig Garbled(AgentUsageBody(TEXT("PLAYER"), TEXT("\"12abc\""), TEXT("\"2\""), TEXT("\"3\"")));
	Garbled.GetUsage();
	TestEqual(TEXT("a count that is not a whole number is a bad reply"), Garbled.Usage.Result, ECrowdyStudioResult::BadReply);
	TestEqual(TEXT("and nothing from it is returned"), Garbled.Usage.TodayChargeMicroUsd, static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAgentLiveModdingTest, "CrowdySDK.CrowdyServices.StudioAgent.AuthorizeLiveModdingSendsEachActionAndMapsTheAnswer", Flags)
bool FCrowdyStudioAgentLiveModdingTest::RunTest(const FString& Parameters)
{
	FAgentRig Rig(AgentLiveBody(TEXT("true")));
	Rig.Script({
		TPair<int32, FString>(200, AgentLiveBody(TEXT("true"))),
		TPair<int32, FString>(200, AgentLiveBody(TEXT("false"))),
		TPair<int32, FString>(200, AgentErrorBody(TEXT("FORBIDDEN"))),
		TPair<int32, FString>(200, AgentErrorBody(TEXT("AGENT_DISABLED"))),
		TPair<int32, FString>(200, AgentErrorBody(TEXT("AGENT_APP_KILLED"))),
		TPair<int32, FString>(200, TEXT("{\"data\":{}}")) });

	const auto SentAction = [&Rig]()
	{
		FString Action;
		const TSharedPtr<FJsonObject> Variables = Rig.SentVariables();
		if (Variables.IsValid())
		{
			Variables->TryGetStringField(TEXT("action"), Action);
		}
		return Action;
	};

	Rig.Authorize(ECrowdyStudioRunMode::DraftTest);
	TestTrue(TEXT("a true answer allows the draft test"), Rig.Live.Result == ECrowdyStudioResult::Success && Rig.Live.bAllowed);
	TestEqual(TEXT("the draft test is sent as draft_test"), SentAction(), FString(TEXT("draft_test")));
	TestEqual(TEXT("the operation is the live modding check"), Rig.SentOperation(), FString(TEXT("CrowdyStudioLiveModding")));
	TestEqual(TEXT("under the app token"), Rig.SentAuthorization(), FString(TEXT("Bearer app-token")));
	const TSharedPtr<FJsonObject> Variables = Rig.SentVariables();
	FString AppId;
	TestTrue(TEXT("appId travels as a string"), Variables.IsValid() && Variables->TryGetStringField(TEXT("appId"), AppId) && AppId == TEXT("31337"));

	Rig.Authorize(ECrowdyStudioRunMode::Live);
	TestTrue(TEXT("a false answer does not allow the deploy"), Rig.Live.Result == ECrowdyStudioResult::Success && !Rig.Live.bAllowed);
	TestFalse(TEXT("and says why"), Rig.Live.Message.IsEmpty());
	TestEqual(TEXT("a live deploy is sent as deploy_live"), SentAction(), FString(TEXT("deploy_live")));

	Rig.Authorize(ECrowdyStudioRunMode::Live);
	TestTrue(TEXT("a FORBIDDEN refusal is denied and allows nothing"), Rig.Live.Result == ECrowdyStudioResult::Denied && !Rig.Live.bAllowed);
	Rig.Authorize(ECrowdyStudioRunMode::Live);
	TestEqual(TEXT("AGENT_DISABLED is denied"), Rig.Live.Result, ECrowdyStudioResult::Denied);
	Rig.Authorize(ECrowdyStudioRunMode::Live);
	TestEqual(TEXT("AGENT_APP_KILLED is denied"), Rig.Live.Result, ECrowdyStudioResult::Denied);
	Rig.Authorize(ECrowdyStudioRunMode::Live);
	TestTrue(TEXT("an answer with no verdict is a bad reply that allows nothing"), Rig.Live.Result == ECrowdyStudioResult::BadReply && !Rig.Live.bAllowed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAgentStrictBooleanTest, "CrowdySDK.CrowdyServices.StudioAgent.AVerdictAndAConsentFlagMustBeJsonBooleans", Flags)
bool FCrowdyStudioAgentStrictBooleanTest::RunTest(const FString& Parameters)
{
	const TCHAR* const NotBooleans[] = {TEXT("1"), TEXT("\"true\""), TEXT("\"yes\""), TEXT("null")};
	for (const TCHAR* Value : NotBooleans)
	{
		FAgentRig Live(FString::Printf(TEXT("{\"data\":{\"crowdyStudioLiveModding\":%s}}"), Value));
		Live.Authorize(ECrowdyStudioRunMode::Live);
		TestTrue(*FString::Printf(TEXT("a live verdict of %s is a bad reply that allows nothing"), Value), Live.Live.Result == ECrowdyStudioResult::BadReply && !Live.Live.bAllowed);

		FAgentRig Read(AgentConsentBody(TEXT("crowdyStudioProviderConsent"), Value, TEXT("null")));
		Read.GetConsent();
		TestTrue(*FString::Printf(TEXT("a consent flag of %s is a bad reply and not consent"), Value), Read.Consent.Result == ECrowdyStudioResult::BadReply && !Read.Consent.bConsented);

		FAgentRig Record(AgentConsentBody(TEXT("crowdyStudioSetProviderConsent"), Value, TEXT("null")));
		Record.RecordConsent(true);
		TestTrue(*FString::Printf(TEXT("a recorded flag of %s is a bad reply"), Value), Record.Consent.Result == ECrowdyStudioResult::BadReply && !Record.Consent.bConsented);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAgentHttpsOnlyTest, "CrowdySDK.CrowdyServices.StudioAgent.BootRefusesAnApiThatIsNotHttps", Flags)
bool FCrowdyStudioAgentHttpsOnlyTest::RunTest(const FString& Parameters)
{
	AddExpectedErrorPlain(TEXT("The Game API endpoint is not encrypted"), EAutomationExpectedErrorFlags::Contains, 0);
	FAgentRig Plain(AgentModelList, 200, true, TEXT("http://game.test"));
	Plain.GetBoot();
	TestEqual(TEXT("an http API is refused"), Plain.Boot.Result, ECrowdyStudioResult::Refused);
	TestTrue(TEXT("with a message that says why"), Plain.Boot.Message.Contains(TEXT("https")));
	TestFalse(TEXT("and nothing, not even the token, was sent"), Plain.SentAnything());

	FAgentRig Lookalike(AgentModelList, 200, true, TEXT("http://localhost.evil.test"));
	Lookalike.GetBoot();
	TestEqual(TEXT("a host that only starts with localhost is refused"), Lookalike.Boot.Result, ECrowdyStudioResult::Refused);
	TestFalse(TEXT("and nothing was sent"), Lookalike.SentAnything());

	FAgentRig Moved(AgentModelList);
	Moved.Agent->SetApiEndpointForTest(TEXT("http://game.test/graphql"));
	Moved.GetBoot();
	TestEqual(TEXT("an endpoint that moved to http is refused"), Moved.Boot.Result, ECrowdyStudioResult::Refused);

	FAgentRig Loopback(AgentModelList, 200, true, TEXT("http://localhost:8080"));
	Loopback.GetBoot();
#if UE_BUILD_SHIPPING
	TestEqual(TEXT("loopback http is refused in Shipping"), Loopback.Boot.Result, ECrowdyStudioResult::Refused);
#else
	TestEqual(TEXT("loopback http is allowed outside Shipping"), Loopback.Boot.Result, ECrowdyStudioResult::Success);
	TestEqual(TEXT("and the model base is that origin"), Loopback.Boot.ModelBaseUrl, FString(TEXT("http://localhost:8080")));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAgentAfterEndTest, "CrowdySDK.CrowdyServices.StudioAgent.AnswerAfterTheEndIsDropped", Flags)
bool FCrowdyStudioAgentAfterEndTest::RunTest(const FString& Parameters)
{
	const auto Script = [](FAgentRig& Rig)
	{
		Rig.Script({
			TPair<int32, FString>(200, AgentUsageBody(TEXT("PLAYER"), TEXT("\"1\""), TEXT("\"2\""), TEXT("\"3\""))),
			TPair<int32, FString>(200, AgentModelList),
			TPair<int32, FString>(200, AgentConsentBody(TEXT("crowdyStudioProviderConsent"), TEXT("true"), TEXT("null"))),
			TPair<int32, FString>(200, AgentLiveBody(TEXT("true"))) });
	};
	const auto IssueAll = [](FAgentRig& Rig, int32& Answers)
	{
		Rig.Agent->GetModelUsage([&Answers](const FCrowdyStudioModelUsage&) { ++Answers; });
		Rig.Agent->GetAgentBoot([&Answers](const FCrowdyStudioAgentBoot&) { ++Answers; });
		Rig.Agent->GetProviderConsent([&Answers](const FCrowdyStudioConsent&) { ++Answers; });
		Rig.Agent->AuthorizeLiveModding(ECrowdyStudioRunMode::Live, [&Answers](const FCrowdyStudioLiveModding&) { ++Answers; });
	};

	FAgentRig Control(AgentModelList);
	Script(Control);
	int32 ControlAnswers = 0;
	IssueAll(Control, ControlAnswers);
	Control.Idle();
	TestEqual(TEXT("the same four calls all answer within the same polls when the subsystem lives"), ControlAnswers, 4);

	FAgentRig Rig(AgentModelList);
	Script(Rig);
	int32 Answers = 0;
	IssueAll(Rig, Answers);
	Rig.Agent->Deinitialize();
	IssueAll(Rig, Answers);
	Rig.Idle();
	TestEqual(TEXT("an answer due after the subsystem ended, and a call made after it, run nothing"), Answers, 0);

	Rig.Auth->OnAppTokenRefreshed.Broadcast();
	Rig.Agent->HandleSignedOut();
	TestEqual(TEXT("an ended subsystem hands out no token"), Rig.TokenEvents.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioAgentDefaultsTest, "CrowdySDK.CrowdyServices.StudioAgent.AnswersDefaultToNothing", Flags)
bool FCrowdyStudioAgentDefaultsTest::RunTest(const FString& Parameters)
{
	static_assert(std::is_same_v<decltype(FCrowdyStudioAgentBoot::AppId), int64>, "the app id is a BigInt");
	static_assert(std::is_same_v<decltype(FCrowdyStudioModelUsage::TodayChargeMicroUsd), int64>, "charges are int64 micro-USD");
	const FCrowdyStudioAgentBoot Boot;
	TestTrue(TEXT("a default boot is not a success"), Boot.Result != ECrowdyStudioResult::Success && Boot.AppId == 0 && Boot.Models.IsEmpty());
	const FCrowdyStudioConsent Consent;
	TestTrue(TEXT("default consent is not consent"), Consent.Result != ECrowdyStudioResult::Success && !Consent.bConsented && Consent.ConsentedAtMs == 0);
	const FCrowdyStudioModelUsage Usage;
	TestTrue(TEXT("default usage names no payer"), Usage.Result != ECrowdyStudioResult::Success && Usage.Payer == ECrowdyStudioPayer::Unknown && Usage.TodayRequests == 0
		&& Usage.DayLimitMicroUsd == 0);
	TestEqual(TEXT("the platform pays as the last payer"), static_cast<int32>(ECrowdyStudioPayer::Platform), 3);
	const FCrowdyStudioLiveModding Live;
	TestTrue(TEXT("a default check allows nothing"), Live.Result != ECrowdyStudioResult::Success && !Live.bAllowed);
	const FCrowdyStudioModel Model;
	TestTrue(TEXT("a default model is empty"), Model.ModelId.IsEmpty() && Model.ContextWindow == 0 && Model.InputModalities.IsEmpty());
	return true;
}

#endif
