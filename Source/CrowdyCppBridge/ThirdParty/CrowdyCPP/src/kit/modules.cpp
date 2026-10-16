#include "crowdy/kit/modules.hpp"

#include <algorithm>

#include "crowdy/replication/connection.hpp"

namespace crowdy::kit {

namespace {

using graphql::JVal;

const graphql::Json& idlePlace() {
  static const graphql::Json idle = graphql::Json::parse(R"({"state":"idle"})");
  return idle;
}

KitCallOptions withAction() {
  KitCallOptions o;
  o.action = true;
  return o;
}

}  // namespace

// ---- WorldKit ----

WorldKit::WorldKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key)
    : runtime_(std::move(runtime)),
      nodeType_(std::move(nodeType)),
      key_(std::move(key)),
      conditions_(runtime_, nodeType_, key_, "world"),
      nodes_(runtime_, nodeType_, key_, "nodes") {}

void WorldKit::state(KitCallback done) { runtime_->call(nodeType_, key_, "state", JVal(), std::move(done)); }

void WorldKit::harvest(std::int64_t node, KitCallback done) {
  runtime_->call(nodeType_, key_, "harvest", JVal::object({{"node", JVal(node)}}), std::move(done), withAction());
}

void WorldKit::schedule(const JVal& event, KitCallback done) {
  runtime_->call(nodeType_, key_, "schedule", event, std::move(done), withAction());
}

void WorldKit::cancel(std::string name, KitCallback done) {
  runtime_->call(nodeType_, key_, "cancel", JVal::object({{"name", JVal(std::move(name))}}), std::move(done), withAction());
}

// ---- MatchQueue ----

struct MatchQueue::Core : std::enable_shared_from_this<MatchQueue::Core> {
  Core(std::shared_ptr<KitRuntime> rt, std::string nt, std::string n) : runtime(std::move(rt)), nodeType(std::move(nt)), name(std::move(n)) {}

  const std::shared_ptr<KitRuntime> runtime;
  const std::string nodeType, name;
  WritableKitStore<graphql::Json> place{idlePlace()};
  mutable std::mutex mu;
  std::string lastMatched;
  std::map<std::uint64_t, std::function<void(const std::string&)>> matched;
  std::uint64_t nextId = 0;
  std::uint64_t timer = 0;
  long interval = 0;

  void settle(const graphql::Json& p) {
    place.set(p);
    if (p["state"].asStringView() != "matched") return;
    const std::string session = p["session"].asString();
    std::vector<std::function<void(const std::string&)>> listeners;
    {
      std::lock_guard lock(mu);
      if (session == lastMatched) return;
      lastMatched = session;
      for (const auto& [id, l] : matched) listeners.push_back(l);
    }
    for (auto& l : listeners) l(session);
  }

  /// A call whose answer is the player's place.
  void placeCall(const char* method, JVal args, KitCallback done, bool action) {
    std::weak_ptr<Core> weak = weak_from_this();
    KitCallOptions o;
    o.action = action;
    runtime->call(nodeType, name, method, std::move(args),
                  [weak, done = std::move(done)](KitReply r) {
                    if (auto self = weak.lock(); self && r.ok()) self->settle(r.value());
                    if (done) done(std::move(r));
                  },
                  o);
  }

  /// A call that leaves the player idle.
  void idleCall(const char* method, KitCallback done) {
    std::weak_ptr<Core> weak = weak_from_this();
    runtime->call(nodeType, name, method, JVal(),
                  [weak, done = std::move(done)](KitReply r) {
                    if (auto self = weak.lock(); self && r.ok()) self->settle(idlePlace());
                    if (done) done(std::move(r));
                  },
                  withAction());
  }

  void tick() {
    const std::string state = place.get()["state"].asString();
    if (state == "party" || state == "queued" || state == "proposed") placeCall("status", JVal(), {}, false);
    std::weak_ptr<Core> weak = weak_from_this();
    std::lock_guard lock(mu);
    if (interval <= 0) return;
    timer = runtime->after(interval, [weak] {
      if (auto self = weak.lock()) self->tick();
    });
  }

  void unwatch() {
    std::uint64_t t = 0;
    {
      std::lock_guard lock(mu);
      interval = 0;
      t = std::exchange(timer, 0);
    }
    if (t) runtime->cancel(t);
  }
};

MatchQueue::MatchQueue(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string name)
    : core_(std::make_shared<Core>(runtime, nodeType, name)), name_(name), counts_(runtime, nodeType, name, "queue") {}

MatchQueue::~MatchQueue() { core_->unwatch(); }

graphql::Json MatchQueue::place() const { return core_->place.get(); }

namespace {
/// Place listeners' ids carry this bit, so removeListener tells them from match listeners.
constexpr std::uint64_t kPlaceListener = 1ULL << 63;
}  // namespace

std::uint64_t MatchQueue::onPlace(std::function<void(const graphql::Json&)> listener) {
  return core_->place.subscribe(std::move(listener)) | kPlaceListener;
}

void MatchQueue::join(const MatchJoin& request, KitCallback done) {
  JVal args = JVal::object({{"rating", JVal(request.rating)}});
  if (!request.party.empty()) {
    graphql::JArray party;
    for (auto p : request.party) party.emplace_back(p);
    args["party"] = JVal(std::move(party));
  }
  if (request.leader) args["leader"] = JVal(*request.leader);
  core_->placeCall("join", std::move(args), std::move(done), true);
}

void MatchQueue::leave(KitCallback done) { core_->idleCall("leave", std::move(done)); }
void MatchQueue::accept(KitCallback done) { core_->placeCall("accept", JVal(), std::move(done), true); }
void MatchQueue::decline(KitCallback done) { core_->idleCall("decline", std::move(done)); }
void MatchQueue::status(KitCallback done) { core_->placeCall("status", JVal(), std::move(done), false); }

void MatchQueue::configure(const JVal& config, KitCallback done) {
  core_->runtime->call(core_->nodeType, core_->name, "configure", config, std::move(done), withAction());
}

void MatchQueue::watch(long intervalMs) {
  core_->unwatch();
  {
    std::lock_guard lock(core_->mu);
    core_->interval = std::max(500L, intervalMs);
  }
  std::weak_ptr<Core> weak = core_;
  std::lock_guard lock(core_->mu);
  core_->timer = core_->runtime->after(core_->interval, [weak] {
    if (auto self = weak.lock()) self->tick();
  });
}

void MatchQueue::unwatch() { core_->unwatch(); }

std::uint64_t MatchQueue::onMatched(std::function<void(const std::string&)> listener) {
  std::lock_guard lock(core_->mu);
  core_->matched.emplace(++core_->nextId, std::move(listener));
  return core_->nextId;
}

void MatchQueue::removeListener(std::uint64_t id) {
  if (id & kPlaceListener) {
    core_->place.removeListener(id & ~kPlaceListener);
    return;
  }
  std::lock_guard lock(core_->mu);
  core_->matched.erase(id);
}

MatchmakingKit::MatchmakingKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType)
    : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)) {}

MatchQueue& MatchmakingKit::queue(const std::string& name) {
  std::lock_guard lock(mu_);
  auto& q = queues_[name];
  if (!q) q = std::make_unique<MatchQueue>(runtime_, nodeType_, name);
  return *q;
}

// ---- sessions ----

std::optional<std::int64_t> sessionTurn(const graphql::Json& state) {
  if (!state.isObject() || state["phase"].asStringView() != "playing") return std::nullopt;
  const graphql::Json who = state["players"].at(static_cast<std::size_t>(state["turn"].asInt64()));
  if (!who.isNumber()) return std::nullopt;
  return who.asInt64();
}

GameSessionHandle::GameSessionHandle(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key)
    : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)), key_(std::move(key)), state_(runtime_, nodeType_, key_, "session") {}

bool GameSessionHandle::isMyTurn() const {
  const std::string me = runtime_->playerId();
  const auto turn = currentTurn();
  return !me.empty() && turn && std::to_string(*turn) == me;
}

void GameSessionHandle::join(KitCallback done) { runtime_->call(nodeType_, key_, "join", JVal(), std::move(done), withAction()); }
void GameSessionHandle::leave(KitCallback done) { runtime_->call(nodeType_, key_, "leave", JVal(), std::move(done), withAction()); }

void GameSessionHandle::ready(bool ready, KitCallback done) {
  runtime_->call(nodeType_, key_, "ready", JVal::object({{"ready", JVal(ready)}}), std::move(done), withAction());
}

void GameSessionHandle::startGame(KitCallback done) {
  runtime_->call(nodeType_, key_, "start", JVal(), std::move(done), withAction());
}

void GameSessionHandle::act(const JVal& action, KitCallback done) {
  runtime_->call(nodeType_, key_, "act", action, std::move(done), withAction());
}

void GameSessionHandle::configure(const JVal& config, KitCallback done) {
  runtime_->call(nodeType_, key_, "configure", config, std::move(done), withAction());
}

SessionsKit::SessionsKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType)
    : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)) {}

GameSessionHandle& SessionsKit::open(const std::string& key) {
  std::lock_guard lock(mu_);
  auto& s = sessions_[key];
  if (!s) s = std::make_unique<GameSessionHandle>(runtime_, nodeType_, key);
  return *s;
}

// ---- areas ----

NpcArea::NpcArea(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key)
    : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)), key_(std::move(key)), members_(runtime_, nodeType_, key_, "area") {}

void NpcArea::attack(std::int64_t mob, KitCallback done) {
  runtime_->call(nodeType_, key_, "attack", JVal::object({{"mob", JVal(mob)}}), std::move(done), withAction());
}

void NpcArea::talk(std::int64_t npc, KitCallback done) {
  runtime_->call(nodeType_, key_, "talk", JVal::object({{"npc", JVal(npc)}}), std::move(done));
}

void NpcArea::adopt(std::string species, std::string name, KitCallback done) {
  runtime_->call(nodeType_, key_, "adopt", JVal::object({{"species", JVal(std::move(species))}, {"name", JVal(std::move(name))}}),
                 std::move(done), withAction());
}

void NpcArea::rename(std::int64_t pet, std::string name, KitCallback done) { this->pet("rename", pet, std::move(name), std::move(done)); }
void NpcArea::summon(std::int64_t pet, KitCallback done) { this->pet("summon", pet, std::nullopt, std::move(done)); }
void NpcArea::dismiss(std::int64_t pet, KitCallback done) { this->pet("dismiss", pet, std::nullopt, std::move(done)); }
void NpcArea::release(std::int64_t pet, KitCallback done) { this->pet("release", pet, std::nullopt, std::move(done)); }

void NpcArea::myPets(KitCallback done) { runtime_->call(nodeType_, key_, "pets", JVal(), std::move(done)); }

void NpcArea::pet(const char* method, std::int64_t pet, std::optional<std::string> name, KitCallback done) {
  JVal args = JVal::object({{"pet", JVal(pet)}});
  if (name) args["name"] = JVal(std::move(*name));
  runtime_->call(nodeType_, key_, method, std::move(args), std::move(done), withAction());
}

NpcsKit::NpcsKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::int64_t areaSize)
    : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)), size_(std::max<std::int64_t>(1, areaSize)) {}

std::string NpcsKit::keyAt(std::int64_t chunkX, std::int64_t chunkZ) const { return shardKey(chunkX, chunkZ, size_); }

NpcArea& NpcsKit::area(const std::string& key) {
  std::lock_guard lock(mu_);
  auto& a = areas_[key];
  if (!a) a = std::make_unique<NpcArea>(runtime_, nodeType_, key);
  return *a;
}

// ---- spectating ----

struct SpectatorHeartbeat::Core : std::enable_shared_from_this<SpectatorHeartbeat::Core> {
  Core(std::shared_ptr<KitRuntime> rt, replication::Connection& c, SpectateOptions o)
      : runtime(std::move(rt)), connection(c), options(std::move(o)) {}

  const std::shared_ptr<KitRuntime> runtime;
  replication::Connection& connection;
  mutable std::mutex mu;
  SpectateOptions options;
  Errc last = Errc::Ok;
  std::uint64_t timer = 0;
  bool stopped = false;

  void beat() {
    replication::SpatialSend send;
    {
      std::lock_guard lock(mu);
      if (stopped) return;
      send.chunk = options.chunk;
      send.uuid = options.uuid;
      send.payload = Bytes(options.state.data(), options.state.size());
      send.distance = options.distance;
      const auto sent = connection.sendActorUpdate(send);
      last = sent.ok() ? Errc::Ok : sent.error();
    }
    schedule();
  }

  void schedule() {
    std::weak_ptr<Core> weak = weak_from_this();
    std::lock_guard lock(mu);
    if (stopped) return;
    timer = runtime->after(std::max(250L, options.intervalMs), [weak] {
      if (auto self = weak.lock()) self->beat();
    });
  }

  void stop() {
    std::uint64_t t = 0;
    {
      std::lock_guard lock(mu);
      stopped = true;
      t = std::exchange(timer, 0);
    }
    if (t) runtime->cancel(t);
  }
};

SpectatorHeartbeat::SpectatorHeartbeat(std::shared_ptr<KitRuntime> runtime, replication::Connection& connection,
                                       SpectateOptions options)
    : core_(std::make_shared<Core>(std::move(runtime), connection, std::move(options))) {
  core_->beat();
}

SpectatorHeartbeat::~SpectatorHeartbeat() { core_->stop(); }

void SpectatorHeartbeat::moveTo(wire::ChunkCoord chunk) {
  std::uint64_t t = 0;
  {
    std::lock_guard lock(core_->mu);
    core_->options.chunk = chunk;
    t = std::exchange(core_->timer, 0);
  }
  if (t) core_->runtime->cancel(t);
  core_->beat();
}

Errc SpectatorHeartbeat::error() const {
  std::lock_guard lock(core_->mu);
  return core_->last;
}

void SpectatorHeartbeat::stop() { core_->stop(); }

std::unique_ptr<SpectatorHeartbeat> SpectateKit::observe(SpectateOptions options) {
  if (!connection_) return nullptr;
  return std::make_unique<SpectatorHeartbeat>(runtime_, *connection_, std::move(options));
}

// ---- emotes ----

Errc EmotesKit::send(const EmoteSend& emote) {
  if (!connection_) return Errc::NotConnected;
  const std::string body = encodeEmoteBody(emote.emote, emote.targetUuid ? std::optional<std::string_view>(*emote.targetUuid) : std::nullopt);
  const auto sent = connection_->sendClientEvent(emote.chunk, emote.uuid, kEventEmote,
                                                 Bytes(reinterpret_cast<const std::uint8_t*>(body.data()), body.size()), emote.distance);
  return sent.ok() ? Errc::Ok : sent.error();
}

std::optional<EmoteEvent> EmotesKit::parse(std::uint16_t eventType, const std::uint8_t* state, std::size_t len) const {
  if (eventType != kEventEmote) return std::nullopt;
  std::vector<std::uint8_t> bytes(2 + len);
  bytes[0] = static_cast<std::uint8_t>(eventType & 0xff);
  bytes[1] = static_cast<std::uint8_t>(eventType >> 8);
  if (len) std::copy(state, state + len, bytes.begin() + 2);
  return parseEmoteEvent(bytes.data(), bytes.size());
}

}  // namespace crowdy::kit
