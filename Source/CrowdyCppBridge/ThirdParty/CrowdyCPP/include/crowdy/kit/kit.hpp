#pragma once

#include <string>
#include <vector>

#include "crowdy/client.hpp"
#include "crowdy/domains/compute.hpp"
#include "crowdy/domains/game_apps.hpp"
#include "crowdy/domains/groups.hpp"
#include "crowdy/kit/actions.hpp"
#include "crowdy/kit/combat.hpp"
#include "crowdy/kit/core.hpp"
#include "crowdy/kit/decks.hpp"
#include "crowdy/kit/economy.hpp"
#include "crowdy/kit/abilities.hpp"
#include "crowdy/kit/director.hpp"
#include "crowdy/kit/engine.hpp"
#include "crowdy/kit/instances.hpp"
#include "crowdy/kit/features.hpp"
#include "crowdy/kit/inventory.hpp"
#include "crowdy/kit/leaderboards.hpp"
#include "crowdy/kit/loot.hpp"
#include "crowdy/kit/liveops.hpp"
#include "crowdy/kit/matches.hpp"
#include "crowdy/kit/matchmaking.hpp"
#include "crowdy/kit/moderation.hpp"
#include "crowdy/kit/movement.hpp"
#include "crowdy/kit/minigames.hpp"
#include "crowdy/kit/mobs.hpp"
#include "crowdy/kit/npcs.hpp"
#include "crowdy/kit/racing.hpp"
#include "crowdy/kit/telemetry.hpp"
#include "crowdy/kit/territory.hpp"
#include "crowdy/kit/objects.hpp"
#include "crowdy/kit/pets.hpp"
#include "crowdy/kit/plots.hpp"
#include "crowdy/kit/progression.hpp"
#include "crowdy/kit/quests.hpp"
#include "crowdy/kit/social.hpp"
#include "crowdy/kit/wire.hpp"
#include "crowdy/kit/worldsim.hpp"

/// The app-scoped Game Kit facade — client.kit(appId): high-level building
/// blocks that map traditional game concepts (inventory, lockable objects,
/// NPCs, land plots, economy, progression, loot, quests, combat, matches,
/// decks, world simulation, social, leaderboards, feature gates) onto the
/// Model/Automations plus optional compute engines. Model-first layers
/// compose client.gameModel(); engine-aware layers use client.compute().
///
/// Two phases, matching the platform's model:
///  1. Studio (admin) loads the rules — deploy() takes declarative
///     KitBlueprints and seeds container types, property schemas, policy-
///     gated functions, and automations in one idempotent pass (requires the
///     app-admin manage_apps permission; run from a trusted admin context).
///  2. The game client plays — the runtime kits wrap the runtime calls
///     assuming the blueprint conventions; authorization is enforced
///     server-side on every call.
namespace crowdy::kit {

/// Options configuring the runtime helpers to match your deployed blueprints
/// (typePrefix values must equal what the blueprints were deployed with).
struct GameKitOptions {
  std::string inventoryTypePrefix;
  OwnerIdKind inventoryOwnerIdKind = OwnerIdKind::Int;
  std::string objectTypeName;
  std::string keyTypeName;
  std::string npcTypeName;
  std::string plotTypeName;
  EconomyKitOptions economy;
  std::string progressionTypePrefix;
  std::string lootTypePrefix;
  std::string questsTypePrefix;
  std::string combatTypePrefix;
  std::string matchesTypePrefix;
  std::string decksTypePrefix;
  std::string worldsimTypePrefix;
  SocialKitOptions social;
  std::string leaderboardsTypePrefix;
  /// Engine-aware helper module names (defaults: npc-engine, mob-engine,
  /// world-engine). Set them to your game's module names (e.g. bwf-mobs).
  std::string npcEngineModule;
  std::string mobEngineModule;
  std::string worldEngineModule;
  std::string mobDefTypeName;
  std::string mobSlotTypeName;
  std::string petTypeName;
  std::string matchEngineModule;
  std::string deckEngineModule;
  std::string instanceEngineModule;
  std::string directorModule;
  std::string matchmakingModule;
  std::string boardEngineModule;
  std::string minigameModule;
  std::string liveopsModule;
  std::string abilitiesModule;
  std::string wardenModule;
  std::string territoryModule;
  std::string racingModule;
  std::string possessionModule;
  std::string liveopsTypePrefix;
  std::string moderationTypePrefix;
};

/// The result of GameKitClient::deploy.
struct KitDeployResult {
  Json seed;
  std::vector<Json> automations;
  std::vector<Json> automationTriggers;
  std::vector<std::string> warnings;  ///< non-fatal static-analysis warnings
};

class GameKitClient {
 public:
  /// `channels`/`teams`/`connection` are optional composition points (match
  /// notify-to-pull channels, social chat); pass what your game uses.
  /// `compute` enables the engine-aware helpers (mobs/pets, capability
  /// detection); without it they report engineAvailable() == false and the
  /// model paths behave exactly as before.
  GameKitClient(std::string appId, domains::GameModelAPI& gameModel,
                domains::GameAppsAPI& gameApps, domains::TeamsAPI* teams = nullptr,
                domains::ChannelsAPI* channels = nullptr,
                replication::Connection* connection = nullptr, GameKitOptions options = {},
                domains::ComputeAPI* compute = nullptr)
      : appId_(appId),
        gameModel_(gameModel),
        engines_(appId, compute),
        inventory_(appId, gameModel, options.inventoryTypePrefix,
                   options.inventoryOwnerIdKind),
        objects_(appId, gameModel, options.objectTypeName, options.keyTypeName),
        npcs_(appId, gameModel, options.npcTypeName, &engines_, options.npcEngineModule),
        plots_(appId, gameModel, gameApps, options.plotTypeName),
        economy_(appId, gameModel, options.economy, &engines_),
        progression_(appId, gameModel, options.progressionTypePrefix),
        loot_(appId, gameModel, options.lootTypePrefix, &engines_),
        quests_(appId, gameModel, options.questsTypePrefix),
        combat_(appId, gameModel, options.combatTypePrefix, &engines_,
                options.mobEngineModule),
        matches_(appId, gameModel, channels, connection, options.matchesTypePrefix,
                 std::nullopt, &engines_, options.matchEngineModule),
        decks_(appId, gameModel, options.decksTypePrefix, &engines_,
               options.deckEngineModule),
        worldsim_(appId, gameModel, options.worldsimTypePrefix, &engines_,
                  options.worldEngineModule),
        social_(appId, teams, channels, gameApps, connection, options.social),
        leaderboards_(appId, gameModel, options.leaderboardsTypePrefix, &engines_,
                      options.boardEngineModule),
        features_(appId, gameModel),
        mobs_(appId, gameModel, engines_,
              options.mobEngineModule.empty() ? "mob-engine" : options.mobEngineModule,
              options.mobDefTypeName.empty() ? "MobDef" : options.mobDefTypeName,
              options.mobSlotTypeName.empty() ? "Mob" : options.mobSlotTypeName),
        pets_(appId, gameModel, engines_,
              options.npcEngineModule.empty() ? "npc-engine" : options.npcEngineModule,
              options.petTypeName.empty() ? "Pet" : options.petTypeName),
        instances_(appId, engines_,
                   options.instanceEngineModule.empty() ? "instance-engine"
                                                        : options.instanceEngineModule),
        director_(appId, gameModel, engines_,
                  options.directorModule.empty() ? "director" : options.directorModule),
        matchmaking_(appId, engines_,
                     options.matchmakingModule.empty() ? "matchmaking"
                                                       : options.matchmakingModule),
        minigames_(appId, engines_, options.minigameModule),
        liveops_(appId, gameModel, options.liveopsTypePrefix, &engines_,
                 options.liveopsModule),
        moderation_(appId, gameModel, options.moderationTypePrefix),
        telemetry_(appId, gameModel),
        abilities_(appId, gameModel, engines_,
                   options.abilitiesModule.empty() ? "abilities-engine" : options.abilitiesModule),
        movement_(appId, gameModel, engines_,
                  options.wardenModule.empty() ? "movement-warden" : options.wardenModule),
        territory_(appId, gameModel, engines_,
                   options.territoryModule.empty() ? "territory" : options.territoryModule),
        racing_(appId, gameModel, engines_,
                options.racingModule.empty() ? "racing" : options.racingModule,
                options.possessionModule.empty() ? "possession" : options.possessionModule) {}

  InventoryKit& inventory() { return inventory_; }
  ObjectsKit& objects() { return objects_; }
  NpcsKit& npcs() { return npcs_; }
  PlotsKit& plots() { return plots_; }
  EconomyKit& economy() { return economy_; }
  ProgressionKit& progression() { return progression_; }
  LootKit& loot() { return loot_; }
  QuestsKit& quests() { return quests_; }
  CombatKit& combat() { return combat_; }
  MatchesKit& matches() { return matches_; }
  DecksKit& decks() { return decks_; }
  WorldsimKit& worldsim() { return worldsim_; }
  SocialKit& social() { return social_; }
  LeaderboardsKit& leaderboards() { return leaderboards_; }
  FeaturesKit& features() { return features_; }
  MobsKit& mobs() { return mobs_; }
  PetsKit& pets() { return pets_; }
  InstancesKit& instances() { return instances_; }
  DirectorKit& director() { return director_; }
  MatchmakingKit& matchmaking() { return matchmaking_; }
  MinigamesKit& minigames() { return minigames_; }
  LiveopsKit& liveops() { return liveops_; }
  ModerationKit& moderation() { return moderation_; }
  TelemetryKit& telemetry() { return telemetry_; }
  AbilitiesKit& abilities() { return abilities_; }
  MovementKit& movement() { return movement_; }
  TerritoryKit& territory() { return territory_; }
  RacingKit& racing() { return racing_; }
  EngineDetector& engines() { return engines_; }

  /// Helpers for an additional lockable object type deployed under a
  /// different type name (e.g. both Door and Chest lock blueprints).
  ObjectsKit objectsFor(std::string_view objectTypeName, std::string_view keyTypeName = {}) {
    return ObjectsKit(appId_, gameModel_, objectTypeName, keyTypeName);
  }

  /// STUDIO (admin): load blueprints into the app — one transactional
  /// gameModelSeed for the definitions, then an upsertAutomation per
  /// automation and an upsertAutomationTrigger per event trigger.
  /// Idempotent: definitions upsert on their names, automations key on the
  /// automation name. Requires the app-admin manage_apps permission; throws
  /// CrowdyGraphQLError (FORBIDDEN / BAD_USER_INPUT) on failure and
  /// std::invalid_argument for duplicate names across blueprints.
  KitDeployResult deploy(const std::vector<KitBlueprint>& blueprints,
                         std::string_view sessionId = {}) {
    MergedBlueprints merged = mergeBlueprints(appId_, blueprints, sessionId);

    KitDeployResult result;
    result.seed = gameModel_.seed(merged.seedInput);
    result.seed["warnings"].forEach(
        [&](Json w) { result.warnings.push_back(w.asString()); });

    for (const auto& automation : merged.automations) {
      result.automations.push_back(gameModel_.upsertAutomation(automation));
    }
    for (const auto& trigger : merged.automationTriggers) {
      result.automationTriggers.push_back(gameModel_.upsertAutomationTrigger(trigger));
    }
    return result;
  }

 private:
  std::string appId_;
  domains::GameModelAPI& gameModel_;
  EngineDetector engines_;
  InventoryKit inventory_;
  ObjectsKit objects_;
  NpcsKit npcs_;
  PlotsKit plots_;
  EconomyKit economy_;
  ProgressionKit progression_;
  LootKit loot_;
  QuestsKit quests_;
  CombatKit combat_;
  MatchesKit matches_;
  DecksKit decks_;
  WorldsimKit worldsim_;
  SocialKit social_;
  LeaderboardsKit leaderboards_;
  FeaturesKit features_;
  MobsKit mobs_;
  PetsKit pets_;
  InstancesKit instances_;
  DirectorKit director_;
  MatchmakingKit matchmaking_;
  MinigamesKit minigames_;
  LiveopsKit liveops_;
  ModerationKit moderation_;
  TelemetryKit telemetry_;
  AbilitiesKit abilities_;
  MovementKit movement_;
  TerritoryKit territory_;
  RacingKit racing_;
};

/// Build a GameKitClient over a CrowdyClient's domains — the C++ analog of
/// CrowdyJS's client.kit(appId). Pass the replication connection when the
/// matches/social helpers should send channel pings natively.
inline GameKitClient makeKit(CrowdyClient& client, std::string appId,
                             replication::Connection* connection = nullptr,
                             GameKitOptions options = {}) {
  return GameKitClient(std::move(appId), client.gameModel(), client.gameApps(), &client.teams(),
                       &client.channels(), connection, std::move(options), &client.compute());
}

}  // namespace crowdy::kit
