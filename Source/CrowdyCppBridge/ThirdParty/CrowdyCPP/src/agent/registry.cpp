#include "crowdy/agent/registry.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <utility>

#include "crowdy_agent_fixture.hpp"

namespace crowdy::agent {

namespace {

[[noreturn]] void descriptorFailure(std::string message) {
  throw CrowdyAgentError("AGENT_TOOL_DESCRIPTOR_INVALID", std::move(message));
}

std::string requireString(const graphql::Json& value, std::string_view field,
                          std::size_t maximum = 65'536) {
  const auto child = value[field];
  if (!child.isString() || child.asStringView().empty() ||
      child.asStringView().size() > maximum) {
    descriptorFailure(std::string(field) + " is outside descriptor bounds");
  }
  return child.asString();
}

std::vector<std::string> strings(const graphql::Json& value,
                                 std::size_t maximum = 256) {
  if (!value.isArray() || value.size() > maximum) {
    descriptorFailure("Descriptor string array is outside bounds");
  }
  std::vector<std::string> result;
  result.reserve(value.size());
  value.forEach([&](const graphql::Json& item) {
    if (!item.isString()) descriptorFailure("Descriptor array must be strings");
    result.push_back(item.asString());
  });
  return result;
}

bool logicalNameValid(std::string_view value) {
  if (value.size() < 3 || value.size() > 160) return false;
  const auto dot = value.find('.');
  if (dot == std::string_view::npos) return false;
  static constexpr std::array<std::string_view, 8> roots = {
      "studio", "project", "workspace", "library",
      "template", "diagnostics", "runtime", "game"};
  if (std::find(roots.begin(), roots.end(), value.substr(0, dot)) ==
      roots.end()) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '_' ||
           character == '.';
  });
}

bool wireNameValid(std::string_view value) {
  return !value.empty() && value.size() <= 64 &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isalnum(character) != 0 || character == '_' ||
                  character == '-';
         });
}

bool semanticVersionValid(std::string_view value) {
  int segments = 0;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto end = value.find('.', start);
    const auto segment =
        value.substr(start, end == std::string_view::npos
                                ? value.size() - start
                                : end - start);
    if (segment.empty() ||
        (segment.size() > 1 && segment.front() == '0') ||
        !std::all_of(segment.begin(), segment.end(),
                     [](unsigned char character) {
                       return std::isdigit(character) != 0;
                     })) {
      return false;
    }
    ++segments;
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return segments == 3;
}

bool redactionPathValid(std::string_view value) {
  if (value.empty() || value.front() != '$') return false;
  for (std::size_t index = 1; index < value.size();) {
    if (value[index] == '.') {
      ++index;
      if (index == value.size() ||
          !(std::isalpha(static_cast<unsigned char>(value[index])) ||
            value[index] == '_')) {
        return false;
      }
      while (index < value.size() &&
             (std::isalnum(static_cast<unsigned char>(value[index])) ||
              value[index] == '_')) {
        ++index;
      }
    } else if (value.substr(index, 3) == "[*]") {
      index += 3;
    } else {
      return false;
    }
  }
  return true;
}

AgentRedactionRule parseRedactionRule(const graphql::Json& value) {
  AgentRedactionRule result;
  result.path = requireString(value, "path", 256);
  result.action = requireString(value, "action", 16);
  if (!redactionPathValid(result.path) ||
      (result.action != "DROP" && result.action != "HASH" &&
       result.action != "MASK" && result.action != "TRUNCATE" &&
       result.action != "SUMMARY")) {
    descriptorFailure("Descriptor redaction rule is invalid");
  }
  if (value["maxBytes"].ok() && !value["maxBytes"].isNull()) {
    result.maxBytes = static_cast<int>(value["maxBytes"].asInt64(-1));
  }
  return result;
}

std::vector<AgentRedactionRule> parseRedactionRules(
    const graphql::Json& value) {
  if (!value.isArray() || value.size() > 64) {
    descriptorFailure("Descriptor redaction rules are outside bounds");
  }
  std::vector<AgentRedactionRule> result;
  result.reserve(value.size());
  value.forEach([&](const graphql::Json& rule) {
    result.push_back(parseRedactionRule(rule));
  });
  return result;
}

AgentScopeRequirement parseScope(const graphql::Json& value) {
  AgentScopeRequirement result;
  result.scope = requireString(value, "scope", 80);
  if (result.scope.size() < 2 ||
      !std::all_of(result.scope.begin(), result.scope.end(),
                   [](unsigned char character) {
                     return (character >= 'a' && character <= 'z') ||
                            (character >= '0' && character <= '9') ||
                            character == '_' || character == '.';
                   })) {
    descriptorFailure("Descriptor scope is invalid");
  }
  const auto when = value["when"];
  if (when.ok() && !when.isNull()) {
    AgentScopeCondition condition;
    condition.argumentPath = requireString(when, "argumentPath", 256);
    condition.operation = requireString(when, "operator", 16);
    const auto comparison = when["value"];
    if (!redactionPathValid(condition.argumentPath) ||
        (condition.operation != "EQUALS" &&
         condition.operation != "CONTAINS") ||
        !comparison.ok() || comparison.isObject() || comparison.isArray()) {
      descriptorFailure("Descriptor conditional scope is invalid");
    }
    condition.canonicalValue = canonicalJson(comparison);
    result.when = std::move(condition);
  }
  return result;
}

std::vector<AgentMode> parseModes(const graphql::Json& value) {
  const auto raw = strings(value, 3);
  if (raw.empty()) descriptorFailure("Descriptor modes must not be empty");
  std::vector<AgentMode> modes;
  std::set<AgentMode> unique;
  for (const auto& item : raw) {
    const auto mode = agentModeFromString(item);
    if (!mode || !unique.insert(*mode).second) {
      descriptorFailure("Descriptor modes contain an unknown or duplicate value");
    }
    modes.push_back(*mode);
  }
  return modes;
}

std::vector<AgentScopeRequirement> parseScopes(const graphql::Json& value) {
  if (!value.isArray() || value.size() > 16) {
    descriptorFailure("Descriptor has too many scope requirements");
  }
  std::vector<AgentScopeRequirement> result;
  result.reserve(value.size());
  value.forEach([&](const graphql::Json& scope) {
    result.push_back(parseScope(scope));
  });
  return result;
}

std::string descriptorKey(const AgentToolDescriptor& descriptor) {
  return descriptor.name + "@" + descriptor.version;
}

std::string registryCanonical(
    const std::vector<AgentRegisteredTool>& entries) {
  std::string output = R"({"contract":"crowdy.agent-tools/1","descriptors":[)";
  for (std::size_t index = 0; index < entries.size(); ++index) {
    if (index != 0) output.push_back(',');
    output += R"({"descriptor":)";
    output += entries[index].descriptor->canonical;
    output += R"(,"descriptorDigest":)";
    output += graphql::JVal(entries[index].descriptorDigest).dump();
    output.push_back('}');
  }
  output += "]}";
  return output;
}

std::vector<graphql::Json> resolveArgumentPath(
    const graphql::Json& value, std::string_view path) {
  if (path.size() < 2 || path.substr(0, 2) != "$.") return {};
  std::vector<graphql::Json> values = {value};
  std::size_t start = 2;
  while (start <= path.size()) {
    const auto end = path.find('.', start);
    auto segment =
        path.substr(start, end == std::string_view::npos
                               ? path.size() - start
                               : end - start);
    const bool wildcard =
        segment.size() >= 3 && segment.substr(segment.size() - 3) == "[*]";
    if (wildcard) segment.remove_suffix(3);
    std::vector<graphql::Json> next;
    for (const auto& candidate : values) {
      const auto child = candidate[segment];
      if (wildcard && child.isArray()) {
        child.forEach(
            [&](const graphql::Json& entry) { next.push_back(entry); });
      } else if (!wildcard && child.ok()) {
        next.push_back(child);
      }
    }
    values = std::move(next);
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return values;
}

}  // namespace

AgentToolDescriptor parseAgentToolDescriptor(const graphql::Json& value) {
  if (!value.isObject()) descriptorFailure("Descriptor must be an object");
  AgentToolDescriptor result;
  result.raw = value;
  result.canonical = canonicalJson(value);
  result.schemaVersion = requireString(value, "schemaVersion", 64);
  result.name = requireString(value, "name", 160);
  result.wireName = requireString(value, "wireName", 64);
  result.version = requireString(value, "version", 32);
  result.summary = requireString(value, "summary", 240);
  if (result.schemaVersion != "crowdy.agent-tool/1" ||
      !logicalNameValid(result.name) || !wireNameValid(result.wireName) ||
      !semanticVersionValid(result.version) || result.summary.size() < 8) {
    descriptorFailure("Descriptor identity fields are invalid");
  }
  const auto major = result.version.substr(0, result.version.find('.'));
  if (!result.wireName.ends_with("_v" + major)) {
    descriptorFailure("Descriptor wire name has the wrong major suffix");
  }
  std::string authoritySurface = result.name + "." + result.wireName;
  std::transform(authoritySurface.begin(), authoritySurface.end(),
                 authoritySurface.begin(), [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  for (const auto surface : kForbiddenAgentToolSurfaces) {
    if (authoritySurface.find(surface) != std::string::npos) {
      descriptorFailure("Descriptor exposes a forbidden authority surface");
    }
  }
  result.executor = agentToolExecutorFromString(
                        requireString(value, "executor", 16))
                        .value_or(AgentToolExecutor::Server);
  if (!agentToolExecutorFromString(value["executor"].asStringView())) {
    descriptorFailure("Descriptor executor is invalid");
  }
  result.modes = parseModes(value["modes"]);
  result.inputSchema = value["inputSchema"];
  result.outputSchema = value["outputSchema"];
  assertBoundedJsonSchema(result.inputSchema, true);
  assertBoundedJsonSchema(result.outputSchema, false);

  const auto risk = value["risk"];
  const auto riskValue =
      agentToolRiskFromString(requireString(risk, "class", 32));
  if (!riskValue) descriptorFailure("Descriptor risk is invalid");
  result.risk = *riskValue;
  result.riskEffects = strings(risk["effects"], 64);
  result.riskReversible = risk["reversible"].asBool(false);
  result.scopes = parseScopes(value["scopes"]);

  const auto approval = value["approval"];
  const auto approvalValue =
      agentApprovalPolicyFromString(requireString(approval, "policy", 16));
  if (!approvalValue) descriptorFailure("Descriptor approval policy is invalid");
  result.approval = *approvalValue;
  result.approvalReasons = strings(approval["reasons"], 32);
  result.approvalMaxTtlSeconds =
      static_cast<int>(approval["maxTtlSeconds"].asInt64(-1));

  const auto idempotency = value["idempotency"];
  const auto idempotencyValue = agentIdempotencyClassFromString(
      requireString(idempotency, "class", 32));
  if (!idempotencyValue) {
    descriptorFailure("Descriptor idempotency class is invalid");
  }
  result.idempotency = *idempotencyValue;
  result.idempotencyKeyScope =
      requireString(idempotency, "keyScope", 32);
  result.timeoutMs = static_cast<int>(value["timeoutMs"].asInt64(-1));

  const auto redaction = value["redaction"];
  result.inputRedaction = parseRedactionRules(redaction["input"]);
  result.outputRedaction = parseRedactionRules(redaction["output"]);
  result.maxPersistedBytes =
      static_cast<int>(redaction["maxPersistedBytes"].asInt64(-1));
  if (result.timeoutMs < 50 || result.timeoutMs > 120'000 ||
      result.approvalMaxTtlSeconds < 0 ||
      result.approvalMaxTtlSeconds > 300 ||
      result.maxPersistedBytes < 0 || result.maxPersistedBytes > 65'536) {
    descriptorFailure("Descriptor timeout, approval, or redaction bounds are invalid");
  }
  const bool approvalRisk =
      result.risk == AgentToolRisk::Destructive ||
      result.risk == AgentToolRisk::TrustConsent ||
      result.risk == AgentToolRisk::Economic ||
      result.risk == AgentToolRisk::Irreversible;
  if (approvalRisk && result.approval != AgentApprovalPolicy::Required) {
    descriptorFailure("Descriptor risk requires exact human approval");
  }
  if ((result.approval == AgentApprovalPolicy::None &&
       (!result.approvalReasons.empty() ||
        result.approvalMaxTtlSeconds != 0)) ||
      (result.approval != AgentApprovalPolicy::None &&
       (result.approvalReasons.empty() ||
        result.approvalMaxTtlSeconds == 0))) {
    descriptorFailure("Descriptor approval metadata is inconsistent");
  }
  const std::string_view expectedKeyScope =
      result.idempotency == AgentIdempotencyClass::Pure
          ? "NONE"
          : result.idempotency == AgentIdempotencyClass::Keyed
                ? "USER_TOOL_ARGUMENTS"
                : "TOOL_CALL";
  if (result.idempotencyKeyScope != expectedKeyScope ||
      (result.executor == AgentToolExecutor::Browser &&
       result.idempotency != AgentIdempotencyClass::Pure &&
       result.idempotency != AgentIdempotencyClass::ToolCallOnce &&
       result.idempotency != AgentIdempotencyClass::NonRetryable)) {
    descriptorFailure("Descriptor idempotency metadata is unsafe");
  }
  for (const auto& rule :
       [&] {
         auto combined = result.inputRedaction;
         combined.insert(combined.end(), result.outputRedaction.begin(),
                         result.outputRedaction.end());
         return combined;
       }()) {
    if ((rule.action == "TRUNCATE" &&
         (!rule.maxBytes || *rule.maxBytes < 1 ||
          *rule.maxBytes > result.maxPersistedBytes)) ||
        (rule.action != "TRUNCATE" && rule.maxBytes)) {
      descriptorFailure("Descriptor redaction byte bounds are invalid");
    }
  }
  return result;
}

AgentToolRegistry::AgentToolRegistry(
    std::vector<AgentToolDescriptor> descriptors) {
  if (descriptors.empty() || descriptors.size() > 256) {
    descriptorFailure("Registry must contain 1 to 256 tools");
  }
  std::sort(descriptors.begin(), descriptors.end(),
            [](const auto& left, const auto& right) {
              return descriptorKey(left) < descriptorKey(right);
            });
  entries_.reserve(descriptors.size());
  for (auto& descriptor : descriptors) {
    const auto key = descriptorKey(descriptor);
    if (byLogical_.contains(key) || byWire_.contains(descriptor.wireName)) {
      descriptorFailure("Registry contains duplicate logical or wire names");
    }
    auto immutable =
        std::make_shared<const AgentToolDescriptor>(std::move(descriptor));
    const auto index = entries_.size();
    entries_.push_back(
        AgentRegisteredTool{immutable, sha256Digest(immutable->canonical)});
    byLogical_.emplace(key, index);
    byWire_.emplace(immutable->wireName, index);
  }
  registryDigest_ = sha256Digest(registryCanonical(entries_));
}

AgentToolRegistry AgentToolRegistry::fromFixtureJson(
    std::string_view fixtureJson) {
  const auto fixture = graphql::Json::parse(fixtureJson);
  if (!fixture.isObject() ||
      fixture["contractVersion"].asStringView() != kContractVersion ||
      !fixture["tools"].isArray()) {
    descriptorFailure("Canonical agent fixture is malformed");
  }
  std::vector<AgentToolDescriptor> descriptors;
  std::vector<std::string> expectedDigests;
  fixture["tools"].forEach([&](const graphql::Json& item) {
    descriptors.push_back(parseAgentToolDescriptor(item["descriptor"]));
    expectedDigests.push_back(requireString(item, "descriptorDigest", 71));
  });
  AgentToolRegistry registry(std::move(descriptors));
  if (expectedDigests.size() != registry.entries_.size()) {
    descriptorFailure("Canonical agent fixture count changed");
  }
  for (std::size_t index = 0; index < registry.entries_.size(); ++index) {
    if (registry.entries_[index].descriptorDigest != expectedDigests[index]) {
      descriptorFailure(
          "Canonical descriptor digest mismatch for " +
          descriptorKey(*registry.entries_[index].descriptor) +
          ": expected " + expectedDigests[index] + ", got " +
          registry.entries_[index].descriptorDigest);
    }
  }
  const auto expectedRegistry =
      requireString(fixture, "registryDigest", 71);
  if (registry.registryDigest_ != expectedRegistry) {
    descriptorFailure("Canonical registry digest mismatch");
  }
  return registry;
}

AgentToolRegistry AgentToolRegistry::fromGraphQLDescriptorSet(
    const graphql::Json& value) {
  if (!value.isObject() || !value["tools"].isArray()) {
    descriptorFailure("GraphQL descriptor set is malformed");
  }
  std::vector<AgentToolDescriptor> descriptors;
  std::unordered_map<std::string, std::string> expectedDigests;
  value["tools"].forEach([&](const graphql::Json& item) {
    const auto descriptorJson =
        requireString(item, "descriptorJson", 1'048'576);
    const auto parsed = graphql::Json::parse(descriptorJson);
    if (!parsed.isObject() || canonicalJson(parsed) != descriptorJson) {
      descriptorFailure("Server descriptor JSON is not canonical");
    }
    auto descriptor = parseAgentToolDescriptor(parsed);
    const auto digest = requireString(item, "descriptorDigest", 71);
    graphql::JArray scopeNames;
    for (const auto& requirement : descriptor.scopes) {
      scopeNames.emplace_back(requirement.scope);
    }
    const auto canonicalField = [&](std::string_view itemField,
                                    const graphql::Json& rawField) {
      const auto encoded = requireString(item, itemField, 1'048'576);
      return encoded == canonicalJson(rawField);
    };
    if (sha256Digest(descriptor.canonical) != digest ||
        descriptor.name != item["name"].asStringView() ||
        descriptor.version != item["version"].asStringView() ||
        descriptor.wireName != item["wireName"].asStringView() ||
        descriptor.schemaVersion != item["schemaVersion"].asStringView() ||
        descriptor.summary != item["summary"].asStringView() ||
        toString(descriptor.executor) != item["executor"].asStringView() ||
        canonicalJson(descriptor.raw["modes"]) !=
            canonicalJson(item["modes"]) ||
        toString(descriptor.risk) != item["risk"].asStringView() ||
        canonicalJson(descriptor.raw["risk"]["effects"]) !=
            canonicalJson(item["riskEffects"]) ||
        descriptor.riskReversible !=
            item["riskReversible"].asBool(!descriptor.riskReversible) ||
        graphql::JVal(std::move(scopeNames)).dump() !=
            canonicalJson(item["scopes"]) ||
        !canonicalField("scopeRequirementsJson",
                        descriptor.raw["scopes"]) ||
        (descriptor.approval == AgentApprovalPolicy::Required) !=
            item["approvalRequired"].asBool(
                descriptor.approval != AgentApprovalPolicy::Required) ||
        toString(descriptor.approval) !=
            item["approvalPolicy"].asStringView() ||
        canonicalJson(descriptor.raw["approval"]["reasons"]) !=
            canonicalJson(item["approvalReasons"]) ||
        descriptor.approvalMaxTtlSeconds !=
            item["approvalMaxTtlSeconds"].asInt64(-1) ||
        toString(descriptor.idempotency) !=
            item["idempotencyClass"].asStringView() ||
        descriptor.idempotencyKeyScope !=
            item["idempotencyKeyScope"].asStringView() ||
        descriptor.timeoutMs != item["timeoutMs"].asInt64(-1) ||
        !canonicalField("inputSchemaJson", descriptor.inputSchema) ||
        !canonicalField("outputSchemaJson", descriptor.outputSchema) ||
        !canonicalField("inputRedactionJson",
                        descriptor.raw["redaction"]["input"]) ||
        !canonicalField("outputRedactionJson",
                        descriptor.raw["redaction"]["output"]) ||
        descriptor.maxPersistedBytes !=
            item["maxPersistedBytes"].asInt64(-1)) {
      descriptorFailure("Server descriptor fields or digest diverged");
    }
    expectedDigests.emplace(descriptorKey(descriptor), digest);
    descriptors.push_back(std::move(descriptor));
  });
  AgentToolRegistry registry(std::move(descriptors));
  for (const auto& entry : registry.entries_) {
    const auto found =
        expectedDigests.find(descriptorKey(*entry.descriptor));
    if (found == expectedDigests.end() ||
        found->second != entry.descriptorDigest) {
      descriptorFailure("Server descriptor digest mismatch");
    }
  }
  const auto expectedRegistry =
      requireString(value, "registryDigest", 71);
  if (registry.registryDigest_ != expectedRegistry) {
    throw CrowdyAgentError(
        "AGENT_CONTEXT_STALE",
        "Game API descriptor registry digest does not match canonical descriptors");
  }
  return registry;
}

std::vector<AgentRegisteredTool> AgentToolRegistry::list(
    AgentRegistryFilter filter) const {
  std::vector<AgentRegisteredTool> result;
  for (const auto& entry : entries_) {
    const auto& descriptor = *entry.descriptor;
    if (filter.mode &&
        std::find(descriptor.modes.begin(), descriptor.modes.end(),
                  *filter.mode) == descriptor.modes.end()) {
      continue;
    }
    if (filter.executor && descriptor.executor != *filter.executor) continue;
    if (filter.availableScopes) {
      const bool unavailable = std::any_of(
          descriptor.scopes.begin(), descriptor.scopes.end(),
          [&](const AgentScopeRequirement& requirement) {
            return !requirement.when &&
                   !filter.availableScopes->contains(requirement.scope);
          });
      if (unavailable) continue;
    }
    result.push_back(entry);
  }
  return result;
}

const AgentRegisteredTool* AgentToolRegistry::get(
    std::string_view name, std::string_view version) const {
  const auto found =
      byLogical_.find(std::string(name) + "@" + std::string(version));
  return found == byLogical_.end() ? nullptr : &entries_[found->second];
}

const AgentRegisteredTool& AgentToolRegistry::require(
    std::string_view name, std::string_view version) const {
  if (const auto* entry = get(name, version)) return *entry;
  const bool knownName = std::any_of(
      entries_.begin(), entries_.end(), [&](const AgentRegisteredTool& entry) {
        return entry.descriptor->name == name;
      });
  throw CrowdyAgentError(
      knownName ? "AGENT_TOOL_VERSION_UNSUPPORTED" : "AGENT_TOOL_UNKNOWN",
      knownName ? "Unsupported agent tool version" : "Unknown agent tool");
}

const AgentRegisteredTool& AgentToolRegistry::fromWireName(
    std::string_view wireName) const {
  const auto found = byWire_.find(std::string(wireName));
  if (found == byWire_.end()) {
    throw CrowdyAgentError("AGENT_TOOL_UNKNOWN",
                           "Unknown agent tool wire name");
  }
  return entries_[found->second];
}

void AgentToolRegistry::validateInput(std::string_view name,
                                      std::string_view version,
                                      const graphql::Json& value) const {
  validateJsonSchemaValue(require(name, version).descriptor->inputSchema,
                          value);
}

void AgentToolRegistry::validateOutput(std::string_view name,
                                       std::string_view version,
                                       const graphql::Json& value) const {
  validateJsonSchemaValue(
      require(name, version).descriptor->outputSchema, value,
      AgentSchemaValidationOptions{AgentSchemaDirection::Output});
}

std::vector<std::string> AgentToolRegistry::requiredScopes(
    std::string_view name, std::string_view version,
    const graphql::Json& arguments) const {
  const auto& descriptor = *require(name, version).descriptor;
  validateInput(name, version, arguments);
  std::vector<std::string> result;
  for (const auto& requirement : descriptor.scopes) {
    if (!requirement.when) {
      result.push_back(requirement.scope);
      continue;
    }
    const auto values =
        resolveArgumentPath(arguments, requirement.when->argumentPath);
    const bool matched = std::any_of(
        values.begin(), values.end(), [&](const graphql::Json& value) {
          if (requirement.when->operation == "EQUALS") {
            return canonicalJson(value) == requirement.when->canonicalValue;
          }
          if (value.isArray()) {
            bool contains = false;
            value.forEach([&](const graphql::Json& entry) {
              contains = contains ||
                         canonicalJson(entry) ==
                             requirement.when->canonicalValue;
            });
            return contains;
          }
          return canonicalJson(value) == requirement.when->canonicalValue;
        });
    if (matched) result.push_back(requirement.scope);
  }
  return result;
}

std::string_view canonicalAgentToolFixtureJsonV1() {
  return generated::kCanonicalAgentToolsV1;
}

const AgentToolRegistry& canonicalAgentToolRegistryV1() {
  static const AgentToolRegistry registry =
      AgentToolRegistry::fromFixtureJson(canonicalAgentToolFixtureJsonV1());
  return registry;
}

}  // namespace crowdy::agent
