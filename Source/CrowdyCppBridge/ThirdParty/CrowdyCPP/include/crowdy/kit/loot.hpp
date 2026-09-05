#pragma once

#include <charconv>
#include <optional>
#include <unordered_set>

#include "crowdy/kit/core.hpp"
#include "crowdy/kit/engine.hpp"

/// Loot — loot tables & drops: weighted tables are unrolled into pure
/// expressions at blueprint-build time (the expression language is
/// loop-free), rolled server-side into durable LootRoll containers, and
/// claimed atomically into an item stack. Covers chest rolls, mob drops, and
/// gacha pulls. Flow: create a LootRoll (member) for a table ->
/// roll_<table> (trusted: server scope by default, or an event automation
/// via drops) stores a single rand() seed and resolves item + quantity from
/// it in one transaction -> the owner claims with claim_roll, which marks
/// the roll claimed AND grants the stack in the same invoke (no
/// double-claim, no client-chosen rewards). Mirrors CrowdyJS's
/// lootBlueprint / kit.loot.
namespace crowdy::kit {

/// One weighted entry of a loot table.
struct LootEntrySpec {
  std::string itemId;
  /// Relative weight (> 0); probabilities are weights normalized per table.
  double weight = 0;
  /// Minimum quantity granted.
  int minQty = 1;
  /// Maximum quantity granted. Defaults to minQty.
  std::optional<int> maxQty;
};

/// One loot table, unrolled into a single expression at blueprint-build time.
struct LootTableSpec {
  /// Stable table identifier; also derives the roll function name.
  std::string tableId;
  /// 1-16 weighted entries (the expression parser caps chain length).
  std::vector<LootEntrySpec> entries;
};

/// An event-triggered drop: rolls a pooled LootRoll when a model event fires.
struct LootDropSpec {
  /// Automation name (unique per app), e.g. "goblin-drop".
  std::string name;
  /// The table to roll.
  std::string tableId;
  /// The model event that triggers the drop (e.g. a mob_died function):
  /// "function_invoked", "property_changed", or "container_created".
  std::string onEvent;
  std::string functionName;       ///< Optional (empty = unset).
  std::string containerTypeName;  ///< Optional (empty = unset).
  std::string propertyKey;        ///< Optional (empty = unset).
  /// property_changed only: which writes roll the drop — "direct", "function",
  /// or "any" (empty = unset, leaving the API default). Properties mutated by
  /// kit functions (a mob's hp reaching 0, say) need "function" or "any"; the
  /// default "direct" sees only client setProperty calls.
  std::string writeSource;
  std::optional<std::int64_t> debounceMs;
  /// Rolls per event.
  int maxTargets = 1;
};

/// Options for lootBlueprint.
struct LootBlueprintOptions {
  /// Prefix for the type/function names.
  std::string typePrefix;
  /// The loot tables; entries are unrolled into expressions at build time.
  std::vector<LootTableSpec> tables;
  /// Who may roll. Defaults to Server (app admins / studio backend);
  /// automations configured via drops may always roll (trusted automations
  /// bypass invoke policies, and the roll functions are marked
  /// autonomousInvocable when drops reference their table).
  TrustedAuthority rollAuthority = TrustedAuthority::server();
  /// Event-triggered drops: each rolls an UNROLLED pooled LootRoll container
  /// of its table when the event fires (automations mutate — they cannot
  /// create containers — so keep a small pool of pre-created rolls per
  /// table).
  std::vector<LootDropSpec> drops;
  /// Owner-mirror typing (see the kit convention).
  OwnerIdKind ownerIdKind = OwnerIdKind::Int;
};

/// Names derived by lootBlueprint for a given prefix.
struct LootNames {
  std::string rollType;
  std::string claimFn;
  std::string fnPrefix;  ///< Snake-cased function-name prefix (empty without a typePrefix).
};

/// Compute the type/function names a loot blueprint (and its runtime helper)
/// uses.
inline LootNames lootNames(std::string_view typePrefix = {}) {
  const std::string fnPrefix =
      typePrefix.empty() ? std::string() : toSnakeCase(typePrefix) + "_";
  LootNames n;
  n.rollType = std::string(typePrefix) + "LootRoll";
  n.claimFn = fnPrefix + "claim_roll";
  n.fnPrefix = fnPrefix;
  return n;
}

/// The roll function name for one table.
inline std::string lootRollFn(std::string_view tableId, std::string_view typePrefix = {}) {
  const std::string fnPrefix =
      typePrefix.empty() ? std::string() : toSnakeCase(typePrefix) + "_";
  return fnPrefix + "roll_" + toSnakeCase(tableId);
}

namespace detail {

/// Format a probability threshold as an expression float literal, matching
/// the JS String(number) rendering the CrowdyJS builder emits (shortest
/// round-trip digits; fixed notation in the normal range; a trailing ".0"
/// when the result carries no fraction marker).
inline std::string lootFloatLiteral(double n) {
  char buf[64];
  std::to_chars_result res{};
  if (n >= 1e-6 && n < 1e21) {
    res = std::to_chars(buf, buf + sizeof(buf), n, std::chars_format::fixed);
  } else {
    res = std::to_chars(buf, buf + sizeof(buf), n, std::chars_format::scientific);
  }
  std::string s(buf, res.ptr);
  const std::size_t e = s.find('e');
  if (e != std::string::npos) {
    // JS renders exponents without zero-padding ("1e-7", not "1e-07").
    std::size_t d = e + 1;
    if (d < s.size() && (s[d] == '+' || s[d] == '-')) ++d;
    while (d + 1 < s.size() && s[d] == '0') s.erase(d, 1);
  }
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos) s += ".0";
  return s;
}

/// The quantity expression for one entry: a rand_int range, or the fixed
/// minimum when no range is declared.
inline std::string lootQtyExpr(const LootEntrySpec& e) {
  const int min = e.minQty;
  const int max = e.maxQty.value_or(min);
  return max > min ? "rand_int(" + std::to_string(min) + ", " + std::to_string(max) + ")"
                   : std::to_string(min);
}

/// Unroll a weighted table into a nested-if chain over the stored roll seed:
/// if(self.seed < t1, "a", if(self.seed < t2, "b", "c")) with cumulative
/// normalized thresholds. Expressions are loop-free, so the selection chain
/// is generated at BUILD time — one rand() is stored first and every branch
/// reads that same seed, keeping the distribution exact.
inline std::string unrollLootItemChain(const std::vector<LootEntrySpec>& entries) {
  double total = 0;
  for (const auto& e : entries) total += e.weight;
  std::string expr = JVal(entries.back().itemId).dump();
  for (std::size_t i = entries.size() - 1; i-- > 0;) {
    // Threshold below which entries[0..i] win; nested back-to-front.
    double cumulative = 0;
    for (std::size_t j = 0; j <= i; ++j) cumulative += entries[j].weight;
    expr = "if(self.seed < " + lootFloatLiteral(cumulative / total) + ", " +
           JVal(entries[i].itemId).dump() + ", " + expr + ")";
  }
  return expr;
}

/// Unroll the per-item quantity chain, keyed on the already-rolled item id.
inline std::string unrollLootQtyChain(const std::vector<LootEntrySpec>& entries) {
  std::string expr = lootQtyExpr(entries.back());
  for (std::size_t i = entries.size() - 1; i-- > 0;) {
    expr = "if(self.rolled_item_id == " + JVal(entries[i].itemId).dump() + ", " +
           lootQtyExpr(entries[i]) + ", " + expr + ")";
  }
  return expr;
}

}  // namespace detail

/// Build the loot blueprint. Runtime counterpart: LootKit.
inline KitBlueprint lootBlueprint(const LootBlueprintOptions& options) {
  if (options.tables.empty()) {
    throw std::invalid_argument("lootBlueprint requires at least one table");
  }
  for (const auto& table : options.tables) {
    if (table.entries.size() < 1 || table.entries.size() > 16) {
      throw std::invalid_argument("lootBlueprint table '" + table.tableId +
                                  "' must have 1-16 entries (parser-limit-aware unrolling)");
    }
    for (const auto& e : table.entries) {
      if (!(e.weight > 0)) {
        throw std::invalid_argument("lootBlueprint table '" + table.tableId +
                                    "' entries must have weight > 0");
      }
    }
  }
  std::unordered_set<std::string> tableIds;
  for (const auto& table : options.tables) tableIds.insert(table.tableId);
  for (const auto& drop : options.drops) {
    if (!tableIds.contains(drop.tableId)) {
      throw std::invalid_argument("lootBlueprint drop '" + drop.name +
                                  "' references unknown table '" + drop.tableId + "'");
    }
  }
  const LootNames names = lootNames(options.typePrefix);
  const OwnerIdKind kind = options.ownerIdKind;
  std::unordered_set<std::string> droppedTables;
  for (const auto& drop : options.drops) droppedTables.insert(drop.tableId);

  auto propertyDef = [&names](const char* key, const char* valueType, std::string defaultJson,
                              const char* description) {
    JVal p;
    p["containerTypeName"] = names.rollType;
    p["key"] = key;
    p["valueType"] = valueType;
    p["defaultValueJson"] = std::move(defaultJson);
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
  bp.name = names.rollType;

  {
    JVal t;
    t["typeName"] = names.rollType;
    t["displayName"] = names.rollType;
    t["instantiableBy"] = "member";
    t["description"] =
        "One durable loot roll: seeded, resolved, and claimed entirely server-side.";
    bp.containerTypes.push_back(std::move(t));
  }

  bp.propertyDefinitions.push_back(ownerMirrorProperty(names.rollType, kind));
  bp.propertyDefinitions.push_back(
      propertyDef("table_id", "string", "\"\"", "The loot table this roll draws from."));
  bp.propertyDefinitions.push_back(propertyDef(
      "seed", "float", "0",
      "The stored rand() seed the weighted selection reads (audit-friendly)."));
  bp.propertyDefinitions.push_back(
      propertyDef("rolled_item_id", "string", "\"\"",
                  "The rolled item (empty until rolled; a roll cannot re-roll)."));
  bp.propertyDefinitions.push_back(propertyDef("rolled_qty", "int", "0", "The rolled quantity."));
  bp.propertyDefinitions.push_back(propertyDef(
      "claimed", "bool", "false", "True once claimed into a stack (single-claim guard)."));

  for (const auto& table : options.tables) {
    JVal fn;
    fn["name"] = lootRollFn(table.tableId, options.typePrefix);
    fn["containerTypeName"] = names.rollType;
    fn["returnType"] = "string";
    fn["mutations"] = JVal::array(
        {mutation("self", "seed", "rand()"),
         mutation("self", "rolled_item_id", detail::unrollLootItemChain(table.entries)),
         mutation("self", "rolled_qty", detail::unrollLootQtyChain(table.entries))});
    fn["returnExpression"] = "self.rolled_item_id";
    applyTrustedAuthority(fn, options.rollAuthority,
                          "self.table_id == " + JVal(table.tableId).dump() +
                              " && self.rolled_item_id == \"\"");
    if (droppedTables.contains(table.tableId)) fn["autonomousInvocable"] = true;
    fn["description"] = "Roll the '" + table.tableId +
                        "' loot table: stores one rand() seed, then resolves item and quantity "
                        "from it in one transaction (unrolled weighted selection).";
    bp.functions.push_back(std::move(fn));
  }

  {
    JVal fn;
    fn["name"] = names.claimFn;
    fn["containerTypeName"] = names.rollType;
    fn["returnType"] = "int";
    {
      JVal p;
      p["name"] = "to_stack_id";
      p["valueType"] = "container_ref";
      p["required"] = true;
      p["description"] =
          "A caller-owned stack of the rolled item that receives the quantity.";
      fn["parameters"] = JVal::array({std::move(p)});
    }
    fn["mutations"] = JVal::array(
        {mutation("self", "claimed", "true"),
         mutation("ref($to_stack_id)", "quantity",
                  "ref($to_stack_id).quantity + self.rolled_qty")});
    fn["returnExpression"] = "self.rolled_qty";
    fn["invokePolicyJson"] = kitPolicyJson(conditionPolicy(joinAnd(
        {ownerEqualsCaller("self.owner_user_id", kind), "not(self.claimed)",
         "self.rolled_item_id != \"\"", "ref($to_stack_id).item_id == self.rolled_item_id",
         ownerEqualsCaller("ref($to_stack_id).owner_user_id", kind)})));
    fn["description"] =
        "Claim a rolled loot into a stack: the claimed flag and the grant commit atomically, so "
        "a roll can never be claimed twice.";
    bp.functions.push_back(std::move(fn));
  }

  for (const auto& drop : options.drops) {
    JVal automation;
    automation["name"] = drop.name;
    automation["functionName"] = lootRollFn(drop.tableId, options.typePrefix);
    automation["targetMode"] = "type";
    automation["targetTypeName"] = names.rollType;
    automation["triggerType"] = "event";
    automation["maxTargets"] = drop.maxTargets;
    {
      JVal whereTable;
      whereTable["key"] = "table_id";
      whereTable["op"] = "==";
      whereTable["value"] = drop.tableId;
      JVal whereUnrolled;
      whereUnrolled["key"] = "rolled_item_id";
      whereUnrolled["op"] = "==";
      whereUnrolled["value"] = "";
      JVal selector;
      selector["selfWhere"] = JVal::array({std::move(whereTable), std::move(whereUnrolled)});
      selector["pick"] = "random";
      automation["selectorJson"] = selector.dump();
    }
    automation["description"] = "Event-triggered '" + drop.tableId +
                                "' drop: rolls a pooled unrolled LootRoll when the event fires.";
    bp.automations.push_back(std::move(automation));

    JVal trigger;
    trigger["automationName"] = drop.name;
    trigger["onEvent"] = drop.onEvent;
    if (!drop.functionName.empty()) trigger["functionName"] = drop.functionName;
    if (!drop.containerTypeName.empty()) trigger["containerTypeName"] = drop.containerTypeName;
    if (!drop.propertyKey.empty()) trigger["propertyKey"] = drop.propertyKey;
    if (!drop.writeSource.empty()) trigger["writeSource"] = drop.writeSource;
    if (drop.debounceMs) trigger["debounceMs"] = *drop.debounceMs;
    bp.automationTriggers.push_back(std::move(trigger));
  }

  return bp;
}

// ---------------------------------------------------------------------------
// Runtime kit
// ---------------------------------------------------------------------------

/// A parsed view of one loot roll.
struct KitLootRoll {
  std::string containerId;
  std::string displayName;
  std::string ownerUserId;
  std::string tableId;
  std::string rolledItemId;  ///< Empty until rolled.
  std::int64_t rolledQty = 0;
  bool claimed = false;
};

/// Runtime helpers for the lootBlueprint conventions: create durable
/// LootRoll containers, roll them (trusted — app admins by default, or the
/// blueprint's event-drop automations), and claim results atomically into an
/// item stack. The weighted selection runs entirely server-side from a
/// stored seed, so clients can neither pick their loot nor claim it twice.
class LootKit {
 public:
  LootKit(std::string appId, domains::GameModelAPI& gameModel, std::string_view typePrefix = {},
          EngineDetector* engines = nullptr, std::string_view engineModuleName = {})
      : appId_(std::move(appId)),
        gameModel_(gameModel),
        typePrefix_(typePrefix),
        names_(lootNames(typePrefix)),
        engines_(engines),
        engineModuleName_(engineModuleName.empty() ? std::string("loot-engine")
                                                   : std::string(engineModuleName)) {}

  const LootNames& names() const { return names_; }

  // -- Engine path (Wave 3): big-table pity rolls with audit trails ---------

  /// Is a big-table loot module deployed + enabled (cached)? Small weighted
  /// tables stay on the model (the coexistence policy).
  bool engineAvailable() { return engines_ != nullptr && engines_->has(engineModuleName_); }

  /// Pull from the module's table (pity-timer path); count caps at 10.
  Json enginePull(std::int64_t count = 1) {
    JVal params;
    params["count"] = count;
    return engineInvoke("pull", params);
  }

  /// Your pity counters + roll totals from the module.
  Json enginePity() { return engineInvoke("pity", JVal()); }

  /// Your rolling audit trail from the module (dispute resolution).
  Json engineAudit() { return engineInvoke("audit", JVal()); }

  /// Create an unrolled LootRoll for a table, owned by ownerUserId (who will
  /// claim it). Event-drop automations pick from the pool of unrolled rolls
  /// — create a few ahead of time for tables wired to drops.
  Json createRoll(std::string_view ownerUserId, std::string_view tableId,
                  std::string_view displayName = {}, std::string_view sessionId = {}) {
    JVal input;
    input["appId"] = appId_;
    input["typeName"] = names_.rollType;
    input["displayName"] = displayName.empty() ? "Roll " + std::string(tableId)
                                               : std::string(displayName);
    if (!sessionId.empty()) input["sessionId"] = sessionId;
    input["properties"] =
        JVal::array({property("owner_user_id", "int", std::string(ownerUserId)),
                     property("table_id", "string", JVal(tableId).dump())});
    return gameModel_.createContainer(input);
  }

  /// Roll a table on an unrolled LootRoll — a TRUSTED call (default
  /// blueprint authority: app admins). Returns the rolled item id. tableId
  /// derives the function name; it is read from the container when empty.
  KitInvokeResult roll(std::string_view rollId, std::string_view tableId = {}) {
    const std::string table =
        tableId.empty() ? state(rollId).tableId : std::string(tableId);
    return kitInvoke(gameModel_, appId_, lootRollFn(table, typePrefix_), rollId);
  }

  /// Claim a rolled loot into a caller-owned stack of the rolled item. The
  /// claimed flag and the grant commit atomically. Returns the granted
  /// quantity.
  KitInvokeResult claim(std::string_view rollId, std::string_view toStackId) {
    JVal params;
    params["to_stack_id"] = toStackId;
    return kitInvoke(gameModel_, appId_, names_.claimFn, rollId, params);
  }

  /// Read one roll's state.
  KitLootRoll state(std::string_view rollId) {
    Json container = gameModel_.container(appId_, rollId);
    return toRoll(container["containerId"].asString(), container["displayName"].asString(),
                  container["ownerUserId"].asString());
  }

  /// List a player's rolls, optionally filtered to one table and/or to
  /// unclaimed rolled loot (the "your drops" screen).
  std::vector<KitLootRoll> rolls(std::string_view ownerUserId, std::string_view tableId = {},
                                 bool unclaimedOnly = false) {
    Json containers = gameModel_.containers(appId_, names_.rollType);
    std::vector<KitLootRoll> out;
    containers.forEach([&](Json c) {
      if (c["ownerUserId"].asString() != ownerUserId) return;
      KitLootRoll r = toRoll(c["containerId"].asString(), c["displayName"].asString(),
                             c["ownerUserId"].asString());
      if (!tableId.empty() && r.tableId != tableId) return;
      if (unclaimedOnly && (r.claimed || r.rolledItemId.empty())) return;
      out.push_back(std::move(r));
    });
    return out;
  }

  /// Roll/claim audit history from the model event log (each roll records
  /// its seed, item, and quantity as applied mutations). Filters to one
  /// table's roll function when tableId is set, else to claim_roll.
  Json history(std::string_view tableId = {}, int limit = 0) {
    JVal vars;
    vars["appId"] = appId_;
    vars["functionName"] =
        tableId.empty() ? names_.claimFn : lootRollFn(tableId, typePrefix_);
    if (limit > 0) vars["limit"] = std::int64_t{limit};
    return gameModel_.events(vars);
  }

 private:
  static JVal property(const char* key, const char* valueType, std::string valueJson) {
    JVal p;
    p["key"] = key;
    p["valueType"] = valueType;
    p["valueJson"] = std::move(valueJson);
    return p;
  }

  KitLootRoll toRoll(std::string containerId, std::string displayName,
                     std::string ownerUserId) {
    Json props = kitContainerProperties(gameModel_, appId_, containerId);
    KitLootRoll r;
    r.containerId = std::move(containerId);
    r.displayName = std::move(displayName);
    r.ownerUserId = std::move(ownerUserId);
    r.tableId = props["table_id"].asString();
    r.rolledItemId = props["rolled_item_id"].asString();
    r.rolledQty = props["rolled_qty"].asInt64();
    r.claimed = props["claimed"].asBool();
    return r;
  }

  Json engineInvoke(std::string_view exportName, const JVal& params) {
    if (engines_ == nullptr) {
      throw std::runtime_error("loot engine unavailable: compute domain not wired");
    }
    EngineInvokeResult result = engines_->invoke(engineModuleName_, exportName, params);
    if (!result.success) {
      throw std::runtime_error("loot." + std::string(exportName) + " failed: " + result.reason);
    }
    return result.body;
  }

  std::string appId_;
  domains::GameModelAPI& gameModel_;
  std::string typePrefix_;
  LootNames names_;
  EngineDetector* engines_ = nullptr;
  std::string engineModuleName_;
};

}  // namespace crowdy::kit
