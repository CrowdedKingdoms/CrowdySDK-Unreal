#pragma once

#include <algorithm>
#include <cstdlib>

#include <optional>
#include <vector>

#include "crowdy/kit/core.hpp"

/// Quests — an admin QuestDef catalog (objective count + reward spec + daily
/// flag) and per-player QuestProgress rows. Progress advances through trusted
/// calls or event automations on your gameplay functions; claim_reward marks
/// the progress claimed AND grants the item and currency rewards through
/// container_ref params in ONE transaction (composing with the inventory
/// stack and economy wallet types); a cron automation resets daily quests at
/// midnight. Mirrors CrowdyJS's questsBlueprint / kit.quests.
namespace crowdy::kit {

/// The gameplay event that advances a quest.
enum class QuestAdvanceEvent { FunctionInvoked, PropertyChanged, ContainerCreated };

inline const char* questAdvanceEventName(QuestAdvanceEvent e) {
  switch (e) {
    case QuestAdvanceEvent::FunctionInvoked: return "function_invoked";
    case QuestAdvanceEvent::PropertyChanged: return "property_changed";
    case QuestAdvanceEvent::ContainerCreated: return "container_created";
  }
  return "function_invoked";
}

/// An event-driven quest advance: bump matching QuestProgress rows on a model
/// event.
struct QuestAdvanceSpec {
  /// Automation name (unique per app), e.g. "advance-on-craft".
  std::string name;
  /// Only progress rows of this quest advance.
  std::string questId;
  /// The gameplay event that advances the quest (e.g. consume_stack invoked).
  QuestAdvanceEvent onEvent = QuestAdvanceEvent::FunctionInvoked;
  std::string functionName;       ///< empty = unset
  std::string containerTypeName;  ///< empty = unset
  std::string propertyKey;        ///< empty = unset
  /// property_changed only: which writes advance the quest — "direct",
  /// "function", or "any" (empty = unset, leaving the API default). Kit
  /// progress properties are written by functions (advance_quest, the daily
  /// reset) rather than by client setProperty calls, so watching one needs
  /// "function" or "any".
  std::string writeSource;
  std::optional<int> debounceMs;
  int amount = 1;      ///< progress added per event
  int maxTargets = 8;  ///< progress rows advanced per event
};

struct QuestsBlueprintOptions {
  /// Prefix for the type/function names. Defaults to none.
  std::string typePrefix;
  /// Who may advance quest progress. Defaults to server; use automation when
  /// driving progress purely through advanceOn event automations. Never
  /// plain players — they would complete their own quests.
  TrustedAuthority advanceAuthority = TrustedAuthority::server();
  /// The wallet currency property that claim_reward pays reward_gold into.
  /// Defaults to "gold" (compose with the economy blueprint's wallet).
  std::string currencyProperty = "gold";
  /// Cron for the daily reset automation. Defaults to UTC midnight.
  std::string dailyResetCron = "0 0 * * *";
  /// Event automations that advance quests from gameplay functions.
  std::vector<QuestAdvanceSpec> advanceOn;
  /// Owner-mirror typing (see the kit convention). Defaults to Int.
  OwnerIdKind ownerIdKind = OwnerIdKind::Int;
};

/// Names derived by questsBlueprint for a given prefix.
struct QuestsNames {
  std::string defType;
  std::string progressType;
  std::string advanceFn;
  std::string claimFn;
  std::string resetFn;
  std::string dailyResetAutomation;
};

/// Compute the type/function names a quests blueprint (and its runtime
/// helper) uses.
inline QuestsNames questsNames(std::string_view typePrefix = {}) {
  const std::string fnPrefix =
      typePrefix.empty() ? std::string() : toSnakeCase(typePrefix) + "_";
  std::string autoPrefix = fnPrefix;
  for (auto& c : autoPrefix) {
    if (c == '_') c = '-';
  }
  QuestsNames n;
  n.defType = std::string(typePrefix) + "QuestDef";
  n.progressType = std::string(typePrefix) + "QuestProgress";
  n.advanceFn = fnPrefix + "advance_quest";
  n.claimFn = fnPrefix + "claim_reward";
  n.resetFn = fnPrefix + "reset_daily";
  n.dailyResetAutomation = autoPrefix + "daily-quest-reset";
  return n;
}

/// Build the quests blueprint. Runtime counterpart: QuestsKit.
inline KitBlueprint questsBlueprint(const QuestsBlueprintOptions& options = {}) {
  const QuestsNames names = questsNames(options.typePrefix);
  const OwnerIdKind kind = options.ownerIdKind;

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
                        std::optional<std::string> defaultJson, std::string description) {
    JVal p;
    p["containerTypeName"] = type;
    p["key"] = key;
    p["valueType"] = valueType;
    if (defaultJson) p["defaultValueJson"] = *defaultJson;
    p["description"] = std::move(description);
    return p;
  };
  auto param = [](const char* name, const char* valueType, std::string description) {
    JVal p;
    p["name"] = name;
    p["valueType"] = valueType;
    p["required"] = true;
    p["description"] = std::move(description);
    return p;
  };
  auto mutation = [](std::string_view target, std::string property, std::string expression) {
    JVal m;
    m["target"] = target;
    m["property"] = std::move(property);
    m["expression"] = std::move(expression);
    return m;
  };
  auto whereClause = [](const char* key, const char* op, JVal value) {
    JVal w;
    w["key"] = key;
    w["op"] = op;
    w["value"] = std::move(value);
    return w;
  };

  KitBlueprint bp;
  bp.name = names.progressType;

  bp.containerTypes.push_back(containerType(
      names.defType, "admin", "Studio quest catalog row (objective, rewards, daily flag)."));
  bp.containerTypes.push_back(containerType(names.progressType, "member",
                                            "A player's progress toward one quest."));

  bp.propertyDefinitions.push_back(propertyDef(names.defType, "quest_id", "string", std::nullopt,
                                               "Stable quest identifier."));
  bp.propertyDefinitions.push_back(propertyDef(names.defType, "target_count", "int", "1",
                                               "Objective count required to complete."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.defType, "reward_item_id", "string", "\"\"",
                  "Item reward (empty for currency-only quests)."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.defType, "reward_qty", "int", "0", "Item reward quantity."));
  bp.propertyDefinitions.push_back(propertyDef(
      names.defType, "reward_gold", "int", "0",
      "Currency reward paid into the '" + options.currencyProperty + "' wallet property."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.defType, "repeatable", "bool", "false",
                  "Whether a player may accept the quest again after claiming."));
  bp.propertyDefinitions.push_back(propertyDef(names.defType, "daily", "bool", "false",
                                               "Whether progress resets on the daily cron."));
  bp.propertyDefinitions.push_back(ownerMirrorProperty(names.progressType, kind));
  bp.propertyDefinitions.push_back(propertyDef(names.progressType, "quest_id", "string", "\"\"",
                                               "The quest this row tracks."));
  bp.propertyDefinitions.push_back(propertyDef(names.progressType, "count", "int", "0",
                                               "Objective progress, clamped to target."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.progressType, "target", "int", "1",
                  "Objective count copied from the def at accept time."));
  bp.propertyDefinitions.push_back(propertyDef(names.progressType, "completed", "bool", "false",
                                               "True once count reaches target."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.progressType, "claimed", "bool", "false",
                  "True once the reward was granted (single-claim guard)."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.progressType, "daily", "bool", "false",
                  "Mirrors the def; selects rows for the daily reset automation."));

  {
    JVal fn;
    fn["name"] = names.advanceFn;
    fn["containerTypeName"] = names.progressType;
    fn["returnType"] = "int";
    fn["parameters"] = JVal::array({param(
        "amount", "int", "Progress to add (negative values are ignored; clamped to target).")});
    fn["mutations"] = JVal::array(
        {mutation("self", "count", "min(self.target, self.count + max(0, $amount))"),
         mutation("self", "completed", "self.count >= self.target")});
    fn["returnExpression"] = "self.count";
    applyTrustedAuthority(fn, options.advanceAuthority);
    // Event automations must be able to run it regardless of authority.
    if (!options.advanceOn.empty()) fn["autonomousInvocable"] = true;
    fn["description"] =
        "Trusted quest progress bump (clamped to the target); completion flips server-side in "
        "the same transaction.";
    bp.functions.push_back(std::move(fn));
  }
  {
    const std::string claimCondition =
        ownerEqualsCaller("self.owner_user_id", kind) +
        " && self.count >= self.target && not(self.claimed) && "
        "ref($def_id).quest_id == self.quest_id && " +
        ownerEqualsCaller("ref($wallet_id).owner_user_id", kind) + " && " +
        ownerEqualsCaller("ref($to_stack_id).owner_user_id", kind);
    JVal fn;
    fn["name"] = names.claimFn;
    fn["containerTypeName"] = names.progressType;
    fn["returnType"] = "int";
    fn["parameters"] = JVal::array(
        {param("def_id", "container_ref",
               "The QuestDef holding the reward spec (must match quest_id)."),
         param("to_stack_id", "container_ref",
               "A caller-owned stack receiving the item reward (item grant is 0 when the item "
               "id does not match, e.g. currency-only quests)."),
         param("wallet_id", "container_ref",
               "The caller's wallet receiving the currency reward.")});
    fn["mutations"] = JVal::array(
        {mutation("self", "claimed", "true"),
         mutation("ref($to_stack_id)", "quantity",
                  "ref($to_stack_id).quantity + if(ref($to_stack_id).item_id == "
                  "ref($def_id).reward_item_id, ref($def_id).reward_qty, 0)"),
         mutation("ref($wallet_id)", options.currencyProperty,
                  "ref($wallet_id)." + options.currencyProperty + " + ref($def_id).reward_gold")});
    fn["returnExpression"] = "ref($wallet_id)." + options.currencyProperty;
    fn["invokePolicyJson"] = kitPolicyJson(conditionPolicy(claimCondition));
    fn["description"] =
        "Turn in a completed quest: the claimed flag, item grant, and currency grant commit "
        "atomically — no double-claims, no client-chosen rewards.";
    bp.functions.push_back(std::move(fn));
  }
  {
    JVal fn;
    fn["name"] = names.resetFn;
    fn["containerTypeName"] = names.progressType;
    fn["returnType"] = "bool";
    fn["mutations"] = JVal::array({mutation("self", "count", "0"),
                                   mutation("self", "completed", "false"),
                                   mutation("self", "claimed", "false")});
    fn["returnExpression"] = "self.completed";
    fn["invokePolicyJson"] = kitPolicyJson(isAutomationPolicy());
    fn["autonomousInvocable"] = true;
    fn["description"] = "Server-driven daily quest reset (automation-only).";
    bp.functions.push_back(std::move(fn));
  }

  {
    JVal selector;
    selector["selfWhere"] = JVal::array({whereClause("daily", "==", true)});
    JVal a;
    a["name"] = names.dailyResetAutomation;
    a["functionName"] = names.resetFn;
    a["targetMode"] = "type";
    a["targetTypeName"] = names.progressType;
    a["triggerType"] = "schedule";
    a["scheduleKind"] = "cron";
    a["cronExpr"] = options.dailyResetCron;
    a["maxTargets"] = 500;
    a["selectorJson"] = selector.dump();
    a["description"] = "Resets daily quest progress on the cron schedule.";
    bp.automations.push_back(std::move(a));
  }
  for (const QuestAdvanceSpec& spec : options.advanceOn) {
    JVal selector;
    selector["selfWhere"] = JVal::array({whereClause("quest_id", "==", spec.questId),
                                         whereClause("completed", "==", false)});
    JVal params;
    params["amount"] = spec.amount;
    JVal a;
    a["name"] = spec.name;
    a["functionName"] = names.advanceFn;
    a["targetMode"] = "type";
    a["targetTypeName"] = names.progressType;
    a["triggerType"] = "event";
    a["maxTargets"] = spec.maxTargets;
    a["selectorJson"] = selector.dump();
    a["paramsJson"] = params.dump();
    a["description"] =
        "Advances '" + spec.questId + "' progress when the gameplay event fires.";
    bp.automations.push_back(std::move(a));

    JVal t;
    t["automationName"] = spec.name;
    t["onEvent"] = questAdvanceEventName(spec.onEvent);
    if (!spec.functionName.empty()) t["functionName"] = spec.functionName;
    if (!spec.containerTypeName.empty()) t["containerTypeName"] = spec.containerTypeName;
    if (!spec.propertyKey.empty()) t["propertyKey"] = spec.propertyKey;
    if (!spec.writeSource.empty()) t["writeSource"] = spec.writeSource;
    if (spec.debounceMs) t["debounceMs"] = *spec.debounceMs;
    bp.automationTriggers.push_back(std::move(t));
  }

  return bp;
}

/// A parsed view of one quest catalog row.
struct KitQuestDef {
  std::string containerId;
  std::string displayName;
  std::string questId;
  std::int64_t targetCount = 1;
  std::string rewardItemId;
  std::int64_t rewardQty = 0;
  std::int64_t rewardGold = 0;
  bool repeatable = false;
  bool daily = false;
};

/// A parsed view of one player's quest progress.
struct KitQuestProgress {
  std::string containerId;
  std::string displayName;
  std::string ownerUserId;  ///< empty when unowned
  std::string questId;
  std::int64_t count = 0;
  std::int64_t target = 1;
  bool completed = false;
  bool claimed = false;
  bool daily = false;
};

/// Input for QuestsKit::defineQuest (admin).
struct KitQuestDefinition {
  std::string questId;
  std::int64_t targetCount = 1;
  std::string rewardItemId;
  std::int64_t rewardQty = 0;
  std::int64_t rewardGold = 0;
  bool repeatable = false;
  bool daily = false;
  std::string displayName;  ///< defaults to "Quest <questId>"
  JArray properties;        ///< extra SeedPropertyInput objects
};

/// Runtime helpers for the questsBlueprint conventions: browse the quest
/// catalog, accept quests into per-player progress rows, advance them
/// (trusted — app admins or event automations), and claim rewards atomically
/// into a stack + wallet. Denials resolve with success=false.
class QuestsKit {
 public:
  QuestsKit(std::string appId, domains::GameModelAPI& gameModel,
            std::string_view typePrefix = {})
      : appId_(std::move(appId)), gameModel_(gameModel), names_(questsNames(typePrefix)) {}

  const QuestsNames& names() const { return names_; }

  // -- FTUE / tutorial step sequencing (Wave 2) -----------------------------

  /// One tutorial step joined with the player's progress.
  struct TutorialStep {
    int stepIndex = 0;
    KitQuestDef def;
    std::optional<KitQuestProgress> progress;
    std::string status;  ///< "locked" | "active" | "complete"
  };

  /// STUDIO (admin) — define an ordered tutorial chain as quest defs whose
  /// questId encodes chain + index ("<chain>:<i>"). Pure convention — the
  /// sequencing is enforced by tutorial()/acceptNextTutorialStep().
  std::vector<Json> defineTutorial(std::vector<KitQuestDefinition> steps,
                                   std::string_view chain = "ftue") {
    std::vector<Json> created;
    int index = 0;
    for (auto& step : steps) {
      step.questId = std::string(chain) + ":" + std::to_string(index);
      created.push_back(defineQuest(step));
      ++index;
    }
    return created;
  }

  /// A player's tutorial view: ordered steps, each locked (an earlier step
  /// is incomplete), active (the first incomplete), or complete.
  std::vector<TutorialStep> tutorial(std::string_view ownerUserId,
                                     std::string_view chain = "ftue") {
    const std::string prefix = std::string(chain) + ":";
    std::vector<TutorialStep> steps;
    for (const auto& def : catalog()) {
      if (def.questId.rfind(prefix, 0) != 0) continue;
      TutorialStep step;
      step.stepIndex = std::atoi(def.questId.c_str() + prefix.size());
      step.def = def;
      steps.push_back(std::move(step));
    }
    std::sort(steps.begin(), steps.end(),
              [](const TutorialStep& a, const TutorialStep& b) { return a.stepIndex < b.stepIndex; });
    auto progress = mine(ownerUserId);
    bool blocked = false;
    for (auto& step : steps) {
      for (const auto& row : progress) {
        if (row.questId == step.def.questId) step.progress = row;
      }
      const bool complete = step.progress && step.progress->completed;
      step.status = complete ? "complete" : (blocked ? "locked" : "active");
      if (!complete) blocked = true;
    }
    return steps;
  }

  /// Ensure the player's ACTIVE tutorial step has a progress row (accepting
  /// it when missing) and return it; nullopt when the chain is complete.
  std::optional<TutorialStep> acceptNextTutorialStep(std::string_view ownerUserId,
                                                     std::string_view chain = "ftue",
                                                     std::string_view sessionId = {}) {
    auto steps = tutorial(ownerUserId, chain);
    for (const auto& step : steps) {
      if (step.status != "active") continue;
      if (!step.progress) {
        accept(ownerUserId, step.def.containerId, {}, sessionId);
        auto refreshed = tutorial(ownerUserId, chain);
        for (const auto& again : refreshed) {
          if (again.stepIndex == step.stepIndex) return again;
        }
      }
      return step;
    }
    return std::nullopt;
  }


  /// List the quest catalog (admin-seeded QuestDef containers).
  std::vector<KitQuestDef> catalog() {
    Json containers = gameModel_.containers(appId_, names_.defType);
    std::vector<KitQuestDef> out;
    containers.forEach([&](Json c) {
      Json props = kitContainerProperties(gameModel_, appId_, c["containerId"].asStringView());
      KitQuestDef d;
      d.containerId = c["containerId"].asString();
      d.displayName = c["displayName"].asString();
      d.questId = props["quest_id"].asString();
      d.targetCount = props["target_count"].asInt64(1);
      d.rewardItemId = props["reward_item_id"].asString();
      d.rewardQty = props["reward_qty"].asInt64();
      d.rewardGold = props["reward_gold"].asInt64();
      d.repeatable = props["repeatable"].asBool();
      d.daily = props["daily"].asBool();
      out.push_back(std::move(d));
    });
    return out;
  }

  /// Define a quest (admin — the catalog type is admin-instantiable).
  Json defineQuest(const KitQuestDefinition& input) {
    JVal vars;
    vars["appId"] = appId_;
    vars["typeName"] = names_.defType;
    vars["displayName"] =
        input.displayName.empty() ? "Quest " + input.questId : input.displayName;
    JArray properties;
    properties.push_back(property("quest_id", "string", JVal(input.questId).dump()));
    properties.push_back(property("target_count", "int", std::to_string(input.targetCount)));
    properties.push_back(
        property("reward_item_id", "string", JVal(input.rewardItemId).dump()));
    properties.push_back(property("reward_qty", "int", std::to_string(input.rewardQty)));
    properties.push_back(property("reward_gold", "int", std::to_string(input.rewardGold)));
    properties.push_back(property("repeatable", "bool", input.repeatable ? "true" : "false"));
    properties.push_back(property("daily", "bool", input.daily ? "true" : "false"));
    for (const auto& p : input.properties) properties.push_back(p);
    vars["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(vars);
  }

  /// Accept a quest: creates the caller's progress row seeded from the def
  /// (target and daily flag copied at accept time).
  Json accept(std::string_view ownerUserId, std::string_view questDefId,
              std::string_view displayName = {}, std::string_view sessionId = {}) {
    Json props = kitContainerProperties(gameModel_, appId_, questDefId);
    const std::string questId = props["quest_id"].asString();
    JVal vars;
    vars["appId"] = appId_;
    vars["typeName"] = names_.progressType;
    vars["displayName"] = displayName.empty()
                              ? "Quest " + questId + " " + std::string(ownerUserId)
                              : std::string(displayName);
    if (!sessionId.empty()) vars["sessionId"] = sessionId;
    JArray properties;
    properties.push_back(property("owner_user_id", "int", std::string(ownerUserId)));
    properties.push_back(property("quest_id", "string", JVal(questId).dump()));
    properties.push_back(
        property("target", "int", std::to_string(props["target_count"].asInt64(1))));
    properties.push_back(property("daily", "bool", props["daily"].asBool() ? "true" : "false"));
    vars["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(vars);
  }

  /// List a player's quest progress rows.
  std::vector<KitQuestProgress> mine(std::string_view ownerUserId) {
    Json containers = gameModel_.containers(appId_, names_.progressType);
    std::vector<KitQuestProgress> out;
    containers.forEach([&](Json c) {
      if (c["ownerUserId"].asString() != ownerUserId) return;
      out.push_back(state(c["containerId"].asStringView()));
    });
    return out;
  }

  /// Read one progress row.
  KitQuestProgress state(std::string_view progressId) {
    Json container = gameModel_.container(appId_, progressId);
    Json props = kitContainerProperties(gameModel_, appId_, progressId);
    KitQuestProgress p;
    p.containerId = container["containerId"].asString();
    p.displayName = container["displayName"].asString();
    p.ownerUserId = container["ownerUserId"].asString();
    p.questId = props["quest_id"].asString();
    p.count = props["count"].asInt64();
    p.target = props["target"].asInt64(1);
    p.completed = props["completed"].asBool();
    p.claimed = props["claimed"].asBool();
    p.daily = props["daily"].asBool();
    return p;
  }

  /// Advance quest progress — a trusted call (default blueprint authority:
  /// app admins; or wire advanceOn event automations). Returns the new count.
  KitInvokeResult advance(std::string_view progressId, std::int64_t amount = 1) {
    JVal params;
    params["amount"] = amount;
    return kitInvoke(gameModel_, appId_, names_.advanceFn, progressId, params);
  }

  /// Turn in a completed quest: marks it claimed AND grants the item +
  /// currency rewards in one transaction. Returns the wallet's new balance.
  KitInvokeResult claim(std::string_view progressId, std::string_view questDefId,
                        std::string_view toStackId, std::string_view walletId) {
    JVal params;
    params["def_id"] = questDefId;
    params["to_stack_id"] = toStackId;
    params["wallet_id"] = walletId;
    return kitInvoke(gameModel_, appId_, names_.claimFn, progressId, params);
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
  QuestsNames names_;
};

}  // namespace crowdy::kit
