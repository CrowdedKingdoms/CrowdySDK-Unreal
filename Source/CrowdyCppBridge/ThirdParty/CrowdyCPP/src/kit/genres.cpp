#include "crowdy/kit/genres.hpp"

namespace crowdy::kit {

namespace {

using graphql::JVal;

KitCallOptions withAction() {
  KitCallOptions o;
  o.action = true;
  return o;
}

/// One handle per key, made on first use.
template <typename T, typename... Args>
T& handle(std::mutex& mu, std::map<std::string, std::unique_ptr<T>>& slots, const std::string& key, Args&&... args) {
  std::lock_guard lock(mu);
  auto& slot = slots[key];
  if (!slot) slot = std::make_unique<T>(std::forward<Args>(args)..., key);
  return *slot;
}

JVal xz(double x, double z) { return JVal::object({{"x", JVal(x)}, {"z", JVal(z)}}); }

}  // namespace

// ---- leaderboards ----

LeaderboardHandle::LeaderboardHandle(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string name)
    : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)), name_(std::move(name)), view_(runtime_, nodeType_, name_, "board") {}

void LeaderboardHandle::top(KitCallback done, const LeaderboardPage& page) {
  JVal args = JVal::object({});
  if (page.limit) args["limit"] = JVal(*page.limit);
  if (page.offset) args["offset"] = JVal(*page.offset);
  runtime_->call(nodeType_, name_, "top", std::move(args), std::move(done));
}

void LeaderboardHandle::around(KitCallback done, const LeaderboardAround& request) {
  JVal args = JVal::object({});
  if (request.player) args["player"] = JVal(*request.player);
  if (request.radius) args["radius"] = JVal(*request.radius);
  runtime_->call(nodeType_, name_, "around", std::move(args), std::move(done));
}

void LeaderboardHandle::me(KitCallback done) { runtime_->call(nodeType_, name_, "me", JVal(), std::move(done)); }

void LeaderboardHandle::season(KitCallback done) { runtime_->call(nodeType_, name_, "season", JVal(), std::move(done)); }

void LeaderboardHandle::submit(std::int64_t player, std::int64_t score, KitCallback done) {
  runtime_->call(nodeType_, name_, "submit", JVal::object({{"player", JVal(player)}, {"score", JVal(score)}}), std::move(done),
                 withAction());
}

void LeaderboardHandle::configure(const JVal& config, KitCallback done) {
  runtime_->call(nodeType_, name_, "configure", config, std::move(done), withAction());
}

void LeaderboardHandle::roll(KitCallback done) { runtime_->call(nodeType_, name_, "roll", JVal(), std::move(done), withAction()); }

LeaderboardsKit::LeaderboardsKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType)
    : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)) {}

LeaderboardHandle& LeaderboardsKit::board(const std::string& name) { return handle(mu_, boards_, name, runtime_, nodeType_); }

// ---- card tables ----

CardTable::CardTable(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key)
    : runtime_(std::move(runtime)),
      nodeType_(std::move(nodeType)),
      key_(std::move(key)),
      table_(runtime_, nodeType_, key_, "table"),
      hand_(runtime_, nodeType_, key_, "table", {"hand"}, [rt = std::weak_ptr<KitRuntime>(runtime_), nt = nodeType_, k = key_](KitCallback cb) {
        if (auto live = rt.lock()) live->call(nt, k, "hand", JVal(), std::move(cb));
      }) {}

void CardTable::watch(KitCallback done) {
  struct Both {
    std::mutex mu;
    int left = 2;
    std::optional<KitReply> result;
    KitCallback done;
  };
  auto both = std::make_shared<Both>();
  both->done = std::move(done);
  // The table's reply, unless the hand's read failed.
  auto part = [both](bool primary) {
    return [both, primary](KitReply r) {
      KitCallback fire;
      std::optional<KitReply> out;
      {
        std::lock_guard lock(both->mu);
        if ((primary && !both->result) || (!r.ok() && (!both->result || both->result->ok()))) both->result = std::move(r);
        if (--both->left > 0) return;
        fire = std::move(both->done);
        out = std::move(both->result);
      }
      if (fire && out) fire(std::move(*out));
    };
  };
  table_.start(part(true));
  hand_.start(part(false));
}

void CardTable::unwatch() {
  table_.stop();
  hand_.stop();
}

void CardTable::join(KitCallback done) { runtime_->call(nodeType_, key_, "join", JVal(), std::move(done), withAction()); }
void CardTable::leave(KitCallback done) { runtime_->call(nodeType_, key_, "leave", JVal(), std::move(done), withAction()); }
void CardTable::start(KitCallback done) { runtime_->call(nodeType_, key_, "start", JVal(), std::move(done), withAction()); }

void CardTable::play(std::string card, std::optional<std::string> suit, KitCallback done) {
  JVal args = JVal::object({{"card", JVal(std::move(card))}});
  if (suit) args["suit"] = JVal(std::move(*suit));
  runtime_->call(nodeType_, key_, "play", std::move(args), std::move(done), withAction());
}

void CardTable::draw(KitCallback done) { runtime_->call(nodeType_, key_, "draw", JVal(), std::move(done), withAction()); }
void CardTable::pass(KitCallback done) { runtime_->call(nodeType_, key_, "pass", JVal(), std::move(done), withAction()); }

TablesKit::TablesKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType) : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)) {}

CardTable& TablesKit::table(const std::string& key) { return handle(mu_, tables_, key, runtime_, nodeType_); }

// ---- tactics ----

TacticsBattle::TacticsBattle(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key)
    : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)), key_(std::move(key)), battle_(runtime_, nodeType_, key_, "battle") {}

std::vector<graphql::Json> TacticsBattle::myUnits() const {
  std::vector<graphql::Json> mine;
  const std::string me = runtime_->playerId();
  if (me.empty()) return mine;
  battle_.get()["units"].forEach([&](const graphql::Json& unit) {
    if (unit["owner"].isNumber() && std::to_string(unit["owner"].asInt64()) == me) mine.push_back(unit);
  });
  return mine;
}

void TacticsBattle::join(KitCallback done) { runtime_->call(nodeType_, key_, "join", JVal(), std::move(done), withAction()); }
void TacticsBattle::start(KitCallback done) { runtime_->call(nodeType_, key_, "start", JVal(), std::move(done), withAction()); }

void TacticsBattle::move(std::int64_t x, std::int64_t z, KitCallback done) {
  runtime_->call(nodeType_, key_, "move", JVal::object({{"x", JVal(x)}, {"z", JVal(z)}}), std::move(done), withAction());
}

void TacticsBattle::attack(std::int64_t target, KitCallback done) {
  runtime_->call(nodeType_, key_, "attack", JVal::object({{"target", JVal(target)}}), std::move(done), withAction());
}

void TacticsBattle::endTurn(KitCallback done) { runtime_->call(nodeType_, key_, "end_turn", JVal(), std::move(done), withAction()); }

TacticsKit::TacticsKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType) : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)) {}

TacticsBattle& TacticsKit::battle(const std::string& key) { return handle(mu_, battles_, key, runtime_, nodeType_); }

// ---- encounters ----

Encounter::Encounter(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key)
    : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)), key_(std::move(key)), view_(runtime_, nodeType_, key_, "encounter") {}

void Encounter::join(KitCallback done) { runtime_->call(nodeType_, key_, "join", JVal(), std::move(done), withAction()); }
void Encounter::start(KitCallback done) { runtime_->call(nodeType_, key_, "start", JVal(), std::move(done), withAction()); }

void Encounter::build(std::int64_t x, std::int64_t z, KitCallback done) {
  runtime_->call(nodeType_, key_, "build", JVal::object({{"x", JVal(x)}, {"z", JVal(z)}}), std::move(done), withAction());
}

void Encounter::hit(std::int64_t enemy, KitCallback done) {
  runtime_->call(nodeType_, key_, "hit", JVal::object({{"enemy", JVal(enemy)}}), std::move(done), withAction());
}

EncountersKit::EncountersKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType)
    : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)) {}

Encounter& EncountersKit::encounter(const std::string& key) { return handle(mu_, encounters_, key, runtime_, nodeType_); }

// ---- minigames ----

Duel::Duel(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key)
    : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)), key_(std::move(key)), view_(runtime_, nodeType_, key_, "duel") {}

void Duel::join(KitCallback done) { runtime_->call(nodeType_, key_, "join", JVal(), std::move(done), withAction()); }
void Duel::leave(KitCallback done) { runtime_->call(nodeType_, key_, "leave", JVal(), std::move(done), withAction()); }

void Duel::throwHand(std::string hand, KitCallback done) {
  runtime_->call(nodeType_, key_, "throw", JVal::object({{"hand", JVal(std::move(hand))}}), std::move(done), withAction());
}

void Duel::rematch(KitCallback done) { runtime_->call(nodeType_, key_, "rematch", JVal(), std::move(done), withAction()); }

MinigamesKit::MinigamesKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType)
    : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)) {}

Duel& MinigamesKit::duel(const std::string& key) { return handle(mu_, duels_, key, runtime_, nodeType_); }

// ---- instances ----

InstanceRun::InstanceRun(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key)
    : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)), key_(std::move(key)), view_(runtime_, nodeType_, key_, "run") {}

void InstanceRun::open(const InstanceOpen& request, KitCallback done) {
  JVal args = JVal::object({});
  if (request.seed) args["seed"] = JVal(*request.seed);
  if (!request.party.empty()) {
    graphql::JArray party;
    for (const auto p : request.party) party.push_back(JVal(p));
    args["party"] = JVal(std::move(party));
  }
  runtime_->call(nodeType_, key_, "open", std::move(args), std::move(done), withAction());
}

void InstanceRun::invite(std::int64_t player, KitCallback done) {
  runtime_->call(nodeType_, key_, "invite", JVal::object({{"player", JVal(player)}}), std::move(done), withAction());
}

void InstanceRun::join(KitCallback done) { runtime_->call(nodeType_, key_, "join", JVal(), std::move(done), withAction()); }
void InstanceRun::enter(KitCallback done) { runtime_->call(nodeType_, key_, "enter", JVal(), std::move(done), withAction()); }
void InstanceRun::descend(KitCallback done) { runtime_->call(nodeType_, key_, "descend", JVal(), std::move(done), withAction()); }
void InstanceRun::abandon(KitCallback done) { runtime_->call(nodeType_, key_, "abandon", JVal(), std::move(done), withAction()); }

InstancesKit::InstancesKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType)
    : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)) {}

InstanceRun& InstancesKit::run(const std::string& key) { return handle(mu_, runs_, key, runtime_, nodeType_); }

// ---- arenas ----

ArenaMatch::ArenaMatch(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key)
    : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)), key_(std::move(key)), view_(runtime_, nodeType_, key_, "arena") {}

void ArenaMatch::join(std::string heroClass, KitCallback done) {
  runtime_->call(nodeType_, key_, "join", JVal::object({{"class", JVal(std::move(heroClass))}}), std::move(done), withAction());
}

void ArenaMatch::leave(KitCallback done) { runtime_->call(nodeType_, key_, "leave", JVal(), std::move(done), withAction()); }
void ArenaMatch::start(KitCallback done) { runtime_->call(nodeType_, key_, "start", JVal(), std::move(done), withAction()); }
void ArenaMatch::move(double x, double z, KitCallback done) { runtime_->call(nodeType_, key_, "move", xz(x, z), std::move(done)); }

void ArenaMatch::cast(std::string ability, double x, double z, KitCallback done) {
  runtime_->call(nodeType_, key_, "cast", JVal::object({{"ability", JVal(std::move(ability))}, {"x", JVal(x)}, {"z", JVal(z)}}),
                 std::move(done), withAction());
}

ArenasKit::ArenasKit(std::shared_ptr<KitRuntime> runtime, std::string nodeType) : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)) {}

ArenaMatch& ArenasKit::arena(const std::string& key) { return handle(mu_, arenas_, key, runtime_, nodeType_); }

// ---- racing ----

Race::Race(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key)
    : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)), key_(std::move(key)), view_(runtime_, nodeType_, key_, "race") {}

void Race::join(KitCallback done) { runtime_->call(nodeType_, key_, "join", JVal(), std::move(done), withAction()); }
void Race::leave(KitCallback done) { runtime_->call(nodeType_, key_, "leave", JVal(), std::move(done), withAction()); }
void Race::start(KitCallback done) { runtime_->call(nodeType_, key_, "start", JVal(), std::move(done), withAction()); }
void Race::pos(double x, double z, KitCallback done) { runtime_->call(nodeType_, key_, "pos", xz(x, z), std::move(done)); }
void Race::ghost(KitCallback done) { runtime_->call(nodeType_, key_, "ghost", JVal(), std::move(done)); }

BallMatch::BallMatch(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key)
    : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)), key_(std::move(key)), view_(runtime_, nodeType_, key_, "ball") {}

void BallMatch::join(std::string team, KitCallback done) {
  runtime_->call(nodeType_, key_, "join", JVal::object({{"team", JVal(std::move(team))}}), std::move(done), withAction());
}

void BallMatch::start(KitCallback done) { runtime_->call(nodeType_, key_, "start", JVal(), std::move(done), withAction()); }
void BallMatch::pos(double x, double z, KitCallback done) { runtime_->call(nodeType_, key_, "pos", xz(x, z), std::move(done)); }
void BallMatch::claim(KitCallback done) { runtime_->call(nodeType_, key_, "claim", JVal(), std::move(done), withAction()); }

void BallMatch::pass(std::int64_t to, KitCallback done) {
  runtime_->call(nodeType_, key_, "pass", JVal::object({{"to", JVal(to)}}), std::move(done), withAction());
}

void BallMatch::shoot(double x, double z, KitCallback done) { runtime_->call(nodeType_, key_, "shoot", xz(x, z), std::move(done), withAction()); }

RacingKit::RacingKit(std::shared_ptr<KitRuntime> runtime, std::string raceType, std::string ballType)
    : runtime_(std::move(runtime)), raceType_(std::move(raceType)), ballType_(std::move(ballType)) {}

Race& RacingKit::race(const std::string& key) { return handle(mu_, races_, key, runtime_, raceType_); }
BallMatch& RacingKit::ball(const std::string& key) { return handle(mu_, matches_, key, runtime_, ballType_); }

// ---- territory ----

TerritoryMap::TerritoryMap(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key)
    : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)), key_(std::move(key)), view_(runtime_, nodeType_, key_, "territory") {}

void TerritoryMap::join(std::string faction, KitCallback done) {
  runtime_->call(nodeType_, key_, "join", JVal::object({{"faction", JVal(std::move(faction))}}), std::move(done), withAction());
}

void TerritoryMap::reset(KitCallback done) { runtime_->call(nodeType_, key_, "reset", JVal(), std::move(done), withAction()); }

ZoneMatch::ZoneMatch(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key)
    : runtime_(std::move(runtime)), nodeType_(std::move(nodeType)), key_(std::move(key)), view_(runtime_, nodeType_, key_, "zone") {}

void ZoneMatch::join(KitCallback done) { runtime_->call(nodeType_, key_, "join", JVal(), std::move(done), withAction()); }
void ZoneMatch::start(KitCallback done) { runtime_->call(nodeType_, key_, "start", JVal(), std::move(done), withAction()); }

TerritoryKit::TerritoryKit(std::shared_ptr<KitRuntime> runtime, std::string territoryType, std::string zoneType)
    : runtime_(std::move(runtime)), territoryType_(std::move(territoryType)), zoneType_(std::move(zoneType)) {}

TerritoryMap& TerritoryKit::map(const std::string& key) { return handle(mu_, maps_, key, runtime_, territoryType_); }
ZoneMatch& TerritoryKit::zone(const std::string& key) { return handle(mu_, zones_, key, runtime_, zoneType_); }

}  // namespace crowdy::kit
