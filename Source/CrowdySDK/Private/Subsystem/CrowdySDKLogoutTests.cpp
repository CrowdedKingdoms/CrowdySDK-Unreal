#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Network/UDP/CrowdyUDPSubsystem.h"
#include "Subsystem/CrowdyActorOwners.h"
#include "Subsystem/CrowdyAuthentication.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdySDKSubsystem.h"
#include "Subsystem/CrowdyStudioAgent.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// A bare subsystem with no Leader subsystem listening: Logout must clear the session's host id itself, at once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySDKLogoutClearsSessionHostTest,
	"CrowdySDK.SDK.LogoutClearsSessionHost",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdySDKLogoutClearsSessionHostTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage()));
	UCrowdyGameSession* Session = NewObject<UCrowdyGameSession>(Instance.Get());
	Session->SetUserID(7);
	Session->SetHostID(FGuid::NewGuid());

	UCrowdySDKSubsystem* Sdk = NewObject<UCrowdySDKSubsystem>(Instance.Get());
	Sdk->SetGameSessionForTest(Session);

	Sdk->Logout();
	TestFalse(TEXT("the session holds no host as soon as Logout returns"), Session->GetAppWriterID().IsValid());
	TestEqual(TEXT("the signed-in user is cleared"), Session->GetUserID(), static_cast<int64>(0));
	return true;
}

// The actor id belongs to one sign-in: a sign-out drops it, and the next sign-in, the same user or another, gets a
// fresh one. The entity tests prove the fresh id is announced and re-keys a live pawn.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySDKLogoutDropsTheActorIdTest,
	"CrowdySDK.SDK.LogoutDropsTheActorIdAndTheNextSignInMintsAnother",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdySDKLogoutDropsTheActorIdTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage()));
	UCrowdyGameSession* Session = NewObject<UCrowdyGameSession>(Instance.Get());
	Session->SetUserID(7);
	Session->BeginSignIn();
	const FGuid First = Session->GetID();
	TestTrue(TEXT("a sign-in with no id mints one"), First.IsValid());
	TestTrue(TEXT("and is signed in"), Session->IsSignedIn());

	UCrowdySDKSubsystem* Sdk = NewObject<UCrowdySDKSubsystem>(Instance.Get());
	Sdk->SetGameSessionForTest(Session);

	Sdk->Logout();
	TestFalse(TEXT("signed out, the session holds no actor id"), Session->GetID().IsValid());
	TestTrue(TEXT("nor its text"), Session->GetUUID().IsEmpty());
	TestFalse(TEXT("and is not signed in"), Session->IsSignedIn());

	Session->SetUserID(7);
	Session->BeginSignIn();
	const FGuid Second = Session->GetID();
	TestTrue(TEXT("the same user signing back in gets a new id"), Second.IsValid() && Second != First);

	// A sign-in over a live one ends the previous sign-in first, keeping its id so it is not adopted.
	Session->EndSignIn();
	Session->SetUserID(8);
	Session->BeginSignIn();
	TestTrue(TEXT("another user's sign-in replaces the held id"), Session->GetID().IsValid() && Session->GetID() != Second);
	return true;
}

// A pawn that resolved before anyone signed in minted an id nothing could send under, so the first sign-in keeps it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySDKSignInAdoptsAnIdMintedSignedOutTest,
	"CrowdySDK.SDK.SignInAdoptsAnIdMintedWhileSignedOut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdySDKSignInAdoptsAnIdMintedSignedOutTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage()));
	UCrowdyGameSession* Session = NewObject<UCrowdyGameSession>(Instance.Get());
	const FGuid Early = Session->EnsureActorID();

	Session->SetUserID(7);
	TestEqual(TEXT("setting the user changes nothing"), Session->GetID(), Early);
	Session->BeginSignIn();
	TestEqual(TEXT("the first sign-in adopts it"), Session->GetID(), Early);

	Session->EndSignIn();
	Session->BeginSignIn();
	TestTrue(TEXT("an adopted id is the sign-in's, so the next sign-in replaces it"), Session->GetID().IsValid() && Session->GetID() != Early);

	// One minted while a sign-in is still completing is adopted too: nothing sends before the connection opens.
	Session->EndSignIn();
	Session->ClearCurrentSessionData();
	Session->SetUserID(9);
	const FGuid Pending = Session->EnsureActorID();
	Session->BeginSignIn();
	TestEqual(TEXT("an id minted between the sign-in starting and completing is adopted"), Session->GetID(), Pending);
	return true;
}

// Clear Current Session Data forgets the values, not the sign-in: only a sign-out ends that, so an id minted afterwards
// still belongs to the sign-in and the next one replaces it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySDKClearingSessionDataKeepsTheSignInTest,
	"CrowdySDK.SDK.ClearCurrentSessionDataLeavesTheSignInToLogout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdySDKClearingSessionDataKeepsTheSignInTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage()));
	UCrowdyGameSession* Session = NewObject<UCrowdyGameSession>(Instance.Get());
	Session->SetUserID(7);
	Session->BeginSignIn();

	Session->ClearCurrentSessionData();
	TestTrue(TEXT("cleared, the session is still signed in"), Session->IsSignedIn());
	const FGuid Minted = Session->EnsureActorID();
	Session->BeginSignIn();
	TestTrue(TEXT("so an id minted then is the sign-in's, and the next sign-in replaces it"), Session->GetID().IsValid() && Session->GetID() != Minted);

	UCrowdySDKSubsystem* Sdk = NewObject<UCrowdySDKSubsystem>(Instance.Get());
	Sdk->SetGameSessionForTest(Session);
	Sdk->Logout();
	TestFalse(TEXT("Logout ends it"), Session->IsSignedIn());
	return true;
}

// The SDK mints a signed-in player's actor id itself: Set UUID with another id while signed in is refused, with one
// warning per sign-in, and an id a game set while signed out is never adopted by the next sign-in.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySDKSetUUIDCannotChooseTheIdTest,
	"CrowdySDK.SDK.SetUUIDNeitherChangesASignedInIdNorIsAdoptedAtSignIn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdySDKSetUUIDCannotChooseTheIdTest::RunTest(const FString& Parameters)
{
	AddExpectedMessagePlain(TEXT("Set UUID ignored"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);

	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage()));
	UCrowdyGameSession* Session = NewObject<UCrowdyGameSession>(Instance.Get());
	Session->SetUserID(7);
	const FString Saved = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Session->SetUUID(Saved);
	TestEqual(TEXT("signed out, a game may set the id"), Session->GetUUID(), Saved);

	Session->BeginSignIn();
	const FGuid Signed = Session->GetID();
	TestTrue(TEXT("a sign-in does not adopt an id a game set"), Signed.IsValid() && Session->GetUUID() != Saved);

	Session->SetUUID(FGuid::NewGuid().ToString(EGuidFormats::Digits));
	Session->SetUUID(FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TestEqual(TEXT("signed in, a game cannot change it"), Session->GetID(), Signed);
	Session->SetUUID(Signed.ToString(EGuidFormats::Digits));
	TestEqual(TEXT("its own id announced again is no change"), Session->GetID(), Signed);

	// The next sign-in warns again, once.
	Session->EndSignIn();
	Session->BeginSignIn();
	const FGuid Next = Session->GetID();
	Session->SetUUID(FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TestEqual(TEXT("nor change the next sign-in's"), Session->GetID(), Next);
	return true;
}

namespace CrowdySDKSignInTests
{
	const TCHAR* const CreatedAnswer = TEXT("{\"data\":{\"createActor\":{\"uuid\":\"created\"}}}");
	const TCHAR* const DeletedAnswer = TEXT("{\"data\":{\"deleteActor\":{\"uuid\":\"deleted\"}}}");
	const TCHAR* const LogoutAnswer = TEXT("{\"data\":{\"logout\":true}}");

	struct FSentRequest
	{
		FString Body;
		FString Authorization;
	};

	TSharedPtr<FCrowdyCppClient> MakeScriptedClient(const TArray<const TCHAR*>& Replies, TArray<FSentRequest>& OutSent)
	{
		const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(CreatedAnswer, 200);
		if (!Client.IsValid())
		{
			return Client;
		}
		TArray<TPair<int32, FString>> Responses;
		for (const TCHAR* Reply : Replies)
		{
			Responses.Emplace(200, Reply);
		}
		Client->SetTestResponseScript(MoveTemp(Responses));
		FCrowdyCppClient* Raw = Client.Get();
		Client->SetTestOnRequest([Raw, &OutSent](const FString&)
		{
			FSentRequest& Sent = OutSent.AddDefaulted_GetRef();
			FString Url;
			Raw->GetLastTestRequest(Url, Sent.Authorization);
			Raw->GetLastTestRequestBody(Sent.Body);
		});
		return Client;
	}

	void Pump(FCrowdyCppClient& Client)
	{
		for (int32 Round = 0; Round < 4; ++Round)
		{
			Client.Poll();
		}
	}

	int32 IndexOfOperation(const TArray<FSentRequest>& Sent, const TCHAR* Operation)
	{
		return Sent.IndexOfByPredicate([Operation](const FSentRequest& Request) { return Request.Body.Contains(Operation); });
	}
}

// Logout ends the server session only once the actor record delete answers, by which time the session has forgotten
// its token and a new sign-in may hold another: the end goes out under the token the player signed out with.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySDKLogoutEndsTheSessionItSignedOutTest,
	"CrowdySDK.SDK.LogoutEndsTheServerSessionUnderTheTokenItSignedOutWith",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdySDKLogoutEndsTheSessionItSignedOutTest::RunTest(const FString& Parameters)
{
	using namespace CrowdySDKSignInTests;
	TArray<FSentRequest> Sent;
	const TSharedPtr<FCrowdyCppClient> Client = MakeScriptedClient({CreatedAnswer, DeletedAnswer, LogoutAnswer}, Sent);
	if (!TestTrue(TEXT("client built"), Client.IsValid()))
	{
		return false;
	}
	Client->SetManagementToken(TEXT("session-old"));
	Client->SetGameToken(TEXT("app-old"));

	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage()));
	UCrowdyGameSession* Session = NewObject<UCrowdyGameSession>(Instance.Get());
	Session->SetAppID(31337);
	Session->SetUserID(7);
	Session->SetSessionToken(TEXT("session-old"));
	Session->SetGameToken(TEXT("app-old"));
	Session->BeginSignIn();

	UCrowdyActorOwners* Owners = NewObject<UCrowdyActorOwners>(Instance.Get());
	Owners->InitializeForTest(Client, 31337, Session);
	Owners->HandleSignedIn(nullptr);
	Pump(*Client);

	UCrowdyAuthentication* Auth = NewObject<UCrowdyAuthentication>(Instance.Get());
	Auth->InitializeForTest(Client, Session);
	UCrowdySDKSubsystem* Sdk = NewObject<UCrowdySDKSubsystem>(Instance.Get());
	Sdk->SetGameSessionForTest(Session);
	Sdk->SetSignInCollaboratorsForTest(Auth, Owners, nullptr);

	Sdk->Logout();
	TestTrue(TEXT("signed out, the session holds no token"), Session->GetSessionToken().IsEmpty());
	TestEqual(TEXT("the session waits for the record delete"), IndexOfOperation(Sent, TEXT("logout")), static_cast<int32>(INDEX_NONE));

	// A sign-in that lands before the delete answers installs its own token.
	Session->SetSessionToken(TEXT("session-new"));
	Client->SetManagementToken(TEXT("session-new"));
	Pump(*Client);

	const int32 Delete = IndexOfOperation(Sent, TEXT("DeleteActor"));
	const int32 Logout = IndexOfOperation(Sent, TEXT("logout"));
	if (!TestTrue(TEXT("the record delete, then the session's end"), Delete != INDEX_NONE && Logout > Delete))
	{
		Owners->Deinitialize();
		return false;
	}
	TestEqual(TEXT("which carries the token the player signed out with"), Sent[Logout].Authorization, FString(TEXT("Bearer session-old")));
	Owners->Deinitialize();
	return true;
}

// The connection opens only for a signed-in player whose actor record has answered: Request UDP Access and a token
// rotation are refused while signed out and deferred while the record is out, and Set Game Session Info signs in, so
// the record's answer opens it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySDKConnectionWaitsForTheRecordTest,
	"CrowdySDK.SDK.TheConnectionOpensOnlyForASignInWhoseRecordAnswered",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdySDKConnectionWaitsForTheRecordTest::RunTest(const FString& Parameters)
{
	using namespace CrowdySDKSignInTests;

	// Each counted exactly: one refusal while signed out, and one attempt to open, which fails for want of a transport.
	AddExpectedMessagePlain(TEXT("Request UDP Access ignored: no player is signed in."), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("UDP access request failed"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	TArray<FSentRequest> Sent;
	const TSharedPtr<FCrowdyCppClient> Client = MakeScriptedClient({CreatedAnswer}, Sent);
	if (!TestTrue(TEXT("client built"), Client.IsValid()))
	{
		return false;
	}

	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage()));
	UCrowdyGameSession* Session = NewObject<UCrowdyGameSession>(Instance.Get());
	UCrowdyActorOwners* Owners = NewObject<UCrowdyActorOwners>(Instance.Get());
	Owners->InitializeForTest(Client, 31337, Session);
	UCrowdyAuthentication* Auth = NewObject<UCrowdyAuthentication>(Instance.Get());
	UCrowdySDKSubsystem* Sdk = NewObject<UCrowdySDKSubsystem>(Instance.Get());
	Sdk->SetGameSessionForTest(Session);
	Sdk->SetSignInCollaboratorsForTest(nullptr, Owners, NewObject<UCrowdyUDPSubsystem>(Instance.Get()));
	Sdk->BindAuthenticationForTest(Auth);

	Sdk->RequestUDPAccess();
	Auth->OnAppTokenRefreshed.Broadcast();

	FGameSessionInfo Info;
	Info.UserID = 7;
	Info.GameToken = TEXT("app-abc");
	Sdk->SetGameSessionInfo(Info);
	TestTrue(TEXT("a session info with a user and a token signs in"), Session->IsSignedIn() && Session->GetID().IsValid());
	TestTrue(TEXT("and records the actor first"), Owners->IsSettlingSignIn() && IndexOfOperation(Sent, TEXT("CreateActor")) == 0);

	Sdk->RequestUDPAccess();
	Auth->OnAppTokenRefreshed.Broadcast();
	Pump(*Client);
	TestTrue(TEXT("the record's answer settles the sign-in, which opens the connection"), Owners->HasSettledSignIn());
	Owners->Deinitialize();
	return true;
}

// A menu installs the saved session each time it opens: the same user and app token again is the sign-in already
// running and changes nothing. An app token with no user id still signs in and connects, with no actor record, and
// says once that the player's id is unrecorded.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySDKSessionInfoAgainKeepsTheSignInTest,
	"CrowdySDK.SDK.SetGameSessionInfoKeepsALiveSignInAndConnectsWithNoUser",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdySDKSessionInfoAgainKeepsTheSignInTest::RunTest(const FString& Parameters)
{
	using namespace CrowdySDKSignInTests;

	// Three openings, each failing for want of a transport: the first sign-in's and the two with no user.
	AddExpectedMessagePlain(TEXT("UDP access request failed"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 3);
	AddExpectedMessagePlain(TEXT("carries no user id"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	TArray<FSentRequest> Sent;
	const TSharedPtr<FCrowdyCppClient> Client = MakeScriptedClient({CreatedAnswer, CreatedAnswer}, Sent);
	if (!TestTrue(TEXT("client built"), Client.IsValid()))
	{
		return false;
	}

	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage()));
	UCrowdyGameSession* Session = NewObject<UCrowdyGameSession>(Instance.Get());
	UCrowdyActorOwners* Owners = NewObject<UCrowdyActorOwners>(Instance.Get());
	Owners->InitializeForTest(Client, 31337, Session);
	UCrowdySDKSubsystem* Sdk = NewObject<UCrowdySDKSubsystem>(Instance.Get());
	Sdk->SetGameSessionForTest(Session);
	Sdk->SetSignInCollaboratorsForTest(nullptr, Owners, NewObject<UCrowdyUDPSubsystem>(Instance.Get()));

	FGameSessionInfo Info;
	Info.UserID = 7;
	Info.GameToken = TEXT("app-abc");
	Sdk->SetGameSessionInfo(Info);
	Pump(*Client);
	const FGuid First = Session->GetID();
	Sdk->SetGameSessionInfo(Info);
	Pump(*Client);
	TestEqual(TEXT("the same user and token again keep the actor id"), Session->GetID(), First);
	TestEqual(TEXT("and record nothing again"), Sent.Num(), 1);
	TestTrue(TEXT("and stay settled"), Owners->HasSettledSignIn());

	UCrowdyGameSession* Unnamed = NewObject<UCrowdyGameSession>(Instance.Get());
	UCrowdyActorOwners* UnnamedOwners = NewObject<UCrowdyActorOwners>(Instance.Get());
	UnnamedOwners->InitializeForTest(Client, 31337, Unnamed);
	UCrowdySDKSubsystem* UnnamedSdk = NewObject<UCrowdySDKSubsystem>(Instance.Get());
	UnnamedSdk->SetGameSessionForTest(Unnamed);
	UnnamedSdk->SetSignInCollaboratorsForTest(nullptr, UnnamedOwners, NewObject<UCrowdyUDPSubsystem>(Instance.Get()));

	FGameSessionInfo TokenOnly;
	TokenOnly.GameToken = TEXT("app-x");
	UnnamedSdk->SetGameSessionInfo(TokenOnly);
	TestTrue(TEXT("a token with no user signs in"), Unnamed->IsSignedIn() && UnnamedOwners->HasSettledSignIn());
	TokenOnly.GameToken = TEXT("app-y");
	UnnamedSdk->SetGameSessionInfo(TokenOnly);
	TestEqual(TEXT("with no actor record made"), Sent.Num(), 1);

	UnnamedOwners->Deinitialize();
	Owners->Deinitialize();
	return true;
}

// The agent's sign-out hand-off rides on the SDK's Logout, so a Logout that stopped calling it must fail here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySDKLogoutHandsTheStudioAgentAnEmptyTokenTest,
	"CrowdySDK.SDK.LogoutHandsTheStudioAgentAnEmptyToken",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdySDKLogoutHandsTheStudioAgentAnEmptyTokenTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage()));
	UCrowdyGameSession* Session = NewObject<UCrowdyGameSession>(Instance.Get());
	Session->SetUserID(7);
	Session->SetGameToken(TEXT("app-token"));

	UCrowdyStudioAgent* Agent = NewObject<UCrowdyStudioAgent>(Instance.Get());
	Agent->InitializeForTest(nullptr, Session, nullptr);
	TArray<FString> Tokens;
	int64 LastUser = 0;
	Agent->OnAgentTokenChanged.AddLambda([&Tokens, &LastUser](int64 UserId, const FString& Token)
	{
		Tokens.Add(Token);
		LastUser = UserId;
	});

	UCrowdySDKSubsystem* Sdk = NewObject<UCrowdySDKSubsystem>(Instance.Get());
	Sdk->SetGameSessionForTest(Session);
	Sdk->SetStudioAgentForTest(Agent);

	Sdk->Logout();
	if (!TestEqual(TEXT("Logout fires the agent's token event once"), Tokens.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("with an empty token"), Tokens[0].IsEmpty());
	TestEqual(TEXT("for the user who left"), LastUser, static_cast<int64>(7));
	return true;
}

#endif
