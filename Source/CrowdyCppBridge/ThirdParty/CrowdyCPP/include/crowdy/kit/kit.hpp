#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "crowdy/client.hpp"
#include "crowdy/domains/exec.hpp"
#include "crowdy/domains/game_apps.hpp"
#include "crowdy/domains/groups.hpp"
#include "crowdy/kit/actions.hpp"
#include "crowdy/kit/economy.hpp"
#include "crowdy/kit/genres.hpp"
#include "crowdy/kit/mirror.hpp"
#include "crowdy/kit/modules.hpp"
#include "crowdy/kit/protocol.hpp"
#include "crowdy/kit/realtime.hpp"
#include "crowdy/kit/runtime.hpp"
#include "crowdy/kit/social.hpp"
#include "crowdy/kit/topology.hpp"
#include "crowdy/kit/wire.hpp"

/// The app-scoped Game Kit — client.kit(appId), here makeKit(client, appId): typed clients for
/// the ck-exec starter hubs (the root world() with liveops, matchmaking() queues with parties,
/// game sessions(), areas of npcs() with mobs and pets, leaderboards(), card tables(), tactics()
/// battles, encounters(), minigames(), private instances(), arenas(), racing(), territory(); the
/// economy: player() and its views inventory(), wallet(), progression(), achievements(),
/// quests(), crafting(), loot(), production(), and market(), shops(), trades()),
/// the topology() that deploys them, the
/// client-side conventions (spectate(), emotes(), shards()), and social() (parties, guilds and
/// chat over teams and channels). Mirrors CrowdyJS `GameKitClient`.
///
/// The hub modules share one connection (runtime()), opened when first needed, which paces
/// calls under the gateway's limit, resends safely (a change carries an action id the hub
/// answers once), and mirrors topics with resync on gaps and reconnects. Callbacks run from
/// CrowdyClient::poll(). The wire codecs (kit/wire.hpp, kit/realtime.hpp) and
/// runOptimisticAction (kit/actions.hpp) stand alone.
namespace crowdy::kit {

/// The node types the kit's modules call, when a game deployed the starters under other names.
struct GameKitNodeTypes {
  /// The root world hub.
  std::string world = "world";
  /// Matchmaking queues.
  std::string matchmaker = "matchmaker";
  /// Game sessions.
  std::string session = "session";
  /// Areas of mobs, NPCs and pets.
  std::string npcs = "npcs";
  /// Leaderboards.
  std::string leaderboard = "leaderboard";
  /// Card tables.
  std::string deck = "deck";
  /// Grid battles.
  std::string tactics = "tactics";
  /// Waves and bosses.
  std::string director = "director";
  /// Duels.
  std::string minigame = "minigame";
  /// Private runs.
  std::string instance = "instance";
  /// Arenas.
  std::string arena = "arena";
  /// Races and ball games.
  std::string race = "race";
  std::string ball = "ball";
  /// Control points and shrinking circles.
  std::string territory = "territory";
  std::string zone = "zone";
  /// Each player's own hub: inventory, wallet, progression, crafting, loot.
  std::string player = "player";
  /// Markets: vendors, order books, trades.
  std::string market = "market";
};

struct GameKitOptions {
  SocialKitOptions social;
  /// How the hub modules reach the app's hubs: a connection, a developer one, the call budget.
  KitRuntimeOptions runtime;
  GameKitNodeTypes nodes;
  /// The key of the root world hub.
  std::string worldKey;
  /// An area's size in chunks, for npcs().areaAt().
  std::int64_t areaSize = 8;
};

class GameKitClient {
 public:
  /// `teams`/`channels`/`connection` are the social helpers' and the realtime conventions'
  /// composition points; `exec` and `dispatcher` the hub modules'. Pass what your game uses.
  GameKitClient(std::string appId, domains::GameAppsAPI& gameApps, domains::TeamsAPI* teams = nullptr,
                domains::ChannelsAPI* channels = nullptr, replication::Connection* connection = nullptr,
                GameKitOptions options = {}, domains::ExecAPI* exec = nullptr,
                std::shared_ptr<graphql::Dispatcher> dispatcher = nullptr)
      : appId_(appId),
        exec_(exec),
        connection_(connection),
        options_(options),
        social_(appId, teams, channels, gameApps, connection, options.social),
        runtime_(std::make_shared<KitRuntime>(appId, exec, std::move(dispatcher), options.runtime)) {}

  ~GameKitClient() { close(); }
  GameKitClient(const GameKitClient&) = delete;
  GameKitClient& operator=(const GameKitClient&) = delete;
  GameKitClient(GameKitClient&&) = delete;
  GameKitClient& operator=(GameKitClient&&) = delete;

  /// Social helpers (parties, guilds, chat over teams + channels).
  SocialKit& social() { return social_; }
  /// The shared connection to the app's hubs.
  KitRuntime& runtime() { return *runtime_; }

  /// The root world hub: conditions, resource nodes, liveops.
  WorldKit& world() { return lazy(world_, [&] { return std::make_unique<WorldKit>(runtime_, options_.nodes.world, options_.worldKey); }); }
  /// Matchmaking queues, with parties matched whole.
  MatchmakingKit& matchmaking() {
    return lazy(matchmaking_, [&] { return std::make_unique<MatchmakingKit>(runtime_, options_.nodes.matchmaker); });
  }
  /// Game sessions: lobby, ready checks, turns, scores.
  SessionsKit& sessions() { return lazy(sessions_, [&] { return std::make_unique<SessionsKit>(runtime_, options_.nodes.session); }); }
  /// Areas of mobs, NPCs and players' pets.
  NpcsKit& npcs() { return lazy(npcs_, [&] { return std::make_unique<NpcsKit>(runtime_, options_.nodes.npcs, options_.areaSize); }); }
  /// Leaderboards the hubs rank (a client never submits its own score).
  LeaderboardsKit& leaderboards() {
    return lazy(leaderboards_, [&] { return std::make_unique<LeaderboardsKit>(runtime_, options_.nodes.leaderboard); });
  }
  /// Card tables with hidden hands the hub holds.
  TablesKit& tables() { return lazy(tables_, [&] { return std::make_unique<TablesKit>(runtime_, options_.nodes.deck); }); }
  /// Grid battles in turns, against the hub's units.
  TacticsKit& tactics() { return lazy(tactics_, [&] { return std::make_unique<TacticsKit>(runtime_, options_.nodes.tactics); }); }
  /// Encounters: waves down a lane, towers, a boss with phases.
  EncountersKit& encounters() {
    return lazy(encounters_, [&] { return std::make_unique<EncountersKit>(runtime_, options_.nodes.director); });
  }
  /// Duels of hidden throws, revealed together.
  MinigamesKit& minigames() { return lazy(minigames_, [&] { return std::make_unique<MinigamesKit>(runtime_, options_.nodes.minigame); }); }
  /// Private runs for one party: seeded floors, permadeath, expiry.
  InstancesKit& instances() { return lazy(instances_, [&] { return std::make_unique<InstancesKit>(runtime_, options_.nodes.instance); }); }
  /// Real-time arenas: classes, abilities, the hub's referee.
  ArenasKit& arenas() { return lazy(arenas_, [&] { return std::make_unique<ArenasKit>(runtime_, options_.nodes.arena); }); }
  /// Races and ball games, on positions the hub's warden keeps.
  RacingKit& racing() {
    return lazy(racing_, [&] { return std::make_unique<RacingKit>(runtime_, options_.nodes.race, options_.nodes.ball); });
  }
  /// Control points factions capture, and shrinking circles.
  TerritoryKit& territory() {
    return lazy(territory_, [&] { return std::make_unique<TerritoryKit>(runtime_, options_.nodes.territory, options_.nodes.zone); });
  }
  /// The caller's own `player` hub (its key is the runtime's playerId): what they own and what
  /// they did. The economy lives in hub state and is not a ledger; the views below share it.
  PlayerKit& player() { return lazy(player_, [&] { return std::make_unique<PlayerKit>(runtime_, options_.nodes.player); }); }
  InventoryKit& inventory() { return lazy(inventory_, [&] { return std::make_unique<InventoryKit>(player()); }); }
  WalletKit& wallet() { return lazy(wallet_, [&] { return std::make_unique<WalletKit>(player()); }); }
  ProgressionKit& progression() { return lazy(progression_, [&] { return std::make_unique<ProgressionKit>(player()); }); }
  AchievementsKit& achievements() { return lazy(achievements_, [&] { return std::make_unique<AchievementsKit>(player()); }); }
  QuestsKit& quests() { return lazy(quests_, [&] { return std::make_unique<QuestsKit>(player()); }); }
  CraftingKit& crafting() { return lazy(crafting_, [&] { return std::make_unique<CraftingKit>(player()); }); }
  LootKit& loot() { return lazy(loot_, [&] { return std::make_unique<LootKit>(player()); }); }
  ProductionKit& production() { return lazy(production_, [&] { return std::make_unique<ProductionKit>(player()); }); }
  /// Markets' order books.
  MarketsKit& market() { return lazy(market_, [&] { return std::make_unique<MarketsKit>(marketCores()); }); }
  /// Markets' vendors.
  ShopsKit& shops() { return lazy(shops_, [&] { return std::make_unique<ShopsKit>(marketCores()); }); }
  /// Trades between two players at a market.
  TradesKit& trades() { return lazy(trades_, [&] { return std::make_unique<TradesKit>(marketCores()); }); }
  /// Composing and deploying the app's tree of starter hubs (developers).
  KitTopology& topology() { return lazy(topology_, [&] { return std::make_unique<KitTopology>(appId_, exec_); }); }
  /// Keeping a viewer with no avatar where the world's messages reach it.
  SpectateKit& spectate() { return lazy(spectate_, [&] { return std::make_unique<SpectateKit>(runtime_, connection_); }); }
  /// Emotes between players, a client event every kit client reads.
  EmotesKit& emotes() { return lazy(emotes_, [&] { return std::make_unique<EmotesKit>(connection_); }); }
  /// Shard keys, as the hubs compute them.
  ShardKit& shards() { return lazy(shards_, [] { return std::make_unique<ShardKit>(); }); }

  /// Closes the kit's connection (one it was given stays open).
  void close() { runtime_->close(); }

 private:
  template <typename T, typename Make>
  T& lazy(std::unique_ptr<T>& slot, Make make) {
    {
      std::lock_guard lock(mu_);
      if (slot) return *slot;
    }
    auto made = make();
    std::lock_guard lock(mu_);
    if (!slot) slot = std::move(made);
    return *slot;
  }

  MarketCores& marketCores() {
    return lazy(marketCores_, [&] { return std::make_unique<MarketCores>(runtime_, options_.nodes.market); });
  }

  std::string appId_;
  domains::ExecAPI* exec_;
  replication::Connection* connection_;
  GameKitOptions options_;
  SocialKit social_;
  std::shared_ptr<KitRuntime> runtime_;
  std::mutex mu_;
  std::unique_ptr<WorldKit> world_;
  std::unique_ptr<MatchmakingKit> matchmaking_;
  std::unique_ptr<SessionsKit> sessions_;
  std::unique_ptr<NpcsKit> npcs_;
  std::unique_ptr<LeaderboardsKit> leaderboards_;
  std::unique_ptr<TablesKit> tables_;
  std::unique_ptr<TacticsKit> tactics_;
  std::unique_ptr<EncountersKit> encounters_;
  std::unique_ptr<MinigamesKit> minigames_;
  std::unique_ptr<InstancesKit> instances_;
  std::unique_ptr<ArenasKit> arenas_;
  std::unique_ptr<RacingKit> racing_;
  std::unique_ptr<TerritoryKit> territory_;
  std::unique_ptr<PlayerKit> player_;
  std::unique_ptr<InventoryKit> inventory_;
  std::unique_ptr<WalletKit> wallet_;
  std::unique_ptr<ProgressionKit> progression_;
  std::unique_ptr<AchievementsKit> achievements_;
  std::unique_ptr<QuestsKit> quests_;
  std::unique_ptr<CraftingKit> crafting_;
  std::unique_ptr<LootKit> loot_;
  std::unique_ptr<ProductionKit> production_;
  std::unique_ptr<MarketCores> marketCores_;
  std::unique_ptr<MarketsKit> market_;
  std::unique_ptr<ShopsKit> shops_;
  std::unique_ptr<TradesKit> trades_;
  std::unique_ptr<KitTopology> topology_;
  std::unique_ptr<SpectateKit> spectate_;
  std::unique_ptr<EmotesKit> emotes_;
  std::unique_ptr<ShardKit> shards_;
};

/// Build a GameKitClient over a CrowdyClient's domains — the C++ analog of CrowdyJS's
/// client.kit(appId). Pass the replication connection when the social helpers should send
/// channel messages natively and for spectate() and emotes(). With an app token the hub modules
/// connect as the player; with `options.runtime.developer` and an identity session, as a
/// developer.
inline std::unique_ptr<GameKitClient> makeKit(CrowdyClient& client, std::string appId,
                                              replication::Connection* connection = nullptr, GameKitOptions options = {}) {
  return std::make_unique<GameKitClient>(std::move(appId), client.gameApps(), &client.teams(), &client.channels(), connection,
                                         std::move(options), &client.exec(), client.graphqlClient().dispatcher());
}

}  // namespace crowdy::kit
