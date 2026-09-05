#pragma once

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <optional>
#include <stdexcept>
#include <vector>

#include "crowdy/core/uuid.hpp"
#include "crowdy/domains/groups.hpp"
#include "crowdy/kit/core.hpp"
#include "crowdy/kit/engine.hpp"
#include "crowdy/replication/connection.hpp"

/// Matches — lobbies, rounds, turns, scoring: the session-layer wrapper every
/// session game needs. Sessions ARE the match primitive (participants +
/// current turn); this adds a session-scoped MatchMeta (lobby state, round,
/// winner, notification channel) and per-player Score rows.
///
/// Lifecycle functions (start_match / advance_round / end_match) are
/// creator-or-host gated and declare a channel notification — the server
/// pings every channel member post-commit with "match_changed", and clients
/// re-pull the meta (the notify-to-pull pattern). Turn order itself uses the
/// platform's gameModelSetSessionTurn (the runtime helper calls it — turn
/// authority is enforced by the service, not an expression).
///
/// End-of-match hooks: attach an event automation to end_match
/// (function_invoked) to submit leaderboard scores or adjust ratings.
/// Mirrors CrowdyJS's matchesBlueprint / kit.matches.
namespace crowdy::kit {

/// Gives each turn a real wall-clock deadline. begin_turn bumps turn_seq and
/// arms a one-shot timer — deduped per match, so arming the next turn replaces
/// the previous deadline — that invokes expire_turn after delayMs. expire_turn
/// records turn_expired_seq and pings the match channel, so the current turn is
/// out of time exactly when turn_expired_seq >= turn_seq.
///
/// That comparison is also the stale-fire guard: a timer already claimed when
/// the player beat the clock fires against an older turn_seq, writes a lower
/// turn_expired_seq, and can never satisfy it.
struct MatchTurnTimer {
  int delayMs = 0;
};

/// Adds a turn_tick interval automation that increments each active match's
/// tick_count, so turn logic can compare tick_count - turn_started_tick >= N.
///
/// Deprecated: counters were a workaround for expressions having no now();
/// one-shot timers made that unnecessary. Prefer MatchTurnTimer, which gives
/// an exact deadline and arms work per match instead of scanning all of them
/// every interval. Still honoured, and slated for removal in the next major.
struct MatchTurnTick {
  int intervalMs = 0;
};

struct MatchesBlueprintOptions {
  /// Prefix for the type/function names. Defaults to none.
  std::string typePrefix;
  /// Who may submit points to a Score row. Defaults to host (the elected
  /// host referees); use server or automation for server-refereed modes.
  /// Never plain players.
  TrustedAuthority scoreAuthority = TrustedAuthority::host();
  /// When set, gives each turn a wall-clock deadline (see MatchTurnTimer).
  /// Prefer this to turnTick.
  std::optional<MatchTurnTimer> turnTimer;
  /// When set, adds the turn_tick automation (see MatchTurnTick). Deprecated.
  std::optional<MatchTurnTick> turnTick;
  /// Owner-mirror typing (see the kit convention). Defaults to Int.
  OwnerIdKind ownerIdKind = OwnerIdKind::Int;
};

/// Names derived by matchesBlueprint for a given prefix.
struct MatchesNames {
  std::string metaType;
  std::string scoreType;
  std::string startFn;
  std::string advanceRoundFn;
  std::string scoreFn;
  std::string endFn;
  std::string beginTurnFn;
  std::string expireTurnFn;
  std::string turnTickFn;
  std::string turnTickAutomation;
};

/// Compute the type/function names a matches blueprint (and its runtime
/// helper) uses.
inline MatchesNames matchesNames(std::string_view typePrefix = {}) {
  const std::string fnPrefix =
      typePrefix.empty() ? std::string() : toSnakeCase(typePrefix) + "_";
  std::string autoPrefix = fnPrefix;
  for (auto& c : autoPrefix) {
    if (c == '_') c = '-';
  }
  MatchesNames n;
  n.metaType = std::string(typePrefix) + "MatchMeta";
  n.scoreType = std::string(typePrefix) + "Score";
  n.startFn = fnPrefix + "start_match";
  n.advanceRoundFn = fnPrefix + "advance_round";
  n.scoreFn = fnPrefix + "score_points";
  n.endFn = fnPrefix + "end_match";
  n.beginTurnFn = fnPrefix + "begin_turn";
  n.expireTurnFn = fnPrefix + "expire_turn";
  n.turnTickFn = fnPrefix + "turn_tick";
  n.turnTickAutomation = autoPrefix + "match-turn-tick";
  return n;
}

/// The channel ping every lifecycle function emits post-commit
/// (notify-to-pull).
inline JVal matchChangedNotification() {
  JVal a1;
  a1["name"] = "channel_id";
  a1["expression"] = "self.channel_id";
  JVal a2;
  a2["name"] = "payload";
  a2["expression"] = "\"match_changed\"";
  JVal n;
  n["kind"] = "channel";
  n["args"] = JVal::array({std::move(a1), std::move(a2)});
  return n;
}

/// The one-shot deadline a turn arms for itself. Deduped per match container,
/// so opening the next turn replaces the pending deadline rather than stacking
/// another one; the seq travels as a parameter so the fire can tell whether the
/// turn it was armed for is still open.
inline JVal turnDeadlineTimer(const MatchesNames& names, int delayMs) {
  JVal param;
  param["name"] = "turn_seq";
  param["expression"] = "self.turn_seq";
  JVal t;
  t["functionName"] = names.expireTurnFn;
  t["delayMsExpression"] = std::to_string(delayMs);
  t["dedupeKeyExpression"] = "concat(\"match_turn:\", $self_container_id)";
  t["params"] = JVal::array({std::move(param)});
  return t;
}

/// Build the matches blueprint. Runtime counterpart: MatchesKit.
inline KitBlueprint matchesBlueprint(const MatchesBlueprintOptions& options = {}) {
  const MatchesNames names = matchesNames(options.typePrefix);
  const OwnerIdKind kind = options.ownerIdKind;

  auto creatorOrHost = [](std::string_view stateCondition) {
    return kitPolicyJson(andPolicy(
        {orPolicy({isHostPolicy(),
                   conditionPolicy("self.creator_user_id == $caller_user_id")}),
         conditionPolicy(stateCondition)}));
  };

  auto propertyDef = [](const std::string& type, const char* key, const char* valueType,
                        const char* defaultJson, const char* description) {
    JVal p;
    p["containerTypeName"] = type;
    p["key"] = key;
    p["valueType"] = valueType;
    p["defaultValueJson"] = defaultJson;
    p["description"] = description;
    return p;
  };
  auto mutation = [](const char* target, const char* property, const char* expression) {
    JVal m;
    m["target"] = target;
    m["property"] = property;
    m["expression"] = expression;
    return m;
  };

  KitBlueprint bp;
  bp.name = names.metaType;

  {
    JVal t;
    t["typeName"] = names.metaType;
    t["displayName"] = names.metaType;
    t["instantiableBy"] = "member";
    t["description"] =
        "Session-scoped match record: lobby state, round, winner, notification channel.";
    bp.containerTypes.push_back(std::move(t));
  }
  {
    JVal t;
    t["typeName"] = names.scoreType;
    t["displayName"] = names.scoreType;
    t["instantiableBy"] = "member";
    t["description"] = "One player's session-scoped score row.";
    bp.containerTypes.push_back(std::move(t));
  }

  bp.propertyDefinitions.push_back(
      propertyDef(names.metaType, "creator_user_id", "int", "0",
                  "The user who created the match (may start/advance/end it)."));
  bp.propertyDefinitions.push_back(propertyDef(names.metaType, "mode", "string", "\"\"",
                                               "App-defined mode label (e.g. 'ranked', 'ffa')."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.metaType, "state", "string", "\"lobby\"",
                  "Match lifecycle: 'lobby' | 'active' | 'finished'."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.metaType, "round", "int", "0", "Current round (0 in the lobby)."));
  bp.propertyDefinitions.push_back(propertyDef(names.metaType, "max_players", "int", "0",
                                               "Advertised player cap (0 = unlimited)."));
  bp.propertyDefinitions.push_back(propertyDef(names.metaType, "winner_user_id", "int", "0",
                                               "Set by end_match (0 while unresolved)."));
  bp.propertyDefinitions.push_back(propertyDef(
      names.metaType, "channel_id", "int", "0",
      "The per-match channel lifecycle notifications ping (notify-to-pull)."));
  if (options.turnTimer) {
    bp.propertyDefinitions.push_back(propertyDef(
        names.metaType, "turn_seq", "int", "0",
        "Monotonic turn counter bumped by begin_turn; each turn arms a deadline tagged with "
        "this value."));
    bp.propertyDefinitions.push_back(propertyDef(
        names.metaType, "turn_expired_seq", "int", "0",
        "Highest turn_seq whose deadline elapsed. The turn is out of time iff "
        "turn_expired_seq >= turn_seq."));
  }
  if (options.turnTick) {
    bp.propertyDefinitions.push_back(propertyDef(
        names.metaType, "tick_count", "int", "0",
        "Monotonic counter bumped by the turn-tick automation (the wall-clock-free timer)."));
  }
  bp.propertyDefinitions.push_back(ownerMirrorProperty(names.scoreType, kind));
  bp.propertyDefinitions.push_back(propertyDef(names.scoreType, "points", "int", "0",
                                               "One player's score in the match."));

  // expire_turn is declared before anything arms it, so the seed resolves the
  // timer target in one pass instead of warning about a forward reference.
  if (options.turnTimer) {
    JVal param;
    param["name"] = "turn_seq";
    param["valueType"] = "int";
    param["required"] = true;
    param["description"] = "The turn this deadline was armed for.";
    JVal fn;
    fn["name"] = names.expireTurnFn;
    fn["containerTypeName"] = names.metaType;
    fn["returnType"] = "int";
    fn["parameters"] = JVal::array({std::move(param)});
    fn["mutations"] = JVal::array(
        {mutation("self", "turn_expired_seq", "max(self.turn_expired_seq, $turn_seq)")});
    fn["returnExpression"] = "self.turn_expired_seq";
    fn["invokePolicyJson"] = kitPolicyJson(isAutomationPolicy());
    fn["autonomousInvocable"] = true;
    fn["notifications"] = JVal::array({matchChangedNotification()});
    fn["description"] =
        "Fired by the turn deadline: record that turn $turn_seq ran out and ping the match "
        "channel. max() keeps turn_expired_seq monotonic, so a deadline that was already "
        "claimed when the player beat the clock lands below turn_seq and is ignored.";
    bp.functions.push_back(std::move(fn));
  }

  {
    JVal fn;
    fn["name"] = names.startFn;
    fn["containerTypeName"] = names.metaType;
    fn["returnType"] = "string";
    if (options.turnTimer) {
      fn["mutations"] = JVal::array({mutation("self", "state", "\"active\""),
                                     mutation("self", "round", "1"),
                                     mutation("self", "turn_seq", "1")});
    } else {
      fn["mutations"] = JVal::array({mutation("self", "state", "\"active\""),
                                     mutation("self", "round", "1")});
    }
    fn["returnExpression"] = "self.state";
    if (options.turnTimer) {
      fn["timers"] =
          JVal::array({turnDeadlineTimer(names, options.turnTimer->delayMs)});
    }
    fn["invokePolicyJson"] = creatorOrHost("self.state == \"lobby\"");
    fn["notifications"] = JVal::array({matchChangedNotification()});
    fn["description"] =
        options.turnTimer
            ? "Start the match (creator or host): lobby → active, round 1, and arm the first "
              "turn’s deadline; pings the match channel post-commit."
            : "Start the match (creator or host): lobby → active, round 1; pings the match "
              "channel post-commit.";
    bp.functions.push_back(std::move(fn));
  }
  {
    JVal fn;
    fn["name"] = names.advanceRoundFn;
    fn["containerTypeName"] = names.metaType;
    fn["returnType"] = "int";
    fn["mutations"] = JVal::array({mutation("self", "round", "self.round + 1")});
    fn["returnExpression"] = "self.round";
    fn["invokePolicyJson"] = creatorOrHost("self.state == \"active\"");
    fn["notifications"] = JVal::array({matchChangedNotification()});
    fn["description"] =
        "Advance to the next round (creator or host); pings the match channel post-commit.";
    bp.functions.push_back(std::move(fn));
  }
  {
    JVal param;
    param["name"] = "winner_user_id";
    param["valueType"] = "int";
    param["required"] = true;
    param["description"] = "The winning user (0 for a draw).";
    JVal fn;
    fn["name"] = names.endFn;
    fn["containerTypeName"] = names.metaType;
    fn["returnType"] = "string";
    fn["parameters"] = JVal::array({std::move(param)});
    // Advancing the seq strands any deadline still pending: it was armed for
    // the previous turn, so its write can no longer reach turn_seq and a
    // finished match never reads as out of time.
    if (options.turnTimer) {
      fn["mutations"] =
          JVal::array({mutation("self", "state", "\"finished\""),
                       mutation("self", "winner_user_id", "$winner_user_id"),
                       mutation("self", "turn_seq", "self.turn_seq + 1")});
    } else {
      fn["mutations"] =
          JVal::array({mutation("self", "state", "\"finished\""),
                       mutation("self", "winner_user_id", "$winner_user_id")});
    }
    fn["returnExpression"] = "self.state";
    fn["invokePolicyJson"] = creatorOrHost("self.state == \"active\"");
    fn["notifications"] = JVal::array({matchChangedNotification()});
    fn["description"] =
        "Finish the match and record the winner (creator or host); attach an event automation "
        "to this function for rating/leaderboard hooks.";
    bp.functions.push_back(std::move(fn));
  }
  {
    JVal param;
    param["name"] = "points";
    param["valueType"] = "int";
    param["required"] = true;
    param["description"] = "Signed points to add.";
    JVal fn;
    fn["name"] = names.scoreFn;
    fn["containerTypeName"] = names.scoreType;
    fn["returnType"] = "int";
    fn["parameters"] = JVal::array({std::move(param)});
    fn["mutations"] = JVal::array({mutation("self", "points", "self.points + $points")});
    fn["returnExpression"] = "self.points";
    applyTrustedAuthority(fn, options.scoreAuthority);
    fn["description"] =
        "Add points to a player's Score row — trusted (host-refereed by default).";
    bp.functions.push_back(std::move(fn));
  }

  if (options.turnTimer) {
    JVal fn;
    fn["name"] = names.beginTurnFn;
    fn["containerTypeName"] = names.metaType;
    fn["returnType"] = "int";
    fn["mutations"] = JVal::array({mutation("self", "turn_seq", "self.turn_seq + 1")});
    fn["returnExpression"] = "self.turn_seq";
    fn["timers"] = JVal::array({turnDeadlineTimer(names, options.turnTimer->delayMs)});
    fn["invokePolicyJson"] = kitPolicyJson(andPolicy(
        {orPolicy({isHostPolicy(),
                   conditionPolicy("self.creator_user_id == $caller_user_id"),
                   conditionPolicy("$current_turn_user_id == $caller_user_id")}),
         conditionPolicy("self.state == \"active\"")}));
    fn["description"] =
        "Open a turn: bump turn_seq and arm this turn's deadline (host, creator, or the "
        "outgoing turn holder). Call it before handing the turn over so the incoming player "
        "is never left without a clock.";
    bp.functions.push_back(std::move(fn));
  }

  if (options.turnTick) {
    JVal fn;
    fn["name"] = names.turnTickFn;
    fn["containerTypeName"] = names.metaType;
    fn["returnType"] = "int";
    fn["mutations"] =
        JVal::array({mutation("self", "tick_count", "self.tick_count + 1")});
    fn["returnExpression"] = "self.tick_count";
    fn["invokePolicyJson"] = kitPolicyJson(isAutomationPolicy());
    fn["autonomousInvocable"] = true;
    fn["description"] =
        "Server-driven timer tick for active matches (counters instead of wall clocks — "
        "expressions have no now()).";
    bp.functions.push_back(std::move(fn));

    JVal w;
    w["key"] = "state";
    w["op"] = "==";
    w["value"] = "active";
    JVal selector;
    selector["selfWhere"] = JVal::array({std::move(w)});
    JVal a;
    a["name"] = names.turnTickAutomation;
    a["functionName"] = names.turnTickFn;
    a["targetMode"] = "type";
    a["targetTypeName"] = names.metaType;
    a["triggerType"] = "schedule";
    a["scheduleKind"] = "interval";
    a["intervalMs"] = options.turnTick->intervalMs;
    a["maxTargets"] = 64;
    a["selectorJson"] = selector.dump();
    a["description"] = "Bumps tick_count on active matches (turn-timeout timer source).";
    bp.automations.push_back(std::move(a));
  }

  return bp;
}

/// A parsed view of one match.
struct KitMatch {
  /// The session backing the match (participants + turn order).
  std::string sessionId;
  /// The MatchMeta container id (the `self` of the lifecycle functions).
  std::string metaId;
  std::string displayName;
  std::int64_t creatorUserId = 0;
  std::string mode;
  std::string state;
  std::int64_t round = 0;
  std::int64_t maxPlayers = 0;
  std::int64_t winnerUserId = 0;
  /// The per-match notification channel ("0" when none was wired).
  std::string channelId;
  /// Present when the blueprint was deployed with turnTick.
  std::optional<std::int64_t> tickCount;
  /// The current turn's sequence number. Present only when the blueprint was
  /// deployed with turnTimer, which is how the helpers detect that turns carry
  /// a deadline.
  std::optional<std::int64_t> turnSeq;
  /// The highest turn sequence whose deadline elapsed. See turnExpired().
  std::optional<std::int64_t> turnExpiredSeq;
};

/// Whether the match's current turn has run out of time — true once the
/// deadline for the open turn has fired. Always false for a match deployed
/// without turnTimer, and false for a deadline stranded by an earlier turn,
/// since those record a lower sequence than the one now open.
inline bool turnExpired(const KitMatch& match) {
  if (!match.turnSeq || !match.turnExpiredSeq) return false;
  return *match.turnSeq > 0 && *match.turnExpiredSeq >= *match.turnSeq;
}

/// One row of the match standings.
struct KitMatchScore {
  std::string containerId;
  std::string ownerUserId;  ///< empty when unowned
  std::int64_t points = 0;
};

/// Runtime helpers for the matchesBlueprint conventions: sessions ARE the
/// match primitive — create() makes a session + a MatchMeta + a per-match
/// channel; turn order goes through the platform's session-turn authority;
/// lifecycle functions ping the channel post-commit.
///
/// Receiving the pings: the lifecycle functions (and notifyChanged) publish
/// "match_changed" on the match channel. Consumers on the native replication
/// connection read the WorldSession channel inbox (session::WorldSession
/// channelInbox()) and re-pull the match with get() when a message for
/// KitMatch::channelId arrives — the notify-to-pull loop this kit does not
/// wrap itself.
class MatchesKit {
 public:
  /// `channels` and `connection` are optional (may be nullptr): channels is
  /// needed by create()/join(), connection by notifyChanged().
  MatchesKit(std::string appId, domains::GameModelAPI& gameModel,
             domains::ChannelsAPI* channels, replication::Connection* connection = nullptr,
             std::string_view typePrefix = {},
             std::optional<core::ActorUuid> actorUuid = std::nullopt,
             EngineDetector* engines = nullptr, std::string_view engineModuleName = {})
      : appId_(std::move(appId)),
        gameModel_(gameModel),
        channels_(channels),
        connection_(connection),
        engines_(engines),
        engineModuleName_(engineModuleName.empty() ? std::string("match-engine")
                                                   : std::string(engineModuleName)),
        names_(matchesNames(typePrefix)),
        actorUuid_(actorUuid ? *actorUuid : core::generateActorUuid()) {}

  const MatchesNames& names() const { return names_; }

  // -- Engine path (Wave 2): server-driven lifecycle over the match engine --

  /// Is a match compute engine deployed + enabled (cached per client)?
  bool engineAvailable() { return engines_ != nullptr && engines_->has(engineModuleName_); }

  /// Declare ready on an engine match (MatchMeta container id); the match
  /// starts server-side once every expected player is ready.
  Json engineReady(std::string_view matchId) { return engineInvoke("ready", matchId, JVal()); }

  /// Submit your move (the engine validates the turn + resolves).
  Json engineSubmitMove(std::string_view matchId, const JVal& params = JVal()) {
    return engineInvoke("submit_move", matchId, params);
  }

  /// Forfeit an engine match.
  Json engineForfeit(std::string_view matchId) { return engineInvoke("forfeit", matchId, JVal()); }

  /// The engine's live view: turn holder, timers, standings, summary.
  Json engineStatus(std::string_view matchId) { return engineInvoke("status", matchId, JVal()); }

  /// Resolve a matchmaking proposal to the match the engine created for it.
  std::optional<std::string> findByProposal(std::string_view proposalId) {
    if (engines_ == nullptr) return std::nullopt;
    JVal params;
    params["proposalId"] = proposalId;
    EngineInvokeResult result = engines_->invoke(engineModuleName_, "find_by_proposal", params);
    if (!result.success) return std::nullopt;
    std::string matchId = result.body["matchId"].asString();
    if (matchId.empty()) return std::nullopt;
    return matchId;
  }

  /// The notify-to-pull loop, ingest-flavored: feed channel notifications
  /// (from a Connection channelMessage handler or the WorldSession channel
  /// inbox) into this method; when one pings `match`'s channel, the match
  /// state is re-pulled and handed to `callback`. Returns true when the ping
  /// matched (the CrowdyJS onMatchChanged analog for the native transport).
  bool onMatchChanged(const KitMatch& match, std::int64_t notifiedChannelId,
                      const std::function<void(const KitMatch&)>& callback) {
    if (std::to_string(notifiedChannelId) != match.channelId) return false;
    callback(get(match.metaId));
    return true;
  }

  /// Create a match: a session (the platform match primitive), a per-match
  /// notification channel, and the session-scoped MatchMeta.
  /// `creatorUserId` is the calling player's user id (stored so the creator
  /// may start/advance/end the match).
  KitMatch create(std::string_view creatorUserId, std::string_view mode = {},
                  std::int64_t maxPlayers = 0, std::string_view displayName = {}) {
    JVal sessionInput;
    sessionInput["appId"] = appId_;
    sessionInput["name"] = displayName.empty()
                               ? "match-" + std::string(mode.empty() ? "default" : mode)
                               : std::string(displayName);
    Json session = gameModel_.createSession(sessionInput);
    const std::string sessionId = session["sessionId"].asString();

    JVal channelInput;
    channelInput["appId"] = appId_;
    channelInput["name"] = "match-" + sessionId;
    channelInput["description"] =
        "Per-match notification channel (kit.matches notify-to-pull).";
    Json channel = requireChannels().create(channelInput);

    JVal metaInput;
    metaInput["appId"] = appId_;
    metaInput["typeName"] = names_.metaType;
    metaInput["displayName"] =
        displayName.empty() ? "Match " + sessionId : std::string(displayName);
    metaInput["sessionId"] = sessionId;
    JArray properties;
    properties.push_back(property("creator_user_id", "int", std::string(creatorUserId)));
    properties.push_back(property("mode", "string", JVal(mode).dump()));
    properties.push_back(property("max_players", "int", std::to_string(maxPlayers)));
    properties.push_back(
        property("channel_id", "int", std::to_string(channel["groupId"].asBigInt())));
    metaInput["properties"] = JVal(std::move(properties));
    Json meta = gameModel_.createContainer(metaInput);
    return toMatch(meta["containerId"].asString(), sessionId,
                   meta["displayName"].asString());
  }

  /// List joinable matches (metas still in the lobby state).
  std::vector<KitMatch> open() {
    Json metas = gameModel_.containers(appId_, names_.metaType);
    std::vector<KitMatch> out;
    metas.forEach([&](Json m) {
      Json sessionId = m["sessionId"];
      if (!sessionId.ok() || sessionId.isNull()) return;
      KitMatch match = toMatch(m["containerId"].asString(), sessionId.asString(),
                               m["displayName"].asString());
      if (match.state == "lobby") out.push_back(std::move(match));
    });
    return out;
  }

  /// Read one match by its MatchMeta container.
  KitMatch get(std::string_view metaId) {
    Json meta = gameModel_.container(appId_, metaId);
    return toMatch(meta["containerId"].asString(), meta["sessionId"].asString(),
                   meta["displayName"].asString());
  }

  /// Join a match: session participation + the notification channel.
  Json join(const KitMatch& match) {
    JVal input;
    input["appId"] = appId_;
    input["sessionId"] = match.sessionId;
    Json participant = gameModel_.joinSession(input);
    if (match.channelId != "0" && !match.channelId.empty()) {
      requireChannels().join(match.channelId);
    }
    return participant;
  }

  /// Start the match (creator or host). Pings the match channel post-commit.
  KitInvokeResult start(const KitMatch& match) {
    return kitInvoke(gameModel_, appId_, names_.startFn, match.metaId, JVal(),
                     match.sessionId);
  }

  /// Advance to the next round (creator or host).
  KitInvokeResult advanceRound(const KitMatch& match) {
    return kitInvoke(gameModel_, appId_, names_.advanceRoundFn, match.metaId, JVal(),
                     match.sessionId);
  }

  /// Whether it is `userId`'s session turn right now.
  bool myTurn(const KitMatch& match, std::string_view userId) {
    Json session = gameModel_.session(appId_, match.sessionId);
    Json turn = session["currentTurnUserId"];
    if (!turn.ok() || turn.isNull()) return false;
    return std::to_string(turn.asBigInt()) == userId;
  }

  /// Pass the turn to the next player via the platform's session-turn
  /// authority (current holder, host, or admin — enforced by the service),
  /// then ping the match channel so everyone re-pulls.
  ///
  /// For a match deployed with turnTimer, this opens the incoming turn first so
  /// it arrives with a deadline already running. Opening it before the handover
  /// also means the outgoing holder is still the session's turn user, which is
  /// what authorizes them to do it.
  Json endTurn(const KitMatch& match, std::string_view nextUserId) {
    if (match.turnSeq) beginTurn(match);
    JVal input;
    input["appId"] = appId_;
    input["sessionId"] = match.sessionId;
    input["userId"] = nextUserId;
    Json session = gameModel_.setSessionTurn(input);
    notifyChanged(match);
    return session;
  }

  /// Open a turn and arm its deadline (turnTimer deployments only) — the
  /// deadline replaces any still pending for this match. endTurn() calls this
  /// for you; call it directly to give the current player a fresh clock, e.g.
  /// after granting extra time.
  KitInvokeResult beginTurn(const KitMatch& match) {
    return kitInvoke(gameModel_, appId_, names_.beginTurnFn, match.metaId, JVal(),
                     match.sessionId);
  }

  /// Cancel the pending turn deadline for this match, if any. Returns how many
  /// timers were dropped. Ending a match already strands its deadline, so this
  /// is only needed when you want turns to stop being timed while play
  /// continues.
  std::int64_t cancelTurnDeadline(const KitMatch& match) {
    JVal input;
    input["appId"] = appId_;
    input["dedupeKey"] = "match_turn:" + match.metaId;
    Json cancelled = gameModel_.cancelTimer(input);
    return cancelled.ok() && !cancelled.isNull() ? cancelled.asBigInt() : 0;
  }

  /// Find-or-create a player's session-scoped Score row.
  Json ensureScore(const KitMatch& match, std::string_view ownerUserId) {
    Json scores = gameModel_.containers(appId_, names_.scoreType, match.sessionId);
    Json mine;
    scores.forEach([&](Json c) {
      if (mine.ok()) return;
      if (c["ownerUserId"].asString() == ownerUserId) mine = c;
    });
    if (mine.ok()) return mine;
    JVal input;
    input["appId"] = appId_;
    input["typeName"] = names_.scoreType;
    input["displayName"] = "Score " + std::string(ownerUserId);
    input["sessionId"] = match.sessionId;
    input["properties"] =
        JVal::array({property("owner_user_id", "int", std::string(ownerUserId))});
    return gameModel_.createContainer(input);
  }

  /// Add points to a Score row — trusted (host-refereed by default).
  /// Returns the new points.
  KitInvokeResult score(const KitMatch& match, std::string_view scoreId,
                        std::int64_t points) {
    JVal params;
    params["points"] = points;
    return kitInvoke(gameModel_, appId_, names_.scoreFn, scoreId, params, match.sessionId);
  }

  /// The match standings, highest points first (client-side sort).
  std::vector<KitMatchScore> standings(const KitMatch& match) {
    Json scores = gameModel_.containers(appId_, names_.scoreType, match.sessionId);
    std::vector<KitMatchScore> rows;
    scores.forEach([&](Json c) {
      Json props = kitContainerProperties(gameModel_, appId_, c["containerId"].asStringView());
      KitMatchScore row;
      row.containerId = c["containerId"].asString();
      row.ownerUserId = c["ownerUserId"].asString();
      row.points = props["points"].asInt64();
      rows.push_back(std::move(row));
    });
    std::sort(rows.begin(), rows.end(),
              [](const KitMatchScore& a, const KitMatchScore& b) { return b.points < a.points; });
    return rows;
  }

  /// Finish the match and record the winner (creator or host). Also drops the
  /// pending turn deadline on a turnTimer match — end_match already strands it,
  /// so this just saves the pointless fire and channel ping.
  KitInvokeResult finish(const KitMatch& match, std::int64_t winnerUserId) {
    JVal params;
    params["winner_user_id"] = winnerUserId;
    KitInvokeResult result = kitInvoke(gameModel_, appId_, names_.endFn, match.metaId,
                                       params, match.sessionId);
    if (result.success && match.turnSeq) cancelTurnDeadline(match);
    return result;
  }

  /// Manually ping the match channel with "match_changed" (the lifecycle
  /// functions do this automatically via their declared notifications; use
  /// this after out-of-band changes such as endTurn). Publishes the raw
  /// payload over the native replication connection; requires channel
  /// membership + send_messages.
  bool notifyChanged(const KitMatch& match) {
    if (match.channelId == "0" || match.channelId.empty()) return false;
    const std::int64_t channelId = std::strtoll(match.channelId.c_str(), nullptr, 10);
    auto result =
        requireConnection().sendChannelMessage(channelId, actorUuid_, asBytes("match_changed"));
    return result.ok();
  }

 private:
  domains::ChannelsAPI& requireChannels() {
    if (!channels_) {
      throw std::runtime_error(
          "MatchesKit needs a ChannelsAPI — construct it with a non-null channels pointer");
    }
    return *channels_;
  }

  replication::Connection& requireConnection() {
    if (!connection_) {
      throw std::runtime_error(
          "MatchesKit needs a replication Connection — construct it with a non-null "
          "connection pointer");
    }
    return *connection_;
  }

  static JVal property(const char* key, const char* valueType, std::string valueJson) {
    JVal p;
    p["key"] = key;
    p["valueType"] = valueType;
    p["valueJson"] = std::move(valueJson);
    return p;
  }

  KitMatch toMatch(std::string metaId, std::string sessionId, std::string displayName) {
    Json props = kitContainerProperties(gameModel_, appId_, metaId);
    KitMatch m;
    m.sessionId = std::move(sessionId);
    m.metaId = std::move(metaId);
    m.displayName = std::move(displayName);
    m.creatorUserId = props["creator_user_id"].asInt64();
    m.mode = props["mode"].asString();
    m.state = props["state"].asString();
    m.round = props["round"].asInt64();
    m.maxPlayers = props["max_players"].asInt64();
    m.winnerUserId = props["winner_user_id"].asInt64();
    m.channelId = std::to_string(props["channel_id"].asInt64());
    Json tick = props["tick_count"];
    if (tick.ok() && !tick.isNull()) m.tickCount = tick.asInt64();
    Json turnSeq = props["turn_seq"];
    if (turnSeq.ok() && !turnSeq.isNull()) m.turnSeq = turnSeq.asInt64();
    Json expiredSeq = props["turn_expired_seq"];
    if (expiredSeq.ok() && !expiredSeq.isNull()) m.turnExpiredSeq = expiredSeq.asInt64();
    return m;
  }

  Json engineInvoke(std::string_view exportName, std::string_view matchId, const JVal& extra) {
    if (engines_ == nullptr) {
      throw std::runtime_error("match engine unavailable: compute domain not wired");
    }
    JVal params = extra;
    params["matchId"] = matchId;
    EngineInvokeResult result = engines_->invoke(engineModuleName_, exportName, params);
    if (!result.success) {
      throw std::runtime_error("matches." + std::string(exportName) + " failed: " + result.reason);
    }
    return result.body;
  }

  std::string appId_;
  domains::GameModelAPI& gameModel_;
  domains::ChannelsAPI* channels_;
  replication::Connection* connection_;
  EngineDetector* engines_ = nullptr;
  std::string engineModuleName_;
  MatchesNames names_;
  core::ActorUuid actorUuid_;
};

}  // namespace crowdy::kit
