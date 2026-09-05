#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "crowdy/core/result.hpp"
#include "crowdy/default_origin.hpp"
#include "crowdy/domains/auth.hpp"
#include "crowdy/domains/discovery.hpp"
#include "crowdy/domains/realtime_control.hpp"
#include "crowdy/domains/game_apps.hpp"
#include "crowdy/domains/game_model.hpp"
#include "crowdy/domains/crowdy_studio_agent.hpp"
#include "crowdy/domains/player_compute.hpp"
#include "crowdy/domains/player_wallet.hpp"
#include "crowdy/domains/marketplace.hpp"
#include "crowdy/domains/player_model.hpp"
#include "crowdy/domains/groups.hpp"
#include "crowdy/domains/portal.hpp"
#include "crowdy/domains/server_status.hpp"
#include "crowdy/domains/users.hpp"
#include "crowdy/domains/world_data.hpp"
#include "crowdy/graphql/estate.hpp"
#include "crowdy/graphql/graphql_client.hpp"
#include "crowdy/graphql/rediscover.hpp"
#include "crowdy/replication/types.hpp"
#include "crowdy/graphql/subscription_client.hpp"

#ifndef CROWDY_NO_EXCEPTIONS
#include "crowdy/domains/compute.hpp"
#include "crowdy/domains/crowdy_studio.hpp"
#endif

namespace crowdy {

namespace domains {
class AdminAPI;
class OperatorAPI;
}  // namespace domains
namespace agent {
#ifndef CROWDY_NO_EXCEPTIONS
struct CrowdyStudioAgentControllerOptions;
class CrowdyStudioAgentControllerRuntime;
#endif
}  // namespace agent
namespace studio {
#ifndef CROWDY_NO_EXCEPTIONS
struct CrowdyStudioIntegrationOptions;
class CrowdyStudioIntegration;
#endif
}  // namespace studio
namespace graphql {
class Dispatcher;
}
namespace replication {
class ReplicationClient;
}

/// Client configuration. Follows the CrowdyJS two-TOKEN model, which survives
/// the collapse to one origin: build one identity client (the shared origin +
/// session token) and one client per game (the app's own gameApiUrl from
/// mintAppToken + the app-scoped token). What changed in 0.20.0 is that both are
/// the same KIND of endpoint — there is no second API to point at.
struct ClientConfig {
  /// API base URL. One origin serves identity and gameplay alike; for a
  /// per-game client this is the app's own datacenter endpoint
  /// (mintAppToken's gameApiUrl), because that is where its shards live.
  ///
  /// LEAVE EVERY ORIGIN EMPTY AND THE CLIENT USES crowdy::kDefaultHttpOrigin —
  /// the public CK API origin for the tier this build was released for, from
  /// the generated `crowdy/default_origin.hpp`. The rule is ALL-OR-NOTHING: set
  /// any one of httpUrl / wsUrl / wsEndpoint and nothing is invented around it,
  /// because a client whose HTTP is on your host and whose WEBSOCKET is on the
  /// tier default is one session split across two origins, looking connected.
  std::string httpUrl;
  /// The SHARED origin (multivalue DNS over every datacenter's balancer). Set
  /// this and a client that loses its endpoint can ask where to go next; leave
  /// it empty and the only option is to retry an address that has stopped
  /// answering. Comes back on mintAppToken/gameClientBootstrap as discoveryUrl,
  /// and is deliberately NOT httpUrl: under direct connect httpUrl names one
  /// instance, which is exactly the thing that can die.
  std::string discoveryUrl;
  /// Custom re-discovery. When null and discoveryUrl is set, the client builds
  /// one that queries gameClientBootstrap against discoveryUrl using the token
  /// it already holds. Supply your own to re-mint instead (the equivalent of
  /// CrowdyJS's createMintRediscover), which needs an identity session.
  ///
  /// Must not throw: returning an empty result leaves the client where it is,
  /// on its normal retry.
  graphql::RediscoverFn rediscover;
  /// Consecutive failures before re-discovery runs. A single blip is what
  /// reconnect backoff is for; moving on the first one would relocate clients
  /// on any transient loss.
  int rediscoverAfterFailures =
      graphql::RediscoverCoordinator::kDefaultAfterFailures;
  /// GraphQL path appended to httpUrl, or an explicitly complete custom
  /// endpoint URL. Base URLs already ending in /graphql are not duplicated.
  std::string graphqlEndpoint = "/graphql";
  long timeoutMs = 60000;
  /// Token persistence; in-memory when null.
  std::shared_ptr<graphql::ITokenStore> tokenStore;
  /// HTTP transport; the default libcurl transport when null. Engine
  /// wrappers inject their own here.
  std::shared_ptr<graphql::IHttpTransport> transport;
  /// Crypto provider used by portal PKCE and native replication. The provider
  /// must outlive the client. Defaults to OpenSSL when that provider is built;
  /// otherwise to an explicit unavailable provider whose operations return
  /// Errc::CryptoUnavailable.
  const core::ICrypto* crypto = nullptr;
  /// Optional async HTTP transport for the non-blocking API path. When set,
  /// *Async calls run on it and their callbacks are delivered from poll();
  /// engines inject their own here (FHttpModule, UnityWebRequest, HTTPRequest).
  std::shared_ptr<graphql::IAsyncHttpTransport> asyncTransport;
  /// Routed API WebSocket base URL. CrowdyCPP does not emulate the browser UDP
  /// proxy for gameplay traffic; this normalized endpoint carries subscriptions
  /// and the realtime control stream.
  std::string wsUrl;
  /// Optional explicitly complete WebSocket endpoint (or a path relative to
  /// wsUrl). When empty, wsUrl is normalized to exactly one /graphql.
  std::string wsEndpoint;
  /// WebSocket transport for GraphQL subscriptions. Uses the optional libcurl
  /// implementation when available; engine wrappers inject their own async
  /// socket implementation here.
  std::shared_ptr<graphql::IWebSocketTransport> webSocketTransport;
  /// Acknowledgement, frame, and capped reconnect limits for subscriptions.
  graphql::GraphQLSubscriptionOptions webSocket;
};

/// Ordered stage reached by refreshGameplayToken(). Complete is the only
/// successful terminal stage; every other value identifies where rotation
/// stopped.
enum class GameplayTokenRefreshStage : std::uint8_t {
  Quiesce = 0,
  Refresh,
  Install,
  Reconnect,
  Complete,
};

inline const char* gameplayTokenRefreshStageName(
    GameplayTokenRefreshStage stage) {
  switch (stage) {
    case GameplayTokenRefreshStage::Quiesce: return "Quiesce";
    case GameplayTokenRefreshStage::Refresh: return "Refresh";
    case GameplayTokenRefreshStage::Install: return "Install";
    case GameplayTokenRefreshStage::Reconnect: return "Reconnect";
    case GameplayTokenRefreshStage::Complete: return "Complete";
  }
  return "?";
}

/// Typed result of safe gameplay-token rotation. On a refresh-stage failure
/// the old bearer remains installed. On a reconnect-stage failure
/// tokenInstalled stays true and the fresh bearer is retained so callers can
/// retry Connection::connect() without rotating again.
struct GameplayTokenRefreshResult {
  GameplayTokenRefreshStage stage = GameplayTokenRefreshStage::Quiesce;
  Status status = Errc::Rejected;
  domains::AppTokenResponse token;
  std::string errorCode;
  std::string errorMessage;

  bool hadActiveReplication = false;
  bool tokenInstalled = false;
  bool reconnectAttempted = false;
  bool reconnected = false;
  replication::ConnState previousReplicationState =
      replication::ConnState::Idle;
  replication::Assignment previousReplicationEndpoint;
  replication::Assignment replicationEndpoint;

  bool ok() const {
    return stage == GameplayTokenRefreshStage::Complete && status.ok();
  }
};

using GameplayTokenRefreshCallback =
    std::function<void(GameplayTokenRefreshResult)>;

/// The Crowded Kingdoms client. Domain accessors mirror CrowdyJS sub-clients;
/// replication() is the native-UDP replacement for CrowdyJS's client.udp /
/// client.realtime.
class CrowdyClient {
 public:
  explicit CrowdyClient(ClientConfig config);
  ~CrowdyClient();

  CrowdyClient(const CrowdyClient&) = delete;
  CrowdyClient& operator=(const CrowdyClient&) = delete;
  CrowdyClient(CrowdyClient&&) noexcept;
  CrowdyClient& operator=(CrowdyClient&&) noexcept;

  // ----- Auth state -----------------------------------------------------------
  /// Seed a token directly (identity session token on an identity client, or
  /// an app-scoped token on a per-game client).
  void setToken(const std::string& token) { auth_->setToken(token); }
  std::string getToken() const { return auth_->token(); }
  /// Restore a persisted token from the configured token store.
  bool restoreSession() { return auth_->restore(); }
  graphql::AuthState& authState() { return *auth_; }

  // ----- Game-client surface --------------------------------------------------
  /// Where apps are placed. Ask BEFORE authenticating: the shared origin is a
  /// multivalue record over every datacenter, so a cold client's first request
  /// lands wherever DNS pointed it, and logging in there writes the session in
  /// the wrong datacenter.
  domains::DiscoveryAPI& discovery() { return *discovery_; }
  /// Instance lifecycle events (SERVER_DRAINING and the terminal codes).
  /// watchRealtimeControl() is the usual way in; this is the raw surface.
  domains::RealtimeControlAPI& realtimeControl() { return *realtimeControl_; }
  domains::AuthAPI& auth() { return *authApi_; }
  domains::UsersAPI& users() { return *users_; }
  domains::PortalAPI& portal() { return *portal_; }
  domains::ServerStatusAPI& serverStatus() { return *serverStatus_; }
  domains::ChunksAPI& chunks() { return *chunks_; }
  domains::VoxelsAPI& voxels() { return *voxels_; }
  domains::ActorsAPI& actors() { return *actors_; }
  domains::AvatarsAPI& avatars() { return *avatars_; }
  domains::StateAPI& state() { return *state_; }
  domains::HostAPI& host() { return *host_; }
  domains::TeleportAPI& teleport() { return *teleport_; }
  domains::TeamsAPI& teams() { return *teams_; }
  domains::ChannelsAPI& channels() { return *channels_; }
  domains::GameModelAPI& gameModel() { return *gameModel_; }
#ifndef CROWDY_NO_EXCEPTIONS
  domains::ComputeAPI& compute() { return *compute_; }
#endif
  domains::PlayerComputeAPI& playerCompute() { return *playerCompute_; }
  domains::PlayerWalletAPI& playerWallet() { return *playerWallet_; }
  domains::MarketplaceAPI& marketplace() { return *marketplace_; }
  domains::PlayerModelAPI& playerModel() { return *playerModel_; }
  domains::GameAppsAPI& gameApps() { return *gameApps_; }
#ifndef CROWDY_NO_EXCEPTIONS
  /// Caller-owned, app-scoped Crowdy Studio projects and reusable files.
  /// Source remains owner-private; grid affinity never grants runtime authority.
  domains::CrowdyStudioAPI& crowdyStudio() { return *crowdyStudio_; }
#endif
  domains::PlatformAPI& platform() { return *platform_; }
  /// Durable provider-neutral Agentic Crowdy Studio runtime plus its
  /// Management policy/usage/operator controls.
  domains::CrowdyStudioAgentAPI& crowdyStudioAgent() {
    return *crowdyStudioAgent_;
  }
#ifndef CROWDY_NO_EXCEPTIONS
  /// Own the production typed HTTP + GraphQL-WS transports alongside the
  /// controller, preventing dangling adapter references.
  std::unique_ptr<agent::CrowdyStudioAgentControllerRuntime>
  createCrowdyStudioAgentController(
      agent::CrowdyStudioAgentControllerOptions options);

  /**
   * Builds an independent, lifetime-safe native Studio assembly from cloned
   * typed domain adapters sharing this client's transport/auth dispatcher.
   * The returned integration may outlive CrowdyClient unless config.crypto was
   * externally borrowed; in that case options.crypto must retain it.
   */
  std::unique_ptr<studio::CrowdyStudioIntegration>
  createCrowdyStudioIntegration(
      studio::CrowdyStudioIntegrationOptions options);
#endif

  // ----- Gameplay-token lifecycle ----------------------------------------------
  /// Safely rotate the app-scoped bearer used by GraphQL and native UDP.
  /// Captures the active native connection, handlers, mode, and endpoint;
  /// quiesces it under the old token; refreshes through portal(); installs the
  /// new token while the socket is closed; then reconnects the same Connection
  /// so handlers remain registered exactly once. No browser UDP-proxy
  /// operations are issued.
  GameplayTokenRefreshResult refreshGameplayToken();

  /// Non-throwing async twin. The portal request uses the configured async
  /// transport and the callback is delivered from poll(). Quiesce happens
  /// before the request starts; reconnect uses the preserved native
  /// Connection.
  void refreshGameplayTokenAsync(GameplayTokenRefreshCallback cb);

  // ----- Async API completion --------------------------------------------------
  /// Drain finished async API callbacks on the calling thread. Call once per
  /// tick from the game thread so *Async callbacks fire where engine objects
  /// are safe to touch. No-op unless an async transport routes through it.
  void poll();

  // ----- Native replication ----------------------------------------------------
  /// The native UDP replication client (lazily constructed). Connect with an
  /// app-scoped token held by this client.
  replication::ReplicationClient& replication();

  // ----- Privileged surfaces ---------------------------------------------------
  /// Studio-admin surface (orgs, apps, billing, usage, ...). Drive
  /// with an org/admin token from a trusted context.
  domains::AdminAPI& admin() { return *admin_; }
  /// Operator control-plane surface (requires is_operator).
  domains::OperatorAPI& operator_() { return *operatorApi_; }

  // ----- Low-level escape hatches ------------------------------------------------
  /// Raw GraphQL against the API endpoint.
  graphql::GraphQLClient& graphqlClient() { return *gql_; }
  /// Normalized routed WebSocket endpoint. Empty when neither wsUrl nor
  /// wsEndpoint was configured.
  const std::string& websocketEndpoint() const { return websocketEndpoint_; }
  /// Generic graphql-transport-ws subscriptions against the API.
  graphql::GraphQLSubscriptionClient& subscriptions() {
    return *subscriptions_;
  }

  /// Move every transport to another datacenter: GraphQL endpoint, WebSocket
  /// subscriptions, and a fresh UDP assignment. Wired to WRONG_DATACENTER
  /// automatically; call it directly only when you have your own reason to move.
  ///
  /// Refuses a target outside this client's estate, and returns whether the
  /// move happened. `wsUrl` may be empty, in which case it is derived from
  /// httpUrl. False means nothing changed — the endpoint was already current,
  /// empty, or off-estate.
  bool moveToDatacenter(const std::string& httpUrl,
                        const std::string& wsUrl = {});

  /// Ask where to go and move there. Returns whether the client actually moved.
  ///
  /// Never throws and never leaves the client worse off: with no answer, an
  /// unchanged answer, or a failing lookup, the client stays where it is on its
  /// normal retry. Concurrent calls share one attempt, because several things
  /// notice a dead endpoint at once and each applying its own answer would move
  /// a struggling client repeatedly.
  bool rediscoverEndpoint(const std::string& appId);

  /// Watch the realtime control stream and act on it: SERVER_DRAINING
  /// re-discovers immediately and keeps the connection, terminal codes are
  /// surfaced. `onEvent` still sees every event, so a host can log or display
  /// them; pass nothing if the automatic handling is all you want.
  ///
  /// Requires an app-scoped token. Keep the returned handle alive — destroying
  /// it cancels the subscription.
  graphql::SubscriptionHandle watchRealtimeControl(
      std::function<void(domains::RealtimeConnectionEvent)> onEvent = {});

  /// True when re-discovery is available (a callback was supplied, or
  /// discoveryUrl was set so the client could build one).
  bool canRediscover() const { return rediscover_->hasCallback(); }

  /// The app id of the live replication connection, or empty. Derived rather
  /// than stored so it cannot disagree with what is actually being played.
  std::string activeAppId() const;

  const ClientConfig& config() const { return config_; }

  /// Terminal dispose: cancels queued/in-flight async callback delivery,
  /// closes subscriptions, and disconnects replication. Platform HTTP
  /// requests may still finish, but their retained completions are fenced.
  void close();

 private:
  void ensureNonblockingAsyncTransport();
  /// (Re)bind every callback that captures `this`. Called from construction and
  /// after any move; see the definition for why a move needs it.
  void installSelfHandlers();
  /// Default re-discovery: gameClientBootstrap against discoveryUrl, using the
  /// token this client already holds.
  graphql::RediscoveredEndpoint bootstrapRediscover(const std::string& appId);

  ClientConfig config_;
  const core::ICrypto* crypto_ = nullptr;
  std::shared_ptr<graphql::IHttpTransport> transport_;
  std::shared_ptr<graphql::AuthState> auth_;
  std::shared_ptr<graphql::Dispatcher> dispatcher_;
  std::shared_ptr<graphql::GraphQLClient> gql_;
  /// By pointer because it owns a mutex and CrowdyClient is movable.
  std::shared_ptr<graphql::RediscoverCoordinator> rediscover_ =
      std::make_shared<graphql::RediscoverCoordinator>();
  std::shared_ptr<graphql::IAsyncHttpTransport>
      fallbackAsyncTransport_;
  std::string websocketEndpoint_;
  std::shared_ptr<graphql::IWebSocketTransport> webSocketTransport_;
  std::shared_ptr<graphql::GraphQLSubscriptionClient> subscriptions_;

  std::unique_ptr<domains::DiscoveryAPI> discovery_;
  std::unique_ptr<domains::RealtimeControlAPI> realtimeControl_;
  std::unique_ptr<domains::AuthAPI> authApi_;
  std::unique_ptr<domains::UsersAPI> users_;
  std::unique_ptr<domains::PortalAPI> portal_;
  std::unique_ptr<domains::ServerStatusAPI> serverStatus_;
  std::unique_ptr<domains::ChunksAPI> chunks_;
  std::unique_ptr<domains::VoxelsAPI> voxels_;
  std::unique_ptr<domains::ActorsAPI> actors_;
  std::unique_ptr<domains::AvatarsAPI> avatars_;
  std::unique_ptr<domains::StateAPI> state_;
  std::unique_ptr<domains::HostAPI> host_;
  std::unique_ptr<domains::TeleportAPI> teleport_;
  std::unique_ptr<domains::TeamsAPI> teams_;
  std::unique_ptr<domains::ChannelsAPI> channels_;
  std::unique_ptr<domains::GameModelAPI> gameModel_;
#ifndef CROWDY_NO_EXCEPTIONS
  std::unique_ptr<domains::ComputeAPI> compute_;
#endif
  std::unique_ptr<domains::PlayerComputeAPI> playerCompute_;
  std::unique_ptr<domains::PlayerWalletAPI> playerWallet_;
  std::unique_ptr<domains::MarketplaceAPI> marketplace_;
  std::unique_ptr<domains::PlayerModelAPI> playerModel_;
  std::unique_ptr<domains::GameAppsAPI> gameApps_;
#ifndef CROWDY_NO_EXCEPTIONS
  std::unique_ptr<domains::CrowdyStudioAPI> crowdyStudio_;
#endif
  std::unique_ptr<domains::PlatformAPI> platform_;
  std::unique_ptr<domains::CrowdyStudioAgentAPI> crowdyStudioAgent_;
  std::unique_ptr<domains::AdminAPI> admin_;
  std::unique_ptr<domains::OperatorAPI> operatorApi_;
  std::unique_ptr<replication::ReplicationClient> replication_;
};

}  // namespace crowdy
