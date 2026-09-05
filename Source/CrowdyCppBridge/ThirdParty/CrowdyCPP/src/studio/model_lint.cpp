#include "crowdy/studio/model_lint.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace crowdy::studio {
namespace {

/// Codes the server uses for "your game model is wrong". Kept as a small sorted
/// array rather than a set: it is looked up on a refusal path, not a hot one, and
/// a literal list is easier to read against the server's own enum.
constexpr std::array<std::string_view, 2> kModelRefusalCodes{
    "CONTAINER_TYPE_UNDEFINED",
    "OBJECT_QUARANTINED",
};

}  // namespace

bool isModelRefusalCode(std::string_view code) {
  return std::find(kModelRefusalCodes.begin(), kModelRefusalCodes.end(),
                   code) != kModelRefusalCodes.end();
}

std::string modelLintSubjectPath(const CrowdyModelLintFinding& finding) {
  return finding.subjectKind + "/" + finding.subject;
}

std::vector<CrowdyStudioDiagnostic> modelLintDiagnostics(
    std::span<const CrowdyModelLintFinding> findings,
    CrowdyStudioTarget target) {
  std::vector<CrowdyStudioDiagnostic> out;
  out.reserve(findings.size());
  for (const auto& finding : findings) {
    CrowdyStudioDiagnostic diagnostic;
    diagnostic.target = target;
    diagnostic.path = modelLintSubjectPath(finding);
    diagnostic.line = 1;
    diagnostic.column = 1;
    diagnostic.severity = finding.severity;
    // The remedy is joined into the message rather than dropped: a Problems list
    // shows one line per entry and has nowhere else to put it, and a finding the
    // developer cannot act on from where they are reading it is the failure this
    // whole surface exists to fix.
    diagnostic.message =
        finding.remedy && !finding.remedy->empty()
            ? finding.message + " \xE2\x80\x94 " + *finding.remedy
            : finding.message;
    diagnostic.code = finding.code;
    diagnostic.source = CrowdyStudioDiagnosticSource::ModelLint;
    out.push_back(std::move(diagnostic));
  }
  return out;
}

bool CrowdyModelLintLog::record(CrowdyModelRefusal refusal) {
  auto key = std::pair{refusal.code, refusal.subject};
  if (!seen_.insert(std::move(key)).second) return false;
  refusals_.push_back(std::move(refusal));
  return true;
}

void CrowdyModelLintLog::reset() {
  seen_.clear();
  refusals_.clear();
}

}  // namespace crowdy::studio
