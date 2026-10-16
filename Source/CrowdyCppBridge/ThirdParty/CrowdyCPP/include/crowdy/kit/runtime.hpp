#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "crowdy/domains/exec.hpp"
#include "crowdy/graphql/dispatcher.hpp"
#include "crowdy/kit/protocol.hpp"

/// The kit's way to the app's hubs. CrowdyJS `src/kit/runtime.ts`.
namespace crowdy::kit {

/// How a KitRuntime reaches the app's hubs.
struct KitRuntimeOptions {
  /// Use this connection rather than opening one. The kit does not close it.
  std::shared_ptr<domains::ExecConnection> connection;
  /// How the kit opens its connection: affinity (`nodeType`, `key`), timeouts.
  domains::ExecConnectOptions connect;
  /// Connect as one of the app's developers (`ExecAPI::connectAsDeveloper`, the org's
  /// `manage_compute`, an identity session rather than an app token): what admin endpoints
  /// (`configure`, `schedule`, ...) need.
  bool developer = false;
  /// Calls, subscriptions and unsubscriptions the kit sends in one window at most. The gateway
  /// takes 120 per 10 s from a player and refuses the rest `Busy`; 100 keeps the kit under it
  /// with room for the game's own calls.
  int callBudget = 100;
  long budgetWindowMs = 10000;
  /// Times a call is sent again when it did not run (`Busy`, `RateLimited`), or, when it
  /// carries an action id, after an unknown outcome (`DeadlineExceeded`, `Unavailable`,
  /// `Moved`).
  int retries = 2;
  /// The player's id (decimal), to match `changed` messages.
  std::string playerId;
};

/// Per call: the action id, and the reply timeout.
struct KitCallOptions {
  /// Send a fresh `action_id`, so a resend is answered once.
  bool action = false;
  /// Or send this one (a deliberate retry of an earlier attempt).
  std::string actionId;
  /// How long to wait for the reply, in milliseconds; 0 for the connection's default.
  long timeoutMs = 0;
};

/// One ExecConnection shared by every kit module, opened when first needed. It paces what it
/// sends to a budget below the gateway's limit, sends a call again when that is safe (it did not
/// run, or it carries an action id the hub answers once), counts subscriptions under the
/// gateway's cap, and reads a hub's coded refusal into the reply. Callbacks run through the
/// dispatcher (CrowdyClient::poll()); the kit's own waits (pacing, resends, a queue's watch)
/// run on one timer thread of its own and are delivered the same way. Thread-safe.
class KitRuntime {
 public:
  KitRuntime(std::string appId, domains::ExecAPI* exec, std::shared_ptr<graphql::Dispatcher> dispatcher,
             KitRuntimeOptions options = {});
  ~KitRuntime();

  KitRuntime(const KitRuntime&) = delete;
  KitRuntime& operator=(const KitRuntime&) = delete;

  const std::string& appId() const;
  /// The connection, opened on first use; null without an exec client or once access ended.
  std::shared_ptr<domains::ExecConnection> connection();
  /// The player's id from the options or setPlayerId; empty when unknown.
  std::string playerId() const;
  void setPlayerId(std::string playerId);

  /// Calls a hub's endpoint with `args` as MessagePack. `done` fires once, through the
  /// dispatcher, with the reply: `ok()`, a hub's refusal (`code()`), or the platform's status.
  void call(std::string nodeType, std::string key, std::string method, graphql::JVal args, KitCallback done,
            KitCallOptions options = {});

  /// Receives what a hub publishes on `topic`. Returns the handle for unsubscribe(), or 0 when
  /// the connection holds kKitSubscriptionsMax topics already (`done` then has code `limit`).
  /// `done` fires with the gateway's answer to the subscription.
  std::uint64_t subscribe(std::string nodeType, std::string key, std::string topic, domains::ExecPushHandler onPush,
                          KitCallback done = {});
  /// Stops one handler, and the subscription with the last one.
  void unsubscribe(std::uint64_t handle);
  /// How many distinct topics the kit subscribes to.
  std::size_t subscriptions() const;

  /// Called with the new host after the connection reconnects; kit mirrors resync then.
  std::uint64_t onReconnect(std::function<void(std::string host)> listener);
  /// Called once when the player's access ended (close code 4403); nothing reconnects.
  std::uint64_t onAccessEnded(std::function<void(std::string reason)> listener);
  void removeListener(std::uint64_t id);

  /// Runs `fn` through the dispatcher `delayMs` from now, on the kit's timer. Returns its id.
  std::uint64_t after(long delayMs, std::function<void()> fn);
  void cancel(std::uint64_t timer);

  /// Closes the connection the kit opened (not one it was given) and stops its timer.
  void close();

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace crowdy::kit
