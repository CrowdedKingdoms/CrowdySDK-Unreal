#include "crowdy/agent/native_tool_dispatcher.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "crowdy/core/clock.hpp"
#include "crowdy/player_host/schemas.hpp"

namespace crowdy::agent {
namespace {

using player_host::AdapterResultV1;
using player_host::AgentErrorV1;
using player_host::CommandResultStatusV1;
using player_host::GameCommandResultV1;
using player_host::LeaseKindV1;
using player_host::PreemptionReasonV1;

constexpr std::array<NativeLocalToolContractV1, 25> kContracts = {
    NativeLocalToolContractV1{
        "diagnostics.local.get", "1.0.0",
        "sha256:42c8abffeb9c1425afc7aa4f26fe4a0d76fb19c5da32f4c8cf37c3fa8888b215",
        10'000, false},
    NativeLocalToolContractV1{
        "game.capabilities.get", "1.0.0",
        "sha256:8e992a9dd090e6be5a23362a50debd41b29ea093b872e9c2fc89b90ea2dfbfcd",
        10'000, false},
    NativeLocalToolContractV1{
        "game.chat.send", "1.0.0",
        "sha256:adcd38097ea3069b7fbc758dfa8b583156188b44716c252446e68205d5092c11",
        10'000, true},
    NativeLocalToolContractV1{
        "game.combat.attack", "1.0.0",
        "sha256:99715c6e8d23beb3072f87906180c99686dc514756edf354894815bc4565f7a1",
        10'000, true},
    NativeLocalToolContractV1{
        "game.control.look", "1.0.0",
        "sha256:1af89c37b98bda67921689427ea5c813a06b9236981ecb6b515e538106ae08c0",
        10'000, true},
    NativeLocalToolContractV1{
        "game.control.move", "1.0.0",
        "sha256:31f7df3ce376fff468d08f2547632514cf60c87ee7c261767cb6cb65646ec156",
        10'000, true},
    NativeLocalToolContractV1{
        "game.control.stop", "1.0.0",
        "sha256:26e6944657a405d4a3bb809fce576626631da32262bb77075cf40aca4c423c61",
        10'000, true},
    NativeLocalToolContractV1{
        "game.craft", "1.0.0",
        "sha256:f8317768ccba168abdfde8bd165ebafc68698455d8d88fe20eeee8c973e0c01a",
        10'000, true},
    NativeLocalToolContractV1{
        "game.interact", "1.0.0",
        "sha256:af0f048f9d3ccedbf163cc27c254f77eecf5a22648bd1b8f7957b574dfae2222",
        10'000, true},
    NativeLocalToolContractV1{
        "game.inventory.consume", "1.0.0",
        "sha256:349c7068a3434a659232b21655f270712239d4734601bdf1889f1bffc7cff4cd",
        10'000, true},
    NativeLocalToolContractV1{
        "game.inventory.select", "1.0.0",
        "sha256:6b605c7d2d6910b3ab584f49f36828bb387c22be1d53e990745af7a381511aba",
        10'000, true},
    NativeLocalToolContractV1{
        "game.inventory.transfer", "1.0.0",
        "sha256:fadc78b29f7eb73b86a02111f7eeeb19eca18409161520c290859884c6b3693a",
        10'000, true},
    NativeLocalToolContractV1{
        "game.mount", "1.0.0",
        "sha256:92134ae0df1660a922176458c1b4a67e12e13c89c3cfb074e36166f2770296dd",
        10'000, true},
    NativeLocalToolContractV1{
        "game.observe", "1.0.0",
        "sha256:77b0f7ea7ef4713e3cd5338b496abdc2d510e4f8120297c4e3560c7369967ed0",
        10'000, false},
    NativeLocalToolContractV1{
        "game.travel.teleport", "1.0.0",
        "sha256:ea6761342b7ff729ef0c7a3481cbcc2cabbe96b5a1d6e06133118b2f1402c7fa",
        10'000, true},
    NativeLocalToolContractV1{
        "project.select", "1.0.0",
        "sha256:a6a2a93826e7bc5d532bed095a1244fde4ca9e29b5d0822c6774f0b2ac4dc8fd",
        10'000, true},
    NativeLocalToolContractV1{
        "runtime.deploy_live", "1.0.0",
        "sha256:83c3f6a8bec8667d5b25c5b5b7c679bda56fda3ccddeae09d77cd43facc1643a",
        120'000, true},
    NativeLocalToolContractV1{
        "runtime.invoke", "1.0.0",
        "sha256:9a28aea0feff18ceb60813edd94d7aad58a57494fe8b6ca074896f7bba2428b6",
        10'000, true},
    NativeLocalToolContractV1{
        "runtime.status.get", "1.0.0",
        "sha256:fb43970c015294a38530cfee63271451819525f687d8d382e35ae2e4f4d81603",
        10'000, false},
    NativeLocalToolContractV1{
        "runtime.stop", "1.0.0",
        "sha256:ddf23da04e1beddc43c2f8e3cc3b4908edda8664f99d78095f4abed75425e8f9",
        10'000, true},
    NativeLocalToolContractV1{
        "runtime.test_draft", "1.0.0",
        "sha256:d75e929b08768d938dadbaa382a138416ecde5f5e8c8b0c58f2bda3d2b608810",
        120'000, true},
    NativeLocalToolContractV1{
        "studio.context.get", "1.0.0",
        "sha256:0fd5eca473e52101b53be36aaa9694e16840a6982b3f672d172b16809e881b69",
        10'000, false},
    NativeLocalToolContractV1{
        "studio.state.get", "1.0.0",
        "sha256:e478829bb9c6351174eebc26ee77c0e2e227d12d13bcc4fc14263042f15efa72",
        10'000, false},
    NativeLocalToolContractV1{
        "workspace.tab.close", "1.0.0",
        "sha256:95f3aa6bac54557cab503a38ba77aeb4d1f22ad2fecee51d5421e820f465402d",
        10'000, true},
    NativeLocalToolContractV1{
        "workspace.tab.open", "1.0.0",
        "sha256:1674a45271bb1e998a561d330e6b2fe9b64d0d5ef8b88e073a3d6119c0c01a28",
        10'000, true},
};

const NativeLocalToolContractV1* contract(std::string_view name,
                                          std::string_view version) {
  const auto found =
      std::find_if(kContracts.begin(), kContracts.end(), [&](const auto& value) {
        return value.name == name && value.version == version;
      });
  return found == kContracts.end() ? nullptr : &*found;
}

AgentErrorV1 error(std::string code, std::string message,
                   bool retryable = false,
                   std::optional<std::string> field = std::nullopt,
                   std::optional<std::string> scope = std::nullopt) {
  AgentErrorV1 value;
  value.code = std::move(code);
  value.message = std::move(message);
  value.retryable = retryable;
  value.field = std::move(field);
  value.required_scope = std::move(scope);
  return value;
}

std::string isoTime(std::int64_t epoch_ms) {
  const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1'000);
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
                static_cast<long long>(epoch_ms % 1'000));
  return buffer;
}

bool bounded(std::string_view value, std::size_t maximum,
             std::size_t minimum = 1) {
  return value.size() >= minimum && value.size() <= maximum;
}

bool digest(std::string_view value) {
  if (value.size() != 71 || value.substr(0, 7) != "sha256:") return false;
  return std::all_of(value.begin() + 7, value.end(), [](char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
  });
}

bool decimal(std::string_view value) {
  if (value.empty() || value.size() > 40 ||
      (value.size() > 1 && value.front() == '0')) {
    return false;
  }
  return std::all_of(value.begin(), value.end(),
                     [](char ch) { return ch >= '0' && ch <= '9'; });
}

bool path(std::string_view value) {
  if (!bounded(value, 256) || value.front() == '/' ||
      value.find('\\') != std::string_view::npos ||
      std::any_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch <= 0x1fU || ch == 0x7fU;
      })) {
    return false;
  }
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t slash = value.find('/', start);
    const auto part = value.substr(
        start, slash == std::string_view::npos ? value.size() - start
                                               : slash - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return true;
}

bool uniqueTargets(const std::vector<StudioTargetV1>& targets) {
  return targets.size() >= 1 && targets.size() <= 2 &&
         (targets.size() == 1 || targets[0] != targets[1]);
}

std::optional<StudioNativeToolKindV1> studioKind(std::string_view name) {
  if (name == "studio.context.get")
    return StudioNativeToolKindV1::ContextGet;
  if (name == "studio.state.get") return StudioNativeToolKindV1::StateGet;
  if (name == "project.select") return StudioNativeToolKindV1::ProjectSelect;
  if (name == "workspace.tab.open")
    return StudioNativeToolKindV1::WorkspaceTabOpen;
  if (name == "workspace.tab.close")
    return StudioNativeToolKindV1::WorkspaceTabClose;
  if (name == "diagnostics.local.get")
    return StudioNativeToolKindV1::DiagnosticsLocalGet;
  if (name == "runtime.status.get")
    return StudioNativeToolKindV1::RuntimeStatusGet;
  if (name == "runtime.test_draft")
    return StudioNativeToolKindV1::RuntimeTestDraft;
  if (name == "runtime.deploy_live")
    return StudioNativeToolKindV1::RuntimeDeployLive;
  if (name == "runtime.invoke") return StudioNativeToolKindV1::RuntimeInvoke;
  if (name == "runtime.stop") return StudioNativeToolKindV1::RuntimeStop;
  return std::nullopt;
}

bool modeAllowed(std::string_view name, NativeAgentModeV1 mode) {
  if (name == "studio.context.get") return true;
  if (name == "game.capabilities.get" || name == "game.observe") {
    return mode == NativeAgentModeV1::Ask || mode == NativeAgentModeV1::Play;
  }
  if (name.rfind("game.", 0) == 0) return mode == NativeAgentModeV1::Play;
  if (name == "runtime.invoke") {
    return mode == NativeAgentModeV1::Build || mode == NativeAgentModeV1::Play;
  }
  if (name == "runtime.test_draft" || name == "runtime.deploy_live" ||
      name == "runtime.stop") {
    return mode == NativeAgentModeV1::Build;
  }
  return mode == NativeAgentModeV1::Ask || mode == NativeAgentModeV1::Build;
}

std::optional<AgentErrorV1> validateInput(
    const NativeToolInvocationV1& invocation) {
  if (invocation.name == "game.capabilities.get") {
    if (!std::holds_alternative<NoArgumentsV1>(invocation.arguments)) {
      return error("AGENT_TOOL_INPUT_INVALID",
                   "capability input must be empty");
    }
    return std::nullopt;
  }
  if (invocation.name == "game.observe") {
    const auto* request =
        std::get_if<player_host::ObserveRequestV1>(&invocation.arguments);
    if (!request) {
      return error("AGENT_TOOL_INPUT_INVALID",
                   "game.observe requires ObserveRequestV1");
    }
    const auto valid = player_host::validateObserveRequestV1(*request);
    if (!valid) {
      return error("AGENT_TOOL_INPUT_INVALID", valid.issue()->message, false,
                   valid.issue()->field);
    }
    return std::nullopt;
  }
  if (invocation.name.rfind("game.", 0) == 0) {
    const auto* command =
        std::get_if<player_host::GameCommandV1>(&invocation.arguments);
    if (!command ||
        player_host::toolName(player_host::commandKind(*command)) !=
            invocation.name) {
      return error("AGENT_TOOL_INPUT_INVALID",
                   "game tool does not match its typed command");
    }
    const auto valid = player_host::validateGameCommandV1(*command);
    if (!valid) {
      return error("AGENT_TOOL_INPUT_INVALID", valid.issue()->message, false,
                   valid.issue()->field);
    }
    return std::nullopt;
  }

  const auto kind = studioKind(invocation.name);
  if (!kind) {
    return error("AGENT_TOOL_UNKNOWN",
                 "tool is not a native local executor surface");
  }
  switch (*kind) {
    case StudioNativeToolKindV1::ContextGet:
    case StudioNativeToolKindV1::StateGet:
    case StudioNativeToolKindV1::DiagnosticsLocalGet:
    case StudioNativeToolKindV1::RuntimeStatusGet:
    case StudioNativeToolKindV1::RuntimeStop:
      if (!std::holds_alternative<NoArgumentsV1>(invocation.arguments)) {
        return error("AGENT_TOOL_INPUT_INVALID",
                     "read/stop tool input must be empty");
      }
      break;
    case StudioNativeToolKindV1::ProjectSelect: {
      const auto* request =
          std::get_if<StudioProjectSelectRequestV1>(&invocation.arguments);
      if (!request || !bounded(request->project_ref, 128)) {
        return error("AGENT_TOOL_INPUT_INVALID",
                     "project.select requires a bounded project reference");
      }
      break;
    }
    case StudioNativeToolKindV1::WorkspaceTabOpen:
    case StudioNativeToolKindV1::WorkspaceTabClose: {
      const auto* request =
          std::get_if<StudioFileTabRequestV1>(&invocation.arguments);
      if (!request || !path(request->path) ||
          (request->reference_ref &&
           !bounded(*request->reference_ref, 128))) {
        return error("AGENT_TOOL_INPUT_INVALID",
                     "workspace tab reference is outside contract bounds");
      }
      break;
    }
    case StudioNativeToolKindV1::RuntimeTestDraft: {
      const auto* request =
          std::get_if<StudioRuntimeTestDraftRequestV1>(&invocation.arguments);
      if (!request || !decimal(request->expected_revision) ||
          !uniqueTargets(request->targets)) {
        return error("AGENT_TOOL_INPUT_INVALID",
                     "draft plan revision or target set is invalid");
      }
      break;
    }
    case StudioNativeToolKindV1::RuntimeDeployLive: {
      const auto* request = std::get_if<StudioRuntimeDeployLiveRequestV1>(
          &invocation.arguments);
      if (!request || !decimal(request->expected_revision) ||
          !digest(request->project_content_hash) ||
          !uniqueTargets(request->targets) || request->draft) {
        return error("AGENT_TOOL_INPUT_INVALID",
                     "live deployment plan is not exact and bounded");
      }
      break;
    }
    case StudioNativeToolKindV1::RuntimeInvoke: {
      const auto* request =
          std::get_if<StudioRuntimeInvokeRequestV1>(&invocation.arguments);
      if (!request || !bounded(request->export_name, 120) ||
          request->params.size() > 32) {
        return error("AGENT_TOOL_INPUT_INVALID",
                     "runtime invocation is outside contract bounds");
      }
      for (const auto& parameter : request->params) {
        if (!bounded(parameter.name, 64) ||
            !bounded(parameter.value, 1'024, 0) ||
            (parameter.type == StudioRuntimeParameterTypeV1::Boolean &&
             parameter.value != "true" && parameter.value != "false")) {
          return error("AGENT_TOOL_INPUT_INVALID",
                       "runtime parameter is invalid");
        }
        if (parameter.type == StudioRuntimeParameterTypeV1::Decimal &&
            !player_host::validateDecimalStringV1(parameter.value,
                                                  "params.value")) {
          return error("AGENT_TOOL_INPUT_INVALID",
                       "decimal runtime parameter is not canonical");
        }
      }
      break;
    }
  }
  return std::nullopt;
}

bool validRuntime(const StudioRuntimeStatusV1& value) {
  return decimal(value.saved_revision) &&
         (!value.running_revision || decimal(*value.running_revision)) &&
         (!value.message || value.message->size() <= 512);
}

bool validProject(const StudioProjectProjectionV1& value) {
  if (!bounded(value.project_id, 128) || !bounded(value.name, 120) ||
      (value.description && value.description->size() > 1'024) ||
      !decimal(value.revision) || value.files.size() > 128 ||
      (value.server_module_name &&
       !bounded(*value.server_module_name, 120)) ||
      (value.client_module_name &&
       !bounded(*value.client_module_name, 120)) ||
      !bounded(value.updated_at, 40, 20) ||
      core::parseIso8601Millis(value.updated_at.data(),
                               value.updated_at.size()) <= 0) {
    return false;
  }
  return std::all_of(
      value.files.begin(), value.files.end(),
      [](const StudioFileSummaryV1& file) {
        return path(file.path) && digest(file.content_hash) &&
               file.byte_length <= 1'048'576;
      });
}

bool validOutput(std::string_view name, const NativeToolOutputV1& output) {
  if (name == "game.capabilities.get") {
    const auto* value =
        std::get_if<player_host::PlayerHostCapabilitiesV1>(&output);
    return value && player_host::validatePlayerHostCapabilitiesV1(*value);
  }
  if (name == "game.observe") {
    const auto* value = std::get_if<player_host::GameObservationV1>(&output);
    return value && player_host::validateGameObservationV1(*value);
  }
  if (name.rfind("game.", 0) == 0) {
    const auto* value =
        std::get_if<player_host::GameCommandResultV1>(&output);
    return value && player_host::validateGameCommandResultV1(*value);
  }
  if (name == "studio.context.get") {
    const auto* value = std::get_if<StudioContextV1>(&output);
    return value && bounded(value->app_ref, 128) &&
           bounded(value->grid_ref, 128) &&
           bounded(value->context_version, 128) &&
           (!value->project_ref || bounded(*value->project_ref, 128)) &&
           (!value->client_epoch || decimal(*value->client_epoch)) &&
           (!value->host_capability_revision ||
            bounded(*value->host_capability_revision, 128)) &&
           value->lease_kinds.size() <= 2 &&
           (value->lease_kinds.size() < 2 ||
            value->lease_kinds[0] != value->lease_kinds[1]) &&
           validRuntime(value->runtime);
  }
  if (name == "studio.state.get") {
    const auto* value = std::get_if<StudioStateV1>(&output);
    return value && value->open_files.size() <= 32 &&
           (!value->project || validProject(*value->project)) &&
           std::all_of(
               value->open_files.begin(), value->open_files.end(),
               [](const StudioOpenFileV1& file) { return path(file.path); }) &&
           validRuntime(value->runtime);
  }
  if (name == "project.select") {
    const auto* value = std::get_if<StudioProjectSelectResultV1>(&output);
    return value && bounded(value->selected_project_ref, 128) &&
           decimal(value->revision);
  }
  if (name == "workspace.tab.open" || name == "workspace.tab.close") {
    const auto* value = std::get_if<StudioOkV1>(&output);
    return value != nullptr;
  }
  if (name == "diagnostics.local.get") {
    const auto* value = std::get_if<StudioDiagnosticsV1>(&output);
    if (!value || value->diagnostics.size() > 256) return false;
    return std::all_of(
        value->diagnostics.begin(), value->diagnostics.end(),
        [](const StudioDiagnosticV1& diagnostic) {
          return path(diagnostic.path) && diagnostic.line >= 1 &&
                 diagnostic.line <= 1'000'000 && diagnostic.column >= 1 &&
                 diagnostic.column <= 1'000'000 &&
                 (!diagnostic.code || diagnostic.code->size() <= 64) &&
                 bounded(diagnostic.message, 2'048);
        });
  }
  if (name == "runtime.status.get") {
    const auto* value = std::get_if<StudioRuntimeStatusV1>(&output);
    return value && validRuntime(*value);
  }
  if (name == "runtime.test_draft" || name == "runtime.deploy_live") {
    const auto* value = std::get_if<StudioRuntimePlanResultV1>(&output);
    return value && validRuntime(value->runtime) &&
           uniqueTargets(value->targets) &&
           (!value->runtime.draft ||
            *value->runtime.draft == (name == "runtime.test_draft"));
  }
  if (name == "runtime.invoke") {
    const auto* value = std::get_if<StudioRuntimeInvokeResultV1>(&output);
    return value && value->result.size() <= 16'384 &&
           decimal(value->fuel_used) &&
           value->duration_us <= 2'147'483'647U;
  }
  if (name == "runtime.stop") {
    const auto* value = std::get_if<StudioRuntimeStopResultV1>(&output);
    return value && value->failures.size() <= 2 &&
           std::all_of(value->failures.begin(), value->failures.end(),
                       [](const std::string& failure) {
                         return bounded(failure, 512);
                       });
  }
  return false;
}

}  // namespace

std::span<const NativeLocalToolContractV1>
nativeLocalToolContractsV1() noexcept {
  return kContracts;
}

struct NativeToolDispatcherV1::Impl
    : std::enable_shared_from_this<NativeToolDispatcherV1::Impl> {
  struct Record {
    NativeToolInvocationV1 invocation;
    std::vector<NativeToolCompletionV1> completions;
    std::optional<NativeToolResultV1> result;
    std::shared_ptr<player_host::CancellationSourceV1> cancellation;
    std::int64_t started_ms = 0;
    std::int64_t deadline_ms = 0;
    std::uint64_t generation = 0;
    bool active = false;
    bool cancel_requested = false;
  };

  player_host::AgentControlLeaseManager& lease_manager;
  CrowdyStudioHostAdapter* studio_adapter;
  NativeToolDispatcherOptionsV1 options;
  const core::IClock& clock;
  mutable std::mutex mutex;
  std::map<std::string, std::shared_ptr<Record>, std::less<>> records;
  std::uint64_t generation = 0;
  bool shutting_down = false;

  Impl(player_host::AgentControlLeaseManager& manager,
       CrowdyStudioHostAdapter* studio,
       NativeToolDispatcherOptionsV1 config)
      : lease_manager(manager),
        studio_adapter(studio),
        options(std::move(config)),
        clock(options.clock ? *options.clock : core::systemClock()) {
    if (options.max_remembered_calls == 0) {
      throw std::invalid_argument(
          "native tool execute-once cache bound must be non-zero");
    }
  }

  std::string contextVersion() const {
    return options.context_version ? options.context_version() : std::string{};
  }

  NativeAgentModeV1 mode() const {
    return options.mode ? options.mode() : NativeAgentModeV1::Ask;
  }

  NativeToolResultV1 resultBase(const NativeToolInvocationV1& invocation,
                                NativeToolResultStatusV1 status,
                                std::int64_t started_ms) const {
    NativeToolResultV1 result;
    result.tool_call_id = invocation.tool_call_id;
    result.status = status;
    try {
      result.observed_context_version = contextVersion();
    } catch (...) {
      result.observed_context_version = invocation.context_version;
    }
    result.started_at = isoTime(started_ms);
    result.finished_at = isoTime(clock.epochMillis());
    return result;
  }

  NativeToolResultV1 failure(const NativeToolInvocationV1& invocation,
                             AgentErrorV1 reason,
                             NativeToolResultStatusV1 status =
                                 NativeToolResultStatusV1::Failed,
                             std::int64_t started_ms = 0) const {
    if (status == NativeToolResultStatusV1::OutcomeUnknown) {
      reason.retryable = false;
    }
    auto result = resultBase(
        invocation, status,
        started_ms == 0 ? clock.epochMillis() : started_ms);
    result.error = std::move(reason);
    return result;
  }

  std::optional<AgentErrorV1> validateEnvelope(
      const NativeToolInvocationV1& invocation, std::int64_t now,
      const NativeLocalToolContractV1*& resolved) const {
    resolved = contract(invocation.name, invocation.version);
    if (!resolved) {
      const bool known_name = std::any_of(
          kContracts.begin(), kContracts.end(), [&](const auto& value) {
            return value.name == invocation.name;
          });
      return error(known_name ? "AGENT_TOOL_VERSION_UNSUPPORTED"
                              : "AGENT_TOOL_UNKNOWN",
                   known_name
                       ? "native tool version is unsupported"
                       : "tool is not an exact registered native local tool",
                   false, known_name ? std::optional<std::string>("version")
                                     : std::nullopt);
    }
    if (invocation.protocol_version != "crowdy.tool-call/1") {
      return error("AGENT_TOOL_INPUT_INVALID",
                   "unsupported native tool-call protocol");
    }
    for (const auto& [name, value, maximum] :
         std::array<std::tuple<std::string_view, std::string_view,
                               std::size_t>,
                    4>{
             std::tuple{"sessionId", std::string_view(invocation.session_id),
                        128},
             std::tuple{"runId", std::string_view(invocation.run_id), 128},
             std::tuple{"toolCallId",
                        std::string_view(invocation.tool_call_id), 128},
             std::tuple{"contextVersion",
                        std::string_view(invocation.context_version), 128},
         }) {
      if (!bounded(value, maximum)) {
        return error("AGENT_TOOL_INPUT_INVALID",
                     std::string(name) + " is outside protocol bounds", false,
                     std::string(name));
      }
    }
    if (invocation.descriptor_digest != resolved->descriptor_digest) {
      return error("AGENT_CONTEXT_STALE",
                   "native tool descriptor digest changed");
    }
    if (!digest(invocation.argument_hash) ||
        (invocation.approval_grant &&
         !bounded(*invocation.approval_grant, 512)) ||
        (invocation.idempotency_key &&
         !bounded(*invocation.idempotency_key, 240))) {
      return error("AGENT_TOOL_INPUT_INVALID",
                   "invalid argument hash or capability metadata");
    }
    if (options.validate_argument_hash &&
        !options.validate_argument_hash(invocation)) {
      return error("AGENT_TOOL_INPUT_INVALID",
                   "argumentHash does not match the typed tool input");
    }

    const bool unconditional_stop =
        invocation.name == "game.control.stop";
    if (!unconditional_stop) {
      if (options.session_id) {
        const auto current = options.session_id();
        if (current && invocation.session_id != *current) {
          return error("AGENT_SESSION_NOT_FOUND",
                       "tool dispatch belongs to another session");
        }
      }
      const auto current_epoch =
          options.client_epoch ? options.client_epoch() : std::nullopt;
      if (!current_epoch || !invocation.client_epoch ||
          *current_epoch != *invocation.client_epoch) {
        return error("AGENT_CLIENT_EPOCH_STALE",
                     "tool dispatch belongs to a stale client epoch");
      }
      if (invocation.context_version != contextVersion()) {
        return error("AGENT_CONTEXT_STALE",
                     "tool dispatch belongs to a stale native context");
      }
      if (!modeAllowed(invocation.name, mode())) {
        return error("AGENT_SCOPE_DENIED",
                     "tool is unavailable in the selected agent mode");
      }
      const std::int64_t deadline = core::parseIso8601Millis(
          invocation.deadline.data(), invocation.deadline.size());
      if (!bounded(invocation.deadline, 40, 20) || deadline <= now) {
        return error("AGENT_TOOL_TIMEOUT",
                     "native tool deadline has expired");
      }
    }
    return validateInput(invocation);
  }

  bool accepting(const std::shared_ptr<Record>& record) const {
    {
      std::lock_guard lock(mutex);
      if (shutting_down || record->cancel_requested || record->result ||
          record->generation != generation) {
        return false;
      }
    }
    try {
      if (record->invocation.name == "game.control.stop") return true;
      if (record->invocation.context_version != contextVersion()) return false;
      if (options.client_epoch) {
        const auto current = options.client_epoch();
        if (!current || !record->invocation.client_epoch ||
            *current != *record->invocation.client_epoch) {
          return false;
        }
      }
      if (options.session_id) {
        const auto current = options.session_id();
        if (current && *current != record->invocation.session_id) return false;
      }
      return true;
    } catch (...) {
      return false;
    }
  }

  bool ambiguousOutcome(const std::shared_ptr<Record>& record) const {
    const auto* resolved =
        contract(record->invocation.name, record->invocation.version);
    if (!resolved || !resolved->effectful) return false;
    if (record->invocation.name.rfind("game.", 0) == 0) return true;
    return record->cancellation && record->cancellation->effectStarted();
  }

  bool acceptOrFence(const std::shared_ptr<Record>& record) {
    if (accepting(record)) return true;
    bool cancellation_in_progress = false;
    {
      std::lock_guard lock(mutex);
      cancellation_in_progress =
          record->cancel_requested || record->result.has_value() ||
          shutting_down;
    }
    if (!cancellation_in_progress) {
      const bool ambiguous = ambiguousOutcome(record);
      finish(record,
             failure(record->invocation,
                     error(ambiguous ? "AGENT_TOOL_OUTCOME_UNKNOWN"
                                     : "AGENT_CONTEXT_STALE",
                           ambiguous
                               ? "effectful native result was fenced after "
                                 "context change"
                               : "late native result was fenced after context "
                                 "change"),
                     ambiguous ? NativeToolResultStatusV1::OutcomeUnknown
                               : NativeToolResultStatusV1::Failed,
                     record->started_ms));
    }
    return false;
  }

  void finish(const std::shared_ptr<Record>& record, NativeToolResultV1 result) {
    std::vector<NativeToolCompletionV1> completions;
    {
      std::lock_guard lock(mutex);
      if (record->result) return;
      record->active = false;
      record->result = result;
      completions = std::move(record->completions);
    }
    for (auto& completion : completions) completion(result);
  }

  void completeAdapterFailure(
      const std::shared_ptr<Record>& record,
      const std::optional<AgentErrorV1>& adapter_error, bool ambiguous) {
    const bool outcome_unknown = ambiguous;
    finish(record,
           failure(record->invocation,
                   adapter_error.value_or(
                       error(outcome_unknown ? "AGENT_TOOL_OUTCOME_UNKNOWN"
                                             : "AGENT_TOOL_FAILED",
                             "native host operation failed")),
                   outcome_unknown
                       ? NativeToolResultStatusV1::OutcomeUnknown
                       : NativeToolResultStatusV1::Failed,
                   record->started_ms));
  }

  void dispatch(NativeToolInvocationV1 invocation,
                NativeToolCompletionV1 completion);
  void executeGame(const std::shared_ptr<Record>& record);
  void executeStudio(const std::shared_ptr<Record>& record);
  void tick();
  void cancelActive(PreemptionReasonV1 reason);
  void clearClosedSession();
};

void NativeToolDispatcherV1::Impl::dispatch(
    NativeToolInvocationV1 invocation, NativeToolCompletionV1 completion) {
  const std::int64_t now = clock.epochMillis();
  if (bounded(invocation.tool_call_id, 128)) {
    bool previous_conflict = false;
    bool joined_previous = false;
    std::optional<NativeToolResultV1> previous_result;
    {
      std::lock_guard lock(mutex);
      const auto found = records.find(invocation.tool_call_id);
      if (found != records.end()) {
        if (found->second->invocation != invocation) {
          previous_conflict = true;
        } else if (found->second->result) {
          previous_result = found->second->result;
        } else {
          found->second->completions.push_back(std::move(completion));
          joined_previous = true;
        }
      }
    }
    if (previous_conflict) {
      completion(failure(
          invocation,
          error("AGENT_IDEMPOTENCY_CONFLICT",
                "native tool call was replayed with different arguments"),
          NativeToolResultStatusV1::Failed, now));
      return;
    }
    if (previous_result) {
      completion(std::move(*previous_result));
      return;
    }
    if (joined_previous) return;
  }
  const NativeLocalToolContractV1* resolved = nullptr;
  std::optional<AgentErrorV1> validation;
  try {
    validation = validateEnvelope(invocation, now, resolved);
  } catch (const std::exception& failure) {
    validation = error("AGENT_TOOL_FAILED", failure.what());
  } catch (...) {
    validation = error(
        "AGENT_TOOL_FAILED",
        "native tool provider validation failed before execution");
  }
  if (validation) {
    completion(failure(
        invocation, *validation, NativeToolResultStatusV1::Failed, now));
    return;
  }

  std::shared_ptr<Record> record;
  std::optional<NativeToolResultV1> replay;
  bool conflict = false;
  bool cache_full = false;
  {
    std::lock_guard lock(mutex);
    const auto found = records.find(invocation.tool_call_id);
    if (found != records.end()) {
      record = found->second;
      if (record->invocation != invocation) {
        conflict = true;
      } else if (record->result) {
        replay = record->result;
      } else {
        record->completions.push_back(std::move(completion));
        return;
      }
    } else if (records.size() >= options.max_remembered_calls) {
      cache_full = true;
    } else {
      record = std::make_shared<Record>();
      record->invocation = std::move(invocation);
      record->completions.push_back(std::move(completion));
      record->cancellation =
          std::make_shared<player_host::CancellationSourceV1>();
      record->started_ms = now;
      const std::int64_t invocation_deadline = core::parseIso8601Millis(
          record->invocation.deadline.data(),
          record->invocation.deadline.size());
      record->deadline_ms =
          record->invocation.name == "game.control.stop"
              ? now + resolved->timeout_ms
              : std::min<std::int64_t>(
                    invocation_deadline,
                    now + static_cast<std::int64_t>(resolved->timeout_ms));
      record->generation = generation;
      record->active = true;
      records.emplace(record->invocation.tool_call_id, record);
    }
  }
  if (conflict) {
    completion(failure(
        invocation,
        error("AGENT_IDEMPOTENCY_CONFLICT",
              "native tool call was replayed with different arguments"),
        NativeToolResultStatusV1::Failed, now));
    return;
  }
  if (replay) {
    completion(std::move(*replay));
    return;
  }
  if (cache_full) {
    if (invocation.name == "game.control.stop") {
      lease_manager.preempt(PreemptionReasonV1::HUMAN_STOP);
      GameCommandResultV1 stopped;
      stopped.status = CommandResultStatusV1::Succeeded;
      stopped.command_kind = player_host::CommandKindV1::Stop;
      auto result = resultBase(invocation,
                               NativeToolResultStatusV1::Succeeded, now);
      result.output = NativeToolOutputV1{std::move(stopped)};
      completion(std::move(result));
      return;
    }
    completion(failure(
        invocation,
        error("AGENT_RATE_LIMITED",
              "native execute-once cache is full"),
        NativeToolResultStatusV1::Failed, now));
    return;
  }

  if (record->invocation.name.rfind("game.", 0) == 0) {
    executeGame(record);
  } else {
    executeStudio(record);
  }
}

void NativeToolDispatcherV1::Impl::executeGame(
    const std::shared_ptr<Record>& record) {
  const std::weak_ptr<Impl> weak = shared_from_this();
  if (record->invocation.name == "game.capabilities.get") {
    lease_manager.refreshCapabilities(
        [weak, record](
            AdapterResultV1<player_host::PlayerHostCapabilitiesV1> result) {
          const auto self = weak.lock();
          if (!self) return;
          if (!self->acceptOrFence(record)) return;
          if (!result.ok()) {
            self->completeAdapterFailure(record, result.error,
                                         result.outcome_unknown);
            return;
          }
          NativeToolOutputV1 output = std::move(*result.value);
          if (!validOutput(record->invocation.name, output)) {
            self->finish(
                record,
                self->failure(
                    record->invocation,
                    error("AGENT_TOOL_OUTPUT_INVALID",
                          "native capability output failed schema validation"),
                    NativeToolResultStatusV1::Failed, record->started_ms));
            return;
          }
          auto completed = self->resultBase(
              record->invocation, NativeToolResultStatusV1::Succeeded,
              record->started_ms);
          completed.output = std::move(output);
          self->finish(record, std::move(completed));
        });
    return;
  }
  if (record->invocation.name == "game.observe") {
    const auto& request =
        std::get<player_host::ObserveRequestV1>(record->invocation.arguments);
    player_host::AgentObservationDispatchV1 dispatch;
    dispatch.client_epoch = record->invocation.client_epoch.value_or("");
    dispatch.lease_id = record->invocation.lease_id;
    lease_manager.observe(
        request, dispatch,
        [weak, record](AdapterResultV1<player_host::GameObservationV1> result) {
          const auto self = weak.lock();
          if (!self) return;
          if (!self->acceptOrFence(record)) return;
          if (!result.ok()) {
            self->completeAdapterFailure(record, result.error,
                                         result.outcome_unknown);
            return;
          }
          NativeToolOutputV1 output = std::move(*result.value);
          if (!validOutput(record->invocation.name, output)) {
            self->finish(
                record,
                self->failure(
                    record->invocation,
                    error("AGENT_TOOL_OUTPUT_INVALID",
                          "native observation output failed schema validation"),
                    NativeToolResultStatusV1::Failed, record->started_ms));
            return;
          }
          auto completed = self->resultBase(
              record->invocation, NativeToolResultStatusV1::Succeeded,
              record->started_ms);
          completed.output = std::move(output);
          self->finish(record, std::move(completed));
        });
    return;
  }

  const auto& command =
      std::get<player_host::GameCommandV1>(record->invocation.arguments);
  player_host::AgentControlDispatchV1 dispatch;
  dispatch.tool_call_id = record->invocation.tool_call_id;
  dispatch.client_epoch = record->invocation.client_epoch.value_or("");
  dispatch.lease_id = record->invocation.lease_id;
  dispatch.approval_grant = record->invocation.approval_grant;
  dispatch.command = command;
  lease_manager.dispatch(
      std::move(dispatch),
      [weak, record](AdapterResultV1<GameCommandResultV1> result) {
        const auto self = weak.lock();
        if (!self) return;
        if (!self->acceptOrFence(record)) return;
        if (!result.ok()) {
          self->completeAdapterFailure(record, result.error,
                                       result.outcome_unknown);
          return;
        }
        if (result.value->status == CommandResultStatusV1::OutcomeUnknown) {
          self->finish(
              record,
              self->failure(
                  record->invocation,
                  result.value->error.value_or(
                      error("AGENT_TOOL_OUTCOME_UNKNOWN",
                            "game command outcome could not be confirmed")),
                  NativeToolResultStatusV1::OutcomeUnknown,
                  record->started_ms));
          return;
        }
        if (result.value->status != CommandResultStatusV1::Succeeded) {
          self->finish(
              record,
              self->failure(
                  record->invocation,
                  result.value->error.value_or(
                      error(result.value->status ==
                                    CommandResultStatusV1::Denied
                                ? "AGENT_SCOPE_DENIED"
                                : "AGENT_TOOL_FAILED",
                            "game command failed")),
                  NativeToolResultStatusV1::Failed, record->started_ms));
          return;
        }
        NativeToolOutputV1 output = std::move(*result.value);
        if (!validOutput(record->invocation.name, output)) {
          self->finish(
              record,
              self->failure(
                  record->invocation,
                  error("AGENT_TOOL_OUTPUT_INVALID",
                        "game command output failed schema validation"),
                  NativeToolResultStatusV1::OutcomeUnknown,
                  record->started_ms));
          return;
        }
        auto completed = self->resultBase(
            record->invocation, NativeToolResultStatusV1::Succeeded,
            record->started_ms);
        completed.output = std::move(output);
        self->finish(record, std::move(completed));
      });
}

void NativeToolDispatcherV1::Impl::executeStudio(
    const std::shared_ptr<Record>& record) {
  const auto kind = studioKind(record->invocation.name);
  if (!kind || !studio_adapter) {
    finish(record,
           failure(record->invocation,
                   error("AGENT_HOST_UNAVAILABLE",
                         "no native Studio host implements this tool"),
                   NativeToolResultStatusV1::Failed, record->started_ms));
    return;
  }

  StudioNativeToolRequestV1 request;
  switch (*kind) {
    case StudioNativeToolKindV1::ContextGet:
    case StudioNativeToolKindV1::StateGet:
    case StudioNativeToolKindV1::DiagnosticsLocalGet:
    case StudioNativeToolKindV1::RuntimeStatusGet:
    case StudioNativeToolKindV1::RuntimeStop:
      request = NoArgumentsV1{};
      break;
    case StudioNativeToolKindV1::ProjectSelect:
      request =
          std::get<StudioProjectSelectRequestV1>(record->invocation.arguments);
      break;
    case StudioNativeToolKindV1::WorkspaceTabOpen:
    case StudioNativeToolKindV1::WorkspaceTabClose:
      request =
          std::get<StudioFileTabRequestV1>(record->invocation.arguments);
      break;
    case StudioNativeToolKindV1::RuntimeTestDraft:
      request = std::get<StudioRuntimeTestDraftRequestV1>(
          record->invocation.arguments);
      break;
    case StudioNativeToolKindV1::RuntimeDeployLive:
      request = std::get<StudioRuntimeDeployLiveRequestV1>(
          record->invocation.arguments);
      break;
    case StudioNativeToolKindV1::RuntimeInvoke:
      request = std::get<StudioRuntimeInvokeRequestV1>(
          record->invocation.arguments);
      break;
  }

  std::optional<LeaseKindV1> required_lease;
  bool approval_required = false;
  if (*kind == StudioNativeToolKindV1::RuntimeTestDraft ||
      *kind == StudioNativeToolKindV1::RuntimeDeployLive) {
    required_lease = LeaseKindV1::Workspace;
  }
  if (*kind == StudioNativeToolKindV1::RuntimeDeployLive) {
    approval_required = true;
  }
  if (*kind == StudioNativeToolKindV1::RuntimeInvoke) {
    const auto& invocation = std::get<StudioRuntimeInvokeRequestV1>(request);
    required_lease = invocation.environment == StudioRuntimeEnvironmentV1::Live
                         ? LeaseKindV1::Play
                         : LeaseKindV1::Workspace;
    approval_required =
        invocation.environment == StudioRuntimeEnvironmentV1::Live;
  }
  if (required_lease) {
    if (!record->invocation.lease_id) {
      finish(record,
             failure(record->invocation,
                     error("AGENT_LEASE_REQUIRED",
                           "runtime operation requires its current lease"),
                     NativeToolResultStatusV1::Failed, record->started_ms));
      return;
    }
    bool lease_active = true;
    try {
      lease_active =
          !options.is_lease_active ||
          options.is_lease_active(
              *record->invocation.lease_id, *required_lease);
    } catch (const std::exception& failure) {
      finish(record,
             this->failure(
                 record->invocation,
                 error("AGENT_TOOL_FAILED", failure.what()),
                 NativeToolResultStatusV1::Failed, record->started_ms));
      return;
    } catch (...) {
      finish(record,
             this->failure(
                 record->invocation,
                 error("AGENT_TOOL_FAILED",
                       "runtime lease provider validation failed"),
                 NativeToolResultStatusV1::Failed, record->started_ms));
      return;
    }
    if (!lease_active) {
      finish(record,
             failure(record->invocation,
                     error("AGENT_LEASE_REVOKED",
                           "runtime lease is no longer active"),
                     NativeToolResultStatusV1::Failed, record->started_ms));
      return;
    }
  }
  if (approval_required) {
    if (!record->invocation.approval_grant) {
      finish(record,
             failure(record->invocation,
                     error("AGENT_APPROVAL_REQUIRED",
                           "runtime operation requires exact human approval"),
                     NativeToolResultStatusV1::Failed, record->started_ms));
      return;
    }
    bool approval_valid = true;
    try {
      approval_valid =
          !options.validate_approval_grant ||
          options.validate_approval_grant(record->invocation);
    } catch (const std::exception& failure) {
      finish(record,
             this->failure(
                 record->invocation,
                 error("AGENT_TOOL_FAILED", failure.what()),
                 NativeToolResultStatusV1::Failed, record->started_ms));
      return;
    } catch (...) {
      finish(record,
             this->failure(
                 record->invocation,
                 error("AGENT_TOOL_FAILED",
                       "runtime approval provider validation failed"),
                 NativeToolResultStatusV1::Failed, record->started_ms));
      return;
    }
    if (!approval_valid) {
      finish(record,
             failure(record->invocation,
                     error("AGENT_APPROVAL_MISMATCH",
                           "approval does not bind this exact runtime call"),
                     NativeToolResultStatusV1::Failed, record->started_ms));
      return;
    }
  }

  ValidatedStudioGateV1 gate;
  gate.session_id = record->invocation.session_id;
  gate.run_id = record->invocation.run_id;
  gate.tool_call_id = record->invocation.tool_call_id;
  gate.client_epoch = record->invocation.client_epoch.value_or("");
  gate.context_version = record->invocation.context_version;
  gate.lease_id = record->invocation.lease_id;
  gate.approval_grant = record->invocation.approval_grant;
  gate.validated_at = isoTime(clock.epochMillis());

  const std::weak_ptr<Impl> weak = shared_from_this();
  try {
    studio_adapter->dispatch(
        *kind, request, gate, record->cancellation->token(),
        [weak, record](
            AdapterResultV1<StudioNativeToolOutputV1> result) mutable {
          const auto self = weak.lock();
          if (!self) return;
          if (!self->acceptOrFence(record)) return;
          if (!result.ok()) {
            self->completeAdapterFailure(record, result.error,
                                         result.outcome_unknown);
            return;
          }
          NativeToolOutputV1 output = std::visit(
              [](auto&& value) -> NativeToolOutputV1 {
                return std::forward<decltype(value)>(value);
              },
              std::move(*result.value));
          if (!validOutput(record->invocation.name, output)) {
            const bool ambiguous = self->ambiguousOutcome(record);
            self->finish(
                record,
                self->failure(
                    record->invocation,
                    error("AGENT_TOOL_OUTPUT_INVALID",
                          "native Studio output failed schema validation"),
                    ambiguous ? NativeToolResultStatusV1::OutcomeUnknown
                              : NativeToolResultStatusV1::Failed,
                    record->started_ms));
            return;
          }
          auto completed = self->resultBase(
              record->invocation, NativeToolResultStatusV1::Succeeded,
              record->started_ms);
          completed.output = std::move(output);
          self->finish(record, std::move(completed));
        });
  } catch (...) {
    const bool ambiguous = ambiguousOutcome(record);
    completeAdapterFailure(
        record,
        error(ambiguous ? "AGENT_TOOL_OUTCOME_UNKNOWN"
                        : "AGENT_TOOL_FAILED",
              ambiguous
                  ? "native Studio call failed after an external effect"
                  : "native Studio host dispatch failed before an effect"),
        ambiguous);
  }
}

void NativeToolDispatcherV1::Impl::tick() {
  lease_manager.tick();
  const std::int64_t now = clock.epochMillis();
  std::vector<std::shared_ptr<Record>> expired;
  bool game_active = false;
  bool studio_active = false;
  {
    std::lock_guard lock(mutex);
    for (const auto& [id, record] : records) {
      (void)id;
      if (!record->active || record->result || record->cancel_requested ||
          record->deadline_ms > now) {
        continue;
      }
      record->cancel_requested = true;
      expired.push_back(record);
      const auto* resolved =
          contract(record->invocation.name, record->invocation.version);
      if (resolved && resolved->effectful) {
        if (record->invocation.name.rfind("game.", 0) == 0) {
          game_active = true;
        } else {
          studio_active = true;
        }
      }
    }
  }
  for (const auto& record : expired) {
    record->cancellation->cancel();
  }
  if (game_active) lease_manager.preempt(PreemptionReasonV1::HUMAN_STOP);
  if (studio_active && studio_adapter) {
    studio_adapter->clearAgentOperation(PreemptionReasonV1::HUMAN_STOP);
  }
  for (const auto& record : expired) {
    const bool ambiguous = ambiguousOutcome(record);
    finish(record,
           failure(record->invocation,
                   error(ambiguous ? "AGENT_TOOL_OUTCOME_UNKNOWN"
                                   : "AGENT_TOOL_TIMEOUT",
                         ambiguous
                             ? "native tool timed out after effect dispatch"
                             : "native tool exceeded its deadline"),
                   ambiguous ? NativeToolResultStatusV1::OutcomeUnknown
                             : NativeToolResultStatusV1::TimedOut,
                   record->started_ms));
  }
}

void NativeToolDispatcherV1::Impl::cancelActive(PreemptionReasonV1 reason) {
  std::vector<std::shared_ptr<Record>> active;
  bool game_active = false;
  bool studio_active = false;
  {
    std::lock_guard lock(mutex);
    ++generation;
    for (const auto& [id, record] : records) {
      (void)id;
      if (!record->active || record->result) continue;
      record->cancel_requested = true;
      active.push_back(record);
      if (record->invocation.name.rfind("game.", 0) == 0) {
        game_active = true;
      } else {
        studio_active = true;
      }
    }
  }

  // Both adapters synchronously clear local authority before callbacks.
  for (const auto& record : active) record->cancellation->cancel();
  if (game_active) lease_manager.preempt(reason);
  if (studio_active && studio_adapter) {
    studio_adapter->clearAgentOperation(reason);
  }
  for (const auto& record : active) {
    const bool ambiguous = ambiguousOutcome(record);
    finish(record,
           failure(record->invocation,
                   error(ambiguous ? "AGENT_TOOL_OUTCOME_UNKNOWN"
                                   : "AGENT_CANCELLED",
                         ambiguous
                             ? "effectful native tool was preempted after "
                               "host dispatch"
                             : "native tool was cancelled by local preemption"),
                   ambiguous ? NativeToolResultStatusV1::OutcomeUnknown
                             : NativeToolResultStatusV1::Cancelled,
                   record->started_ms));
  }
}

void NativeToolDispatcherV1::Impl::clearClosedSession() {
  cancelActive(PreemptionReasonV1::SESSION_CLOSED);
  {
    std::lock_guard lock(mutex);
    records.clear();
  }
  lease_manager.clearClosedSession();
}

NativeToolDispatcherV1::NativeToolDispatcherV1(
    player_host::AgentControlLeaseManager& lease_manager,
    CrowdyStudioHostAdapter* studio_adapter,
    NativeToolDispatcherOptionsV1 options)
    : impl_(std::make_shared<Impl>(lease_manager, studio_adapter,
                                  std::move(options))) {}

NativeToolDispatcherV1::~NativeToolDispatcherV1() {
  if (!impl_) return;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->shutting_down = true;
  }
  impl_->cancelActive(PreemptionReasonV1::SESSION_CLOSED);
}

NativeToolDispatcherV1::NativeToolDispatcherV1(
    NativeToolDispatcherV1&&) noexcept = default;

NativeToolDispatcherV1& NativeToolDispatcherV1::operator=(
    NativeToolDispatcherV1&&) noexcept = default;

void NativeToolDispatcherV1::dispatch(NativeToolInvocationV1 invocation,
                                      NativeToolCompletionV1 completion) {
  impl_->dispatch(std::move(invocation), std::move(completion));
}

void NativeToolDispatcherV1::tick() { impl_->tick(); }

void NativeToolDispatcherV1::cancelActive(PreemptionReasonV1 reason) {
  impl_->cancelActive(reason);
}

void NativeToolDispatcherV1::clearClosedSession() {
  impl_->clearClosedSession();
}

bool NativeToolDispatcherV1::has(std::string_view tool_call_id) const {
  std::lock_guard lock(impl_->mutex);
  return impl_->records.find(tool_call_id) != impl_->records.end();
}

}  // namespace crowdy::agent
