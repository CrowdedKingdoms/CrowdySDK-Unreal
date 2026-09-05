#pragma once

#include <string_view>

#include "crowdy/agent/controller.hpp"
#include "crowdy/agent/native_tool_dispatcher.hpp"

namespace crowdy::agent {

struct NativeBrowserToolDispatcherAdapterOptions {
  /// Defaults to the immutable CrowdyJS v12 canonical registry.
  const AgentToolRegistry* registry = nullptr;
  /// Used only to timestamp conversion failures before native dispatch starts.
  const core::IClock* clock = nullptr;
};

/**
 * Exact bridge from durable AgentToolInvocation envelopes to the typed native
 * local-tool dispatcher. JSON is schema-validated and converted field by field
 * into closed C++ variants; native outputs are converted back to canonical
 * validated JSON. No CrowdyClient, GraphQL, UDP, filesystem, shell, provider,
 * or generic callback authority crosses this boundary.
 */
class NativeBrowserToolDispatcherAdapter final
    : public IAgentBrowserToolDispatcher {
 public:
  explicit NativeBrowserToolDispatcherAdapter(
      NativeToolDispatcherV1& dispatcher,
      NativeBrowserToolDispatcherAdapterOptions options = {});

  void dispatch(AgentToolInvocation invocation,
                AgentCallback<AgentToolResult> callback) override;
  void cancelActive(AgentPreemptionReason reason) override;
  void clearClosedSession() override;
  void tick() override;

  bool has(std::string_view toolCallId) const;

 private:
  NativeToolDispatcherV1& dispatcher_;
  const AgentToolRegistry& registry_;
  const core::IClock& clock_;
};

using NativeToolBrowserDispatcherAdapter =
    NativeBrowserToolDispatcherAdapter;

}  // namespace crowdy::agent
