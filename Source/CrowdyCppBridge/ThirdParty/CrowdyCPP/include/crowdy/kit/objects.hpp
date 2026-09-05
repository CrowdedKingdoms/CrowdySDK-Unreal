#pragma once

#include <optional>
#include <stdexcept>
#include <vector>

#include "crowdy/kit/core.hpp"

/// Lockable objects — doors, chests, gates, switches — whose open/close
/// functions are gated by a configurable authority source: ownership, a key
/// item the caller must hold, a runtime grid permission, a team/group
/// permission, or any custom policy rule. Mirrors CrowdyJS's lockBlueprint /
/// kit.objects.
namespace crowdy::kit {

/// The authority source that may operate a lockable object. Several may be
/// combined (any one grants access — they are OR'd):
///
/// - owner — only the container's owner (owner_of_self); "only the owner of
///   this chest can open it".
/// - key — the caller must own a matching key item; "if a player has key 1
///   they can open door 1". Adds a required key_id (container_ref) param.
/// - gridPermission — the caller must hold a runtime grid permission; ties
///   the object to a world region ("movement permission on chunk X opens the
///   doors in it").
/// - chunkPermission — the caller must hold the runtime permission on the
///   grid covering the chunk the OBJECT stands in (cx/cy/cz int properties,
///   seeded via ObjectsKit::create with a chunk). No hand-pinned grid id —
///   one function serves every such object in the world. mode picks the
///   covering grid when several overlap: "first" (enforcement parity,
///   default) | "smallest" (innermost plot) | "largest". Requires game-api
///   v0.13.12+.
/// - groupPermission — the caller must be in a team/group (optionally with a
///   specific group permission).
/// - custom — any hand-written invoke-policy rule.
struct LockAuthority {
  enum class Kind { Owner, Key, GridPermission, ChunkPermission, GroupPermission, Custom };
  Kind kind = Kind::Owner;
  std::string permissionKey;  ///< runtime permission key (grid/chunk kinds)
  JVal gridId;                ///< optional grid id (gridPermission); null = unset
  std::string mode;           ///< chunkPermission covering-grid pick; empty = "first"
  std::string groupId;        ///< group id (groupPermission)
  std::string permission;     ///< optional group permission (groupPermission)
  JVal custom;                ///< hand-written policy rule (custom)

  static LockAuthority owner() { return {}; }
  static LockAuthority key() {
    LockAuthority a;
    a.kind = Kind::Key;
    return a;
  }
  static LockAuthority gridPermission(std::string permissionKey, JVal gridId = JVal()) {
    LockAuthority a;
    a.kind = Kind::GridPermission;
    a.permissionKey = std::move(permissionKey);
    a.gridId = std::move(gridId);
    return a;
  }
  static LockAuthority chunkPermission(std::string permissionKey, std::string mode = {}) {
    LockAuthority a;
    a.kind = Kind::ChunkPermission;
    a.permissionKey = std::move(permissionKey);
    a.mode = std::move(mode);
    return a;
  }
  static LockAuthority groupPermission(std::string groupId, std::string permission = {}) {
    LockAuthority a;
    a.kind = Kind::GroupPermission;
    a.groupId = std::move(groupId);
    a.permission = std::move(permission);
    return a;
  }
  static LockAuthority customRule(JVal rule) {
    LockAuthority a;
    a.kind = Kind::Custom;
    a.custom = std::move(rule);
    return a;
  }
};

/// Options for lockBlueprint.
struct LockBlueprintOptions {
  /// Container type name for the lockable object (Door, Chest, Gate, …).
  /// Also drives the function names (open_door / close_door). Empty defaults
  /// to "Lockable".
  std::string objectTypeName;
  /// Key item type name (only used with a key authority). Empty defaults to
  /// "<objectTypeName>Key".
  std::string keyTypeName;
  /// One or more authority sources; any one grants access.
  std::vector<LockAuthority> authority;
  /// Extra policy rule AND'ed on top of the OR'd authorities — e.g.
  /// featureGate("vip") for members-only doors regardless of keys.
  JVal policyExtra;
};

/// Names derived by lockBlueprint for a given object type.
struct LockNames {
  std::string objectType;
  std::string keyType;
  std::string openFn;
  std::string closeFn;
};

/// Compute the type/function names a lock blueprint (and its runtime helper)
/// uses. Empty arguments select the defaults.
inline LockNames lockNames(std::string_view objectTypeName = {},
                           std::string_view keyTypeName = {}) {
  const std::string objectType =
      objectTypeName.empty() ? std::string("Lockable") : std::string(objectTypeName);
  const std::string snake = toSnakeCase(objectType);
  LockNames n;
  n.objectType = objectType;
  n.keyType = keyTypeName.empty() ? objectType + "Key" : std::string(keyTypeName);
  n.openFn = "open_" + snake;
  n.closeFn = "close_" + snake;
  return n;
}

/// The policy rule one LockAuthority compiles to.
inline JVal lockAuthorityRule(const LockAuthority& authority) {
  switch (authority.kind) {
    case LockAuthority::Kind::Owner:
      return ownerOfSelfPolicy();
    case LockAuthority::Kind::Key:
      // Container ownership is not readable from expressions, so the key
      // mirrors its owner into an owner_user_id property; the condition
      // verifies both the match and the ownership server-side.
      return conditionPolicy(
          "ref($key_id).key_id == self.required_key_id && "
          "ref($key_id).owner_user_id == $caller_user_id");
    case LockAuthority::Kind::GridPermission:
      return gridPermissionPolicy(authority.permissionKey, authority.gridId);
    case LockAuthority::Kind::ChunkPermission: {
      // Resolved per-invocation against the grid covering the object's chunk.
      const std::string mode = authority.mode.empty() ? std::string("first") : authority.mode;
      return conditionPolicy("has_chunk_permission($caller_user_id, \"" +
                             authority.permissionKey + "\", self.cx, self.cy, self.cz, \"" +
                             mode + "\")");
    }
    case LockAuthority::Kind::GroupPermission:
      return groupPermissionPolicy(authority.groupId, authority.permission);
    case LockAuthority::Kind::Custom:
      break;
  }
  return authority.custom;
}

/// Blueprint for a lockable game object (door, chest, gate, switch) whose
/// open/close functions are gated by a configurable authority source:
/// ownership, a key item the caller must hold, a runtime grid permission, a
/// team/group permission, or any custom policy rule. Multiple authorities are
/// OR'd, so "the owner, or anyone with the right key" is one blueprint.
///
/// Runtime counterpart: ObjectsKit.
inline KitBlueprint lockBlueprint(const LockBlueprintOptions& options) {
  if (options.authority.empty()) {
    throw std::invalid_argument("lockBlueprint requires at least one authority source");
  }
  const LockNames names = lockNames(options.objectTypeName, options.keyTypeName);
  bool usesKey = false;
  bool usesChunk = false;
  for (const auto& a : options.authority) {
    if (a.kind == LockAuthority::Kind::Key) usesKey = true;
    if (a.kind == LockAuthority::Kind::ChunkPermission) usesChunk = true;
  }

  JArray rules;
  for (const auto& a : options.authority) rules.push_back(lockAuthorityRule(a));
  JVal authorityPolicy = rules.size() == 1 ? rules.front() : orPolicy(std::move(rules));
  const std::string policyJson =
      kitPolicyJson(andPolicies(std::move(authorityPolicy), {options.policyExtra}));

  JArray parameters;
  if (usesKey) {
    JVal p;
    p["name"] = "key_id";
    p["valueType"] = "container_ref";
    p["required"] = true;
    parameters.push_back(std::move(p));
  }

  auto containerType = [](const std::string& typeName, const char* description) {
    JVal t;
    t["typeName"] = typeName;
    t["displayName"] = typeName;
    t["instantiableBy"] = "admin";
    t["description"] = description;
    return t;
  };
  auto propertyDef = [](const std::string& type, const char* key, const char* valueType,
                        std::optional<std::string> defaultJson) {
    JVal p;
    p["containerTypeName"] = type;
    p["key"] = key;
    p["valueType"] = valueType;
    if (defaultJson) p["defaultValueJson"] = *defaultJson;
    return p;
  };
  auto mutation = [](const char* expression) {
    JVal m;
    m["target"] = "self";
    m["property"] = "is_open";
    m["expression"] = expression;
    return m;
  };

  KitBlueprint bp;
  bp.name = names.objectType;

  bp.containerTypes.push_back(containerType(
      names.objectType, "A lockable world object operated through gated functions."));
  bp.propertyDefinitions.push_back(propertyDef(names.objectType, "is_open", "bool", "false"));

  if (usesChunk) {
    // The chunk the object stands in, read by has_chunk_permission().
    for (const char* axis : {"cx", "cy", "cz"}) {
      bp.propertyDefinitions.push_back(propertyDef(names.objectType, axis, "int", "0"));
    }
  }

  if (usesKey) {
    bp.propertyDefinitions.push_back(
        propertyDef(names.objectType, "required_key_id", "string", std::nullopt));
    bp.containerTypes.push_back(containerType(
        names.keyType, "A key item granting access to matching lockable objects."));
    bp.propertyDefinitions.push_back(propertyDef(names.keyType, "key_id", "string", std::nullopt));
    bp.propertyDefinitions.push_back(
        propertyDef(names.keyType, "owner_user_id", "int", std::nullopt));
  }

  {
    JVal fn;
    fn["name"] = names.openFn;
    fn["containerTypeName"] = names.objectType;
    fn["returnType"] = "bool";
    fn["parameters"] = JVal(parameters);
    fn["mutations"] = JVal::array({mutation("true")});
    fn["returnExpression"] = "self.is_open";
    fn["invokePolicyJson"] = policyJson;
    fn["description"] = "Open a " + names.objectType + "; the invoke policy decides who may.";
    bp.functions.push_back(std::move(fn));
  }
  {
    JVal fn;
    fn["name"] = names.closeFn;
    fn["containerTypeName"] = names.objectType;
    fn["returnType"] = "bool";
    fn["parameters"] = JVal(parameters);
    fn["mutations"] = JVal::array({mutation("false")});
    fn["returnExpression"] = "self.is_open";
    fn["invokePolicyJson"] = policyJson;
    fn["description"] = "Close a " + names.objectType + "; same authority as opening.";
    bp.functions.push_back(std::move(fn));
  }

  return bp;
}

/// A chunk coordinate for chunk-permission-gated objects.
struct KitChunkCoords {
  std::int64_t x = 0;
  std::int64_t y = 0;
  std::int64_t z = 0;
};

/// Input for ObjectsKit::create.
struct KitLockableCreateInput {
  std::string displayName;
  /// The key id that opens the object (key-gated objects).
  std::optional<std::string> requiredKeyId;
  /// Owner user id (owner-gated objects); empty = none.
  std::string ownerUserId;
  /// The chunk the object occupies (chunkPermission authority).
  std::optional<KitChunkCoords> chunk;
  /// Extra SeedPropertyInput objects appended after the generated ones.
  JArray properties;
  std::string sessionId;
};

/// Runtime helpers for the lockBlueprint conventions: instantiate lockable
/// world objects, grant key items, and operate the objects through their
/// authority-gated open/close functions. Authorization is decided entirely
/// server-side; a denied attempt resolves with success == false.
///
/// Obtained via GameKitClient::objects() (or objectsFor("Door") for a
/// non-default type name).
class ObjectsKit {
 public:
  ObjectsKit(std::string appId, domains::GameModelAPI& gameModel,
             std::string_view objectTypeName = {}, std::string_view keyTypeName = {})
      : appId_(std::move(appId)),
        gameModel_(gameModel),
        names_(lockNames(objectTypeName, keyTypeName)) {}

  const LockNames& names() const { return names_; }

  /// Instantiate a lockable object (admin/studio call — the type is
  /// admin-instantiable). For key-gated objects set requiredKeyId to the key
  /// id that opens it; for owner-gated objects set ownerUserId; for
  /// chunk-permission-gated objects set chunk to where the object stands
  /// (feeds the has_chunk_permission policy).
  Json create(const KitLockableCreateInput& input) {
    JArray properties;
    properties.push_back(property("is_open", "bool", "false"));
    if (input.requiredKeyId) {
      properties.push_back(
          property("required_key_id", "string", JVal(*input.requiredKeyId).dump()));
    }
    if (input.chunk) {
      properties.push_back(property("cx", "int", std::to_string(input.chunk->x)));
      properties.push_back(property("cy", "int", std::to_string(input.chunk->y)));
      properties.push_back(property("cz", "int", std::to_string(input.chunk->z)));
    }
    for (const auto& p : input.properties) properties.push_back(p);

    JVal vars;
    vars["appId"] = appId_;
    vars["typeName"] = names_.objectType;
    vars["displayName"] = input.displayName;
    if (!input.ownerUserId.empty()) vars["ownerUserId"] = input.ownerUserId;
    if (!input.sessionId.empty()) vars["sessionId"] = input.sessionId;
    vars["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(vars);
  }

  /// Grant a player a key item (admin/studio call). Creates a key container
  /// owned by the player, with the owner mirrored into the owner_user_id
  /// property that the key condition policy reads.
  Json grantKey(std::string_view keyId, std::string_view toUserId,
                std::string_view displayName = {}) {
    JVal vars;
    vars["appId"] = appId_;
    vars["typeName"] = names_.keyType;
    vars["displayName"] =
        displayName.empty() ? "Key " + std::string(keyId) : std::string(displayName);
    vars["ownerUserId"] = toUserId;
    JArray properties;
    properties.push_back(property("key_id", "string", JVal(keyId).dump()));
    properties.push_back(property("owner_user_id", "int", std::string(toUserId)));
    vars["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(vars);
  }

  /// List the key items a player holds.
  std::vector<Json> keysOf(std::string_view userId) {
    Json containers = gameModel_.containers(appId_, names_.keyType);
    std::vector<Json> out;
    containers.forEach([&](Json c) {
      Json owner = c["ownerUserId"];
      if (!owner.ok() || owner.isNull()) return;
      if (owner.asString() != userId) return;
      out.push_back(c);
    });
    return out;
  }

  /// Try to open an object. Pass keyId (the CONTAINER ID of a key the caller
  /// holds) when the object is key-gated; owner/grid/group authorities need
  /// no params. A denial is not an exception — check success.
  KitInvokeResult open(std::string_view objectId, std::string_view keyId = {}) {
    JVal params;
    if (!keyId.empty()) params["key_id"] = keyId;
    return kitInvoke(gameModel_, appId_, names_.openFn, objectId, params);
  }

  /// Try to close an object; same authority as open().
  KitInvokeResult close(std::string_view objectId, std::string_view keyId = {}) {
    JVal params;
    if (!keyId.empty()) params["key_id"] = keyId;
    return kitInvoke(gameModel_, appId_, names_.closeFn, objectId, params);
  }

  /// Read whether an object is currently open.
  bool isOpen(std::string_view objectId) {
    Json props = kitContainerProperties(gameModel_, appId_, objectId);
    return props["is_open"].asBool();
  }

  /// List all objects of this lockable type.
  Json list(std::string_view sessionId = {}) {
    return gameModel_.containers(appId_, names_.objectType, sessionId);
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
  LockNames names_;
};

}  // namespace crowdy::kit
