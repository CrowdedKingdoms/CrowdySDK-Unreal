#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/Optional.h"
#include "Templates/Function.h"

/**
 * The parsed outcome of a Game Model container-state read routed through
 * CrowdyCPP. bOk is the read-success flag: it is true only when the server was
 * reached AND returned a decodable container-state object. A reachable-but-absent container (a null,
 * empty, or non-object state) reports bOk == false with State null, and is told
 * apart from a network/GraphQL failure only by ErrorMessage.
 */
struct FCrowdyCppContainerStateResult
{
	bool bOk = false;
	TSharedPtr<FJsonObject> State;   // the decoded container state when bOk
	FString ErrorMessage;            // why the read yielded no state
};

/**
 * One property write the server applied inside a gameModelInvoke transaction, laid out like
 * FCrowdyMutationApplied. The value fields ride as JSON-encoded strings exactly as the Game API returns them.
 */
struct FCrowdyCppMutationApplied
{
	// The container written, which need not be the invoke's own: an effect that writes source.<attr> reports it
	// here. Empty only against a server predating the field.
	FString ContainerId;
	FString Key;
	FString OldValueJson;
	FString NewValueJson;
};

/**
 * The parsed outcome of a gameModelInvoke routed through CrowdyCPP, laid out like
 * FCrowdyInvokeResult so the subsystem maps it across field for field. bTransportOk and bSuccess are
 * kept separate so a rolled-back invoke (bTransportOk == true, bSuccess == false) is never mistaken for a
 * network/GraphQL failure (bTransportOk == false); in the failure case ErrorMessage carries the transport
 * or GraphQL error, in the rolled-back case it carries the server's reason.
 */
struct FCrowdyCppInvokeResult
{
	bool bTransportOk = false;
	bool bSuccess = false;
	FString ReturnValueJson;
	FString ErrorMessage;
	TArray<FCrowdyCppMutationApplied> Mutations;

	// Why the call failed and whether repeating it could work, filled from whichever channel reported the failure.
	// An in-band rejection (bTransportOk true, bSuccess false) carries the server's PlayerFaultInfo; the overload
	// refusal is a thrown GraphQL error instead (bTransportOk false) and carries the same attribution in
	// errors[].extensions. FaultCode is the server's stable enumerated reason, Blame its vocabulary verbatim
	// ("PLATFORM" | "AUTHOR" | "BUDGET"), empty when the failure carried no attribution.
	FString FaultCode;
	FString Blame;

	// The object a quarantine refusal is about, and the lint finding that quarantined it. A function or automation
	// with an enforced gameModelLint error against it refuses to run until its definition is written again.
	//
	// Do NOT gate reading these on FaultCode: on gameModelInvoke the server rebuilds the error at the player
	// boundary, so the code arrives as USER_CODE_ERROR with blame AUTHOR while these three survive intact. A
	// non-empty QuarantineReason is the reliable signal, and it is the only field that says what to fix.
	FString QuarantinedKind;   // "function" or "automation"
	FString QuarantinedName;
	FString QuarantineReason;

	// True only when the server said so AND said whose fault it was. CrowdyCPP's own extensions.retryable defaults
	// TRUE (only an explicit false means "do not bother"), which is right for its callers but wrong to pass through
	// here unqualified: an unattributed network failure would then read as a licensed retry, and a retry of a call
	// that may already have committed is worse than reporting the failure.
	bool bRetryable = false;

	// How long the server says to wait before trying again, from extensions.retryAfterMs. Only the thrown channel
	// ever carries it: PlayerFaultInfo has no timing field at all, so an in-band fault leaves this unset by
	// construction rather than by omission.
	//
	// Unset and zero are different instructions and must not be collapsed. Zero means the window has already rolled
	// and the call may go again; unset means the server named no wait, and the caller owes it a local backoff. An
	// int defaulting to 0 would turn silence into an immediate re-send, which against a rate limiter is the one
	// response guaranteed to be refused again.
	//
	// It is the milliseconds REMAINING in the current fixed window when the refusal was built, not a fixed backoff,
	// so it is a deadline measured from receipt. A later refusal in the same window carries a smaller number, and a
	// value cached from an earlier one is always too long.
	TOptional<int64> RetryAfterMs;
};

/**
 * Whether a server error code means "your game model is wrong", as opposed to a transient failure or a
 * permissions problem. Keys off the code, never the message text.
 *
 * The set of codes is CrowdyCPP's, not this bridge's, so it grows with the platform rather than with a list
 * maintained here. It answers for the codes the server states directly (a container bound to an undefined type,
 * a quarantined object); it does NOT cover gameModelInvoke, where the player boundary rewrites the code and only
 * a non-empty FCrowdyCppInvokeResult::QuarantineReason identifies the refusal.
 */
CROWDYCPPBRIDGE_API bool CrowdyCppIsModelRefusalCode(const FString& Code);

/**
 * The parsed outcome of a Game Model runtime op routed generically through CrowdyCPP. Data is the GraphQL
 * response's `data` object (re-serialized from the CrowdyCPP outcome, nesting-guarded, then re-parsed with the UE
 * JSON reader), so a caller wraps it back into a { "data": ... } envelope and feeds it to the matching
 * FCrowdyGameApiCodec::ParseXEnvelope - field extraction is then identical by construction. bTransportOk is
 * the transport-success flag: a network/GraphQL failure reports bTransportOk == false with Data null and
 * the reason in ErrorMessage; the per-op logic outcome (e.g. a rolled-back delete returning false) lives inside Data.
 */
struct FCrowdyCppJsonResult
{
	bool bTransportOk = false;
	TSharedPtr<FJsonObject> Data;   // the GraphQL `data` object when bTransportOk
	FString ErrorMessage;

	// The server's stable extensions.code for the first GraphQL error, empty when the failure carried none (a
	// network error, a timeout, a non-2xx with no error body). Branch on this rather than on ErrorMessage, which is
	// written for a human and the server is free to reword. APP_UNAVAILABLE is the one every caller should know:
	// the app's datacenter cannot serve it right now and there is nowhere else to go, so a retry elsewhere is not
	// the answer and ErrorMessage is worth showing verbatim.
	FString ErrorCode;
};

/**
 * The outcome of a Game Model studio/automation authoring mutation (seed, automation upsert, trigger upsert).
 * These ops return only success or failure to the caller: bOk is the transport-and-GraphQL success flag, and on
 * failure ErrorMessage carries the transport or server reason. They require an admin (manage_apps) token.
 */
struct FCrowdyCppStudioOpResult
{
	bool bOk = false;
	FString ErrorMessage;
};

/**
 * The parsed outcome of a sign-in. Every sign-in path (password, register, dev bypass, magic link, social) returns
 * the same payload, so they share one result. The token is the identity SESSION token: it mints app-scoped
 * gameplay tokens and is never itself accepted by the gameplay roots or the UDP surface.
 * Email and Gamertag are empty when the server returns them as null.
 */
struct FCrowdyCppAuthResult
{
	bool bOk = false;
	FString ErrorMessage;
	FString SessionToken;
	int64 SessionGameTokenID = 0;
	int64 UserID = 0;
	FString Email;
	FString Gamertag;
};

/**
 * The parsed outcome of an app-token mint or refresh. The token is the app-scoped gameplay credential: the Game API
 * bearer, the UDP HMAC key, and (as the game token id) the UDP spatial message tail. The two ids arrive as BigInt
 * JSON strings and are widened here; the URLs are the per-app endpoints to adopt, and are empty when the server
 * returns them as null.
 */
struct FCrowdyCppAppTokenResult
{
	bool bOk = false;
	FString ErrorMessage;
	FString ErrorCode;      // the server's extensions.code, as on FCrowdyCppJsonResult
	FString AppToken;
	int64 AppGameTokenID = 0;
	FString AppID;
	FString ExpiresAt;
	FString GameApiUrl;
	FString GameApiWsUrl;

	// The shared origin: one name that every datacenter answers, so it survives the loss of the single instance
	// GameApiUrl names. Feed it back as FCrowdyCppClientConfig::DiscoveryUrl and a client whose endpoint dies can
	// ask where to go next instead of retrying an address that has stopped answering. Empty when the server has no
	// public URL.
	FString DiscoveryUrl;

	FString LaunchUrl;

	// The replication server the Game API installed this token on, when the request named one. Empty on a mint, on
	// a rotation that named no server, and on one the API answered without an authorizedServer: in all three the
	// holder of this token has to re-assign before it can send. Never assume the server it was asked about.
	FString AuthorizedServerIp4;
	int32 AuthorizedServerClientPort = 0;

	bool HasAuthorizedServer() const { return !AuthorizedServerIp4.IsEmpty() && AuthorizedServerClientPort > 0; }
};

/**
 * The outcome of an operation whose payload has no typed counterpart here, handed back as the already-unwrapped
 * value of the operation's single selected field rather than the whole GraphQL data object. It is an FJsonValue
 * rather than an FJsonObject because some of these operations select a list.
 */
struct FCrowdyCppJsonValueResult
{
	bool bOk = false;
	TSharedPtr<FJsonValue> Value;
	FString ErrorMessage;
};

/** The outcome of an operation that answers with a single boolean. bOk is the transport flag; bValue is the answer. */
struct FCrowdyCppBoolResult
{
	bool bOk = false;
	bool bValue = false;
	FString ErrorMessage;
};

/** The outcome of an operation that answers with a list of strings. */
struct FCrowdyCppStringListResult
{
	bool bOk = false;
	TArray<FString> Values;
	FString ErrorMessage;
};

/**
 * Which bearer a call carries. There is one API origin and two tokens: Game is the app-scoped gameplay credential
 * (or a manage_apps token for authoring ops), Management is the user's identity session token. A call never
 * inherits whichever bearer the previous caller happened to leave installed; the plane is resolved per call.
 *
 * This used to name an endpoint as well as a bearer. It no longer can: the platform collapsed its two GraphQL
 * origins into one, so the only thing left to choose is the token. The two are still not interchangeable, because
 * the same call under each answers about a different subject.
 */
enum class ECrowdyCppTokenPlane : uint8
{
	Game,
	Management
};

/**
 * How to reach the API. Two URLs, but unlike the pair this replaced they are not two planes: ApiUrl is where this
 * client talks, and DiscoveryUrl is the fallback it consults when that stops working.
 *
 * Passed as a struct rather than as two strings on purpose. The old factory took (GameApiUrl, ManagementApiUrl) in
 * that order, and a two-string call carrying the old meaning would still compile while quietly pointing the client
 * at the wrong origin, which only a live server could reveal.
 */
struct FCrowdyCppClientConfig
{
	/**
	 * The API base URL this client issues against. For a per-game client that is the app's OWN datacenter endpoint
	 * (mintAppToken's gameApiUrl, or appDiscovery's), because an app lives in one datacenter and that is where its
	 * shards are. A request answered anywhere else is refused rather than served across a WAN.
	 *
	 * Before the app is known, the shared origin is the right value: it answers everywhere.
	 */
	FString ApiUrl;

	/**
	 * The shared origin, which is a multivalue DNS record over every datacenter's balancer. Deliberately not ApiUrl:
	 * ApiUrl names one instance, and that instance is exactly the thing that can die. Leave it empty and a client
	 * that loses its endpoint can only retry an address that has stopped answering.
	 *
	 * Setting it does not by itself make the client mobile. Re-discovery reads a cached answer that
	 * SetRediscoveredEndpoint supplies, because CrowdyCPP's own bootstrap re-discovery runs on its blocking
	 * transport, which this bridge does not install.
	 */
	FString DiscoveryUrl;

	// What makes two clients the same client. An owner that caches one compares configs to decide whether to reuse
	// it, and a client that has since MOVED still matches the config it was built from: the move is the client
	// recovering on its own, and rebuilding it would undo that and cancel everything in flight to do so.
	bool operator==(const FCrowdyCppClientConfig& Other) const
	{
		return ApiUrl == Other.ApiUrl && DiscoveryUrl == Other.DiscoveryUrl;
	}

	bool operator!=(const FCrowdyCppClientConfig& Other) const { return !(*this == Other); }
};

/** One app's placement, as answered by appDiscovery. */
struct FCrowdyCppAppEndpoint
{
	FString AppID;
	FString DatacenterCode;   // e.g. "or" / "va"; empty when the app has no placement
	FString GameApiUrl;
	FString GameApiWsUrl;

	// True when this entry names somewhere to go. An app with no placement is a legitimate answer meaning "stay on
	// the shared origin", not a broken app, so it is told apart from a failed query rather than folded into one.
	bool IsPlaced() const { return !GameApiUrl.IsEmpty(); }
};

/** The outcome of an appDiscovery query. bOk is the transport-success flag; an app with no placement is still bOk. */
struct FCrowdyCppAppDiscoveryResult
{
	bool bOk = false;
	TArray<FCrowdyCppAppEndpoint> Endpoints;
	FString ErrorMessage;
};

/**
 * One frame of the realtime CONTROL stream: the lifecycle of the API instance this client is connected to, as
 * opposed to anything about gameplay.
 *
 * SERVER_DRAINING is why this exists. It is the only advance warning that an instance is being taken out of
 * service, and a client speaking UDP natively never subscribed to the stream that carries it, so it used to learn
 * its instance was going away by watching it stop answering.
 */
struct FCrowdyCppRealtimeControlEvent
{
	FString Status;    // "failed", or "draining" for the one advisory case
	FString Code;      // branch on this, never on Message: the server is free to reword the text
	FString Message;
	bool bRetryable = true;

	// This instance is going out of service but the stream still works. Move now rather than waiting for it to stop.
	bool IsDraining() const { return Code == TEXT("SERVER_DRAINING"); }

	// The subscription ends right after this, so the cause has to be fixed and the subscription reopened. Draining
	// is the exception: it arrives mid-stream on a healthy subscription and does not end it.
	bool IsTerminal() const { return !IsDraining() && Status == TEXT("failed"); }
};

/**
 * An opaque reference to one issued request, returned by every issuing call. Its only use is to cancel that request
 * before it completes; it carries no result and it does not keep anything alive. A handle whose request has already
 * completed refers to nothing, and cancelling it is a no-op rather than an error.
 */
struct FCrowdyCppRequestHandle
{
	uint64 Id = 0;

	bool IsValid() const { return Id != 0; }
};

/**
 * An opaque reference to one live subscription, returned by Subscribe. Unlike a request handle it refers to
 * something that stays open: it is valid until the caller unsubscribes, the server ends the subscription, or the
 * client is closed.
 */
struct FCrowdyCppSubscriptionHandle
{
	uint64 Id = 0;

	bool IsValid() const { return Id != 0; }
};

/**
 * What one subscription delivers. Every callback runs on the thread that drives Poll(), which is the game thread,
 * and none of them runs after Unsubscribe returns for that subscription.
 *
 * A subscription is a stream rather than a request, so unlike a completion these may run any number of times, or
 * not at all. What is guaranteed is that a subscription which ends for any reason other than the caller's own
 * Unsubscribe says so exactly once, through either OnComplete or a terminal OnError.
 */
struct FCrowdyCppSubscriptionCallbacks
{
	// One server push, carrying the GraphQL response's `data` object.
	TFunction<void(TSharedPtr<FJsonObject> Data)> OnNext;

	// The subscription failed. bTerminal is true when it is over and no further callback will arrive; it is false
	// for a failure the client will recover from by reconnecting and replaying the subscription itself.
	TFunction<void(const FString& Message, bool bTerminal)> OnError;

	// The server ended the subscription normally. Nothing arrives after this.
	TFunction<void()> OnComplete;
};

/**
 * Which generated operation set an operation name is looked up in. Each value maps to one namespace of the API's
 * generated operations, which is the authority on the operation's single-operation GraphQL document, so no query
 * text is maintained by hand here.
 *
 * It is not the authority on which token to send. The platform merged its two GraphQL origins into one and the
 * generated set stopped naming an endpoint at all, while the two tokens still mean different things. The bearer
 * therefore comes from the domain, or from a plane the caller names explicitly.
 */
enum class ECrowdyCppApiDomain : uint8
{
	GameModel,
	Auth,
	Users,
	Apps,
	AppAccess,
	GameApps,
	Organizations,
	CrowdyStudio,
	Teams,
	Channels,
	Avatars,
	State,
	Host,
	Chunks,
	Voxels,
	Actors,
	ServerStatus,
	Teleport,
	Platform,
	Realtime,
	Compute
};

/**
 * Unreal-typed facade over a vendored crowdy::CrowdyClient driving the async,
 * non-throwing API path. It hides the CrowdyCPP client
 * behind a pimpl so this header exposes only Unreal types: callers need neither
 * the CrowdyCPP include path nor exception support, matching the bridge boundary
 * the funnel established.
 *
 * Lifetime: async requests capture into the underlying client, so this facade
 * must outlive every in-flight request; a subsystem that owns one across ticks
 * and calls Poll() each tick satisfies that. Callbacks are delivered from Poll(),
 * so an OnDone should capture only state that outlives the next Poll() (prefer a
 * TWeakObjectPtr / TWeakPtr over a raw pointer).
 *
 * Threading: issue from the game thread only. The bearer for a call is installed on
 * the underlying client immediately before the request is built, and those two steps
 * are individually thread-safe but not atomic together, so two threads issuing at once
 * could interleave and send one plane's token under the other plane's request. Poll()
 * delivers on its calling thread, which is the same thread for the same reason.
 *
 * Every completion is delivered exactly once, and never simply dropped. A request
 * still pending when it is cancelled, when the client is closed, or when the client
 * is destroyed completes right then with a failed result carrying
 * CanceledErrorMessage(). That guarantee is what a latent Blueprint node rests on:
 * a completion that never arrives is indistinguishable from a hang, and leaves an
 * in-flight flag set for the rest of the session.
 */
class CROWDYCPPBRIDGE_API FCrowdyCppClient
{
public:
	// Real client over Unreal's FHttpModule. Returns null if construction failed.
	static TSharedPtr<FCrowdyCppClient> Make(const FCrowdyCppClientConfig& Config);

	// Test client: every request resolves to a fixed canned HTTP response with no
	// network I/O, exercising the real response-interpretation path. It starts
	// with a placeholder game bearer installed so a test that does not care about
	// tokens is not warned about the missing one.
	// The config is optional and matters only to a test that asserts which URL a request was built against.
	static TSharedPtr<FCrowdyCppClient> MakeForTest(const FString& CannedResponseBody, int32 HttpStatus,
		const FCrowdyCppClientConfig& Config = FCrowdyCppClientConfig());

	// What the last request would have carried, for a client from MakeForTest. Reports the resolved endpoint URL
	// and the Authorization header, which together are the only observable proof that an operation was issued
	// against the right origin under the right bearer. False when no request has been issued or this is a real client.
	bool GetLastTestRequest(FString& OutUrl, FString& OutAuthorizationHeader) const;

	// Script per-request (status, body) answers for a client from MakeForTest, consumed in order; requests past the
	// end of the script get the canned response the client was built with. Lets one test drive a sequence such as a
	// datacenter redirect followed by the retried call's answer. No-op on a real client.
	void SetTestResponseScript(TArray<TPair<int32, FString>> Responses);

	// Install a hook that runs while a test client's request is in flight, before its response is delivered. A test
	// uses it to act as a concurrent caller - for example moving this client's endpoint mid-request, the way a
	// parallel request's datacenter redirect would. No-op on a real client.
	void SetTestOnRequest(TFunction<void(const FString& Url)> Hook);

	// The API origin this client is currently issuing against, with the GraphQL path resolved onto it. It moves:
	// a WRONG_DATACENTER redirect and MoveToDatacenter both change it, so read it rather than assuming it is still
	// whatever the config named.
	FString GetApiEndpoint() const;

	~FCrowdyCppClient();

	// Install the bearer used for Game API calls (an app-scoped token, or a manage_apps token for authoring ops).
	void SetGameToken(const FString& Token);

	// Install the bearer used for identity API calls (a user session token).
	void SetManagementToken(const FString& Token);

	// Terminal dispose: closes the underlying transports, delivers every request still pending as canceled, and
	// clears both bearers. Every call issued afterwards fails immediately rather than reaching the network, and the
	// client cannot be reopened, so this belongs in an owner's teardown and nowhere else. Safe to call more than
	// once, and called by the destructor, so releasing the client is a complete teardown on its own.
	void Close();

	// Drain finished async API callbacks on the calling thread. Call once per
	// tick from the game thread so callbacks land where engine objects are safe.
	void Poll();

	// Complete one pending request now with a canceled result, and discard whatever the server eventually answers.
	// The underlying HTTP request is not recalled: what is cancelled is the delivery, so this is the right tool for
	// a caller that has stopped caring (a closed window, an abandoned flow) rather than a way to spare the server
	// work. Returns false for a request that has already completed, which is a no-op rather than an error.
	bool Cancel(FCrowdyCppRequestHandle Handle);

	// Cancel every request still pending, returning how many completions were delivered.
	int32 CancelAll();

	// How many requests have been issued and not yet completed.
	int32 NumPendingRequests() const;

	// The ErrorMessage every canceled completion carries, so a caller can tell a cancellation apart from a
	// transport failure without inspecting anything else.
	static const FString& CanceledErrorMessage();

	// gameModelContainerState. OnDone runs on the calling thread exactly once, from Poll() for a request that
	// reaches the server and inline for one that fails before it is issued. A test using MakeForTest must call
	// Poll() to surface the canned result.
	FCrowdyCppRequestHandle ReadContainerState(int64 AppId, const FString& ContainerId,
		TFunction<void(FCrowdyCppContainerStateResult)> OnDone);

	// gameModelInvoke. ParamsJson is the already-compact-serialized params object (or "{}" for none); it is
	// passed through verbatim as the paramsJson field. SessionId is omitted from the request when empty
	// (app-global scope). Same OnDone contract as ReadContainerState (exactly once, from Poll()).
	FCrowdyCppRequestHandle InvokeFunction(int64 AppId, const FString& FunctionName, const FString& SelfContainerId,
		const FString& SessionId, const FString& ParamsJson,
		TFunction<void(FCrowdyCppInvokeResult)> OnDone);

	// gameModelContainers. TypeName and SessionId are omitted from the request when empty. bOk is the
	// transport-success flag (an empty list is bOk == true, empty array); a canceled read reports bOk false with an
	// empty list. Same OnDone contract as above.
	FCrowdyCppRequestHandle ListContainers(int64 AppId, const FString& TypeName, const FString& SessionId,
		TFunction<void(bool bOk, TArray<TSharedPtr<FJsonObject>>)> OnDone);

	// Issue any generated operation by name. Domain selects the operation set the name is looked up in; the lookup
	// yields that operation's own single-operation GraphQL document, so the request carries no surface the server
	// is not being asked for. A name the domain does not define fails without a round trip. Variables is the full
	// GraphQL variables object. On success the raw `data` object is handed back for the caller to parse. Same
	// OnDone contract as the calls above (exactly once, from Poll()).
	//
	// One origin serves every operation, so all that is left to choose is the bearer, and the two are not
	// interchangeable: the same call under the app-scoped bearer and under the session bearer answers about
	// different things, and picking wrong is a mistake only a live server can reveal. Plane names the one to use;
	// without it the operation's own exception applies, then the domain's plane. A domain whose operations do not
	// share one plane has no such fallback, so a call into one of those fails immediately rather than guessing.
	FCrowdyCppRequestHandle RunOp(ECrowdyCppApiDomain Domain, const FString& OperationName,
		const TSharedPtr<FJsonObject>& Variables, TFunction<void(FCrowdyCppJsonResult)> OnDone,
		TOptional<ECrowdyCppTokenPlane> Plane = TOptional<ECrowdyCppTokenPlane>());

	// Game Model runtime ops (sessions, edges, direct property writes, deletes). Variables is built by the matching
	// FCrowdyGameApiCodec::BuildXVariables and the result is parsed by the matching ParseXEnvelope, so the wire
	// contract and the field extraction are defined in exactly one place.
	FCrowdyCppRequestHandle RunRuntimeOp(const FString& OperationName, const TSharedPtr<FJsonObject>& Variables,
		TFunction<void(FCrowdyCppJsonResult)> OnDone)
	{
		return RunOp(ECrowdyCppApiDomain::GameModel, OperationName, Variables, MoveTemp(OnDone));
	}

	// gameModelSeed. InputJson is a serialized seed input object (container types, property defs, functions) as
	// produced by the kit emit; it is re-parsed and passed as the mutation input. Requires an admin token. Same
	// OnDone contract as the calls above (exactly once, from Poll()).
	FCrowdyCppRequestHandle SeedSchema(const FString& InputJson, TFunction<void(FCrowdyCppStudioOpResult)> OnDone);

	// gameModelUpsertAutomation. InputJson is one serialized automation input object. Requires an admin token.
	// Same OnDone contract as the calls above.
	FCrowdyCppRequestHandle UpsertAutomation(const FString& InputJson,
		TFunction<void(FCrowdyCppStudioOpResult)> OnDone);

	// gameModelUpsertAutomationTrigger. InputJson is one serialized trigger input object. Requires an admin token.
	// Same OnDone contract as the calls above.
	FCrowdyCppRequestHandle UpsertAutomationTrigger(const FString& InputJson,
		TFunction<void(FCrowdyCppStudioOpResult)> OnDone);

	// Sign-in and account identity. The five sign-in paths carry no bearer (they are how a caller obtains one) and
	// the three identity calls carry the session token. Each returns the identity SESSION token, from which an
	// app-scoped gameplay token must then be minted. Same OnDone contract as the calls above: exactly once, from
	// Poll().
	//
	// Sign in against the datacenter the app is served from, not wherever DNS happened to land: resolve the app
	// with ResolveAppEndpoints and move there first, or the session is written in one datacenter and every
	// subsequent call crosses a WAN to reach it.
	FCrowdyCppRequestHandle SignInWithPassword(const FString& Email, const FString& Password,
		TFunction<void(FCrowdyCppAuthResult)> OnDone);

	// Creates the account and signs in. Gamertag is omitted from the request when empty.
	FCrowdyCppRequestHandle RegisterWithPassword(const FString& Email, const FString& Password,
		const FString& Gamertag, TFunction<void(FCrowdyCppAuthResult)> OnDone);

	// Magic-link step 1. RedirectUri is omitted when empty, which asks the server for its default. The value is the
	// { sent } object; the one-time token itself arrives only in the email.
	FCrowdyCppRequestHandle RequestLoginLink(const FString& Email, const FString& RedirectUri,
		TFunction<void(FCrowdyCppJsonValueResult)> OnDone);

	// Magic-link step 2, completing the sign-in with the one-time token from the link.
	FCrowdyCppRequestHandle CompleteLoginLink(const FString& Token, TFunction<void(FCrowdyCppAuthResult)> OnDone);

	// Social step 1. The value is the { authorizeUrl, state } object: open the URL, and bind the callback listener to
	// the state, which is the CSRF material the provider round-trips.
	FCrowdyCppRequestHandle SocialLoginStart(const FString& Provider, const FString& RedirectUri,
		TFunction<void(FCrowdyCppJsonValueResult)> OnDone);

	// Social step 2, completing the sign-in with the code and state captured from the provider callback.
	FCrowdyCppRequestHandle SocialLoginComplete(const FString& Provider, const FString& Code, const FString& State,
		TFunction<void(FCrowdyCppAuthResult)> OnDone);

	// The federated sign-in providers the server currently has enabled. Public: no bearer.
	FCrowdyCppRequestHandle ListLoginProviders(TFunction<void(FCrowdyCppStringListResult)> OnDone);

	// The signed-in user's linked sign-in identities, as a JSON array. Carries the session token.
	//
	// The operation does not select the identity's userId, so a caller that surfaces one has to supply it from the
	// signed-in session rather than read it here. Both this call and LinkIdentity are scoped to the current user, so
	// that substitution is exact rather than a guess.
	FCrowdyCppRequestHandle ListMyIdentities(TFunction<void(FCrowdyCppJsonValueResult)> OnDone);

	// Attach an additional federated identity to the signed-in account, from a social callback's code and state. This
	// does not start a new session. The value is the linked identity object. Carries the session token.
	FCrowdyCppRequestHandle LinkIdentity(const FString& Provider, const FString& Code, const FString& State,
		TFunction<void(FCrowdyCppJsonValueResult)> OnDone);

	// Detach a federated identity. The server refuses to remove the account's last remaining sign-in method, which
	// arrives as a failure rather than as a false answer. Carries the session token.
	FCrowdyCppRequestHandle UnlinkIdentity(const FString& IdentityId, TFunction<void(FCrowdyCppBoolResult)> OnDone);

	// Exchange the identity session token for an app-scoped gameplay token, under the session bearer. The returned
	// token is NOT installed on this client: an owner decides which plane it belongs to. The result also carries
	// the app's own datacenter endpoint and the shared DiscoveryUrl, which together are what let a client relocate.
	FCrowdyCppRequestHandle MintAppToken(int64 AppId, TFunction<void(FCrowdyCppAppTokenResult)> OnDone);

	// Rotate the current app-scoped token for a fresh one before it expires. It carries the GAME bearer rather than
	// the session one, because the token being rotated is what authorizes its own rotation. The replacement is not
	// installed here either, for the same reason as the mint.
	FCrowdyCppRequestHandle RefreshAppToken(TFunction<void(FCrowdyCppAppTokenResult)> OnDone);

	// The same rotation, naming the replication server this client is already connected to so the Game API can
	// authorize the replacement token there and the connection can keep its socket. Read AuthorizedServerIp4 on the
	// result to tell an authorized keep from an answer that still requires a re-assign; the two are not the same and
	// only the first lets a caller stay put. Requires ck-api v1.83.7, which is why the no-server form above stays.
	FCrowdyCppRequestHandle RefreshAppToken(const FString& CurrentServerIp4, int32 CurrentServerClientPort,
		TFunction<void(FCrowdyCppAppTokenResult)> OnDone);

	/**
	 * appDiscovery: where the named apps are served. Carries no bearer, because a client knows its app id (a
	 * build-time constant) long before it holds any credential, and that is the whole point of the call.
	 *
	 * Issue it against the SHARED origin. That name is a multivalue record over every datacenter's balancer, so a
	 * cold client's first request lands wherever DNS pointed it, and roughly half the time that is not the
	 * datacenter hosting the app; authenticating there writes the session in the wrong place. Resolving first and
	 * moving before signing in is what avoids that.
	 *
	 * One call resolves many apps, so a launcher offering several games pays one round trip rather than N. Same
	 * OnDone contract as every other call here (exactly once, from Poll()).
	 */
	FCrowdyCppRequestHandle ResolveAppEndpoints(const TArray<FString>& AppIDs,
		TFunction<void(FCrowdyCppAppDiscoveryResult)> OnDone);

	/**
	 * Point every transport at another datacenter: the GraphQL endpoint, the subscription socket, and the UDP
	 * assignment together. Returns whether the move happened.
	 *
	 * False means nothing changed, which is not necessarily a failure: the endpoint may already be current, empty,
	 * or outside this client's estate. That last one is a deliberate bound rather than a bug. A move target arrives
	 * from a server, and one compromised instance must not be able to walk a whole fleet onto an origin it chose,
	 * so a target whose host does not share the current one's site is refused.
	 *
	 * Moving them together is the point. An HTTP client that follows a redirect while its socket stays behind is
	 * querying one datacenter and playing in another, which does not fail; it just makes every write cross a WAN.
	 */
	bool MoveToDatacenter(const FString& ApiUrl, const FString& WsUrl = FString());

	/**
	 * Supply the answer re-discovery should give when the client next needs one, normally the result of a
	 * ResolveAppEndpoints against the shared origin.
	 *
	 * It is a cached answer rather than a live lookup because CrowdyCPP asks for one synchronously, from whichever
	 * thread noticed the endpoint was dead, and must not be blocked there. This bridge installs no blocking
	 * transport, so CrowdyCPP's own bootstrap re-discovery would find nothing to call and quietly answer "no idea";
	 * keeping a warm answer here is what makes the client actually mobile.
	 *
	 * Refreshing it costs one query and the answer changes only when an operator moves the app, so resolving at
	 * connect time and after each move is enough. Passing empty strings clears it.
	 */
	void SetRediscoveredEndpoint(const FString& ApiUrl, const FString& WsUrl);

	/**
	 * Watch the realtime control stream: SERVER_DRAINING and the terminal codes. Requires an app-scoped bearer, or
	 * the server answers APP_ID_REQUIRED, because game tokens are app-agnostic and one socket is shared across apps.
	 *
	 * Draining is handled here before OnEvent sees it: the client re-discovers and moves immediately, while the
	 * instance still works. OnEvent still receives every event so an owner can log or surface them. Keep the
	 * returned handle alive; unsubscribing ends the watch.
	 */
	FCrowdyCppSubscriptionHandle WatchRealtimeControl(
		TFunction<void(FCrowdyCppRealtimeControlEvent)> OnEvent);

	/**
	 * Open a GraphQL subscription against the Game API over a WebSocket, carrying the game bearer. Variables may be
	 * null for a document that takes none, and OperationName may be empty for a single-operation document.
	 *
	 * One socket is shared by every subscription: the first one opens it, the last one to end closes it, and a
	 * connection that drops is re-established with the open subscriptions replayed. Callbacks are delivered from
	 * Poll(), like every other completion here, so they land on the game thread.
	 *
	 * Returns an invalid handle when the client is unusable, in which case OnError has already run.
	 */
	FCrowdyCppSubscriptionHandle Subscribe(const FString& Document, const TSharedPtr<FJsonObject>& Variables,
		const FString& OperationName, FCrowdyCppSubscriptionCallbacks Callbacks);

	// Open a subscription on a generated operation, named rather than spelled out. The operation set is the
	// authority on the document, exactly as it is for RunOp, so no subscription query text is written by hand and
	// none can drift from the schema. A name the domain does not define fails immediately through OnError.
	FCrowdyCppSubscriptionHandle SubscribeOperation(ECrowdyCppApiDomain Domain, const FString& OperationName,
		const TSharedPtr<FJsonObject>& Variables, FCrowdyCppSubscriptionCallbacks Callbacks);

	// End one subscription. The server is told when the socket is up, and no further callback runs for it. Returns
	// false for a handle that refers to nothing, which is a no-op rather than an error.
	bool Unsubscribe(FCrowdyCppSubscriptionHandle Handle);

	// End every live subscription, returning how many were ended. Callbacks are suppressed, as for Unsubscribe.
	int32 UnsubscribeAll();

	// How many subscriptions are open. A subscription the server has ended is no longer counted.
	int32 NumActiveSubscriptions() const;

	// Test-only WebSocket driving, for a client from MakeForTest: it has no socket, and these play the server by
	// hand instead. That is what makes the graphql-transport-ws handshake testable headlessly, since it is a
	// conversation rather than a single round trip. Each is a no-op on a real client.
	TArray<FString> TakeTestWebSocketSentFrames();
	void TestWebSocketOpen();
	void TestWebSocketReceiveText(const FString& Text);
	void TestWebSocketCloseFromServer(int32 Code, bool bClean);
	bool WasTestWebSocketClosedByClient() const;

	// How many connections have been opened. More than one means the subscription client reconnected, which is the
	// observable a test needs to prove that traffic on the other API plane leaves the socket alone.
	int32 NumTestWebSocketConnections() const;

private:
	FCrowdyCppClient();
	FCrowdyCppClient(const FCrowdyCppClient&) = delete;
	FCrowdyCppClient& operator=(const FCrowdyCppClient&) = delete;

	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
