#include "CrowdyAuthTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyCppClient.h"
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Subsystem/CrowdyAuthentication.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	constexpr EAutomationTestFlags BlockedTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const TCHAR* const BlockedAnswer = TEXT(
		"{\"errors\":[{\"message\":\"This account is blocked.\","
		"\"extensions\":{\"code\":\"ACCOUNT_BLOCKED\",\"blockedUntil\":\"2026-12-01T00:00:00Z\"}}],\"data\":null}");

	// Far-out expiry so the rotation timer a successful mint arms cannot fire during a test.
	const TCHAR* const BlockedMintAnswer = TEXT(
		"{\"data\":{\"mintAppToken\":{\"token\":\"app-xyz\",\"gameTokenId\":\"5150\",\"appId\":\"31337\","
		"\"expiresAt\":\"2099-01-01T00:00:00Z\",\"gameApiUrl\":\"https://game.test\","
		"\"gameApiWsUrl\":\"wss://game.test\",\"launchUrl\":\"https://launch.test\"}}}");

	/** An authentication subsystem with a stored session token, whose first answers are scripted to be blocked. */
	struct FBlockedRig
	{
		TStrongObjectPtr<UGameInstance> Instance;
		TStrongObjectPtr<UCrowdyAuthentication> Auth;
		TStrongObjectPtr<UCrowdyGameSession> Session;
		TStrongObjectPtr<UCrowdyAuthTestListener> Listener;
		TSharedPtr<FCrowdyCppClient> Client;

		explicit FBlockedRig(int32 BlockedAnswers)
			: Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient))
			, Auth(NewObject<UCrowdyAuthentication>(Instance.Get()))
			, Session(NewObject<UCrowdyGameSession>(Instance.Get()))
			, Listener(NewObject<UCrowdyAuthTestListener>(GetTransientPackage(), NAME_None, RF_Transient))
			, Client(FCrowdyCppClient::MakeForTest(BlockedMintAnswer, 200))
		{
			if (!Client.IsValid())
			{
				return;
			}

			TArray<TPair<int32, FString>> Script;
			for (int32 Index = 0; Index < BlockedAnswers; ++Index)
			{
				Script.Emplace(403, FString(BlockedAnswer));
			}
			Client->SetTestResponseScript(MoveTemp(Script));
			Client->SetManagementToken(TEXT("session-abc"));
			Client->SetGameToken(TEXT("app-old"));

			Session->SetAppID(31337);
			Session->SetUserID(7781);
			Session->SetSessionToken(TEXT("session-abc"));
			Session->SetGameToken(TEXT("app-old"));
			Auth->InitializeForTest(Client, Session.Get());

			Session->OnAccessEnded.AddDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleAccessEnded);
			Auth->OnLogin.AddDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleLogin);
			Auth->OnLoginFailed.AddDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleLoginFailed);
		}

		~FBlockedRig()
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

		void Mint()
		{
			FOnAuthSuccess OnSuccess;
			OnSuccess.BindDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleFlowSuccess);
			FOnAuthError OnError;
			OnError.BindDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleFlowError);
			Auth->MintForTest(OnSuccess, OnError);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAccountBlockedSessionBroadcastsOnceTest,
	"CrowdySDK.Auth.AccountBlocked.SessionBroadcastsOnce", BlockedTestFlags)

bool FCrowdyAccountBlockedSessionBroadcastsOnceTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient));
	const TStrongObjectPtr<UCrowdyGameSession> Session(NewObject<UCrowdyGameSession>(Instance.Get()));
	const TStrongObjectPtr<UCrowdyAuthTestListener> Listener(
		NewObject<UCrowdyAuthTestListener>(GetTransientPackage(), NAME_None, RF_Transient));
	Session->OnAccessEnded.AddDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleAccessEnded);

	TestFalse(TEXT("a new session has not lost access"), Session->HasAccessEnded());

	Session->SetAccessEnded(TEXT("suspended"), TEXT("2026-12-01T00:00:00Z"));
	Session->SetAccessEnded(TEXT("something else"), FString());
	TestTrue(TEXT("the access has ended"), Session->HasAccessEnded());
	TestEqual(TEXT("it is announced once"), Listener->AccessEndedReasons.Num(), 1);
	TestEqual(TEXT("with the first reason"), Session->GetAccessEndedReason(), TEXT("suspended"));
	TestEqual(TEXT("and the first end time"), Session->GetAccessBlockedUntil(), TEXT("2026-12-01T00:00:00Z"));
	TestEqual(TEXT("the listener heard the end time too"),
		Listener->AccessEndedBlockedUntils.Num() == 1 ? Listener->AccessEndedBlockedUntils[0] : FString(),
		TEXT("2026-12-01T00:00:00Z"));

	Session->ClearAccessEnded();
	TestFalse(TEXT("clearing forgets it"), Session->HasAccessEnded());
	TestTrue(TEXT("and the reason"), Session->GetAccessEndedReason().IsEmpty());
	TestTrue(TEXT("and the end time"), Session->GetAccessBlockedUntil().IsEmpty());

	Session->SetAccessEnded(TEXT("revoked"), FString());
	TestEqual(TEXT("after a clear it is announced again"), Listener->AccessEndedReasons.Num(), 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAccountBlockedLateEndIsAnnouncedTest,
	"CrowdySDK.Auth.AccountBlocked.LateEndIsAnnounced", BlockedTestFlags)

bool FCrowdyAccountBlockedLateEndIsAnnouncedTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient));
	const TStrongObjectPtr<UCrowdyGameSession> Session(NewObject<UCrowdyGameSession>(Instance.Get()));
	const TStrongObjectPtr<UCrowdyAuthTestListener> Listener(
		NewObject<UCrowdyAuthTestListener>(GetTransientPackage(), NAME_None, RF_Transient));
	Session->OnAccessEnded.AddDynamic(Listener.Get(), &UCrowdyAuthTestListener::HandleAccessEnded);

	Session->SetAccessEnded(TEXT("access ended: blocked"), FString());
	Session->SetAccessEnded(TEXT("This account is blocked."), TEXT("2026-12-01T00:00:00Z"));
	TestEqual(TEXT("the end time is announced once more"), Listener->AccessEndedReasons.Num(), 2);
	TestEqual(TEXT("and kept"), Session->GetAccessBlockedUntil(), TEXT("2026-12-01T00:00:00Z"));
	TestEqual(TEXT("with the block's reason"), Session->GetAccessEndedReason(), TEXT("This account is blocked."));
	TestEqual(TEXT("the listener heard the end time"),
		Listener->AccessEndedBlockedUntils.Num() == 2 ? Listener->AccessEndedBlockedUntils[1] : FString(),
		TEXT("2026-12-01T00:00:00Z"));

	Session->SetAccessEnded(TEXT("again"), TEXT("2027-01-01T00:00:00Z"));
	TestEqual(TEXT("a later end time is not announced"), Listener->AccessEndedReasons.Num(), 2);
	TestEqual(TEXT("nor kept"), Session->GetAccessBlockedUntil(), TEXT("2026-12-01T00:00:00Z"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAccountBlockedBridgeCarriesTheBlockTest,
	"CrowdySDK.Auth.AccountBlocked.BridgeCarriesTheBlock", BlockedTestFlags)

bool FCrowdyAccountBlockedBridgeCarriesTheBlockTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(BlockedAnswer, 403);
	if (!TestTrue(TEXT("the scripted client was built"), Client.IsValid()))
	{
		return false;
	}

	FCrowdyCppAuthResult SignIn;
	Client->SignInWithPassword(TEXT("a@example.test"), TEXT("p"), [&SignIn](FCrowdyCppAuthResult Result) { SignIn = MoveTemp(Result); });
	Client->Poll();
	TestFalse(TEXT("the blocked sign-in fails"), SignIn.bOk);
	TestEqual(TEXT("with the server's code"), SignIn.ErrorCode, FString(CrowdyCppAccountBlockedCode));
	TestEqual(TEXT("its message"), SignIn.ErrorMessage, TEXT("This account is blocked."));
	TestEqual(TEXT("and the end of the block"), SignIn.BlockedUntil, TEXT("2026-12-01T00:00:00Z"));

	FCrowdyCppAppTokenResult Minted;
	Client->MintAppToken(31337, [&Minted](FCrowdyCppAppTokenResult Result) { Minted = MoveTemp(Result); });
	Client->Poll();
	TestFalse(TEXT("the blocked mint fails"), Minted.bOk);
	TestEqual(TEXT("with the server's code"), Minted.ErrorCode, FString(CrowdyCppAccountBlockedCode));
	TestEqual(TEXT("and the end of the block"), Minted.BlockedUntil, TEXT("2026-12-01T00:00:00Z"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAccountBlockedMintReachesTheSessionTest,
	"CrowdySDK.Auth.AccountBlocked.MintReachesTheSession", BlockedTestFlags)

bool FCrowdyAccountBlockedMintReachesTheSessionTest::RunTest(const FString& Parameters)
{
	FBlockedRig Rig(1);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Mint();
	Rig.Drain();

	TestTrue(TEXT("the session lost access"), Rig.Session->HasAccessEnded());
	TestEqual(TEXT("announced once"), Rig.Listener->AccessEndedReasons.Num(), 1);
	TestEqual(TEXT("with the server's message"), Rig.Session->GetAccessEndedReason(), TEXT("This account is blocked."));
	TestEqual(TEXT("and the end of the block"), Rig.Session->GetAccessBlockedUntil(), TEXT("2026-12-01T00:00:00Z"));
	TestEqual(TEXT("the sign-in failed with the same reason"),
		Rig.Listener->FlowErrors.Num() == 1 ? Rig.Listener->FlowErrors[0] : FString(), TEXT("This account is blocked."));
	TestEqual(TEXT("and announced it"), Rig.Listener->LoginFailures.Num(), 1);
	TestEqual(TEXT("it did not succeed"), Rig.Listener->FlowSuccesses, 0);
	TestEqual(TEXT("and nothing is retried"), Rig.Client->NumPendingRequests(), 0);
	TestFalse(TEXT("no rotation is armed"), Rig.Auth->IsRotationTimerArmedForTest());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAccountBlockedRefreshDoesNotRetryTest,
	"CrowdySDK.Auth.AccountBlocked.RefreshDoesNotRetry", BlockedTestFlags)

bool FCrowdyAccountBlockedRefreshDoesNotRetryTest::RunTest(const FString& Parameters)
{
	FBlockedRig Rig(1);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Auth->RefreshAppToken();
	Rig.Drain();

	TestTrue(TEXT("the session lost access"), Rig.Session->HasAccessEnded());
	TestEqual(TEXT("with the end of the block"), Rig.Session->GetAccessBlockedUntil(), TEXT("2026-12-01T00:00:00Z"));
	FString Body;
	Rig.Client->GetLastTestRequestBody(Body);
	TestFalse(TEXT("no re-mint followed the refusal"), Body.Contains(TEXT("mintAppToken")));
	TestEqual(TEXT("nothing is pending"), Rig.Client->NumPendingRequests(), 0);
	TestFalse(TEXT("no rotation retry is armed"), Rig.Auth->IsRotationTimerArmedForTest());
	TestEqual(TEXT("no retry was spent"), Rig.Auth->GetRotationRetriesUsedForTest(), 0);
	TestEqual(TEXT("the background refresh reported no failure"), Rig.Listener->LoginFailures.Num(), 0);

	Rig.Auth->RefreshAppToken();
	Rig.Auth->RecoverExpiredAppToken();
	TestEqual(TEXT("later rotation triggers send nothing"), Rig.Client->NumPendingRequests(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAccountBlockedSignInClearsItTest,
	"CrowdySDK.Auth.AccountBlocked.SignInClearsIt", BlockedTestFlags)

bool FCrowdyAccountBlockedSignInClearsItTest::RunTest(const FString& Parameters)
{
	FBlockedRig Rig(1);
	if (!TestTrue(TEXT("the scripted client was built"), Rig.Client.IsValid()))
	{
		return false;
	}

	Rig.Mint();
	Rig.Drain();
	if (!TestTrue(TEXT("the session lost access"), Rig.Session->HasAccessEnded()))
	{
		return false;
	}

	Rig.Mint();
	Rig.Drain();

	TestEqual(TEXT("the second sign-in succeeded"), Rig.Listener->FlowSuccesses, 1);
	TestFalse(TEXT("so the session has access again"), Rig.Session->HasAccessEnded());
	TestTrue(TEXT("with no reason left"), Rig.Session->GetAccessEndedReason().IsEmpty());
	TestTrue(TEXT("and no end time"), Rig.Session->GetAccessBlockedUntil().IsEmpty());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
