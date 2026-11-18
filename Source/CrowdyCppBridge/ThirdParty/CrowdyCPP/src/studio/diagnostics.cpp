#include "crowdy/studio/diagnostics.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <string>
#include <string_view>
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

// CrowdyJS reads these lines with regular expressions. They are read here with
// linear scanners, each giving exactly the match of the pattern above it: MSVC's
// std::regex recurses per character and throws error_stack on long lines.

bool isSpace(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

bool isDigit(char c) { return c >= '0' && c <= '9'; }

std::size_t skipSpaces(std::string_view line, std::size_t at) {
  while (at < line.size() && isSpace(line[at])) ++at;
  return at;
}

std::size_t skipDigits(std::string_view line, std::size_t at) {
  while (at < line.size() && isDigit(line[at])) ++at;
  return at;
}

/// One line, with what every scan of it shares: where its trailing spaces start and
/// where the next `]` is (asked for at rising positions only, so found once).
class LineScan {
 public:
  explicit LineScan(std::string_view line) : line_(line), end_(line.size()) {
    while (end_ > 0 && isSpace(line_[end_ - 1])) --end_;
  }

  std::string_view line() const { return line_; }
  /// One past the last character that is not a space.
  std::size_t contentEnd() const { return end_; }

  std::size_t nextBracket(std::size_t from) {
    // The last search, from `searchedFrom_`, found the first `]` there is at or after
    // it; that is also the first at or after `from` unless it lies before `from`.
    const bool known = searched_ && from >= searchedFrom_ &&
                       (bracket_ == std::string_view::npos || bracket_ >= from);
    if (!known) {
      bracket_ = line_.find(']', from);
      searchedFrom_ = from;
      searched_ = true;
    }
    return bracket_;
  }

 private:
  std::string_view line_;
  std::size_t end_;
  bool searched_ = false;
  std::size_t searchedFrom_ = 0;
  std::size_t bracket_ = std::string_view::npos;
};

struct SeverityText {
  std::string_view keyword;
  std::optional<std::string_view> code;
  std::string_view message;
};

/// `(error|warning|info|note)(?:\[([^\]]+)\])?:\s*(.+?)\s*$` from `at`.
std::optional<SeverityText> severityAt(LineScan& scan, std::size_t at) {
  const std::string_view line = scan.line();
  SeverityText text;
  for (const std::string_view keyword : {"error", "warning", "info", "note"}) {
    if (line.substr(at, keyword.size()) == keyword) {
      text.keyword = keyword;
      break;
    }
  }
  if (text.keyword.empty()) return std::nullopt;
  at += text.keyword.size();
  if (at < line.size() && line[at] == '[') {
    const std::size_t close = scan.nextBracket(at + 1);
    if (close == std::string_view::npos || close == at + 1) return std::nullopt;
    text.code = line.substr(at + 1, close - at - 1);
    at = close + 1;
  }
  if (at >= line.size() || line[at] != ':') return std::nullopt;
  ++at;
  if (at == line.size()) return std::nullopt;
  const std::size_t begin = skipSpaces(line, at);
  // Nothing but spaces: the lazy `.+?` still takes one, the last.
  text.message = begin >= scan.contentEnd() ? line.substr(line.size() - 1)
                                            : line.substr(begin, scan.contentEnd() - begin);
  return text;
}

/// `^\s*(error|warning|info|note)(?:\[([^\]]+)\])?:\s*(.+?)\s*$`
std::optional<SeverityText> headerOf(LineScan& scan) {
  return severityAt(scan, skipSpaces(scan.line(), 0));
}

struct ArrowLocation {
  std::string_view path;
  std::string_view line;
  std::string_view column;
  std::optional<std::string_view> endColumn;
};

/// `^\s*-->\s+(.+?):([0-9]+):([0-9]+)(?:-([0-9]+))?\s*$`
std::optional<ArrowLocation> arrowOf(const LineScan& scan) {
  const std::string_view line = scan.line();
  const std::size_t arrow = skipSpaces(line, 0);
  if (line.substr(arrow, 3) != "-->") return std::nullopt;
  const std::size_t afterArrow = arrow + 3;
  const std::size_t pathStart = skipSpaces(line, afterArrow);
  if (pathStart == afterArrow) return std::nullopt;
  // The tail is anchored at the end, so it starts at one place at most; read it backwards.
  const auto digitsBefore = [&](std::size_t end) {
    std::size_t begin = end;
    while (begin > pathStart && isDigit(line[begin - 1])) --begin;
    return begin;
  };
  ArrowLocation location;
  const std::size_t end = scan.contentEnd();
  std::size_t at = digitsBefore(end);
  if (at == end || at == pathStart) return std::nullopt;
  if (line[at - 1] == '-') {
    location.endColumn = line.substr(at, end - at);
    const std::size_t columnEnd = at - 1;
    at = digitsBefore(columnEnd);
    if (at == columnEnd) return std::nullopt;
    location.column = line.substr(at, columnEnd - at);
  } else {
    location.column = line.substr(at, end - at);
  }
  if (at == pathStart || line[at - 1] != ':') return std::nullopt;
  const std::size_t lineEnd = at - 1;
  at = digitsBefore(lineEnd);
  if (at == lineEnd || at == pathStart || line[at - 1] != ':') return std::nullopt;
  location.line = line.substr(at, lineEnd - at);
  const std::size_t colon = at - 1;
  if (colon > pathStart) {
    location.path = line.substr(pathStart, colon - pathStart);
  } else if (pathStart - afterArrow >= 2) {
    // `\s+` gives its last space to the path, which may not be empty.
    location.path = line.substr(pathStart - 1, 1);
  } else {
    return std::nullopt;
  }
  return location;
}

struct CompactDiagnostic {
  std::string_view path;
  std::string_view line;
  std::string_view column;
  SeverityText text;
};

/// `:([0-9]+):([0-9]+):\s*` and a severity, from the colon at `colon`.
std::optional<CompactDiagnostic> compactTail(LineScan& scan, std::size_t colon) {
  const std::string_view line = scan.line();
  CompactDiagnostic found;
  const std::size_t lineEnd = skipDigits(line, colon + 1);
  if (lineEnd == colon + 1 || lineEnd >= line.size() || line[lineEnd] != ':') return std::nullopt;
  found.line = line.substr(colon + 1, lineEnd - colon - 1);
  const std::size_t columnEnd = skipDigits(line, lineEnd + 1);
  if (columnEnd == lineEnd + 1 || columnEnd >= line.size() || line[columnEnd] != ':') {
    return std::nullopt;
  }
  found.column = line.substr(lineEnd + 1, columnEnd - lineEnd - 1);
  const auto text = severityAt(scan, skipSpaces(line, columnEnd + 1));
  if (!text) return std::nullopt;
  found.text = *text;
  return found;
}

/// `^\s*(.+?):([0-9]+):([0-9]+):\s*(error|warning|info|note)(?:\[([^\]]+)\])?:\s*(.+?)\s*$`
std::optional<CompactDiagnostic> compactOf(LineScan& scan) {
  const std::string_view line = scan.line();
  const std::size_t start = skipSpaces(line, 0);
  for (std::size_t colon = line.find(':', start + 1); colon != std::string_view::npos;
       colon = line.find(':', colon + 1)) {
    if (auto found = compactTail(scan, colon)) {
      found->path = line.substr(start, colon - start);
      return found;
    }
  }
  // `\s*` gives its last space to the path, which may not be empty.
  if (start > 0 && start < line.size() && line[start] == ':') {
    if (auto found = compactTail(scan, start)) {
      found->path = line.substr(start - 1, 1);
      return found;
    }
  }
  return std::nullopt;
}

struct TargetPath {
  CrowdyStudioTarget target = CrowdyStudioTarget::Server;
  std::string_view rest;
};

/// The leftmost match of `(?:^|/)(server|client)/(.+)$`, ignoring case.
std::optional<TargetPath> targetPathOf(std::string_view path) {
  const auto at = [&](std::size_t name) -> std::optional<TargetPath> {
    if (path.size() < name + 8 || path[name + 6] != '/') return std::nullopt;
    std::string word(path.substr(name, 6));
    for (char& c : word) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (word != "server" && word != "client") return std::nullopt;
    return TargetPath{word == "server" ? CrowdyStudioTarget::Server : CrowdyStudioTarget::Client,
                      path.substr(name + 7)};
  };
  if (auto found = at(0)) return found;
  for (std::size_t slash = path.find('/'); slash != std::string_view::npos;
       slash = path.find('/', slash + 1)) {
    if (auto found = at(slash + 1)) return found;
  }
  return std::nullopt;
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

  if (const auto targetMatch = targetPathOf(path)) {
    target = targetMatch->target;
    path = std::string(targetMatch->rest);
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

    LineScan scan(line);
    if (const auto header = headerOf(scan)) {
      PendingDiagnostic value;
      value.severity = normalizeSeverity(header->keyword);
      value.message = truncateUtf8(
          header->message, kCrowdyStudioDiagnosticMaxMessageBytes);
      if (header->code) value.code = boundedCode(*header->code);
      pending = std::move(value);
      continue;
    }

    if (pending) {
      if (const auto arrow = arrowOf(scan)) {
        const auto location = normalizeLocation(arrow->path, defaultTarget);
        const auto lineStart = coordinate(arrow->line);
        const auto columnStart = coordinate(arrow->column);
        if (location && lineStart && columnStart) {
          CrowdyStudioDiagnostic diagnostic;
          diagnostic.target = location->target;
          diagnostic.path = location->path;
          diagnostic.line = *lineStart;
          diagnostic.column = *columnStart;
          if (arrow->endColumn) {
            diagnostic.endColumn = coordinate(*arrow->endColumn);
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
    }

    if (const auto compact = compactOf(scan)) {
      const auto location = normalizeLocation(compact->path, defaultTarget);
      const auto lineStart = coordinate(compact->line);
      const auto columnStart = coordinate(compact->column);
      if (!location || !lineStart || !columnStart) continue;
      CrowdyStudioDiagnostic diagnostic;
      diagnostic.target = location->target;
      diagnostic.path = location->path;
      diagnostic.line = *lineStart;
      diagnostic.column = *columnStart;
      diagnostic.severity = normalizeSeverity(compact->text.keyword);
      diagnostic.message = truncateUtf8(
          compact->text.message, kCrowdyStudioDiagnosticMaxMessageBytes);
      if (compact->text.code) {
        diagnostic.code = boundedCode(*compact->text.code);
      }
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
