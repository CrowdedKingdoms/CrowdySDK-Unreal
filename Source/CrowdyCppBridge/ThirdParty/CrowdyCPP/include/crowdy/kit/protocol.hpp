#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "crowdy/domains/exec.hpp"
#include "crowdy/graphql/json.hpp"

/// The game kit protocol: what kit hubs (ck-exec's starter packs, built on `ckx_sdk::kit`)
/// send the kit's clients, read without I/O. CrowdyJS `src/kit/protocol.ts`.
///
/// A kit hub's topics carry kit messages: MessagePack maps with the hub's data and four reserved
/// fields beside it: `v` (kKitProtocol), `epoch` (the publishing instance's epoch, which changes
/// whenever it is placed again), `seq` (one more than the topic's last message) and `kind`:
///
/// - `snapshot`: the topic's whole public state; it replaces what the client holds.
/// - `delta`: one change to a collection (`id`, then `type_name` and `props`, or `removed`),
///   applied in `seq` order. A gap in `seq`, or another `epoch`, means a resync.
/// - `changed`: something private changed for the players in `who` (`what` names it); they read
///   it back with a call the hub answers for its caller alone. Topics have no access control,
///   so kit hubs never publish anything private.
/// - `event`: something that happened, with nothing to keep.
///
/// Every kit hub answers `snapshot { topic }` with a `snapshot` message whose `seq` is the
/// topic's last. A call that changes something may carry an `action_id`; the hub answers a call
/// sent again with the first answer. A refusal's message is `kit:<code>: <text>`.
namespace crowdy::kit {

/// The protocol version kit messages carry in `v`.
inline constexpr int kKitProtocol = 1;
/// The method every kit hub answers with a topic's snapshot.
inline constexpr std::string_view kKitSnapshotMethod = "snapshot";
/// The longest `action_id` a hub takes, in bytes.
inline constexpr std::size_t kKitActionIdMax = 64;
/// Subscriptions one connection holds at most (the gateway refuses the next).
inline constexpr std::size_t kKitSubscriptionsMax = 256;

enum class KitMessageKind { Snapshot, Delta, Changed, Event };

std::string_view kitMessageKindName(KitMessageKind kind) noexcept;

/// The reserved fields of a kit message.
struct KitHeader {
  int v = 0;
  std::int64_t epoch = 0;
  std::int64_t seq = 0;
  KitMessageKind kind = KitMessageKind::Snapshot;
};

/// A kit message split into its header and its body (every other field).
struct KitMessage {
  KitHeader header;
  graphql::Json body;
};

/// The header of `value` when it is a kit message: a map with the four reserved fields.
std::optional<KitHeader> kitHeader(const graphql::Json& value);
inline bool isKitMessage(const graphql::Json& value) { return kitHeader(value).has_value(); }
/// `value` split into header and body, or nullopt when it is not a kit message.
std::optional<KitMessage> splitKitMessage(const graphql::Json& value);

/// The body of a `changed` message.
struct KitChanged {
  /// The players who should read again, as decimal text.
  std::vector<std::string> who;
  /// What changed for them, as the hub's endpoints name it.
  std::string what;
};
std::optional<KitChanged> kitChanged(const graphql::Json& body);

/// One member of a collection, as a `delta` or a collection snapshot carries it.
struct KitContainer {
  std::int64_t id = 0;
  std::string typeName;
  /// Its public properties, all of them: a delta replaces what the client held.
  graphql::Json props;
};

// ---- refusals ----

struct KitRefusal {
  std::string code;
  std::string text;
};

/// The code and text of a kit refusal's message, or nullopt for any other message.
std::optional<KitRefusal> parseKitRefusal(std::string_view message);

/// The refusal codes kit hubs share; a hub may add its own (lowercase, digits, `_`).
namespace codes {
inline constexpr std::string_view kBadRequest = "bad_request";
inline constexpr std::string_view kNotFound = "not_found";
inline constexpr std::string_view kForbidden = "forbidden";
inline constexpr std::string_view kConflict = "conflict";
inline constexpr std::string_view kFull = "full";
inline constexpr std::string_view kLimit = "limit";
inline constexpr std::string_view kCooldown = "cooldown";
inline constexpr std::string_view kNotYourTurn = "not_your_turn";
inline constexpr std::string_view kNotMember = "not_member";
inline constexpr std::string_view kStarted = "started";
inline constexpr std::string_view kFinished = "finished";
inline constexpr std::string_view kNotReady = "not_ready";
inline constexpr std::string_view kReserved = "reserved";
inline constexpr std::string_view kEmpty = "empty";
inline constexpr std::string_view kOutOfReach = "out_of_reach";
inline constexpr std::string_view kExpired = "expired";
inline constexpr std::string_view kInsufficient = "insufficient";
inline constexpr std::string_view kAlreadyDone = "already_done";
/// A hub the call needed did not answer: the kit sends a call with an action id again.
inline constexpr std::string_view kUnavailable = "unavailable";
}  // namespace codes

/// A kit call's answer: the exec reply, with a hub's coded refusal read out. A refusal is
/// `AppError` with a `kit:<code>:` message; `code()` is what to branch on, `refusal->text` what
/// to show. CrowdyJS `KitError`.
struct KitReply {
  domains::ExecStatus status = domains::ExecStatus::Unavailable;
  /// MessagePack for Ok; the refusal's message otherwise.
  std::string payload;
  std::optional<KitRefusal> refusal;

  bool ok() const { return status == domains::ExecStatus::Ok; }
  /// The hub's refusal code, or "" when the call ran or the platform refused it.
  std::string code() const { return refusal ? refusal->code : std::string(); }
  graphql::Json value() const { return graphql::Json::fromMsgpack(payload); }
  std::string message() const { return ok() ? std::string() : payload; }

  static KitReply from(domains::ExecReply reply);
  /// A refusal the kit makes itself, before anything is sent (a subscription past the cap).
  static KitReply refused(std::string_view code, std::string_view text);
};

using KitCallback = std::function<void(KitReply)>;

// ---- action ids ----

/// A fresh `action_id`: unique per attempt, the same for every resend of that attempt.
std::string newActionId();
/// `args` with `action_id` beside them: an object gains the field, null becomes
/// `{ action_id }`. Anything else cannot carry one and is returned as it is.
graphql::JVal withActionId(graphql::JVal args, std::string_view actionId);

// ---- shard keys ----

/// The centre chunk of the shard holding chunk `(x, z)`: shards are `size` by `size` chunks,
/// keyed by their centre. The same function as `ckx_sdk::kit::shard`, so a client finds the hub
/// for where it stands without asking.
std::pair<std::int64_t, std::int64_t> shardCentre(std::int64_t x, std::int64_t z, std::int64_t size);
/// The key of the shard holding chunk `(x, z)`: `"<x>,<z>"` of its centre.
std::string shardKey(std::int64_t x, std::int64_t z, std::int64_t size);
/// The centre chunk a shard key names, or nullopt when it is not `"<x>,<z>"`.
std::optional<std::pair<std::int64_t, std::int64_t>> parseShardKey(std::string_view key);
/// The keys of the shards within `radius` shards of the one holding `(x, z)`, its own first.
std::vector<std::string> shardKeysAround(std::int64_t x, std::int64_t z, std::int64_t size, int radius);

}  // namespace crowdy::kit
