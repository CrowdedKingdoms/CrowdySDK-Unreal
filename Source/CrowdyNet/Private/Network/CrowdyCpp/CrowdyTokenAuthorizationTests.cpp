// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Network/CrowdyCpp/CrowdyTokenAuthorization.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// A rotated app token either lets the connection keep the server it is on or it does not, and getting that
// answer wrong in the permissive direction is silent: the client carries on signing datagrams with a token the
// server was never given, and a server drops those without saying anything. So the no cases are the ones worth
// stating one at a time, because three unrelated situations all have to produce that same no.
namespace
{
	constexpr EAutomationTestFlags CrowdyTokenAuthorizationTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FCrowdyCppCurrentServer MakeCurrentServer(const FString& Ip4 = TEXT("203.0.113.7"), const int32 Port = 7777)
	{
		FCrowdyCppCurrentServer Server;
		Server.Ip4 = Ip4;
		Server.ClientPort = Port;
		return Server;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKeepsCurrentServerOnAnExactMatchTest,
	"CrowdySDK.CrowdyNet.RotatedTokenKeepsAServerItWasAuthorizedOn", CrowdyTokenAuthorizationTestFlags)

bool FCrowdyKeepsCurrentServerOnAnExactMatchTest::RunTest(const FString& Parameters)
{
	const FCrowdyCppCurrentServer Current = MakeCurrentServer();

	// The yes. Without this the refusals below would all pass with a function that answered no to everything,
	// which is the shape this whole feature would take if it were quietly inert.
	TestTrue(TEXT("a token authorized on exactly this server keeps it"),
		CrowdyTokenAuthorization::KeepsCurrentServer(TEXT("203.0.113.7"), 7777, &Current));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReassignsWhenNoServerWasNamedTest,
	"CrowdySDK.CrowdyNet.RotatedTokenReassignsWhenNoServerWasNamed", CrowdyTokenAuthorizationTestFlags)

bool FCrowdyReassignsWhenNoServerWasNamedTest::RunTest(const FString& Parameters)
{
	const FCrowdyCppCurrentServer Current = MakeCurrentServer();

	// The connection has no server yet, so there is nothing to keep.
	TestFalse(TEXT("no current server means re-assign"),
		CrowdyTokenAuthorization::KeepsCurrentServer(TEXT("203.0.113.7"), 7777, nullptr));

	// The Game API answered with no authorizedServer. Its own documentation makes this a normal outcome rather
	// than a failure: the node may be gone, draining, full, or not local to the app. It is also what an API too
	// old to know the argument at all leaves behind, and both mean the same thing here.
	TestFalse(TEXT("an answer naming no server means re-assign"),
		CrowdyTokenAuthorization::KeepsCurrentServer(FString(), 0, &Current));
	TestFalse(TEXT("and so does half an answer with only a port"),
		CrowdyTokenAuthorization::KeepsCurrentServer(FString(), 7777, &Current));
	TestFalse(TEXT("and so does half an answer with only an address"),
		CrowdyTokenAuthorization::KeepsCurrentServer(TEXT("203.0.113.7"), 0, &Current));

	// Two nameless servers are not the same server. This is the case the emptiness checks actually decide: for
	// every other empty answer the address comparison already says no, so without this the checks could be
	// deleted with the suite still green while a client with an unnamed assignment claimed to be authorized on it.
	const FCrowdyCppCurrentServer Nameless = MakeCurrentServer(FString(), 7777);
	TestFalse(TEXT("an empty answer does not match an equally empty current server"),
		CrowdyTokenAuthorization::KeepsCurrentServer(FString(), 7777, &Nameless));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReassignsWhenADifferentServerWasNamedTest,
	"CrowdySDK.CrowdyNet.RotatedTokenReassignsWhenADifferentServerWasNamed", CrowdyTokenAuthorizationTestFlags)

bool FCrowdyReassignsWhenADifferentServerWasNamedTest::RunTest(const FString& Parameters)
{
	const FCrowdyCppCurrentServer Current = MakeCurrentServer();

	// Both halves are checked, and separately, because one node's address with another's port is the answer a
	// comparison that only looked at the address would accept.
	TestFalse(TEXT("the same address on a different port is a different server"),
		CrowdyTokenAuthorization::KeepsCurrentServer(TEXT("203.0.113.7"), 7778, &Current));
	TestFalse(TEXT("a different address on the same port is a different server"),
		CrowdyTokenAuthorization::KeepsCurrentServer(TEXT("203.0.113.8"), 7777, &Current));

	return true;
}

// The token and the server it is authorized on are one fact, and the danger is them drifting apart: a pair left
// behind by an outgoing token would make its replacement look authorized somewhere it was never installed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNewTokenIsAuthorizedNowhereTest,
	"CrowdySDK.CrowdyNet.ANewAppTokenIsAuthorizedNowhereUntilTold", CrowdyTokenAuthorizationTestFlags)

bool FCrowdyNewTokenIsAuthorizedNowhereTest::RunTest(const FString& Parameters)
{
	// Outered to a game instance rather than the transient package. This is a UGameInstanceSubsystem, whose
	// ClassWithin is UGameInstance, and constructing one anywhere else ensures rather than failing quietly.
	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage()));
	if (!TestTrue(TEXT("a game instance was created to own the session"), Instance.IsValid()))
	{
		return false;
	}

	const TStrongObjectPtr<UCrowdyGameSession> Session(NewObject<UCrowdyGameSession>(Instance.Get()));
	if (!TestTrue(TEXT("a session was created"), Session.IsValid()))
	{
		return false;
	}

	Session->SetGameToken(TEXT("first-token"));
	Session->SetAppTokenAuthorizedServer(TEXT("203.0.113.7"), 7777);
	TestEqual(TEXT("the session records where the current token is authorized"),
		Session->GetAppTokenAuthorizedServerIp4(), FString(TEXT("203.0.113.7")));
	TestEqual(TEXT("with the port that came with it"),
		Session->GetAppTokenAuthorizedServerClientPort(), 7777);

	// A replacement arriving from a mint, or from a rotation the API answered without a server.
	Session->SetGameToken(TEXT("second-token"));

	TestEqual(TEXT("installing a token clears the previous token's authorized address"),
		Session->GetAppTokenAuthorizedServerIp4(), FString());
	TestEqual(TEXT("and its port, since neither half means anything alone"),
		Session->GetAppTokenAuthorizedServerClientPort(), 0);

	// Read through the rule that actually consumes it, so this says what the stale pair would have caused rather
	// than only that two fields were reset.
	const FCrowdyCppCurrentServer Current = MakeCurrentServer();
	TestFalse(TEXT("so the replacement token does not claim the server the old one held"),
		CrowdyTokenAuthorization::KeepsCurrentServer(Session->GetAppTokenAuthorizedServerIp4(),
			Session->GetAppTokenAuthorizedServerClientPort(), &Current));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
