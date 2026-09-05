#pragma once

#include <optional>
#include <vector>

#include "crowdy/domains/game_apps.hpp"
#include "crowdy/kit/core.hpp"

/// Sellable/rentable land — a Plot container mapping a world grid to a price,
/// whose buy/rent functions consume currency AND grant runtime grid
/// permissions in one transaction (via permission effects). Mirrors
/// CrowdyJS's plotBlueprint / kit.plots.
namespace crowdy::kit {

/// Options for plotBlueprint.
struct PlotBlueprintOptions {
  /// Plot container type name. Empty defaults to "Plot".
  std::string typeName;
  /// Runtime permission keys buying/renting grants on the plot's grid.
  std::vector<std::string> permissionKeys{"access", "update_voxel_data"};
  /// Currency property on the wallet container.
  std::string currencyProperty = "gold";
  /// Property on the wallet container mirroring its owner's user id (the
  /// standard kit convention, since expressions can't read container
  /// ownership).
  std::string walletOwnerProperty = "owner_user_id";
  /// Also generate rent_plot: a TTL grant priced by the plot's rent_price
  /// property, expiring after rent_ttl_seconds.
  bool rentable = false;
  /// Extra policy rule AND'ed into buy_plot — e.g. featureGate("land_owner")
  /// to sell land only to a paid tier. Null = none.
  JVal buyPolicyExtra;
  /// Extra policy rule AND'ed into rent_plot. Null = none.
  JVal rentPolicyExtra;
  /// How the owner_user_id mirrors are typed on the plot AND the wallet (see
  /// the kit owner-mirroring convention). Int is the kit standard; use String
  /// for models that mirrored owners as strings — generated guards then
  /// compare via to_string($caller_user_id), buying writes the owner as a
  /// string, and an empty string means "for sale".
  OwnerIdKind ownerIdKind = OwnerIdKind::Int;
};

/// Names derived by plotBlueprint for a given plot type.
struct PlotNames {
  std::string plotType;
  std::string buyFn;
  std::string rentFn;
  std::string evictFn;
};

/// Compute the type/function names a plot blueprint (and its runtime helper)
/// uses. An empty argument selects the default "Plot".
inline PlotNames plotNames(std::string_view typeName = {}) {
  const std::string plotType = typeName.empty() ? std::string("Plot") : std::string(typeName);
  const std::string snake = toSnakeCase(plotType);
  PlotNames n;
  n.plotType = plotType;
  n.buyFn = "buy_" + snake;
  n.rentFn = "rent_" + snake;
  n.evictFn = "evict_" + snake;
  return n;
}

/// Blueprint for sellable/rentable land: a Plot container mapping a world
/// grid to a price, whose buy/rent functions consume currency AND grant
/// runtime grid permissions in one transaction (via permission effects) —
/// the canonical read+write permission loop. Buying sets the plot's
/// owner_user_id; renting grants with a TTL; evict revokes. The grants are
/// enforced by the replication layer on movement/voxel writes immediately at
/// commit. Requires game-api v0.13.11+ (effects) — pair with chunk-permission
/// locks (v0.13.12+) for doors that honor the purchase.
///
/// Runtime counterpart: PlotsKit.
inline KitBlueprint plotBlueprint(const PlotBlueprintOptions& options = {}) {
  const PlotNames names = plotNames(options.typeName);
  const std::vector<std::string>& keys = options.permissionKeys;
  const std::string& currency = options.currencyProperty;
  const std::string& walletOwner = options.walletOwnerProperty;
  const bool rentable = options.rentable;
  const OwnerIdKind kind = options.ownerIdKind;

  auto param = [](const char* name, const char* valueType) {
    JVal p;
    p["name"] = name;
    p["valueType"] = valueType;
    p["required"] = true;
    return p;
  };
  auto propertyDef = [&](const char* key, const char* valueType,
                         std::optional<std::string> defaultJson) {
    JVal p;
    p["containerTypeName"] = names.plotType;
    p["key"] = key;
    p["valueType"] = valueType;
    if (defaultJson) p["defaultValueJson"] = *defaultJson;
    return p;
  };
  auto mutation = [](std::string target, const std::string& prop, std::string expression) {
    JVal m;
    m["target"] = std::move(target);
    m["property"] = prop;
    m["expression"] = std::move(expression);
    return m;
  };
  auto permissionEffect = [&](const char* action, const char* userExpression) {
    JVal e;
    e["action"] = action;
    JArray permissionKeys;
    for (const auto& k : keys) permissionKeys.push_back(JVal(k));
    e["permissionKeys"] = JVal(std::move(permissionKeys));
    e["userExpression"] = userExpression;
    e["gridIdExpression"] = "self.grid_id";
    return e;
  };
  auto walletGuard = [&](const std::string& priceExpr, const JVal& extra) {
    const std::string expression =
        ownerEqualsCaller("ref($wallet_id)." + walletOwner, kind) + " && ref($wallet_id)." +
        currency + " >= " + priceExpr;
    return kitPolicyJson(andPolicies(conditionPolicy(expression), {extra}));
  };

  KitBlueprint bp;
  bp.name = names.plotType;

  {
    JVal t;
    t["typeName"] = names.plotType;
    t["displayName"] = names.plotType;
    t["instantiableBy"] = "admin";
    t["description"] = "A sellable/rentable plot of land mapped to a world grid.";
    bp.containerTypes.push_back(std::move(t));
  }

  bp.propertyDefinitions.push_back(propertyDef("grid_id", "int", std::nullopt));
  bp.propertyDefinitions.push_back(propertyDef("price", "int", "0"));
  bp.propertyDefinitions.push_back(
      propertyDef("owner_user_id", kind == OwnerIdKind::String ? "string" : "int",
                  kind == OwnerIdKind::String ? "\"\"" : "0"));
  if (rentable) {
    bp.propertyDefinitions.push_back(propertyDef("rent_price", "int", "0"));
    bp.propertyDefinitions.push_back(propertyDef("rent_ttl_seconds", "int", "86400"));
  }

  {
    JVal fn;
    fn["name"] = names.buyFn;
    fn["containerTypeName"] = names.plotType;
    fn["returnType"] = "int";
    fn["parameters"] = JVal::array({param("wallet_id", "container_ref")});
    fn["mutations"] = JVal::array(
        {mutation("ref($wallet_id)", currency, "ref($wallet_id)." + currency + " - self.price"),
         mutation("self", "owner_user_id",
                  kind == OwnerIdKind::String ? "to_string($caller_user_id)"
                                              : "$caller_user_id")});
    fn["permissionEffects"] = JVal::array({permissionEffect("grant", "$caller_user_id")});
    fn["returnExpression"] = "ref($wallet_id)." + currency;
    fn["invokePolicyJson"] = walletGuard("self.price", options.buyPolicyExtra);
    fn["description"] = "Buy this plot: spend the price AND receive grid permissions atomically.";
    bp.functions.push_back(std::move(fn));
  }
  {
    JVal fn;
    fn["name"] = names.evictFn;
    fn["containerTypeName"] = names.plotType;
    fn["parameters"] = JVal::array({param("target_user_id", "int")});
    fn["mutations"] = JVal(JArray{});
    fn["permissionEffects"] = JVal::array({permissionEffect("revoke", "$target_user_id")});
    fn["invokePolicyJson"] =
        kitPolicyJson(conditionPolicy(ownerEqualsCaller("self.owner_user_id", kind)));
    fn["description"] =
        "Revoke a user's permissions on this plot (owner-gated; admins bypass).";
    bp.functions.push_back(std::move(fn));
  }
  if (rentable) {
    JVal fn;
    fn["name"] = names.rentFn;
    fn["containerTypeName"] = names.plotType;
    fn["returnType"] = "int";
    fn["parameters"] = JVal::array({param("wallet_id", "container_ref")});
    fn["mutations"] = JVal::array({mutation(
        "ref($wallet_id)", currency, "ref($wallet_id)." + currency + " - self.rent_price")});
    JVal effect = permissionEffect("grant", "$caller_user_id");
    effect["ttlSecondsExpression"] = "self.rent_ttl_seconds";
    fn["permissionEffects"] = JVal::array({std::move(effect)});
    fn["returnExpression"] = "ref($wallet_id)." + currency;
    fn["invokePolicyJson"] = walletGuard("self.rent_price", options.rentPolicyExtra);
    fn["description"] =
        "Rent this plot: spend the rent AND receive an expiring grid grant atomically.";
    bp.functions.push_back(std::move(fn));
  }

  return bp;
}

/// A parsed view of one plot.
struct KitPlot {
  std::string containerId;
  std::string displayName;
  std::int64_t gridId = 0;
  std::int64_t price = 0;
  /// 0 when unowned. Parsed numerically for both owner-mirror kinds: with
  /// OwnerIdKind::String blueprints the stored decimal string converts, and
  /// the for-sale sentinel "" parses to 0.
  std::int64_t ownerUserId = 0;
  std::optional<std::int64_t> rentPrice;
  std::optional<std::int64_t> rentTtlSeconds;
};

/// Input for PlotsKit::create.
struct KitPlotCreateInput {
  std::string displayName;
  /// The id of an existing grid (create it first via GameAppsAPI::createGrid).
  std::string gridId;
  std::int64_t price = 0;
  std::optional<std::int64_t> rentPrice;
  std::optional<std::int64_t> rentTtlSeconds;
  /// Extra SeedPropertyInput objects appended after the generated ones.
  JArray properties;
};

/// Runtime helpers for the plotBlueprint conventions: list/create plots and
/// drive the buy/rent/evict functions whose permission effects grant or
/// revoke real, replication-enforced grid permissions transactionally with
/// the currency mutation. Authorization (wallet ownership, price, plot
/// ownership) is enforced server-side — a denial resolves with
/// success == false, never an exception.
///
/// Obtained via GameKitClient::plots().
class PlotsKit {
 public:
  PlotsKit(std::string appId, domains::GameModelAPI& gameModel, domains::GameAppsAPI& gameApps,
           std::string_view typeName = {})
      : appId_(std::move(appId)),
        gameModel_(gameModel),
        gameApps_(gameApps),
        names_(plotNames(typeName)) {}

  const PlotNames& names() const { return names_; }

  /// Instantiate a plot over an existing grid (admin — the type is
  /// admin-instantiable). Create the grid first (GameAppsAPI::createGrid) and
  /// pass its id here.
  Json create(const KitPlotCreateInput& input) {
    JArray properties;
    properties.push_back(property("grid_id", "int", input.gridId));
    properties.push_back(property("price", "int", std::to_string(input.price)));
    if (input.rentPrice) {
      properties.push_back(property("rent_price", "int", std::to_string(*input.rentPrice)));
    }
    if (input.rentTtlSeconds) {
      properties.push_back(
          property("rent_ttl_seconds", "int", std::to_string(*input.rentTtlSeconds)));
    }
    for (const auto& p : input.properties) properties.push_back(p);

    JVal vars;
    vars["appId"] = appId_;
    vars["typeName"] = names_.plotType;
    vars["displayName"] = input.displayName;
    vars["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(vars);
  }

  /// List plots with parsed state (grid, price, current owner).
  std::vector<KitPlot> list() {
    Json containers = gameModel_.containers(appId_, names_.plotType);
    std::vector<KitPlot> out;
    containers.forEach([&](Json c) {
      Json props = kitContainerProperties(gameModel_, appId_, c["containerId"].asStringView());
      KitPlot plot;
      plot.containerId = c["containerId"].asString();
      plot.displayName = c["displayName"].asString();
      plot.gridId = props["grid_id"].asBigInt();
      plot.price = props["price"].asInt64();
      plot.ownerUserId = props["owner_user_id"].asBigInt();
      if (props["rent_price"].ok()) plot.rentPrice = props["rent_price"].asInt64();
      if (props["rent_ttl_seconds"].ok()) {
        plot.rentTtlSeconds = props["rent_ttl_seconds"].asInt64();
      }
      out.push_back(std::move(plot));
    });
    return out;
  }

  /// Buy a plot: spends the price from the caller's wallet AND grants the
  /// blueprint's grid permissions in one transaction. Returns the wallet's
  /// remaining balance.
  KitInvokeResult buy(std::string_view plotId, std::string_view walletId) {
    JVal params;
    params["wallet_id"] = walletId;
    return kitInvoke(gameModel_, appId_, names_.buyFn, plotId, params);
  }

  /// Rent a plot (blueprint deployed with rentable = true): like buy() but
  /// the grant expires after the plot's rent_ttl_seconds.
  KitInvokeResult rent(std::string_view plotId, std::string_view walletId) {
    JVal params;
    params["wallet_id"] = walletId;
    return kitInvoke(gameModel_, appId_, names_.rentFn, plotId, params);
  }

  /// Revoke a user's permissions on a plot (plot owner or app admin).
  KitInvokeResult evict(std::string_view plotId, std::int64_t targetUserId) {
    JVal params;
    params["target_user_id"] = targetUserId;
    return kitInvoke(gameModel_, appId_, names_.evictFn, plotId, params);
  }

  /// A user's effective permission keys on a plot's grid (for HUD display —
  /// enforcement happens server-side regardless).
  std::vector<std::string> accessOf(std::string_view userId, std::string_view gridId) {
    Json res = gameApps_.userPermissions(appId_, gridId, userId);
    std::vector<std::string> out;
    res["permissionKeys"].forEach([&](Json key) { out.push_back(key.asString()); });
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

  std::string appId_;
  domains::GameModelAPI& gameModel_;
  domains::GameAppsAPI& gameApps_;
  PlotNames names_;
};

}  // namespace crowdy::kit
