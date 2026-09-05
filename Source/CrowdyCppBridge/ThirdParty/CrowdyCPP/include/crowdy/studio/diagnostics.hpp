#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "crowdy/studio/models.hpp"

namespace crowdy::studio {

enum class CrowdyStudioDiagnosticSeverity { Error, Warning, Info, Hint };

/**
 * Where a diagnostic came from, which is what an editor filters on.
 *
 * ModelLint is the server's `gameModelLint`, and it answers a different kind of question
 * from the other three. Those are about code the developer is editing. This is about
 * whether the app's game model hangs together at all -- a container naming a type nobody
 * defined, a function calling one that does not exist -- which has no file and no line and
 * which no amount of reading the open document would reveal.
 */
enum class CrowdyStudioDiagnosticSource {
  Rustc,
  LocalAdvisory,
  Runtime,
  ModelLint
};

inline constexpr std::string_view toString(
    CrowdyStudioDiagnosticSeverity value) {
  switch (value) {
    case CrowdyStudioDiagnosticSeverity::Error: return "error";
    case CrowdyStudioDiagnosticSeverity::Warning: return "warning";
    case CrowdyStudioDiagnosticSeverity::Info: return "info";
    case CrowdyStudioDiagnosticSeverity::Hint: return "hint";
  }
  return "";
}

inline constexpr std::string_view toString(
    CrowdyStudioDiagnosticSource value) {
  switch (value) {
    case CrowdyStudioDiagnosticSource::Rustc: return "rustc";
    case CrowdyStudioDiagnosticSource::LocalAdvisory:
      return "local-advisory";
    case CrowdyStudioDiagnosticSource::Runtime: return "runtime";
    case CrowdyStudioDiagnosticSource::ModelLint: return "model-lint";
  }
  return "";
}

/// One target-relative problem marker. Coordinates are one-based, matching
/// CrowdyJS and rustc. Engine editor adapters may convert them to zero-based
/// marker ranges at their own boundary.
struct CrowdyStudioDiagnostic {
  CrowdyStudioTarget target = CrowdyStudioTarget::Server;
  std::string path;
  std::uint32_t line = 1;
  std::uint32_t column = 1;
  std::optional<std::uint32_t> endLine;
  std::optional<std::uint32_t> endColumn;
  CrowdyStudioDiagnosticSeverity severity =
      CrowdyStudioDiagnosticSeverity::Info;
  std::string message;
  std::optional<std::string> code;
  CrowdyStudioDiagnosticSource source =
      CrowdyStudioDiagnosticSource::LocalAdvisory;

  bool operator==(const CrowdyStudioDiagnostic&) const = default;
};

inline constexpr std::size_t kCrowdyStudioDiagnosticMaxInputBytes =
    1024 * 1024;
inline constexpr std::size_t kCrowdyStudioDiagnosticMaxLines = 20'000;
inline constexpr std::size_t kCrowdyStudioDiagnosticMaxLineBytes = 16'384;
inline constexpr std::size_t kCrowdyStudioDiagnosticMaxCount = 256;
inline constexpr std::size_t kCrowdyStudioDiagnosticMaxMessageBytes = 2'048;
inline constexpr std::size_t kCrowdyStudioDiagnosticMaxCodeBytes = 64;
inline constexpr std::uint32_t kCrowdyStudioDiagnosticMaxCoordinate =
    1'000'000;

/**
 * Parse rustc human or one-message-per-line JSON output.
 *
 * The parser follows CrowdyJS's primary-span, severity, path, and de-duplication
 * behavior while enforcing the native agent output bounds above. Invalid JSON,
 * locations, coordinates, and UTF-8 are isolated to their line and never
 * throw. CRLF and lone CR are treated as line separators.
 */
std::vector<CrowdyStudioDiagnostic> parseRustcDiagnostics(
    std::string_view output, CrowdyStudioTarget defaultTarget);

/// Compatibility projection for integrations that still render diagnostics as
/// opaque strings. New code should consume CrowdyStudioDiagnostic directly.
std::string crowdyStudioDiagnosticText(
    const CrowdyStudioDiagnostic& diagnostic);
std::vector<std::string> crowdyStudioDiagnosticTexts(
    std::span<const CrowdyStudioDiagnostic> diagnostics);

}  // namespace crowdy::studio
