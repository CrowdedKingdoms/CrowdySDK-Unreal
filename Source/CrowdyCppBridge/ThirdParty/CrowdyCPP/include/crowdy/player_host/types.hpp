#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "crowdy/generated/enums.hpp"

namespace crowdy::player_host {

inline constexpr std::string_view kPlayerHostContractV1 =
    "crowdy.player-host/1";
inline constexpr std::string_view kGameObservationContractV1 =
    "crowdy.game-observation/1";
inline constexpr std::string_view kGameCommandResultContractV1 =
    "crowdy.game-command-result/1";
inline constexpr std::string_view kValidatedGateContractV1 =
    "crowdy.validated-gate/1";

using PreemptionReasonV1 = gen::CrowdyStudioAgentPreemptionReason;
using ToolRiskV1 = gen::CrowdyStudioAgentToolRisk;

inline constexpr std::array<PreemptionReasonV1, 16>
    kClosedPreemptionReasonsV1 = {
        PreemptionReasonV1::HUMAN_INPUT,
        PreemptionReasonV1::HUMAN_EDIT,
        PreemptionReasonV1::HUMAN_STOP,
        PreemptionReasonV1::ESCAPE,
        PreemptionReasonV1::DEATH,
        PreemptionReasonV1::CONTEXT_CHANGED,
        PreemptionReasonV1::PERMISSION_CHANGED,
        PreemptionReasonV1::ADMISSION_CHANGED,
        PreemptionReasonV1::CONTROL_TARGET_CHANGED,
        PreemptionReasonV1::DISCONNECTED,
        PreemptionReasonV1::CLIENT_REATTACHED,
        PreemptionReasonV1::QUOTA_FAILURE,
        PreemptionReasonV1::BUDGET_FAILURE,
        PreemptionReasonV1::OPERATOR_KILL,
        PreemptionReasonV1::LEASE_EXPIRED,
        PreemptionReasonV1::SESSION_CLOSED,
};

inline constexpr std::string_view preemptionReasonName(
    PreemptionReasonV1 value) noexcept {
  return gen::toString(value);
}

enum class ApprovalPolicyV1 { None, Required, Conditional };

enum class LeaseScopeV1 {
  Observe,
  Locomotion,
  Interact,
  Craft,
  Combat,
  Communicate,
  Travel,
  Grid,
  TrustConsent,
  Commerce,
};

inline constexpr std::array<LeaseScopeV1, 10> kLeaseScopesV1 = {
    LeaseScopeV1::Observe,      LeaseScopeV1::Locomotion,
    LeaseScopeV1::Interact,     LeaseScopeV1::Craft,
    LeaseScopeV1::Combat,       LeaseScopeV1::Communicate,
    LeaseScopeV1::Travel,       LeaseScopeV1::Grid,
    LeaseScopeV1::TrustConsent, LeaseScopeV1::Commerce,
};

enum class CommandKindV1 {
  Move,
  Look,
  Stop,
  InventorySelect,
  InventoryConsume,
  InventoryTransfer,
  Interact,
  Craft,
  Mount,
  CombatAttack,
  ChatSend,
  TravelTeleport,
};

inline constexpr std::array<CommandKindV1, 12> kCommandKindsV1 = {
    CommandKindV1::Move,
    CommandKindV1::Look,
    CommandKindV1::Stop,
    CommandKindV1::InventorySelect,
    CommandKindV1::InventoryConsume,
    CommandKindV1::InventoryTransfer,
    CommandKindV1::Interact,
    CommandKindV1::Craft,
    CommandKindV1::Mount,
    CommandKindV1::CombatAttack,
    CommandKindV1::ChatSend,
    CommandKindV1::TravelTeleport,
};

struct GameToolSurfaceV1 {
  std::string_view name;
  std::optional<CommandKindV1> command_kind;
};

inline constexpr std::array<GameToolSurfaceV1, 14> kMandatoryGameToolSurfacesV1 = {
    GameToolSurfaceV1{"game.capabilities.get", std::nullopt},
    GameToolSurfaceV1{"game.observe", std::nullopt},
    GameToolSurfaceV1{"game.control.move", CommandKindV1::Move},
    GameToolSurfaceV1{"game.control.look", CommandKindV1::Look},
    GameToolSurfaceV1{"game.control.stop", CommandKindV1::Stop},
    GameToolSurfaceV1{"game.inventory.select",
                      CommandKindV1::InventorySelect},
    GameToolSurfaceV1{"game.inventory.consume",
                      CommandKindV1::InventoryConsume},
    GameToolSurfaceV1{"game.inventory.transfer",
                      CommandKindV1::InventoryTransfer},
    GameToolSurfaceV1{"game.interact", CommandKindV1::Interact},
    GameToolSurfaceV1{"game.craft", CommandKindV1::Craft},
    GameToolSurfaceV1{"game.mount", CommandKindV1::Mount},
    GameToolSurfaceV1{"game.combat.attack", CommandKindV1::CombatAttack},
    GameToolSurfaceV1{"game.chat.send", CommandKindV1::ChatSend},
    GameToolSurfaceV1{"game.travel.teleport",
                      CommandKindV1::TravelTeleport},
};

std::string_view toString(LeaseScopeV1 value) noexcept;
std::string_view toString(CommandKindV1 value) noexcept;
std::string_view toolName(CommandKindV1 value) noexcept;
std::optional<CommandKindV1> commandKindForToolName(
    std::string_view name) noexcept;

/**
 * Decimal-string vectors preserve coordinates that cannot be represented
 * exactly by JSON numbers, JavaScript numbers, or every engine scalar type.
 */
struct Vector3V1 {
  std::string x;
  std::string y;
  std::string z;
  bool operator==(const Vector3V1&) const = default;
};

struct LookV1 {
  std::string yaw;
  std::string pitch;
  bool operator==(const LookV1&) const = default;
};

struct CommandCapabilityV1 {
  CommandKindV1 kind = CommandKindV1::Stop;
  std::string tool_name;
  std::optional<LeaseScopeV1> required_scope;
  ToolRiskV1 risk = ToolRiskV1::READ_ONLY;
  ApprovalPolicyV1 approval = ApprovalPolicyV1::None;
  std::uint32_t rate_limit_per_second = 1;
  bool operator==(const CommandCapabilityV1&) const = default;
};

struct ObservationCapabilityV1 {
  std::uint32_t max_age_ms = 2'000;
  std::uint32_t max_nearby_actors = 0;
  std::uint32_t max_nearby_voxels = 0;
  bool operator==(const ObservationCapabilityV1&) const = default;
};

struct PlayerHostCapabilitiesV1 {
  std::string contract_version{std::string(kPlayerHostContractV1)};
  std::string game_id;
  std::string revision;
  std::string controlled_entity_id;
  std::vector<CommandCapabilityV1> commands;
  ObservationCapabilityV1 observation;
  std::string advertised_at;
  bool operator==(const PlayerHostCapabilitiesV1&) const = default;
};

enum class ObserveDetailV1 { Minimal, Standard, Tactical };

struct ObserveRequestV1 {
  ObserveDetailV1 detail = ObserveDetailV1::Standard;
  std::uint32_t max_nearby_actors = 0;
  std::uint32_t max_nearby_voxels = 0;
  bool operator==(const ObserveRequestV1&) const = default;
};

enum class ActorKindV1 { Player, Npc, Mob, Object, Vehicle };
enum class ActorDispositionV1 { Self, Friendly, Neutral, Hostile, Unknown };

struct ObservationActorV1 {
  std::string actor_id;
  ActorKindV1 kind = ActorKindV1::Object;
  Vector3V1 position;
  std::string distance;
  ActorDispositionV1 disposition = ActorDispositionV1::Unknown;
  std::optional<std::string> label;
  std::optional<std::string> health;
  bool operator==(const ObservationActorV1&) const = default;
};

struct ObservationInventorySlotV1 {
  std::uint32_t slot = 0;
  std::string item_id;
  std::uint32_t quantity = 0;
  bool usable = false;
  bool operator==(const ObservationInventorySlotV1&) const = default;
};

struct ObservationInventoryV1 {
  std::uint32_t selected_slot = 0;
  std::vector<ObservationInventorySlotV1> slots;
  std::vector<std::string> craftable_recipe_ids;
  bool operator==(const ObservationInventoryV1&) const = default;
};

enum class ControlledEntityKindV1 { Player, Mount, Vehicle };

struct ObservationPlayerV1 {
  Vector3V1 position;
  Vector3V1 velocity;
  LookV1 look;
  std::string health;
  bool alive = true;
  bool operator==(const ObservationPlayerV1&) const = default;
};

struct ObservationControlledEntityV1 {
  ControlledEntityKindV1 kind = ControlledEntityKindV1::Player;
  Vector3V1 position;
  Vector3V1 velocity;
  bool operator==(const ObservationControlledEntityV1&) const = default;
};

enum class ObservationTargetKindV1 { Actor, Voxel, Object, None };

struct ObservationTargetV1 {
  std::string target_id;
  ObservationTargetKindV1 kind = ObservationTargetKindV1::None;
  std::string distance;
  bool operator==(const ObservationTargetV1&) const = default;
};

struct ObservationGridV1 {
  std::string grid_ref;
  Vector3V1 low;
  Vector3V1 high;
  std::vector<LeaseScopeV1> effective_scopes;
  bool operator==(const ObservationGridV1&) const = default;
};

enum class VoxelInteractionV1 { None, Mine, Place, Use };

struct ObservationVoxelV1 {
  Vector3V1 position;
  std::string material;
  VoxelInteractionV1 interaction = VoxelInteractionV1::None;
  bool operator==(const ObservationVoxelV1&) const = default;
};

struct ObservationInputStateV1 {
  bool modal_open = false;
  bool text_input_focused = false;
  bool human_input_active = false;
  bool operator==(const ObservationInputStateV1&) const = default;
};

struct GameObservationV1 {
  std::string contract_version{std::string(kGameObservationContractV1)};
  std::string observation_id;
  std::string capability_revision;
  std::string controlled_entity_id;
  std::string observed_at;
  std::string expires_at;
  ObservationPlayerV1 player;
  ObservationControlledEntityV1 controlled_entity;
  std::optional<ObservationTargetV1> target;
  std::optional<ObservationInventoryV1> inventory;
  std::optional<ObservationGridV1> grid;
  std::vector<ObservationActorV1> nearby_actors;
  std::vector<ObservationVoxelV1> nearby_voxels;
  ObservationInputStateV1 input_state;
  bool operator==(const GameObservationV1&) const = default;
};

struct PlannedCommandV1 {
  std::string observation_id;
  std::string capability_revision;
  std::string controlled_entity_id;
  bool operator==(const PlannedCommandV1&) const = default;
};

enum class MoveDirectionV1 { Forward, Backward, Left, Right, Up, Down };

struct MoveCommandV1 {
  PlannedCommandV1 planned;
  MoveDirectionV1 direction = MoveDirectionV1::Forward;
  double intensity = 0;
  std::uint32_t duration_ms = 16;
  bool operator==(const MoveCommandV1&) const = default;
};

struct LookCommandV1 {
  PlannedCommandV1 planned;
  double delta_yaw = 0;
  double delta_pitch = 0;
  bool operator==(const LookCommandV1&) const = default;
};

struct StopCommandV1 {
  bool operator==(const StopCommandV1&) const = default;
};

struct InventorySelectCommandV1 {
  PlannedCommandV1 planned;
  std::uint32_t slot = 0;
  bool operator==(const InventorySelectCommandV1&) const = default;
};

struct InventoryConsumeCommandV1 {
  PlannedCommandV1 planned;
  std::uint32_t slot = 0;
  std::uint32_t quantity = 1;
  bool operator==(const InventoryConsumeCommandV1&) const = default;
};

enum class InventoryTransferDirectionV1 { ToContainer, FromContainer };

struct InventoryTransferCommandV1 {
  PlannedCommandV1 planned;
  InventoryTransferDirectionV1 direction =
      InventoryTransferDirectionV1::ToContainer;
  std::uint32_t slot = 0;
  std::uint32_t quantity = 1;
  std::string container_ref;
  bool operator==(const InventoryTransferCommandV1&) const = default;
};

enum class InteractActionV1 { Mine, Place, Use, Fish, NpcTalk };

struct InteractCommandV1 {
  PlannedCommandV1 planned;
  InteractActionV1 action = InteractActionV1::Use;
  std::string target_ref;
  std::optional<std::uint32_t> inventory_slot;
  bool operator==(const InteractCommandV1&) const = default;
};

struct CraftCommandV1 {
  PlannedCommandV1 planned;
  std::string recipe_id;
  std::uint32_t quantity = 1;
  bool operator==(const CraftCommandV1&) const = default;
};

enum class MountActionV1 { Mount, Dismount };

struct MountCommandV1 {
  PlannedCommandV1 planned;
  MountActionV1 action = MountActionV1::Dismount;
  std::optional<std::string> mount_ref;
  bool operator==(const MountCommandV1&) const = default;
};

enum class CombatAttackV1 { Primary, Secondary };

struct CombatAttackCommandV1 {
  PlannedCommandV1 planned;
  std::string target_ref;
  CombatAttackV1 attack = CombatAttackV1::Primary;
  bool operator==(const CombatAttackCommandV1&) const = default;
};

enum class ChatChannelV1 { Local, Group };

struct ChatSendCommandV1 {
  PlannedCommandV1 planned;
  ChatChannelV1 channel = ChatChannelV1::Local;
  std::string text;
  bool operator==(const ChatSendCommandV1&) const = default;
};

struct TravelTeleportCommandV1 {
  PlannedCommandV1 planned;
  std::string destination_ref;
  bool operator==(const TravelTeleportCommandV1&) const = default;
};

using GameCommandV1 =
    std::variant<MoveCommandV1, LookCommandV1, StopCommandV1,
                 InventorySelectCommandV1, InventoryConsumeCommandV1,
                 InventoryTransferCommandV1, InteractCommandV1, CraftCommandV1,
                 MountCommandV1, CombatAttackCommandV1, ChatSendCommandV1,
                 TravelTeleportCommandV1>;

CommandKindV1 commandKind(const GameCommandV1& command) noexcept;
const PlannedCommandV1* plannedCommand(const GameCommandV1& command) noexcept;

struct AgentErrorV1 {
  std::string code;
  std::string message;
  bool retryable = false;
  std::optional<std::string> remediation;
  std::optional<std::string> field;
  std::optional<std::string> required_scope;
  bool operator==(const AgentErrorV1&) const = default;
};

enum class CommandResultStatusV1 {
  Succeeded,
  Failed,
  Denied,
  OutcomeUnknown,
};

struct CommandResultDetailV1 {
  std::string name;
  std::string value;
  bool operator==(const CommandResultDetailV1&) const = default;
};

struct GameCommandResultV1 {
  std::string contract_version{std::string(kGameCommandResultContractV1)};
  CommandResultStatusV1 status = CommandResultStatusV1::Failed;
  CommandKindV1 command_kind = CommandKindV1::Stop;
  std::optional<std::string> observation_id;
  std::vector<CommandResultDetailV1> details;
  std::optional<AgentErrorV1> error;
  bool operator==(const GameCommandResultV1&) const = default;
};

struct ValidatedGateV1 {
  std::string contract_version{std::string(kValidatedGateContractV1)};
  std::string client_epoch;
  std::optional<std::string> lease_id;
  std::vector<LeaseScopeV1> scopes;
  std::string context_version;
  std::optional<std::string> observation_id;
  std::string validated_at;
  bool operator==(const ValidatedGateV1&) const = default;
};

enum class LeaseKindV1 { Workspace, Play };
enum class LeaseStatusV1 { Active, Revoked, Expired };

struct AgentControlLeaseV1 {
  std::string lease_id;
  LeaseKindV1 kind = LeaseKindV1::Play;
  LeaseStatusV1 status = LeaseStatusV1::Active;
  std::string client_epoch;
  std::vector<LeaseScopeV1> scopes;
  std::string holder;
  std::optional<std::string> controlled_entity_id;
  std::optional<std::string> host_capability_revision;
  std::string context_version;
  std::string granted_at;
  std::string expires_at;
  std::optional<PreemptionReasonV1> revoked_reason;
  bool operator==(const AgentControlLeaseV1&) const = default;
};

struct AgentControlHeartbeatV1 {
  std::string lease_id;
  std::string client_epoch;
  std::string context_version;
  std::string controlled_entity_id;
  std::string host_capability_revision;
  bool operator==(const AgentControlHeartbeatV1&) const = default;
};

}  // namespace crowdy::player_host
