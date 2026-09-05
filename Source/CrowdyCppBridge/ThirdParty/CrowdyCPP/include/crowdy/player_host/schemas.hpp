#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "crowdy/player_host/types.hpp"

namespace crowdy::player_host {

struct ValidationIssueV1 {
  std::string field;
  std::string message;
  bool operator==(const ValidationIssueV1&) const = default;
};

class ValidationResultV1 {
 public:
  static ValidationResultV1 success();
  static ValidationResultV1 failure(std::string field, std::string message);

  bool ok() const noexcept { return !issue_.has_value(); }
  explicit operator bool() const noexcept { return ok(); }
  const std::optional<ValidationIssueV1>& issue() const noexcept {
    return issue_;
  }

 private:
  explicit ValidationResultV1(std::optional<ValidationIssueV1> issue)
      : issue_(std::move(issue)) {}
  std::optional<ValidationIssueV1> issue_;
};

struct PlayerHostSchemaLimitsV1 {
  static constexpr std::size_t max_id_bytes = 128;
  static constexpr std::size_t max_decimal_bytes = 48;
  static constexpr std::size_t max_chat_bytes = 280;
  static constexpr std::size_t max_commands = 64;
  static constexpr std::size_t max_observation_actors = 64;
  static constexpr std::size_t max_observation_voxels = 128;
  static constexpr std::size_t max_inventory_slots = 256;
  static constexpr std::size_t max_recipe_ids = 128;
  static constexpr std::size_t max_result_details = 32;
};

struct CommandSchemaV1 {
  CommandKindV1 kind;
  std::string_view tool_name;
  std::optional<LeaseScopeV1> default_scope;
};

inline constexpr std::array<CommandSchemaV1, 12> kGameCommandSchemasV1 = {
    CommandSchemaV1{CommandKindV1::Move, "game.control.move",
                    LeaseScopeV1::Locomotion},
    CommandSchemaV1{CommandKindV1::Look, "game.control.look",
                    LeaseScopeV1::Locomotion},
    CommandSchemaV1{CommandKindV1::Stop, "game.control.stop", std::nullopt},
    CommandSchemaV1{CommandKindV1::InventorySelect, "game.inventory.select",
                    LeaseScopeV1::Interact},
    CommandSchemaV1{CommandKindV1::InventoryConsume, "game.inventory.consume",
                    LeaseScopeV1::Interact},
    CommandSchemaV1{CommandKindV1::InventoryTransfer,
                    "game.inventory.transfer", LeaseScopeV1::Interact},
    CommandSchemaV1{CommandKindV1::Interact, "game.interact",
                    LeaseScopeV1::Interact},
    CommandSchemaV1{CommandKindV1::Craft, "game.craft",
                    LeaseScopeV1::Craft},
    CommandSchemaV1{CommandKindV1::Mount, "game.mount",
                    LeaseScopeV1::Locomotion},
    CommandSchemaV1{CommandKindV1::CombatAttack, "game.combat.attack",
                    LeaseScopeV1::Combat},
    CommandSchemaV1{CommandKindV1::ChatSend, "game.chat.send",
                    LeaseScopeV1::Communicate},
    CommandSchemaV1{CommandKindV1::TravelTeleport, "game.travel.teleport",
                    LeaseScopeV1::Travel},
};

ValidationResultV1 validateDecimalStringV1(std::string_view value,
                                           std::string_view field);
ValidationResultV1 validatePlayerHostCapabilitiesV1(
    const PlayerHostCapabilitiesV1& value);
ValidationResultV1 validateObserveRequestV1(const ObserveRequestV1& value);
ValidationResultV1 validateGameObservationV1(const GameObservationV1& value);
ValidationResultV1 validateGameCommandV1(const GameCommandV1& value);
ValidationResultV1 validateGameCommandResultV1(
    const GameCommandResultV1& value);
ValidationResultV1 validateValidatedGateV1(const ValidatedGateV1& value);
ValidationResultV1 validateAgentControlLeaseV1(
    const AgentControlLeaseV1& value);

}  // namespace crowdy::player_host
