#pragma once

#include <algorithm>
#include <optional>
#include <vector>

#include "crowdy/kit/core.hpp"
#include "crowdy/kit/engine.hpp"

/// Leaderboards — per-player LeaderboardEntry rows keyed by a board_id,
/// written only through the trusted submit_score, with optional cron season
/// rolls. Mirrors CrowdyJS's leaderboardsBlueprint / kit.leaderboards.
namespace crowdy::kit {

/// Options for leaderboardsBlueprint.
struct LeaderboardsBlueprintOptions {
  /// Prefix for the type/function names. Defaults to none.
  std::string typePrefix;
  /// Who may submit scores: host (the elected host referees; default),
  /// server (app admins / studio backend), or automation (e.g. an event
  /// automation on a match layer's end_match).
  TrustedAuthority submitAuthority = TrustedAuthority::host();
  /// When true (default), submitting keeps the best score
  /// (max(self.score, $points)); when false, submissions overwrite.
  bool keepBest = true;
  /// When set, adds a cron automation that rolls the season: bumps season
  /// and resets score/rank on EVERY entry (e.g. "0 0 1 * *" for monthly
  /// seasons). Empty = no season roll.
  std::string seasonCron;
  /// Owner-mirror typing (see the kit convention). Defaults to Int.
  OwnerIdKind ownerIdKind = OwnerIdKind::Int;
};

/// Names derived by leaderboardsBlueprint for a given prefix.
struct LeaderboardsNames {
  std::string entryType;
  std::string submitFn;
  std::string rollSeasonFn;
  std::string seasonAutomation;
};

/// Compute the type/function names a leaderboards blueprint (and its runtime
/// helper) uses.
inline LeaderboardsNames leaderboardsNames(std::string_view typePrefix = {}) {
  const std::string fnPrefix =
      typePrefix.empty() ? std::string() : toSnakeCase(typePrefix) + "_";
  std::string autoPrefix = fnPrefix;
  for (char& c : autoPrefix) {
    if (c == '_') c = '-';
  }
  LeaderboardsNames n;
  n.entryType = std::string(typePrefix) + "LeaderboardEntry";
  n.submitFn = fnPrefix + "submit_score";
  n.rollSeasonFn = fnPrefix + "roll_season";
  n.seasonAutomation = autoPrefix + "season-roll";
  return n;
}

/// Blueprint for leaderboards: per-player LeaderboardEntry rows keyed by a
/// board_id, written only through the trusted submit_score (host-refereed by
/// default — configure submitAuthority), with optional cron season rolls.
///
/// Ranking is honest about the platform: container lists have no server-side
/// ORDER BY, so LeaderboardsKit::top() fetches a board's entries and sorts
/// client-side — fine for the few hundred entries a per-app board holds.
/// Automation selectors' "pick: highest" covers server-side top-1 needs.
///
/// Runtime counterpart: LeaderboardsKit.
inline KitBlueprint leaderboardsBlueprint(const LeaderboardsBlueprintOptions& options = {}) {
  const OwnerIdKind kind = options.ownerIdKind;
  const LeaderboardsNames names = leaderboardsNames(options.typePrefix);

  auto propertyDef = [](const std::string& type, const char* key, const char* valueType,
                        std::string defaultJson, std::string description) {
    JVal p;
    p["containerTypeName"] = type;
    p["key"] = key;
    p["valueType"] = valueType;
    p["defaultValueJson"] = std::move(defaultJson);
    p["description"] = std::move(description);
    return p;
  };
  auto mutation = [](const char* property, std::string expression) {
    JVal m;
    m["target"] = "self";
    m["property"] = property;
    m["expression"] = std::move(expression);
    return m;
  };

  KitBlueprint bp;
  bp.name = names.entryType;

  {
    JVal t;
    t["typeName"] = names.entryType;
    t["displayName"] = names.entryType;
    t["instantiableBy"] = "member";
    t["description"] =
        "One player's entry on one leaderboard (score writable only via submit_score).";
    bp.containerTypes.push_back(std::move(t));
  }

  bp.propertyDefinitions.push_back(ownerMirrorProperty(names.entryType, kind));
  bp.propertyDefinitions.push_back(
      propertyDef(names.entryType, "board_id", "string", "\"\"",
                  "Which leaderboard this entry belongs to (e.g. 'weekly_kills')."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.entryType, "score", "int", "0",
                  "The ranked score, written only through submit_score."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.entryType, "season", "int", "1",
                  "Season counter, bumped by the season-roll automation."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.entryType, "rank", "int", "0",
                  "Optional stamped rank (0 = unstamped; ranking is client-side by default)."));

  {
    JVal fn;
    fn["name"] = names.submitFn;
    fn["containerTypeName"] = names.entryType;
    fn["returnType"] = "int";
    {
      JVal p;
      p["name"] = "points";
      p["valueType"] = "int";
      p["required"] = true;
      p["description"] = "The score to submit.";
      fn["parameters"] = JVal::array({std::move(p)});
    }
    fn["mutations"] = JVal::array(
        {mutation("score", options.keepBest ? "max(self.score, $points)" : "$points")});
    fn["returnExpression"] = "self.score";
    applyTrustedAuthority(fn, options.submitAuthority);
    fn["description"] = std::string("Trusted score submission (") +
                        (options.keepBest ? "keeps the best score" : "overwrites") +
                        "); never a plain player call.";
    bp.functions.push_back(std::move(fn));
  }

  if (!options.seasonCron.empty()) {
    {
      JVal fn;
      fn["name"] = names.rollSeasonFn;
      fn["containerTypeName"] = names.entryType;
      fn["returnType"] = "int";
      fn["mutations"] = JVal::array({mutation("season", "self.season + 1"),
                                     mutation("score", "0"), mutation("rank", "0")});
      fn["returnExpression"] = "self.season";
      fn["invokePolicyJson"] = kitPolicyJson(isAutomationPolicy());
      fn["autonomousInvocable"] = true;
      fn["description"] =
          "Season roll (automation-only): bumps the season and resets scores.";
      bp.functions.push_back(std::move(fn));
    }
    {
      JVal a;
      a["name"] = names.seasonAutomation;
      a["functionName"] = names.rollSeasonFn;
      a["targetMode"] = "type";
      a["targetTypeName"] = names.entryType;
      a["triggerType"] = "schedule";
      a["scheduleKind"] = "cron";
      a["cronExpr"] = options.seasonCron;
      a["maxTargets"] = 500;
      a["description"] = "Rolls every leaderboard entry into the next season on the cron.";
      bp.automations.push_back(std::move(a));
    }
  }

  return bp;
}

/// A parsed view of one leaderboard entry.
struct KitLeaderboardEntry {
  std::string containerId;
  std::string displayName;
  std::string ownerUserId;  ///< empty when the entry has no owner
  std::string boardId;
  std::int64_t score = 0;
  std::int64_t season = 1;
  /// 1-based position AFTER client-side sorting (not the stamped rank
  /// property).
  int position = 0;
};

/// Runtime helpers for the leaderboardsBlueprint conventions: ensure
/// per-player entries, submit scores (trusted — host by default), and rank.
/// There is no server-side ORDER BY on container lists, so reads fetch a
/// board's entries and sort client-side — fine for the few hundred entries a
/// per-app board holds.
///
/// Obtained via GameKitClient::leaderboards().
class LeaderboardsKit {
 public:
  LeaderboardsKit(std::string appId, domains::GameModelAPI& gameModel,
                  std::string_view typePrefix = {}, EngineDetector* engines = nullptr,
                  std::string_view engineModuleName = {})
      : appId_(std::move(appId)),
        gameModel_(gameModel),
        names_(leaderboardsNames(typePrefix)),
        engines_(engines),
        engineModuleName_(engineModuleName.empty() ? std::string("board-engine")
                                                   : std::string(engineModuleName)) {}

  const LeaderboardsNames& names() const { return names_; }

  // -- Engine path (Wave 2): server-computed rankings ------------------------

  /// Is a board compute engine deployed + enabled (cached per client)?
  bool engineAvailable() { return engines_ != nullptr && engines_->has(engineModuleName_); }

  /// A server-ranked page (tie-aware rank + percentile computed module-side).
  Json engineTop(std::string_view boardId, std::int64_t page = 0, std::int64_t perPage = 10) {
    JVal params;
    params["boardId"] = boardId;
    params["page"] = page;
    params["perPage"] = perPage;
    return engineInvoke("top", params);
  }

  /// Your (or a subject's) server-computed ranked row.
  Json engineRankOf(std::string_view boardId, std::string_view subjectId = {}) {
    JVal params;
    params["boardId"] = boardId;
    if (!subjectId.empty()) params["subjectId"] = subjectId;
    return engineInvoke("rank_of", params);
  }

  /// Submit YOUR OWN score (personal-best semantics) to an engine board.
  Json engineSubmitSelf(std::string_view boardId, std::int64_t score) {
    JVal params;
    params["boardId"] = boardId;
    params["score"] = score;
    return engineInvoke("submit_self", params);
  }

  /// Frozen season snapshots (top rows per rolled season).
  Json engineSeasons(std::string_view boardId) {
    JVal params;
    params["boardId"] = boardId;
    return engineInvoke("seasons", params);
  }

  /// Find-or-create a player's entry on a board.
  Json ensureEntry(std::string_view ownerUserId, std::string_view boardId,
                   std::string_view displayName = {}) {
    const std::vector<KitLeaderboardEntry> entries = board(boardId);
    for (const auto& entry : entries) {
      if (entry.ownerUserId == ownerUserId) {
        return gameModel_.container(appId_, entry.containerId);
      }
    }
    JVal input;
    input["appId"] = appId_;
    input["typeName"] = names_.entryType;
    input["displayName"] = displayName.empty()
                               ? std::string(boardId) + " " + std::string(ownerUserId)
                               : std::string(displayName);
    JArray properties;
    properties.push_back(property("owner_user_id", "int", std::string(ownerUserId)));
    properties.push_back(property("board_id", "string", JVal(boardId).dump()));
    input["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(input);
  }

  /// Submit a score — a trusted call (host by default per the blueprint's
  /// submitAuthority). Resolves with the entry's (possibly kept-best) score.
  KitInvokeResult submit(std::string_view entryId, std::int64_t points) {
    JVal params;
    params["points"] = points;
    return kitInvoke(gameModel_, appId_, names_.submitFn, entryId, params);
  }

  /// All entries of one board, sorted best-first with 1-based positions.
  std::vector<KitLeaderboardEntry> board(std::string_view boardId) {
    Json containers = gameModel_.containers(appId_, names_.entryType);
    std::vector<KitLeaderboardEntry> rows;
    containers.forEach([&](Json c) {
      Json props = kitContainerProperties(gameModel_, appId_, c["containerId"].asStringView());
      KitLeaderboardEntry row;
      row.containerId = c["containerId"].asString();
      row.displayName = c["displayName"].asString();
      Json owner = c["ownerUserId"];
      if (owner.ok() && !owner.isNull()) row.ownerUserId = owner.asString();
      row.boardId = props["board_id"].asString();
      row.score = props["score"].asInt64();
      row.season = props["season"].asInt64(1);
      if (row.boardId == boardId) rows.push_back(std::move(row));
    });
    std::stable_sort(rows.begin(), rows.end(),
                     [](const KitLeaderboardEntry& a, const KitLeaderboardEntry& b) {
                       return a.score > b.score;
                     });
    for (std::size_t i = 0; i < rows.size(); ++i) rows[i].position = static_cast<int>(i) + 1;
    return rows;
  }

  /// The top n of a board (client-side ranking).
  std::vector<KitLeaderboardEntry> top(std::string_view boardId, std::size_t n = 10) {
    std::vector<KitLeaderboardEntry> entries = board(boardId);
    if (entries.size() > n) entries.resize(n);
    return entries;
  }

  /// The entries around one player on a board (radius above and below), for
  /// "your neighborhood" widgets.
  std::vector<KitLeaderboardEntry> around(std::string_view boardId, std::string_view userId,
                                          std::size_t radius = 2) {
    const std::vector<KitLeaderboardEntry> entries = board(boardId);
    std::size_t index = entries.size();
    for (std::size_t i = 0; i < entries.size(); ++i) {
      if (entries[i].ownerUserId == userId) {
        index = i;
        break;
      }
    }
    if (index == entries.size()) return {};
    const std::size_t begin = index >= radius ? index - radius : 0;
    const std::size_t end = std::min(entries.size(), index + radius + 1);
    return std::vector<KitLeaderboardEntry>(entries.begin() + static_cast<std::ptrdiff_t>(begin),
                                            entries.begin() + static_cast<std::ptrdiff_t>(end));
  }

  /// The board's current season (max season across its entries; 1 when
  /// empty).
  std::int64_t season(std::string_view boardId) {
    std::int64_t max = 1;
    for (const auto& entry : board(boardId)) max = std::max(max, entry.season);
    return max;
  }

 private:
  static JVal property(const char* key, const char* valueType, std::string valueJson) {
    JVal p;
    p["key"] = key;
    p["valueType"] = valueType;
    p["valueJson"] = std::move(valueJson);
    return p;
  }

  Json engineInvoke(std::string_view exportName, const JVal& params) {
    if (engines_ == nullptr) {
      throw std::runtime_error("board engine unavailable: compute domain not wired");
    }
    EngineInvokeResult result = engines_->invoke(engineModuleName_, exportName, params);
    if (!result.success) {
      throw std::runtime_error("leaderboards." + std::string(exportName) +
                               " failed: " + result.reason);
    }
    return result.body;
  }

  std::string appId_;
  domains::GameModelAPI& gameModel_;
  LeaderboardsNames names_;
  EngineDetector* engines_ = nullptr;
  std::string engineModuleName_;
};

}  // namespace crowdy::kit
