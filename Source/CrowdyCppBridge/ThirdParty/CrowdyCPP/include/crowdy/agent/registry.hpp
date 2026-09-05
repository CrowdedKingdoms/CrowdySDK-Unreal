#pragma once

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "crowdy/agent/schema.hpp"
#include "crowdy/agent/types.hpp"

namespace crowdy::agent {

inline constexpr std::array<std::string_view, 22>
    kForbiddenAgentToolSurfaces = {
        "graphql",   "fetch",      "http",       "url",
        "socket",    "shell",      "process",    "filesystem",
        "dom",       "keyboard",   "mouse",      "pointer",
        "host_call", "playercodebroker", "udp",  "token",
        "cookie",    "storage",    "devtools",   "database",
        "buddy",     "raw",
};

struct AgentScopeCondition {
  std::string argumentPath;
  std::string operation;
  std::string canonicalValue;
};

struct AgentScopeRequirement {
  std::string scope;
  std::optional<AgentScopeCondition> when;
};

struct AgentRedactionRule {
  std::string path;
  std::string action;
  std::optional<int> maxBytes;
};

/// Parsed immutable `crowdy.agent-tool/1` descriptor.
struct AgentToolDescriptor {
  std::string schemaVersion;
  std::string name;
  std::string wireName;
  std::string version;
  std::string summary;
  AgentToolExecutor executor = AgentToolExecutor::Server;
  std::vector<AgentMode> modes;
  graphql::Json inputSchema;
  graphql::Json outputSchema;
  AgentToolRisk risk = AgentToolRisk::ReadOnly;
  std::vector<std::string> riskEffects;
  bool riskReversible = true;
  std::vector<AgentScopeRequirement> scopes;
  AgentApprovalPolicy approval = AgentApprovalPolicy::None;
  std::vector<std::string> approvalReasons;
  int approvalMaxTtlSeconds = 0;
  AgentIdempotencyClass idempotency = AgentIdempotencyClass::Pure;
  std::string idempotencyKeyScope;
  int timeoutMs = 0;
  std::vector<AgentRedactionRule> inputRedaction;
  std::vector<AgentRedactionRule> outputRedaction;
  int maxPersistedBytes = 0;
  graphql::Json raw;
  std::string canonical;
};

struct AgentRegisteredTool {
  std::shared_ptr<const AgentToolDescriptor> descriptor;
  std::string descriptorDigest;
};

struct AgentRegistryFilter {
  std::optional<AgentMode> mode;
  std::optional<AgentToolExecutor> executor;
  const std::set<std::string>* availableScopes = nullptr;
};

/// Immutable exact-lookup registry pinned by canonical SHA-256.
class AgentToolRegistry {
 public:
  static constexpr std::string_view kContractVersion =
      "crowdy.agent-tools/1";

  explicit AgentToolRegistry(std::vector<AgentToolDescriptor> descriptors);

  static AgentToolRegistry fromFixtureJson(std::string_view fixtureJson);
  static AgentToolRegistry fromGraphQLDescriptorSet(
      const graphql::Json& value);

  const std::string& registryDigest() const { return registryDigest_; }
  std::string_view contractVersion() const { return kContractVersion; }
  const std::vector<AgentRegisteredTool>& list() const { return entries_; }
  std::vector<AgentRegisteredTool> list(AgentRegistryFilter filter) const;

  const AgentRegisteredTool* get(std::string_view name,
                                 std::string_view version) const;
  const AgentRegisteredTool& require(std::string_view name,
                                     std::string_view version) const;
  const AgentRegisteredTool& fromWireName(std::string_view wireName) const;

  void validateInput(std::string_view name, std::string_view version,
                     const graphql::Json& value) const;
  void validateOutput(std::string_view name, std::string_view version,
                      const graphql::Json& value) const;
  std::vector<std::string> requiredScopes(
      std::string_view name, std::string_view version,
      const graphql::Json& arguments) const;

 private:
  std::vector<AgentRegisteredTool> entries_;
  std::unordered_map<std::string, std::size_t> byLogical_;
  std::unordered_map<std::string, std::size_t> byWire_;
  std::string registryDigest_;
};

using CrowdyAgentToolRegistry = AgentToolRegistry;

AgentToolDescriptor parseAgentToolDescriptor(const graphql::Json& value);

/// Canonical 28-tool Game API registry committed from CrowdyJS v12.
const AgentToolRegistry& canonicalAgentToolRegistryV1();
std::string_view canonicalAgentToolFixtureJsonV1();

}  // namespace crowdy::agent
