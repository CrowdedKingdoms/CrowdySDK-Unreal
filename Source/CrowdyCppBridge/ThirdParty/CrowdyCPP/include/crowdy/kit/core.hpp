#pragma once

#include <cctype>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "crowdy/domains/game_model.hpp"
#include "crowdy/graphql/errors.hpp"

/// Game Kit core — blueprints and the deploy/invoke machinery, mirroring
/// CrowdyJS's kit/blueprints/core and kit/shared modules.
///
/// A blueprint is a self-contained declarative bundle of game-model
/// definitions (container types, property defs, policy-gated functions),
/// optional seed containers/edges, and optional automations that implement
/// one game concept. Blueprints are plain data (graphql::JVal trees) built by
/// the builder functions; deploy them with GameKitClient::deploy (requires
/// the app-admin manage_apps permission), then use the runtime kits with a
/// player token. Authorization is enforced server-side on every call.
namespace crowdy::kit {

using graphql::JArray;
using graphql::Json;
using graphql::JVal;

// ---------------------------------------------------------------------------
// Invoke policies (serialized to invokePolicyJson)
// ---------------------------------------------------------------------------

inline JVal policy(std::string_view type) {
  JVal p;
  p["type"] = type;
  return p;
}
inline JVal allowPolicy() { return policy("allow"); }
inline JVal ownerOfSelfPolicy() { return policy("owner_of_self"); }
inline JVal isHostPolicy() { return policy("is_host"); }
inline JVal isCurrentTurnPolicy() { return policy("is_current_turn"); }
inline JVal isParticipantPolicy() { return policy("is_participant"); }
inline JVal isAutomationPolicy() { return policy("is_automation"); }
inline JVal conditionPolicy(std::string_view expression) {
  JVal p = policy("condition");
  p["expression"] = expression;
  return p;
}
/// Monetization gate: the caller's access tier must hold `feature`.
inline JVal featureGate(std::string_view feature) {
  JVal p = policy("tier_feature");
  p["feature"] = feature;
  return p;
}
inline JVal gridPermissionPolicy(std::string_view key, const JVal& gridId = JVal()) {
  JVal p = policy("grid_permission");
  p["key"] = key;
  if (!gridId.isNull()) p["gridId"] = gridId;
  return p;
}
inline JVal groupPermissionPolicy(std::string_view groupId, std::string_view permission = {}) {
  JVal p = policy("group_permission");
  p["groupId"] = groupId;
  if (!permission.empty()) p["permission"] = permission;
  return p;
}
inline JVal andPolicy(JArray rules) {
  JVal p = policy("and");
  p["rules"] = JVal(std::move(rules));
  return p;
}
inline JVal orPolicy(JArray rules) {
  JVal p = policy("or");
  p["rules"] = JVal(std::move(rules));
  return p;
}
inline JVal notPolicy(JVal rule) {
  JVal p = policy("not");
  p["rule"] = std::move(rule);
  return p;
}

/// AND extra rules into a base policy (skipping nulls) — the composition
/// point for *policyExtra options.
inline JVal andPolicies(JVal base, std::vector<JVal> extra = {}) {
  JArray rules;
  rules.push_back(std::move(base));
  for (auto& e : extra) {
    if (!e.isNull()) rules.push_back(std::move(e));
  }
  if (rules.size() == 1) return rules.front();
  return andPolicy(std::move(rules));
}

/// Serialize a policy tree to the wire invokePolicyJson.
inline std::string kitPolicyJson(const JVal& p) { return p.dump(); }

// ---------------------------------------------------------------------------
// Authority helpers
// ---------------------------------------------------------------------------

/// Who may call a TRUSTED mutation (XP grants, score submits, loot rolls,
/// currency mints): reward-granting functions must never be plain player
/// calls (anti-cheat convention).
struct TrustedAuthority {
  enum class Kind { Server, Host, Automation, Owner, Custom } kind = Kind::Server;
  JVal custom;

  static TrustedAuthority server() { return {Kind::Server, {}}; }
  static TrustedAuthority host() { return {Kind::Host, {}}; }
  static TrustedAuthority automation() { return {Kind::Automation, {}}; }
  static TrustedAuthority owner() { return {Kind::Owner, {}}; }
  static TrustedAuthority customRule(JVal rule) { return {Kind::Custom, std::move(rule)}; }
};

/// Merge the function-level fields a TrustedAuthority compiles to into a
/// function definition (invokePolicyJson, invokeScope, autonomousInvocable).
/// extraCondition, when non-empty, is AND'ed into the policy.
inline void applyTrustedAuthority(JVal& fn, const TrustedAuthority& authority,
                                  std::string_view extraCondition = {}) {
  JVal rule;
  switch (authority.kind) {
    case TrustedAuthority::Kind::Server: rule = allowPolicy(); break;
    case TrustedAuthority::Kind::Host: rule = isHostPolicy(); break;
    case TrustedAuthority::Kind::Automation: rule = isAutomationPolicy(); break;
    case TrustedAuthority::Kind::Owner: rule = ownerOfSelfPolicy(); break;
    case TrustedAuthority::Kind::Custom: rule = authority.custom; break;
  }
  if (!extraCondition.empty())
    rule = andPolicy({std::move(rule), conditionPolicy(extraCondition)});
  fn["invokePolicyJson"] = kitPolicyJson(rule);
  if (authority.kind == TrustedAuthority::Kind::Server) fn["invokeScope"] = "server";
  if (authority.kind == TrustedAuthority::Kind::Automation) fn["autonomousInvocable"] = true;
}

// ---------------------------------------------------------------------------
// Naming / expression helpers
// ---------------------------------------------------------------------------

/// Convert PascalCase/camelCase to snake_case for derived function names.
inline std::string toSnakeCase(std::string_view name) {
  std::string out;
  out.reserve(name.size() + 4);
  for (std::size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    if (c == ' ' || c == '-') {
      if (!out.empty() && out.back() != '_') out.push_back('_');
      continue;
    }
    if (std::isupper(static_cast<unsigned char>(c))) {
      if (i > 0 && (std::islower(static_cast<unsigned char>(name[i - 1])) ||
                    std::isdigit(static_cast<unsigned char>(name[i - 1]))))
        out.push_back('_');
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    } else {
      out.push_back(c);
    }
  }
  return out;
}

/// How kit types mirror their owner's user id into an owner_user_id property
/// (expressions cannot read container ownership directly). Kit standard is
/// Int; use String only for models that mirrored the owner as a string.
enum class OwnerIdKind { Int, String };

/// Expression fragment: ownerExpr equals a user-id expression.
inline std::string ownerEquals(std::string_view ownerExpr, std::string_view userExpr,
                               OwnerIdKind kind = OwnerIdKind::Int) {
  std::string out(ownerExpr);
  out += " == ";
  if (kind == OwnerIdKind::String) {
    out += "to_string(";
    out += userExpr;
    out += ")";
  } else {
    out += userExpr;
  }
  return out;
}

/// Expression fragment: ownerExpr equals the calling user.
inline std::string ownerEqualsCaller(std::string_view ownerExpr,
                                     OwnerIdKind kind = OwnerIdKind::Int) {
  return ownerEquals(ownerExpr, "$caller_user_id", kind);
}

/// The SeedPropertyDefInput for a kit-standard owner mirror property.
inline JVal ownerMirrorProperty(std::string_view containerTypeName,
                                OwnerIdKind kind = OwnerIdKind::Int) {
  JVal p;
  p["containerTypeName"] = containerTypeName;
  p["key"] = "owner_user_id";
  p["valueType"] = kind == OwnerIdKind::String ? "string" : "int";
  p["defaultValueJson"] = kind == OwnerIdKind::String ? "\"\"" : "0";
  p["description"] =
      "Mirror of the container owner's user id (kit convention: expressions "
      "cannot read container ownership).";
  return p;
}

// ---------------------------------------------------------------------------
// Blueprints + merging
// ---------------------------------------------------------------------------

/// A declarative bundle implementing one game concept. Every element is a
/// JVal object matching the corresponding Seed*/Upsert* GraphQL input.
struct KitBlueprint {
  std::string name;
  JArray containerTypes;
  JArray propertyDefinitions;
  JArray functions;
  JArray containers;         ///< seed instances (catalog/world data)
  JArray edges;              ///< edges between seeded containers (by tempId)
  JArray automations;        ///< appId bound at deploy
  JArray automationTriggers;
};

struct MergedBlueprints {
  JVal seedInput;
  JArray automations;
  JArray automationTriggers;
};

/// Read a string member of a JVal object (empty when absent or non-string).
inline std::string blueprintField(const JVal& obj, std::string_view key) {
  if (!obj.isObject()) return {};
  const auto& o = obj.obj();
  auto it = o.find(key);
  if (it == o.end()) return {};
  if (const std::string* s = it->second.asStringPtr()) return *s;
  return {};
}

/// Merge blueprints into one gameModelSeed payload plus the automation
/// upserts, rejecting duplicate type/property/function/automation names
/// across blueprints. Throws std::invalid_argument on collision.
inline MergedBlueprints mergeBlueprints(std::string_view appId,
                                        const std::vector<KitBlueprint>& blueprints,
                                        std::string_view sessionId = {}) {
  JArray containerTypes, propertyDefinitions, functions, containers, edges;
  JArray automations, automationTriggers;

  std::unordered_map<std::string, std::string> seenTypes, seenProps, seenFunctions,
      seenAutomations, seenTempIds;

  auto claim = [](std::unordered_map<std::string, std::string>& seen, const std::string& key,
                  const std::string& blueprintName, const char* kind) {
    auto [it, inserted] = seen.emplace(key, blueprintName);
    if (!inserted) {
      throw std::invalid_argument("Blueprint '" + blueprintName + "' redefines " + kind + " '" +
                                  key + "' already defined by blueprint '" + it->second + "'");
    }
  };

  for (const auto& bp : blueprints) {
    for (const auto& t : bp.containerTypes) {
      claim(seenTypes, blueprintField(t, "typeName"), bp.name, "container type");
      containerTypes.push_back(t);
    }
    for (const auto& p : bp.propertyDefinitions) {
      claim(seenProps, blueprintField(p, "containerTypeName") + "." + blueprintField(p, "key"),
            bp.name, "property");
      propertyDefinitions.push_back(p);
    }
    for (const auto& f : bp.functions) {
      claim(seenFunctions, blueprintField(f, "name"), bp.name, "function");
      functions.push_back(f);
    }
    for (const auto& c : bp.containers) {
      claim(seenTempIds, blueprintField(c, "tempId"), bp.name, "container tempId");
      containers.push_back(c);
    }
    for (const auto& e : bp.edges) edges.push_back(e);
    for (const auto& a : bp.automations) {
      claim(seenAutomations, blueprintField(a, "name"), bp.name, "automation");
      JVal bound = a;
      bound["appId"] = appId;
      automations.push_back(std::move(bound));
    }
    for (const auto& t : bp.automationTriggers) {
      JVal bound = t;
      bound["appId"] = appId;
      automationTriggers.push_back(std::move(bound));
    }
  }

  JVal seedInput;
  seedInput["appId"] = appId;
  if (!sessionId.empty()) seedInput["sessionId"] = sessionId;
  if (!containerTypes.empty()) seedInput["containerTypes"] = JVal(std::move(containerTypes));
  if (!propertyDefinitions.empty())
    seedInput["propertyDefinitions"] = JVal(std::move(propertyDefinitions));
  if (!functions.empty()) seedInput["functions"] = JVal(std::move(functions));
  if (!containers.empty()) seedInput["containers"] = JVal(std::move(containers));
  if (!edges.empty()) seedInput["edges"] = JVal(std::move(edges));

  return {std::move(seedInput), std::move(automations), std::move(automationTriggers)};
}

/// Concatenate several blueprints into ONE composite blueprint (no collision
/// checks — those happen at deploy). Used by composite builders such as
/// guildBlueprint.
inline KitBlueprint composeBlueprints(std::string name, const std::vector<KitBlueprint>& list) {
  KitBlueprint out;
  out.name = std::move(name);
  for (const auto& bp : list) {
    out.containerTypes.insert(out.containerTypes.end(), bp.containerTypes.begin(),
                              bp.containerTypes.end());
    out.propertyDefinitions.insert(out.propertyDefinitions.end(), bp.propertyDefinitions.begin(),
                                   bp.propertyDefinitions.end());
    out.functions.insert(out.functions.end(), bp.functions.begin(), bp.functions.end());
    out.containers.insert(out.containers.end(), bp.containers.begin(), bp.containers.end());
    out.edges.insert(out.edges.end(), bp.edges.begin(), bp.edges.end());
    out.automations.insert(out.automations.end(), bp.automations.begin(), bp.automations.end());
    out.automationTriggers.insert(out.automationTriggers.end(), bp.automationTriggers.begin(),
                                  bp.automationTriggers.end());
  }
  return out;
}

// ---------------------------------------------------------------------------
// Runtime invoke helpers
// ---------------------------------------------------------------------------

/// A kit invoke outcome: authority denials and expression errors are NOT
/// exceptions — check success.
struct KitInvokeResult {
  bool success = false;
  Json returnValue;         ///< parsed returnValueJson when present
  std::string errorMessage;
  Json raw;                 ///< full server result (event id, mutations, ...)
};

inline constexpr std::string_view kInvokeContractViolationPrefix =
    "Invoke params violate";

/// True when a thrown GraphQL failure is a gameplay referee verdict rather
/// than a transport/session error. Legacy FORBIDDEN invoke denials and the
/// stable BAD_REQUEST invoke-contract prefix map to success=false; unrelated
/// BAD_REQUESTs and non-GraphQL exceptions still throw.
inline bool isKitVerdictError(const std::exception& error) {
  const auto* graphqlError =
      dynamic_cast<const graphql::CrowdyGraphQLError*>(&error);
  if (!graphqlError) return false;
  if (graphqlError->code() == "FORBIDDEN") return true;
  return graphqlError->code() == "BAD_REQUEST" &&
         std::string_view(graphqlError->what())
             .starts_with(kInvokeContractViolationPrefix);
}

inline KitInvokeResult kitVerdict(std::string_view functionName,
                                  std::string_view message) {
  JVal raw;
  raw["eventId"] = "";
  raw["functionName"] = functionName;
  raw["success"] = false;
  raw["returnValueJson"] = nullptr;
  raw["errorMessage"] = message;
  raw["mutationsApplied"] = JVal(JArray{});

  KitInvokeResult result;
  result.success = false;
  result.errorMessage = std::string(message);
  result.raw = Json::parse(raw.dump());
  return result;
}

/// Invoke a model function and wrap the result. Authority denials are part
/// of the result (success = false), never exceptions — whether the server
/// reports them in the payload, as a legacy FORBIDDEN GraphQL error, or as a
/// BAD_REQUEST beginning with the stable invoke-contract violation prefix.
inline KitInvokeResult kitInvoke(domains::GameModelAPI& gameModel, std::string_view appId,
                                 std::string_view functionName, std::string_view selfContainerId,
                                 const JVal& params = JVal(), std::string_view sessionId = {}) {
  JVal input;
  input["appId"] = appId;
  input["functionName"] = functionName;
  input["selfContainerId"] = selfContainerId;
  input["paramsJson"] = params.isNull() ? std::string("{}") : params.dump();
  if (!sessionId.empty()) input["sessionId"] = sessionId;

  KitInvokeResult result;
  Json raw;
  try {
    raw = gameModel.invoke(input);
  } catch (const graphql::CrowdyGraphQLError& e) {
    if (isKitVerdictError(e)) return kitVerdict(functionName, e.what());
    throw;
  }
  result.raw = raw;
  result.success = raw["success"].asBool();
  result.errorMessage = raw["errorMessage"].asString();
  if (result.success) {
    auto rv = raw["returnValueJson"];
    if (rv.isString()) result.returnValue = Json::parse(rv.asStringView());
  }
  return result;
}

/// Read a container's visible properties as a parsed JSON object.
inline Json kitContainerProperties(domains::GameModelAPI& gameModel, std::string_view appId,
                                   std::string_view containerId) {
  Json state = gameModel.containerState(appId, containerId);
  return Json::parse(state["propertiesJson"].asStringView());
}

}  // namespace crowdy::kit
