#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "crowdy/core/result.hpp"
#include "crowdy/domains/domain_base.hpp"
#include "crowdy/graphql/dispatcher.hpp"
#include "crowdy/generated/enums.hpp"
#include "crowdy/graphql/json.hpp"
#include "crowdy/graphql/websocket.hpp"

/// client.exec() — ck-exec: a player's connection to the
/// execution host that runs an app's hubs (stateful, one instance per key) and
/// spokes (stateless, replicated), and a developer's deploys.
///
///   auto exec = client.exec().connect(appId, {.nodeType = "arena", .key = "m1"});
///   exec->call("arena", "m1", "state", {}, [](crowdy::domains::ExecReply r) {
///     if (r.ok()) render(r.value());
///   });
///   client.poll();  // callbacks run here, as for every async surface
///
/// `connect` asks the Game API for a host (`execConnect`, with the app-scoped
/// token of `appId` as the session token) and opens a WebSocket to its gateway.
/// Frames are ck-exec's client protocol; payloads are MessagePack
/// (graphql::Json::toMsgpack / fromMsgpack). The connection recovers by itself:
/// a closed socket or a `Moved` reply asks for a host again, renews every
/// subscription, and retries the call once. Mirrors CrowdyJS `client.exec`.
/// See https://docs.dev.crowdedkingdoms.com/exec/intro.
namespace crowdy::domains {

/// Call statuses as the gateway sends them, by wire value.
enum class ExecStatus : std::uint8_t {
  Ok = 0,
  AppError = 1,
  Busy = 2,
  Moved = 3,
  NotFound = 4,
  DeadlineExceeded = 5,
  Denied = 6,
  RateLimited = 7,
  Unavailable = 8,
  Internal = 9,
  Trapped = 10,
  BadRequest = 11,
  Unknown = 255,
};

std::string_view execStatusName(ExecStatus status) noexcept;
ExecStatus execStatusFromWire(std::uint8_t value) noexcept;
/// Busy, Moved, Unavailable and RateLimited: trying again later can succeed.
bool execStatusRetryable(ExecStatus status) noexcept;

/// ck-exec's client protocol, one WebSocket binary message per frame.
namespace exec_wire {

struct ClientFrame {
  enum class Kind { Call, Subscribe, Unsubscribe, Ping };
  Kind kind = Kind::Call;
  std::uint32_t rid = 0;  // the ping nonce for Ping
  std::string nodeType;
  std::string key;
  std::string method;  // the topic for Subscribe / Unsubscribe
  std::string payload;
};

struct ServerFrame {
  enum class Kind { Reply, Push, Pong };
  Kind kind = Kind::Reply;
  std::uint32_t rid = 0;  // the pong nonce for Pong
  std::uint8_t status = 0;
  std::string nodeType;
  std::string key;
  std::string topic;
  std::string payload;
};

/// InvalidArgument when a name does not fit its length prefix.
Result<std::string> encode(const ClientFrame& frame);
/// Malformed for a truncated frame or an unknown type.
Result<ServerFrame> decode(std::string_view bytes);

}  // namespace exec_wire

/// Where a player connects, and the connect token for it (about a minute).
struct ExecEndpoint {
  std::string gatewayUrl;
  std::string token;
  std::string host;
};

/// A call's answer. The SDK never retries a `Busy` reply: a gateway refuses a player's
/// calls over its limit (120 per 10 s per player and app on a host) as `Busy` with a
/// message starting "rate limited", and calling again before `retryAfterMs()` is refused
/// again.
struct ExecReply {
  ExecStatus status = ExecStatus::Unavailable;
  /// MessagePack for Ok; the refusal's message (UTF-8) otherwise.
  std::string payload;

  bool ok() const { return status == ExecStatus::Ok; }
  bool retryable() const { return execStatusRetryable(status); }
  /// The caller's call limit refused it (`Busy` "rate limited ...", or `RateLimited`).
  bool rateLimited() const;
  /// How long to wait before calling again, when a rate-limit refusal says (`retry in N ms`).
  std::optional<long> retryAfterMs() const;
  /// The payload decoded from MessagePack (a null Json when it is not).
  graphql::Json value() const { return graphql::Json::fromMsgpack(payload); }
  std::string message() const { return ok() ? std::string() : payload; }
};

/// A published message on a topic this connection subscribes to.
struct ExecPush {
  std::string nodeType;
  std::string key;
  std::string topic;
  std::string payload;

  graphql::Json value() const { return graphql::Json::fromMsgpack(payload); }
};

using ExecReplyCallback = std::function<void(ExecReply)>;
using ExecPushHandler = std::function<void(const ExecPush&)>;
/// Finds a host: calls `found` once, from any thread.
using ExecDial = std::function<void(std::function<void(Result<ExecEndpoint>)> found)>;

struct ExecConnectOptions {
  /// Put the player on the host running this node type (with `key`).
  std::string nodeType;
  /// The instance key within `nodeType`; empty for the root hub or a spoke.
  std::string key;
  /// How long a call waits for its reply.
  long callTimeoutMs = 10000;
  long openTimeoutMs = 10000;
  /// Connect again when the socket closes unexpectedly.
  bool reconnect = true;
  long initialReconnectDelayMs = 250;
  long maxReconnectDelayMs = 5000;
};

/// Filters for `ExecAPI::logs`. Empty strings and negative numbers mean "not set".
struct ExecLogsQuery {
  std::string nodeType;
  std::string key;
  /// The least severe level included: 0 errors only ... 3 everything (the default).
  int maxLevel = -1;
  /// Only lines older than this line id, to page back.
  std::string before;
  /// At most this many lines (default 100, at most 500).
  int limit = -1;
  /// Only lines of this flow (a line's `flow`, 32 hex digits): one call through every hub
  /// and host. `ExecAPI::logs` only; mod logs take no flow.
  std::string flow;
};

/// One player's connection to a ck-exec host. Thread-safe. Every callback runs
/// through the dispatcher (CrowdyClient::poll()); without one, on the
/// transport's thread.
class ExecConnection {
 public:
  ExecConnection(std::shared_ptr<graphql::IWebSocketTransport> transport,
                 std::shared_ptr<graphql::Dispatcher> dispatcher, ExecDial dial,
                 ExecConnectOptions options = {});
  ~ExecConnection();

  ExecConnection(const ExecConnection&) = delete;
  ExecConnection& operator=(const ExecConnection&) = delete;

  /// A connection to a known gateway with a connect token you already have
  /// (tools and tests). It does not reconnect, and it dials the URL it is given:
  /// `execGatewayRefusal` judges only what the Game API names.
  static std::shared_ptr<ExecConnection> open(
      std::shared_ptr<graphql::IWebSocketTransport> transport,
      std::shared_ptr<graphql::Dispatcher> dispatcher, ExecEndpoint endpoint,
      ExecConnectOptions options = {});

  /// Start connecting. `done` fires once: Ok when open, the failure otherwise
  /// (Rejected when the gateway refused the connect token, `lastFailure()` says why).
  /// Calls made before then wait for the connection.
  void connect(std::function<void(Status)> done = {});

  /// Call a node's endpoint with raw bytes. `done` fires once.
  void callRaw(std::string nodeType, std::string key, std::string method,
               std::string payload, ExecReplyCallback done);
  /// Call with `args` as MessagePack; decode the reply with ExecReply::value().
  void call(std::string nodeType, std::string key, std::string method,
            const graphql::JVal& args, ExecReplyCallback done);
  /// Receive what a node publishes on `topic`. Returns a handle for
  /// unsubscribe(); `done` fires with the gateway's answer to the first one.
  std::uint64_t subscribe(std::string nodeType, std::string key, std::string topic,
                          ExecPushHandler onPush, ExecReplyCallback done = {});
  /// Stop one handler (and the subscription, when it was the last one).
  void unsubscribe(std::uint64_t handle);
  void ping(ExecReplyCallback done);

  /// Called with the new host after every reconnect.
  void onReconnect(std::function<void(std::string host)> listener);

  std::string host() const;
  bool connected() const;
  /// Why the last attempt to connect failed, or the open connection was lost: nullopt until
  /// then, and again once a connection opens. Since ck-exec 0.10.0 a gateway refuses a bad
  /// connect token with HTTP 401 and its reason before any WebSocket exists, which is `Denied`
  /// with that reason (the transport's `WebSocketError::httpStatus` / `httpBody`), and a player
  /// past their session cap with 429, which is `Unavailable`. A gateway before 0.10.0 closed
  /// with 4401, also `Denied`. Calls waiting on the attempt fail with the same reply.
  std::optional<ExecReply> lastFailure() const;
  /// Close; nothing reconnects and pending calls fail as Unavailable.
  void close();

 private:
  friend class ExecAPI;
  /// Dial only gateways `execGatewayRefusal` passes for the Game API `gameApiUrl` names.
  void pinGateway(std::function<std::string()> gameApiUrl);

  class Impl;
  std::shared_ptr<Impl> impl_;
};

/// One node type of a deploy: its compiled module and its manifest settings.
struct ExecNodeType {
  std::string name;
  /// "hub" or "spoke".
  std::string kind;
  /// The compiled module (`wasm32-unknown-unknown`, built with ckx-sdk). Leave it empty
  /// and set `crate` to name a module of the deploy's `buildId` instead.
  std::string wasm;
  /// A crate of the deploy's `buildId`, when `wasm` is empty.
  std::string crate;
  /// The type that owns this one; empty for the root.
  std::string parent;
  bool client = false;
  /// Types it may call and subscribe to; "*" for any.
  std::vector<std::string> calls;
  /// Any other manifest fields (`persist_every_ms`, `replicas`, `seed_b64`, ...).
  graphql::JVal extra;
};

/// One crate for `ExecAPI::build`: its files as (path, content), e.g. `Cargo.toml` and
/// `src/lib.rs`.
struct ExecCrate {
  std::string name;
  std::vector<std::pair<std::string, std::string>> files;
};

/// The CLIENT ABI a served CLIENT half must be built for (crowdy-client-sdk `ABI_VERSION`);
/// `ExecAPI::modClientArtifactBytes` refuses any other. CrowdyJS `EXEC_CLIENT_ABI_VERSION`.
inline constexpr int kExecClientAbiVersion = 0;

/// A CLIENT half's capability summary, which the build derives from the module and its author
/// cannot declare. Visitors consent to its hash.
struct ExecClientCapabilitySummary {
  int version = 0;
  /// "client".
  std::string target;
  /// The module's WASM imports as `module.name`.
  std::vector<std::string> imports;
  /// The client host calls it can reach.
  std::vector<std::string> hostFunctions;
  /// The host catalog groups of those calls.
  std::vector<std::string> capabilityGroups;
  /// HUD and overlay hooks among them.
  std::vector<std::string> presentationHooks;
  /// The functions the module exports.
  std::vector<std::string> exportedFunctions;
};

/// A served CLIENT half, decoded and checked by `ExecAPI::modClientArtifactBytes`.
struct ExecModClientArtifactBytes {
  std::string modId;
  /// The mod's name: its name on the grid event bus.
  std::string name;
  std::string gridId;
  int clientVersion = 0;
  /// The module; its SHA-256 is `digest`.
  std::vector<std::uint8_t> bytes;
  /// SHA-256 of `bytes`, lowercase hex.
  std::string digest;
  int sizeBytes = 0;
  /// Fuel for each dispatch (init, tick, invoke, event), a GraphQL BigInt as decimal text: load
  /// it into the module's `ck_fuel` global before every call.
  std::string fuelPerDispatch;
  /// How often to tick it, in milliseconds (16-1000).
  int tickIntervalMs = 0;
  std::string capabilitySummaryJson;
  /// What the player consented to: let the module call only its `hostFunctions`.
  ExecClientCapabilitySummary capabilitySummary;
  std::string capabilityHash;
  int abiVersion = 0;
};

using ExecModClientArtifactBytesCallback =
    std::function<void(graphql::GraphQLOutcome, ExecModClientArtifactBytes)>;

class ExecAPI : public DomainBase {
 public:
  ExecAPI(std::shared_ptr<graphql::GraphQLClient> gql,
          std::shared_ptr<graphql::IWebSocketTransport> transport);

  /// A host for the signed-in player and its connect token (`execConnect`),
  /// blocking. The session token must be the app-scoped token of `appId`. `connect` dials
  /// its gateway only when `execGatewayRefusal` passes it.
  Result<ExecEndpoint> endpoint(std::string appId, std::string nodeType = {}, std::string key = {}) const;
  void endpointAsync(std::string appId, std::string nodeType, std::string key,
                     std::function<void(Result<ExecEndpoint>)> done) const;

  /// Connect the signed-in player to ck-exec for `appId`. Returns at once; the
  /// connection opens in the background and calls made meanwhile wait for it. A gateway
  /// `execGatewayRefusal` refuses is never dialed: the attempt fails `Unavailable` with the
  /// reason (`lastFailure()`), and a reconnect asks the Game API again.
  std::shared_ptr<ExecConnection> connect(std::string appId, ExecConnectOptions options = {}) const;
  /// The same, calling back once the connection is open (or with the failure).
  void connectAsync(std::string appId, ExecConnectOptions options,
                    std::function<void(Result<std::shared_ptr<ExecConnection>>)> done) const;

  /// Deploy a new version of the app's nodes and make it active (`execDeploy`),
  /// blocking: the manifest with each module's SHA-256, and each distinct module
  /// once. Returns `{ version }`. Requires the org `manage_compute` permission.
  /// With `buildId`, a type may leave `wasm` empty and name a `crate` of that build.
  graphql::Json deploy(std::string appId, std::string root, const std::vector<ExecNodeType>& types,
                       std::string buildId = {}) const;
  void deployAsync(std::string appId, std::string root, const std::vector<ExecNodeType>& types,
                   graphql::GraphQLCallback done) const;
  void deployAsync(std::string appId, std::string root, const std::vector<ExecNodeType>& types,
                   std::string buildId, graphql::GraphQLCallback done) const;

  // ---- builds ----

  /// The starter packs (`execStarters`), which replace the compute templates:
  /// `{ manifestJson, starters: [{ crate, nodeType, description, files: [{ path, content }] }] }`.
  /// `manifestJson`'s types name their crate, for `deploy` with the build's id. Requires
  /// `manage_compute`.
  graphql::Json starters(std::string appId) const;
  void startersAsync(std::string appId, graphql::GraphQLCallback done) const;
  /// Build crates into modules on the platform (`execBuild`), so you need no Rust
  /// toolchain. Returns the build at once, queued: `{ buildId, status, kind, log, createdAt,
  /// startedAt, finishedAt, artifacts: [{ crate, digest, sizeBytes, capabilitySummaryJson,
  /// capabilityHash, tickIntervalMs }] }`. `kind` is `exec` here and `client` for a CLIENT half
  /// (`modClientBuild`), whose artifact alone carries the three capability fields. Requires
  /// `manage_compute`.
  graphql::Json build(std::string appId, const std::vector<ExecCrate>& crates) const;
  void buildAsync(std::string appId, const std::vector<ExecCrate>& crates, graphql::GraphQLCallback done) const;
  /// A build's status, log and modules (`execBuildStatus`), or null. Requires
  /// `view_compute_diagnostics`.
  graphql::Json buildStatus(std::string appId, std::string buildId) const;
  void buildStatusAsync(std::string appId, std::string buildId, graphql::GraphQLCallback done) const;
  /// Polls `buildStatus` every `intervalMs`, blocking, until the build succeeds or fails, and
  /// returns it; a failed build's `log` says why. After `timeoutMs` it returns the last
  /// status, still `queued` or `building`; null when the app has no such build. From an
  /// event loop, poll `buildStatusAsync` on your own timer instead.
  graphql::Json waitForBuild(std::string appId, std::string buildId, int intervalMs = 2000,
                             int timeoutMs = 600000) const;

  // ---- mods: players' code on grids they own ----
  //
  // A mod is the node type `mod:<name>` (`execModType`) keyed by its grid's id; players call it
  // through an `ExecConnection` like any node. Deploying, installing, publishing and deleting
  // need to own the grid and `write_server_code` on the access tier and the grid; switching on
  // or off needs `run_server_code` there, and on also the app's code admission.

  /// The mod starter (`execModStarter`): `{ crate, nodeType, description, files }`.
  graphql::Json modStarter(std::string appId) const;
  void modStarterAsync(std::string appId, graphql::GraphQLCallback done) const;
  /// Build a mod from one crate (`execModBuild`); one build at a time per player. Needs
  /// `write_server_code` in the app.
  graphql::Json modBuild(std::string appId, const ExecCrate& crate) const;
  void modBuildAsync(std::string appId, const ExecCrate& crate, graphql::GraphQLCallback done) const;
  /// A mod build of yours (`execModBuildStatus`): a mod's (`kind` `exec`) or a CLIENT half's
  /// (`kind` `client`).
  graphql::Json modBuildStatus(std::string appId, std::string buildId) const;
  void modBuildStatusAsync(std::string appId, std::string buildId, graphql::GraphQLCallback done) const;
  /// Polls `modBuildStatus`, server or CLIENT, blocking, like `waitForBuild`.
  graphql::Json waitForModBuild(std::string appId, std::string buildId, int intervalMs = 2000,
                                int timeoutMs = 600000) const;
  /// Deploy a mod build of yours to a grid you own (`execModDeploy`): a new mod starts off.
  graphql::Json modDeploy(std::string appId, std::string gridId, std::string name, std::string buildId) const;
  void modDeployAsync(std::string appId, std::string gridId, std::string name, std::string buildId,
                      graphql::GraphQLCallback done) const;
  /// Switch a mod on your grid on or off (`execModSetEnabled`).
  graphql::Json modSetEnabled(std::string appId, std::string gridId, std::string name, bool enabled) const;
  void modSetEnabledAsync(std::string appId, std::string gridId, std::string name, bool enabled,
                          graphql::GraphQLCallback done) const;
  /// Stop and remove a mod on your grid, with its state (`execModDelete`).
  graphql::Json modDelete(std::string appId, std::string gridId, std::string name) const;
  void modDeleteAsync(std::string appId, std::string gridId, std::string name, graphql::GraphQLCallback done) const;
  /// A grid's mods (`execMods`).
  graphql::Json mods(std::string appId, std::string gridId) const;
  void modsAsync(std::string appId, std::string gridId, graphql::GraphQLCallback done) const;
  /// Your mods in the app (`execMyMods`).
  graphql::Json myMods(std::string appId) const;
  void myModsAsync(std::string appId, graphql::GraphQLCallback done) const;
  /// A mod of yours' log lines (`execModLogs`); `query.nodeType` and `query.key` are ignored.
  graphql::Json modLogs(std::string appId, std::string gridId, std::string name, const ExecLogsQuery& query = {}) const;
  void modLogsAsync(std::string appId, std::string gridId, std::string name, const ExecLogsQuery& query,
                    graphql::GraphQLCallback done) const;
  /// Publish a mod of yours for other grid owners to install (`execModPublish`; no payments).
  graphql::Json modPublish(std::string appId, std::string gridId, std::string name, std::string title,
                           std::string description = {}) const;
  void modPublishAsync(std::string appId, std::string gridId, std::string name, std::string title,
                       std::string description, graphql::GraphQLCallback done) const;
  /// The app's listed mods (`execModListings`), each with the CLIENT half it had when published:
  /// `clientDigest`, `clientCapabilitySummaryJson`, `clientCapabilityHash` and
  /// `clientTickIntervalMs`, null without one.
  graphql::Json modListings(std::string appId) const;
  void modListingsAsync(std::string appId, graphql::GraphQLCallback done) const;
  /// Delist a listing you published (`execModUnpublish`).
  graphql::Json modUnpublish(std::string appId, std::string listingId) const;
  void modUnpublishAsync(std::string appId, std::string listingId, graphql::GraphQLCallback done) const;
  /// Install a listing onto a grid you own as your own mod, switched off (`execModInstall`), with
  /// the listing's CLIENT half if it has one; visitors, you too, consent to that CLIENT half afresh.
  graphql::Json modInstall(std::string appId, std::string gridId, std::string name, std::string listingId) const;
  void modInstallAsync(std::string appId, std::string gridId, std::string name, std::string listingId,
                       graphql::GraphQLCallback done) const;
  /// The app's mods by grid or owner, or all (`execAppMods`). Needs `view_compute_diagnostics`.
  graphql::Json appMods(std::string appId, std::string gridId = {}, std::string ownerId = {}) const;
  void appModsAsync(std::string appId, std::string gridId, std::string ownerId, graphql::GraphQLCallback done) const;
  /// The switches of the mods kill ladder that are off (`execModSwitches`). Needs
  /// `view_compute_diagnostics`.
  graphql::Json modSwitches(std::string appId) const;
  void modSwitchesAsync(std::string appId, graphql::GraphQLCallback done) const;
  /// The kill ladder (`execModSetSwitch`): one mod, a player's, a grid's, a listing's installs,
  /// or all (no target). Needs `manage_compute`.
  graphql::Json modSetSwitch(std::string appId, gen::ExecModScope scope, bool off, std::string target = {},
                             std::string reason = {}) const;
  void modSetSwitchAsync(std::string appId, gen::ExecModScope scope, bool off, std::string target, std::string reason,
                         graphql::GraphQLCallback done) const;

  // ---- CLIENT halves: a mod's browser half ----
  //
  // A mod may carry a CLIENT half: WASM built from a `crowdy-client-sdk` crate, which its grid
  // serves to visitors who consent to its capability hash or trust its author. The SDK runs no
  // WASM. A native engine lists a grid's CLIENT halves, asks the player, fetches each consented one
  // with `modClientArtifactBytes` and runs it in its own sandbox, letting it call only its
  // summary's `hostFunctions`. Building, attaching and detaching need `write_client_code`.

  /// Build the CLIENT half of a mod from one crowdy-client-sdk crate (`execModClientBuild`):
  /// compiled for wasm32-unknown-unknown in the build sandbox, fuel-metered, checked against the
  /// CLIENT ABI and at most 512 KiB, its capability summary derived from the module. Its
  /// `Cargo.toml` may have only `[package]`, `[lib]` as a cdylib, `[dependencies]` on
  /// `crowdy-client-sdk`, `serde` and `serde_json`, and `[package.metadata.crowdy]
  /// tick_interval_ms`. Returns the build queued (`kind` `client`); wait with `waitForModBuild`,
  /// then attach it with `modClientDeploy`. One build, server or CLIENT, at a time per player.
  graphql::Json modClientBuild(std::string appId, const ExecCrate& crate) const;
  void modClientBuildAsync(std::string appId, const ExecCrate& crate, graphql::GraphQLCallback done) const;
  /// Attach a succeeded CLIENT build of yours to your mod `name` on a grid you own, replacing the
  /// CLIENT half it had (`execModClientDeploy`): `{ modId, gridId, name, ownerId, clientVersion,
  /// digest, sizeBytes, capabilitySummaryJson, capabilityHash, tickIntervalMs, updatedAt }`, its
  /// `clientVersion` one higher. The mod must run as you and the app's code admission must admit
  /// the new version. A visitor's consent carries over only while the capability hash is unchanged.
  graphql::Json modClientDeploy(std::string appId, std::string gridId, std::string name, std::string buildId) const;
  void modClientDeployAsync(std::string appId, std::string gridId, std::string name, std::string buildId,
                            graphql::GraphQLCallback done) const;
  /// Detach the CLIENT half of a mod on your grid, with every visitor's consent to it
  /// (`execModClientDelete`); the mod keeps running.
  graphql::Json modClientDelete(std::string appId, std::string gridId, std::string name) const;
  void modClientDeleteAsync(std::string appId, std::string gridId, std::string name,
                            graphql::GraphQLCallback done) const;
  /// The CLIENT halves a grid serves (`execGridClientMods`): of its mods that are switched on, not
  /// stopped by the kill ladder, running as the grid's owner and admitted. Each is `{ modId, name,
  /// gridId, authorId, listingId, clientVersion, digest, capabilitySummaryJson, capabilityHash,
  /// tickIntervalMs, callerConsented, authorCapabilitySummaryJson, authorCapabilityHash,
  /// callerTrustsAuthor, updatedAt }`; the author's summary is the union of their CLIENT halves on
  /// the grid. Ask once per author (`trustAuthor`) or per CLIENT half (`consentClientMod`), fetch
  /// with `modClientArtifactBytes`, cache by `digest`, and poll this to stop the CLIENT halves no
  /// longer listed or whose digest changed. Requires access to the app.
  graphql::Json gridClientMods(std::string appId, std::string gridId) const;
  void gridClientModsAsync(std::string appId, std::string gridId, graphql::GraphQLCallback done) const;
  /// Consent to run one mod's CLIENT half at `capabilityHash`, the one `gridClientMods` showed you
  /// (`execConsentClientMod`). A CLIENT half whose capabilities change carries a new hash and the
  /// consent stops holding; a hash that is not the current one is refused as `CONFLICT`.
  graphql::Json consentClientMod(std::string appId, std::string modId, std::string capabilityHash) const;
  void consentClientModAsync(std::string appId, std::string modId, std::string capabilityHash,
                             graphql::GraphQLCallback done) const;
  /// Trust one author's CLIENT halves on a grid you stand in at the hash of their union,
  /// `authorCapabilityHash` (`execTrustAuthor`): it covers their CLIENT halves there while the union
  /// is no wider, and consents to each current one at its own hash. A hash that is not the current
  /// one is `CONFLICT`; not standing in the grid, or an author with nothing served there, is
  /// `NOT_FOUND`.
  graphql::Json trustAuthor(std::string appId, std::string gridId, std::string authorId,
                            std::string capabilityHash) const;
  void trustAuthorAsync(std::string appId, std::string gridId, std::string authorId, std::string capabilityHash,
                        graphql::GraphQLCallback done) const;
  /// Take back your consent to one mod's CLIENT half, whatever hash you consented to
  /// (`execRevokeClientModConsent`); `true` when you had consented. While you trust its author on
  /// its grid it is still served to you: `revokeAuthorTrust` takes that back.
  graphql::Json revokeClientModConsent(std::string appId, std::string modId) const;
  void revokeClientModConsentAsync(std::string appId, std::string modId, graphql::GraphQLCallback done) const;
  /// Stop trusting an author on a grid and take back your consent to each of their CLIENT halves
  /// there (`execRevokeAuthorTrust`), so none is served to you until you consent or trust again;
  /// `true` when anything was taken back. Works from anywhere, not only inside the grid.
  graphql::Json revokeAuthorTrust(std::string appId, std::string gridId, std::string authorId) const;
  void revokeAuthorTrustAsync(std::string appId, std::string gridId, std::string authorId,
                              graphql::GraphQLCallback done) const;
  /// A served CLIENT half's module, base64, with what a runtime needs to run it
  /// (`execModClientArtifact`): `{ modId, name, gridId, clientVersion, digest, wasmBase64,
  /// sizeBytes, capabilitySummaryJson, capabilityHash, tickIntervalMs, fuelPerDispatch, abiVersion }`.
  /// Served only to a player holding `run_client_code` in the app, standing in the mod's grid now,
  /// who consented to it at its current hash or trusts its author at a union no wider; every
  /// refusal is `NOT_FOUND`. At most 12 fetches a minute per player and mod on each API instance
  /// (`RATE_LIMITED`): a digest never changes its bytes, so cache the module by `digest`.
  graphql::Json modClientArtifact(std::string appId, std::string modId) const;
  void modClientArtifactAsync(std::string appId, std::string modId, graphql::GraphQLCallback done) const;
  /// `modClientArtifact` decoded and checked for a native runtime: the bytes with their SHA-256
  /// recomputed, and the capability summary parsed. Bytes that differ from `digest`, a CLIENT ABI
  /// other than `kExecClientAbiVersion`, or a capability summary that does not parse are refused
  /// and never returned: blocking, as a graphql::CrowdyProtocolError (an empty result in a
  /// CROWDY_NO_EXCEPTIONS build); async, as an outcome of kind `Protocol` with the reason in
  /// `errorMessage`. GraphQL refusals (`NOT_FOUND`, `RATE_LIMITED`) arrive as for any call.
  ExecModClientArtifactBytes modClientArtifactBytes(std::string appId, std::string modId) const;
  void modClientArtifactBytesAsync(std::string appId, std::string modId,
                                   ExecModClientArtifactBytesCallback done) const;

  // ---- operations ----

  /// A host and a developer connect token for `appId` (`execConnectAsDeveloper`),
  /// blocking. The session's calls arrive as `Caller::Developer` with your user id and
  /// may reach any node type, not only `client` ones. Requires the org
  /// `manage_compute` permission and your own session token, not an app token.
  Result<ExecEndpoint> developerEndpoint(std::string appId, std::string nodeType = {}, std::string key = {}) const;
  void developerEndpointAsync(std::string appId, std::string nodeType, std::string key,
                              std::function<void(Result<ExecEndpoint>)> done) const;
  /// Connect as one of the app's developers (studio tools, manual runs, admin
  /// endpoints). The same connection as `connect`, reconnecting with a fresh
  /// developer token.
  std::shared_ptr<ExecConnection> connectAsDeveloper(std::string appId, ExecConnectOptions options = {}) const;
  void connectAsDeveloperAsync(std::string appId, ExecConnectOptions options,
                               std::function<void(Result<std::shared_ptr<ExecConnection>>)> done) const;

  /// Guest log lines (`execLogs`), newest first, kept for 24 hours: an array of
  /// `{ id, nodeType, key, level, host, at, text, flow }`. `flow` is the call the line was
  /// written in (32 lowercase hex digits, shared by everything it caused), null outside a
  /// call; filter by it with `query.flow`. Requires `view_compute_diagnostics`.
  graphql::Json logs(std::string appId, const ExecLogsQuery& query = {}) const;
  void logsAsync(std::string appId, const ExecLogsQuery& query, graphql::GraphQLCallback done) const;
  /// What the manager has placed (`execInstances`). Requires `view_compute_diagnostics`.
  graphql::Json instances(std::string appId) const;
  void instancesAsync(std::string appId, graphql::GraphQLCallback done) const;
  /// The app's versions, newest first (`execVersions`): `{ version, createdBy, createdAt,
  /// types, active, manifestJson }`. `manifestJson` is the deployed manifest (a type's spawn
  /// seed shown as `seed_bytes`; parse it with graphql::Json::parse), null when the version's
  /// row is gone. Requires `view_compute_diagnostics`.
  graphql::Json versions(std::string appId) const;
  void versionsAsync(std::string appId, graphql::GraphQLCallback done) const;
  /// Calls to each endpoint over the last `sinceMinutes` (default 60, at most 10080), by
  /// outcome, most called first (`execEndpointStats`): `{ nodeType, method, calls, appErrors,
  /// busy, denied, deadlineExceeded, otherErrors, timedCalls, latencyMsAvg, latencyMsMax,
  /// firstMinute, lastMinute }`. An empty `nodeType` and a negative `sinceMinutes` mean
  /// "not set". Requires `view_compute_diagnostics`.
  graphql::Json endpointStats(std::string appId, std::string nodeType = {}, int sinceMinutes = -1) const;
  void endpointStatsAsync(std::string appId, std::string nodeType, int sinceMinutes,
                          graphql::GraphQLCallback done) const;
  /// `{ activeVersion, disabled, disabledTypes, budgetPaused }` (`execAppStatus`).
  /// Requires `view_compute_diagnostics`.
  graphql::Json status(std::string appId) const;
  void statusAsync(std::string appId, graphql::GraphQLCallback done) const;
  /// Make an earlier version active again, a rollback (`execActivateVersion`);
  /// instances pick it up when they next start. Requires `manage_compute`.
  graphql::Json activateVersion(std::string appId, int version) const;
  void activateVersionAsync(std::string appId, int version, graphql::GraphQLCallback done) const;
  /// The kill switch, for the whole app or one node type (`execSetEnabled`). Off:
  /// nothing of it is placed, what runs is persisted and stopped, calls are refused
  /// with `Denied`. Requires `manage_compute`.
  graphql::Json setEnabled(std::string appId, bool enabled, std::string nodeType = {}) const;
  void setEnabledAsync(std::string appId, bool enabled, std::string nodeType, graphql::GraphQLCallback done) const;

 private:
  ExecDial dialer(std::string appId, std::string nodeType, std::string key, bool developer = false) const;
  /// A connection over `dial` that dials only gateways the Game API this client talks to may name.
  std::shared_ptr<ExecConnection> pinned(ExecDial dial, ExecConnectOptions options) const;
  static graphql::JVal deployVariables(std::string appId, std::string root, const std::vector<ExecNodeType>& types,
                                       const std::string& buildId);

  std::shared_ptr<graphql::IWebSocketTransport> transport_;
};

/// The SHA-256 of `bytes` as lowercase hex, as a deploy names modules.
std::string execSha256Hex(std::string_view bytes);

/// The node type players call a mod by: `mod:<name>`, keyed by its grid's id.
std::string execModType(std::string_view name);

/// Why `ExecAPI::connect` will not send a connect token to `gatewayUrl`, or nullopt when it
/// will. The Game API names the gateway, and the token rides in its query string, so a gateway
/// must be `ws:` or `wss:` (`wss:` whenever the Game API is `https:`), carry no credentials, and
/// be on the estate of the Game API or of this release's default origin (`kDefaultHttpOrigin`),
/// as `graphql::isSameEstate` bounds a move; two IP literals must be equal. A Game API on
/// loopback (ck-exec's local cluster) may also name a loopback gateway. Mirrors CrowdyJS
/// `execGatewayRefusal`; `tools/parity/fixtures/exec-gateway-cases.json` holds both to the same
/// answers. Apply it before dialing an `endpoint()` yourself.
std::optional<std::string> execGatewayRefusal(std::string_view gameApiUrl, std::string_view gatewayUrl);

/// Parse a CLIENT half's capability summary: `capabilitySummaryJson`, `authorCapabilitySummaryJson`
/// or a listing's `clientCapabilitySummaryJson`. nullopt unless it is a JSON object whose
/// `hostFunctions` is an array of strings, since nothing else can bound the module's host calls.
std::optional<ExecClientCapabilitySummary> parseExecClientCapabilitySummary(std::string_view json);

}  // namespace crowdy::domains
