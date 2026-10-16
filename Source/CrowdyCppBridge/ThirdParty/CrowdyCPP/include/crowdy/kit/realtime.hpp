#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "crowdy/graphql/json.hpp"
#include "crowdy/kit/wire.hpp"

/// What kit hubs show players near them through the realtime tier, decoded: server-driven
/// actors (mobs, NPCs, pets, a ball) and server events. CrowdyJS `src/kit/realtime.ts`.
///
/// A server-driven actor's state is KitActorState as MessagePack (`ckx_sdk::kit::wire`
/// `ActorState`). A server event is an event type from the registry in kit/wire.hpp with a JSON
/// body.
namespace crowdy::kit {

/// A server-driven actor as a kit hub shows it.
struct KitActorState {
  /// What it is: `mob`, `npc`, `pet`, `ball`, `ghost`, or a game's own word.
  std::string kind;
  /// Its id within the hub that shows it.
  std::int64_t id = 0;
  /// Where it is, in chunks.
  double x = 0, y = 0, z = 0;
  std::optional<std::int64_t> hp;
  std::optional<std::int64_t> maxHp;
  std::optional<std::string> mode;
  /// The player it belongs to (a pet, a summon).
  std::optional<std::int64_t> owner;
  std::optional<double> yaw;
  std::optional<std::string> name;
};

/// A kit actor's state from its MessagePack bytes, or nullopt when they are not one (a
/// player's own state, say): a map with `kind`, `id`, `x` and `z`.
inline std::optional<KitActorState> decodeKitActorState(std::string_view bytes) {
  const graphql::Json v = graphql::Json::fromMsgpack(bytes);
  if (!v.isObject() || !v["kind"].isString() || !v["id"].isNumber() || !v["x"].isNumber() || !v["z"].isNumber()) {
    return std::nullopt;
  }
  KitActorState s;
  s.kind = v["kind"].asString();
  s.id = v["id"].asInt64();
  s.x = v["x"].asDouble();
  s.y = v["y"].isNumber() ? v["y"].asDouble() : 0.0;
  s.z = v["z"].asDouble();
  if (v["hp"].isNumber()) s.hp = v["hp"].asInt64();
  if (v["max_hp"].isNumber()) s.maxHp = v["max_hp"].asInt64();
  if (v["mode"].isString()) s.mode = v["mode"].asString();
  if (v["owner"].isNumber()) s.owner = v["owner"].asInt64();
  if (v["yaw"].isNumber()) s.yaw = v["yaw"].asDouble();
  if (v["name"].isString()) s.name = v["name"].asString();
  return s;
}

/// The event types of the kit's registry; each body is JSON.
inline constexpr std::array<std::uint16_t, 13> kKitEventTypes = {
    kEventContactDamage, kEventWeather,     kEventTurn,     kEventScore,      kEventProposal,
    kEventAbility,       kEventMovementViolation, kEventControlPoint, kEventRaceTiming, kEventZoneChange,
    kEventEncounter,     kEventPossession,  kEventEmote,
};

/// Whether `eventType` is in the kit's registry.
inline bool isKitEventType(std::uint16_t eventType) {
  for (auto t : kKitEventTypes) {
    if (t == eventType) return true;
  }
  return false;
}

/// A server event a kit hub sent: its type and the JSON object its state carries, or nullopt
/// when the state is not one.
inline std::optional<EngineEvent> parseKitServerEvent(std::uint16_t eventType, std::string_view state) {
  graphql::Json body = state.empty() ? graphql::Json::parse("{}") : graphql::Json::parse(state);
  if (!body.isObject()) return std::nullopt;
  return EngineEvent{eventType, body};
}

}  // namespace crowdy::kit
