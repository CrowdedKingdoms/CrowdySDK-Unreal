#pragma once

#include <cstddef>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "crowdy/studio/diagnostics.hpp"

namespace crowdy::studio {

/**
 * Game-model lint, and the refusals a shipped game sees instead of it.
 *
 * TWO AUDIENCES, TWO MECHANISMS, and the split is forced by permissions rather than
 * chosen. `gameModelLint` is gated on `manage_apps`: an authoring context can ask whether
 * the whole app hangs together, and a client holding a player token cannot and should not
 * be able to. What the shipped client gets is the server refusing one operation, with a
 * code and the facts attached.
 *
 * WHY THE SECOND HALF EARNS ITS KEEP. The case this exists for is a client that called
 * `gameModelEnsureContainer` against a type nobody had defined. The call SUCCEEDED. The
 * container was written, nothing bound to it, and the only trace anywhere was the game's
 * own log line -- `[GameModel] InvokeAndApply: no container bound for entity F3B8B18E...`
 * -- which names a symptom and nothing about the cause. It was read as a permissions fault
 * for most of a day. The server refuses that call now and says why; this makes sure the
 * why reaches the developer rather than being swallowed into a generic warning.
 */

/// One finding as `gameModelLint` returns it.
struct CrowdyModelLintFinding {
  std::string code;
  CrowdyStudioDiagnosticSeverity severity =
      CrowdyStudioDiagnosticSeverity::Warning;
  std::string subjectKind;
  std::string subject;
  std::string message;
  std::optional<std::string> remedy;
  std::optional<std::size_t> count;

  bool operator==(const CrowdyModelLintFinding&) const = default;
};

/// The GraphQL document, kept beside the type it decodes into so the two cannot drift.
inline constexpr std::string_view kCrowdyModelLintQuery = R"(
  query CrowdyModelLint($appId: BigInt!) {
    gameModelLint(appId: $appId) {
      appId
      errorCount
      warningCount
      clean
      findings { code severity subjectKind subject message remedy count }
    }
  }
)";

/**
 * A stable label for the object a finding is about.
 *
 * Prefixed with the kind because subjects collide across kinds: an automation and a
 * function may both be called `on_join`, and a Problems list showing both as `on_join`
 * would be actively misleading about which one is broken.
 */
std::string modelLintSubjectPath(const CrowdyModelLintFinding& finding);

/**
 * Project findings onto the diagnostic type editors already render.
 *
 * THERE IS NO FILE OR LINE, and inventing one would be worse than admitting it. A finding
 * is about a function, a container type or the app, none of which is open in a text editor
 * -- the model is authored through the API. So the path carries the subject and the line is
 * 1: enough to group and label in a Problems list, without sending a click into a file that
 * has nothing to do with the problem.
 */
std::vector<CrowdyStudioDiagnostic> modelLintDiagnostics(
    std::span<const CrowdyModelLintFinding> findings,
    CrowdyStudioTarget target = CrowdyStudioTarget::Server);

/// A refusal the server attributes to the app's game model.
struct CrowdyModelRefusal {
  std::string code;
  std::string message;
  /// The object it is about, for deduplication and for the developer.
  std::string subject;

  bool operator==(const CrowdyModelRefusal&) const = default;
};

/// Whether an error code means "your game model is wrong", as opposed to a
/// transient or permissions failure. Keys off the code, never the message text.
bool isModelRefusalCode(std::string_view code);

/**
 * Deduplicating sink for model refusals.
 *
 * ONCE PER (code, subject), NOT PER OCCURRENCE, and that is not a nicety. These fire on
 * gameplay paths -- a bind can be attempted every frame, for every entity -- and a warning
 * printed at that rate is indistinguishable from noise. Warnings being ignorable is the
 * root cause of the incident this class exists because of, so reproducing it with better
 * wording would be the one clearly wrong outcome.
 *
 * Holds the set for the life of the object rather than expiring it: the point is to say a
 * thing once, and a TTL would only decide how often to repeat what has already been read.
 */
class CrowdyModelLintLog {
 public:
  /// Record a refusal. True when it was the first of its kind, which is also
  /// when the caller should log it.
  bool record(CrowdyModelRefusal refusal);

  /// Every distinct refusal so far, for an editor panel rather than a log tail.
  [[nodiscard]] std::span<const CrowdyModelRefusal> collected() const {
    return refusals_;
  }

  /// Forget everything, so a reconnect to a repaired app reports afresh.
  void reset();

 private:
  std::set<std::pair<std::string, std::string>> seen_;
  std::vector<CrowdyModelRefusal> refusals_;
};

}  // namespace crowdy::studio
