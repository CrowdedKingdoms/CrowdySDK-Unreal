#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "crowdy/kit/mirror.hpp"
#include "crowdy/kit/runtime.hpp"

/// The kit's economy: the caller's own `player` hub (inventory, wallet, progression,
/// achievements, quests, crafting, loot, production) and markets (vendors, order books, trades).
/// CrowdyJS `src/kit/modules/player.ts` and `market.ts`. It lives in hub state and is not a
/// ledger: a transfer between players is steps each hub answers once.
namespace crowdy::kit {

/// The caller's own `player` hub, keyed by their player id (the runtime's `playerId`). Its
/// stores exist from the start and fill once watch() has read them; inventory, wallet and
/// progress are read by calls only the player may make, again whenever the hub says they changed.
class PlayerKit {
 public:
  /// `key` names the hub; empty for the runtime's player id.
  PlayerKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType = "player", std::string key = "");
  ~PlayerKit();
  PlayerKit(const PlayerKit&) = delete;
  PlayerKit& operator=(const PlayerKit&) = delete;

  /// The hub's key: the player's id, or "" when the kit does not know it.
  std::string key() const;

  /// The `player` topic: `{ player, level, achievements, quests_done }`, public.
  WritableKitStore<graphql::Json>& profile();
  /// `{ slots, stacks, mail, held }`.
  WritableKitStore<graphql::Json>& inventory();
  /// `{ balances, held }`.
  WritableKitStore<graphql::Json>& wallet();
  /// `{ level, xp, next, total_xp, achievements, quests }`.
  WritableKitStore<graphql::Json>& progress();

  /// Mirrors the profile and reads what only the player reads (`which`: any of `profile`,
  /// `inventory`, `wallet`, `progress`; all when empty). `done` fires once each has a value.
  void watch(KitCallback done = {}, std::vector<std::string> which = {});
  void unwatch();
  /// Levels gained and achievements unlocked: `{ type: "level", level }`, `{ type: "achievement", achievement }`.
  std::uint64_t onEvent(std::function<void(const graphql::Json&)> listener);
  void removeListener(std::uint64_t id);

  /// Calls the player's hub; a change carries an action id.
  void call(std::string method, graphql::JVal args, KitCallback done, bool change = true);
  /// Everything the hub keeps for the player.
  void me(KitCallback done);
  /// The game's items, recipes, loot tables, quests and achievements.
  void rules(KitCallback done);
  /// Another player's public profile.
  void profileOf(std::int64_t player, KitCallback done);

 private:
  struct Core;
  std::shared_ptr<Core> core_;
};

/// The player's items: stacks in slots, and mail for what did not fit.
class InventoryKit {
 public:
  explicit InventoryKit(PlayerKit& player) : player_(player) {}
  WritableKitStore<graphql::Json>& items() { return player_.inventory(); }
  void watch(KitCallback done = {}) { player_.watch(std::move(done), {"inventory"}); }
  /// How many of `item` the player has, as last read.
  std::int64_t count(const std::string& item) const;
  /// Refusal: `insufficient`.
  void discard(std::string item, std::int64_t count, KitCallback done);
  /// Moves what fits from the mail into the inventory; the reply is what moved.
  void takeMail(KitCallback done);

 private:
  PlayerKit& player_;
};

/// The player's soft currencies.
class WalletKit {
 public:
  explicit WalletKit(PlayerKit& player) : player_(player) {}
  WritableKitStore<graphql::Json>& balances() { return player_.wallet(); }
  void watch(KitCallback done = {}) { player_.watch(std::move(done), {"wallet"}); }
  /// The balance of `currency`, as last read.
  std::int64_t balance(const std::string& currency) const;

 private:
  PlayerKit& player_;
};

/// Experience and levels, and public profiles.
class ProgressionKit {
 public:
  explicit ProgressionKit(PlayerKit& player) : player_(player) {}
  WritableKitStore<graphql::Json>& progress() { return player_.progress(); }
  WritableKitStore<graphql::Json>& profile() { return player_.profile(); }
  void watch(KitCallback done = {}) { player_.watch(std::move(done), {"profile", "progress"}); }
  void profileOf(std::int64_t player, KitCallback done) { player_.profileOf(player, std::move(done)); }
  /// Called with each level the player reaches.
  std::uint64_t onLevel(std::function<void(std::int64_t)> listener);

 private:
  PlayerKit& player_;
};

/// Achievements: counts of what the player did, unlocked once.
class AchievementsKit {
 public:
  explicit AchievementsKit(PlayerKit& player) : player_(player) {}
  void watch(KitCallback done = {}) { player_.watch(std::move(done), {"profile", "progress"}); }
  /// `[{ id, counter, target, count, unlocked_at }]`, as last read.
  std::vector<graphql::Json> list() const;
  /// Called with each achievement the player unlocks.
  std::uint64_t onUnlock(std::function<void(const std::string&)> listener);

 private:
  PlayerKit& player_;
};

/// Quests, first-session tutorials among them.
class QuestsKit {
 public:
  explicit QuestsKit(PlayerKit& player) : player_(player) {}
  void watch(KitCallback done = {}) { player_.watch(std::move(done), {"progress"}); }
  /// `[{ id, state, objectives, reward, xp, tutorial }]`, as last read.
  std::vector<graphql::Json> list() const;
  /// Refusals: `conflict`, `forbidden` (level), `not_found`.
  void accept(std::string quest, KitCallback done);
  /// Claims a complete quest's reward. Refusals: `not_ready`, `full`.
  void claim(std::string quest, KitCallback done);
  /// Reports a counter the game lets a client report (a tutorial step). Refusal: `forbidden`.
  void step(std::string counter, KitCallback done);

 private:
  PlayerKit& player_;
};

/// Crafting from the game's recipes.
class CraftingKit {
 public:
  explicit CraftingKit(PlayerKit& player) : player_(player) {}
  /// The game's recipes (from the hub's rules).
  void recipes(KitCallback done);
  /// Crafts `times` (once without). Refusals: `insufficient`, `full`, `forbidden`, `not_found`.
  void craft(std::string recipe, std::optional<std::int64_t> times, KitCallback done);

 private:
  PlayerKit& player_;
};

/// Loot tables with pity.
class LootKit {
 public:
  explicit LootKit(PlayerKit& player) : player_(player) {}
  void tables(KitCallback done);
  /// Pays the table's cost and rolls once. Refusals: `insufficient`, `full`, `not_found`.
  void open(std::string table, KitCallback done);

 private:
  PlayerKit& player_;
};

/// Idle production that catches up on the time away.
class ProductionKit {
 public:
  explicit ProductionKit(PlayerKit& player) : player_(player) {}
  /// `{ producers, stock, capacity }` (from `me`).
  void view(KitCallback done);
  /// Moves what the producers made into the inventory, as far as it fits.
  void collect(KitCallback done);

 private:
  PlayerKit& player_;
};

// ---- markets ----

/// One market hub's mirrors and calls, shared by its market, shop and trade handles.
class MarketCore {
 public:
  MarketCore(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key);

  /// The `market` topic: `{ vendors, currency, quotes, open_trades, pending_steps }`.
  HubState& view() { return view_; }
  /// The caller's open orders.
  PrivateState& orders() { return orders_; }
  /// The caller's open and settling trades.
  PrivateState& trades() { return trades_; }
  /// A change, with an action id.
  void change(std::string method, graphql::JVal args, KitCallback done);
  void read(std::string method, graphql::JVal args, KitCallback done);

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_, key_;
  HubState view_;
  PrivateState orders_, trades_;
};

/// A limit order: a buy holds `price * count` of the market's currency, a sell the items.
struct MarketOrderRequest {
  std::string item;
  /// `buy` or `sell`.
  std::string side;
  std::int64_t price = 0;
  std::int64_t count = 0;
};

/// A market's order books.
class MarketHandle {
 public:
  explicit MarketHandle(std::shared_ptr<MarketCore> core) : core_(std::move(core)) {}
  HubState& view() { return core_->view(); }
  PrivateState& orders() { return core_->orders(); }
  void watch(KitCallback done = {});
  void unwatch();
  /// `{ item, currency, bids, asks, last }`.
  void book(std::string item, KitCallback done);
  /// Refusals: `insufficient`, `limit`, `not_found`, `unavailable`.
  void order(const MarketOrderRequest& order, KitCallback done);
  /// Refusal: `not_found`.
  void cancel(std::int64_t order, KitCallback done);

 private:
  std::shared_ptr<MarketCore> core_;
};

/// A market's vendors: fixed prices, stock that restocks, a limit per player.
class ShopHandle {
 public:
  explicit ShopHandle(std::shared_ptr<MarketCore> core) : core_(std::move(core)) {}
  HubState& view() { return core_->view(); }
  void watch(KitCallback done = {}) { core_->view().start(std::move(done)); }
  void unwatch() { core_->view().stop(); }
  void vendors(KitCallback done);
  /// Buys `count` (one without). Refusals: `insufficient`, `limit`, `full`, `not_found`, `unavailable`.
  void buy(std::string vendor, std::string item, std::optional<std::int64_t> count, KitCallback done);
  /// Refusals: `forbidden` (it does not buy them), `insufficient`, `unavailable`.
  void sell(std::string vendor, std::string item, std::optional<std::int64_t> count, KitCallback done);

 private:
  std::shared_ptr<MarketCore> core_;
};

/// Trades between two players: each offers a bundle, held in their own hub; when both accept
/// the last offers, the market moves each to the other.
class TradeDesk {
 public:
  explicit TradeDesk(std::shared_ptr<MarketCore> core) : core_(std::move(core)) {}
  PrivateState& mine() { return core_->trades(); }
  void watch(KitCallback done = {}) { core_->trades().start(std::move(done)); }
  void unwatch() { core_->trades().stop(); }
  /// Opens a trade with `player`; the reply is its id.
  void open(std::int64_t player, KitCallback done);
  /// Offers `bundle` `{ items, currency }`. Refusals: `insufficient`, `forbidden`, `finished`.
  void offer(std::int64_t trade, graphql::JVal bundle, KitCallback done);
  /// The reply is the trade's state. Refusals: `not_ready`, `finished`.
  void accept(std::int64_t trade, KitCallback done);
  void decline(std::int64_t trade, KitCallback done);

 private:
  std::shared_ptr<MarketCore> core_;
};

/// The cores of a kit's markets, one per key.
class MarketCores {
 public:
  MarketCores(std::shared_ptr<KitRuntime> runtime, std::string nodeType) : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)) {}
  std::shared_ptr<MarketCore> at(const std::string& key);

 private:
  std::shared_ptr<KitRuntime> runtime_;
  std::string nodeType_;
  std::mutex mu_;
  std::map<std::string, std::shared_ptr<MarketCore>> cores_;
};

/// Markets' order books: at() a market by its name.
class MarketsKit {
 public:
  explicit MarketsKit(MarketCores& cores) : cores_(cores) {}
  MarketHandle at(const std::string& key = "town") { return MarketHandle(cores_.at(key)); }

 private:
  MarketCores& cores_;
};

/// Markets' vendors.
class ShopsKit {
 public:
  explicit ShopsKit(MarketCores& cores) : cores_(cores) {}
  ShopHandle at(const std::string& key = "town") { return ShopHandle(cores_.at(key)); }

 private:
  MarketCores& cores_;
};

/// Trades between two players at a market.
class TradesKit {
 public:
  explicit TradesKit(MarketCores& cores) : cores_(cores) {}
  TradeDesk at(const std::string& key = "town") { return TradeDesk(cores_.at(key)); }

 private:
  MarketCores& cores_;
};

}  // namespace crowdy::kit
