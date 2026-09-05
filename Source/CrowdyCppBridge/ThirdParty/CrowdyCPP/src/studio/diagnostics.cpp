#include "crowdy/studio/diagnostics.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <regex>
#include <string>
#include <unordered_set>
#include <utility>

#include "crowdy/graphql/json.hpp"

namespace crowdy::studio {
namespace {

constexpr std::string_view kReplacementCharacter = "\xef\xbf\xbd";

std::size_t utf8SequenceLength(unsigned char byte) {
  if (byte <= 0x7f) return 1;
  if (byte >= 0xc2 && byte <= 0xdf) return 2;
  if (byte >= 0xe0 && byte <= 0xef) return 3;
  if (byte >= 0xf0 && byte <= 0xf4) return 4;
  return 0;
}

bool validUtf8Sequence(std::string_view value, std::size_t index,
                       std::size_t length) {
  if (length == 0 || index + length > value.size()) return false;
  for (std::size_t offset = 1; offset < length; ++offset) {
    const unsigned char byte =
        static_cast<unsigned char>(value[index + offset]);
    if ((byte & 0xc0U) != 0x80U) return false;
  }
  const unsigned char first = static_cast<unsigned char>(value[index]);
  const unsigned char second =
      length > 1 ? static_cast<unsigned char>(value[index + 1]) : 0;
  if (length == 3 &&
      ((first == 0xe0 && second < 0xa0) ||
       (first == 0xed && second >= 0xa0))) {
    return false;
  }
  if (length == 4 &&
      ((first == 0xf0 && second < 0x90) ||
       (first == 0xf4 && second >= 0x90))) {
    return false;
  }
  return true;
}

std::string sanitizeUtf8(std::string_view input) {
  const std::size_t limit =
      std::min(input.size(), kCrowdyStudioDiagnosticMaxInputBytes);
  std::string result;
  result.reserve(limit);
  for (std::size_t index = 0; index < limit;) {
    const unsigned char byte = static_cast<unsigned char>(input[index]);
    const std::size_t length = utf8SequenceLength(byte);
    if (length == 1) {
      result.push_back(static_cast<char>(byte));
      ++index;
      continue;
    }
    if (length > 1 && index + length <= limit &&
        validUtf8Sequence(input, index, length)) {
      result.append(input.substr(index, length));
      index += length;
      continue;
    }
    result.append(kReplacementCharacter);
    ++index;
  }
  return result;
}

std::string truncateUtf8(std::string_view value, std::size_t maxBytes) {
  if (value.size() <= maxBytes) return std::string(value);
  std::string result;
  result.reserve(maxBytes);
  for (std::size_t index = 0; index < value.size();) {
    const std::size_t length =
        utf8SequenceLength(static_cast<unsigned char>(value[index]));
    const std::size_t safeLength =
        length > 0 && validUtf8Sequence(value, index, length) ? length : 1;
    if (result.size() + safeLength > maxBytes) break;
    result.append(value.substr(index, safeLength));
    index += safeLength;
  }
  return result;
}

std::string trimAscii(std::string_view value) {
  std::size_t first = 0;
  while (first < value.size() &&
         std::isspace(static_cast<unsigned char>(value[first])) != 0) {
    ++first;
  }
  std::size_t last = value.size();
  while (last > first &&
         std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
    --last;
  }
  return std::string(value.substr(first, last - first));
}

std::vector<std::string> splitBoundedLines(std::string_view value) {
  std::vector<std::string> lines;
  lines.reserve(std::min<std::size_t>(256, kCrowdyStudioDiagnosticMaxLines));
  std::size_t start = 0;
  for (std::size_t index = 0;
       index < value.size() &&
       lines.size() < kCrowdyStudioDiagnosticMaxLines;
       ++index) {
    if (value[index] != '\n' && value[index] != '\r') continue;
    lines.push_back(truncateUtf8(
        value.substr(start, index - start),
        kCrowdyStudioDiagnosticMaxLineBytes));
    if (value[index] == '\r' && index + 1 < value.size() &&
        value[index + 1] == '\n') {
      ++index;
    }
    start = index + 1;
  }
  if (lines.size() < kCrowdyStudioDiagnosticMaxLines &&
      start <= value.size()) {
    lines.push_back(truncateUtf8(
        value.substr(start), kCrowdyStudioDiagnosticMaxLineBytes));
  }
  return lines;
}

CrowdyStudioDiagnosticSeverity normalizeSeverity(std::string_view value) {
  if (value == "warning") return CrowdyStudioDiagnosticSeverity::Warning;
  if (value == "info" || value == "note" || value == "help") {
    return CrowdyStudioDiagnosticSeverity::Info;
  }
  return CrowdyStudioDiagnosticSeverity::Error;
}

std::optional<std::uint32_t> coordinate(std::string_view value) {
  if (value.empty()) return std::nullopt;
  std::uint64_t parsed = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} ||
      result.ptr != value.data() + value.size() || parsed == 0 ||
      parsed > kCrowdyStudioDiagnosticMaxCoordinate) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(parsed);
}

std::optional<std::uint32_t> coordinate(const graphql::Json& value) {
  if (!value.isNumber()) return std::nullopt;
  const double parsed = value.asDouble();
  if (!std::isfinite(parsed) || std::trunc(parsed) != parsed ||
      parsed < 1 ||
      parsed >
          static_cast<double>(kCrowdyStudioDiagnosticMaxCoordinate)) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(parsed);
}

std::optional<std::string> boundedCode(std::string_view value) {
  if (value.empty()) return std::nullopt;
  return truncateUtf8(value, kCrowdyStudioDiagnosticMaxCodeBytes);
}

struct DiagnosticLocation {
  CrowdyStudioTarget target = CrowdyStudioTarget::Server;
  std::string path;
};

std::optional<DiagnosticLocation> normalizeLocation(
    std::string_view raw, CrowdyStudioTarget defaultTarget) {
  std::string path = trimAscii(raw);
  std::replace(path.begin(), path.end(), '\\', '/');
  CrowdyStudioTarget target = defaultTarget;

  static const std::regex targetPath(
      R"((?:^|/)(server|client)/(.+)$)",
      std::regex::ECMAScript | std::regex::icase);
  std::smatch targetMatch;
  if (std::regex_search(path, targetMatch, targetPath)) {
    std::string selected = targetMatch[1].str();
    std::transform(
        selected.begin(), selected.end(), selected.begin(),
        [](unsigned char character) {
          return static_cast<char>(std::tolower(character));
        });
    target = selected == "server" ? CrowdyStudioTarget::Server
                                  : CrowdyStudioTarget::Client;
    path = targetMatch[2].str();
  } else {
    const std::size_t sourceIndex = path.rfind("/src/");
    if (sourceIndex != std::string::npos) {
      path = path.substr(sourceIndex + 1);
    } else if (const std::size_t slash = path.rfind('/');
               slash != std::string::npos &&
               path.substr(slash + 1) == "Cargo.toml") {
      path = "Cargo.toml";
    }
  }

  try {
    return DiagnosticLocation{
        target, normalizeCrowdyStudioPath(path)};
  } catch (...) {
    return std::nullopt;
  }
}

std::vector<CrowdyStudioDiagnostic> parseJsonDiagnostic(
    const std::string& line, CrowdyStudioTarget defaultTarget) {
  const std::string trimmed = trimAscii(line);
  if (trimmed.empty() || trimmed.front() != '{') return {};
  const graphql::Json root = graphql::Json::parse(trimmed);
  if (!root.isObject()) return {};
  const graphql::Json nestedMessage = root["message"];
  const graphql::Json message =
      nestedMessage.isObject() ? nestedMessage : root;
  if (!message.isObject() || !message["message"].isString()) return {};

  const CrowdyStudioDiagnosticSeverity severity =
      message["level"].isString()
          ? normalizeSeverity(message["level"].asStringView())
          : CrowdyStudioDiagnosticSeverity::Error;
  std::optional<std::string> code;
  if (message["code"].isObject() &&
      message["code"]["code"].isString()) {
    code = boundedCode(message["code"]["code"].asStringView());
  }
  const graphql::Json spans = message["spans"];
  if (!spans.isArray()) return {};

  const std::string diagnosticMessage = truncateUtf8(
      message["message"].asStringView(),
      kCrowdyStudioDiagnosticMaxMessageBytes);
  std::vector<CrowdyStudioDiagnostic> result;
  result.reserve(std::min<std::size_t>(
      spans.size(), kCrowdyStudioDiagnosticMaxCount));
  for (std::size_t index = 0;
       index < spans.size() &&
       result.size() < kCrowdyStudioDiagnosticMaxCount;
       ++index) {
    const graphql::Json span = spans.at(index);
    if (!span.isObject() || !span["is_primary"].isBool() ||
        !span["is_primary"].asBool() || !span["file_name"].isString()) {
      continue;
    }
    const auto lineStart = coordinate(span["line_start"]);
    const auto columnStart = coordinate(span["column_start"]);
    if (!lineStart || !columnStart) continue;
    const auto location =
        normalizeLocation(span["file_name"].asStringView(), defaultTarget);
    if (!location) continue;

    CrowdyStudioDiagnostic diagnostic;
    diagnostic.target = location->target;
    diagnostic.path = location->path;
    diagnostic.line = *lineStart;
    diagnostic.column = *columnStart;
    diagnostic.endLine = coordinate(span["line_end"]);
    diagnostic.endColumn = coordinate(span["column_end"]);
    diagnostic.severity = severity;
    diagnostic.message = diagnosticMessage;
    diagnostic.code = code;
    diagnostic.source = CrowdyStudioDiagnosticSource::Rustc;
    result.push_back(std::move(diagnostic));
  }
  return result;
}

std::string deduplicationKey(const CrowdyStudioDiagnostic& diagnostic) {
  std::string key;
  key.reserve(diagnostic.path.size() + diagnostic.message.size() + 64);
  key += toString(diagnostic.target);
  key.push_back(':');
  key += diagnostic.path;
  key.push_back(':');
  key += std::to_string(diagnostic.line);
  key.push_back(':');
  key += std::to_string(diagnostic.column);
  key.push_back(':');
  key += toString(diagnostic.severity);
  key.push_back(':');
  key += diagnostic.code.value_or("");
  key.push_back(':');
  key += diagnostic.message;
  return key;
}

}  // namespace

std::vector<CrowdyStudioDiagnostic> parseRustcDiagnostics(
    std::string_view output, CrowdyStudioTarget defaultTarget) {
  if (output.empty()) return {};

  static const std::regex header(
      R"(^\s*(error|warning|info|note)(?:\[([^\]]+)\])?:\s*(.+?)\s*$)",
      std::regex::ECMAScript);
  static const std::regex arrow(
      R"(^\s*-->\s+(.+?):([0-9]+):([0-9]+)(?:-([0-9]+))?\s*$)",
      std::regex::ECMAScript);
  static const std::regex compact(
      R"(^\s*(.+?):([0-9]+):([0-9]+):\s*(error|warning|info|note)(?:\[([^\]]+)\])?:\s*(.+?)\s*$)",
      std::regex::ECMAScript);

  struct PendingDiagnostic {
    CrowdyStudioDiagnosticSeverity severity =
        CrowdyStudioDiagnosticSeverity::Error;
    std::string message;
    std::optional<std::string> code;
  };

  std::optional<PendingDiagnostic> pending;
  std::vector<CrowdyStudioDiagnostic> diagnostics;
  diagnostics.reserve(16);
  std::unordered_set<std::string> seen;
  const auto append = [&](CrowdyStudioDiagnostic diagnostic) {
    if (diagnostics.size() >= kCrowdyStudioDiagnosticMaxCount) return;
    const std::string key = deduplicationKey(diagnostic);
    if (seen.insert(key).second) {
      diagnostics.push_back(std::move(diagnostic));
    }
  };

  const std::string sanitized = sanitizeUtf8(output);
  for (const std::string& line : splitBoundedLines(sanitized)) {
    if (diagnostics.size() >= kCrowdyStudioDiagnosticMaxCount) break;

    auto jsonDiagnostics = parseJsonDiagnostic(line, defaultTarget);
    if (!jsonDiagnostics.empty()) {
      for (auto& diagnostic : jsonDiagnostics) {
        append(std::move(diagnostic));
      }
      pending.reset();
      continue;
    }

    std::smatch match;
    if (std::regex_match(line, match, header)) {
      PendingDiagnostic value;
      value.severity = normalizeSeverity(match[1].str());
      value.message = truncateUtf8(
          match[3].str(), kCrowdyStudioDiagnosticMaxMessageBytes);
      if (match[2].matched) value.code = boundedCode(match[2].str());
      pending = std::move(value);
      continue;
    }

    if (pending && std::regex_match(line, match, arrow)) {
      const auto location = normalizeLocation(match[1].str(), defaultTarget);
      const auto lineStart = coordinate(match[2].str());
      const auto columnStart = coordinate(match[3].str());
      if (location && lineStart && columnStart) {
        CrowdyStudioDiagnostic diagnostic;
        diagnostic.target = location->target;
        diagnostic.path = location->path;
        diagnostic.line = *lineStart;
        diagnostic.column = *columnStart;
        if (match[4].matched) {
          diagnostic.endColumn = coordinate(match[4].str());
        }
        diagnostic.severity = pending->severity;
        diagnostic.message = pending->message;
        diagnostic.code = pending->code;
        diagnostic.source = CrowdyStudioDiagnosticSource::Rustc;
        append(std::move(diagnostic));
      }
      pending.reset();
      continue;
    }

    if (std::regex_match(line, match, compact)) {
      const auto location = normalizeLocation(match[1].str(), defaultTarget);
      const auto lineStart = coordinate(match[2].str());
      const auto columnStart = coordinate(match[3].str());
      if (!location || !lineStart || !columnStart) continue;
      CrowdyStudioDiagnostic diagnostic;
      diagnostic.target = location->target;
      diagnostic.path = location->path;
      diagnostic.line = *lineStart;
      diagnostic.column = *columnStart;
      diagnostic.severity = normalizeSeverity(match[4].str());
      diagnostic.message = truncateUtf8(
          match[6].str(), kCrowdyStudioDiagnosticMaxMessageBytes);
      if (match[5].matched) diagnostic.code = boundedCode(match[5].str());
      diagnostic.source = CrowdyStudioDiagnosticSource::Rustc;
      append(std::move(diagnostic));
    }
  }
  return diagnostics;
}

std::string crowdyStudioDiagnosticText(
    const CrowdyStudioDiagnostic& diagnostic) {
  return diagnostic.message;
}

std::vector<std::string> crowdyStudioDiagnosticTexts(
    std::span<const CrowdyStudioDiagnostic> diagnostics) {
  std::vector<std::string> result;
  result.reserve(diagnostics.size());
  for (const auto& diagnostic : diagnostics) {
    result.push_back(crowdyStudioDiagnosticText(diagnostic));
  }
  return result;
}

}  // namespace crowdy::studio
