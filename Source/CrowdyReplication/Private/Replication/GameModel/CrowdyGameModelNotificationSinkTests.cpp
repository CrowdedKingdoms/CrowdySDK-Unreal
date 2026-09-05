#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "Replication/GameModel/CrowdyModelChangedPing.h"
#include "Replication/GameModel/CrowdyModelNotificationSink.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyNotificationSinkTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	TArray<uint8> AsciiBytes(const FString& S)
	{
		TArray<uint8> Bytes;
		Bytes.Reserve(S.Len());
		for (const TCHAR Ch : S)
		{
			Bytes.Add(static_cast<uint8>(Ch));
		}
		return Bytes;
	}
}

// Both carriers normalize to ONE hint: the server-native 139 (a container id in the state bytes) and the fallback
// ping (a container id + entity id) land on the SAME FCrowdyModelChangeHint.ContainerId. If the carriers
// resolved their hint differently, a consumer could not treat them uniformly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNotificationSinkNormalizesTest,
	"CrowdySDK.GameModel.NotificationSinkNormalizes", CrowdyNotificationSinkTestFlags)
bool FCrowdyNotificationSinkNormalizesTest::RunTest(const FString& Parameters)
{
	// 139 carrier: decode the container id from the state bytes.
	FString FromState;
	TestTrue(TEXT("139 state decodes"),
		UCrowdyGameModelSubsystem::DecodeContainerIdFromState(AsciiBytes(TEXT("cid-123")), FromState));
	TestEqual(TEXT("139 container id"), FromState, FString(TEXT("cid-123")));

	// Empty state -> no decodable id (dropped, not a crash).
	FString Empty;
	TestFalse(TEXT("empty state fails"),
		UCrowdyGameModelSubsystem::DecodeContainerIdFromState(TArray<uint8>(), Empty));

	// A trailing null + surrounding whitespace trims to the same id; bytes after the null terminator are ignored.
	TArray<uint8> Padded = AsciiBytes(TEXT(" cid-123 "));
	Padded.Add(0);
	Padded.Add(static_cast<uint8>('X'));
	FString Trimmed;
	TestTrue(TEXT("padded state decodes"), UCrowdyGameModelSubsystem::DecodeContainerIdFromState(Padded, Trimmed));
	TestEqual(TEXT("padded id trimmed at null + whitespace"), Trimmed, FString(TEXT("cid-123")));

	// Ping carrier: same container id, plus the entity id and the model-changed event type.
	FCrowdyModelChangedPing Ping;
	Ping.EntityID = FGuid::NewGuid();
	Ping.ContainerId = TEXT("cid-123");
	const FCrowdyModelChangeHint PingHint = UCrowdyGameModelSubsystem::MakeHintFromPing(Ping);
	TestEqual(TEXT("ping container id matches the 139 container id"), PingHint.ContainerId, FromState);
	TestEqual(TEXT("ping carries the entity id"), PingHint.EntityID, Ping.EntityID);
	TestEqual(TEXT("ping event type is model-changed"), PingHint.EventType,
		static_cast<int32>(CrowdyGameModelMetaKeys::ModelChangedEventType));

	return true;
}

// NotifyModelChanged broadcasts the normalized hint to every OnModelChanged subscriber (the sink seam). A
// consumer receives the hint without knowing which carrier produced it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyNotificationSinkBroadcastsTest,
	"CrowdySDK.GameModel.NotificationSinkBroadcasts", CrowdyNotificationSinkTestFlags)
bool FCrowdyNotificationSinkBroadcastsTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model))
	{
		return false;
	}

	// Subscribe a probe. The subsystem's own re-pull consumer is bound in Initialize, which a NewObject test does
	// not run, so only this probe fires (no world, no HTTP).
	FString Received;
	int32 Count = 0;
	Model->OnModelChanged().AddLambda([&Received, &Count](const FCrowdyModelChangeHint& Hint)
	{
		Received = Hint.ContainerId;
		++Count;
	});

	FCrowdyModelChangeHint Hint;
	Hint.ContainerId = TEXT("cid-abc");
	Model->NotifyModelChanged(Hint);

	TestEqual(TEXT("subscriber fired once"), Count, 1);
	TestEqual(TEXT("subscriber saw the hint's container"), Received, FString(TEXT("cid-abc")));

	Model->OnModelChanged().Clear();
	return true;
}

// The channel carrier (opcode 18) decodes the changed container id from a payload of
// concat("cmc:", id). Robust to a raw-ASCII payload and a base64-encoded one; ordinary channel traffic (no cmc:
// prefix) is ignored, and the id is trimmed like the 139 path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyChannelCarrierDecodesTest,
	"CrowdySDK.GameModel.ChannelCarrierDecodes", CrowdyNotificationSinkTestFlags)
bool FCrowdyChannelCarrierDecodesTest::RunTest(const FString& Parameters)
{
	const FString Prefix = CrowdyGameModelMetaKeys::ModelChangedChannelPrefix;

	// Raw ASCII payload: prefix + id (what the PIE-verified spatial state carrier does).
	FString Id;
	TestTrue(TEXT("raw cmc payload decodes"),
		UCrowdyGameModelSubsystem::DecodeChannelModelChangedId(AsciiBytes(Prefix + TEXT("cid-7")), Id));
	TestEqual(TEXT("raw id"), Id, FString(TEXT("cid-7")));

	// Ordinary channel traffic (chat, an RPC frame) and an empty payload are ignored.
	FString Ignored;
	TestFalse(TEXT("non-cmc payload ignored"),
		UCrowdyGameModelSubsystem::DecodeChannelModelChangedId(AsciiBytes(TEXT("hello world")), Ignored));
	TestFalse(TEXT("empty payload ignored"),
		UCrowdyGameModelSubsystem::DecodeChannelModelChangedId(TArray<uint8>(), Ignored));

	// Base64 payload: the server may base64-encode (the docs describe payload as base64 binary). Encode the raw
	// concat bytes and feed the base64 as the payload; the decoder's fallback path recovers the id.
	const FString B64 = FBase64::Encode(AsciiBytes(Prefix + TEXT("cid-7")));
	FString FromB64;
	TestTrue(TEXT("base64 cmc payload decodes"),
		UCrowdyGameModelSubsystem::DecodeChannelModelChangedId(AsciiBytes(B64), FromB64));
	TestEqual(TEXT("base64 id"), FromB64, FString(TEXT("cid-7")));

	// Trailing null + whitespace trims to the same id.
	TArray<uint8> Padded = AsciiBytes(Prefix + TEXT(" cid-7 "));
	Padded.Add(0);
	FString Trimmed;
	TestTrue(TEXT("padded decodes"), UCrowdyGameModelSubsystem::DecodeChannelModelChangedId(Padded, Trimmed));
	TestEqual(TEXT("padded id trimmed"), Trimmed, FString(TEXT("cid-7")));

	return true;
}

// The self-echo drop. A container this client just invoked drops its FIRST model-changed echo
// (consume-once, so a later genuine change still re-pulls); the drop is per-container; an unmarked or empty
// container is never dropped.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelSelfEchoDropTest,
	"CrowdySDK.GameModel.SelfEchoDrop", CrowdyNotificationSinkTestFlags)
bool FCrowdyGameModelSelfEchoDropTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model))
	{
		return false;
	}

	// An unmarked container is never a self-echo.
	TestFalse(TEXT("unmarked container is not a self-echo"), Model->ConsumeSelfEcho(TEXT("cid-1")));

	// After marking, the first echo is dropped; the second is not (consume-once).
	Model->MarkSelfActed(TEXT("cid-1"));
	TestTrue(TEXT("first echo dropped"), Model->ConsumeSelfEcho(TEXT("cid-1")));
	TestFalse(TEXT("second echo re-pulls (consumed)"), Model->ConsumeSelfEcho(TEXT("cid-1")));

	// Per-container: consuming one leaves another pending.
	Model->MarkSelfActed(TEXT("cid-a"));
	Model->MarkSelfActed(TEXT("cid-b"));
	TestTrue(TEXT("cid-a dropped"), Model->ConsumeSelfEcho(TEXT("cid-a")));
	TestFalse(TEXT("cid-a now consumed"), Model->ConsumeSelfEcho(TEXT("cid-a")));
	TestTrue(TEXT("cid-b still pending"), Model->ConsumeSelfEcho(TEXT("cid-b")));

	// An empty id is never marked / never a self-echo.
	Model->MarkSelfActed(FString());
	TestFalse(TEXT("empty id is not a self-echo"), Model->ConsumeSelfEcho(FString()));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
