#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "crowdy/core/uuid.hpp"
#include "crowdy/kit/mirror.hpp"
#include "crowdy/kit/runtime.hpp"
#include "crowdy/kit/wire.hpp"
#include "crowdy/wire/codec.hpp"

namespace crowdy::replication {
class Connection;
}

/// Typed clients for the ck-exec starter hubs, and the kit's client-side conventions.
/// CrowdyJS `src/kit/modules/`. Every reply is a KitReply: `value()` is the hub's answer
/// (snake_case fields, as the hub sends them), `code()` a hub's refusal.
namespace crowdy::kit {

// ---- the world ----

/// The root world hub (the `world-tick` starter): its clock, weather and liveops modifiers as
/// the mirrored conditions() topic, its resource nodes as the mirrored nodes() collection,
/// harvesting, and for developers the liveops schedule. The root hub takes 50 calls a second
/// for the whole app, so read it from the mirrors, not by polling state().
class WorldKit {
 public:
  WorldKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType = "world", std::string key = "");

  /// The `world` topic: `{ time_of_day, night, weather, modifiers }`.
  HubState& conditions() { return conditions_; }
  /// The `nodes` topic: each resource node's `amount` and `regrow_at`, by node id.
  CollectionMirror& nodes() { return nodes_; }

  /// Everything the world shows: `{ conditions, clock_ms, nodes, events, online }`.
  void state(KitCallback done);
  /// Takes one from resource node `node`; the reply is what it holds now. Refusals:
  /// `cooldown`, `empty` (it grows back), `not_found`.
  void harvest(std::int64_t node, KitCallback done);
  /// Schedules (or replaces) a liveops event `{ name, start_ms, end_ms, modifiers }`.
  /// Developers only (a developer kit).
  void schedule(const graphql::JVal& event, KitCallback done);
  /// Cancels a liveops event; the reply is whether there was one. Developers only.
  void cancel(std::string name, KitCallback done);

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_, key_;
  HubState conditions_;
  CollectionMirror nodes_;
};

// ---- matchmaking ----

/// Joining a queue: alone, as a party's leader (`party`: the others), or confirming (`leader`).
struct MatchJoin {
  std::int64_t rating = 0;
  std::vector<std::int64_t> party;
  std::optional<std::int64_t> leader;
};

/// One queue. Every call's answer updates place(); watch() keeps it current while the player
/// waits (the queue never names its players on its topic, so a waiting client asks) and fires
/// onMatched() with the session key once a match is made.
class MatchQueue {
 public:
  MatchQueue(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string name);
  ~MatchQueue();
  MatchQueue(const MatchQueue&) = delete;
  MatchQueue& operator=(const MatchQueue&) = delete;

  /// The `queue` topic: `{ waiting, proposals, forming }`, how many and never who.
  HubState& counts() { return counts_; }
  /// The player's place, as last answered: `{ state: "idle" }`, `party`, `queued`, `proposed`
  /// or `{ state: "matched", session }`.
  graphql::Json place() const;
  /// Called with every new place; removeListener() stops it.
  std::uint64_t onPlace(std::function<void(const graphql::Json&)> listener);

  /// Joins the queue (or updates the rating). Refusals: `conflict` (a match to accept),
  /// `limit` (party size), `forbidden`, `full`.
  void join(const MatchJoin& request, KitCallback done);
  /// Leaves the queue, a forming party, or a proposed match.
  void leave(KitCallback done);
  /// Accepts the proposed match; the answer is `matched` once everyone has.
  void accept(KitCallback done);
  /// Declines the proposed match; the others go back to the queue.
  void decline(KitCallback done);
  void status(KitCallback done);
  /// Sets the queue's config. Developers only.
  void configure(const graphql::JVal& config, KitCallback done);

  /// Asks for the player's place every `intervalMs` (at least 500) while they wait in a party,
  /// the queue or a proposal.
  void watch(long intervalMs = 2000);
  void unwatch();
  /// Called with the session key when a match is made for the player.
  std::uint64_t onMatched(std::function<void(const std::string& session)> listener);
  void removeListener(std::uint64_t id);

  const std::string& name() const { return name_; }

 private:
  struct Core;
  std::shared_ptr<Core> core_;
  std::string name_;
  HubState counts_;
};

/// Matchmaking queues (the `matchmaker` starter): queue() by name.
class MatchmakingKit {
 public:
  MatchmakingKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType = "matchmaker");
  /// The queue named `name` (`ranked`, `casual`, ...), one per name.
  MatchQueue& queue(const std::string& name);

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_;
  std::mutex mu_;
  std::map<std::string, std::unique_ptr<MatchQueue>> queues_;
};

// ---- sessions ----

/// Whose turn it is in a session's state while it plays, or nullopt.
std::optional<std::int64_t> sessionTurn(const graphql::Json& state);

/// One session from lobby to result: join, ready up, the host starts, players act in turn
/// under a time limit, and the hub keeps the score and names the winner. state() mirrors the
/// `session` topic; every change is a snapshot on it.
class GameSessionHandle {
 public:
  GameSessionHandle(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key);

  /// The `session` topic: `{ config, phase, reserved, players, ready, connected, host, turn,
  /// round, scores, winner, moves }`.
  HubState& state() { return state_; }
  void watch(KitCallback done = {}) { state_.start(std::move(done)); }
  void unwatch() { state_.stop(); }
  std::optional<std::int64_t> currentTurn() const { return sessionTurn(state_.get()); }
  /// Whether it is the player's turn, as the mirror last saw it.
  bool isMyTurn() const;

  /// Refusals: `started`, `reserved`, `full`.
  void join(KitCallback done);
  void leave(KitCallback done);
  /// Refusal: `not_member` (join the lobby first).
  void ready(bool ready, KitCallback done);
  /// Starts the session (the host, once everyone is ready). Refusals: `forbidden`,
  /// `not_ready`, `too_few_players`, `started`.
  void startGame(KitCallback done);
  /// Plays the player's move (the starter's is `{ points }`, 0 to 3), with an action id: a move
  /// resent after a lost reply is played once. Refusals: `not_your_turn`, `invalid_move`.
  void act(const graphql::JVal& action, KitCallback done);
  /// Changes the config: the host in the lobby, or a developer.
  void configure(const graphql::JVal& config, KitCallback done);

  const std::string& key() const { return key_; }

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_, key_;
  HubState state_;
};

/// Game sessions (the `session` starter): open() one by its key.
class SessionsKit {
 public:
  SessionsKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType = "session");
  /// The session with key `key` (a matchmaker's `matched.session`, or one a game names).
  GameSessionHandle& open(const std::string& key);

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_;
  std::mutex mu_;
  std::map<std::string, std::unique_ptr<GameSessionHandle>> sessions_;
};

// ---- areas of mobs, NPCs and pets ----

/// One area: its mobs, NPCs and pets mirrored from the `area` topic, hits on mobs (the hub
/// decides reach, cooldown and damage), NPC talk, and the player's pets. The hub also shows its
/// actors through the realtime tier (decodeKitActorState).
class NpcArea {
 public:
  NpcArea(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key);

  /// The `area` topic: every member's public properties, by id.
  CollectionMirror& members() { return members_; }
  void watch(KitCallback done = {}) { members_.start(std::move(done)); }
  void unwatch() { members_.stop(); }
  std::vector<KitContainer> mobs() const { return members_.list("Mob"); }
  std::vector<KitContainer> npcs() const { return members_.list("Npc"); }
  std::vector<KitContainer> pets() const { return members_.list("Pet"); }

  /// Hits mob `mob`; the reply is its hp after the hit. Refusals: `not_in_area`,
  /// `out_of_reach`, `cooldown`, `down`, `not_found`.
  void attack(std::int64_t mob, KitCallback done);
  /// What NPC `npc` says, with its public properties.
  void talk(std::int64_t npc, KitCallback done);
  /// Adopts a pet where the player stands; the reply is its id. Refusals: `not_in_area`,
  /// `limit`, `full`, `bad_request`.
  void adopt(std::string species, std::string name, KitCallback done);
  void rename(std::int64_t pet, std::string name, KitCallback done);
  /// The pet follows its owner (and its bond grows).
  void summon(std::int64_t pet, KitCallback done);
  /// The pet stays where it is.
  void dismiss(std::int64_t pet, KitCallback done);
  void release(std::int64_t pet, KitCallback done);
  /// The player's pets in this area, with their bond: `[{ id, species, name, x, z, mode, bond }]`.
  void myPets(KitCallback done);

  const std::string& key() const { return key_; }

 private:
  void pet(const char* method, std::int64_t pet, std::optional<std::string> name, KitCallback done);

  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_, key_;
  CollectionMirror members_;
};

/// The world's areas of mobs, NPCs and pets (the `npc-mobs` starter): one hub per area, keyed
/// by its centre chunk, so a client finds the hub for where it stands (areaAt) without asking.
class NpcsKit {
 public:
  NpcsKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType = "npcs", std::int64_t areaSize = 8);
  /// The key of the area holding chunk `(chunkX, chunkZ)`.
  std::string keyAt(std::int64_t chunkX, std::int64_t chunkZ) const;
  NpcArea& areaAt(std::int64_t chunkX, std::int64_t chunkZ) { return area(keyAt(chunkX, chunkZ)); }
  /// The area with key `key` (`"<x>,<z>"`, its centre chunk).
  NpcArea& area(const std::string& key);

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_;
  std::int64_t size_;
  std::mutex mu_;
  std::map<std::string, std::unique_ptr<NpcArea>> areas_;
};

// ---- client conventions ----

/// Shard keys: the hub for where a player stands, computed the way the hubs compute it.
class ShardKit {
 public:
  std::string key(std::int64_t x, std::int64_t z, std::int64_t size) const { return shardKey(x, z, size); }
  std::vector<std::string> around(std::int64_t x, std::int64_t z, std::int64_t size, int radius = 1) const {
    return shardKeysAround(x, z, size, radius);
  }
};

/// How a spectator stays where the world's messages reach them.
struct SpectateOptions {
  /// The spectator's actor uuid (32 characters).
  core::ActorUuid uuid{};
  /// Where to watch from: a chunk inside the app's world grid.
  wire::ChunkCoord chunk;
  /// How often to say so, in milliseconds (at least 250).
  long intervalMs = 2000;
  /// The actor state to send; empty registers without a body.
  std::vector<std::uint8_t> state;
  /// Replication distance, 0-8.
  std::uint8_t distance = 8;
};

/// A running spectator heartbeat: an actor update every interval, through the kit's timer.
class SpectatorHeartbeat {
 public:
  SpectatorHeartbeat(std::shared_ptr<KitRuntime> runtime, replication::Connection& connection, SpectateOptions options);
  ~SpectatorHeartbeat();
  SpectatorHeartbeat(const SpectatorHeartbeat&) = delete;
  SpectatorHeartbeat& operator=(const SpectatorHeartbeat&) = delete;

  /// Watches from another chunk from now on (sent at once).
  void moveTo(wire::ChunkCoord chunk);
  /// The last send's error, or Errc::Ok when it went out.
  Errc error() const;
  void stop();

 private:
  struct Core;
  std::shared_ptr<Core> core_;
};

/// Spectating: the realtime tier delivers what happens near an actor only while that actor is
/// live, so a viewer with no avatar of its own (a spectator, a kill-cam, a second screen) sends
/// a small actor update every couple of seconds from a chunk inside the app's world grid.
class SpectateKit {
 public:
  SpectateKit(std::shared_ptr<KitRuntime> runtime, replication::Connection* connection)
      : runtime_(std::move(runtime)), connection_(connection) {}
  /// Starts the heartbeat; null without a replication connection.
  std::unique_ptr<SpectatorHeartbeat> observe(SpectateOptions options);

 private:
  std::shared_ptr<KitRuntime> runtime_;
  replication::Connection* connection_;
};

/// One emote to send.
struct EmoteSend {
  /// The sending actor's uuid.
  core::ActorUuid uuid{};
  /// The chunk the sender stands in.
  wire::ChunkCoord chunk;
  /// The emote's name, as the game knows it: `wave`, `cheer`, ...
  std::string emote;
  /// The actor it is aimed at, if any.
  std::optional<std::string> targetUuid;
  /// Replication distance, 0-8.
  std::uint8_t distance = 2;
};

/// Emotes: a client event (type kEventEmote) with a JSON body `{ emote, targetUuid }`,
/// replicated to the players near the sender. A kit convention, so players on CrowdyJS,
/// CrowdyCPP and CrowdyPy see each other's; no hub is involved.
class EmotesKit {
 public:
  explicit EmotesKit(replication::Connection* connection) : connection_(connection) {}
  /// Sends an emote; Errc::Ok when it was accepted for sending, NotConnected without a connection.
  Errc send(const EmoteSend& emote);
  /// An emote from a client event's type and state, or nullopt when it is another event.
  std::optional<EmoteEvent> parse(std::uint16_t eventType, const std::uint8_t* state, std::size_t len) const;

 private:
  replication::Connection* connection_;
};

}  // namespace crowdy::kit
