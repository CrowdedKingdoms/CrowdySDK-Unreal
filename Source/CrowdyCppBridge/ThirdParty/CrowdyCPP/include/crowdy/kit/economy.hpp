#pragma once

#include <algorithm>
#include <map>
#include <optional>

#include "crowdy/kit/core.hpp"
#include "crowdy/kit/engine.hpp"

/// Economy — a server-authoritative economy: per-player Wallet containers
/// (one int property per currency), an admin ShopListing catalog with an
/// atomic buy_listing (wallet debit + stock decrement + item grant in ONE
/// invoke), escrow TradeOffer swaps, and a player-to-player MarketListing
/// flow (buyer wallet -> seller wallet + stack transfer, all in one
/// transaction). Anti-duplication is structural: every movement of currency
/// or items is a single function invocation whose condition guards check
/// balances, stock, item identity, and ownership mirrors server-side.
/// Mirrors CrowdyJS's economyBlueprint / kit.economy.
namespace crowdy::kit {

/// The optional NPC-trader restock automation configuration.
struct EconomyRestockSpec {
  std::int64_t intervalMs = 0;
  int amount = 1;  ///< Units added per tick (clamped to max_stock).
};

struct EconomyBlueprintOptions {
  /// Prefix applied to the container type names ("Black" -> BlackWallet /
  /// BlackShopListing / ...) and, snake-cased, to the function names
  /// (black_earn_gold, ...). Lets several economies coexist.
  std::string typePrefix;
  /// Currency property names on the wallet, one int property (default 0) per
  /// entry, each with earn_<c> / spend_<c> functions.
  std::vector<std::string> currencies{"gold"};
  /// The wallet currency shops and market listings are priced in. Defaults
  /// to the first entry of currencies. (Expressions cannot pick a wallet
  /// property dynamically, so one deployment trades in one currency; deploy
  /// a prefixed second blueprint for a second shop currency.)
  std::string shopCurrency;
  /// Who may mint currency via earn_<c>. Trusted grants should be Server
  /// (app admins / studio backend, the default), Host, or Automation —
  /// never plain players.
  TrustedAuthority earnAuthority = TrustedAuthority::server();
  /// How owner_user_id mirrors are typed on the stack containers this
  /// economy guards (see the kit owner-mirroring convention).
  OwnerIdKind ownerIdKind = OwnerIdKind::Int;
  /// When set, adds a restock_listing automation that periodically raises
  /// every listing's stock (by amount, clamped to max_stock) — the NPC
  /// trader restock pattern.
  ///
  /// Runs only while the app has a player in it (2026-09-01), so a shop does not
  /// restock while the world is empty and does not backfill the runs it missed.
  /// Clamping to max_stock is what keeps that harmless: the shop refills to its
  /// ceiling on the first tick after somebody returns rather than staying empty.
  std::optional<EconomyRestockSpec> restock;
};

/// Names derived by economyBlueprint for a given prefix.
struct EconomyNames {
  std::string walletType;
  std::string listingType;
  std::string tradeType;
  std::string marketType;
  std::string fnPrefix;  ///< Snake-cased function-name prefix (empty without a typePrefix).
  std::string buyListingFn;
  std::string acceptTradeFn;
  std::string cancelTradeFn;
  std::string buyMarketFn;
  std::string cancelMarketFn;
  std::string restockFn;
  std::string restockAutomation;
};

/// Compute the type/function names an economy blueprint (and its runtime
/// helper) uses.
inline EconomyNames economyNames(std::string_view typePrefix = {}) {
  const std::string fnPrefix =
      typePrefix.empty() ? std::string() : toSnakeCase(typePrefix) + "_";
  EconomyNames n;
  n.walletType = std::string(typePrefix) + "Wallet";
  n.listingType = std::string(typePrefix) + "ShopListing";
  n.tradeType = std::string(typePrefix) + "TradeOffer";
  n.marketType = std::string(typePrefix) + "MarketListing";
  n.fnPrefix = fnPrefix;
  n.buyListingFn = fnPrefix + "buy_listing";
  n.acceptTradeFn = fnPrefix + "accept_trade";
  n.cancelTradeFn = fnPrefix + "cancel_trade";
  n.buyMarketFn = fnPrefix + "buy_market_listing";
  n.cancelMarketFn = fnPrefix + "cancel_market_listing";
  n.restockFn = fnPrefix + "restock_listing";
  n.restockAutomation = fnPrefix;
  std::replace(n.restockAutomation.begin(), n.restockAutomation.end(), '_', '-');
  n.restockAutomation += "shop-restock";
  return n;
}

/// The earn/spend function name for one currency (verb is "earn" or "spend").
inline std::string economyCurrencyFn(std::string_view verb, std::string_view currency,
                                     std::string_view typePrefix = {}) {
  const std::string fnPrefix =
      typePrefix.empty() ? std::string() : toSnakeCase(typePrefix) + "_";
  return fnPrefix + std::string(verb) + "_" + toSnakeCase(currency);
}

/// Build the economy blueprint. Trade/market guards verify stack ownership
/// through the kit-standard owner_user_id mirror property on the stack type
/// (the inventory blueprint defines it), and pin the offer creator via the
/// injected $self_owner_id — the server-truth owner of the offer/listing
/// container. Trusted mints (earn_<c>) default to invokeScope "server" (app
/// admins only). Runtime counterpart: EconomyKit.
inline KitBlueprint economyBlueprint(const EconomyBlueprintOptions& options = {}) {
  if (options.currencies.empty()) {
    throw std::invalid_argument("economyBlueprint requires at least one currency");
  }
  const std::string currency =
      options.shopCurrency.empty() ? options.currencies.front() : options.shopCurrency;
  if (std::find(options.currencies.begin(), options.currencies.end(), currency) ==
      options.currencies.end()) {
    throw std::invalid_argument("economyBlueprint shopCurrency '" + currency +
                                "' must be one of the declared currencies");
  }
  const EconomyNames names = economyNames(options.typePrefix);
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
  bp.name = names.walletType;

  bp.containerTypes.push_back(containerType(names.walletType, "member",
                                            "A per-player wallet holding currency balances."));
  bp.containerTypes.push_back(containerType(
      names.listingType, "admin", "A studio-priced shop listing (catalog row) players buy from."));
  bp.containerTypes.push_back(
      containerType(names.tradeType, "member",
                    "A player-to-player escrow trade offer, swapped atomically on accept."));
  bp.containerTypes.push_back(containerType(
      names.marketType, "member", "A player market listing paid straight into the seller wallet."));

  // Wallet: owner mirror + one balance property per currency.
  bp.propertyDefinitions.push_back(ownerMirrorProperty(names.walletType, kind));
  for (const auto& c : options.currencies) {
    bp.propertyDefinitions.push_back(propertyDef(names.walletType, c.c_str(), "int", "0",
                                                 "Balance of the '" + c + "' currency."));
  }
  // Shop listing catalog row.
  bp.propertyDefinitions.push_back(
      propertyDef(names.listingType, "item_id", "string", std::nullopt,
                  "The item this listing sells (matched against stack item_id)."));
  bp.propertyDefinitions.push_back(propertyDef(names.listingType, "price", "int", "0",
                                               "Price per unit in '" + currency + "'."));
  bp.propertyDefinitions.push_back(propertyDef(names.listingType, "stock", "int", "0",
                                               "Units remaining; buying decrements atomically."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.listingType, "max_stock", "int", "0",
                  "Restock ceiling used by the shop-restock automation."));
  // Escrow trade offer.
  bp.propertyDefinitions.push_back(propertyDef(names.tradeType, "to_user_id", "int", "0",
                                               "The user invited to accept this trade."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.tradeType, "give_stack_id", "string", "\"\"",
                  "Container id of the offerer's escrowed source stack."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.tradeType, "receive_stack_id", "string", "\"\"",
                  "Container id of the offerer's stack that receives the wanted items."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.tradeType, "give_item_id", "string", "\"\"", "Item the offerer gives."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.tradeType, "give_qty", "int", "0", "Quantity the offerer gives."));
  bp.propertyDefinitions.push_back(propertyDef(names.tradeType, "want_item_id", "string", "\"\"",
                                               "Item the offerer wants in return."));
  bp.propertyDefinitions.push_back(propertyDef(names.tradeType, "want_qty", "int", "0",
                                               "Quantity the offerer wants in return."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.tradeType, "status", "string", "\"open\"",
                  "Trade lifecycle: 'open' | 'accepted' | 'cancelled'."));
  // Market listing.
  bp.propertyDefinitions.push_back(
      propertyDef(names.marketType, "stack_id", "string", "\"\"",
                  "Container id of the seller's escrowed source stack."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.marketType, "item_id", "string", "\"\"", "Item being sold."));
  bp.propertyDefinitions.push_back(propertyDef(names.marketType, "quantity", "int", "0",
                                               "Units transferred to the buyer on purchase."));
  bp.propertyDefinitions.push_back(
      propertyDef(names.marketType, "price", "int", "0",
                  "Ask price in '" + currency + "' paid straight into the seller's wallet."));
  bp.propertyDefinitions.push_back(propertyDef(names.marketType, "active", "bool", "true",
                                               "False once bought or cancelled."));

  // earn/spend per currency.
  for (const auto& c : options.currencies) {
    {
      JVal fn;
      fn["name"] = economyCurrencyFn("earn", c, options.typePrefix);
      fn["containerTypeName"] = names.walletType;
      fn["returnType"] = "int";
      fn["parameters"] =
          JVal::array({param("amount", "int", "Units to add (negative values are ignored).")});
      fn["mutations"] = JVal::array({mutation("self", c, "self." + c + " + max(0, $amount)")});
      fn["returnExpression"] = "self." + c;
      applyTrustedAuthority(fn, options.earnAuthority);
      fn["description"] = "Mint '" + c +
                          "' into this wallet — a trusted grant (default: app admins via server "
                          "scope).";
      bp.functions.push_back(std::move(fn));
    }
    {
      JVal fn;
      fn["name"] = economyCurrencyFn("spend", c, options.typePrefix);
      fn["containerTypeName"] = names.walletType;
      fn["returnType"] = "int";
      fn["parameters"] = JVal::array(
          {param("amount", "int", "Units to deduct; must not exceed the balance.")});
      fn["mutations"] = JVal::array({mutation("self", c, "self." + c + " - $amount")});
      fn["returnExpression"] = "self." + c;
      fn["invokePolicyJson"] = kitPolicyJson(andPolicy(
          {ownerOfSelfPolicy(), conditionPolicy("$amount > 0 && self." + c + " >= $amount")}));
      fn["description"] =
          "Spend '" + c + "' from the caller's own wallet; refuses to overdraw.";
      bp.functions.push_back(std::move(fn));
    }
  }

  // Shop: atomic wallet debit + stock decrement + item grant.
  {
    JVal fn;
    fn["name"] = names.buyListingFn;
    fn["containerTypeName"] = names.listingType;
    fn["returnType"] = "int";
    fn["parameters"] = JVal::array(
        {param("wallet_id", "container_ref", "The buyer's wallet (must be owned by the caller)."),
         param("to_stack_id", "container_ref",
               "A stack of the listed item that receives the unit.")});
    fn["mutations"] = JVal::array(
        {mutation("ref($wallet_id)", currency, "ref($wallet_id)." + currency + " - self.price"),
         mutation("self", "stock", "self.stock - 1"),
         mutation("ref($to_stack_id)", "quantity", "ref($to_stack_id).quantity + 1")});
    fn["returnExpression"] = "ref($wallet_id)." + currency;
    fn["invokePolicyJson"] = kitPolicyJson(conditionPolicy(joinAnd(
        {"self.stock > 0", ownerEqualsCaller("ref($wallet_id).owner_user_id", kind),
         "ref($wallet_id)." + currency + " >= self.price",
         "ref($to_stack_id).item_id == self.item_id"})));
    fn["description"] =
        "Buy one unit: wallet debit, stock decrement, and item grant in one transaction — money "
        "duplication is structurally impossible.";
    bp.functions.push_back(std::move(fn));
  }

  // Escrow trade: atomic four-stack swap. $self_owner_id (injected,
  // unspoofable) is the offer creator, so a forged offer cannot drain a
  // third party's stacks.
  {
    JVal fn;
    fn["name"] = names.acceptTradeFn;
    fn["containerTypeName"] = names.tradeType;
    fn["returnType"] = "string";
    fn["parameters"] = JVal::array(
        {param("give_stack_id", "container_ref",
               "The offerer's escrowed source stack (must match the recorded give_stack_id)."),
         param("to_give_stack_id", "container_ref",
               "The acceptor's stack that receives the given items."),
         param("want_stack_id", "container_ref",
               "The acceptor's stack that pays the wanted items."),
         param("to_want_stack_id", "container_ref",
               "The offerer's stack that receives the wanted items (must match the recorded "
               "receive_stack_id).")});
    fn["mutations"] = JVal::array(
        {mutation("ref($give_stack_id)", "quantity",
                  "ref($give_stack_id).quantity - self.give_qty"),
         mutation("ref($to_give_stack_id)", "quantity",
                  "ref($to_give_stack_id).quantity + self.give_qty"),
         mutation("ref($want_stack_id)", "quantity",
                  "ref($want_stack_id).quantity - self.want_qty"),
         mutation("ref($to_want_stack_id)", "quantity",
                  "ref($to_want_stack_id).quantity + self.want_qty"),
         mutation("self", "status", "\"accepted\"")});
    fn["returnExpression"] = "self.status";
    fn["invokePolicyJson"] = kitPolicyJson(conditionPolicy(joinAnd(
        {"self.status == \"open\"", "self.to_user_id == $caller_user_id",
         "$give_stack_id == self.give_stack_id", "$to_want_stack_id == self.receive_stack_id",
         ownerEquals("ref($give_stack_id).owner_user_id", "$self_owner_id", kind),
         "ref($give_stack_id).item_id == self.give_item_id",
         "ref($give_stack_id).quantity >= self.give_qty",
         ownerEqualsCaller("ref($to_give_stack_id).owner_user_id", kind),
         "ref($to_give_stack_id).item_id == self.give_item_id",
         ownerEqualsCaller("ref($want_stack_id).owner_user_id", kind),
         "ref($want_stack_id).item_id == self.want_item_id",
         "ref($want_stack_id).quantity >= self.want_qty",
         ownerEquals("ref($to_want_stack_id).owner_user_id", "$self_owner_id", kind),
         "ref($to_want_stack_id).item_id == self.want_item_id"})));
    fn["description"] =
        "Accept an escrow trade: both stack pairs swap atomically after ownership/item/quantity "
        "guards; any failure rolls the whole swap back.";
    bp.functions.push_back(std::move(fn));
  }
  {
    JVal fn;
    fn["name"] = names.cancelTradeFn;
    fn["containerTypeName"] = names.tradeType;
    fn["returnType"] = "string";
    fn["mutations"] = JVal::array({mutation("self", "status", "\"cancelled\"")});
    fn["returnExpression"] = "self.status";
    fn["invokePolicyJson"] = kitPolicyJson(conditionPolicy(
        "self.status == \"open\" && ($self_owner_id == $caller_user_id || self.to_user_id == "
        "$caller_user_id)"));
    fn["description"] = "Cancel an open trade (either party may).";
    bp.functions.push_back(std::move(fn));
  }

  // Market: wallet->wallet payment + stack transfer, one invoke, 5 mutations.
  {
    JVal fn;
    fn["name"] = names.buyMarketFn;
    fn["containerTypeName"] = names.marketType;
    fn["returnType"] = "int";
    fn["parameters"] = JVal::array(
        {param("wallet_id", "container_ref", "The buyer's wallet (must be owned by the caller)."),
         param("seller_wallet_id", "container_ref",
               "The seller's wallet that receives the payment."),
         param("from_stack_id", "container_ref",
               "The seller's escrowed source stack (must match the recorded stack_id)."),
         param("to_stack_id", "container_ref", "The buyer's stack that receives the items.")});
    fn["mutations"] = JVal::array(
        {mutation("ref($wallet_id)", currency, "ref($wallet_id)." + currency + " - self.price"),
         mutation("ref($seller_wallet_id)", currency,
                  "ref($seller_wallet_id)." + currency + " + self.price"),
         mutation("ref($from_stack_id)", "quantity",
                  "ref($from_stack_id).quantity - self.quantity"),
         mutation("ref($to_stack_id)", "quantity", "ref($to_stack_id).quantity + self.quantity"),
         mutation("self", "active", "false")});
    fn["returnExpression"] = "ref($wallet_id)." + currency;
    fn["invokePolicyJson"] = kitPolicyJson(conditionPolicy(joinAnd(
        {"self.active", ownerEqualsCaller("ref($wallet_id).owner_user_id", kind),
         "ref($wallet_id)." + currency + " >= self.price",
         ownerEquals("ref($seller_wallet_id).owner_user_id", "$self_owner_id", kind),
         "$from_stack_id == self.stack_id",
         ownerEquals("ref($from_stack_id).owner_user_id", "$self_owner_id", kind),
         "ref($from_stack_id).item_id == self.item_id",
         "ref($from_stack_id).quantity >= self.quantity",
         ownerEqualsCaller("ref($to_stack_id).owner_user_id", kind),
         "ref($to_stack_id).item_id == self.item_id"})));
    fn["description"] =
        "Buy a market listing: buyer wallet → seller wallet payment plus the stack transfer, all "
        "atomic; the listing deactivates in the same transaction.";
    bp.functions.push_back(std::move(fn));
  }
  {
    JVal fn;
    fn["name"] = names.cancelMarketFn;
    fn["containerTypeName"] = names.marketType;
    fn["returnType"] = "bool";
    fn["mutations"] = JVal::array({mutation("self", "active", "false")});
    fn["returnExpression"] = "self.active";
    fn["invokePolicyJson"] = kitPolicyJson(
        conditionPolicy("self.active && $self_owner_id == $caller_user_id"));
    fn["description"] = "Take a market listing down (seller only).";
    bp.functions.push_back(std::move(fn));
  }

  if (options.restock) {
    {
      JVal fn;
      fn["name"] = names.restockFn;
      fn["containerTypeName"] = names.listingType;
      fn["returnType"] = "int";
      fn["parameters"] = JVal::array(
          {param("amount", "int", "Units to add per tick (clamped to max_stock).")});
      fn["mutations"] = JVal::array(
          {mutation("self", "stock", "min(self.max_stock, self.stock + max(0, $amount))")});
      fn["returnExpression"] = "self.stock";
      fn["invokePolicyJson"] = kitPolicyJson(isAutomationPolicy());
      fn["autonomousInvocable"] = true;
      fn["description"] = "Server-driven shop restock tick (automation-only).";
      bp.functions.push_back(std::move(fn));
    }
    JVal automation;
    automation["name"] = names.restockAutomation;
    automation["functionName"] = names.restockFn;
    automation["targetMode"] = "type";
    automation["targetTypeName"] = names.listingType;
    automation["triggerType"] = "schedule";
    automation["scheduleKind"] = "interval";
    automation["intervalMs"] = options.restock->intervalMs;
    automation["maxTargets"] = 32;
    {
      JVal where;
      where["key"] = "stock";
      where["op"] = "<";
      where["value"] = "self.max_stock";
      JVal selector;
      selector["selfWhere"] = JVal::array({std::move(where)});
      automation["selectorJson"] = selector.dump();
    }
    {
      JVal params;
      params["amount"] = std::int64_t{options.restock->amount};
      automation["paramsJson"] = params.dump();
    }
    automation["description"] = "Periodically restocks shop listings below their max_stock.";
    bp.automations.push_back(std::move(automation));
  }

  return bp;
}

// ---------------------------------------------------------------------------
// Runtime kit
// ---------------------------------------------------------------------------

/// Options for EconomyKit. Must match the deployed economy blueprint.
struct EconomyKitOptions {
  std::string typePrefix;                  ///< The typePrefix the blueprint was deployed with.
  std::vector<std::string> currencies{"gold"};  ///< The currencies it was deployed with.
};

/// A parsed view of one wallet.
struct KitWallet {
  std::string containerId;
  std::string displayName;
  std::string ownerUserId;
  std::map<std::string, std::int64_t> balances;  ///< Balance per currency property.
};

/// A parsed view of one shop listing.
struct KitShopListing {
  std::string containerId;
  std::string displayName;
  std::string itemId;
  std::int64_t price = 0;
  std::int64_t stock = 0;
  std::int64_t maxStock = 0;
};

/// A parsed view of one escrow trade offer.
struct KitTradeOffer {
  std::string containerId;
  std::string displayName;
  std::string fromUserId;  ///< The offer creator (server-assigned container owner).
  std::int64_t toUserId = 0;
  std::string giveStackId;
  std::string receiveStackId;
  std::string giveItemId;
  std::int64_t giveQty = 0;
  std::string wantItemId;
  std::int64_t wantQty = 0;
  std::string status;
};

/// A parsed view of one market listing.
struct KitMarketListing {
  std::string containerId;
  std::string displayName;
  std::string sellerUserId;  ///< The seller (server-assigned container owner).
  std::string stackId;
  std::string itemId;
  std::int64_t quantity = 0;
  std::int64_t price = 0;
  bool active = false;
};

/// Input for EconomyKit::tradeOffer. The stacks named here are the OFFERER's.
struct KitTradeOfferCreate {
  std::string toUserId;  ///< The invited player's user id (decimal string).
  std::string giveStackId;
  std::string giveItemId;
  std::int64_t giveQty = 0;
  std::string wantItemId;
  std::int64_t wantQty = 0;
  std::string receiveStackId;
  std::string displayName;  ///< Optional; derived from the items when empty.
  std::string sessionId;    ///< Optional.
};

/// Runtime helpers for the economyBlueprint conventions: wallets with
/// per-currency balances, shop purchases, escrow trades, and player market
/// listings. Every movement of currency or items is a single gated invoke —
/// balances, stock, item identity, and ownership are all verified
/// server-side, and a denial resolves with success=false (never an
/// exception). earn is a trusted grant: with the default blueprint authority
/// (Server) it succeeds only for app admins — call it from studio/backend
/// code, or drive grants through automations instead.

/// Order-book market methods over the market-engine (Wave 2): price-time
/// priority matching with escrowed settlement. Deposits/withdrawals bridge
/// to the game's wallets via compute events. Obtained via
/// EconomyKit::orderBook().
class MarketKit {
 public:
  MarketKit(EngineDetector* engines, std::string moduleName)
      : engines_(engines), moduleName_(std::move(moduleName)) {}

  /// Is the market engine deployed + enabled (cached per client)?
  bool engineAvailable() { return engines_ != nullptr && engines_->has(moduleName_); }

  /// Move coins into your market account (escrow source for bids).
  Json depositCoins(std::int64_t amount) {
    JVal params;
    params["amount"] = amount;
    return invoke("deposit_coins", params);
  }

  /// Move items into your market account (escrow source for asks).
  Json depositItems(std::string_view item, std::int64_t quantity) {
    JVal params;
    params["item"] = item;
    params["quantity"] = quantity;
    return invoke("deposit_items", params);
  }

  /// Place a limit bid: locks coins at your limit; fills at maker price.
  Json bid(std::string_view item, std::int64_t price, std::int64_t quantity) {
    return place(item, "buy", price, quantity);
  }

  /// Place a limit ask: locks items until filled or cancelled.
  Json ask(std::string_view item, std::int64_t price, std::int64_t quantity) {
    return place(item, "sell", price, quantity);
  }

  /// Cancel a resting order (owner only); escrow refunds instantly.
  Json cancel(std::string_view item, std::int64_t orderId) {
    JVal params;
    params["item"] = item;
    params["orderId"] = orderId;
    return invoke("cancel", params);
  }

  /// Book depth: best bids/asks as [price, quantity] levels.
  Json book(std::string_view item) {
    JVal params;
    params["item"] = item;
    return invoke("book", params);
  }

  /// Your market account (settled + locked balances).
  Json account() { return invoke("account", JVal()); }

  /// Withdraw settled balances back to the game layer.
  Json withdraw(std::optional<std::int64_t> coins = std::nullopt, std::string_view item = {},
                std::optional<std::int64_t> quantity = std::nullopt) {
    JVal params;
    if (coins) params["coins"] = *coins;
    if (!item.empty()) params["item"] = item;
    if (quantity) params["quantity"] = *quantity;
    return invoke("withdraw", params);
  }

 private:
  Json place(std::string_view item, std::string_view side, std::int64_t price,
             std::int64_t quantity) {
    JVal params;
    params["item"] = item;
    params["side"] = side;
    params["price"] = price;
    params["quantity"] = quantity;
    return invoke("place", params);
  }

  Json invoke(std::string_view exportName, const JVal& params) {
    if (engines_ == nullptr) {
      throw std::runtime_error("market engine unavailable: compute domain not wired");
    }
    EngineInvokeResult result = engines_->invoke(moduleName_, exportName, params);
    if (!result.success) {
      throw std::runtime_error("market." + std::string(exportName) + " failed: " + result.reason);
    }
    return result.body;
  }

  EngineDetector* engines_ = nullptr;
  std::string moduleName_;
};

class EconomyKit {
 public:
  EconomyKit(std::string appId, domains::GameModelAPI& gameModel, EconomyKitOptions options = {},
             EngineDetector* engines = nullptr)
      : appId_(std::move(appId)),
        gameModel_(gameModel),
        orderBook_(engines, "market-engine"),
        typePrefix_(std::move(options.typePrefix)),
        currencies_(std::move(options.currencies)),
        names_(economyNames(typePrefix_)) {}

  const EconomyNames& names() const { return names_; }

  /// The order-book market (Wave 2 engine): escrowed bid/ask price
  /// discovery; the model-side fixed-price market listings keep working
  /// without it.
  MarketKit& orderBook() { return orderBook_; }

  /// Find the player's wallet, creating it when absent (member-instantiable;
  /// the server assigns ownership to the caller). Sets the owner_user_id
  /// mirror property the blueprint's guards read.
  Json ensureWallet(std::string_view ownerUserId, std::string_view displayName = {},
                    std::string_view sessionId = {}) {
    Json existing = gameModel_.containers(appId_, names_.walletType, sessionId);
    Json mine;
    existing.forEach([&](Json c) {
      if (!mine.isNull()) return;
      if (c["ownerUserId"].asString() == ownerUserId) mine = c;
    });
    if (mine.ok() && !mine.isNull()) return mine;

    JVal input;
    input["appId"] = appId_;
    input["typeName"] = names_.walletType;
    input["displayName"] = displayName.empty() ? "Wallet " + std::string(ownerUserId)
                                               : std::string(displayName);
    if (!sessionId.empty()) input["sessionId"] = sessionId;
    input["properties"] =
        JVal::array({property("owner_user_id", "int", std::string(ownerUserId))});
    return gameModel_.createContainer(input);
  }

  /// Read one currency balance (default: the configured first currency).
  std::int64_t balance(std::string_view walletId, std::string_view currency = {}) {
    Json props = kitContainerProperties(gameModel_, appId_, walletId);
    return props[currency.empty() ? defaultCurrency() : currency].asInt64();
  }

  /// Read a wallet with every configured currency balance parsed.
  KitWallet wallet(std::string_view walletId) {
    Json container = gameModel_.container(appId_, walletId);
    Json props = kitContainerProperties(gameModel_, appId_, walletId);
    KitWallet w;
    w.containerId = container["containerId"].asString();
    w.displayName = container["displayName"].asString();
    w.ownerUserId = container["ownerUserId"].asString();
    for (const auto& c : currencies_) w.balances[c] = props[c].asInt64();
    return w;
  }

  /// Mint currency into a wallet — a TRUSTED grant (default blueprint
  /// authority: app admins only). Returns the new balance.
  KitInvokeResult earn(std::string_view walletId, std::int64_t amount,
                       std::string_view currency = {}) {
    JVal params;
    params["amount"] = amount;
    return kitInvoke(
        gameModel_, appId_,
        economyCurrencyFn("earn", currency.empty() ? defaultCurrency() : currency, typePrefix_),
        walletId, params);
  }

  /// Spend currency from the caller's own wallet. The server refuses to
  /// overdraw. Returns the new balance.
  KitInvokeResult spend(std::string_view walletId, std::int64_t amount,
                        std::string_view currency = {}) {
    JVal params;
    params["amount"] = amount;
    return kitInvoke(
        gameModel_, appId_,
        economyCurrencyFn("spend", currency.empty() ? defaultCurrency() : currency, typePrefix_),
        walletId, params);
  }

  // ----- Shop (admin-priced listings; atomic buys) ---------------------------

  /// Create a shop listing (admin — the type is admin-instantiable).
  /// maxStock (default: stock) feeds the optional restock automation.
  Json shopCreate(std::string_view displayName, std::string_view itemId, std::int64_t price,
                  std::int64_t stock = 0, std::optional<std::int64_t> maxStock = std::nullopt,
                  JArray extraProperties = {}) {
    JVal input;
    input["appId"] = appId_;
    input["typeName"] = names_.listingType;
    input["displayName"] = displayName;
    JArray properties;
    properties.push_back(property("item_id", "string", JVal(itemId).dump()));
    properties.push_back(property("price", "int", std::to_string(price)));
    properties.push_back(property("stock", "int", std::to_string(stock)));
    properties.push_back(
        property("max_stock", "int", std::to_string(maxStock ? *maxStock : stock)));
    for (auto& p : extraProperties) properties.push_back(std::move(p));
    input["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(input);
  }

  /// List shop listings with parsed state.
  std::vector<KitShopListing> shopList() {
    Json containers = gameModel_.containers(appId_, names_.listingType);
    std::vector<KitShopListing> out;
    containers.forEach([&](Json c) {
      Json props = kitContainerProperties(gameModel_, appId_, c["containerId"].asStringView());
      KitShopListing l;
      l.containerId = c["containerId"].asString();
      l.displayName = c["displayName"].asString();
      l.itemId = props["item_id"].asString();
      l.price = props["price"].asInt64();
      l.stock = props["stock"].asInt64();
      l.maxStock = props["max_stock"].asInt64();
      out.push_back(std::move(l));
    });
    return out;
  }

  /// Buy one unit: wallet debit + stock decrement + item grant into
  /// toStackId (a stack of the listed item), all in one transaction.
  /// Returns the wallet's remaining balance.
  KitInvokeResult shopBuy(std::string_view listingId, std::string_view walletId,
                          std::string_view toStackId) {
    JVal params;
    params["wallet_id"] = walletId;
    params["to_stack_id"] = toStackId;
    return kitInvoke(gameModel_, appId_, names_.buyListingFn, listingId, params);
  }

  // ----- Escrow trades (player<->player item swaps, atomic on accept) --------

  /// Create a trade offer to another player. giveStackId is the offerer's
  /// escrowed source, receiveStackId receives the wanted items on accept.
  /// The offer's container ownership (server-assigned to the caller) is what
  /// the accept guards trust — a forged offer over someone else's stacks can
  /// never be accepted.
  Json tradeOffer(const KitTradeOfferCreate& input) {
    JVal vars;
    vars["appId"] = appId_;
    vars["typeName"] = names_.tradeType;
    vars["displayName"] =
        input.displayName.empty()
            ? "Trade " + std::to_string(input.giveQty) + "x " + input.giveItemId + " for " +
                  std::to_string(input.wantQty) + "x " + input.wantItemId
            : input.displayName;
    if (!input.sessionId.empty()) vars["sessionId"] = input.sessionId;
    JArray properties;
    properties.push_back(property("to_user_id", "int", input.toUserId));
    properties.push_back(property("give_stack_id", "string", JVal(input.giveStackId).dump()));
    properties.push_back(
        property("receive_stack_id", "string", JVal(input.receiveStackId).dump()));
    properties.push_back(property("give_item_id", "string", JVal(input.giveItemId).dump()));
    properties.push_back(property("give_qty", "int", std::to_string(input.giveQty)));
    properties.push_back(property("want_item_id", "string", JVal(input.wantItemId).dump()));
    properties.push_back(property("want_qty", "int", std::to_string(input.wantQty)));
    vars["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(vars);
  }

  /// Accept a trade as the invited player, supplying YOUR two stacks: the
  /// one paying the wanted items (wantStackId) and the one receiving the
  /// given items (toGiveStackId). The offerer's stacks come from the offer
  /// record. All four quantity writes commit atomically or not at all.
  KitInvokeResult tradeAccept(std::string_view offerId, std::string_view wantStackId,
                              std::string_view toGiveStackId) {
    KitTradeOffer offer = trade(offerId);
    JVal params;
    params["give_stack_id"] = offer.giveStackId;
    params["to_give_stack_id"] = toGiveStackId;
    params["want_stack_id"] = wantStackId;
    params["to_want_stack_id"] = offer.receiveStackId;
    return kitInvoke(gameModel_, appId_, names_.acceptTradeFn, offerId, params);
  }

  /// Cancel an open trade (either party may).
  KitInvokeResult tradeCancel(std::string_view offerId) {
    return kitInvoke(gameModel_, appId_, names_.cancelTradeFn, offerId);
  }

  /// Read one trade offer with parsed state.
  KitTradeOffer trade(std::string_view offerId) {
    Json container = gameModel_.container(appId_, offerId);
    return toTrade(container["containerId"].asString(), container["displayName"].asString(),
                   container["ownerUserId"].asString());
  }

  /// List trades the user created or was invited to (open ones first).
  std::vector<KitTradeOffer> myTrades(std::string_view userId) {
    Json containers = gameModel_.containers(appId_, names_.tradeType);
    std::vector<KitTradeOffer> out;
    containers.forEach([&](Json c) {
      KitTradeOffer t = toTrade(c["containerId"].asString(), c["displayName"].asString(),
                                c["ownerUserId"].asString());
      if (t.fromUserId == userId || std::to_string(t.toUserId) == userId)
        out.push_back(std::move(t));
    });
    std::stable_sort(out.begin(), out.end(), [](const KitTradeOffer& a, const KitTradeOffer& b) {
      return a.status == "open" && b.status != "open";
    });
    return out;
  }

  // ----- Player market (list a stack for currency; atomic purchases) ---------

  /// List items for sale: names YOUR escrowed source stack and the ask
  /// price. Payment lands straight in your wallet when someone buys.
  Json marketList(std::string_view stackId, std::string_view itemId, std::int64_t quantity,
                  std::int64_t price, std::string_view displayName = {},
                  std::string_view sessionId = {}) {
    JVal input;
    input["appId"] = appId_;
    input["typeName"] = names_.marketType;
    input["displayName"] = displayName.empty()
                               ? std::to_string(quantity) + "x " + std::string(itemId) + " for " +
                                     std::to_string(price)
                               : std::string(displayName);
    if (!sessionId.empty()) input["sessionId"] = sessionId;
    JArray properties;
    properties.push_back(property("stack_id", "string", JVal(stackId).dump()));
    properties.push_back(property("item_id", "string", JVal(itemId).dump()));
    properties.push_back(property("quantity", "int", std::to_string(quantity)));
    properties.push_back(property("price", "int", std::to_string(price)));
    input["properties"] = JVal(std::move(properties));
    return gameModel_.createContainer(input);
  }

  /// Browse market listings (active ones only by default).
  std::vector<KitMarketListing> marketBrowse(bool includeInactive = false) {
    Json containers = gameModel_.containers(appId_, names_.marketType);
    std::vector<KitMarketListing> out;
    containers.forEach([&](Json c) {
      Json props = kitContainerProperties(gameModel_, appId_, c["containerId"].asStringView());
      KitMarketListing l;
      l.containerId = c["containerId"].asString();
      l.displayName = c["displayName"].asString();
      l.sellerUserId = c["ownerUserId"].asString();
      l.stackId = props["stack_id"].asString();
      l.itemId = props["item_id"].asString();
      l.quantity = props["quantity"].asInt64();
      l.price = props["price"].asInt64();
      l.active = props["active"].asBool();
      if (includeInactive || l.active) out.push_back(std::move(l));
    });
    return out;
  }

  /// Buy a market listing: pays the seller's wallet and transfers the items
  /// into toStackId in one transaction. The seller's wallet and source stack
  /// are resolved from the listing. Returns the buyer wallet's remaining
  /// balance. Throws std::runtime_error when the listing is unowned or the
  /// seller has no wallet.
  KitInvokeResult marketBuy(std::string_view listingId, std::string_view walletId,
                            std::string_view toStackId) {
    Json container = gameModel_.container(appId_, listingId);
    Json props = kitContainerProperties(gameModel_, appId_, listingId);
    const std::string sellerUserId = container["ownerUserId"].asString();
    if (sellerUserId.empty()) {
      throw std::runtime_error("Market listing has no seller (unowned container)");
    }
    const std::string sellerWallet = sellerWalletId(sellerUserId);
    JVal params;
    params["wallet_id"] = walletId;
    params["seller_wallet_id"] = sellerWallet;
    params["from_stack_id"] = props["stack_id"].asString();
    params["to_stack_id"] = toStackId;
    return kitInvoke(gameModel_, appId_, names_.buyMarketFn, listingId, params);
  }

  /// Take a listing down (seller only).
  KitInvokeResult marketCancel(std::string_view listingId) {
    return kitInvoke(gameModel_, appId_, names_.cancelMarketFn, listingId);
  }

 private:
  static JVal property(const char* key, const char* valueType, std::string valueJson) {
    JVal p;
    p["key"] = key;
    p["valueType"] = valueType;
    p["valueJson"] = std::move(valueJson);
    return p;
  }

  const std::string& defaultCurrency() const { return currencies_.front(); }

  /// Find an existing wallet container id for a user (no creation).
  std::string sellerWalletId(const std::string& sellerUserId) {
    Json wallets = gameModel_.containers(appId_, names_.walletType);
    std::string found;
    wallets.forEach([&](Json c) {
      if (!found.empty()) return;
      if (c["ownerUserId"].asString() == sellerUserId)
        found = c["containerId"].asString();
    });
    if (found.empty()) {
      throw std::runtime_error("Seller " + sellerUserId + " has no " + names_.walletType +
                               "; they must ensureWallet() before selling");
    }
    return found;
  }

  KitTradeOffer toTrade(std::string containerId, std::string displayName,
                        std::string ownerUserId) {
    Json props = kitContainerProperties(gameModel_, appId_, containerId);
    KitTradeOffer t;
    t.containerId = std::move(containerId);
    t.displayName = std::move(displayName);
    t.fromUserId = std::move(ownerUserId);
    t.toUserId = props["to_user_id"].asInt64();
    t.giveStackId = props["give_stack_id"].asString();
    t.receiveStackId = props["receive_stack_id"].asString();
    t.giveItemId = props["give_item_id"].asString();
    t.giveQty = props["give_qty"].asInt64();
    t.wantItemId = props["want_item_id"].asString();
    t.wantQty = props["want_qty"].asInt64();
    t.status = props["status"].asString();
    return t;
  }

  std::string appId_;
  domains::GameModelAPI& gameModel_;
  MarketKit orderBook_;
  std::string typePrefix_;
  std::vector<std::string> currencies_;
  EconomyNames names_;
};

}  // namespace crowdy::kit
