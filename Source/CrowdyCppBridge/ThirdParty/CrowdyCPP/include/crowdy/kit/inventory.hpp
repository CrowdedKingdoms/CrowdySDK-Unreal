#pragma once

#include <optional>

#include "crowdy/kit/core.hpp"

/// Inventory — a server-authoritative per-player bag: an Inventory container
/// plus ItemStack containers (item_id, quantity, slot) and owner-gated
/// functions to grant, consume, move, and transfer stacks. Quantity guards
/// live in the invoke policies, so an untrusted client can never overdraw or
/// touch another player's items. Mirrors CrowdyJS's inventoryBlueprint /
/// kit.inventory.
namespace crowdy::kit {

struct InventoryIngredient {
  std::string itemId;
  std::int64_t quantity = 1;
};

struct InventoryRecipeSpec {
  std::string recipeId;
  std::vector<InventoryIngredient> inputs;
  InventoryIngredient output;
};

struct InventoryBarterSpec {
  std::string barterId;
  InventoryIngredient pay;
  InventoryIngredient receive;
};

struct InventoryBlueprintOptions {
  /// Prefix for type names ("Bank" -> BankInventory/BankItemStack) and,
  /// snake-cased, for function names (bank_grant_stack, ...). Lets several
  /// inventory systems coexist.
  std::string typePrefix;
  int maxSlots = 24;   ///< default max_slots on new inventories
  int slotCount = 64;  ///< exclusive upper bound for stack slot indexes
  std::vector<InventoryRecipeSpec> recipes;
  std::vector<InventoryBarterSpec> barters;
  OwnerIdKind ownerIdKind = OwnerIdKind::Int;
  std::string stackInstantiableBy = "member";
  bool serverGrant = false;
};

struct InventoryNames {
  std::string inventoryType;
  std::string stackType;
  std::string grantFn;
  std::string consumeFn;
  std::string moveFn;
  std::string transferFn;
  std::string containsEdge;
};

inline InventoryNames inventoryNames(std::string_view typePrefix = {}) {
  const std::string fnPrefix =
      typePrefix.empty() ? std::string() : toSnakeCase(typePrefix) + "_";
  InventoryNames n;
  n.inventoryType = std::string(typePrefix) + "Inventory";
  n.stackType = std::string(typePrefix) + "ItemStack";
  n.grantFn = fnPrefix + "grant_stack";
  n.consumeFn = fnPrefix + "consume_stack";
  n.moveFn = fnPrefix + "move_stack";
  n.transferFn = fnPrefix + "transfer_stack";
  n.containsEdge = "inventory_contains";
  return n;
}

inline std::string inventoryCraftFunctionName(std::string_view recipeId,
                                              std::string_view typePrefix = {}) {
  const std::string prefix =
      typePrefix.empty() ? std::string() : toSnakeCase(typePrefix) + "_";
  return prefix + "craft_" + toSnakeCase(recipeId);
}

inline std::string inventoryBarterFunctionName(std::string_view barterId,
                                               std::string_view typePrefix = {}) {
  const std::string prefix =
      typePrefix.empty() ? std::string() : toSnakeCase(typePrefix) + "_";
  return prefix + "barter_" + toSnakeCase(barterId);
}

/// Build the inventory blueprint. Runtime counterpart: InventoryKit.
inline KitBlueprint inventoryBlueprint(const InventoryBlueprintOptions& options = {}) {
  const InventoryNames names = inventoryNames(options.typePrefix);
  const std::string ownerOnly = kitPolicyJson(ownerOfSelfPolicy());

  auto containerType = [](const std::string& typeName, const char* description) {
    JVal t;
    t["typeName"] = typeName;
    t["displayName"] = typeName;
    t["instantiableBy"] = "member";
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
  auto param = [](const char* name, const char* valueType) {
    JVal p;
    p["name"] = name;
    p["valueType"] = valueType;
    p["required"] = true;
    return p;
  };
  auto mutation = [](std::string_view target, const char* property, std::string expression) {
    JVal m;
    m["target"] = target;
    m["property"] = property;
    m["expression"] = std::move(expression);
    return m;
  };

  KitBlueprint bp;
  bp.name = names.inventoryType;

  bp.containerTypes.push_back(
      containerType(names.inventoryType, "A bag of item stacks owned by one player."));
  {
    JVal stack =
        containerType(names.stackType, "One stack of a single item type in an inventory slot.");
    stack["instantiableBy"] = options.stackInstantiableBy;
    bp.containerTypes.push_back(std::move(stack));
  }

  bp.propertyDefinitions.push_back(
      propertyDef(names.inventoryType, "max_slots", "int", std::to_string(options.maxSlots)));
  bp.propertyDefinitions.push_back(
      propertyDef(names.stackType, "item_id", "string", std::nullopt));
  {
    bp.propertyDefinitions.push_back(ownerMirrorProperty(names.stackType, options.ownerIdKind));
  }
  bp.propertyDefinitions.push_back(propertyDef(names.stackType, "quantity", "int", "0"));
  bp.propertyDefinitions.push_back(propertyDef(names.stackType, "slot", "int", "0"));

  {
    JVal fn;
    fn["name"] = names.grantFn;
    fn["containerTypeName"] = names.stackType;
    fn["returnType"] = "int";
    fn["parameters"] = JVal::array({param("amount", "int")});
    fn["mutations"] =
        JVal::array({mutation("self", "quantity", "self.quantity + max(0, $amount)")});
    fn["returnExpression"] = "self.quantity";
    fn["invokePolicyJson"] =
        options.serverGrant ? kitPolicyJson(isAutomationPolicy()) : ownerOnly;
    if (options.serverGrant) {
      fn["invokeScope"] = "internal";
      fn["autonomousInvocable"] = true;
    }
    fn["description"] = options.serverGrant
                            ? "Trusted server/compute grant; players cannot mint items."
                            : "Add items to a stack the caller owns.";
    bp.functions.push_back(std::move(fn));
  }
  {
    JVal fn;
    fn["name"] = names.consumeFn;
    fn["containerTypeName"] = names.stackType;
    fn["returnType"] = "int";
    fn["parameters"] = JVal::array({param("amount", "int")});
    fn["mutations"] = JVal::array({mutation("self", "quantity", "self.quantity - $amount")});
    fn["returnExpression"] = "self.quantity";
    fn["invokePolicyJson"] = kitPolicyJson(andPolicy(
        {ownerOfSelfPolicy(), conditionPolicy("$amount > 0 && self.quantity >= $amount")}));
    fn["description"] = "Spend items from a stack; refuses to overdraw.";
    bp.functions.push_back(std::move(fn));
  }
  {
    JVal fn;
    fn["name"] = names.moveFn;
    fn["containerTypeName"] = names.stackType;
    fn["returnType"] = "int";
    fn["parameters"] = JVal::array({param("to_slot", "int")});
    fn["mutations"] = JVal::array({mutation(
        "self", "slot", "clamp($to_slot, 0, " + std::to_string(options.slotCount - 1) + ")")});
    fn["returnExpression"] = "self.slot";
    fn["invokePolicyJson"] = ownerOnly;
    fn["description"] = "Move a stack to another slot.";
    bp.functions.push_back(std::move(fn));
  }
  {
    JVal fn;
    fn["name"] = names.transferFn;
    fn["containerTypeName"] = names.stackType;
    fn["returnType"] = "int";
    fn["parameters"] = JVal::array({param("to_id", "container_ref"), param("amount", "int")});
    fn["mutations"] = JVal::array(
        {mutation("self", "quantity", "self.quantity - $amount"),
         mutation("ref($to_id)", "quantity", "ref($to_id).quantity + $amount")});
    fn["returnExpression"] = "self.quantity";
    fn["invokePolicyJson"] = kitPolicyJson(andPolicy(
        {ownerOfSelfPolicy(),
         conditionPolicy(
             "$amount > 0 && self.quantity >= $amount && ref($to_id).item_id == self.item_id")}));
    fn["description"] = "Atomically move items between two stacks of the same item type.";
    bp.functions.push_back(std::move(fn));
  }

  for (const auto& recipe : options.recipes) {
    if (recipe.inputs.empty() || recipe.inputs.size() > 6) {
      throw std::invalid_argument("inventory recipe '" + recipe.recipeId +
                                  "' must have 1-6 inputs");
    }
    JVal fn;
    fn["name"] = inventoryCraftFunctionName(recipe.recipeId, options.typePrefix);
    fn["containerTypeName"] = names.inventoryType;
    fn["returnType"] = "int";
    JArray parameters;
    JArray mutations;
    std::vector<std::string> guards;
    for (std::size_t i = 0; i < recipe.inputs.size(); ++i) {
      const std::string input = "input_" + std::to_string(i) + "_id";
      parameters.push_back(param(input.c_str(), "container_ref"));
      mutations.push_back(mutation("ref($" + input + ")", "quantity",
                                   "ref($" + input + ").quantity - " +
                                       std::to_string(recipe.inputs[i].quantity)));
      guards.push_back(
          ownerEqualsCaller("ref($" + input + ").owner_user_id", options.ownerIdKind));
      guards.push_back("ref($" + input + ").item_id == \"" +
                       recipe.inputs[i].itemId + "\"");
      guards.push_back("ref($" + input + ").quantity >= " +
                       std::to_string(recipe.inputs[i].quantity));
    }
    parameters.push_back(param("output_id", "container_ref"));
    mutations.push_back(mutation(
        "ref($output_id)", "quantity",
        "ref($output_id).quantity + " + std::to_string(recipe.output.quantity)));
    guards.push_back(ownerEqualsCaller("ref($output_id).owner_user_id", options.ownerIdKind));
    guards.push_back("ref($output_id).item_id == \"" + recipe.output.itemId + "\"");
    std::string guard;
    for (const auto& part : guards) {
      if (!guard.empty()) guard += " && ";
      guard += part;
    }
    fn["parameters"] = JVal(std::move(parameters));
    fn["mutations"] = JVal(std::move(mutations));
    fn["returnExpression"] = "ref($output_id).quantity";
    fn["invokePolicyJson"] =
        kitPolicyJson(andPolicy({ownerOfSelfPolicy(), conditionPolicy(guard)}));
    fn["autonomousInvocable"] = true;
    fn["description"] = "Atomically craft '" + recipe.recipeId +
                        "': consume all inputs and grant the output, or write nothing.";
    bp.functions.push_back(std::move(fn));
  }

  for (const auto& barter : options.barters) {
    JVal fn;
    fn["name"] = inventoryBarterFunctionName(barter.barterId, options.typePrefix);
    fn["containerTypeName"] = names.inventoryType;
    fn["returnType"] = "int";
    fn["parameters"] =
        JVal::array({param("pay_id", "container_ref"), param("receive_id", "container_ref")});
    fn["mutations"] = JVal::array({
        mutation("ref($pay_id)", "quantity",
                 "ref($pay_id).quantity - " + std::to_string(barter.pay.quantity)),
        mutation("ref($receive_id)", "quantity",
                 "ref($receive_id).quantity + " +
                     std::to_string(barter.receive.quantity)),
    });
    fn["returnExpression"] = "ref($receive_id).quantity";
    const std::string guard =
        ownerEqualsCaller("ref($pay_id).owner_user_id", options.ownerIdKind) +
        " && ref($pay_id).item_id == \"" + barter.pay.itemId +
        "\" && ref($pay_id).quantity >= " + std::to_string(barter.pay.quantity) +
        " && " +
        ownerEqualsCaller("ref($receive_id).owner_user_id", options.ownerIdKind) +
        " && ref($receive_id).item_id == \"" + barter.receive.itemId + "\"";
    fn["invokePolicyJson"] =
        kitPolicyJson(andPolicy({ownerOfSelfPolicy(), conditionPolicy(guard)}));
    fn["autonomousInvocable"] = true;
    fn["description"] =
        "Atomically execute barter '" + barter.barterId + "', or write nothing.";
    bp.functions.push_back(std::move(fn));
  }

  return bp;
}

/// A parsed view of one item stack.
struct KitItemStack {
  std::string containerId;
  std::string displayName;
  std::string ownerUserId;
  std::string itemId;
  std::int64_t quantity = 0;
  std::int64_t slot = 0;
};

/// Runtime helpers for the inventoryBlueprint conventions. Obtained via
/// GameKitClient::inventory().
class InventoryKit {
 public:
  InventoryKit(std::string appId, domains::GameModelAPI& gameModel,
               std::string_view typePrefix = {},
               OwnerIdKind ownerIdKind = OwnerIdKind::Int)
      : appId_(std::move(appId)),
        gameModel_(gameModel),
        typePrefix_(typePrefix),
        ownerIdKind_(ownerIdKind),
        names_(inventoryNames(typePrefix)) {}

  const InventoryNames& names() const { return names_; }

  /// Find the caller's inventory container, creating it when absent.
  Json ensure(std::string_view ownerUserId, std::string_view displayName = {},
              std::string_view sessionId = {}) {
    Json existing = gameModel_.containers(appId_, names_.inventoryType, sessionId);
    Json mine;
    existing.forEach([&](Json c) {
      if (!mine.isNull()) return;
      if (c["ownerUserId"].asString() == ownerUserId) mine = c;
    });
    if (mine.ok() && !mine.isNull()) return mine;

    JVal input;
    input["appId"] = appId_;
    input["typeName"] = names_.inventoryType;
    input["displayName"] = displayName.empty()
                               ? "Inventory " + std::string(ownerUserId)
                               : std::string(displayName);
    if (!sessionId.empty()) input["sessionId"] = sessionId;
    return gameModel_.createContainer(input);
  }

  /// List a player's item stacks with parsed properties.
  std::vector<KitItemStack> stacks(std::string_view ownerUserId) {
    Json containers = gameModel_.containers(appId_, names_.stackType);
    std::vector<KitItemStack> out;
    containers.forEach([&](Json c) {
      if (c["ownerUserId"].asString() != ownerUserId) return;
      out.push_back(parseStack(c));
    });
    return out;
  }

  /// Create a new stack owned by the caller. Pass ownerUserId (your own user
  /// id) to also set the owner_user_id mirror property cross-container guards
  /// verify.
  Json createStack(std::string_view itemId, std::int64_t quantity = 0, std::int64_t slot = 0,
                   std::string_view ownerUserId = {}, std::string_view displayName = {},
                   std::string_view sessionId = {}) {
    JVal input;
    input["appId"] = appId_;
    input["typeName"] = names_.stackType;
    input["displayName"] =
        displayName.empty() ? "Stack " + std::string(itemId) : std::string(displayName);
    if (!sessionId.empty()) input["sessionId"] = sessionId;
    JArray properties;
    properties.push_back(property("item_id", "string", JVal(itemId).dump()));
    properties.push_back(property("quantity", "int", std::to_string(quantity)));
    properties.push_back(property("slot", "int", std::to_string(slot)));
    if (!ownerUserId.empty())
      properties.push_back(property(
          "owner_user_id",
          ownerIdKind_ == OwnerIdKind::String ? "string" : "int",
          ownerIdKind_ == OwnerIdKind::String ? JVal(ownerUserId).dump()
                                              : std::string(ownerUserId)));
    input["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(input);
  }

  /// Add items to a stack the caller owns. Returns the new quantity.
  KitInvokeResult grant(std::string_view stackId, std::int64_t amount) {
    JVal params;
    params["amount"] = amount;
    return kitInvoke(gameModel_, appId_, names_.grantFn, stackId, params);
  }

  /// Spend items; the server refuses to overdraw (success=false).
  KitInvokeResult consume(std::string_view stackId, std::int64_t amount) {
    JVal params;
    params["amount"] = amount;
    return kitInvoke(gameModel_, appId_, names_.consumeFn, stackId, params);
  }

  /// Move a stack to another slot (clamped server-side).
  KitInvokeResult move(std::string_view stackId, std::int64_t toSlot) {
    JVal params;
    params["to_slot"] = toSlot;
    return kitInvoke(gameModel_, appId_, names_.moveFn, stackId, params);
  }

  /// Atomically move items between two stacks of the same item type.
  KitInvokeResult transfer(std::string_view fromStackId, std::string_view toStackId,
                           std::int64_t amount) {
    JVal params;
    params["to_id"] = toStackId;
    params["amount"] = amount;
    return kitInvoke(gameModel_, appId_, names_.transferFn, fromStackId, params);
  }

  /// Atomically craft a generated recipe. Input stack ids must follow the
  /// recipe input order; every write commits together or none do.
  KitInvokeResult craft(std::string_view inventoryId, std::string_view recipeId,
                        const std::vector<std::string>& inputStackIds,
                        std::string_view outputStackId) {
    JVal params;
    for (std::size_t i = 0; i < inputStackIds.size(); ++i) {
      params["input_" + std::to_string(i) + "_id"] = inputStackIds[i];
    }
    params["output_id"] = outputStackId;
    return kitInvoke(gameModel_, appId_,
                     inventoryCraftFunctionName(recipeId, typePrefix_), inventoryId, params);
  }

  /// Atomically execute a generated item-for-item barter offer.
  KitInvokeResult barter(std::string_view inventoryId, std::string_view barterId,
                         std::string_view payStackId,
                         std::string_view receiveStackId) {
    JVal params;
    params["pay_id"] = payStackId;
    params["receive_id"] = receiveStackId;
    return kitInvoke(gameModel_, appId_,
                     inventoryBarterFunctionName(barterId, typePrefix_), inventoryId, params);
  }

  /// Record that a stack belongs to an inventory (inventory_contains edge).
  Json linkStack(std::string_view inventoryId, std::string_view stackId) {
    JVal input;
    input["appId"] = appId_;
    input["fromContainerId"] = inventoryId;
    input["toContainerId"] = stackId;
    input["relationshipType"] = names_.containsEdge;
    return gameModel_.addEdge(input);
  }

  /// Read every stack linked to an inventory (via inventory_contains edges).
  std::vector<KitItemStack> contents(std::string_view inventoryId) {
    JVal vars;
    vars["appId"] = appId_;
    vars["rootId"] = inventoryId;
    vars["relationshipType"] = names_.containsEdge;
    vars["depth"] = std::int64_t{1};
    Json result = gameModel_.traverse(vars);
    std::vector<KitItemStack> out;
    result["nodes"].forEach([&](Json node) {
      if (node["typeName"].asString() != names_.stackType) return;
      out.push_back(parseStack(node));
    });
    return out;
  }

 private:
  static JVal property(const char* key, const char* valueType, std::string valueJson) {
    JVal p;
    p["key"] = key;
    p["valueType"] = valueType;
    p["valueJson"] = std::move(valueJson);
    return p;
  }

  KitItemStack parseStack(const Json& c) {
    Json props = kitContainerProperties(gameModel_, appId_, c["containerId"].asStringView());
    KitItemStack s;
    s.containerId = c["containerId"].asString();
    s.displayName = c["displayName"].asString();
    s.ownerUserId = c["ownerUserId"].asString();
    s.itemId = props["item_id"].asString();
    s.quantity = props["quantity"].asInt64();
    s.slot = props["slot"].asInt64();
    return s;
  }

  std::string appId_;
  domains::GameModelAPI& gameModel_;
  std::string typePrefix_;
  OwnerIdKind ownerIdKind_;
  InventoryNames names_;
};

}  // namespace crowdy::kit
