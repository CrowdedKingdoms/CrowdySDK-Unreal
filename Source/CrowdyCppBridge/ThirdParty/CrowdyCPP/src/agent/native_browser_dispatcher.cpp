#include "crowdy/agent/native_browser_dispatcher.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <limits>
#include <stdexcept>
#include <utility>

#include "crowdy/agent/schema.hpp"

namespace crowdy::agent {
namespace {

using graphql::JArray;
using graphql::JVal;
using graphql::Json;
using namespace player_host;

[[noreturn]] void invalid(std::string field, std::string message) {
  throw CrowdyAgentError("AGENT_TOOL_INPUT_INVALID", std::move(message),
                         false, std::nullopt, std::move(field));
}

std::string text(const Json& value, std::string_view field) {
  const auto child = value[field];
  if (!child.isString()) {
    invalid(std::string(field), std::string(field) + " must be a string");
  }
  return child.asString();
}

std::optional<std::string> optionalText(const Json& value,
                                        std::string_view field) {
  const auto child = value[field];
  if (!child.ok() || child.isNull()) return std::nullopt;
  if (!child.isString()) {
    invalid(std::string(field), std::string(field) + " must be a string");
  }
  return child.asString();
}

std::uint32_t uint32(const Json& value, std::string_view field) {
  const auto child = value[field];
  const auto number = child.asInt64(-1);
  if (!child.isNumber() || number < 0 ||
      number > std::numeric_limits<std::uint32_t>::max()) {
    invalid(std::string(field),
            std::string(field) + " must be an unsigned integer");
  }
  return static_cast<std::uint32_t>(number);
}

double number(const Json& value, std::string_view field) {
  const auto child = value[field];
  if (!child.isNumber()) {
    invalid(std::string(field), std::string(field) + " must be a number");
  }
  return child.asDouble();
}

template <typename Enum>
Enum enumValue(std::string_view value, std::string_view field,
               std::initializer_list<std::pair<std::string_view, Enum>>
                   choices) {
  const auto found = std::find_if(
      choices.begin(), choices.end(),
      [&](const auto& choice) { return choice.first == value; });
  if (found == choices.end()) {
    invalid(std::string(field),
            std::string(field) + " contains an unknown value");
  }
  return found->second;
}

PlannedCommandV1 planned(const Json& value) {
  return PlannedCommandV1{
      .observation_id = text(value, "observationId"),
      .capability_revision = text(value, "capabilityRevision"),
      .controlled_entity_id = text(value, "controlledEntityId")};
}

std::vector<StudioTargetV1> targets(const Json& value) {
  const auto array = value["targets"];
  if (!array.isArray()) invalid("targets", "targets must be an array");
  std::vector<StudioTargetV1> result;
  result.reserve(array.size());
  array.forEach([&](const Json& entry) {
    result.push_back(enumValue<StudioTargetV1>(
        entry.asStringView(), "targets",
        {{"SERVER", StudioTargetV1::Server},
         {"CLIENT", StudioTargetV1::Client}}));
  });
  return result;
}

NativeToolArgumentsV1 nativeArguments(std::string_view name,
                                      const Json& value) {
  if (name == "game.capabilities.get" || name == "studio.context.get" ||
      name == "studio.state.get" || name == "diagnostics.local.get" ||
      name == "runtime.status.get" || name == "runtime.stop") {
    return NoArgumentsV1{};
  }
  if (name == "game.observe") {
    return ObserveRequestV1{
        .detail = enumValue<ObserveDetailV1>(
            text(value, "detail"), "detail",
            {{"MINIMAL", ObserveDetailV1::Minimal},
             {"STANDARD", ObserveDetailV1::Standard},
             {"TACTICAL", ObserveDetailV1::Tactical}}),
        .max_nearby_actors = uint32(value, "maxNearbyActors"),
        .max_nearby_voxels = uint32(value, "maxNearbyVoxels")};
  }
  if (name == "game.control.move") {
    return GameCommandV1{MoveCommandV1{
        .planned = planned(value),
        .direction = enumValue<MoveDirectionV1>(
            text(value, "direction"), "direction",
            {{"FORWARD", MoveDirectionV1::Forward},
             {"BACKWARD", MoveDirectionV1::Backward},
             {"LEFT", MoveDirectionV1::Left},
             {"RIGHT", MoveDirectionV1::Right},
             {"UP", MoveDirectionV1::Up},
             {"DOWN", MoveDirectionV1::Down}}),
        .intensity = number(value, "intensity"),
        .duration_ms = uint32(value, "durationMs")}};
  }
  if (name == "game.control.look") {
    return GameCommandV1{LookCommandV1{
        .planned = planned(value),
        .delta_yaw = number(value, "deltaYaw"),
        .delta_pitch = number(value, "deltaPitch")}};
  }
  if (name == "game.control.stop") {
    return GameCommandV1{StopCommandV1{}};
  }
  if (name == "game.inventory.select") {
    return GameCommandV1{InventorySelectCommandV1{
        .planned = planned(value), .slot = uint32(value, "slot")}};
  }
  if (name == "game.inventory.consume") {
    return GameCommandV1{InventoryConsumeCommandV1{
        .planned = planned(value),
        .slot = uint32(value, "slot"),
        .quantity = uint32(value, "quantity")}};
  }
  if (name == "game.inventory.transfer") {
    return GameCommandV1{InventoryTransferCommandV1{
        .planned = planned(value),
        .direction = enumValue<InventoryTransferDirectionV1>(
            text(value, "direction"), "direction",
            {{"TO_CONTAINER",
              InventoryTransferDirectionV1::ToContainer},
             {"FROM_CONTAINER",
              InventoryTransferDirectionV1::FromContainer}}),
        .slot = uint32(value, "slot"),
        .quantity = uint32(value, "quantity"),
        .container_ref = text(value, "containerRef")}};
  }
  if (name == "game.interact") {
    std::optional<std::uint32_t> slot;
    if (value["inventorySlot"].ok() && !value["inventorySlot"].isNull()) {
      slot = uint32(value, "inventorySlot");
    }
    return GameCommandV1{InteractCommandV1{
        .planned = planned(value),
        .action = enumValue<InteractActionV1>(
            text(value, "action"), "action",
            {{"MINE", InteractActionV1::Mine},
             {"PLACE", InteractActionV1::Place},
             {"USE", InteractActionV1::Use},
             {"FISH", InteractActionV1::Fish},
             {"NPC_TALK", InteractActionV1::NpcTalk}}),
        .target_ref = text(value, "targetRef"),
        .inventory_slot = slot}};
  }
  if (name == "game.craft") {
    return GameCommandV1{CraftCommandV1{
        .planned = planned(value),
        .recipe_id = text(value, "recipeId"),
        .quantity = uint32(value, "quantity")}};
  }
  if (name == "game.mount") {
    return GameCommandV1{MountCommandV1{
        .planned = planned(value),
        .action = enumValue<MountActionV1>(
            text(value, "action"), "action",
            {{"MOUNT", MountActionV1::Mount},
             {"DISMOUNT", MountActionV1::Dismount}}),
        .mount_ref = optionalText(value, "mountRef")}};
  }
  if (name == "game.combat.attack") {
    return GameCommandV1{CombatAttackCommandV1{
        .planned = planned(value),
        .target_ref = text(value, "targetRef"),
        .attack = enumValue<CombatAttackV1>(
            text(value, "attack"), "attack",
            {{"PRIMARY", CombatAttackV1::Primary},
             {"SECONDARY", CombatAttackV1::Secondary}})}};
  }
  if (name == "game.chat.send") {
    return GameCommandV1{ChatSendCommandV1{
        .planned = planned(value),
        .channel = enumValue<ChatChannelV1>(
            text(value, "channel"), "channel",
            {{"LOCAL", ChatChannelV1::Local},
             {"GROUP", ChatChannelV1::Group}}),
        .text = text(value, "text")}};
  }
  if (name == "game.travel.teleport") {
    return GameCommandV1{TravelTeleportCommandV1{
        .planned = planned(value),
        .destination_ref = text(value, "destinationRef")}};
  }
  if (name == "project.select") {
    return StudioProjectSelectRequestV1{
        .project_ref = text(value, "projectRef")};
  }
  if (name == "workspace.tab.open" ||
      name == "workspace.tab.close") {
    return StudioFileTabRequestV1{
        .source = enumValue<StudioFileSourceV1>(
            text(value, "source"), "source",
            {{"PROJECT", StudioFileSourceV1::Project},
             {"PERSONAL_LIBRARY",
              StudioFileSourceV1::PersonalLibrary},
             {"COMMON", StudioFileSourceV1::Common}}),
        .target = enumValue<StudioTargetV1>(
            text(value, "target"), "target",
            {{"SERVER", StudioTargetV1::Server},
             {"CLIENT", StudioTargetV1::Client}}),
        .path = text(value, "path"),
        .reference_ref = optionalText(value, "referenceRef")};
  }
  if (name == "runtime.test_draft") {
    return StudioRuntimeTestDraftRequestV1{
        .expected_revision = text(value, "expectedRevision"),
        .targets = targets(value)};
  }
  if (name == "runtime.deploy_live") {
    return StudioRuntimeDeployLiveRequestV1{
        .expected_revision = text(value, "expectedRevision"),
        .project_content_hash = text(value, "projectContentHash"),
        .targets = targets(value),
        .pairing_preference = enumValue<StudioPairingPreferenceV1>(
            text(value, "pairingPreference"), "pairingPreference",
            {{"NONE", StudioPairingPreferenceV1::None},
             {"OPTIONAL", StudioPairingPreferenceV1::Optional},
             {"REQUIRED", StudioPairingPreferenceV1::Required}}),
        .draft = value["draft"].asBool(true)};
  }
  if (name == "runtime.invoke") {
    StudioRuntimeInvokeRequestV1 result;
    result.export_name = text(value, "exportName");
    result.environment = enumValue<StudioRuntimeEnvironmentV1>(
        text(value, "environment"), "environment",
        {{"DRAFT", StudioRuntimeEnvironmentV1::Draft},
         {"LIVE", StudioRuntimeEnvironmentV1::Live}});
    const auto params = value["params"];
    if (!params.isArray()) invalid("params", "params must be an array");
    params.forEach([&](const Json& entry) {
      result.params.push_back(StudioRuntimeParameterV1{
          .name = text(entry, "name"),
          .type = enumValue<StudioRuntimeParameterTypeV1>(
              text(entry, "type"), "type",
              {{"STRING", StudioRuntimeParameterTypeV1::String},
               {"DECIMAL", StudioRuntimeParameterTypeV1::Decimal},
               {"BOOLEAN", StudioRuntimeParameterTypeV1::Boolean}}),
          .value = text(entry, "value")});
    });
    return result;
  }
  invalid("name", "tool is not a native local executor surface");
}

void validateExtensionInput(std::string_view name, const Json& value) {
  std::vector<std::string_view> allowed;
  if (name == "studio.state.get") {
    allowed = {};
  } else if (name == "project.select") {
    allowed = {"projectRef"};
  } else if (name == "workspace.tab.open" ||
             name == "workspace.tab.close") {
    allowed = {"source", "target", "path", "referenceRef"};
  } else {
    invalid("name", "tool is not registered in the canonical native subset");
  }
  if (!value.isObject()) invalid("$", "tool arguments must be an object");
  bool unknown = false;
  value.forEachMember([&](std::string_view key, const Json&) {
    unknown = unknown ||
              std::find(allowed.begin(), allowed.end(), key) ==
                  allowed.end();
  });
  if (unknown || value.size() > allowed.size()) {
    invalid("$", "native extension input contains unknown fields");
  }
}

NativeToolInvocationV1 nativeInvocation(
    const AgentToolInvocation& source, const AgentToolRegistry& registry) {
  if (registry.get(source.name, source.version)) {
    registry.validateInput(source.name, source.version, source.arguments);
  } else {
    validateExtensionInput(source.name, source.arguments);
  }
  NativeToolInvocationV1 result;
  result.protocol_version = source.protocolVersion;
  result.session_id = source.sessionId;
  result.run_id = source.runId;
  result.tool_call_id = source.toolCallId;
  result.name = source.name;
  result.version = source.version;
  result.descriptor_digest = source.descriptorDigest;
  result.arguments = nativeArguments(source.name, source.arguments);
  result.argument_hash = source.argumentHash;
  result.context_version = source.contextVersion;
  result.client_epoch = source.clientEpoch;
  result.lease_id = source.leaseId;
  result.approval_grant = source.approvalGrant;
  result.idempotency_key = source.idempotencyKey;
  result.deadline = source.deadline;
  return result;
}

std::string isoTime(std::int64_t epochMs) {
  const std::time_t seconds = static_cast<std::time_t>(epochMs / 1'000);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif
  char buffer[96];
  std::snprintf(buffer, sizeof(buffer),
                "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                utc.tm_min, utc.tm_sec,
                static_cast<long long>(epochMs % 1'000));
  return buffer;
}

std::string_view approvalName(ApprovalPolicyV1 value) {
  switch (value) {
    case ApprovalPolicyV1::None:
      return "NONE";
    case ApprovalPolicyV1::Required:
      return "REQUIRED";
    case ApprovalPolicyV1::Conditional:
      return "CONDITIONAL";
  }
  return "";
}

std::string_view actorKindName(ActorKindV1 value) {
  switch (value) {
    case ActorKindV1::Player:
      return "PLAYER";
    case ActorKindV1::Npc:
      return "NPC";
    case ActorKindV1::Mob:
      return "MOB";
    case ActorKindV1::Object:
      return "OBJECT";
    case ActorKindV1::Vehicle:
      return "VEHICLE";
  }
  return "";
}

std::string_view dispositionName(ActorDispositionV1 value) {
  switch (value) {
    case ActorDispositionV1::Self:
      return "SELF";
    case ActorDispositionV1::Friendly:
      return "FRIENDLY";
    case ActorDispositionV1::Neutral:
      return "NEUTRAL";
    case ActorDispositionV1::Hostile:
      return "HOSTILE";
    case ActorDispositionV1::Unknown:
      return "UNKNOWN";
  }
  return "";
}

std::string_view controlledKindName(ControlledEntityKindV1 value) {
  switch (value) {
    case ControlledEntityKindV1::Player:
      return "PLAYER";
    case ControlledEntityKindV1::Mount:
      return "MOUNT";
    case ControlledEntityKindV1::Vehicle:
      return "VEHICLE";
  }
  return "";
}

std::string_view targetKindName(ObservationTargetKindV1 value) {
  switch (value) {
    case ObservationTargetKindV1::Actor:
      return "ACTOR";
    case ObservationTargetKindV1::Voxel:
      return "VOXEL";
    case ObservationTargetKindV1::Object:
      return "OBJECT";
    case ObservationTargetKindV1::None:
      return "NONE";
  }
  return "";
}

std::string_view interactionName(VoxelInteractionV1 value) {
  switch (value) {
    case VoxelInteractionV1::None:
      return "NONE";
    case VoxelInteractionV1::Mine:
      return "MINE";
    case VoxelInteractionV1::Place:
      return "PLACE";
    case VoxelInteractionV1::Use:
      return "USE";
  }
  return "";
}

std::string_view commandStatusName(CommandResultStatusV1 value) {
  switch (value) {
    case CommandResultStatusV1::Succeeded:
      return "SUCCEEDED";
    case CommandResultStatusV1::Failed:
      return "FAILED";
    case CommandResultStatusV1::Denied:
      return "DENIED";
    case CommandResultStatusV1::OutcomeUnknown:
      return "OUTCOME_UNKNOWN";
  }
  return "";
}

JVal vector3(const Vector3V1& value) {
  return JVal::object(
      {{"x", value.x}, {"y", value.y}, {"z", value.z}});
}

JVal errorJson(const AgentErrorV1& value) {
  JVal result;
  result["code"] = value.code;
  result["message"] = value.message;
  result["retryable"] = value.retryable;
  if (value.remediation) result["remediation"] = *value.remediation;
  if (value.field) result["field"] = *value.field;
  if (value.required_scope) result["requiredScope"] = *value.required_scope;
  return result;
}

JVal capabilitiesJson(const PlayerHostCapabilitiesV1& value) {
  JVal result;
  result["contractVersion"] = value.contract_version;
  result["gameId"] = value.game_id;
  result["revision"] = value.revision;
  result["controlledEntityId"] = value.controlled_entity_id;
  JArray commands;
  for (const auto& command : value.commands) {
    JVal item;
    item["kind"] = toString(command.kind);
    item["toolName"] = command.tool_name;
    if (command.required_scope) {
      item["requiredScope"] = toString(*command.required_scope);
    }
    item["risk"] = gen::toString(command.risk);
    item["approval"] = approvalName(command.approval);
    item["rateLimitPerSecond"] =
        static_cast<std::int64_t>(command.rate_limit_per_second);
    commands.emplace_back(std::move(item));
  }
  result["commands"] = std::move(commands);
  result["observation"]["maxAgeMs"] =
      static_cast<std::int64_t>(value.observation.max_age_ms);
  result["observation"]["maxNearbyActors"] =
      static_cast<std::int64_t>(value.observation.max_nearby_actors);
  result["observation"]["maxNearbyVoxels"] =
      static_cast<std::int64_t>(value.observation.max_nearby_voxels);
  result["advertisedAt"] = value.advertised_at;
  return result;
}

JVal observationJson(const GameObservationV1& value) {
  JVal result;
  result["contractVersion"] = value.contract_version;
  result["observationId"] = value.observation_id;
  result["capabilityRevision"] = value.capability_revision;
  result["controlledEntityId"] = value.controlled_entity_id;
  result["observedAt"] = value.observed_at;
  result["expiresAt"] = value.expires_at;
  result["player"]["position"] = vector3(value.player.position);
  result["player"]["velocity"] = vector3(value.player.velocity);
  result["player"]["look"]["yaw"] = value.player.look.yaw;
  result["player"]["look"]["pitch"] = value.player.look.pitch;
  result["player"]["health"] = value.player.health;
  result["player"]["alive"] = value.player.alive;
  result["controlledEntity"]["kind"] =
      controlledKindName(value.controlled_entity.kind);
  result["controlledEntity"]["position"] =
      vector3(value.controlled_entity.position);
  result["controlledEntity"]["velocity"] =
      vector3(value.controlled_entity.velocity);
  if (value.target) {
    result["target"]["targetId"] = value.target->target_id;
    result["target"]["kind"] = targetKindName(value.target->kind);
    result["target"]["distance"] = value.target->distance;
  }
  if (value.inventory) {
    result["inventory"]["selectedSlot"] =
        static_cast<std::int64_t>(value.inventory->selected_slot);
    JArray slots;
    for (const auto& slot : value.inventory->slots) {
      JVal item;
      item["slot"] = static_cast<std::int64_t>(slot.slot);
      item["itemId"] = slot.item_id;
      item["quantity"] = static_cast<std::int64_t>(slot.quantity);
      item["usable"] = slot.usable;
      slots.emplace_back(std::move(item));
    }
    result["inventory"]["slots"] = std::move(slots);
    JArray recipes;
    for (const auto& recipe : value.inventory->craftable_recipe_ids) {
      recipes.emplace_back(recipe);
    }
    result["inventory"]["craftableRecipeIds"] = std::move(recipes);
  }
  if (value.grid) {
    result["grid"]["gridRef"] = value.grid->grid_ref;
    result["grid"]["low"] = vector3(value.grid->low);
    result["grid"]["high"] = vector3(value.grid->high);
    JArray scopes;
    for (const auto scope : value.grid->effective_scopes) {
      scopes.emplace_back(toString(scope));
    }
    result["grid"]["effectiveScopes"] = std::move(scopes);
  }
  JArray actors;
  for (const auto& actor : value.nearby_actors) {
    JVal item;
    item["actorId"] = actor.actor_id;
    item["kind"] = actorKindName(actor.kind);
    item["position"] = vector3(actor.position);
    item["distance"] = actor.distance;
    item["disposition"] = dispositionName(actor.disposition);
    if (actor.label) item["label"] = *actor.label;
    if (actor.health) item["health"] = *actor.health;
    actors.emplace_back(std::move(item));
  }
  result["nearbyActors"] = std::move(actors);
  JArray voxels;
  for (const auto& voxel : value.nearby_voxels) {
    JVal item;
    item["position"] = vector3(voxel.position);
    item["material"] = voxel.material;
    item["interaction"] = interactionName(voxel.interaction);
    voxels.emplace_back(std::move(item));
  }
  result["nearbyVoxels"] = std::move(voxels);
  result["inputState"]["modalOpen"] = value.input_state.modal_open;
  result["inputState"]["textInputFocused"] =
      value.input_state.text_input_focused;
  result["inputState"]["humanInputActive"] =
      value.input_state.human_input_active;
  return result;
}

JVal commandResultJson(const GameCommandResultV1& value) {
  JVal result;
  result["contractVersion"] = value.contract_version;
  result["status"] = commandStatusName(value.status);
  result["commandKind"] = toString(value.command_kind);
  if (value.observation_id) result["observationId"] = *value.observation_id;
  if (!value.details.empty()) {
    JArray details;
    for (const auto& detail : value.details) {
      details.emplace_back(
          JVal::object({{"name", detail.name}, {"value", detail.value}}));
    }
    result["details"] = std::move(details);
  }
  if (value.error) result["error"] = errorJson(*value.error);
  return result;
}

std::string_view targetName(StudioTargetV1 value) {
  return value == StudioTargetV1::Server ? "SERVER" : "CLIENT";
}

std::string_view sourceName(StudioFileSourceV1 value) {
  switch (value) {
    case StudioFileSourceV1::Project:
      return "PROJECT";
    case StudioFileSourceV1::PersonalLibrary:
      return "PERSONAL_LIBRARY";
    case StudioFileSourceV1::Common:
      return "COMMON";
  }
  return "";
}

std::string_view pairingName(StudioPairingPreferenceV1 value) {
  switch (value) {
    case StudioPairingPreferenceV1::None:
      return "NONE";
    case StudioPairingPreferenceV1::Optional:
      return "OPTIONAL";
    case StudioPairingPreferenceV1::Required:
      return "REQUIRED";
  }
  return "";
}

std::string_view saveStateName(StudioSaveStateV1 value) {
  switch (value) {
    case StudioSaveStateV1::Saving:
      return "SAVING";
    case StudioSaveStateV1::Saved:
      return "SAVED";
    case StudioSaveStateV1::Conflict:
      return "CONFLICT";
    case StudioSaveStateV1::Offline:
      return "OFFLINE";
  }
  return "";
}

std::string_view projectKindName(StudioProjectKindV1 value) {
  switch (value) {
    case StudioProjectKindV1::Server:
      return "SERVER";
    case StudioProjectKindV1::Client:
      return "CLIENT";
    case StudioProjectKindV1::FullStack:
      return "FULL_STACK";
  }
  return "";
}

std::string_view runtimePhaseName(StudioRuntimePhaseV1 value) {
  switch (value) {
    case StudioRuntimePhaseV1::Idle:
      return "IDLE";
    case StudioRuntimePhaseV1::TestingDraft:
      return "TESTING_DRAFT";
    case StudioRuntimePhaseV1::DeployingLive:
      return "DEPLOYING_LIVE";
    case StudioRuntimePhaseV1::Compiling:
      return "COMPILING";
    case StudioRuntimePhaseV1::Enabling:
      return "ENABLING";
    case StudioRuntimePhaseV1::Running:
      return "RUNNING";
    case StudioRuntimePhaseV1::CompileFailed:
      return "COMPILE_FAILED";
    case StudioRuntimePhaseV1::Stopping:
      return "STOPPING";
    case StudioRuntimePhaseV1::Stopped:
      return "STOPPED";
    case StudioRuntimePhaseV1::PartialFailure:
      return "PARTIAL_FAILURE";
    case StudioRuntimePhaseV1::Error:
      return "ERROR";
  }
  return "";
}

std::string_view runtimeSyncName(StudioRuntimeSyncV1 value) {
  switch (value) {
    case StudioRuntimeSyncV1::NeverRun:
      return "NEVER_RUN";
    case StudioRuntimeSyncV1::RunningSaved:
      return "RUNNING_SAVED";
    case StudioRuntimeSyncV1::RunningStale:
      return "RUNNING_STALE";
    case StudioRuntimeSyncV1::Stopped:
      return "STOPPED";
  }
  return "";
}

JVal runtimeJson(const StudioRuntimeStatusV1& value) {
  JVal result;
  result["phase"] = runtimePhaseName(value.phase);
  result["savedRevision"] = value.saved_revision;
  if (value.running_revision) {
    result["runningRevision"] = *value.running_revision;
  }
  result["sync"] = runtimeSyncName(value.sync);
  if (value.target) result["target"] = targetName(*value.target);
  if (value.draft) result["draft"] = *value.draft;
  if (value.message) result["message"] = *value.message;
  return result;
}

JVal projectJson(const StudioProjectProjectionV1& value) {
  JVal result;
  result["projectId"] = value.project_id;
  result["name"] = value.name;
  if (value.description) result["description"] = *value.description;
  result["kind"] = projectKindName(value.kind);
  result["revision"] = value.revision;
  JArray files;
  for (const auto& file : value.files) {
    JVal item;
    item["target"] = targetName(file.target);
    item["path"] = file.path;
    item["contentHash"] = file.content_hash;
    item["byteLength"] = static_cast<std::int64_t>(file.byte_length);
    files.emplace_back(std::move(item));
  }
  result["files"] = std::move(files);
  if (value.server_module_name) {
    result["serverModuleName"] = *value.server_module_name;
  }
  if (value.client_module_name) {
    result["clientModuleName"] = *value.client_module_name;
  }
  result["pairingPreference"] = pairingName(value.pairing_preference);
  result["updatedAt"] = value.updated_at;
  return result;
}

JVal studioOutputJson(std::string_view name,
                      const NativeToolOutputV1& output) {
  if (name == "studio.context.get") {
    const auto& value = std::get<StudioContextV1>(output);
    JVal result;
    result["appRef"] = value.app_ref;
    if (value.project_ref) result["projectRef"] = *value.project_ref;
    result["gridRef"] = value.grid_ref;
    result["contextVersion"] = value.context_version;
    result["saveState"] = saveStateName(value.save_state);
    result["runtime"] = runtimeJson(value.runtime);
    if (value.client_epoch) result["clientEpoch"] = *value.client_epoch;
    JArray kinds;
    for (const auto kind : value.lease_kinds) {
      kinds.emplace_back(kind == LeaseKindV1::Workspace ? "WORKSPACE"
                                                        : "PLAY");
    }
    result["leaseKinds"] = std::move(kinds);
    if (value.host_capability_revision) {
      result["hostCapabilityRevision"] =
          *value.host_capability_revision;
    }
    return result;
  }
  if (name == "studio.state.get") {
    const auto& value = std::get<StudioStateV1>(output);
    JVal result;
    if (value.project) result["project"] = projectJson(*value.project);
    JArray files;
    for (const auto& file : value.open_files) {
      JVal item;
      item["source"] = sourceName(file.source);
      item["target"] = targetName(file.target);
      item["path"] = file.path;
      files.emplace_back(std::move(item));
    }
    result["openFiles"] = std::move(files);
    result["saveState"] = saveStateName(value.save_state);
    result["runtime"] = runtimeJson(value.runtime);
    return result;
  }
  if (name == "project.select") {
    const auto& value = std::get<StudioProjectSelectResultV1>(output);
    return JVal::object({{"selectedProjectRef", value.selected_project_ref},
                         {"revision", value.revision}});
  }
  if (name == "workspace.tab.open" || name == "workspace.tab.close") {
    return JVal::object({{"ok", std::get<StudioOkV1>(output).ok}});
  }
  if (name == "diagnostics.local.get") {
    const auto& value = std::get<StudioDiagnosticsV1>(output);
    JVal result;
    JArray diagnostics;
    for (const auto& diagnostic : value.diagnostics) {
      JVal item;
      item["source"] =
          diagnostic.source == StudioDiagnosticSourceV1::LocalAdvisory
              ? "LOCAL_ADVISORY"
              : diagnostic.source == StudioDiagnosticSourceV1::Rustc
                    ? "RUSTC"
                    : "RUNTIME";
      item["target"] = targetName(diagnostic.target);
      item["path"] = diagnostic.path;
      item["line"] = static_cast<std::int64_t>(diagnostic.line);
      item["column"] = static_cast<std::int64_t>(diagnostic.column);
      switch (diagnostic.severity) {
        case StudioDiagnosticSeverityV1::Error:
          item["severity"] = "ERROR";
          break;
        case StudioDiagnosticSeverityV1::Warning:
          item["severity"] = "WARNING";
          break;
        case StudioDiagnosticSeverityV1::Info:
          item["severity"] = "INFO";
          break;
        case StudioDiagnosticSeverityV1::Hint:
          item["severity"] = "HINT";
          break;
      }
      if (diagnostic.code) item["code"] = *diagnostic.code;
      item["message"] = diagnostic.message;
      diagnostics.emplace_back(std::move(item));
    }
    result["diagnostics"] = std::move(diagnostics);
    return result;
  }
  if (name == "runtime.status.get") {
    return runtimeJson(std::get<StudioRuntimeStatusV1>(output));
  }
  if (name == "runtime.test_draft" ||
      name == "runtime.deploy_live") {
    const auto& value = std::get<StudioRuntimePlanResultV1>(output);
    JVal result;
    result["runtime"] = runtimeJson(value.runtime);
    JArray selected;
    for (const auto target : value.targets) {
      selected.emplace_back(targetName(target));
    }
    result[name == "runtime.test_draft" ? "compiledTargets"
                                         : "deployedTargets"] =
        std::move(selected);
    return result;
  }
  if (name == "runtime.invoke") {
    const auto& value = std::get<StudioRuntimeInvokeResultV1>(output);
    JVal result;
    switch (value.result_type) {
      case StudioRuntimeResultTypeV1::Empty:
        result["resultType"] = "EMPTY";
        break;
      case StudioRuntimeResultTypeV1::Text:
        result["resultType"] = "TEXT";
        break;
      case StudioRuntimeResultTypeV1::Base64:
        result["resultType"] = "BASE64";
        break;
    }
    result["result"] = value.result;
    result["fuelUsed"] = value.fuel_used;
    result["durationUs"] =
        static_cast<std::int64_t>(value.duration_us);
    return result;
  }
  const auto& value = std::get<StudioRuntimeStopResultV1>(output);
  JVal result;
  result["serverStopped"] = value.server_stopped;
  result["clientStopped"] = value.client_stopped;
  JArray failures;
  for (const auto& failure : value.failures) {
    failures.emplace_back(failure);
  }
  result["failures"] = std::move(failures);
  return result;
}

JVal outputJson(std::string_view name, const NativeToolOutputV1& output) {
  if (name == "game.capabilities.get") {
    return capabilitiesJson(
        std::get<PlayerHostCapabilitiesV1>(output));
  }
  if (name == "game.observe") {
    return observationJson(std::get<GameObservationV1>(output));
  }
  if (name.rfind("game.", 0) == 0) {
    return commandResultJson(std::get<GameCommandResultV1>(output));
  }
  return studioOutputJson(name, output);
}

AgentError publicError(const AgentErrorV1& source) {
  AgentError result;
  result.code = source.code;
  result.message = source.message;
  result.retryable = source.retryable;
  result.remediation = source.remediation;
  result.field = source.field;
  result.requiredScope = source.required_scope;
  return result;
}

AgentToolResultStatus publicStatus(NativeToolResultStatusV1 value) {
  switch (value) {
    case NativeToolResultStatusV1::Succeeded:
      return AgentToolResultStatus::Succeeded;
    case NativeToolResultStatusV1::Failed:
      return AgentToolResultStatus::Failed;
    case NativeToolResultStatusV1::Cancelled:
      return AgentToolResultStatus::Cancelled;
    case NativeToolResultStatusV1::TimedOut:
      return AgentToolResultStatus::TimedOut;
    case NativeToolResultStatusV1::OutcomeUnknown:
      return AgentToolResultStatus::OutcomeUnknown;
  }
  return AgentToolResultStatus::Failed;
}

bool outcomeMayBeUnknown(std::string_view name, std::string_view version) {
  const auto contracts = nativeLocalToolContractsV1();
  const auto found = std::find_if(
      contracts.begin(), contracts.end(), [&](const auto& contract) {
        return contract.name == name && contract.version == version;
      });
  if (found == contracts.end() || !found->effectful) return false;
  if (name.rfind("game.", 0) == 0) return true;
  return name == "runtime.test_draft" ||
         name == "runtime.deploy_live" ||
         name == "runtime.invoke";
}

AgentToolResult publicResult(const NativeToolResultV1& source,
                             std::string_view name,
                             std::string_view version,
                             const AgentToolRegistry& registry) {
  AgentToolResult result;
  result.protocolVersion = source.protocol_version;
  result.toolCallId = source.tool_call_id;
  result.status = publicStatus(source.status);
  result.observedContextVersion = source.observed_context_version;
  result.startedAt = source.started_at;
  result.finishedAt = source.finished_at;
  if (source.error) result.error = publicError(*source.error);
  if (source.output) {
    const JVal encoded = outputJson(name, *source.output);
    const Json parsed = Json::parse(encoded.dump());
    if (!parsed.ok()) {
      throw CrowdyAgentError("AGENT_TOOL_OUTPUT_INVALID",
                             "Native output could not be encoded as JSON");
    }
    if (registry.get(name, version)) {
      registry.validateOutput(name, version, parsed);
    }
    result.outputJson = canonicalJson(parsed);
  }
  return result;
}

AgentToolResult conversionFailure(const AgentToolInvocation& invocation,
                                  const AgentError& error,
                                  const core::IClock& clock) {
  const auto now = isoTime(clock.epochMillis());
  AgentToolResult result;
  result.toolCallId = invocation.toolCallId;
  result.status = AgentToolResultStatus::Failed;
  result.error = error;
  result.observedContextVersion = invocation.contextVersion;
  result.startedAt = now;
  result.finishedAt = now;
  return result;
}

PreemptionReasonV1 nativePreemption(AgentPreemptionReason value) {
  const auto parsed = gen::crowdyStudioAgentPreemptionReasonFromString(
      toString(value));
  if (!parsed) {
    throw std::invalid_argument("unknown agent preemption reason");
  }
  return *parsed;
}

}  // namespace

NativeBrowserToolDispatcherAdapter::NativeBrowserToolDispatcherAdapter(
    NativeToolDispatcherV1& dispatcher,
    NativeBrowserToolDispatcherAdapterOptions options)
    : dispatcher_(dispatcher),
      registry_(options.registry ? *options.registry
                                 : canonicalAgentToolRegistryV1()),
      clock_(options.clock ? *options.clock : core::systemClock()) {}

void NativeBrowserToolDispatcherAdapter::dispatch(
    AgentToolInvocation invocation,
    AgentCallback<AgentToolResult> callback) {
  try {
    const std::string name = invocation.name;
    const std::string version = invocation.version;
    const bool ambiguous = outcomeMayBeUnknown(name, version);
    auto converted = nativeInvocation(invocation, registry_);
    dispatcher_.dispatch(
        std::move(converted),
        [callback = std::move(callback), name, version,
         ambiguous, registry = &registry_](NativeToolResultV1 result) mutable {
          try {
            callback(AgentOutcome<AgentToolResult>::success(
                publicResult(result, name, version, *registry)));
          } catch (const CrowdyAgentError& error) {
            const bool outcome_unknown =
                result.status == NativeToolResultStatusV1::OutcomeUnknown ||
                (ambiguous &&
                 result.status == NativeToolResultStatusV1::Succeeded);
            AgentToolResult failed;
            failed.toolCallId = result.tool_call_id;
            failed.status = outcome_unknown
                                ? AgentToolResultStatus::OutcomeUnknown
                                : AgentToolResultStatus::Failed;
            failed.error = error.value();
            if (outcome_unknown) failed.error->retryable = false;
            failed.observedContextVersion =
                result.observed_context_version;
            failed.startedAt = result.started_at;
            failed.finishedAt = result.finished_at;
            callback(AgentOutcome<AgentToolResult>::success(
                std::move(failed)));
          } catch (const std::exception& error) {
            const bool outcome_unknown =
                result.status == NativeToolResultStatusV1::OutcomeUnknown ||
                (ambiguous &&
                 result.status == NativeToolResultStatusV1::Succeeded);
            AgentToolResult failed;
            failed.toolCallId = result.tool_call_id;
            failed.status = outcome_unknown
                                ? AgentToolResultStatus::OutcomeUnknown
                                : AgentToolResultStatus::Failed;
            failed.error =
                toAgentError(error, "AGENT_TOOL_OUTPUT_INVALID");
            if (outcome_unknown) failed.error->retryable = false;
            failed.observedContextVersion =
                result.observed_context_version;
            failed.startedAt = result.started_at;
            failed.finishedAt = result.finished_at;
            callback(AgentOutcome<AgentToolResult>::success(
                std::move(failed)));
          }
        });
  } catch (const CrowdyAgentError& error) {
    callback(AgentOutcome<AgentToolResult>::success(
        conversionFailure(invocation, error.value(), clock_)));
  } catch (const std::exception& error) {
    callback(AgentOutcome<AgentToolResult>::success(conversionFailure(
        invocation, toAgentError(error, "AGENT_TOOL_INPUT_INVALID"),
        clock_)));
  }
}

void NativeBrowserToolDispatcherAdapter::cancelActive(
    AgentPreemptionReason reason) {
  dispatcher_.cancelActive(nativePreemption(reason));
}

void NativeBrowserToolDispatcherAdapter::clearClosedSession() {
  dispatcher_.clearClosedSession();
}

void NativeBrowserToolDispatcherAdapter::tick() {
  dispatcher_.tick();
}

bool NativeBrowserToolDispatcherAdapter::has(
    std::string_view toolCallId) const {
  return dispatcher_.has(toolCallId);
}

}  // namespace crowdy::agent
