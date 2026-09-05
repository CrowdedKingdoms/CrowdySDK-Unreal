#include "crowdy/player_host/schemas.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <type_traits>
#include <utility>

#include "crowdy/core/clock.hpp"

namespace crowdy::player_host {
namespace {

ValidationResultV1 text(std::string_view value, std::string_view field,
                        std::size_t maximum, std::size_t minimum = 1) {
  if (value.size() < minimum || value.size() > maximum) {
    return ValidationResultV1::failure(
        std::string(field), "string length is outside contract bounds");
  }
  for (const unsigned char ch : value) {
    if (ch <= 0x1fU || ch == 0x7fU) {
      return ValidationResultV1::failure(std::string(field),
                                         "control characters are not allowed");
    }
  }
  return ValidationResultV1::success();
}

ValidationResultV1 dateTime(std::string_view value, std::string_view field) {
  if (value.size() < 20 || value.size() > 40 || value[4] != '-' ||
      value[7] != '-' || value[10] != 'T' || value[13] != ':' ||
      value[16] != ':' || value.back() != 'Z' ||
      core::parseIso8601Millis(value.data(), value.size()) <= 0) {
    return ValidationResultV1::failure(std::string(field),
                                       "expected an ISO-8601 UTC timestamp");
  }
  return ValidationResultV1::success();
}

ValidationResultV1 vector(const Vector3V1& value, std::string_view field) {
  auto result = validateDecimalStringV1(value.x, std::string(field) + ".x");
  if (!result) return result;
  result = validateDecimalStringV1(value.y, std::string(field) + ".y");
  if (!result) return result;
  return validateDecimalStringV1(value.z, std::string(field) + ".z");
}

ValidationResultV1 planned(const PlannedCommandV1& value) {
  auto result = text(value.observation_id, "observationId",
                     PlayerHostSchemaLimitsV1::max_id_bytes);
  if (!result) return result;
  result = text(value.capability_revision, "capabilityRevision",
                PlayerHostSchemaLimitsV1::max_id_bytes);
  if (!result) return result;
  return text(value.controlled_entity_id, "controlledEntityId",
              PlayerHostSchemaLimitsV1::max_id_bytes);
}

ValidationResultV1 agentError(const AgentErrorV1& value) {
  auto result = text(value.code, "error.code", 64);
  if (!result || value.code.rfind("AGENT_", 0) != 0) {
    return ValidationResultV1::failure("error.code",
                                       "expected a stable AGENT_ error code");
  }
  result = text(value.message, "error.message", 512);
  if (!result) return result;
  if (value.remediation) {
    result = text(*value.remediation, "error.remediation", 512);
    if (!result) return result;
  }
  if (value.field) {
    result = text(*value.field, "error.field", 256);
    if (!result) return result;
  }
  if (value.required_scope) {
    result = text(*value.required_scope, "error.requiredScope", 80);
    if (!result) return result;
  }
  return ValidationResultV1::success();
}

template <typename T>
bool unique(const std::vector<T>& values) {
  return std::set<T>(values.begin(), values.end()).size() == values.size();
}

}  // namespace

ValidationResultV1 ValidationResultV1::success() {
  return ValidationResultV1(std::nullopt);
}

ValidationResultV1 ValidationResultV1::failure(std::string field,
                                                std::string message) {
  return ValidationResultV1(
      ValidationIssueV1{std::move(field), std::move(message)});
}

ValidationResultV1 validateDecimalStringV1(std::string_view value,
                                           std::string_view field) {
  if (value.empty() ||
      value.size() > PlayerHostSchemaLimitsV1::max_decimal_bytes) {
    return ValidationResultV1::failure(std::string(field),
                                       "decimal string length is invalid");
  }
  std::size_t index = value.front() == '-' ? 1 : 0;
  if (index == value.size()) {
    return ValidationResultV1::failure(std::string(field),
                                       "decimal string has no digits");
  }
  const std::size_t integer_start = index;
  while (index < value.size() && value[index] >= '0' && value[index] <= '9') {
    ++index;
  }
  if (integer_start == index ||
      (index - integer_start > 1 && value[integer_start] == '0')) {
    return ValidationResultV1::failure(std::string(field),
                                       "decimal integer part is not canonical");
  }
  if (index == value.size()) return ValidationResultV1::success();
  if (value[index++] != '.') {
    return ValidationResultV1::failure(std::string(field),
                                       "decimal string contains invalid text");
  }
  const std::size_t fraction_start = index;
  while (index < value.size() && value[index] >= '0' && value[index] <= '9') {
    ++index;
  }
  if (index != value.size() || fraction_start == index ||
      index - fraction_start > 9) {
    return ValidationResultV1::failure(
        std::string(field), "decimal fraction must contain one to nine digits");
  }
  return ValidationResultV1::success();
}

ValidationResultV1 validatePlayerHostCapabilitiesV1(
    const PlayerHostCapabilitiesV1& value) {
  if (value.contract_version != kPlayerHostContractV1) {
    return ValidationResultV1::failure("contractVersion",
                                       "unsupported player-host contract");
  }
  auto result = text(value.game_id, "gameId",
                     PlayerHostSchemaLimitsV1::max_id_bytes);
  if (!result) return result;
  result = text(value.revision, "revision",
                PlayerHostSchemaLimitsV1::max_id_bytes);
  if (!result) return result;
  result = text(value.controlled_entity_id, "controlledEntityId",
                PlayerHostSchemaLimitsV1::max_id_bytes);
  if (!result) return result;
  if (value.commands.size() > PlayerHostSchemaLimitsV1::max_commands) {
    return ValidationResultV1::failure("commands",
                                       "too many command capabilities");
  }
  std::set<CommandKindV1> kinds;
  for (const auto& command : value.commands) {
    if (!kinds.insert(command.kind).second) {
      return ValidationResultV1::failure("commands",
                                         "command kinds must be unique");
    }
    if (command.tool_name != toolName(command.kind)) {
      return ValidationResultV1::failure(
          "commands.toolName", "command kind is mapped to the wrong tool");
    }
    if (command.rate_limit_per_second < 1 ||
        command.rate_limit_per_second > 100) {
      return ValidationResultV1::failure(
          "commands.rateLimitPerSecond", "rate limit is outside 1..100");
    }
  }
  if (value.observation.max_age_ms < 100 ||
      value.observation.max_age_ms > 10'000 ||
      value.observation.max_nearby_actors > 128 ||
      value.observation.max_nearby_voxels > 256) {
    return ValidationResultV1::failure(
        "observation", "advertised observation bounds are invalid");
  }
  return dateTime(value.advertised_at, "advertisedAt");
}

ValidationResultV1 validateObserveRequestV1(const ObserveRequestV1& value) {
  if (value.max_nearby_actors >
          PlayerHostSchemaLimitsV1::max_observation_actors ||
      value.max_nearby_voxels >
          PlayerHostSchemaLimitsV1::max_observation_voxels) {
    return ValidationResultV1::failure(
        "observe", "request exceeds protocol observation bounds");
  }
  return ValidationResultV1::success();
}

ValidationResultV1 validateGameObservationV1(const GameObservationV1& value) {
  if (value.contract_version != kGameObservationContractV1) {
    return ValidationResultV1::failure("contractVersion",
                                       "unsupported observation contract");
  }
  auto result = text(value.observation_id, "observationId",
                     PlayerHostSchemaLimitsV1::max_id_bytes);
  if (!result) return result;
  result = text(value.capability_revision, "capabilityRevision",
                PlayerHostSchemaLimitsV1::max_id_bytes);
  if (!result) return result;
  result = text(value.controlled_entity_id, "controlledEntityId",
                PlayerHostSchemaLimitsV1::max_id_bytes);
  if (!result) return result;
  result = dateTime(value.observed_at, "observedAt");
  if (!result) return result;
  result = dateTime(value.expires_at, "expiresAt");
  if (!result) return result;
  result = vector(value.player.position, "player.position");
  if (!result) return result;
  result = vector(value.player.velocity, "player.velocity");
  if (!result) return result;
  result = validateDecimalStringV1(value.player.look.yaw, "player.look.yaw");
  if (!result) return result;
  result =
      validateDecimalStringV1(value.player.look.pitch, "player.look.pitch");
  if (!result) return result;
  result = validateDecimalStringV1(value.player.health, "player.health");
  if (!result) return result;
  result = vector(value.controlled_entity.position, "controlledEntity.position");
  if (!result) return result;
  result = vector(value.controlled_entity.velocity, "controlledEntity.velocity");
  if (!result) return result;

  if (value.target) {
    result = text(value.target->target_id, "target.targetId",
                  PlayerHostSchemaLimitsV1::max_id_bytes);
    if (!result) return result;
    result = validateDecimalStringV1(value.target->distance, "target.distance");
    if (!result) return result;
  }
  if (value.inventory) {
    if (value.inventory->selected_slot > 255 ||
        value.inventory->slots.size() >
            PlayerHostSchemaLimitsV1::max_inventory_slots ||
        value.inventory->craftable_recipe_ids.size() >
            PlayerHostSchemaLimitsV1::max_recipe_ids ||
        !unique(value.inventory->craftable_recipe_ids)) {
      return ValidationResultV1::failure(
          "inventory", "inventory projection is outside deterministic bounds");
    }
    for (const auto& slot : value.inventory->slots) {
      if (slot.slot > 255 || slot.quantity > 1'000'000) {
        return ValidationResultV1::failure(
            "inventory.slots", "inventory slot values are outside bounds");
      }
      result = text(slot.item_id, "inventory.slots.itemId",
                    PlayerHostSchemaLimitsV1::max_id_bytes);
      if (!result) return result;
    }
    for (const auto& recipe : value.inventory->craftable_recipe_ids) {
      result = text(recipe, "inventory.craftableRecipeIds",
                    PlayerHostSchemaLimitsV1::max_id_bytes);
      if (!result) return result;
    }
  }
  if (value.grid) {
    result = text(value.grid->grid_ref, "grid.gridRef",
                  PlayerHostSchemaLimitsV1::max_id_bytes);
    if (!result) return result;
    result = vector(value.grid->low, "grid.low");
    if (!result) return result;
    result = vector(value.grid->high, "grid.high");
    if (!result) return result;
    if (value.grid->effective_scopes.size() > kLeaseScopesV1.size() ||
        !unique(value.grid->effective_scopes)) {
      return ValidationResultV1::failure(
          "grid.effectiveScopes", "effective scopes must be unique and bounded");
    }
  }
  if (value.nearby_actors.size() >
          PlayerHostSchemaLimitsV1::max_observation_actors ||
      value.nearby_voxels.size() >
          PlayerHostSchemaLimitsV1::max_observation_voxels) {
    return ValidationResultV1::failure(
        "nearby", "observation exceeded deterministic array bounds");
  }
  for (const auto& actor : value.nearby_actors) {
    result = text(actor.actor_id, "nearbyActors.actorId",
                  PlayerHostSchemaLimitsV1::max_id_bytes);
    if (!result) return result;
    result = vector(actor.position, "nearbyActors.position");
    if (!result) return result;
    result = validateDecimalStringV1(actor.distance, "nearbyActors.distance");
    if (!result) return result;
    if (actor.label) {
      result = text(*actor.label, "nearbyActors.label", 128, 0);
      if (!result) return result;
    }
    if (actor.health) {
      result = validateDecimalStringV1(*actor.health, "nearbyActors.health");
      if (!result) return result;
    }
  }
  for (const auto& voxel : value.nearby_voxels) {
    result = vector(voxel.position, "nearbyVoxels.position");
    if (!result) return result;
    result = text(voxel.material, "nearbyVoxels.material", 128);
    if (!result) return result;
  }
  return ValidationResultV1::success();
}

ValidationResultV1 validateGameCommandV1(const GameCommandV1& value) {
  const PlannedCommandV1* common = plannedCommand(value);
  if (common) {
    auto result = planned(*common);
    if (!result) return result;
  }
  return std::visit(
      [](const auto& command) -> ValidationResultV1 {
        using T = std::decay_t<decltype(command)>;
        if constexpr (std::is_same_v<T, MoveCommandV1>) {
          if (!std::isfinite(command.intensity) || command.intensity < 0 ||
              command.intensity > 1 || command.duration_ms < 16 ||
              command.duration_ms > 2'000) {
            return ValidationResultV1::failure(
                "move", "movement intensity or duration is outside bounds");
          }
        } else if constexpr (std::is_same_v<T, LookCommandV1>) {
          if (!std::isfinite(command.delta_yaw) ||
              !std::isfinite(command.delta_pitch) ||
              command.delta_yaw < -180 || command.delta_yaw > 180 ||
              command.delta_pitch < -90 || command.delta_pitch > 90) {
            return ValidationResultV1::failure(
                "look", "look delta is outside deterministic bounds");
          }
        } else if constexpr (std::is_same_v<T,
                                            InventorySelectCommandV1>) {
          if (command.slot > 255) {
            return ValidationResultV1::failure("slot",
                                               "slot must be in 0..255");
          }
        } else if constexpr (std::is_same_v<T,
                                            InventoryConsumeCommandV1>) {
          if (command.slot > 255 || command.quantity < 1 ||
              command.quantity > 64) {
            return ValidationResultV1::failure(
                "inventory", "consume slot or quantity is outside bounds");
          }
        } else if constexpr (std::is_same_v<
                                 T, InventoryTransferCommandV1>) {
          if (command.slot > 255 || command.quantity < 1 ||
              command.quantity > 64) {
            return ValidationResultV1::failure(
                "inventory", "transfer slot or quantity is outside bounds");
          }
          return text(command.container_ref, "containerRef",
                      PlayerHostSchemaLimitsV1::max_id_bytes);
        } else if constexpr (std::is_same_v<T, InteractCommandV1>) {
          if (command.inventory_slot && *command.inventory_slot > 255) {
            return ValidationResultV1::failure(
                "inventorySlot", "inventory slot must be in 0..255");
          }
          return text(command.target_ref, "targetRef",
                      PlayerHostSchemaLimitsV1::max_id_bytes);
        } else if constexpr (std::is_same_v<T, CraftCommandV1>) {
          if (command.quantity < 1 || command.quantity > 64) {
            return ValidationResultV1::failure(
                "quantity", "craft quantity must be in 1..64");
          }
          return text(command.recipe_id, "recipeId",
                      PlayerHostSchemaLimitsV1::max_id_bytes);
        } else if constexpr (std::is_same_v<T, MountCommandV1>) {
          if (command.action == MountActionV1::Mount && !command.mount_ref) {
            return ValidationResultV1::failure(
                "mountRef", "mount action requires a mount reference");
          }
          if (command.mount_ref) {
            return text(*command.mount_ref, "mountRef",
                        PlayerHostSchemaLimitsV1::max_id_bytes);
          }
        } else if constexpr (std::is_same_v<T, CombatAttackCommandV1>) {
          return text(command.target_ref, "targetRef",
                      PlayerHostSchemaLimitsV1::max_id_bytes);
        } else if constexpr (std::is_same_v<T, ChatSendCommandV1>) {
          return text(command.text, "text",
                      PlayerHostSchemaLimitsV1::max_chat_bytes);
        } else if constexpr (std::is_same_v<T,
                                            TravelTeleportCommandV1>) {
          return text(command.destination_ref, "destinationRef",
                      PlayerHostSchemaLimitsV1::max_id_bytes);
        }
        return ValidationResultV1::success();
      },
      value);
}

ValidationResultV1 validateGameCommandResultV1(
    const GameCommandResultV1& value) {
  if (value.contract_version != kGameCommandResultContractV1) {
    return ValidationResultV1::failure("contractVersion",
                                       "unsupported command-result contract");
  }
  if (value.observation_id) {
    auto result = text(*value.observation_id, "observationId",
                       PlayerHostSchemaLimitsV1::max_id_bytes);
    if (!result) return result;
  }
  if (value.details.size() > PlayerHostSchemaLimitsV1::max_result_details) {
    return ValidationResultV1::failure("details",
                                       "too many command-result details");
  }
  for (const auto& detail : value.details) {
    auto result = text(detail.name, "details.name", 64);
    if (!result) return result;
    result = text(detail.value, "details.value", 512, 0);
    if (!result) return result;
  }
  if (value.error) return agentError(*value.error);
  return ValidationResultV1::success();
}

ValidationResultV1 validateValidatedGateV1(const ValidatedGateV1& value) {
  if (value.contract_version != kValidatedGateContractV1) {
    return ValidationResultV1::failure("contractVersion",
                                       "unsupported validated-gate contract");
  }
  auto result = text(value.client_epoch, "clientEpoch", 40);
  if (!result) return result;
  if (value.lease_id) {
    result = text(*value.lease_id, "leaseId",
                  PlayerHostSchemaLimitsV1::max_id_bytes);
    if (!result) return result;
  }
  if (value.scopes.size() > kLeaseScopesV1.size() ||
      !unique(value.scopes)) {
    return ValidationResultV1::failure("scopes",
                                       "gate scopes must be unique and bounded");
  }
  result = text(value.context_version, "contextVersion",
                PlayerHostSchemaLimitsV1::max_id_bytes);
  if (!result) return result;
  if (value.observation_id) {
    result = text(*value.observation_id, "observationId",
                  PlayerHostSchemaLimitsV1::max_id_bytes);
    if (!result) return result;
  }
  return dateTime(value.validated_at, "validatedAt");
}

ValidationResultV1 validateAgentControlLeaseV1(
    const AgentControlLeaseV1& value) {
  auto result = text(value.lease_id, "leaseId",
                     PlayerHostSchemaLimitsV1::max_id_bytes);
  if (!result) return result;
  result = text(value.client_epoch, "clientEpoch", 40);
  if (!result) return result;
  if (value.scopes.size() > kLeaseScopesV1.size() ||
      !unique(value.scopes)) {
    return ValidationResultV1::failure(
        "scopes", "lease scopes must be unique and bounded");
  }
  result = text(value.holder, "holder", 128);
  if (!result) return result;
  if (value.controlled_entity_id) {
    result = text(*value.controlled_entity_id, "controlledEntityId",
                  PlayerHostSchemaLimitsV1::max_id_bytes);
    if (!result) return result;
  }
  if (value.host_capability_revision) {
    result = text(*value.host_capability_revision,
                  "hostCapabilityRevision",
                  PlayerHostSchemaLimitsV1::max_id_bytes);
    if (!result) return result;
  }
  result = text(value.context_version, "contextVersion",
                PlayerHostSchemaLimitsV1::max_id_bytes);
  if (!result) return result;
  result = dateTime(value.granted_at, "grantedAt");
  if (!result) return result;
  return dateTime(value.expires_at, "expiresAt");
}

}  // namespace crowdy::player_host
