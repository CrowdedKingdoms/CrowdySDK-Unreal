#include "crowdy/kit/economy.hpp"

#include <algorithm>

namespace crowdy::kit {

namespace {

using graphql::JVal;

constexpr const char* kTopic = "player";

KitCallOptions withAction() {
  KitCallOptions o;
  o.action = true;
  return o;
}

std::vector<graphql::Json> listOf(const graphql::Json& value) {
  std::vector<graphql::Json> out;
  value.forEach([&](const graphql::Json& j) { out.push_back(j); });
  return out;
}

}  // namespace

// ---- PlayerKit ----

struct PlayerKit::Core : std::enable_shared_from_this<PlayerKit::Core> {
  Core(std::shared_ptr<KitRuntime> rt, std::string nt, std::string k) : runtime(std::move(rt)), nodeType(std::move(nt)), fixedKey(std::move(k)) {}

  const std::shared_ptr<KitRuntime> runtime;
  const std::string nodeType;
  const std::string fixedKey;
  WritableKitStore<graphql::Json> profile{graphql::Json()};
  WritableKitStore<graphql::Json> inventory{graphql::Json()};
  WritableKitStore<graphql::Json> wallet{graphql::Json()};
  WritableKitStore<graphql::Json> progress{graphql::Json()};
  std::mutex mu;
  std::string key;
  std::unique_ptr<HubState> profileState;
  std::unique_ptr<PrivateState> inventoryState, walletState, progressState;
  std::map<std::uint64_t, std::function<void(const graphql::Json&)>> listeners;
  std::uint64_t nextId = 0;

  std::string currentKey() const { return fixedKey.empty() ? runtime->playerId() : fixedKey; }

  /// The mirrors, made once the key is known; false while it is not.
  bool ready() {
    std::lock_guard lock(mu);
    if (profileState) return true;
    key = currentKey();
    if (key.empty()) return false;
    std::weak_ptr<Core> weak = weak_from_this();
    auto reader = [weak](const char* method) {
      return [weak, method](KitCallback cb) {
        if (auto self = weak.lock()) self->runtime->call(self->nodeType, self->key, method, JVal(), std::move(cb));
      };
    };
    profileState = std::make_unique<HubState>(runtime, nodeType, key, kTopic);
    inventoryState = std::make_unique<PrivateState>(runtime, nodeType, key, kTopic, std::vector<std::string>{"inventory"}, reader("inventory"));
    walletState = std::make_unique<PrivateState>(runtime, nodeType, key, kTopic, std::vector<std::string>{"wallet"}, reader("wallet"));
    progressState = std::make_unique<PrivateState>(runtime, nodeType, key, kTopic, std::vector<std::string>{"progress"}, reader("progress"));
    profileState->subscribe([weak] {
      if (auto self = weak.lock()) self->profile.set(self->profileState->get());
    });
    inventoryState->subscribe([weak] {
      if (auto self = weak.lock()) self->inventory.set(self->inventoryState->get());
    });
    walletState->subscribe([weak] {
      if (auto self = weak.lock()) self->wallet.set(self->walletState->get());
    });
    progressState->subscribe([weak] {
      if (auto self = weak.lock()) self->progress.set(self->progressState->get());
    });
    profileState->onEvent([weak](const graphql::Json& body, const KitHeader&) {
      auto self = weak.lock();
      if (!self) return;
      std::vector<std::function<void(const graphql::Json&)>> copies;
      {
        std::lock_guard l(self->mu);
        for (const auto& [id, fn] : self->listeners) copies.push_back(fn);
      }
      for (auto& fn : copies) fn(body);
    });
    return true;
  }
};

PlayerKit::PlayerKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key)
    : core_(std::make_shared<Core>(std::move(runtime), std::move(nodeType), std::move(key))) {}

PlayerKit::~PlayerKit() = default;

std::string PlayerKit::key() const { return core_->currentKey(); }

WritableKitStore<graphql::Json>& PlayerKit::profile() { return core_->profile; }
WritableKitStore<graphql::Json>& PlayerKit::inventory() { return core_->inventory; }
WritableKitStore<graphql::Json>& PlayerKit::wallet() { return core_->wallet; }
WritableKitStore<graphql::Json>& PlayerKit::progress() { return core_->progress; }

void PlayerKit::watch(KitCallback done, std::vector<std::string> which) {
  if (!core_->ready()) {
    if (done) done(KitReply::refused(codes::kBadRequest, "the kit does not know which player it is: set its playerId"));
    return;
  }
  if (which.empty()) which = {"profile", "inventory", "wallet", "progress"};
  struct Join {
    std::mutex mu;
    std::size_t left = 0;
    std::optional<KitReply> failure;
    KitCallback done;
  };
  auto join = std::make_shared<Join>();
  join->left = which.size();
  join->done = std::move(done);
  auto part = [join](KitReply r) {
    KitCallback fire;
    KitReply out = r;
    {
      std::lock_guard lock(join->mu);
      if (!r.ok() && !join->failure) join->failure = r;
      if (--join->left > 0) return;
      fire = std::move(join->done);
      if (join->failure) out = *join->failure;
    }
    if (fire) fire(std::move(out));
  };
  for (const auto& w : which) {
    if (w == "profile") {
      core_->profileState->start(part);
    } else if (w == "inventory") {
      core_->inventoryState->start(part);
    } else if (w == "wallet") {
      core_->walletState->start(part);
    } else if (w == "progress") {
      core_->progressState->start(part);
    } else {
      part(KitReply::refused(codes::kBadRequest, "watch names profile, inventory, wallet or progress"));
    }
  }
}

void PlayerKit::unwatch() {
  std::lock_guard lock(core_->mu);
  if (!core_->profileState) return;
  core_->profileState->stop();
  core_->inventoryState->stop();
  core_->walletState->stop();
  core_->progressState->stop();
}

std::uint64_t PlayerKit::onEvent(std::function<void(const graphql::Json&)> listener) {
  std::lock_guard lock(core_->mu);
  core_->listeners.emplace(++core_->nextId, std::move(listener));
  return core_->nextId;
}

void PlayerKit::removeListener(std::uint64_t id) {
  std::lock_guard lock(core_->mu);
  core_->listeners.erase(id);
}

void PlayerKit::call(std::string method, JVal args, KitCallback done, bool change) {
  const std::string key = core_->currentKey();
  if (key.empty()) {
    if (done) done(KitReply::refused(codes::kBadRequest, "the kit does not know which player it is: set its playerId"));
    return;
  }
  core_->runtime->call(core_->nodeType, key, std::move(method), std::move(args), std::move(done), change ? withAction() : KitCallOptions{});
}

void PlayerKit::me(KitCallback done) { call("me", JVal(), std::move(done), false); }
void PlayerKit::rules(KitCallback done) { call("rules", JVal(), std::move(done), false); }

void PlayerKit::profileOf(std::int64_t player, KitCallback done) {
  core_->runtime->call(core_->nodeType, std::to_string(player), "profile", JVal(), std::move(done));
}

// ---- the views ----

std::int64_t InventoryKit::count(const std::string& item) const {
  std::int64_t n = 0;
  player_.inventory().get()["stacks"].forEach([&](const graphql::Json& s) {
    if (s["item"].asStringView() == item) n += s["count"].asInt64();
  });
  return n;
}

void InventoryKit::discard(std::string item, std::int64_t count, KitCallback done) {
  player_.call("discard", JVal::object({{"item", JVal(std::move(item))}, {"count", JVal(count)}}), std::move(done));
}

void InventoryKit::takeMail(KitCallback done) { player_.call("take_mail", JVal(), std::move(done)); }

std::int64_t WalletKit::balance(const std::string& currency) const {
  const graphql::Json b = player_.wallet().get()["balances"][currency];
  return b.isNumber() ? b.asInt64() : 0;
}

std::uint64_t ProgressionKit::onLevel(std::function<void(std::int64_t)> listener) {
  return player_.onEvent([listener = std::move(listener)](const graphql::Json& e) {
    if (e["type"].asStringView() == "level") listener(e["level"].asInt64());
  });
}

std::vector<graphql::Json> AchievementsKit::list() const { return listOf(player_.progress().get()["achievements"]); }

std::uint64_t AchievementsKit::onUnlock(std::function<void(const std::string&)> listener) {
  return player_.onEvent([listener = std::move(listener)](const graphql::Json& e) {
    if (e["type"].asStringView() == "achievement") listener(e["achievement"].asString());
  });
}

std::vector<graphql::Json> QuestsKit::list() const { return listOf(player_.progress().get()["quests"]); }

void QuestsKit::accept(std::string quest, KitCallback done) {
  player_.call("accept", JVal::object({{"quest", JVal(std::move(quest))}}), std::move(done));
}

void QuestsKit::claim(std::string quest, KitCallback done) {
  player_.call("claim", JVal::object({{"quest", JVal(std::move(quest))}}), std::move(done));
}

void QuestsKit::step(std::string counter, KitCallback done) {
  player_.call("step", JVal::object({{"counter", JVal(std::move(counter))}}), std::move(done));
}

namespace {

/// `field` of the rules, as the reply's value.
KitCallback ruleField(const char* field, KitCallback done) {
  return [field, done = std::move(done)](KitReply r) {
    if (r.ok()) {
      const std::string part = r.value()[field].toMsgpack();
      r.payload = part;
    }
    if (done) done(std::move(r));
  };
}

}  // namespace

void CraftingKit::recipes(KitCallback done) { player_.rules(ruleField("recipes", std::move(done))); }

void CraftingKit::craft(std::string recipe, std::optional<std::int64_t> times, KitCallback done) {
  JVal args = JVal::object({{"recipe", JVal(std::move(recipe))}});
  if (times) args["times"] = JVal(*times);
  player_.call("craft", std::move(args), std::move(done));
}

void LootKit::tables(KitCallback done) { player_.rules(ruleField("loot", std::move(done))); }

void LootKit::open(std::string table, KitCallback done) {
  player_.call("open", JVal::object({{"table", JVal(std::move(table))}}), std::move(done));
}

void ProductionKit::view(KitCallback done) {
  player_.me([done = std::move(done)](KitReply r) {
    if (r.ok()) r.payload = r.value()["production"].toMsgpack();
    if (done) done(std::move(r));
  });
}

void ProductionKit::collect(KitCallback done) { player_.call("collect", JVal(), std::move(done)); }

// ---- markets ----

MarketCore::MarketCore(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key)
    : runtime_(std::move(runtime)),
      nodeType_(std::move(nodeType)),
      key_(std::move(key)),
      view_(runtime_, nodeType_, key_, "market"),
      orders_(runtime_, nodeType_, key_, "market", {"orders"},
              [rt = std::weak_ptr<KitRuntime>(runtime_), nt = nodeType_, k = key_](KitCallback cb) {
                if (auto live = rt.lock()) live->call(nt, k, "orders", JVal(), std::move(cb));
              }),
      trades_(runtime_, nodeType_, key_, "market", {"trades"},
              [rt = std::weak_ptr<KitRuntime>(runtime_), nt = nodeType_, k = key_](KitCallback cb) {
                if (auto live = rt.lock()) live->call(nt, k, "trades", JVal(), std::move(cb));
              }) {}

void MarketCore::change(std::string method, JVal args, KitCallback done) {
  runtime_->call(nodeType_, key_, std::move(method), std::move(args), std::move(done), withAction());
}

void MarketCore::read(std::string method, JVal args, KitCallback done) {
  runtime_->call(nodeType_, key_, std::move(method), std::move(args), std::move(done));
}

std::shared_ptr<MarketCore> MarketCores::at(const std::string& key) {
  std::lock_guard lock(mu_);
  auto& core = cores_[key];
  if (!core) core = std::make_shared<MarketCore>(runtime_, nodeType_, key);
  return core;
}

void MarketHandle::watch(KitCallback done) {
  auto core = core_;
  core->view().start([core, done = std::move(done)](KitReply view) mutable {
    if (!view.ok()) {
      if (done) done(std::move(view));
      return;
    }
    core->orders().start([view = std::move(view), done = std::move(done)](KitReply orders) mutable {
      if (done) done(orders.ok() ? std::move(view) : std::move(orders));
    });
  });
}

void MarketHandle::unwatch() {
  core_->view().stop();
  core_->orders().stop();
}

void MarketHandle::book(std::string item, KitCallback done) { core_->read("book", JVal::object({{"item", JVal(std::move(item))}}), std::move(done)); }

void MarketHandle::order(const MarketOrderRequest& order, KitCallback done) {
  core_->change("order",
                JVal::object({{"item", JVal(order.item)}, {"side", JVal(order.side)}, {"price", JVal(order.price)}, {"count", JVal(order.count)}}),
                std::move(done));
}

void MarketHandle::cancel(std::int64_t order, KitCallback done) { core_->change("cancel", JVal::object({{"order", JVal(order)}}), std::move(done)); }

void ShopHandle::vendors(KitCallback done) { core_->read("vendors", JVal(), std::move(done)); }

namespace {

JVal shopArgs(std::string vendor, std::string item, std::optional<std::int64_t> count) {
  JVal args = JVal::object({{"vendor", JVal(std::move(vendor))}, {"item", JVal(std::move(item))}});
  if (count) args["count"] = JVal(*count);
  return args;
}

}  // namespace

void ShopHandle::buy(std::string vendor, std::string item, std::optional<std::int64_t> count, KitCallback done) {
  core_->change("buy", shopArgs(std::move(vendor), std::move(item), count), std::move(done));
}

void ShopHandle::sell(std::string vendor, std::string item, std::optional<std::int64_t> count, KitCallback done) {
  core_->change("sell", shopArgs(std::move(vendor), std::move(item), count), std::move(done));
}

void TradeDesk::open(std::int64_t player, KitCallback done) { core_->change("trade", JVal::object({{"with", JVal(player)}}), std::move(done)); }

void TradeDesk::offer(std::int64_t trade, JVal bundle, KitCallback done) {
  core_->change("offer", JVal::object({{"trade", JVal(trade)}, {"bundle", std::move(bundle)}}), std::move(done));
}

void TradeDesk::accept(std::int64_t trade, KitCallback done) { core_->change("accept", JVal::object({{"trade", JVal(trade)}}), std::move(done)); }

void TradeDesk::decline(std::int64_t trade, KitCallback done) { core_->change("decline", JVal::object({{"trade", JVal(trade)}}), std::move(done)); }

}  // namespace crowdy::kit
