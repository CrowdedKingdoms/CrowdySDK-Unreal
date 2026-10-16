#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "crowdy/kit/mirror.hpp"
#include "crowdy/kit/runtime.hpp"

/// Typed clients for the ck-exec genre starters. CrowdyJS `src/kit/modules/` (leaderboards,
/// tables, tactics, encounters, minigames, instances, arenas, racing, territory). Every reply is
/// a KitReply: `value()` is the hub's answer (snake_case fields, as the hub sends them), `code()`
/// a hub's refusal. Each handle mirrors its hub's topic (view()) and sends every change with an
/// action id, so a change resent after a lost reply is answered once; a reported position is not
/// (the next report replaces it).
namespace crowdy::kit {

// ---- leaderboards ----

/// A page of a board: from `offset`, at most `limit` (the board's top size by default).
struct LeaderboardPage {
  std::optional<std::int64_t> limit;
  std::optional<std::int64_t> offset;
};

/// The places around `player` (the caller, without one), `radius` either side.
struct LeaderboardAround {
  std::optional<std::int64_t> player;
  std::optional<std::int64_t> radius;
};

/// One board: hubs submit the scores they decided (a client never submits its own), seasons roll
/// on the board's schedule.
class LeaderboardHandle {
 public:
  LeaderboardHandle(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string name);

  /// The `board` topic: `{ season, season_started, entries, top }`.
  HubState& view() { return view_; }
  void watch(KitCallback done = {}) { view_.start(std::move(done)); }
  void unwatch() { view_.stop(); }

  /// `[{ rank, player, score }]`, best first.
  void top(KitCallback done, const LeaderboardPage& page = {});
  void around(KitCallback done, const LeaderboardAround& request = {});
  /// The caller's own place, or null without a score this season.
  void me(KitCallback done);
  /// `{ season, started, entries, last }`.
  void season(KitCallback done);
  /// Submits a score for `player`; the reply is the score kept. Developers only (and hubs).
  void submit(std::int64_t player, std::int64_t score, KitCallback done);
  /// Sets the board's config `{ keep, order, top_size, season_cron, max_entries }`. Developers only.
  void configure(const graphql::JVal& config, KitCallback done);
  /// Ends the season now; the reply is the new season. Developers only.
  void roll(KitCallback done);

  const std::string& name() const { return name_; }

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_, name_;
  HubState view_;
};

/// Leaderboards (the `leaderboard` starter): board() by name.
class LeaderboardsKit {
 public:
  LeaderboardsKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType = "leaderboard");
  LeaderboardHandle& board(const std::string& name);

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_;
  std::mutex mu_;
  std::map<std::string, std::unique_ptr<LeaderboardHandle>> boards_;
};

// ---- card tables ----

/// One card table: the public table on the `table` topic, the caller's hand as private state
/// (read again when the hub says it changed for them), turns in the hub's order.
class CardTable {
 public:
  CardTable(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key);

  /// `{ phase, players, host, hand_counts, top, suit, draw_count, turn, turn_ends, last, winner, scores }`.
  HubState& table() { return table_; }
  /// `{ cards, playable, drawn }`, never on the topic.
  PrivateState& hand() { return hand_; }
  /// Mirrors the table and the caller's hand; `done` fires once both have their first value.
  void watch(KitCallback done = {});
  void unwatch();

  /// Refusals: `started`, `full`.
  void join(KitCallback done);
  void leave(KitCallback done);
  /// Deals (the host, from two players). Refusals: `forbidden`, `too_few_players`, `started`.
  void start(KitCallback done);
  /// Plays a card from the hand; an eight names `suit` (`S`, `H`, `D` or `C`). Refusals:
  /// `not_your_turn`, `not_found`, `not_playable`, `bad_request`.
  void play(std::string card, std::optional<std::string> suit, KitCallback done);
  /// Draws a card (once a turn). Refusals: `not_your_turn`, `conflict`, `empty`.
  void draw(KitCallback done);
  /// Passes, after drawing. Refusal: `conflict`.
  void pass(KitCallback done);

  const std::string& key() const { return key_; }

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_, key_;
  HubState table_;
  PrivateState hand_;
};

/// Card tables (the `deck` starter): table() by key.
class TablesKit {
 public:
  TablesKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType = "deck");
  CardTable& table(const std::string& key);

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_;
  std::mutex mu_;
  std::map<std::string, std::unique_ptr<CardTable>> tables_;
};

// ---- tactics ----

/// One grid battle in phases: in the players' phase each moves and strikes with their unit, and
/// the hub's units act once every player has ended the phase.
class TacticsBattle {
 public:
  TacticsBattle(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key);

  /// The `battle` topic: `{ phase, round, width, height, obstacles, units, players, host,
  /// ended_turn, phase_ends, log }`.
  HubState& battle() { return battle_; }
  void watch(KitCallback done = {}) { battle_.start(std::move(done)); }
  void unwatch() { battle_.stop(); }
  /// The player's units, as the mirror last saw them.
  std::vector<graphql::Json> myUnits() const;

  /// Refusals: `started`, `full`.
  void join(KitCallback done);
  /// Refusals: `forbidden` (the host starts it), `started`.
  void start(KitCallback done);
  /// Refusals: `not_your_turn`, `conflict` (it moved), `out_of_reach`, `not_member`.
  void move(std::int64_t x, std::int64_t z, KitCallback done);
  /// Refusals: `not_your_turn`, `conflict`, `out_of_reach`, `forbidden` (its own side), `not_found`.
  void attack(std::int64_t target, KitCallback done);
  void endTurn(KitCallback done);

  const std::string& key() const { return key_; }

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_, key_;
  HubState battle_;
};

/// Grid battles (the `tactics` starter): battle() by key.
class TacticsKit {
 public:
  TacticsKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType = "tactics");
  TacticsBattle& battle(const std::string& key);

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_;
  std::mutex mu_;
  std::map<std::string, std::unique_ptr<TacticsBattle>> battles_;
};

// ---- encounters ----

/// One encounter: waves down a lane on the hub's clock, towers that may not cut the lane, a
/// boss with phases. view()'s events (`wave_started`, `wave_cleared`, `boss_phase`, `won`,
/// `lost`) come through `view().onEvent`.
class Encounter {
 public:
  Encounter(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key);

  /// The `encounter` topic: `{ phase, players, width, height, spawn, base, base_hp, gold, wave,
  /// waves, next_wave_at, enemies, towers, boss_phase, kills }`.
  HubState& view() { return view_; }
  void watch(KitCallback done = {}) { view_.start(std::move(done)); }
  void unwatch() { view_.stop(); }

  /// Refusal: `full`.
  void join(KitCallback done);
  /// The first to join, or a developer. Refusals: `forbidden`, `started`, `bad_request`.
  void start(KitCallback done);
  /// Builds a tower at `(x, z)`; the reply is the gold left. Refusals: `blocks_lane`,
  /// `insufficient`, `conflict`, `bad_request`, `not_member`.
  void build(std::int64_t x, std::int64_t z, KitCallback done);
  /// Strikes enemy `enemy`; the reply is its hp left. Refusals: `cooldown`, `not_found`, `not_member`.
  void hit(std::int64_t enemy, KitCallback done);

  const std::string& key() const { return key_; }

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_, key_;
  HubState view_;
};

/// Waves and bosses (the `director` starter): encounter() by key.
class EncountersKit {
 public:
  EncountersKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType = "director");
  Encounter& encounter(const std::string& key);

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_;
  std::mutex mu_;
  std::map<std::string, std::unique_ptr<Encounter>> encounters_;
};

// ---- minigames ----

/// One duel of hidden throws: the hub keeps each round's throws until both are in (or the round's
/// time runs out), then reveals them; a majority of `best_of` rounds wins.
class Duel {
 public:
  Duel(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key);

  /// The `duel` topic: `{ phase, seats, best_of, round, deadline, last, winner }`.
  HubState& view() { return view_; }
  void watch(KitCallback done = {}) { view_.start(std::move(done)); }
  void unwatch() { view_.stop(); }

  /// Takes a seat; the reply is whether it started the first round. Refusal: `full`.
  void join(KitCallback done);
  /// Before the duel starts. Refusal: `started`.
  void leave(KitCallback done);
  /// The caller's hidden throw this round (CrowdyJS `throw`); the reply is whether it completed
  /// the round. Refusals: `conflict`, `bad_request`, `not_member`.
  void throwHand(std::string hand, KitCallback done);
  /// Once it is over: play again. Refusals: `conflict`, `not_member`.
  void rematch(KitCallback done);

  const std::string& key() const { return key_; }

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_, key_;
  HubState view_;
};

/// Duels (the `minigame` starter): duel() by key.
class MinigamesKit {
 public:
  MinigamesKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType = "minigame");
  Duel& duel(const std::string& key);

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_;
  std::mutex mu_;
  std::map<std::string, std::unique_ptr<Duel>> duels_;
};

// ---- instances ----

/// Opening a run: its seed (the hub picks one without), and for a developer the party.
struct InstanceOpen {
  std::optional<std::int64_t> seed;
  std::vector<std::int64_t> party;
};

/// One private run: a party plays down floors the hub generates from the run's seed, a party
/// with nobody standing is wiped, a run nobody touches expires, and a cleared or wiped run posts
/// each member's depth to the leaderboard its config names.
class InstanceRun {
 public:
  InstanceRun(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key);

  /// The `run` topic: `{ phase, owner, seed, floor, floors, room, rooms, members, invited, gold,
  /// relics, grid, log }`.
  HubState& view() { return view_; }
  void watch(KitCallback done = {}) { view_.start(std::move(done)); }
  void unwatch() { view_.stop(); }

  /// The reply is the run's seed. Refusals: `started`, `bad_request`, `conflict`.
  void open(const InstanceOpen& request, KitCallback done);
  /// The owner invites a player. Refusals: `forbidden`, `full`.
  void invite(std::int64_t player, KitCallback done);
  /// An invited player joins. Refusal: `forbidden`.
  void join(KitCallback done);
  /// Resolves the room the party is at; the reply is the log's line for it. Refusals:
  /// `not_member`, `forbidden` (down), `conflict` (descend first), `finished`.
  void enter(KitCallback done);
  /// The owner leads the party down; the reply is the new floor. Refusals: `not_ready`, `forbidden`.
  void descend(KitCallback done);
  void abandon(KitCallback done);

  const std::string& key() const { return key_; }

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_, key_;
  HubState view_;
};

/// Private runs (the `instance` starter): run() by key.
class InstancesKit {
 public:
  InstancesKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType = "instance");
  InstanceRun& run(const std::string& key);

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_;
  std::mutex mu_;
  std::map<std::string, std::unique_ptr<InstanceRun>> runs_;
};

// ---- arenas ----

/// One real-time arena: players fight with their class's abilities and the hub referees every
/// hit. A client reports where its player is (move(), a few times a second) and the hub keeps
/// what the class's speed allows. Events (`cast`, `impact`, `kill`, `respawn`, `violation`,
/// `removed`, `finished`) come through `view().onEvent`.
class ArenaMatch {
 public:
  ArenaMatch(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key);

  /// The `arena` topic: `{ phase, width, depth, fighters, projectiles, areas, kill_target, ends_at, winner }`.
  HubState& view() { return view_; }
  void watch(KitCallback done = {}) { view_.start(std::move(done)); }
  void unwatch() { view_.stop(); }

  /// Joins as `heroClass` (the starter's are `warrior` and `mage`). Refusals: `bad_request`, `full`, `started`.
  void join(std::string heroClass, KitCallback done);
  void leave(KitCallback done);
  /// The first to join, or a developer. Refusals: `forbidden`, `not_ready`, `started`.
  void start(KitCallback done);
  /// Reports where the player is; the reply is `[x, z]`, the position the hub kept. Refusals:
  /// `forbidden` (removed by the warden), `conflict` (down).
  void move(double x, double z, KitCallback done);
  /// Casts `ability` at `(x, z)`; the reply is whether a hit-scan strike hit at once. Refusals:
  /// `cooldown`, `insufficient`, `out_of_reach`, `not_found`.
  void cast(std::string ability, double x, double z, KitCallback done);

  const std::string& key() const { return key_; }

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_, key_;
  HubState view_;
};

/// Real-time arenas (the `arena` starter): arena() by key.
class ArenasKit {
 public:
  ArenasKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType = "arena");
  ArenaMatch& arena(const std::string& key);

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_;
  std::mutex mu_;
  std::map<std::string, std::unique_ptr<ArenaMatch>> arenas_;
};

// ---- racing ----

/// One race: gates in order, a countdown, laps and splits, places, and the winner's ghost.
/// Events: `go`, `timing` (its `stage`: `started`, `checkpoint`, `lap`, `finished`),
/// `violation`, `finished`.
class Race {
 public:
  Race(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key);

  /// The `race` topic: `{ phase, course_id, gates, laps, go_at, closes_at, racers }`.
  HubState& view() { return view_; }
  void watch(KitCallback done = {}) { view_.start(std::move(done)); }
  void unwatch() { view_.stop(); }

  /// Refusals: `started`, `full`.
  void join(KitCallback done);
  void leave(KitCallback done);
  /// Starts the countdown (the first to join, or a developer). Refusals: `forbidden`, `started`.
  void start(KitCallback done);
  /// Reports where the racer is; the reply is `[x, z]`, the position the hub kept.
  void pos(double x, double z, KitCallback done);
  /// The winner's run `{ player, total_ms, frames }`, or null before there is one.
  void ghost(KitCallback done);

  const std::string& key() const { return key_; }

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_, key_;
  HubState view_;
};

/// One ball game: two teams, a ball the hub moves, possession, a goal in each end wall. Events:
/// `claimed`, `passed`, `shot`, `goal`, `violation`, `finished`.
class BallMatch {
 public:
  BallMatch(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key);

  /// The `ball` topic: `{ phase, width, depth, goal_width, ball, players, score, ends_at, winner }`.
  HubState& view() { return view_; }
  void watch(KitCallback done = {}) { view_.start(std::move(done)); }
  void unwatch() { view_.stop(); }

  /// `team` is `red` or `blue`. Refusals: `bad_request`, `full`, `started`.
  void join(std::string team, KitCallback done);
  /// Refusals: `forbidden`, `not_ready`, `started`.
  void start(KitCallback done);
  void pos(double x, double z, KitCallback done);
  /// Refusals: `out_of_reach`, `cooldown`, `conflict`.
  void claim(KitCallback done);
  /// Passes to a teammate. Refusals: `conflict`, `bad_request`.
  void pass(std::int64_t to, KitCallback done);
  /// Shoots toward `(x, z)`. Refusal: `conflict`.
  void shoot(double x, double z, KitCallback done);

  const std::string& key() const { return key_; }

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_, key_;
  HubState view_;
};

/// Races and ball games (the `racing` starter, types `race` and `ball`).
class RacingKit {
 public:
  RacingKit(std::shared_ptr<KitRuntime> runtime, std::string raceType = "race", std::string ballType = "ball");
  Race& race(const std::string& key);
  BallMatch& ball(const std::string& key);

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string raceType_, ballType_;
  std::mutex mu_;
  std::map<std::string, std::unique_ptr<Race>> races_;
  std::map<std::string, std::unique_ptr<BallMatch>> matches_;
};

// ---- territory ----

/// One war over a map's control points: a faction captures a point by having more members on it
/// than any other (the hub reads where players are), and each point held pays its faction.
/// Events: `captured`, `income`, `won`.
class TerritoryMap {
 public:
  TerritoryMap(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key);

  /// The `territory` topic: `{ phase, points, scores, members, winner }`.
  HubState& view() { return view_; }
  void watch(KitCallback done = {}) { view_.start(std::move(done)); }
  void unwatch() { view_.stop(); }

  /// Joins a faction, once. Refusals: `conflict`, `bad_request`, `full`.
  void join(std::string faction, KitCallback done);
  /// A new war. Developers only.
  void reset(KitCallback done);

  const std::string& key() const { return key_; }

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_, key_;
  HubState view_;
};

/// One match's circle: it holds, then shrinks, and wears down every player outside it until one
/// is left. Events: `warning`, `shrinking`, `settled`, `final`, `eliminated`, `won`.
class ZoneMatch {
 public:
  ZoneMatch(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key);

  /// The `zone` topic: `{ phase, center, radius, stage, next_radius, players, alive, winner }`.
  HubState& view() { return view_; }
  void watch(KitCallback done = {}) { view_.start(std::move(done)); }
  void unwatch() { view_.stop(); }

  /// Refusals: `started`, `full`.
  void join(KitCallback done);
  /// The first to join, or a developer. Refusals: `forbidden`, `not_ready`, `started`.
  void start(KitCallback done);

  const std::string& key() const { return key_; }

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_, key_;
  HubState view_;
};

/// Control points and shrinking circles (the `territory` starter, types `territory` and `zone`).
class TerritoryKit {
 public:
  TerritoryKit(std::shared_ptr<KitRuntime> runtime, std::string territoryType = "territory", std::string zoneType = "zone");
  TerritoryMap& map(const std::string& key);
  ZoneMatch& zone(const std::string& key);

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string territoryType_, zoneType_;
  std::mutex mu_;
  std::map<std::string, std::unique_ptr<TerritoryMap>> maps_;
  std::map<std::string, std::unique_ptr<ZoneMatch>> zones_;
};

}  // namespace crowdy::kit
