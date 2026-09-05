#include "crowdy/agent/errors.hpp"

#include <algorithm>
#include <cctype>

namespace crowdy::agent {

namespace {

std::optional<std::string> bounded(std::optional<std::string> value,
                                   std::size_t maximum) {
  if (!value) return std::nullopt;
  *value = sanitizeAgentText(*value);
  if (value->size() > maximum) value->resize(maximum);
  if (value->empty()) return std::nullopt;
  return value;
}

bool startsSensitive(std::string_view value, std::size_t offset,
                     std::size_t* length) {
  static constexpr std::array<std::string_view, 5> words = {
      "bearer", "token", "secret", "api_key", "api-key"};
  for (const auto word : words) {
    if (offset + word.size() > value.size()) continue;
    bool same = true;
    for (std::size_t i = 0; i < word.size(); ++i) {
      const auto left =
          static_cast<unsigned char>(value[offset + i]);
      const auto right = static_cast<unsigned char>(word[i]);
      if (std::tolower(left) != std::tolower(right)) {
        same = false;
        break;
      }
    }
    if (same) {
      *length = word.size();
      return true;
    }
  }
  return false;
}

}  // namespace

bool isAgentErrorCode(std::string_view code) {
  return std::find(kAgentErrorCodes.begin(), kAgentErrorCodes.end(), code) !=
         kAgentErrorCodes.end();
}

std::string sanitizeAgentText(std::string_view value) {
  std::string output;
  output.reserve(std::min<std::size_t>(value.size(), 512));
  for (std::size_t i = 0; i < value.size() && output.size() < 512;) {
    std::size_t wordLength = 0;
    if (startsSensitive(value, i, &wordLength)) {
      std::size_t end = i + wordLength;
      while (end < value.size() &&
             (value[end] == ' ' || value[end] == ':' || value[end] == '=')) {
        ++end;
      }
      while (end < value.size()) {
        const unsigned char c = static_cast<unsigned char>(value[end]);
        if (std::isspace(c) || c == ',' || c == ';') break;
        ++end;
      }
      output += "[redacted]";
      i = end;
      continue;
    }
    const unsigned char c = static_cast<unsigned char>(value[i++]);
    if ((c < 0x20 && c != '\t' && c != '\n' && c != '\r') || c == 0x7f) {
      output.push_back(' ');
    } else {
      output.push_back(static_cast<char>(c));
    }
  }
  const auto first = output.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "Agent operation failed";
  const auto last = output.find_last_not_of(" \t\r\n");
  return output.substr(first, last - first + 1);
}

CrowdyAgentError::CrowdyAgentError(
    std::string code, std::string message, bool retryable,
    std::optional<std::string> remediation, std::optional<std::string> field,
    std::optional<std::string> requiredScope)
    : graphql::CrowdyError(
          isAgentErrorCode(code) ? std::move(code) : "AGENT_TOOL_FAILED",
          sanitizeAgentText(message)),
      retryable_(retryable),
      remediation_(bounded(std::move(remediation), 512)),
      field_(bounded(std::move(field), 256)),
      requiredScope_(bounded(std::move(requiredScope), 80)) {}

AgentError CrowdyAgentError::value() const {
  return AgentError{code(), what(), retryable_, remediation_, field_,
                    requiredScope_};
}

CrowdyAgentOutcomeUnknownError::CrowdyAgentOutcomeUnknownError(
    std::string message)
    : CrowdyAgentError(
          "AGENT_TOOL_OUTCOME_UNKNOWN", std::move(message), false,
          "Inspect the current project or game state before continuing.") {}

AgentError toAgentError(const std::exception& error,
                        std::string_view fallbackCode) {
  if (const auto* agentError =
          dynamic_cast<const CrowdyAgentError*>(&error)) {
    return agentError->value();
  }
  return makeAgentError(fallbackCode, "Agent operation failed");
}

AgentError makeAgentError(std::string_view code, std::string_view message,
                          bool retryable) {
  return CrowdyAgentError(std::string(code), std::string(message), retryable)
      .value();
}

}  // namespace crowdy::agent
