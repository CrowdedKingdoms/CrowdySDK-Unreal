#include "Replication/GameModel/CrowdyModelIdentity.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Engine/GameInstance.h"
#include "Subsystem/CrowdyGameSession.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyGameModelTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// UCrowdyGameSession is a UGameInstanceSubsystem (ClassWithin=UGameInstance); a transient-package
	// NewObject trips a ClassWithin ensure, so outer it to a bare GameInstance -- the established idiom
	// (see MakeStateRegistry() in CrowdyStateDiscoveryTests.cpp). Initialize()/Deinitialize() never run,
	// which is fine here: SetUUID/SetUserID/GetID/GetUserID touch only GameSessionInfo.
	UCrowdyGameSession* MakeGameSession()
	{
		UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
		return NewObject<UCrowdyGameSession>(GameInstance);
	}
}

// Every FGuid survives NetIDToContainerKey -> ContainerKeyToNetID losslessly, the key is always
// exactly 32 lowercase hex characters, and malformed keys (wrong length or non-hex content) fail
// cleanly instead of asserting/crashing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelIdentityRoundTripTest,
	"CrowdySDK.GameModel.IdentityRoundTrip", CrowdyGameModelTestFlags)
bool FCrowdyGameModelIdentityRoundTripTest::RunTest(const FString& Parameters)
{
	for (int32 Index = 0; Index < 16; ++Index)
	{
		const FGuid Original = FGuid::NewGuid();
		const FString Key = FCrowdyModelIdentity::NetIDToContainerKey(Original);

		TestEqual(TEXT("container key is exactly 32 characters"), Key.Len(), 32);
		TestEqual(TEXT("container key is all-lowercase"), Key, Key.ToLower());

		FGuid RoundTripped;
		const bool bParsed = FCrowdyModelIdentity::ContainerKeyToNetID(Key, RoundTripped);
		TestTrue(TEXT("container key parses back"), bParsed);
		TestEqual(TEXT("round trip reproduces the original NetID"), RoundTripped, Original);
	}

	// Malformed input: wrong length (too short, and empty), and right length but non-hex content.
	{
		FString NonHexKey;
		for (int32 Index = 0; Index < 32; ++Index)
		{
			NonHexKey.AppendChar(TEXT('z'));
		}

		FGuid Unused;
		TestFalse(TEXT("too-short key is rejected"), FCrowdyModelIdentity::ContainerKeyToNetID(TEXT("abc123"), Unused));
		TestFalse(TEXT("empty key is rejected"), FCrowdyModelIdentity::ContainerKeyToNetID(TEXT(""), Unused));
		TestFalse(TEXT("32 non-hex characters are rejected"), FCrowdyModelIdentity::ContainerKeyToNetID(NonHexKey, Unused));
	}

	return true;
}

// TryLocalUserIDForOwner resolves the session's own int64 user id ONLY for the session's own FGuid; a
// different (remote) FGuid or a null session both fail cleanly with OutUserID left at 0. The session's
// ID/UserID are seeded purely through UCrowdyGameSession's existing public API (SetUUID + SetUserID) --
// SetUUID round-trips through USerializationFunctionLibrary::ToGuid, which parses the same 32-hex-digit
// Digits format NetIDToContainerKey/ContainerKeyToNetID use, so no new test-only seam is needed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelLocalUserIdResolvesTest,
	"CrowdySDK.GameModel.LocalUserIdResolves", CrowdyGameModelTestFlags)
bool FCrowdyGameModelLocalUserIdResolvesTest::RunTest(const FString& Parameters)
{
	UCrowdyGameSession* Session = MakeGameSession();
	if (!TestNotNull(TEXT("session created"), Session))
	{
		return false;
	}

	const FGuid LocalID = FGuid::NewGuid();
	constexpr int64 LocalUserID = 424242;
	Session->SetUUID(LocalID.ToString(EGuidFormats::Digits));
	Session->SetUserID(LocalUserID);
	TestEqual(TEXT("session ID seeded correctly"), Session->GetID(), LocalID);

	int64 OutUserID = -1;
	TestTrue(TEXT("the session's own FGuid resolves"),
		FCrowdyModelIdentity::TryLocalUserIDForOwner(Session, LocalID, OutUserID));
	TestEqual(TEXT("resolved user id matches the session"), OutUserID, LocalUserID);

	const FGuid RemoteID = FGuid::NewGuid();
	OutUserID = -1;
	TestFalse(TEXT("a remote FGuid does not resolve"),
		FCrowdyModelIdentity::TryLocalUserIDForOwner(Session, RemoteID, OutUserID));
	TestEqual(TEXT("a remote FGuid leaves OutUserID at 0"), OutUserID, (int64)0);

	OutUserID = -1;
	TestFalse(TEXT("a null session does not resolve"),
		FCrowdyModelIdentity::TryLocalUserIDForOwner(nullptr, LocalID, OutUserID));
	TestEqual(TEXT("a null session leaves OutUserID at 0"), OutUserID, (int64)0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
