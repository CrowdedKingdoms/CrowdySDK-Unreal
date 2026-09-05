#include "crowdy/player_host/types.hpp"

#include <type_traits>

namespace crowdy::player_host {

std::string_view toString(LeaseScopeV1 value) noexcept {
  switch (value) {
    case LeaseScopeV1::Observe:
      return "observe";
    case LeaseScopeV1::Locomotion:
      return "locomotion";
    case LeaseScopeV1::Interact:
      return "interact";
    case LeaseScopeV1::Craft:
      return "craft";
    case LeaseScopeV1::Combat:
      return "combat";
    case LeaseScopeV1::Communicate:
      return "communicate";
    case LeaseScopeV1::Travel:
      return "travel";
    case LeaseScopeV1::Grid:
      return "grid";
    case LeaseScopeV1::TrustConsent:
      return "trust_consent";
    case LeaseScopeV1::Commerce:
      return "commerce";
  }
  return "";
}

std::string_view toString(CommandKindV1 value) noexcept {
  switch (value) {
    case CommandKindV1::Move:
      return "MOVE";
    case CommandKindV1::Look:
      return "LOOK";
    case CommandKindV1::Stop:
      return "STOP";
    case CommandKindV1::InventorySelect:
      return "INVENTORY_SELECT";
    case CommandKindV1::InventoryConsume:
      return "INVENTORY_CONSUME";
    case CommandKindV1::InventoryTransfer:
      return "INVENTORY_TRANSFER";
    case CommandKindV1::Interact:
      return "INTERACT";
    case CommandKindV1::Craft:
      return "CRAFT";
    case CommandKindV1::Mount:
      return "MOUNT";
    case CommandKindV1::CombatAttack:
      return "COMBAT_ATTACK";
    case CommandKindV1::ChatSend:
      return "CHAT_SEND";
    case CommandKindV1::TravelTeleport:
      return "TRAVEL_TELEPORT";
  }
  return "";
}

std::string_view toolName(CommandKindV1 value) noexcept {
  switch (value) {
    case CommandKindV1::Move:
      return "game.control.move";
    case CommandKindV1::Look:
      return "game.control.look";
    case CommandKindV1::Stop:
      return "game.control.stop";
    case CommandKindV1::InventorySelect:
      return "game.inventory.select";
    case CommandKindV1::InventoryConsume:
      return "game.inventory.consume";
    case CommandKindV1::InventoryTransfer:
      return "game.inventory.transfer";
    case CommandKindV1::Interact:
      return "game.interact";
    case CommandKindV1::Craft:
      return "game.craft";
    case CommandKindV1::Mount:
      return "game.mount";
    case CommandKindV1::CombatAttack:
      return "game.combat.attack";
    case CommandKindV1::ChatSend:
      return "game.chat.send";
    case CommandKindV1::TravelTeleport:
      return "game.travel.teleport";
  }
  return "";
}

std::optional<CommandKindV1> commandKindForToolName(
    std::string_view name) noexcept {
  for (const auto& surface : kMandatoryGameToolSurfacesV1) {
    if (surface.name == name) return surface.command_kind;
  }
  return std::nullopt;
}

CommandKindV1 commandKind(const GameCommandV1& command) noexcept {
  return std::visit(
      [](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, MoveCommandV1>) {
          return CommandKindV1::Move;
        } else if constexpr (std::is_same_v<T, LookCommandV1>) {
          return CommandKindV1::Look;
        } else if constexpr (std::is_same_v<T, StopCommandV1>) {
          return CommandKindV1::Stop;
        } else if constexpr (std::is_same_v<T, InventorySelectCommandV1>) {
          return CommandKindV1::InventorySelect;
        } else if constexpr (std::is_same_v<T, InventoryConsumeCommandV1>) {
          return CommandKindV1::InventoryConsume;
        } else if constexpr (std::is_same_v<T,
                                            InventoryTransferCommandV1>) {
          return CommandKindV1::InventoryTransfer;
        } else if constexpr (std::is_same_v<T, InteractCommandV1>) {
          return CommandKindV1::Interact;
        } else if constexpr (std::is_same_v<T, CraftCommandV1>) {
          return CommandKindV1::Craft;
        } else if constexpr (std::is_same_v<T, MountCommandV1>) {
          return CommandKindV1::Mount;
        } else if constexpr (std::is_same_v<T, CombatAttackCommandV1>) {
          return CommandKindV1::CombatAttack;
        } else if constexpr (std::is_same_v<T, ChatSendCommandV1>) {
          return CommandKindV1::ChatSend;
        } else {
          return CommandKindV1::TravelTeleport;
        }
      },
      command);
}

const PlannedCommandV1* plannedCommand(const GameCommandV1& command) noexcept {
  return std::visit(
      [](const auto& value) -> const PlannedCommandV1* {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, StopCommandV1>) {
          return nullptr;
        } else {
          return &value.planned;
        }
      },
      command);
}

}  // namespace crowdy::player_host
