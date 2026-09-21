#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

/**
 * Lifecycle state of the replication connection.
 *
 * Connecting covers both server assignment and the wait before the server accepts traffic on the newly installed
 * session. Reconnecting is a re-assignment in progress, which the connection performs on its own after a server-side
 * reconnect command, an expired token, or the silence watchdog. Failed means the last attempt did not come up and
 * nothing will retry on its own; recovering from it is another ConnectAsync. Closed is terminal.
 */
enum class ECrowdyCppConnState : uint8
{
	Idle,
	Connecting,
	Connected,
	Reconnecting,
	Failed,
	Closed
};

/**
 * One inbound spatial message, already lifted out of any bundle and its frame decoded.
 *
 * Payload is the payload region alone: the frame header and the tail are consumed by the decode and are not part of
 * it. Both views point into the receive buffer and are valid only for the duration of the callback, so anything kept
 * past it has to be copied.
 *
 * How far to trust this. The opcode is one the server legitimately sends, and a frame that carried a server
 * signature had it checked. A frame can also arrive carrying no signature at all, which the protocol permits and
 * which the decode cannot distinguish for you here, so treat the contents as untrusted input and bound anything read
 * out of Payload. EpochMillis in particular is a server-supplied value with no range guarantee: do not subtract it
 * from a local clock without checking the result is sane.
 *
 * Replication distance and decay rate are not carried. They are send-side routing instructions the server acts on,
 * and nothing reads them back off a delivered message.
 */
struct FCrowdyCppSpatialMessage
{
	uint8 Opcode = 0;
	int64 AppId = 0;
	int64 ChunkX = 0;
	int64 ChunkY = 0;
	int64 ChunkZ = 0;

	/** 32 ASCII octets, not null-terminated. */
	TArrayView<const uint8> Uuid;

	TArrayView<const uint8> Payload;
	int64 EpochMillis = 0;
	uint8 Sequence = 0;
};

/**
 * One inbound channel message.
 *
 * Channel messages carry no signature at all: the protocol's notification layout has no room for one. The sender id
 * is therefore a claim rather than a fact, and so is the channel. Treat every field as untrusted, and do not use
 * SenderUuid to authorise anything. The same view lifetime rule applies as for a spatial message.
 */
struct FCrowdyCppChannelMessage
{
	int64 ChannelId = 0;

	/** 32 ASCII octets, not null-terminated. */
	TArrayView<const uint8> SenderUuid;

	TArrayView<const uint8> Payload;
	int64 EpochMillis = 0;
	uint8 Sequence = 0;
};

/**
 * A server error frame, and what the facade believes provoked it.
 *
 * The sequence number is the only link the protocol offers between an error and a send, and it is one byte, so it
 * repeats every 256 sends. The facade keeps a note of what each sequence was last used for and reports it here,
 * which turns "error 18, sequence 91" into something a reader can act on. It is a diagnostic aid and nothing more:
 * an attribution can be wrong whenever the counter has lapped, and a forged frame can name any sequence it likes.
 * Log it, show it, do not branch on it.
 */
struct FCrowdyCppSendError
{
	uint8 Sequence = 0;
	uint8 ErrorCode = 0;

	/** False when no send is on record for this sequence, or the one on record is too old to believe. */
	bool bAttributed = false;

	/** The opcode of the send this sequence was last used for. Meaningless unless bAttributed. */
	uint8 SendOpcode = 0;

	/** True when that send was a channel message rather than a spatial one. Meaningless unless bAttributed. */
	bool bSendWasChannel = false;

	/** The actor id that send carried, 32 octets. Empty unless bAttributed. */
	TArrayView<const uint8> SendUuid;

	/** How long before this frame arrived the attributed send went out. Meaningless unless bAttributed. */
	int64 SendAgeMs = 0;
};

/**
 * What the connection delivers. Every callback runs on the thread that calls Poll, and none of them runs after
 * Disconnect returns on that same thread.
 */
struct FCrowdyCppReplicationHandlers
{
	/** Every spatial message the server legitimately sends. The opcode is on the message. */
	TFunction<void(const FCrowdyCppSpatialMessage&)> OnSpatial;

	TFunction<void(const FCrowdyCppChannelMessage&)> OnChannel;

	/**
	 * A server error frame. Error frames carry no signature, so treat every field as a claim. An expired-token
	 * error additionally makes the connection re-assign its session. The views on the error are valid only for the
	 * duration of the call, like every other delivered message.
	 */
	TFunction<void(const FCrowdyCppSendError&)> OnError;

	TFunction<void(ECrowdyCppConnState)> OnStateChanged;
};

/** A replication server the session was assigned to, or the reason there is none. */
struct FCrowdyCppSessionAssignment
{
	bool bOk = false;
	FString Ip4;
	FString Ip6;
	int32 ClientPort = 0;

	/** Written to the log on failure, so keep credentials and raw server responses out of it. */
	FString ErrorMessage;
};

/**
 * The app-scoped token material the connection signs with. The token is used as 64 key octets exactly as it is
 * written, never decoded, so anything other than 64 characters is rejected. An expiry of zero means the token does
 * not expire and no proactive refresh is scheduled.
 */
struct FCrowdyCppReplicationToken
{
	bool bOk = false;
	FString Token;
	int64 GameTokenId = 0;
	int64 ExpiresAtEpochMs = 0;

	/** Written to the log on failure. The token itself must never be put in here. */
	FString ErrorMessage;

	/**
	 * True when the Game API installed THIS token on the server the refresh was told the connection is using.
	 *
	 * A replication server silently drops datagrams signed with a token it was never given, so false is not a
	 * degraded yes: the connection has to re-assign before it can send again, which is what every rotation did
	 * before the Game API could answer this at all. Only ever set from an answer that named a server, never
	 * assumed from a rotation that succeeded.
	 */
	bool bAuthorizedOnCurrentServer = false;
};

/** The replication server a connection is currently using, for a rotation that has to name it. */
struct FCrowdyCppCurrentServer
{
	FString Ip4;
	int32 ClientPort = 0;
};

/**
 * Polled by a blocking session callback. It becomes true when the connection is being torn down, and a callback that
 * ignores it will hold up teardown for as long as it blocks, on whichever thread called Disconnect.
 */
using FCrowdyCppShouldAbort = TFunction<bool()>;

/**
 * Assign (or re-assign) a replication server. Called on the connection's own network thread, both for the initial
 * connect and for every later re-assignment, and it is expected to block until it has an answer.
 *
 * Two rules, and both matter. It must poll the abort predicate and give up promptly when it goes true. And it must
 * never be implemented as a wait on work the calling thread itself has to run, nor call back into the connection,
 * because either deadlocks.
 *
 * The call has a server-side effect beyond returning an address: it installs this client's session on the server it
 * picks, so it is what makes the assigned endpoint usable rather than merely known.
 */
using FCrowdyCppAssignServer = TFunction<FCrowdyCppSessionAssignment(const FCrowdyCppShouldAbort&)>;

/**
 * Rotate the app-scoped token. Called on the same thread and under the same two rules as the assignment callback,
 * shortly before the current token expires. The new material is installed on the connection for you; the
 * implementation is responsible for whatever else in the game holds that token.
 *
 * The second argument names the server the connection is on, or is null when it has none. An implementation that
 * passes it to the Game API and sets bAuthorizedOnCurrentServer from the answer lets the connection keep its
 * socket; one that ignores it is still correct and simply re-assigns after every rotation.
 */
using FCrowdyCppRefreshToken =
	TFunction<FCrowdyCppReplicationToken(const FCrowdyCppShouldAbort&, const FCrowdyCppCurrentServer*)>;

/** How the connection is configured. Fixed for the life of one connection. */
struct FCrowdyCppReplicationConfig
{
	int64 AppId = 0;

	/** The token to sign with until the first refresh. A connection cannot open without a valid one. */
	FCrowdyCppReplicationToken Token;

	/** Prefer the assignment's IPv6 address. IPv4 is the default because it is the more reachable of the two. */
	bool bPreferIpv6 = false;

	/** How long after assignment to wait before treating the session as live, since the install is asynchronous. */
	int32 SessionReadyWaitMs = 1500;

	/** Refresh the token this long before it expires. Zero disables the proactive refresh. */
	int64 RefreshLeadMs = 5 * 60 * 1000;

	/**
	 * Re-assign after this long with sends flowing and nothing at all received. Zero disables it, which is the
	 * default: a player alone in the world legitimately receives nothing.
	 */
	int64 WatchdogSilenceMs = 0;

	/**
	 * Shortest gap between two server assignments. A re-assignment can be provoked from the wire by an unsigned
	 * error frame, and each one costs a blocking round trip against the Game API, so without a floor a trickle of
	 * forged datagrams turns into a request storm against your own backend. Zero removes the floor.
	 */
	int64 MinReassignIntervalMs = 5000;

	/** How many decoded messages can wait between the network thread and the next Poll. Clamped to a sane range. */
	int32 RingCapacity = 4096;

	/** How many sends can wait for the network thread. Beyond this a send is refused rather than queued, and counted. */
	int32 MaxQueuedSends = 2048;

	int32 SocketRecvBufferBytes = 1 << 20;

	/**
	 * Kernel send buffer size hint. A value at or below zero leaves the OS default in place.
	 *
	 * This is a latency budget, not a capacity dial, and bigger is not better. The buffer absorbs one frame's burst
	 * so a client emitting several entities at once does not lose them; anything beyond that is a standing queue,
	 * and every datagram behind it is delivered late. For a position stream that trade is the wrong way round: a
	 * stale position is worthless because the next update supersedes it, so excess traffic should be refused
	 * (counted in SendsDeferred) rather than queued and arrive after it stopped being true.
	 *
	 * 128 KB is a few times the largest single-frame burst a client produces, and it caps the delay the buffer can
	 * add at roughly one emission interval. The library's own default is 1 MB, which at a sustained ~1.6 MB/s
	 * measured about 600 ms of queueing delay on top of the real round trip.
	 */
	int32 SocketSendBufferBytes = 128 * 1024;

	/**
	 * Pack the messages sent within one network-thread pass into MESSAGE_BUNDLE datagrams (at most 1232 bytes or
	 * 32 members each; a lone message goes out unwrapped). Every member stays a complete, individually signed
	 * message, so only the datagram boundary moves. Requires a replication server that unpacks client bundles
	 * (v0.27.0 or later); against an older one every bundled message is dropped together, and false restores one
	 * datagram per message.
	 */
	bool bBundleSends = true;

	/**
	 * How long the library lets a pending bundle wait for more messages. The network thread flushes after every
	 * drain of the send queue, so a bundle only waits on this while the kernel send buffer is full; what it does
	 * set is the pass cadence of that thread on Windows, where the library caps each receive wait at one window
	 * because the socket cannot be woken by a send. 1 ms means queued sends leave within about a millisecond; a
	 * larger value trades that for fewer wakeups.
	 */
	int32 BundleWindowMs = 1;

	/**
	 * Tell the server what this client can read (CLIENT_CAPABILITIES) once the session is connected and every
	 * 15 s after, so downlink bundles arrive as MESSAGE_BUNDLE_SIGNED with one signature per datagram instead of
	 * one per member. A server older than v0.30.0 ignores it and keeps the per-member form. The repeat covers a
	 * token refresh or a server-side migration, both of which reset the server's record silently. False restores
	 * the 0.41 behaviour and is a diagnostic switch, not a setting a shipping build needs.
	 */
	bool bAdvertiseCapabilities = true;
};

/** Cumulative counters since the connection opened. They only grow, so a rate is a difference between snapshots. */
struct FCrowdyCppReplicationStats
{
	int64 DatagramsSent = 0;
	int64 DatagramsReceived = 0;
	int64 MessagesSent = 0;
	int64 MessagesReceived = 0;
	int64 BytesSent = 0;
	int64 BytesReceived = 0;
	int64 HmacFailures = 0;
	int64 Malformed = 0;

	/**
	 * Received datagrams that were MESSAGE_BUNDLE_SIGNED: one signature over the whole datagram, verified once,
	 * members unsigned. Counted before verification, so a forged one shows up here and in HmacFailures both.
	 * Stays zero against a server that does not sign bundles, or with bAdvertiseCapabilities off.
	 */
	int64 SignedBundlesReceived = 0;

	/** Inbound messages dropped because the queue to Poll was full. */
	int64 RingDropped = 0;

	/**
	 * Outbound messages that never reached the wire: refused because the queue to the network thread was
	 * full, discarded with the queue when the connection went down, or rejected by the library on the way out.
	 *
	 * Counted in messages rather than in calls, which is why a refused video frame adds every fragment it
	 * would have crossed as rather than one. A refusal is reported to the caller as well, so a rise here that
	 * is matched by refusals the caller saw is backpressure and not loss.
	 */
	int64 SendsDropped = 0;

	/**
	 * Datagrams the kernel could not accept because its send buffer was full. Nothing was transmitted and the socket
	 * is healthy, so this rising means the client is outrunning the send buffer rather than that sends are failing.
	 * An unbundled datagram is not retried: a state update is only worth sending while it is current, and the next
	 * one supersedes it. A deferred bundle stays pending and is retried on the next pass, so under bundling this
	 * counts attempts, not lost datagrams. Raise SocketSendBufferBytes if this climbs under load.
	 */
	int64 SendsDeferred = 0;

	/** Datagrams that failed for a genuine socket fault. Unlike SendsDeferred, this rising is a problem. */
	int64 SendsFailed = 0;

	/** Sent datagrams that were MESSAGE_BUNDLE wrappers carrying two or more messages. Zero with bundling off. */
	int64 BundlesSent = 0;

	/**
	 * Messages lost with a bundle whose flush hit a genuine socket fault. They had already been counted in
	 * MessagesSent when they joined the bundle, so this is what subtracts them back out.
	 */
	int64 MessagesDropped = 0;

	int64 Reconnects = 0;

	/** Server-supplied and unauthenticated. See the note on FCrowdyCppSpatialMessage before using it as a clock. */
	int64 LastServerEpochMs = 0;
};

/**
 * Unreal-typed facade over the vendored replication client: the UDP socket, message framing, HMAC signing and
 * verification, bundle unpacking, and the reconnect, watchdog and token-rotation lifecycle. It hides the library
 * behind a pimpl so this header exposes only Unreal types.
 *
 * Threading, which is the part to read carefully.
 *
 * The facade owns one network thread. It is the only thread that touches the library, which is deliberate: the
 * library's socket is closed and reopened by its own reconnect path, and a send racing that would write into a
 * descriptor that has already been recycled. So a send does not go out on the calling thread. It is validated
 * there, queued, and put on the wire by the network thread, which is why it reports no sequence number back.
 *
 * Sends may therefore be issued from any thread. Everything else on this class, meaning Poll, ConnectAsync,
 * SetHandlers, SetToken, Disconnect, the getters and releasing the last reference, belongs to a single owning
 * thread, normally the game thread. Two threads polling at once will corrupt the queue between the network thread
 * and the handlers; development builds check for it.
 *
 * Callbacks are delivered only from Poll, so a caller that polls from the game thread gets them where engine objects
 * are safe to touch.
 *
 * Lifetime. Disconnect is a terminal dispose and the destructor performs it, so releasing the facade is a complete
 * teardown. A handler is free to release the last reference from inside Poll; the object stays alive until Poll
 * returns.
 */
class CROWDYCPPBRIDGE_API FCrowdyCppReplication : public TSharedFromThis<FCrowdyCppReplication>
{
public:
	/**
	 * Build a connection. Nothing is opened and no callback fires until ConnectAsync. Returns null when the
	 * configuration cannot produce a usable connection, which in practice means a token that is not 64 characters.
	 */
	static TSharedPtr<FCrowdyCppReplication> Make(const FCrowdyCppReplicationConfig& Config,
		FCrowdyCppAssignServer AssignServer, FCrowdyCppRefreshToken RefreshToken);

	~FCrowdyCppReplication();

	/** Replace the handler set. A handler already running finishes first. */
	void SetHandlers(FCrowdyCppReplicationHandlers Handlers);

	/** Install new token material after an external rotation. Ignored when the token cannot sign. */
	void SetToken(const FCrowdyCppReplicationToken& Token);

	/**
	 * Assign a server and open the socket, on the network thread. Returns immediately: the outcome arrives as a
	 * state change through the handlers, Connected on success and Failed when the assignment or the socket did not
	 * come up. Calling it while already connected, or after Disconnect, does nothing.
	 */
	void ConnectAsync();

	/**
	 * Terminal dispose: stops the network thread, closes the socket and drops the handlers. Safe to call more than
	 * once, and called by the destructor. The connection cannot be reopened; build a new one instead.
	 *
	 * This blocks until the network thread has stopped. If a session callback is mid-flight it is asked to abort
	 * first, so how long that takes is decided by how promptly the callback honours the abort predicate.
	 */
	void Disconnect();

	ECrowdyCppConnState GetState() const;

	/**
	 * Deliver queued messages to the handlers on the calling thread and return how many were delivered. MaxEvents
	 * is a real budget and the default is deliberately finite: the network thread refills the queue continuously
	 * under load, so an unbounded call would not return while traffic lasts. Whatever is left waits for the next
	 * call.
	 */
	int32 Poll(int32 MaxEvents = 256);

	/**
	 * Queue one spatial message. Payload is the payload region alone: the frame header, the signature and the tail
	 * are written for you. Uuid is 32 ASCII octets and addresses the sending actor, except for an actor-to-actor
	 * message, where it addresses the destination.
	 *
	 * Distance and decay are the server's fan-out instructions. They are ignored for an actor-to-actor message,
	 * which the server delivers to one recipient rather than fanning out, and for a heartbeat, which has no fan-out
	 * at all; for those two the frame carries zero in both fields whatever is passed here.
	 *
	 * A voxel payload is checked for self-consistency, because its length field and its trailing bytes are written
	 * separately upstream and a frame whose declared length does not match what follows it is a frame the receiver
	 * will mis-read.
	 *
	 * Returns false with a reason when the opcode is not one that can be sent, the connection is not open, the
	 * payload does not fit one datagram or is malformed for its opcode, or the outbound queue is full. Returning
	 * true means accepted for sending, not delivered.
	 */
	bool SendSpatial(uint8 Opcode, int64 ChunkX, int64 ChunkY, int64 ChunkZ, TArrayView<const uint8> Uuid,
		TArrayView<const uint8> Payload, uint8 Distance, uint8 Decay, FString& OutError);

	/** Queue a channel publish. Reaches every member wherever they are, so it carries no chunk, distance or decay. */
	bool SendChannelMessage(int64 ChannelId, TArrayView<const uint8> Uuid, TArrayView<const uint8> Payload,
		FString& OutError);

	/**
	 * Split one encoded video frame into fragments and queue every one of them.
	 *
	 * Frame is a whole JPEG or WebP image; Codec is the wire byte for which, 0 or 1. FrameId is the
	 * caller's own counter, one per frame and wrapping at 16 bits, and it is what a receiver reassembles
	 * by, so two frames sharing an id from one actor are indistinguishable.
	 *
	 * The frame is queued as a whole or not at all. A frame that needs more than sixteen fragments, or that
	 * does not fit the outbound queue with room for every fragment, is refused with a reason and not one
	 * fragment is queued. OutFragmentsQueued says how many were accepted, which is zero on every refusal.
	 *
	 * That guarantee covers the queue and stops there. Once the fragments are on it, the network thread puts
	 * them out one datagram at a time and gives up on the rest of the cycle when the kernel send buffer is
	 * full, so a saturated buffer can still leave a prefix of a frame on the wire. The far end sees an image
	 * that never completes, which is the same shape as packet loss and is what the next frame recovers from.
	 *
	 * Whether the server accepts video at all is not knowable here. An app the server does not permit gets an
	 * error frame back, reported through the error handler with the opcode it was provoked by.
	 */
	bool SendVideoFrame(int64 ChunkX, int64 ChunkY, int64 ChunkZ, TArrayView<const uint8> Uuid,
		TArrayView<const uint8> Frame, int32 FrameId, uint8 Codec, uint8 Distance, uint8 Decay,
		int32& OutFragmentsQueued, FString& OutError);

	FCrowdyCppReplicationStats GetStats() const;

	/** The endpoint currently in use. Reports nothing once the connection is down or has never come up. */
	FCrowdyCppSessionAssignment GetAssignment() const;

	/**
	 * True for an opcode the client is allowed to send. It answers about the opcode only: a send can still be
	 * refused for its payload or for the state of the connection.
	 */
	static bool IsSendableSpatialOpcode(uint8 Opcode);

	/** True for an opcode the server legitimately sends. Anything else is dropped instead of being delivered. */
	static bool IsDeliverableSpatialOpcode(uint8 Opcode);

	/**
	 * A short name for a replication error code, for logs and diagnostics. Codes the protocol does not define come
	 * back as "Unrecognised", since the byte is server-supplied and nothing constrains it to the known set.
	 */
	static FString DescribeErrorCode(uint8 ErrorCode);

	/**
	 * True for the code the server sends when the app is not permitted to do what a send asked for.
	 *
	 * Offered as a predicate because the code's numeric value lives in a vendored header no other module may
	 * include, and a refusal is the only answer a client ever gets about a capability it does not hold.
	 */
	static bool IsUnauthorizedErrorCode(uint8 ErrorCode);

	/**
	 * How long after a send the facade will still attribute an error frame to it. The sequence number is one byte,
	 * so beyond roughly a lap of the counter an attribution says more about traffic volume than about the error.
	 */
	static constexpr int64 MaxSendAttributionAgeMs = 5000;

private:
	FCrowdyCppReplication();
	FCrowdyCppReplication(const FCrowdyCppReplication&) = delete;
	FCrowdyCppReplication& operator=(const FCrowdyCppReplication&) = delete;

	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
