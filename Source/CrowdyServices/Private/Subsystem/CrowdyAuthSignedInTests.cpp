#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Subsystem/CrowdyAuthentication.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UObject/StrongObjectPtr.h"

// IsSignedIn answers about this process, and the state it reads is the only part of that answer a test can reach
// without a server: which fields of the live session have been written, and in what order the sign-in pipeline
// writes them. The identity token is stored before the app token is minted, so the interesting case is the window
// in between, where a saved credential exists and the client is still not authorized.
namespace
{
	constexpr EAutomationTestFlags AuthStateTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A game-instance subsystem resolves its owner from its outer, so both objects are built under a real
	// UGameInstance rather than the transient package. Strong pointers keep them past a collection mid-test.
	struct FAuthFixture
	{
		TStrongObjectPtr<UGameInstance> Instance;
		TStrongObjectPtr<UCrowdyAuthentication> Auth;
		TStrongObjectPtr<UCrowdyGameSession> Session;

		FAuthFixture()
			: Instance(NewObject<UGameInstance>(GetTransientPackage()))
		{
			Auth.Reset(NewObject<UCrowdyAuthentication>(Instance.Get()));
			Session.Reset(NewObject<UCrowdyGameSession>(Instance.Get()));
		}

		bool IsUsable() const
		{
			return Instance.IsValid() && Auth.IsValid() && Session.IsValid();
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthIsSignedInWithoutGameSessionTest,
	"CrowdySDK.Services.AuthIsSignedInWithoutGameSession", AuthStateTestFlags)

bool FCrowdyAuthIsSignedInWithoutGameSessionTest::RunTest(const FString& Parameters)
{
	FAuthFixture Fixture;
	if (!TestTrue(TEXT("the fixture was constructed"), Fixture.IsUsable()))
	{
		return false;
	}

	// Sign-in can be asked about before the SDK has injected anything, so the accessor has to answer rather than
	// dereference the session it does not have.
	TestFalse(TEXT("an uninjected subsystem is not signed in"), Fixture.Auth->IsSignedIn());

	Fixture.Auth->InjectDependencies(nullptr);
	TestFalse(TEXT("injecting no session is still not signed in"), Fixture.Auth->IsSignedIn());

	Fixture.Auth->InjectDependencies(Fixture.Session.Get());
	TestFalse(TEXT("a session carrying nothing is not signed in"), Fixture.Auth->IsSignedIn());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAuthIsSignedInTracksLiveSessionTest,
	"CrowdySDK.Services.AuthIsSignedInTracksLiveSession", AuthStateTestFlags)

bool FCrowdyAuthIsSignedInTracksLiveSessionTest::RunTest(const FString& Parameters)
{
	FAuthFixture Fixture;
	if (!TestTrue(TEXT("the fixture was constructed"), Fixture.IsUsable()))
	{
		return false;
	}

	Fixture.Auth->InjectDependencies(Fixture.Session.Get());

	// What the pipeline has written by the time it dispatches the mint: an identity is known and its session token is
	// stored and persisted, but no app token exists yet. A sign-in or restore that then fails leaves exactly this
	// behind, so reading it as signed in would put a login screen in the same wrong state HasSavedSession does.
	Fixture.Session->SetUserID(7781);
	Fixture.Session->SetSessionToken(TEXT("session-placeholder"));
	TestFalse(TEXT("a stored identity token alone is not a sign-in"), Fixture.Auth->IsSignedIn());

	// The app token is applied last, at the point the success delegates fire.
	Fixture.Session->SetGameToken(TEXT("app-placeholder"));
	TestTrue(TEXT("a user id with an app token reads as signed in"), Fixture.Auth->IsSignedIn());

	// An app token with no user behind it identifies nobody to be signed in as.
	Fixture.Session->SetUserID(0);
	TestFalse(TEXT("an app token with no user is not a sign-in"), Fixture.Auth->IsSignedIn());

	Fixture.Session->SetUserID(7781);
	TestTrue(TEXT("restoring the user id restores the answer"), Fixture.Auth->IsSignedIn());

	// Logout clears the live session, and the accessor has to follow it down within the same process.
	Fixture.Session->ClearCurrentSessionData();
	TestFalse(TEXT("a cleared session is not signed in"), Fixture.Auth->IsSignedIn());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
