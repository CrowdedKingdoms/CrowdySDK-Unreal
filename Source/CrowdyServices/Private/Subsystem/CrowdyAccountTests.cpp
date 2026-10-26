#include "CrowdyAuthTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystem/AsyncActions/CrowdyAccountActions.h"
#include "Subsystem/CrowdyAuthentication.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdyPlayers.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	constexpr EAutomationTestFlags AccountTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Far-out expiry so the rotation timer a successful mint arms cannot fire during a test.
	const TCHAR* const AccountMintAnswer = TEXT(
		"{\"data\":{\"mintAppToken\":{\"token\":\"app-xyz\",\"gameTokenId\":\"5150\",\"appId\":\"31337\","
		"\"expiresAt\":\"2099-01-01T00:00:00Z\",\"gameApiUrl\":\"https://game.test\","
		"\"gameApiWsUrl\":\"wss://game.test\",\"launchUrl\":\"https://launch.test\"}}}");

	FString AccessRowAnswer(const TCHAR* Field, const TCHAR* Status)
	{
		return FString::Printf(TEXT("{\"data\":{\"%s\":{\"appUserAccessId\":\"1\",\"appId\":\"31337\",\"userId\":\"7781\","
			"\"tierId\":\"900\",\"status\":\"%s\",\"grantedBy\":\"system\",\"subscriptionId\":null,\"expiresAt\":null,"
			"\"suspendedUntil\":null,\"createdAt\":\"2026-10-01T00:00:00Z\",\"updatedAt\":\"2026-10-01T00:00:00Z\"}}}"), Field, Status);
	}

	TSharedPtr<FJsonObject> ParseObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
		return Object;
	}

	/** An authentication subsystem signed in as player 7781 of app 31337, on a client that answers Answer. */
	struct FAccountRig
	{
		TStrongObjectPtr<UGameInstance> Instance;
		TStrongObjectPtr<UCrowdyAuthentication> Auth;
		TStrongObjectPtr<UCrowdyGameSession> Session;
		TStrongObjectPtr<UCrowdyAuthTestListener> AuthListener;
		TStrongObjectPtr<UCrowdyAccountTestListener> Listener;
		TSharedPtr<FCrowdyCppClient> Client;
		int32 ServerSessionEnds = 0;

		explicit FAccountRig(const FString& Answer, int32 Status = 200)
			: Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient))
			, Auth(NewObject<UCrowdyAuthentication>(Instance.Get()))
			, Session(NewObject<UCrowdyGameSession>(Instance.Get()))
			, AuthListener(NewObject<UCrowdyAuthTestListener>(GetTransientPackage(), NAME_None, RF_Transient))
			, Listener(NewObject<UCrowdyAccountTestListener>(GetTransientPackage(), NAME_None, RF_Transient))
			, Client(FCrowdyCppClient::MakeForTest(Answer, Status))
		{
			if (!Client.IsValid())
			{
				return;
			}
			Client->SetManagementToken(TEXT("session-abc"));
			Client->SetGameToken(TEXT("app-old"));

			Session->SetAppID(31337);
			Session->SetUserID(7781);
			Session->SetSessionToken(TEXT("session-abc"));
			Session->SetGameToken(TEXT("app-old"));
			Auth->InitializeForTest(Client, Session.Get());
			Auth->OnServerSessionEnded.AddLambda([this]() { ++ServerSessionEnds; });
			Auth->OnLogin.AddDynamic(AuthListener.Get(), &UCrowdyAuthTestListener::HandleLogin);
		}

		~FAccountRig()
		{
			Auth->Deinitialize();
		}

		void Drain()
		{
			for (int32 Pass = 0; Client && Pass < 8 && Client->NumPendingRequests() > 0; ++Pass)
			{
				Client->Poll();
			}
		}

		FOnAccountDone Done() const
		{
			FOnAccountDone Delegate;
			Delegate.BindDynamic(Listener.Get(), &UCrowdyAccountTestListener::HandleDone);
			return Delegate;
		}

		FOnAccountError Failed() const
		{
			FOnAccountError Delegate;
			Delegate.BindDynamic(Listener.Get(), &UCrowdyAccountTestListener::HandleError);
			return Delegate;
		}

		FString LastAuthorization() const
		{
			FString Url;
			FString Authorization;
			Client->GetLastTestRequest(Url, Authorization);
			return Authorization;
		}

		FString LastBody() const
		{
			FString Body;
			Client->GetLastTestRequestBody(Body);
			return Body;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAccountErrorCodesTest, "CrowdySDK.Account.ErrorCodes", AccountTestFlags)

bool FCrowdyAccountErrorCodesTest::RunTest(const FString& Parameters)
{
	auto CodeOf = [](const TCHAR* ServerCode) { return FCrowdyAccountError::FromFailure(TEXT("why"), ServerCode).Code; };
	TestEqual(TEXT("no code never reached a verdict"), CodeOf(TEXT("")), ECrowdyAccountErrorCode::NetworkError);
	TestEqual(TEXT("UNAUTHENTICATED"), CodeOf(TEXT("UNAUTHENTICATED")), ECrowdyAccountErrorCode::NotSignedIn);
	TestEqual(TEXT("INVALID_CURRENT_PASSWORD"), CodeOf(TEXT("INVALID_CURRENT_PASSWORD")), ECrowdyAccountErrorCode::WrongPassword);
	TestEqual(TEXT("PASSWORD_ALREADY_SET"), CodeOf(TEXT("PASSWORD_ALREADY_SET")), ECrowdyAccountErrorCode::PasswordAlreadySet);
	TestEqual(TEXT("PASSWORD_NOT_SET"), CodeOf(TEXT("PASSWORD_NOT_SET")), ECrowdyAccountErrorCode::NoPassword);
	TestEqual(TEXT("RATE_LIMITED"), CodeOf(TEXT("RATE_LIMITED")), ECrowdyAccountErrorCode::RateLimited);
	TestEqual(TEXT("GAMERTAG_ALREADY_EXISTS"), CodeOf(TEXT("GAMERTAG_ALREADY_EXISTS")), ECrowdyAccountErrorCode::NameTaken);
	TestEqual(TEXT("any other code is the server's"), CodeOf(TEXT("FORBIDDEN")), ECrowdyAccountErrorCode::ServerError);
	TestEqual(TEXT("the message rides along"), FCrowdyAccountError::FromFailure(TEXT("why"), TEXT("X")).Message, TEXT("why"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAccountAccessStatesTest, "CrowdySDK.Account.AccessStates", AccountTestFlags)

bool FCrowdyAccountAccessStatesTest::RunTest(const FString& Parameters)
{
	const FDateTime Now(2026, 10, 16, 12, 0, 0);
	auto StateOf = [&Now](const TCHAR* Json) { return FCrowdyAppAccess::FromJson(ParseObject(Json), Now).State; };

	TestEqual(TEXT("no row is None"), FCrowdyAppAccess::FromJson(nullptr, Now).State, ECrowdyAccessState::None);
	TestEqual(TEXT("active"), StateOf(TEXT("{\"status\":\"active\"}")), ECrowdyAccessState::Active);
	TestEqual(TEXT("revoked"), StateOf(TEXT("{\"status\":\"revoked\"}")), ECrowdyAccessState::Revoked);
	TestEqual(TEXT("a suspension still running"),
		StateOf(TEXT("{\"status\":\"active\",\"suspendedUntil\":\"2026-10-17T00:00:00Z\"}")), ECrowdyAccessState::Suspended);
	TestEqual(TEXT("a suspension that has passed"),
		StateOf(TEXT("{\"status\":\"active\",\"suspendedUntil\":\"2026-10-15T00:00:00Z\"}")), ECrowdyAccessState::Active);
	TestEqual(TEXT("an expired grant"),
		StateOf(TEXT("{\"status\":\"active\",\"expiresAt\":\"2026-10-15T00:00:00Z\"}")), ECrowdyAccessState::Expired);
	TestEqual(TEXT("a grant still running"),
		StateOf(TEXT("{\"status\":\"active\",\"expiresAt\":\"2026-11-01T00:00:00Z\"}")), ECrowdyAccessState::Active);

	const FCrowdyAppAccess Free = FCrowdyAppAccess::FromJson(
		ParseObject(TEXT("{\"status\":\"active\",\"tierId\":\"900\",\"grantedBy\":\"system\"}")), Now);
	TestEqual(TEXT("the tier is read"), Free.TierID, static_cast<int64>(900));
	TestTrue(TEXT("a system grant is free"), Free.bFreeGrant);
	TestFalse(TEXT("an administrator's grant is not"),
		FCrowdyAppAccess::FromJson(ParseObject(TEXT("{\"status\":\"active\",\"grantedBy\":\"81\"}")), Now).bFreeGrant);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAccountSignOutEndsServerSessionTest, "CrowdySDK.Account.SignOutEndsServerSession",
	AccountTestFlags)

bool FCrowdyAccountSignOutEndsServerSessionTest::RunTest(const FString& Parameters)
{
	FAccountRig Rig(TEXT("{\"data\":{\"logout\":true}}"));
	if (!TestTrue(TEXT("the test client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Auth->EndServerSession();
	// Sign-out scrubs the bearer straight after; the request already carries it.
	Rig.Client->SetManagementToken(FString());
	Rig.Drain();

	TestTrue(TEXT("logout was sent"), Rig.LastBody().Contains(TEXT("logout")));
	TestEqual(TEXT("under the session token"), Rig.LastAuthorization(), TEXT("Bearer session-abc"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAccountSignedOutSendsNoLogoutTest, "CrowdySDK.Account.SignedOutSendsNoLogout",
	AccountTestFlags)

bool FCrowdyAccountSignedOutSendsNoLogoutTest::RunTest(const FString& Parameters)
{
	FAccountRig Rig(TEXT("{\"data\":{\"logout\":true}}"));
	if (!TestTrue(TEXT("the test client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Session->SetSessionToken(FString());
	Rig.Auth->EndServerSession();
	FString Body;
	TestFalse(TEXT("nothing is sent without a session"), Rig.Client->GetLastTestRequestBody(Body));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAccountDeleteSignsOutTest, "CrowdySDK.Account.DeleteSignsOut", AccountTestFlags)

bool FCrowdyAccountDeleteSignsOutTest::RunTest(const FString& Parameters)
{
	FAccountRig Rig(TEXT("{\"data\":{\"deleteMyAccount\":true}}"));
	if (!TestTrue(TEXT("the test client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Auth->DeleteAccount(Rig.Done(), Rig.Failed());
	Rig.Drain();

	TestEqual(TEXT("it succeeded"), Rig.Listener->Dones, 1);
	TestEqual(TEXT("under the app token"), Rig.LastAuthorization(), TEXT("Bearer app-old"));
	TestEqual(TEXT("the SDK is told to sign out"), Rig.ServerSessionEnds, 1);
	TestTrue(TEXT("and the ended session is forgotten"), Rig.Session->GetSessionToken().IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAccountRefusedDeleteKeepsSessionTest, "CrowdySDK.Account.RefusedDeleteKeepsSession",
	AccountTestFlags)

bool FCrowdyAccountRefusedDeleteKeepsSessionTest::RunTest(const FString& Parameters)
{
	FAccountRig Rig(TEXT("{\"errors\":[{\"message\":\"Not now.\",\"extensions\":{\"code\":\"FORBIDDEN\"}}],\"data\":null}"), 403);
	if (!TestTrue(TEXT("the test client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Auth->DeleteAccount(Rig.Done(), Rig.Failed());
	Rig.Drain();

	TestEqual(TEXT("it failed"), Rig.Listener->Errors.Num(), 1);
	TestEqual(TEXT("nothing signs out"), Rig.ServerSessionEnds, 0);
	TestEqual(TEXT("the session stays"), Rig.Session->GetSessionToken(), TEXT("session-abc"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAccountSignOutEverywhereTest, "CrowdySDK.Account.SignOutEverywhere", AccountTestFlags)

bool FCrowdyAccountSignOutEverywhereTest::RunTest(const FString& Parameters)
{
	FAccountRig Rig(TEXT("{\"data\":{\"logoutAllDevices\":true}}"));
	if (!TestTrue(TEXT("the test client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Auth->SignOutEverywhere(Rig.Done(), Rig.Failed());
	Rig.Drain();

	TestEqual(TEXT("it succeeded"), Rig.Listener->Dones, 1);
	TestTrue(TEXT("logoutAllDevices was sent"), Rig.LastBody().Contains(TEXT("logoutAllDevices")));
	TestEqual(TEXT("under the session token"), Rig.LastAuthorization(), TEXT("Bearer session-abc"));
	TestEqual(TEXT("the SDK is told to sign out"), Rig.ServerSessionEnds, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAccountWrongPasswordTest, "CrowdySDK.Account.WrongPassword", AccountTestFlags)

bool FCrowdyAccountWrongPasswordTest::RunTest(const FString& Parameters)
{
	FAccountRig Rig(TEXT("{\"errors\":[{\"message\":\"Invalid current password\","
		"\"extensions\":{\"code\":\"INVALID_CURRENT_PASSWORD\"}}],\"data\":null}"), 403);
	if (!TestTrue(TEXT("the test client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Auth->ChangePassword(TEXT("old"), TEXT("new"), Rig.Done(), Rig.Failed());
	Rig.Drain();

	TestEqual(TEXT("it failed once"), Rig.Listener->Errors.Num(), 1);
	TestEqual(TEXT("as a wrong password"),
		Rig.Listener->Errors.IsEmpty() ? ECrowdyAccountErrorCode::Unknown : Rig.Listener->Errors[0].Code,
		ECrowdyAccountErrorCode::WrongPassword);
	TestEqual(TEXT("the session stays"), Rig.Session->GetSessionToken(), TEXT("session-abc"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAccountPasswordIsClearedAfterSendTest, "CrowdySDK.Account.PasswordIsClearedAfterSend",
	AccountTestFlags)

bool FCrowdyAccountPasswordIsClearedAfterSendTest::RunTest(const FString& Parameters)
{
	FAccountRig Rig(TEXT("{\"data\":{\"changePassword\":true}}"));
	if (!TestTrue(TEXT("the test client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	TArray<TPair<int32, FString>> Script;
	Script.Emplace(200, FString(TEXT("{\"data\":{\"changePassword\":true}}")));
	Script.Emplace(200, FString(TEXT("{\"data\":{\"setInitialPassword\":true}}")));
	Rig.Client->SetTestResponseScript(MoveTemp(Script));

	const TStrongObjectPtr<UCrowdyAccount_ChangePassword> Change(NewObject<UCrowdyAccount_ChangePassword>());
	Change->CurrentPassword = TEXT("current-pw-1");
	Change->NewPassword = TEXT("changed-pw-2");
	Change->Send(*Rig.Auth, Rig.Done(), Rig.Failed());
	Rig.Drain();

	TestTrue(TEXT("the change carried the new password"), Rig.LastBody().Contains(TEXT("changed-pw-2")));
	TestTrue(TEXT("the current password is gone"), Change->CurrentPassword.IsEmpty());
	TestTrue(TEXT("and the new one"), Change->NewPassword.IsEmpty());

	const TStrongObjectPtr<UCrowdyAccount_AddPassword> Add(NewObject<UCrowdyAccount_AddPassword>());
	Add->NewPassword = TEXT("added-pw-3");
	Add->Send(*Rig.Auth, Rig.Done(), Rig.Failed());
	Rig.Drain();

	TestTrue(TEXT("the add carried the password"), Rig.LastBody().Contains(TEXT("added-pw-3")));
	TestTrue(TEXT("and it is gone from the action"), Add->NewPassword.IsEmpty());
	TestEqual(TEXT("both calls were answered"), Rig.Listener->Dones, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAccountNoSessionSendsNothingTest, "CrowdySDK.Account.NoSessionSendsNothing",
	AccountTestFlags)

bool FCrowdyAccountNoSessionSendsNothingTest::RunTest(const FString& Parameters)
{
	FAccountRig Rig(TEXT("{\"data\":{\"changePassword\":true}}"));
	if (!TestTrue(TEXT("the test client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Session->SetSessionToken(FString());
	Rig.Auth->ChangePassword(TEXT("old"), TEXT("new"), Rig.Done(), Rig.Failed());
	Rig.Auth->AddPassword(TEXT("new"), Rig.Done(), Rig.Failed());
	Rig.Auth->SignOutEverywhere(Rig.Done(), Rig.Failed());
	Rig.Auth->GetMyAccess(FOnAppAccessRead(), Rig.Failed());

	FString Body;
	TestFalse(TEXT("nothing was sent"), Rig.Client->GetLastTestRequestBody(Body));
	TestEqual(TEXT("each failed"), Rig.Listener->Errors.Num(), 4);
	for (const FCrowdyAccountError& Error : Rig.Listener->Errors)
	{
		TestEqual(TEXT("as not signed in"), Error.Code, ECrowdyAccountErrorCode::NotSignedIn);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAccountNoAccessIsNoneTest, "CrowdySDK.Account.NoAccessIsNone", AccountTestFlags)

bool FCrowdyAccountNoAccessIsNoneTest::RunTest(const FString& Parameters)
{
	FAccountRig Rig(TEXT("{\"data\":{\"myAppAccess\":null}}"));
	if (!TestTrue(TEXT("the test client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	FOnAppAccessRead Read;
	Read.BindDynamic(Rig.Listener.Get(), &UCrowdyAccountTestListener::HandleAccess);
	Rig.Auth->GetMyAccess(Read, Rig.Failed());
	Rig.Drain();

	TestEqual(TEXT("it was read"), Rig.Listener->Accesses.Num(), 1);
	TestEqual(TEXT("as no access"), Rig.Listener->Accesses.IsEmpty() ? ECrowdyAccessState::Active : Rig.Listener->Accesses[0].State,
		ECrowdyAccessState::None);
	TestEqual(TEXT("under the session token"), Rig.LastAuthorization(), TEXT("Bearer session-abc"));
	TestTrue(TEXT("for this app"), Rig.LastBody().Contains(TEXT("31337")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAccountClaimFinishesSignInTest, "CrowdySDK.Account.ClaimFinishesSignIn", AccountTestFlags)

bool FCrowdyAccountClaimFinishesSignInTest::RunTest(const FString& Parameters)
{
	FAccountRig Rig(AccountMintAnswer);
	if (!TestTrue(TEXT("the test client was built"), Rig.Client.IsValid()))
	{
		return false;
	}
	// The sign-in this follows was refused, so no app token is held.
	Rig.Session->SetGameToken(FString());
	TArray<TPair<int32, FString>> Script;
	Script.Emplace(200, AccessRowAnswer(TEXT("claimFreeAppAccess"), TEXT("active")));
	Rig.Client->SetTestResponseScript(MoveTemp(Script));

	FOnAuthSuccess OnSuccess;
	OnSuccess.BindDynamic(Rig.AuthListener.Get(), &UCrowdyAuthTestListener::HandleFlowSuccess);
	FOnAuthError OnError;
	OnError.BindDynamic(Rig.AuthListener.Get(), &UCrowdyAuthTestListener::HandleFlowError);
	Rig.Auth->ClaimFreeAccess(OnSuccess, OnError);
	Rig.Drain();

	TestEqual(TEXT("it succeeded"), Rig.AuthListener->FlowSuccesses, 1);
	TestEqual(TEXT("as a sign-in"), Rig.AuthListener->Logins, 1);
	TestEqual(TEXT("with the minted token"), Rig.Session->GetGameToken(), TEXT("app-xyz"));
	TestTrue(TEXT("and no error"), Rig.AuthListener->FlowErrors.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAccountClaimRefusedByRevokeTest, "CrowdySDK.Account.ClaimRefusedByRevoke", AccountTestFlags)

bool FCrowdyAccountClaimRefusedByRevokeTest::RunTest(const FString& Parameters)
{
	FAccountRig Rig(AccountMintAnswer);
	if (!TestTrue(TEXT("the test client was built"), Rig.Client.IsValid()))
	{
		return false;
	}
	Rig.Session->SetGameToken(FString());
	TArray<TPair<int32, FString>> Script;
	Script.Emplace(200, AccessRowAnswer(TEXT("claimFreeAppAccess"), TEXT("revoked")));
	Rig.Client->SetTestResponseScript(MoveTemp(Script));

	FOnAuthSuccess OnSuccess;
	OnSuccess.BindDynamic(Rig.AuthListener.Get(), &UCrowdyAuthTestListener::HandleFlowSuccess);
	FOnAuthError OnError;
	OnError.BindDynamic(Rig.AuthListener.Get(), &UCrowdyAuthTestListener::HandleFlowError);
	Rig.Auth->ClaimFreeAccess(OnSuccess, OnError);
	Rig.Drain();

	TestEqual(TEXT("it failed"), Rig.AuthListener->FlowErrors.Num(), 1);
	TestEqual(TEXT("without signing in"), Rig.AuthListener->Logins, 0);
	TestTrue(TEXT("and no token was minted"), Rig.Session->GetGameToken().IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAccountClaimAnsweringAfterLogoutIsDroppedTest,
	"CrowdySDK.Account.ClaimAnsweringAfterLogoutIsDropped", AccountTestFlags)

bool FCrowdyAccountClaimAnsweringAfterLogoutIsDroppedTest::RunTest(const FString& Parameters)
{
	FAccountRig Rig(AccountMintAnswer);
	if (!TestTrue(TEXT("the test client was built"), Rig.Client.IsValid()))
	{
		return false;
	}
	// Logout deletes the saved session, so this instance gets a vault and a slot of its own.
	Rig.Auth->SetInstanceSuffixForTest(TEXT(".autotest-stale-claim"));
	Rig.Session->SetGameToken(FString());
	TArray<TPair<int32, FString>> Script;
	Script.Emplace(200, AccessRowAnswer(TEXT("claimFreeAppAccess"), TEXT("active")));
	Rig.Client->SetTestResponseScript(MoveTemp(Script));

	FOnAuthSuccess OnSuccess;
	OnSuccess.BindDynamic(Rig.AuthListener.Get(), &UCrowdyAuthTestListener::HandleFlowSuccess);
	FOnAuthError OnError;
	OnError.BindDynamic(Rig.AuthListener.Get(), &UCrowdyAuthTestListener::HandleFlowError);
	Rig.Auth->ClaimFreeAccess(OnSuccess, OnError);
	Rig.Auth->ClearSavedSession();
	Rig.Drain();

	TestEqual(TEXT("the claim does not succeed"), Rig.AuthListener->FlowSuccesses, 0);
	TestEqual(TEXT("nor sign in"), Rig.AuthListener->Logins, 0);
	TestEqual(TEXT("it fails once instead"), Rig.AuthListener->FlowErrors.Num(), 1);
	TestTrue(TEXT("and no token was minted"), Rig.Session->GetGameToken().IsEmpty());
	return true;
}

namespace
{
	/** A players subsystem on a client that answers Answer. */
	struct FPlayersRig
	{
		TStrongObjectPtr<UGameInstance> Instance;
		TStrongObjectPtr<UCrowdyPlayers> Players;
		TStrongObjectPtr<UCrowdyAccountTestListener> Listener;
		TSharedPtr<FCrowdyCppClient> Client;

		explicit FPlayersRig(const FString& Answer)
			: Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient))
			, Players(NewObject<UCrowdyPlayers>(Instance.Get()))
			, Listener(NewObject<UCrowdyAccountTestListener>(GetTransientPackage(), NAME_None, RF_Transient))
			, Client(FCrowdyCppClient::MakeForTest(Answer, 200))
		{
			if (Client.IsValid())
			{
				Client->SetGameToken(TEXT("app-old"));
				Client->SetManagementToken(TEXT("session-abc"));
				Players->InitializeForTest(Client);
			}
		}

		void Drain()
		{
			for (int32 Pass = 0; Client && Pass < 8 && Client->NumPendingRequests() > 0; ++Pass)
			{
				Client->Poll();
			}
		}

		FOnPlayerProfileRead Read() const
		{
			FOnPlayerProfileRead Delegate;
			Delegate.BindDynamic(Listener.Get(), &UCrowdyAccountTestListener::HandleProfile);
			return Delegate;
		}

		FOnPlayersError Failed() const
		{
			FOnPlayersError Delegate;
			Delegate.BindDynamic(Listener.Get(), &UCrowdyAccountTestListener::HandleError);
			return Delegate;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPlayersRenameSendsBothTest, "CrowdySDK.Players.RenameSendsBoth", AccountTestFlags)

bool FCrowdyPlayersRenameSendsBothTest::RunTest(const FString& Parameters)
{
	FPlayersRig Rig(TEXT("{\"data\":{\"updateGamertag\":{\"userId\":\"7781\",\"gamertag\":\"ada\",\"disambiguation\":\"0042\",\"userType\":\"player\"}}}"));
	if (!TestTrue(TEXT("the test client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Players->SetPlayerName(TEXT("ada"), TEXT("0042"), Rig.Read(), Rig.Failed());
	Rig.Drain();

	FString Body;
	Rig.Client->GetLastTestRequestBody(Body);
	TestTrue(TEXT("the gamertag is sent"), Body.Contains(TEXT("\"gamertag\":\"ada\"")));
	TestTrue(TEXT("and the disambiguation"), Body.Contains(TEXT("\"disambiguation\":\"0042\"")));
	FString Url;
	FString Authorization;
	Rig.Client->GetLastTestRequest(Url, Authorization);
	TestEqual(TEXT("under the session token, which a rename needs"), Authorization, TEXT("Bearer session-abc"));
	TestEqual(TEXT("the new name is read back"), Rig.Listener->Profiles.Num(), 1);
	TestEqual(TEXT("with its disambiguation"),
		Rig.Listener->Profiles.IsEmpty() ? FString() : Rig.Listener->Profiles[0].Disambiguation, TEXT("0042"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPlayersTakenNameTest, "CrowdySDK.Players.TakenNameIsNameTaken", AccountTestFlags)

bool FCrowdyPlayersTakenNameTest::RunTest(const FString& Parameters)
{
	FPlayersRig Rig(TEXT("{\"errors\":[{\"message\":\"That gamertag is taken.\",\"extensions\":{\"code\":\"CONFLICT\"}}],\"data\":null}"));
	if (!TestTrue(TEXT("the test client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Players->SetPlayerName(TEXT("ada"), TEXT("0042"), Rig.Read(), Rig.Failed());
	Rig.Drain();

	TestEqual(TEXT("it failed"), Rig.Listener->Errors.Num(), 1);
	TestEqual(TEXT("as Name Taken, from the server's code"),
		Rig.Listener->Errors.IsEmpty() ? ECrowdyAccountErrorCode::Unknown : Rig.Listener->Errors[0].Code, ECrowdyAccountErrorCode::NameTaken);
	TestEqual(TEXT("with the server's message"), Rig.Listener->Errors.IsEmpty() ? FString() : Rig.Listener->Errors[0].Message, TEXT("That gamertag is taken."));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPlayersUnknownPlayerTest, "CrowdySDK.Players.UnknownPlayerIsNotFound", AccountTestFlags)

bool FCrowdyPlayersUnknownPlayerTest::RunTest(const FString& Parameters)
{
	FPlayersRig Rig(TEXT("{\"data\":{\"playerProfile\":null}}"));
	if (!TestTrue(TEXT("the test client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Players->GetPlayerName(9999, Rig.Read(), Rig.Failed());
	Rig.Drain();

	TestEqual(TEXT("it failed"), Rig.Listener->Errors.Num(), 1);
	TestEqual(TEXT("as not found"),
		Rig.Listener->Errors.IsEmpty() ? ECrowdyAccountErrorCode::Unknown : Rig.Listener->Errors[0].Code, ECrowdyAccountErrorCode::NotFound);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPlayersNamesBoundsTest, "CrowdySDK.Players.NamesBounds", AccountTestFlags)

bool FCrowdyPlayersNamesBoundsTest::RunTest(const FString& Parameters)
{
	FPlayersRig Rig(TEXT("{\"data\":{\"playerProfiles\":[{\"userId\":\"1\",\"gamertag\":\"a\",\"disambiguation\":\"1\"},"
		"{\"userId\":\"2\",\"gamertag\":null,\"disambiguation\":null}]}}"));
	if (!TestTrue(TEXT("the test client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	FOnPlayerProfilesRead Read;
	Read.BindDynamic(Rig.Listener.Get(), &UCrowdyAccountTestListener::HandleProfiles);

	TArray<int64> TooMany;
	for (int64 Id = 1; Id <= UCrowdyPlayers::MaxProfilesPerCall + 1; ++Id)
	{
		TooMany.Add(Id);
	}
	Rig.Players->GetPlayerNames(TooMany, Read, Rig.Failed());
	Rig.Players->GetPlayerNames(TArray<int64>(), Read, Rig.Failed());
	FString Body;
	TestFalse(TEXT("neither sends a request"), Rig.Client->GetLastTestRequestBody(Body));
	TestEqual(TEXT("too many fails"), Rig.Listener->Errors.Num(), 1);
	TestEqual(TEXT("none answers empty"), Rig.Listener->ProfileLists.Num(), 1);

	Rig.Players->GetPlayerNames({ 1, 2 }, Read, Rig.Failed());
	Rig.Drain();
	TestEqual(TEXT("two names were read"), Rig.Listener->ProfileLists.Num() == 2 ? Rig.Listener->ProfileLists[1].Num() : 0, 2);
	TestTrue(TEXT("a player with no name reads empty"),
		Rig.Listener->ProfileLists.Num() == 2 && Rig.Listener->ProfileLists[1].Num() == 2 && Rig.Listener->ProfileLists[1][1].Gamertag.IsEmpty());
	return true;
}

#endif
