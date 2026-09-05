#pragma once

#include <optional>

#include "crowdy/kit/core.hpp"

/// Progression — character progression: per-player Progress (xp / level /
/// skill points / rating), an admin SkillDef catalog with prerequisite
/// chains bought into per-player SkillRanks, threshold AchievementDefs
/// unlocked into AchievementUnlocks, and an ELO-style adjust_rating hook for
/// the match layer. Level-ups are computed server-side inside grant_xp using
/// the fn: helper pattern: the XP curve lives in ONE internal function
/// (xp_for_level, not directly invocable) that mutation expressions call as
/// fn:xp_for_level(self.level + 1). Ordered mutations see earlier writes, so
/// xp is applied first, then the skill-point award, then the level bump —
/// one level per grant (call repeatedly for multi-level jumps). Mirrors
/// CrowdyJS's progressionBlueprint / kit.progression.
namespace crowdy::kit {

struct ProgressionBlueprintOptions {
  /// Prefix applied to the container type names ("Pvp" -> PvpProgress /
  /// PvpSkillDef / ...) and, snake-cased, to the function names
  /// (pvp_grant_xp, ...).
  std::string typePrefix;
  /// Who may grant XP. Trusted grants should be Server (the default),
  /// Automation (e.g. an event automation on a mob_died function), or Host —
  /// never plain players.
  TrustedAuthority xpAuthority = TrustedAuthority::server();
  /// Who may adjust the competitive rating (the match layer's
  /// applyMatchResult calls this).
  TrustedAuthority ratingAuthority = TrustedAuthority::host();
  /// The XP curve: an expression over $level returning the TOTAL xp required
  /// to reach that level. Evaluated via the read-only fn: helper-call
  /// pattern (fn:xp_for_level(self.level + 1)), so the curve lives in ONE
  /// internal function and every reader stays in sync.
  std::string xpForLevelExpression = "100 * $level * $level";
  /// Skill points granted per level-up.
  int skillPointsPerLevel = 1;
  /// Initial competitive rating for new players.
  int initialRating = 1000;
  /// Owner-mirror typing (see the kit convention).
  OwnerIdKind ownerIdKind = OwnerIdKind::Int;
};

/// Names derived by progressionBlueprint for a given prefix.
struct ProgressionNames {
  std::string progressType;
  std::string skillDefType;
  std::string skillRankType;
  std::string achievementDefType;
  std::string achievementUnlockType;
  std::string xpForLevelFn;
  std::string grantXpFn;
  std::string buySkillFn;
  std::string unlockAchievementFn;
  std::string adjustRatingFn;
};

/// Compute the type/function names a progression blueprint (and its runtime
/// helper) uses.
inline ProgressionNames progressionNames(std::string_view typePrefix = {}) {
  const std::string fnPrefix =
      typePrefix.empty() ? std::string() : toSnakeCase(typePrefix) + "_";
  ProgressionNames n;
  n.progressType = std::string(typePrefix) + "Progress";
  n.skillDefType = std::string(typePrefix) + "SkillDef";
  n.skillRankType = std::string(typePrefix) + "SkillRank";
  n.achievementDefType = std::string(typePrefix) + "AchievementDef";
  n.achievementUnlockType = std::string(typePrefix) + "AchievementUnlock";
  n.xpForLevelFn = fnPrefix + "xp_for_level";
  n.grantXpFn = fnPrefix + "grant_xp";
  n.buySkillFn = fnPrefix + "spend_skill_point";
  n.unlockAchievementFn = fnPrefix + "unlock_achievement";
  n.adjustRatingFn = fnPrefix + "adjust_rating";
  return n;
}

/// Build the progression blueprint. Runtime counterpart: ProgressionKit.
inline KitBlueprint progressionBlueprint(const ProgressionBlueprintOptions& options = {}) {
  const ProgressionNames names = progressionNames(options.typePrefix);
  const OwnerIdKind kind = options.ownerIdKind;

  const std::string levelUpCondition =
      "self.xp >= fn:" + names.xpForLevelFn + "(self.level + 1)";

  auto containerType = [](const std::string& typeName, const char* instantiableBy,
                          const char* description) {
    JVal t;
    t["typeName"] = typeName;
    t["displayName"] = typeName;
    t["instantiableBy"] = instantiableBy;
    t["description"] = description;
    return t;
  };
  auto propertyDef = [](const std::string& type, const char* key, const char* valueType,
                        std::optional<std::string> defaultJson, const char* description) {
    JVal p;
    p["containerTypeName"] = type;
    p["key"] = key;
    p["valueType"] = valueType;
    if (defaultJson) p["defaultValueJson"] = *defaultJson;
    p["description"] = description;
    return p;
  };
  auto param = [](const char* name, const char* valueType, const char* description) {
    JVal p;
    p["name"] = name;
    p["valueType"] = valueType;
    p["required"] = true;
    p["description"] = description;
    return p;
  };
  auto mutation = [](std::string target, std::string property, std::string expression) {
    JVal m;
    m["target"] = std::move(target);
    m["property"] = std::move(property);
    m["expression"] = std::move(expression);
    return m;
  };
  auto joinAnd = [](const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& part : parts) {
      if (!out.empty()) out += " && ";
      out += part;
    }
    return out;
  };

  KitBlueprint bp;
  bp.name = names.progressType;

  bp.containerTypes.push_back(containerType(
      names.progressType, "member", "Per-player progression: xp, level, skill points, rating."));
  bp.containerTypes.push_back(containerType(
      names.skillDefType, "admin", "Studio skill catalog row (cost, prerequisite, max rank)."));
  bp.containerTypes.push_back(
      containerType(names.skillRankType, "member", "A player's purchased rank of one skill."));
  bp.containerTypes.push_back(containerType(names.achievementDefType, "admin",
                                            "Studio achievement catalog row (xp threshold)."));
  bp.containerTypes.push_back(containerType(names.achievementUnlockType, "member",
                                            "A player's unlock state for one achievement."));

  bp.propertyDefinitions.push_back(ownerMirrorProperty(names.progressType, kind));
  bp.propertyDefinitions.push_back(
      propertyDef(names.progressType, "xp", "int", "0", "Lifetime experience points."));
  bp.propertyDefinitions.push_back(propertyDef(names.progressType, "level", "int", "1",
                                               "Current level, advanced server-side by grant_xp."));
  bp.propertyDefinitions.push_back(propertyDef(names.progressType, "skill_points", "int", "0",
                                               "Unspent skill points (earned on level-up)."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.progressType, "rating", "int", std::to_string(options.initialRating),
                  "Competitive rating (ELO-style), adjusted by the match layer."));
  bp.propertyDefinitions.push_back(propertyDef(names.skillDefType, "skill_id", "string",
                                               std::nullopt, "Stable skill identifier."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.skillDefType, "cost", "int", "1", "Skill points per rank."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.skillDefType, "requires_skill_id", "string", "\"\"",
                  "Prerequisite skill_id (empty for none); rank ≥ 1 required."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.skillDefType, "max_rank", "int", "1", "Maximum purchasable rank."));
  bp.propertyDefinitions.push_back(ownerMirrorProperty(names.skillRankType, kind));
  bp.propertyDefinitions.push_back(propertyDef(names.skillRankType, "skill_id", "string", "\"\"",
                                               "The skill this rank row tracks."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.skillRankType, "rank", "int", "0", "Current rank (0 = not learned)."));
  bp.propertyDefinitions.push_back(propertyDef(names.achievementDefType, "achievement_id",
                                               "string", std::nullopt,
                                               "Stable achievement identifier."));
  bp.propertyDefinitions.push_back(propertyDef(names.achievementDefType, "threshold", "int", "0",
                                               "Total xp required to unlock."));
  bp.propertyDefinitions.push_back(ownerMirrorProperty(names.achievementUnlockType, kind));
  bp.propertyDefinitions.push_back(
      propertyDef(names.achievementUnlockType, "achievement_id", "string", "\"\"",
                  "The achievement this unlock row tracks."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.achievementUnlockType, "unlocked", "bool", "false",
                  "True once the threshold check has passed (idempotent)."));

  {
    JVal fn;
    fn["name"] = names.xpForLevelFn;
    fn["containerTypeName"] = names.progressType;
    fn["returnType"] = "int";
    fn["invokeScope"] = "internal";
    fn["parameters"] = JVal::array(
        {param("level", "int", "The level whose total-xp requirement to compute.")});
    fn["mutations"] = JVal(JArray{});
    fn["returnExpression"] = options.xpForLevelExpression;
    fn["description"] =
        "The XP curve: total xp required to reach $level. Internal-only; read via fn: calls from "
        "grant_xp (the fn: helper pattern).";
    bp.functions.push_back(std::move(fn));
  }
  {
    JVal fn;
    fn["name"] = names.grantXpFn;
    fn["containerTypeName"] = names.progressType;
    fn["returnType"] = "int";
    fn["parameters"] =
        JVal::array({param("amount", "int", "XP to add (negative values are ignored).")});
    fn["mutations"] = JVal::array(
        {mutation("self", "xp", "self.xp + max(0, $amount)"),
         mutation("self", "skill_points",
                  "self.skill_points + if(" + levelUpCondition + ", " +
                      std::to_string(options.skillPointsPerLevel) + ", 0)"),
         mutation("self", "level",
                  "if(" + levelUpCondition + ", self.level + 1, self.level)")});
    fn["returnExpression"] = "self.level";
    applyTrustedAuthority(fn, options.xpAuthority);
    fn["description"] =
        "Trusted XP grant: adds xp, then awards skill points and bumps the level when the "
        "fn:xp_for_level curve is crossed (one level per grant).";
    bp.functions.push_back(std::move(fn));
  }
  {
    JVal fn;
    fn["name"] = names.buySkillFn;
    fn["containerTypeName"] = names.skillRankType;
    fn["returnType"] = "int";
    fn["parameters"] = JVal::array(
        {param("progress_id", "container_ref",
               "The caller's Progress container (pays the skill points)."),
         param("def_id", "container_ref", "The SkillDef being bought."),
         param("prereq_id", "container_ref",
               "The caller's SkillRank for the prerequisite skill (pass this rank container "
               "itself when the skill has no prerequisite).")});
    fn["mutations"] = JVal::array(
        {mutation("ref($progress_id)", "skill_points",
                  "ref($progress_id).skill_points - ref($def_id).cost"),
         mutation("self", "rank", "self.rank + 1")});
    fn["returnExpression"] = "self.rank";
    fn["invokePolicyJson"] = kitPolicyJson(conditionPolicy(joinAnd(
        {ownerEqualsCaller("self.owner_user_id", kind),
         ownerEqualsCaller("ref($progress_id).owner_user_id", kind),
         "self.skill_id == ref($def_id).skill_id",
         "ref($progress_id).skill_points >= ref($def_id).cost",
         "self.rank < ref($def_id).max_rank",
         "if(ref($def_id).requires_skill_id == \"\", true, ref($prereq_id).skill_id == "
         "ref($def_id).requires_skill_id && ref($prereq_id).rank >= 1 && " +
             ownerEqualsCaller("ref($prereq_id).owner_user_id", kind) + ")"})));
    fn["description"] =
        "Buy one rank of a skill: point cost, max rank, and the prerequisite chain are all "
        "checked server-side; the spend and the rank-up are one transaction.";
    bp.functions.push_back(std::move(fn));
  }
  {
    JVal fn;
    fn["name"] = names.unlockAchievementFn;
    fn["containerTypeName"] = names.achievementUnlockType;
    fn["returnType"] = "bool";
    fn["parameters"] = JVal::array(
        {param("progress_id", "container_ref",
               "The caller's Progress container (xp threshold source)."),
         param("def_id", "container_ref", "The AchievementDef whose threshold to check.")});
    fn["mutations"] = JVal::array({mutation("self", "unlocked", "true")});
    fn["returnExpression"] = "self.unlocked";
    fn["invokePolicyJson"] = kitPolicyJson(conditionPolicy(joinAnd(
        {ownerEqualsCaller("self.owner_user_id", kind),
         ownerEqualsCaller("ref($progress_id).owner_user_id", kind),
         "self.achievement_id == ref($def_id).achievement_id",
         "ref($progress_id).xp >= ref($def_id).threshold"})));
    fn["description"] =
        "Unlock an achievement once its xp threshold is met (idempotent: re-invoking just "
        "re-writes true).";
    bp.functions.push_back(std::move(fn));
  }
  {
    JVal fn;
    fn["name"] = names.adjustRatingFn;
    fn["containerTypeName"] = names.progressType;
    fn["returnType"] = "int";
    fn["parameters"] = JVal::array(
        {param("delta", "int",
               "Signed rating change (an ELO delta computed by the match layer).")});
    fn["mutations"] = JVal::array({mutation("self", "rating", "max(0, self.rating + $delta)")});
    fn["returnExpression"] = "self.rating";
    applyTrustedAuthority(fn, options.ratingAuthority);
    fn["description"] =
        "Trusted rating adjustment (host-gated by default) — wired from kit.matches match "
        "results.";
    bp.functions.push_back(std::move(fn));
  }

  return bp;
}

// ---------------------------------------------------------------------------
// Runtime kit
// ---------------------------------------------------------------------------

/// A parsed view of one player's progression.
struct KitProgress {
  std::string containerId;
  std::string displayName;
  std::string ownerUserId;
  std::int64_t xp = 0;
  std::int64_t level = 1;
  std::int64_t skillPoints = 0;
  std::int64_t rating = 0;
};

/// A parsed view of one skill catalog row.
struct KitSkillDef {
  std::string containerId;
  std::string displayName;
  std::string skillId;
  std::int64_t cost = 1;
  std::string requiresSkillId;
  std::int64_t maxRank = 1;
};

/// A parsed view of one player skill rank.
struct KitSkillRank {
  std::string containerId;
  std::string displayName;
  std::string ownerUserId;
  std::string skillId;
  std::int64_t rank = 0;
};

/// A parsed view of one achievement definition.
struct KitAchievementDef {
  std::string containerId;
  std::string displayName;
  std::string achievementId;
  std::int64_t threshold = 0;
};

/// A parsed view of one player achievement unlock.
struct KitAchievementUnlock {
  std::string containerId;
  std::string displayName;
  std::string ownerUserId;
  std::string achievementId;
  bool unlocked = false;
};

/// Runtime helpers for the progressionBlueprint conventions: ensure a
/// player's Progress, grant XP (trusted — app admins by default), buy skills
/// against the catalog's costs and prerequisites, unlock threshold
/// achievements, and apply match rating results. Everything is
/// authority-checked server-side; denials resolve with success=false.
class ProgressionKit {
 public:
  ProgressionKit(std::string appId, domains::GameModelAPI& gameModel,
                 std::string_view typePrefix = {})
      : appId_(std::move(appId)), gameModel_(gameModel), names_(progressionNames(typePrefix)) {}

  const ProgressionNames& names() const { return names_; }

  /// Find the player's Progress container, creating it when absent (with the
  /// owner_user_id mirror the guards read).
  Json ensure(std::string_view ownerUserId, std::string_view displayName = {},
              std::string_view sessionId = {}) {
    Json existing = gameModel_.containers(appId_, names_.progressType, sessionId);
    Json mine;
    existing.forEach([&](Json c) {
      if (!mine.isNull()) return;
      if (c["ownerUserId"].asString() == ownerUserId) mine = c;
    });
    if (mine.ok() && !mine.isNull()) return mine;

    JVal input;
    input["appId"] = appId_;
    input["typeName"] = names_.progressType;
    input["displayName"] = displayName.empty() ? "Progress " + std::string(ownerUserId)
                                               : std::string(displayName);
    if (!sessionId.empty()) input["sessionId"] = sessionId;
    input["properties"] =
        JVal::array({property("owner_user_id", "int", std::string(ownerUserId))});
    return gameModel_.createContainer(input);
  }

  /// Read one player's progression state.
  KitProgress state(std::string_view progressId) {
    Json container = gameModel_.container(appId_, progressId);
    Json props = kitContainerProperties(gameModel_, appId_, progressId);
    KitProgress p;
    p.containerId = container["containerId"].asString();
    p.displayName = container["displayName"].asString();
    p.ownerUserId = container["ownerUserId"].asString();
    p.xp = props["xp"].asInt64();
    p.level = props["level"].asInt64(1);
    p.skillPoints = props["skill_points"].asInt64();
    p.rating = props["rating"].asInt64();
    return p;
  }

  /// Grant XP — a TRUSTED call (default blueprint authority: app admins via
  /// server scope; or drive it from an event automation). Awards skill
  /// points and levels up when the curve is crossed. Returns the new level.
  KitInvokeResult grantXp(std::string_view progressId, std::int64_t amount) {
    JVal params;
    params["amount"] = amount;
    return kitInvoke(gameModel_, appId_, names_.grantXpFn, progressId, params);
  }

  /// List the skill catalog (admin-seeded SkillDef containers).
  std::vector<KitSkillDef> skillCatalog() {
    Json containers = gameModel_.containers(appId_, names_.skillDefType);
    std::vector<KitSkillDef> out;
    containers.forEach([&](Json c) {
      Json props = kitContainerProperties(gameModel_, appId_, c["containerId"].asStringView());
      KitSkillDef d;
      d.containerId = c["containerId"].asString();
      d.displayName = c["displayName"].asString();
      d.skillId = props["skill_id"].asString();
      d.cost = props["cost"].asInt64(1);
      d.requiresSkillId = props["requires_skill_id"].asString();
      d.maxRank = props["max_rank"].asInt64(1);
      out.push_back(std::move(d));
    });
    return out;
  }

  /// Define a skill (admin — the catalog type is admin-instantiable).
  Json defineSkill(std::string_view skillId, std::int64_t cost = 1,
                   std::string_view requiresSkillId = {}, std::int64_t maxRank = 1,
                   std::string_view displayName = {}, JArray extraProperties = {}) {
    JVal input;
    input["appId"] = appId_;
    input["typeName"] = names_.skillDefType;
    input["displayName"] = displayName.empty() ? "Skill " + std::string(skillId)
                                               : std::string(displayName);
    JArray properties;
    properties.push_back(property("skill_id", "string", JVal(skillId).dump()));
    properties.push_back(property("cost", "int", std::to_string(cost)));
    properties.push_back(
        property("requires_skill_id", "string", JVal(requiresSkillId).dump()));
    properties.push_back(property("max_rank", "int", std::to_string(maxRank)));
    for (auto& p : extraProperties) properties.push_back(std::move(p));
    input["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(input);
  }

  /// Find-or-create the caller's SkillRank row for a skill (rank 0 until
  /// bought).
  Json ensureSkillRank(std::string_view ownerUserId, std::string_view skillId,
                       std::string_view displayName = {}) {
    for (const auto& s : skills(ownerUserId)) {
      if (s.skillId == skillId) return gameModel_.container(appId_, s.containerId);
    }
    JVal input;
    input["appId"] = appId_;
    input["typeName"] = names_.skillRankType;
    input["displayName"] = displayName.empty() ? "Skill " + std::string(skillId) + " " +
                                                     std::string(ownerUserId)
                                               : std::string(displayName);
    input["properties"] =
        JVal::array({property("owner_user_id", "int", std::string(ownerUserId)),
                     property("skill_id", "string", JVal(skillId).dump())});
    return gameModel_.createContainer(input);
  }

  /// Buy one rank of a skill. prereqRankId is the caller's SkillRank for the
  /// prerequisite skill; omit it for skills without one (the rank row itself
  /// is passed to satisfy the required param — the condition ignores it when
  /// the def declares no prerequisite).
  KitInvokeResult buySkill(std::string_view skillRankId, std::string_view progressId,
                           std::string_view skillDefId, std::string_view prereqRankId = {}) {
    JVal params;
    params["progress_id"] = progressId;
    params["def_id"] = skillDefId;
    params["prereq_id"] = prereqRankId.empty() ? skillRankId : prereqRankId;
    return kitInvoke(gameModel_, appId_, names_.buySkillFn, skillRankId, params);
  }

  /// List a player's skill ranks.
  std::vector<KitSkillRank> skills(std::string_view ownerUserId) {
    Json containers = gameModel_.containers(appId_, names_.skillRankType);
    std::vector<KitSkillRank> out;
    containers.forEach([&](Json c) {
      if (c["ownerUserId"].asString() != ownerUserId) return;
      Json props = kitContainerProperties(gameModel_, appId_, c["containerId"].asStringView());
      KitSkillRank r;
      r.containerId = c["containerId"].asString();
      r.displayName = c["displayName"].asString();
      r.ownerUserId = c["ownerUserId"].asString();
      r.skillId = props["skill_id"].asString();
      r.rank = props["rank"].asInt64();
      out.push_back(std::move(r));
    });
    return out;
  }

  /// List the achievement catalog (admin-seeded AchievementDef containers).
  std::vector<KitAchievementDef> achievementCatalog() {
    Json containers = gameModel_.containers(appId_, names_.achievementDefType);
    std::vector<KitAchievementDef> out;
    containers.forEach([&](Json c) {
      Json props = kitContainerProperties(gameModel_, appId_, c["containerId"].asStringView());
      KitAchievementDef d;
      d.containerId = c["containerId"].asString();
      d.displayName = c["displayName"].asString();
      d.achievementId = props["achievement_id"].asString();
      d.threshold = props["threshold"].asInt64();
      out.push_back(std::move(d));
    });
    return out;
  }

  /// Define an achievement (admin).
  Json defineAchievement(std::string_view achievementId, std::int64_t threshold,
                         std::string_view displayName = {}) {
    JVal input;
    input["appId"] = appId_;
    input["typeName"] = names_.achievementDefType;
    input["displayName"] = displayName.empty() ? "Achievement " + std::string(achievementId)
                                               : std::string(displayName);
    input["properties"] =
        JVal::array({property("achievement_id", "string", JVal(achievementId).dump()),
                     property("threshold", "int", std::to_string(threshold))});
    return gameModel_.createContainer(input);
  }

  /// List a player's achievement unlocks.
  std::vector<KitAchievementUnlock> achievements(std::string_view ownerUserId) {
    Json containers = gameModel_.containers(appId_, names_.achievementUnlockType);
    std::vector<KitAchievementUnlock> out;
    containers.forEach([&](Json c) {
      if (c["ownerUserId"].asString() != ownerUserId) return;
      Json props = kitContainerProperties(gameModel_, appId_, c["containerId"].asStringView());
      KitAchievementUnlock u;
      u.containerId = c["containerId"].asString();
      u.displayName = c["displayName"].asString();
      u.ownerUserId = c["ownerUserId"].asString();
      u.achievementId = props["achievement_id"].asString();
      u.unlocked = props["unlocked"].asBool();
      out.push_back(std::move(u));
    });
    return out;
  }

  /// Unlock an achievement once its xp threshold is met. Creates the
  /// caller's unlock row when absent, then invokes the gated function
  /// (idempotent).
  KitInvokeResult unlockAchievement(std::string_view ownerUserId, std::string_view progressId,
                                    std::string_view achievementDefId,
                                    std::string_view achievementId) {
    std::string unlockId;
    for (const auto& a : achievements(ownerUserId)) {
      if (a.achievementId == achievementId) {
        unlockId = a.containerId;
        break;
      }
    }
    if (unlockId.empty()) {
      JVal input;
      input["appId"] = appId_;
      input["typeName"] = names_.achievementUnlockType;
      input["displayName"] =
          "Achievement " + std::string(achievementId) + " " + std::string(ownerUserId);
      input["properties"] =
          JVal::array({property("owner_user_id", "int", std::string(ownerUserId)),
                       property("achievement_id", "string", JVal(achievementId).dump())});
      Json created = gameModel_.createContainer(input);
      unlockId = created["containerId"].asString();
    }
    JVal params;
    params["progress_id"] = progressId;
    params["def_id"] = achievementDefId;
    return kitInvoke(gameModel_, appId_, names_.unlockAchievementFn, unlockId, params);
  }

  /// Apply a match result as a rating delta (trusted — host-gated by
  /// default; the match layer computes the delta). Returns the new rating.
  KitInvokeResult applyMatchResult(std::string_view progressId, std::int64_t delta) {
    JVal params;
    params["delta"] = delta;
    return kitInvoke(gameModel_, appId_, names_.adjustRatingFn, progressId, params);
  }

 private:
  static JVal property(const char* key, const char* valueType, std::string valueJson) {
    JVal p;
    p["key"] = key;
    p["valueType"] = valueType;
    p["valueJson"] = std::move(valueJson);
    return p;
  }

  std::string appId_;
  domains::GameModelAPI& gameModel_;
  ProgressionNames names_;
};

}  // namespace crowdy::kit
