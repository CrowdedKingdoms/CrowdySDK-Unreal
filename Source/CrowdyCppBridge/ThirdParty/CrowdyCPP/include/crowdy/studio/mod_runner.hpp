#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
  /// The draft test runs, or the project is live.
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
  /// A transport or protocol failure, or a build that did not finish within `pollLimit`.
  Failed,
};

std::string_view toString(CrowdyStudioRunOutcome outcome);

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
  CrowdyStudioDeployment deployment = CrowdyStudioDeployment::Draft;
  /// Ask `crowdyStudioLiveModding` first (`draft_test` for a draft, `deploy_live` for
  /// live): the build, deploy and switch-on calls do not check the app's Allow Live
  /// Modding. A dock running the agent's action sets it.
  bool checkLiveModding = false;
  std::int64_t pollIntervalMs = 1'500;
  /// Polls a build may take before the run fails (about 10 minutes by default).
  int pollLimit = 400;
};

/// One target of a run, in the order run: the CLIENT half first.
struct CrowdyStudioRunTarget {
  CrowdyStudioTarget target = CrowdyStudioTarget::Server;
  /// The last step it reached.
  CrowdyStudioRunStep phase = CrowdyStudioRunStep::Building;
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
/// Callbacks run from the client's dispatcher (CrowdyClient::poll); build polls wait on
/// one timer thread of the runner's. Destroying the runner ends its runs without calling
/// back. CrowdyClient::createCrowdyStudioModRunner() makes one on the client's session.
class CrowdyStudioModRunner {
 public:
  using DoneCallback = std::function<void(CrowdyStudioRunResult)>;
  using ProgressCallback = std::function<void(const CrowdyStudioRunProgress&)>;

  /// `agent` is needed only for `checkLiveModding`.
  CrowdyStudioModRunner(std::shared_ptr<domains::ExecAPI> exec,
                        std::shared_ptr<domains::CrowdyStudioAPI> projects,
                        std::shared_ptr<domains::CrowdyStudioAgentAPI> agent,
                        std::shared_ptr<graphql::Dispatcher> dispatcher);
  ~CrowdyStudioModRunner();

  CrowdyStudioModRunner(const CrowdyStudioModRunner&) = delete;
  CrowdyStudioModRunner& operator=(const CrowdyStudioModRunner&) = delete;

  /// Start a run. `onDone` runs once; `onProgress` at each step. Throws
  /// std::invalid_argument for a request without a project or a pin.
  CrowdyStudioRunHandle run(CrowdyStudioRunRequest request, DoneCallback onDone,
                            ProgressCallback onProgress = {});

  struct Impl;

 private:
  std::shared_ptr<Impl> impl_;
};

}  // namespace crowdy::studio
