// Copyright Epic Games, Inc. All Rights Reserved.

#include "Replication/GameModel/CrowdyGameModelSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyGameModelSessionTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Distinct from the other test files' byte helpers so a unity build never merges two definitions.
	TArray<uint8> SessionCueBytes(const FString& Text)
	{
		TArray<uint8> Bytes;
		Bytes.Reserve(Text.Len());
		for (const TCHAR Char : Text)
		{
			Bytes.Add(static_cast<uint8>(Char));
		}
		return Bytes;
	}

	bool DecodeCue(const FString& Payload, FString& OutId, int64& OutRevision, FString& OutKind)
	{
		OutId.Reset();
		OutRevision = -1;
		OutKind.Reset();
		return UCrowdyGameModelSubsystem::DecodeChannelSessionCue(SessionCueBytes(Payload), OutId, OutRevision, OutKind);
	}
}

// The cue the server sends after every session change decodes into its three fields, in both encodings the channel
// carries, and every malformed shape is refused rather than partially read: the payload is attacker-reachable.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelSessionCueDecodeTest,
	"CrowdySDK.GameModel.SessionCueDecode", CrowdyGameModelSessionTestFlags)
bool FCrowdyGameModelSessionCueDecodeTest::RunTest(const FString& Parameters)
{
	FString Id;
	int64 Revision = -1;
	FString Kind;

	if (TestTrue(TEXT("a well-formed cue decodes"), DecodeCue(TEXT("gms|sess-abc|12|participant_joined"), Id, Revision, Kind)))
	{
		TestEqual(TEXT("session id"), Id, FString(TEXT("sess-abc")));
		TestEqual(TEXT("revision"), Revision, static_cast<int64>(12));
		TestEqual(TEXT("kind"), Kind, FString(TEXT("participant_joined")));
	}

	// The base64 form decodes to the same three fields; the shared form set must produce it, or the decoder is
	// accepting fewer encodings than the channel subsystem skips.
	{
		const FString Plain = TEXT("gms|sess-b64|7|ended");
		TArray<FString> Forms;
		CrowdyGameModelMetaKeys::GameModelChannelPayloadForms(SessionCueBytes(FBase64::Encode(Plain)), Forms);
		TestTrue(TEXT("the form set includes the decoded cue"), Forms.Contains(Plain));
		if (TestTrue(TEXT("the base64 form decodes"), DecodeCue(FBase64::Encode(Plain), Id, Revision, Kind)))
		{
			TestEqual(TEXT("base64 session id"), Id, FString(TEXT("sess-b64")));
			TestEqual(TEXT("base64 revision"), Revision, static_cast<int64>(7));
			TestEqual(TEXT("base64 kind"), Kind, FString(TEXT("ended")));
		}
	}

	{
		FString LongId;
		for (int32 Index = 0; Index < 129; ++Index) { LongId.AppendChar(TEXT('a')); }
		TestFalse(TEXT("an oversized id is refused"), DecodeCue(TEXT("gms|") + LongId + TEXT("|1|ended"), Id, Revision, Kind));
	}

	TestFalse(TEXT("a missing kind field is refused"), DecodeCue(TEXT("gms|sess-abc|12"), Id, Revision, Kind));
	TestFalse(TEXT("a missing revision field is refused"), DecodeCue(TEXT("gms|sess-abc"), Id, Revision, Kind));
	TestFalse(TEXT("a fourth field is refused"), DecodeCue(TEXT("gms|sess-abc|12|ended|extra"), Id, Revision, Kind));
	TestFalse(TEXT("an empty id is refused"), DecodeCue(TEXT("gms||12|ended"), Id, Revision, Kind));
	TestFalse(TEXT("a non-integer revision is refused"), DecodeCue(TEXT("gms|sess-abc|12x|ended"), Id, Revision, Kind));
	TestFalse(TEXT("a negative revision is refused"), DecodeCue(TEXT("gms|sess-abc|-1|ended"), Id, Revision, Kind));
	TestFalse(TEXT("an empty revision is refused"), DecodeCue(TEXT("gms|sess-abc||ended"), Id, Revision, Kind));
	TestFalse(TEXT("an empty kind is refused"), DecodeCue(TEXT("gms|sess-abc|12|"), Id, Revision, Kind));
	TestFalse(TEXT("a kind over 64 characters is refused"),
		DecodeCue(FString(TEXT("gms|sess-abc|12|")) + FString::ChrN(65, TEXT('k')), Id, Revision, Kind));
	TestFalse(TEXT("the wrong prefix is refused"), DecodeCue(TEXT("gmx|sess-abc|12|ended"), Id, Revision, Kind));
	TestFalse(TEXT("a model-changed payload is refused"), DecodeCue(TEXT("cmc:sess-abc"), Id, Revision, Kind));
	TestFalse(TEXT("a signal payload is refused"), DecodeCue(TEXT("csg:BossWave:abc"), Id, Revision, Kind));
	TestFalse(TEXT("chat is refused"), DecodeCue(TEXT("hello world"), Id, Revision, Kind));
	TestFalse(TEXT("an empty payload is refused"),
		UCrowdyGameModelSubsystem::DecodeChannelSessionCue(TArray<uint8>(), Id, Revision, Kind));

	// A kind of exactly 64 characters is the boundary and is accepted.
	if (TestTrue(TEXT("a kind of exactly 64 characters is accepted"),
		DecodeCue(FString(TEXT("gms|sess-abc|3|")) + FString::ChrN(64, TEXT('k')), Id, Revision, Kind)))
	{
		TestEqual(TEXT("64-character kind length"), Kind.Len(), 64);
	}

	return true;
}

// UCrowdyChannels skips Game Model frames with HasGameModelChannelPrefix, so a cue must be claimed by it in every
// encoding the decoder accepts, or the frame would also be fed to the reliable-RPC decoder.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelSessionCuePrefixSkipTest,
	"CrowdySDK.GameModel.SessionCuePrefixSkip", CrowdyGameModelSessionTestFlags)
bool FCrowdyGameModelSessionCuePrefixSkipTest::RunTest(const FString& Parameters)
{
	const FString Plain = TEXT("gms|sess-abc|12|host_changed");
	TestTrue(TEXT("the skip test claims a raw cue"),
		CrowdyGameModelMetaKeys::HasGameModelChannelPrefix(SessionCueBytes(Plain)));
	TestTrue(TEXT("the skip test claims a base64 cue"),
		CrowdyGameModelMetaKeys::HasGameModelChannelPrefix(SessionCueBytes(FBase64::Encode(Plain))));
	TestFalse(TEXT("the skip test leaves chat alone"),
		CrowdyGameModelMetaKeys::HasGameModelChannelPrefix(SessionCueBytes(TEXT("gm|not a cue"))));

	// The cue prefix must not be claimed by the other two decoders, or a cue would fire a re-pull or a signal.
	FString Id;
	FString Name;
	TestFalse(TEXT("the model-changed decoder refuses a cue"),
		UCrowdyGameModelSubsystem::DecodeChannelModelChangedId(SessionCueBytes(Plain), Id));
	TestFalse(TEXT("the signal decoder refuses a cue"),
		UCrowdyGameModelSubsystem::DecodeChannelSignal(SessionCueBytes(Plain), Name, Id));
	return true;
}

// The incarnation a leave sends: an explicit one wins, the remembered one is second, and with neither the leave
// cannot be sent at all because the server requires one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelSessionLeaveIncarnationTest,
	"CrowdySDK.GameModel.SessionLeaveIncarnation", CrowdyGameModelSessionTestFlags)
bool FCrowdyGameModelSessionLeaveIncarnationTest::RunTest(const FString& Parameters)
{
	const int32 Remembered = 3;
	int32 Out = -1;

	TestTrue(TEXT("explicit resolves"), UCrowdyGameModelSubsystem::ResolveLeaveIncarnation(5, &Remembered, Out));
	TestEqual(TEXT("explicit wins over remembered"), Out, 5);

	TestTrue(TEXT("explicit resolves with nothing remembered"), UCrowdyGameModelSubsystem::ResolveLeaveIncarnation(2, nullptr, Out));
	TestEqual(TEXT("explicit alone"), Out, 2);

	TestTrue(TEXT("remembered resolves when explicit is 0"), UCrowdyGameModelSubsystem::ResolveLeaveIncarnation(0, &Remembered, Out));
	TestEqual(TEXT("remembered used"), Out, 3);

	TestTrue(TEXT("remembered resolves when explicit is negative"), UCrowdyGameModelSubsystem::ResolveLeaveIncarnation(-1, &Remembered, Out));
	TestEqual(TEXT("remembered used over a negative explicit"), Out, 3);

	Out = -1;
	TestFalse(TEXT("neither fails"), UCrowdyGameModelSubsystem::ResolveLeaveIncarnation(0, nullptr, Out));
	TestEqual(TEXT("neither leaves 0"), Out, 0);

	const int32 RememberedZero = 0;
	TestFalse(TEXT("a remembered 0 is not an incarnation"), UCrowdyGameModelSubsystem::ResolveLeaveIncarnation(0, &RememberedZero, Out));

	// A locally raised error never carries a server code, so the pair a UI reads always describes one failure.
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (TestNotNull(TEXT("subsystem created"), Model))
	{
		Model->SetLastModelError(TEXT("no incarnation remembered"));
		TestEqual(TEXT("local error message"), Model->GetLastModelError(), FString(TEXT("no incarnation remembered")));
		TestEqual(TEXT("local error has no code"), Model->GetLastModelErrorCode(), FString());
	}
	return true;
}

// The facade mapping carries every session field, including the two presence flags that separate a null from a 0.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelSessionToBpSessionTest,
	"CrowdySDK.GameModel.SessionToBpSession", CrowdyGameModelSessionTestFlags)
bool FCrowdyGameModelSessionToBpSessionTest::RunTest(const FString& Parameters)
{
	FCrowdyGameSessionData Data;
	Data.SessionId = TEXT("sess-1");
	Data.Name = TEXT("Arena");
	Data.Status = TEXT("active");
	Data.CreatedByUserId = 1001;
	Data.CurrentTurnUserId = 1002;
	Data.bHasCurrentTurn = true;
	Data.MetadataJson = TEXT("{\"map\":\"desert\"}");
	Data.Admission = TEXT("locked");
	Data.MaxParticipants = 4;
	Data.bHasMaxParticipants = true;
	Data.ParticipantCount = 2;
	Data.HostUserId = 1001;
	Data.bHasHost = true;
	Data.HostTerm = 3;
	Data.Revision = 42;
	Data.EndedAt = TEXT("2026-01-02T03:04:05Z");
	Data.EndReason = TEXT("abandoned");
	Data.CreatedAt = TEXT("2026-01-01T00:00:00Z");
	Data.Presence = TEXT("none");
	Data.SeededContainerCount = 8;
	Data.bHasSeededContainerCount = true;

	const FCrowdyGameModelSession Out = UCrowdyGameModelSubsystem::ToBpSession(Data);
	TestEqual(TEXT("SessionId"), Out.SessionId, FString(TEXT("sess-1")));
	TestEqual(TEXT("Name"), Out.Name, FString(TEXT("Arena")));
	TestTrue(TEXT("Status"), Out.Status == ECrowdySessionStatus::Active);
	TestEqual(TEXT("CreatedByUserId"), Out.CreatedByUserId, static_cast<int64>(1001));
	TestEqual(TEXT("CurrentTurnUserId"), Out.CurrentTurnUserId, static_cast<int64>(1002));
	TestTrue(TEXT("bHasCurrentTurn"), Out.bHasCurrentTurn);
	TestEqual(TEXT("MetadataJson"), Out.MetadataJson, FString(TEXT("{\"map\":\"desert\"}")));
	TestTrue(TEXT("Admission"), Out.Admission == ECrowdySessionAdmission::Locked);
	TestEqual(TEXT("MaxParticipants"), Out.MaxParticipants, 4);
	TestTrue(TEXT("bHasMaxParticipants"), Out.bHasMaxParticipants);
	TestEqual(TEXT("ParticipantCount"), Out.ParticipantCount, 2);
	TestEqual(TEXT("HostUserId"), Out.HostUserId, static_cast<int64>(1001));
	TestTrue(TEXT("bHasHost"), Out.bHasHost);
	TestEqual(TEXT("HostTerm"), Out.HostTerm, 3);
	TestEqual(TEXT("Revision"), Out.Revision, static_cast<int64>(42));
	TestEqual(TEXT("EndedAt"), Out.EndedAt, FString(TEXT("2026-01-02T03:04:05Z")));
	TestTrue(TEXT("EndReason"), Out.EndReason == ECrowdySessionEndReason::Abandoned);
	TestEqual(TEXT("CreatedAt"), Out.CreatedAt, FString(TEXT("2026-01-01T00:00:00Z")));
	TestTrue(TEXT("Presence"), Out.Presence == ECrowdySessionPresence::None);
	TestEqual(TEXT("SeededContainerCount"), Out.SeededContainerCount, 8);
	TestTrue(TEXT("bHasSeededContainerCount"), Out.bHasSeededContainerCount);

	// The flags travel independently of the values: an unbounded, hostless session keeps both false.
	FCrowdyGameSessionData Bare;
	Bare.MaxParticipants = 4;
	Bare.HostUserId = 1001;
	Bare.SeededContainerCount = 3;
	const FCrowdyGameModelSession BareOut = UCrowdyGameModelSubsystem::ToBpSession(Bare);
	TestFalse(TEXT("bHasMaxParticipants false when the wire had null"), BareOut.bHasMaxParticipants);
	TestFalse(TEXT("bHasHost false when the wire had null"), BareOut.bHasHost);
	TestFalse(TEXT("bHasCurrentTurn false by default"), BareOut.bHasCurrentTurn);
	TestFalse(TEXT("bHasSeededContainerCount false when the wire had null"), BareOut.bHasSeededContainerCount);

	// The seed words round-trip; an unknown word is the server default.
	TestEqual(TEXT("app word"), FString(UCrowdyGameModelSubsystem::SessionSeedStateWord(ECrowdySessionSeedState::App)), FString(TEXT("app")));
	TestEqual(TEXT("defaults word"), FString(UCrowdyGameModelSubsystem::SessionSeedStateWord(ECrowdySessionSeedState::Defaults)), FString(TEXT("defaults")));
	TestTrue(TEXT("app parses"), UCrowdyGameModelSubsystem::ParseSessionSeedState(TEXT("app")) == ECrowdySessionSeedState::App);
	TestTrue(TEXT("anything else is defaults"), UCrowdyGameModelSubsystem::ParseSessionSeedState(TEXT("bogus")) == ECrowdySessionSeedState::Defaults);
	return true;
}

// The snapshot mapping carries the session, every participant in order, and the snapshot's own revision.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelSessionToBpSnapshotTest,
	"CrowdySDK.GameModel.SessionToBpSnapshot", CrowdyGameModelSessionTestFlags)
bool FCrowdyGameModelSessionToBpSnapshotTest::RunTest(const FString& Parameters)
{
	FCrowdyGameSessionSnapshotData Data;
	Data.Revision = 9;
	Data.Session.SessionId = TEXT("sess-2");
	Data.Session.HostUserId = 7;
	Data.Session.bHasHost = true;

	FCrowdyGameSessionParticipantData First;
	First.SessionId = TEXT("sess-2");
	First.UserId = 7;
	First.Role = TEXT("host");
	First.State = TEXT("joined");
	First.Incarnation = 1;
	First.ActorUuid = TEXT("0123456789abcdef0123456789abcdef");
	First.JoinedAt = TEXT("2026-01-01T00:00:01Z");

	FCrowdyGameSessionParticipantData Second;
	Second.SessionId = TEXT("sess-2");
	Second.UserId = 8;
	Second.Role = TEXT("player");
	Second.State = TEXT("left");
	Second.Incarnation = 2;
	Second.JoinedAt = TEXT("2026-01-01T00:00:02Z");
	Second.LeftAt = TEXT("2026-01-01T00:05:00Z");
	Second.LeftReason = TEXT("presence_expired");

	Data.Participants.Add(First);
	Data.Participants.Add(Second);

	const FCrowdyGameModelSessionSnapshot Out = UCrowdyGameModelSubsystem::ToBpSnapshot(Data);
	TestEqual(TEXT("snapshot revision"), Out.Revision, static_cast<int64>(9));
	TestEqual(TEXT("session id"), Out.Session.SessionId, FString(TEXT("sess-2")));
	TestEqual(TEXT("session host"), Out.Session.HostUserId, static_cast<int64>(7));
	TestTrue(TEXT("session has host"), Out.Session.bHasHost);
	if (!TestEqual(TEXT("two participants"), Out.Participants.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("first user"), Out.Participants[0].UserId, static_cast<int64>(7));
	TestEqual(TEXT("first role"), Out.Participants[0].Role, FString(TEXT("host")));
	TestTrue(TEXT("first state"), Out.Participants[0].State == ECrowdySessionParticipantState::Joined);
	TestEqual(TEXT("first incarnation"), Out.Participants[0].Incarnation, 1);
	TestEqual(TEXT("first actor"), Out.Participants[0].ActorUuid, FString(TEXT("0123456789abcdef0123456789abcdef")));
	TestEqual(TEXT("first joined"), Out.Participants[0].JoinedAt, FString(TEXT("2026-01-01T00:00:01Z")));
	TestEqual(TEXT("first session id"), Out.Participants[0].SessionId, FString(TEXT("sess-2")));
	TestEqual(TEXT("second user"), Out.Participants[1].UserId, static_cast<int64>(8));
	TestTrue(TEXT("second state"), Out.Participants[1].State == ECrowdySessionParticipantState::Left);
	TestEqual(TEXT("second incarnation"), Out.Participants[1].Incarnation, 2);
	TestEqual(TEXT("second left at"), Out.Participants[1].LeftAt, FString(TEXT("2026-01-01T00:05:00Z")));
	TestTrue(TEXT("second left reason"), Out.Participants[1].LeftReason == ECrowdySessionLeftReason::PresenceExpired);

	// The event mapping, the third struct the stream and the log both produce.
	FCrowdyGameSessionEventData EventData;
	EventData.AppId = 55;
	EventData.SessionId = TEXT("sess-2");
	EventData.Revision = 10;
	EventData.Kind = TEXT("turn_changed");
	EventData.PayloadJson = TEXT("{\"userId\":\"8\"}");
	EventData.CreatedAt = TEXT("2026-01-01T00:06:00Z");
	const FCrowdyGameModelSessionEvent Event = UCrowdyGameModelSubsystem::ToBpSessionEvent(EventData);
	TestEqual(TEXT("event session id"), Event.SessionId, FString(TEXT("sess-2")));
	TestEqual(TEXT("event revision"), Event.Revision, static_cast<int64>(10));
	TestTrue(TEXT("event kind"), Event.Kind == ECrowdySessionEventKind::TurnChanged);
	TestEqual(TEXT("event kind name"), Event.KindName, FString(TEXT("turn_changed")));
	TestEqual(TEXT("event user id parsed from the payload"), Event.UserId, static_cast<int64>(8));
	TestFalse(TEXT("a stream event is not a cue"), Event.bIsCue);
	TestEqual(TEXT("event payload"), Event.PayloadJson, FString(TEXT("{\"userId\":\"8\"}")));
	TestEqual(TEXT("event created at"), Event.CreatedAt, FString(TEXT("2026-01-01T00:06:00Z")));

	// A host change carries the new and previous host and the term; a payload the server did not send leaves the
	// typed fields empty and an unknown kind keeps its word.
	FCrowdyGameSessionEventData HostData;
	HostData.Kind = TEXT("host_changed");
	HostData.PayloadJson = TEXT("{\"hostUserId\":\"9\",\"hostTerm\":3,\"previousHostUserId\":\"8\",\"reason\":\"host_left\"}");
	const FCrowdyGameModelSessionEvent HostEvent = UCrowdyGameModelSubsystem::ToBpSessionEvent(HostData);
	TestTrue(TEXT("host change kind"), HostEvent.Kind == ECrowdySessionEventKind::HostChanged);
	TestEqual(TEXT("new host"), HostEvent.HostUserId, static_cast<int64>(9));
	TestEqual(TEXT("previous host"), HostEvent.PreviousHostUserId, static_cast<int64>(8));
	TestEqual(TEXT("host term"), HostEvent.HostTerm, 3);
	TestEqual(TEXT("host change reason"), HostEvent.ReasonName, FString(TEXT("host_left")));

	FCrowdyGameSessionEventData OddData;
	OddData.Kind = TEXT("something_new");
	OddData.PayloadJson = TEXT("{\"admission\":\"locked\"}");
	const FCrowdyGameModelSessionEvent OddEvent = UCrowdyGameModelSubsystem::ToBpSessionEvent(OddData);
	TestTrue(TEXT("unknown kind"), OddEvent.Kind == ECrowdySessionEventKind::Unknown);
	TestEqual(TEXT("unknown kind keeps its word"), OddEvent.KindName, FString(TEXT("something_new")));
	TestTrue(TEXT("admission parsed from the payload"), OddEvent.Admission == ECrowdySessionAdmission::Locked);
	TestEqual(TEXT("no user in an admission payload"), OddEvent.UserId, static_cast<int64>(0));
	return true;
}

// The server's words map to the enums a Blueprint switches on, and back for the words the SDK sends; an unknown
// word is Unknown / Other rather than a silent default.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelSessionWordsTest,
	"CrowdySDK.GameModel.SessionWords", CrowdyGameModelSessionTestFlags)
bool FCrowdyGameModelSessionWordsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("active"), UCrowdyGameModelSubsystem::ParseSessionStatus(TEXT("active")) == ECrowdySessionStatus::Active);
	TestTrue(TEXT("completed"), UCrowdyGameModelSubsystem::ParseSessionStatus(TEXT("completed")) == ECrowdySessionStatus::Completed);
	TestTrue(TEXT("odd status"), UCrowdyGameModelSubsystem::ParseSessionStatus(TEXT("paused")) == ECrowdySessionStatus::Unknown);

	for (const ECrowdySessionAdmission Admission : { ECrowdySessionAdmission::Open, ECrowdySessionAdmission::Locked, ECrowdySessionAdmission::Closed })
	{
		TestTrue(TEXT("admission round trip"),
			UCrowdyGameModelSubsystem::ParseSessionAdmission(UCrowdyGameModelSubsystem::SessionAdmissionWord(Admission)) == Admission);
	}
	TestEqual(TEXT("locked word"), FString(UCrowdyGameModelSubsystem::SessionAdmissionWord(ECrowdySessionAdmission::Locked)), FString(TEXT("locked")));
	TestTrue(TEXT("presence none"), UCrowdyGameModelSubsystem::ParseSessionPresence(TEXT("none")) == ECrowdySessionPresence::None);
	TestEqual(TEXT("presence none word"), FString(UCrowdyGameModelSubsystem::SessionPresenceWord(ECrowdySessionPresence::None)), FString(TEXT("none")));
	TestTrue(TEXT("empty end reason is None"), UCrowdyGameModelSubsystem::ParseSessionEndReason(TEXT("")) == ECrowdySessionEndReason::None);
	TestTrue(TEXT("empty_timeout"), UCrowdyGameModelSubsystem::ParseSessionEndReason(TEXT("empty_timeout")) == ECrowdySessionEndReason::EmptyTimeout);
	TestTrue(TEXT("session_ended"), UCrowdyGameModelSubsystem::ParseLeftReason(TEXT("session_ended")) == ECrowdySessionLeftReason::SessionEnded);
	TestTrue(TEXT("participant_expired"), UCrowdyGameModelSubsystem::ParseSessionEventKind(TEXT("participant_expired")) == ECrowdySessionEventKind::ParticipantExpired);

	TestTrue(TEXT("no code is None"), UCrowdyGameModelSubsystem::ClassifySessionError(TEXT("")) == ECrowdySessionError::None);
	TestTrue(TEXT("SESSION_FULL"), UCrowdyGameModelSubsystem::ClassifySessionError(TEXT("SESSION_FULL")) == ECrowdySessionError::Full);
	TestTrue(TEXT("SESSION_HOST_TERM_STALE"), UCrowdyGameModelSubsystem::ClassifySessionError(TEXT("SESSION_HOST_TERM_STALE")) == ECrowdySessionError::HostTermStale);
	TestTrue(TEXT("FORBIDDEN"), UCrowdyGameModelSubsystem::ClassifySessionError(TEXT("FORBIDDEN")) == ECrowdySessionError::NotAllowed);
	TestTrue(TEXT("an unknown code is Other"), UCrowdyGameModelSubsystem::ClassifySessionError(TEXT("RATE_LIMITED")) == ECrowdySessionError::Other);

	// The host term a host action sends: explicit wins, UseKnownHostTerm sends the remembered one, SkipHostTermCheck
	// and an unknown session send none.
	const int32 Known = 4;
	TestEqual(TEXT("explicit term"), UCrowdyGameModelSubsystem::ResolveHostTerm(7, &Known), 7);
	TestEqual(TEXT("known term"), UCrowdyGameModelSubsystem::ResolveHostTerm(UCrowdyGameModelSubsystem::UseKnownHostTerm, &Known), 4);
	TestEqual(TEXT("nothing known"), UCrowdyGameModelSubsystem::ResolveHostTerm(UCrowdyGameModelSubsystem::UseKnownHostTerm, nullptr), 0);
	TestEqual(TEXT("skip ignores the known term"), UCrowdyGameModelSubsystem::ResolveHostTerm(UCrowdyGameModelSubsystem::SkipHostTermCheck, &Known), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
