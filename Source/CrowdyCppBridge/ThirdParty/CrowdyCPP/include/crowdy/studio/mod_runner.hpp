#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "crowdy/graphql/auth_state.hpp"
#include "crowdy/graphql/dispatcher.hpp"
#include "crowdy/graphql/errors.hpp"
#include "crowdy/studio/diagnostics.hpp"
#include "crowdy/studio/models.hpp"
#include "crowdy/studio/runtime.hpp"

namespace crowdy::domains {
class CrowdyStudioAPI;
class CrowdyStudioAgentAPI;
class ExecAPI;
}  // namespace crowdy::domains

namespace crowdy::studio {

/// Where a run is, or how far a target got.
enum class CrowdyStudioRunStep {
  ReadingProject,
  CheckingLiveModding,
  Building,
  Deploying,
  Enabling,
  Attaching,
  Consenting,
  Done,
};

std::string_view toString(CrowdyStudioRunStep step);

/// How a run ended.
enum class CrowdyStudioRunOutcome {
  /// Every step went through and nothing blocks the mod: the draft test runs, or the
  /// project is live. A mod just switched on may start a moment later (`running`).
  Running,
  /// A target's build failed: its `buildLog` and `diagnostics` say why. Nothing after it
  /// was published.
  CompileFailed,
  /// The project is not at the expected revision (or a bound project's commit).
  RevisionConflict,
  /// `RATE_LIMITED`: the player already has a build queued or running.
  RateLimited,
  /// A build answered `INTERNAL_SERVER_ERROR` with HTTP 503: the instance's build queue is
  /// full, or it cannot build now. Retryable.
  QueueFull,
  /// The API refused a step with a GraphQL error (a permission, the grid's owner, the
  /// live-modding check): `error` carries its code.
  Refused,
  /// The handle was cancelled; what already happened stays, as `targets` reports it.
  Cancelled,
  /// A transport or protocol failure that reading once more did not clear, a build that
  /// did not finish within `pollLimit`, or an exception thrown inside the run (an
  /// `onProgress` that threw included).
  Failed,
  /// Every step went through, but the switch-on answer says the mod is blocked: the
  /// target's `blocked` and `message` say why (a switch of the app's kill ladder, the
  /// app's own switch, its budget, or the owner's access). Nothing runs or is served until
  /// that clears; then both halves are the ones this run deployed, with nothing to repeat.
  Blocked,
};

std::string_view toString(CrowdyStudioRunOutcome outcome);

/// What the platform holds for a target when a run ends, whatever the outcome.
enum class CrowdyStudioRunTargetState {
  /// Nothing was submitted.
  NotStarted,
  /// Submitted, and the run ended before the build settled; the build may still finish.
  Building,
  /// The build failed: `buildLog` and `diagnostics` say why.
  BuildFailed,
  /// Built: a SERVER half not deployed, or a CLIENT half not attached.
  Built,
  /// SERVER: the build is the mod's version, and this run did not switch it on (a mod that
  /// was on already restarts on it).
  Deployed,
  /// SERVER: deployed and switched on; `running` and `blocked` say whether it runs.
  SwitchedOn,
  /// CLIENT: attached to `modName` without its author's consent, so nobody is served it.
  Attached,
  /// CLIENT: attached and consented to as its author: served to its author while the mod
  /// is on and unblocked.
  Consented,
};

std::string_view toString(CrowdyStudioRunTargetState state);

/// The most of a build's log a run keeps: its last 64 KiB, as the API serves it.
inline constexpr std::size_t kCrowdyStudioRunLogBytes = 64 * 1024;

struct CrowdyStudioRunRequest {
  CrowdyStudioProjectScope scope;
  std::string projectId;
  /// The revision to build (`project.revision.id`); a project that moved is refused.
  std::string expectedRevisionId;
  /// A GitHub-bound project's mirror commit to build (`project.github->sha`), checked
  /// instead of, or as well as, the revision. One of the two is required.
  std::optional<std::string> expectedGithubSha;
  /// The SERVER module name the player saw (`metadata.serverModuleName`): a project whose
  /// SERVER module is named otherwise now, or that has none, is a RevisionConflict.
  std::optional<std::string> expectedServerModuleName;
  /// The CLIENT module name the player saw (`metadata.clientModuleName`), checked likewise.
  std::optional<std::string> expectedClientModuleName;
  CrowdyStudioDeployment deployment = CrowdyStudioDeployment::Draft;
  /// Ask `crowdyStudioLiveModding` first (`draft_test` for a draft, `deploy_live` for
  /// live): the build, deploy and switch-on calls do not check the app's Allow Live
  /// Modding. A dock running the agent's action sets it.
  bool checkLiveModding = false;
  /// The wait between build polls, and before a read is read once more.
  std::int64_t pollIntervalMs = 1'500;
  /// Polls a build may take before the run fails (about 10 minutes by default). A dropped
  /// status read (no answer, an HTTP 5xx or 429 that is not the API's answer, or
  /// `PLATFORM_BUSY`) is a missed poll: it counts toward the limit, and polling goes on.
  int pollLimit = 400;
};

/// One target of a run, in the order run: the CLIENT half first.
struct CrowdyStudioRunTarget {
  CrowdyStudioTarget target = CrowdyStudioTarget::Server;
  /// The last step it reached.
  CrowdyStudioRunStep phase = CrowdyStudioRunStep::Building;
  /// What the platform holds for it when the run ends.
  CrowdyStudioRunTargetState state = CrowdyStudioRunTargetState::NotStarted;
  std::string moduleName;
  /// The mod it runs as (SERVER) or rides (CLIENT).
  std::string modName;
  std::string buildId;
  /// `queued`, `building`, `succeeded` or `failed`.
  std::string compileStatus;
  /// The build's log, at most its last kCrowdyStudioRunLogBytes.
  std::string buildLog;
  std::vector<CrowdyStudioDiagnostic> diagnostics;
  /// What the grid serves now: the mod's digest, or the CLIENT half's.
  std::string digest;
  /// The mod's version, or the CLIENT half's `clientVersion`.
  std::int64_t version = 0;
  /// The CLIENT half's capability hash, which its author consented to.
  std::string capabilityHash;
  /// The id of the mod it runs as (SERVER) or rides (CLIENT), once an answer named it.
  std::string modId;
  /// That mod's switch, from the last answer that carried it: the switch-on's, or the
  /// listing's for a CLIENT-only project's mod that was on already.
  std::optional<bool> enabled;
  /// Whether an instance of that mod ran when that answer was given.
  std::optional<bool> running;
  /// Why that mod, switched on, will not run: a switch of the app's kill ladder, the app's
  /// own switch, its budget, or the owner's access. Unset when nothing blocks it.
  std::optional<std::string> blocked;
  std::string message;
};

struct CrowdyStudioRunResult {
  CrowdyStudioRunOutcome outcome = CrowdyStudioRunOutcome::Failed;
  CrowdyStudioDeployment deployment = CrowdyStudioDeployment::Draft;
  std::string projectId;
  /// The revision read and built; empty when the read failed.
  std::string projectRevisionId;
  std::optional<std::string> githubSha;
  std::vector<CrowdyStudioRunTarget> targets;
  std::string message;
  /// The GraphQL error that ended the run, when one did.
  std::optional<graphql::GraphQLErrorDetail> error;

  bool ok() const { return outcome == CrowdyStudioRunOutcome::Running; }
};

struct CrowdyStudioRunProgress {
  CrowdyStudioRunStep step = CrowdyStudioRunStep::ReadingProject;
  std::optional<CrowdyStudioTarget> target;
  std::string message;
};

/// One run. Copies share it; an empty handle refers to none.
class CrowdyStudioRunHandle {
 public:
  CrowdyStudioRunHandle() = default;
  /// Stop at the next step (within one poll interval); the run ends Cancelled. Safe from
  /// any thread.
  void cancel() const;
  /// The run has called back.
  bool done() const;
  explicit operator bool() const { return state_ != nullptr; }

  struct State;

 private:
  friend class CrowdyStudioModRunner;
  explicit CrowdyStudioRunHandle(std::shared_ptr<State> state) : state_(std::move(state)) {}
  std::shared_ptr<State> state_;
};

/// Crowdy Studio's draft test and live deploy without blocking: CrowdyStudioModRuntime's
/// sequence over the async calls, for a game thread. A run reads the project and refuses
/// one that is not at the expected revision, then builds exactly that read's files: the
/// CLIENT half first (so a failed compile publishes nothing), then the SERVER half, which
/// it deploys and switches on (a CLIENT-only project rides a mod of its CLIENT module's
/// name, deployed from the mod starter and switched on first), then attaches the CLIENT
/// half and consents to it as its author, so the author's own client is served it.
///
/// A dropped read (no answer, an HTTP 5xx or 429 that is not the API's answer, or
/// `PLATFORM_BUSY`) does not end a run: a build's status is polled again, counting toward
/// `pollLimit`, and the project, live-modding, mod-listing and mod-starter reads are read
/// once more after `pollIntervalMs` (or the wait a busy refusal asked for). The build,
/// deploy, switch-on, attach and consent calls are never repeated.
///
/// Callbacks run from the client's dispatcher (CrowdyClient::poll); build polls wait on
/// one timer thread of the runner's. Every run reaches its `onDone` once, an exception
/// thrown inside it included (as Failed). Destroying the runner ends its runs without
/// calling back. CrowdyClient::createCrowdyStudioModRunner() makes one on the client's
/// session, and createCrowdyStudioModRunner(appToken) one whose requests carry an app
/// token of their own.
class CrowdyStudioModRunner {
 public:
  using DoneCallback = std::function<void(CrowdyStudioRunResult)>;
  using ProgressCallback = std::function<void(const CrowdyStudioRunProgress&)>;

  /// `agent` is needed only for `checkLiveModding`.
  CrowdyStudioModRunner(std::shared_ptr<domains::ExecAPI> exec,
                        std::shared_ptr<domains::CrowdyStudioAPI> projects,
                        std::shared_ptr<domains::CrowdyStudioAgentAPI> agent,
                        std::shared_ptr<graphql::Dispatcher> dispatcher);
  /// A runner whose requests carry a token of their own: `appTokenAuth` is the AuthState
  /// of the client `exec`, `projects` and `agent` were made over, which setAppToken writes.
  CrowdyStudioModRunner(std::shared_ptr<domains::ExecAPI> exec,
                        std::shared_ptr<domains::CrowdyStudioAPI> projects,
                        std::shared_ptr<domains::CrowdyStudioAgentAPI> agent,
                        std::shared_ptr<graphql::Dispatcher> dispatcher,
                        std::shared_ptr<graphql::AuthState> appTokenAuth);
  ~CrowdyStudioModRunner();

  CrowdyStudioModRunner(const CrowdyStudioModRunner&) = delete;
  CrowdyStudioModRunner& operator=(const CrowdyStudioModRunner&) = delete;

  /// Start a run. `onDone` runs once; `onProgress` at each step. Throws
  /// std::invalid_argument for a request without a project or a pin, before anything is
  /// sent; nothing else it does throws out of run() or CrowdyClient::poll().
  CrowdyStudioRunHandle run(CrowdyStudioRunRequest request, DoneCallback onDone,
                            ProgressCallback onProgress = {});

  /// The app token this runner's requests carry from now on, for a runner made with a
  /// token of its own (CrowdyClient::createCrowdyStudioModRunner(appToken)): call it when
  /// the app token rotates. A request already sent keeps the token it went with. Safe from
  /// any thread. Throws std::logic_error on a runner that shares its client's session, and
  /// std::invalid_argument for an empty token.
  void setAppToken(std::string token);

  struct Impl;

 private:
  std::shared_ptr<Impl> impl_;
};

}  // namespace crowdy::studio
