#include "crowdy/studio/mod_runner.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "crowdy/domains/crowdy_studio.hpp"
#include "crowdy/domains/crowdy_studio_agent.hpp"
#include "crowdy/domains/exec.hpp"

namespace crowdy::studio {

std::string_view toString(CrowdyStudioRunStep step) {
  switch (step) {
    case CrowdyStudioRunStep::ReadingProject: return "ReadingProject";
    case CrowdyStudioRunStep::CheckingLiveModding: return "CheckingLiveModding";
    case CrowdyStudioRunStep::Building: return "Building";
    case CrowdyStudioRunStep::Deploying: return "Deploying";
    case CrowdyStudioRunStep::Enabling: return "Enabling";
    case CrowdyStudioRunStep::Attaching: return "Attaching";
    case CrowdyStudioRunStep::Consenting: return "Consenting";
    case CrowdyStudioRunStep::Done: return "Done";
  }
  return "";
}

std::string_view toString(CrowdyStudioRunOutcome outcome) {
  switch (outcome) {
    case CrowdyStudioRunOutcome::Running: return "Running";
    case CrowdyStudioRunOutcome::CompileFailed: return "CompileFailed";
    case CrowdyStudioRunOutcome::RevisionConflict: return "RevisionConflict";
    case CrowdyStudioRunOutcome::RateLimited: return "RateLimited";
    case CrowdyStudioRunOutcome::QueueFull: return "QueueFull";
    case CrowdyStudioRunOutcome::Refused: return "Refused";
    case CrowdyStudioRunOutcome::Cancelled: return "Cancelled";
    case CrowdyStudioRunOutcome::Failed: return "Failed";
  }
  return "";
}

struct CrowdyStudioRunHandle::State {
  std::atomic<bool> cancelled{false};
  std::atomic<bool> done{false};
};

void CrowdyStudioRunHandle::cancel() const {
  if (state_) state_->cancelled.store(true, std::memory_order_release);
}

bool CrowdyStudioRunHandle::done() const {
  return state_ && state_->done.load(std::memory_order_acquire);
}

namespace mod_runner_detail {

using Clock = std::chrono::steady_clock;

/// One thread that posts each job to the dispatcher when it falls due.
class Timer : public std::enable_shared_from_this<Timer> {
 public:
  explicit Timer(std::shared_ptr<graphql::Dispatcher> dispatcher)
      : dispatcher_(std::move(dispatcher)) {}

  void after(std::int64_t delayMs, std::function<void()> job) {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    due_.emplace(Clock::now() + std::chrono::milliseconds(std::max<std::int64_t>(0, delayMs)),
                 std::move(job));
    if (!worker_.joinable()) {
      worker_ = std::thread([self = shared_from_this()] { self->loop(); });
    }
    wake_.notify_all();
  }

  void stop() {
    std::thread worker;
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      due_.clear();
      wake_.notify_all();
      worker = std::move(worker_);
    }
    if (!worker.joinable()) return;
    if (worker.get_id() == std::this_thread::get_id()) {
      worker.detach();
    } else {
      worker.join();
    }
  }

 private:
  void loop() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
      if (due_.empty()) {
        wake_.wait(lock);
        continue;
      }
      const auto next = due_.begin()->first;
      if (Clock::now() < next) {
        wake_.wait_until(lock, next);
        continue;
      }
      auto job = std::move(due_.begin()->second);
      due_.erase(due_.begin());
      lock.unlock();
      dispatcher_->post(std::move(job));
      lock.lock();
    }
  }

  const std::shared_ptr<graphql::Dispatcher> dispatcher_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::multimap<Clock::time_point, std::function<void()>> due_;
  bool stopping_ = false;
  std::thread worker_;
};

class Run;

}  // namespace mod_runner_detail

struct CrowdyStudioModRunner::Impl {
  std::shared_ptr<domains::ExecAPI> exec;
  std::shared_ptr<domains::CrowdyStudioAPI> projects;
  std::shared_ptr<domains::CrowdyStudioAgentAPI> agent;
  std::shared_ptr<graphql::Dispatcher> dispatcher;
  std::shared_ptr<mod_runner_detail::Timer> timer;
  std::mutex mutex;
  std::map<std::uint64_t, std::shared_ptr<mod_runner_detail::Run>> runs;
  std::uint64_t nextRun = 0;

  void forget(std::uint64_t id) {
    std::lock_guard lock(mutex);
    runs.erase(id);
  }
};

namespace mod_runner_detail {

using Outcome = CrowdyStudioRunOutcome;
using Step = CrowdyStudioRunStep;

/// The last kCrowdyStudioRunLogBytes of a build's log, cut at a character.
std::string logTail(std::string log) {
  if (log.size() <= kCrowdyStudioRunLogBytes) return log;
  std::size_t start = log.size() - kCrowdyStudioRunLogBytes;
  while (start < log.size() && (static_cast<unsigned char>(log[start]) & 0xC0) == 0x80) ++start;
  return log.substr(start);
}

std::vector<CrowdyStudioDiagnostic> diagnosticsOf(const std::string& log,
                                                  CrowdyStudioTarget target) {
  try {
    return parseRustcDiagnostics(log, target);
  } catch (const std::exception&) {
    return {};
  }
}

class Run : public std::enable_shared_from_this<Run> {
  /// A callback that reaches this run only while it is unfinished and not cancelled.
  /// (First in the class: the members below deduce its type.)
  template <typename Fn>
  auto guard(Fn fn) {
    return [weak = weak_from_this(), fn = std::move(fn)](auto&&... args) mutable {
      auto self = weak.lock();
      if (!self || self->finished_) return;
      if (self->state_->cancelled.load(std::memory_order_acquire)) {
        self->finish(Outcome::Cancelled, "The run was cancelled");
        return;
      }
      fn(*self, std::forward<decltype(args)>(args)...);
    };
  }

 public:
  Run(std::weak_ptr<CrowdyStudioModRunner::Impl> impl, std::uint64_t id,
      CrowdyStudioRunRequest request, CrowdyStudioModRunner::DoneCallback done,
      CrowdyStudioModRunner::ProgressCallback progress,
      std::shared_ptr<CrowdyStudioRunHandle::State> state)
      : impl_(std::move(impl)),
        id_(id),
        request_(std::move(request)),
        done_(std::move(done)),
        progress_(std::move(progress)),
        state_(std::move(state)) {
    result_.deployment = request_.deployment;
    result_.projectId = request_.projectId;
  }

  void start() {
    auto impl = impl_.lock();
    if (!impl) return;
    step(Step::ReadingProject, std::nullopt, "Reading project " + request_.projectId);
    impl->projects->getProjectAsync(
        request_.scope, request_.projectId,
        guard([](Run& run, graphql::GraphQLOutcome outcome, CrowdyStudioProject project) {
          run.onProject(std::move(outcome), std::move(project));
        }));
  }

  /// The runner is going away: end without calling back.
  void abandon() {
    finished_ = true;
    done_ = nullptr;
    progress_ = nullptr;
    state_->cancelled.store(true, std::memory_order_release);
  }

 private:
  void onProject(graphql::GraphQLOutcome outcome, CrowdyStudioProject project) {
    if (!outcome.ok()) return fail(outcome, "Reading the project failed", false);
    project_ = std::move(project);
    result_.projectRevisionId = project_.revision.id;
    if (project_.github) result_.githubSha = project_.github->sha;
    if (request_.expectedGithubSha &&
        (!project_.github || project_.github->sha != request_.expectedGithubSha)) {
      return finish(Outcome::RevisionConflict,
                    "The project's repository is not at commit " + *request_.expectedGithubSha);
    }
    if (!request_.expectedRevisionId.empty() &&
        project_.revision.id != request_.expectedRevisionId) {
      return finish(Outcome::RevisionConflict, "The project is at revision " + project_.revision.id +
                                                   ", not " + request_.expectedRevisionId);
    }
    const auto targets = projectTargets(project_.kind);
    hasServer_ = std::find(targets.begin(), targets.end(), CrowdyStudioTarget::Server) != targets.end();
    hasClient_ = std::find(targets.begin(), targets.end(), CrowdyStudioTarget::Client) != targets.end();
    const auto named = [](const std::optional<std::string>& name) {
      return name ? *name : std::string();
    };
    serverName_ = named(project_.metadata.serverModuleName);
    clientName_ = named(project_.metadata.clientModuleName);
    modName_ = hasServer_ ? serverName_ : clientName_;
    if ((hasServer_ && serverName_.empty()) || (hasClient_ && clientName_.empty())) {
      return finish(Outcome::Failed, "Crowdy Studio target module name is required");
    }
    if (!crowdyStudioIsModName(modName_)) {
      return finish(Outcome::Failed,
                    hasServer_ ? "The server module name '" + modName_ +
                                     "' must be 1-48 lowercase letters, digits, - or _ to run as a mod"
                               : "The CLIENT module name '" + modName_ +
                                     "' names the mod its CLIENT half rides, so it must be 1-48 "
                                     "lowercase letters, digits, - or _");
    }
    if (hasClient_) {
      result_.targets.push_back(targetEntry(CrowdyStudioTarget::Client, clientName_));
    }
    if (hasServer_) {
      result_.targets.push_back(targetEntry(CrowdyStudioTarget::Server, serverName_));
    }
    for (auto& entry : result_.targets) {
      if (filesOf(entry.target).empty()) {
        entry.message = "Crowdy Studio target has no project files";
        return finish(Outcome::Failed, entry.message);
      }
    }
    if (hasClient_) {
      for (const auto& file : filesOf(CrowdyStudioTarget::Client)) {
        if (file.path != "Cargo.toml" || !crowdyStudioIsLegacyClientCrate(file.content)) continue;
        CrowdyStudioRunTarget& client = target(CrowdyStudioTarget::Client);
        client.compileStatus = "failed";
        client.buildLog = std::string(kCrowdyStudioLegacyClientCrate);
        client.message = clientName_ + " is a legacy player compute crate; a CLIENT half builds on "
                                       "crowdy-client-sdk";
        return finish(Outcome::CompileFailed, client.message);
      }
    }
    if (request_.checkLiveModding) return checkLiveModding();
    buildFirst();
  }

  void checkLiveModding() {
    auto impl = impl_.lock();
    if (!impl) return;
    if (!impl->agent) {
      return finish(Outcome::Failed, "checkLiveModding needs the runner's CrowdyStudioAgentAPI");
    }
    const domains::CrowdyStudioLiveModdingAction action =
        request_.deployment == CrowdyStudioDeployment::Draft
            ? domains::CrowdyStudioLiveModdingAction::DraftTest
            : domains::CrowdyStudioLiveModdingAction::DeployLive;
    step(Step::CheckingLiveModding, std::nullopt,
         "Asking whether this player may " + std::string(domains::toString(action)));
    impl->agent->liveModdingAsync(
        request_.scope.appId, action, guard([action](Run& run, graphql::GraphQLOutcome outcome) {
          if (!outcome.ok()) return run.fail(outcome, "The live-modding check refused", false);
          if (!outcome.data.asBool()) {
            return run.finish(Outcome::Refused, "The app's Allow Live Modding refused " +
                                                    std::string(domains::toString(action)));
          }
          run.buildFirst();
        }));
  }

  void buildFirst() {
    if (hasClient_) {
      build(CrowdyStudioTarget::Client, [](Run& run) {
        if (run.hasServer_) {
          run.build(CrowdyStudioTarget::Server, [](Run& next) { next.deployServer(); });
        } else {
          run.findRider();
        }
      });
    } else {
      build(CrowdyStudioTarget::Server, [](Run& run) { run.deployServer(); });
    }
  }

  void build(CrowdyStudioTarget which, std::function<void(Run&)> then) {
    auto impl = impl_.lock();
    if (!impl) return;
    CrowdyStudioRunTarget& entry = target(which);
    entry.phase = Step::Building;
    step(Step::Building, which, "Submitting " + entry.moduleName);
    const domains::ExecCrate crate = crowdyStudioModCrate(entry.moduleName, filesOf(which));
    auto queued = guard([which, then = std::move(then)](Run& run, graphql::GraphQLOutcome outcome) mutable {
      CrowdyStudioRunTarget& built = run.target(which);
      if (!outcome.ok()) {
        built.message = "The build was refused";
        return run.fail(outcome, built.moduleName + " was not built", true);
      }
      built.buildId = outcome.data["buildId"].asString();
      built.compileStatus = outcome.data["status"].asString();
      if (built.buildId.empty()) {
        return run.finish(Outcome::Failed, built.moduleName + "'s build returned no build id");
      }
      run.poll(built.buildId, 0,
               [which](Run& done, const graphql::Json& status) { done.recordBuild(which, status); },
               [which, then = std::move(then)](Run& done) { then(done); },
               [which](Run& done) {
                 CrowdyStudioRunTarget& failed = done.target(which);
                 failed.message = failed.moduleName + " failed to compile";
                 done.finish(Outcome::CompileFailed, failed.message);
               });
    });
    // The saved revision these files are, so the mod and CLIENT half report it.
    const domains::ExecStudioSource studio{project_.projectId, project_.revision.id};
    if (which == CrowdyStudioTarget::Client) {
      impl->exec->modClientBuildAsync(request_.scope.appId, crate, studio, std::move(queued));
    } else {
      impl->exec->modBuildAsync(request_.scope.appId, crate, studio, std::move(queued));
    }
  }

  /// Poll a build until it succeeds or fails, `record`ing each answer.
  void poll(std::string buildId, int attempt, std::function<void(Run&, const graphql::Json&)> record,
            std::function<void(Run&)> succeeded, std::function<void(Run&)> failed) {
    auto impl = impl_.lock();
    if (!impl) return;
    auto wait = guard([buildId, attempt, record = std::move(record), succeeded = std::move(succeeded),
                       failed = std::move(failed)](Run& run) mutable {
      auto owner = run.impl_.lock();
      if (!owner) return;
      owner->exec->modBuildStatusAsync(
          run.request_.scope.appId, buildId,
          run.guard([buildId, attempt, record = std::move(record), succeeded = std::move(succeeded),
                     failed = std::move(failed)](Run& next, graphql::GraphQLOutcome outcome) mutable {
            if (!outcome.ok()) return next.fail(outcome, "Reading build " + buildId + " failed", false);
            record(next, outcome.data);
            const std::string status = outcome.data["status"].asString();
            if (status == "succeeded") return succeeded(next);
            if (status == "failed") return failed(next);
            if (attempt + 1 >= next.request_.pollLimit) {
              return next.finish(Outcome::Failed, "Build " + buildId + " did not finish within " +
                                                      std::to_string(next.request_.pollLimit) + " polls");
            }
            next.poll(buildId, attempt + 1, std::move(record), std::move(succeeded), std::move(failed));
          }));
    });
    impl->timer->after(request_.pollIntervalMs, std::move(wait));
  }

  void recordBuild(CrowdyStudioTarget which, const graphql::Json& status) {
    CrowdyStudioRunTarget& entry = target(which);
    entry.compileStatus = status["status"].asString();
    if (status["log"].isString()) {
      entry.buildLog = logTail(status["log"].asString());
      entry.diagnostics = diagnosticsOf(entry.buildLog, which);
    }
  }

  void deployServer() {
    auto impl = impl_.lock();
    if (!impl) return;
    CrowdyStudioRunTarget& server = target(CrowdyStudioTarget::Server);
    server.phase = Step::Deploying;
    step(Step::Deploying, CrowdyStudioTarget::Server, "Deploying " + serverName_);
    impl->exec->modDeployAsync(
        request_.scope.appId, request_.scope.gridId, serverName_, server.buildId,
        guard([](Run& run, graphql::GraphQLOutcome outcome) {
          if (!outcome.ok()) return run.fail(outcome, "Deploying " + run.serverName_ + " failed", false);
          run.recordMod(outcome.data);
          run.enableServer();
        }));
  }

  void enableServer() {
    auto impl = impl_.lock();
    if (!impl) return;
    target(CrowdyStudioTarget::Server).phase = Step::Enabling;
    step(Step::Enabling, CrowdyStudioTarget::Server, "Enabling " + serverName_);
    impl->exec->modSetEnabledAsync(
        request_.scope.appId, request_.scope.gridId, serverName_, true,
        guard([](Run& run, graphql::GraphQLOutcome outcome) {
          if (!outcome.ok()) return run.fail(outcome, "Enabling " + run.serverName_ + " failed", false);
          run.recordMod(outcome.data);
          if (run.hasClient_) return run.attachClient();
          run.succeed();
        }));
  }

  void recordMod(const graphql::Json& mod) {
    CrowdyStudioRunTarget& server = target(CrowdyStudioTarget::Server);
    if (mod["digest"].isString()) server.digest = mod["digest"].asString();
    if (mod["version"].isNumber()) server.version = mod["version"].asInt64();
  }

  /// A CLIENT-only project's CLIENT half rides the mod named for its CLIENT module: with
  /// none of the player's on the grid, the mod starter is deployed under it first, and the
  /// mod is switched on, since only a running mod's CLIENT half is served.
  void findRider() {
    auto impl = impl_.lock();
    if (!impl) return;
    step(Step::Deploying, CrowdyStudioTarget::Client, "Looking for mod '" + modName_ + "' on the grid");
    impl->exec->myModsAsync(
        request_.scope.appId, guard([](Run& run, graphql::GraphQLOutcome outcome) {
          if (!outcome.ok()) return run.fail(outcome, "Listing the player's mods failed", false);
          std::optional<bool> enabled;
          outcome.data.forEach([&](const graphql::Json& mod) {
            if (mod["gridId"].asBigIntString() == run.request_.scope.gridId &&
                mod["name"].asString() == run.modName_) {
              enabled = mod["enabled"].asBool();
            }
          });
          if (!enabled) return run.deployStarter();
          if (!*enabled) return run.enableRider();
          run.attachClient();
        }));
  }

  void deployStarter() {
    auto impl = impl_.lock();
    if (!impl) return;
    step(Step::Deploying, CrowdyStudioTarget::Client, "Deploying the mod starter as " + modName_);
    impl->exec->modStarterAsync(
        request_.scope.appId, guard([](Run& run, graphql::GraphQLOutcome outcome) {
          if (!outcome.ok()) return run.fail(outcome, "Reading the mod starter failed", false);
          std::vector<CrowdyStudioProjectFile> files;
          for (const auto& file : crowdyStudioModStarterOf(outcome.data).files) {
            CrowdyStudioProjectFile project;
            project.target = CrowdyStudioTarget::Server;
            project.path = file.path;
            project.content = file.content;
            files.push_back(std::move(project));
          }
          run.buildStarter(crowdyStudioModCrate(run.modName_, files));
        }));
  }

  void buildStarter(domains::ExecCrate crate) {
    auto impl = impl_.lock();
    if (!impl) return;
    impl->exec->modBuildAsync(
        request_.scope.appId, crate, guard([](Run& run, graphql::GraphQLOutcome outcome) {
          if (!outcome.ok()) return run.fail(outcome, "The mod starter was not built", true);
          const std::string buildId = outcome.data["buildId"].asString();
          if (buildId.empty()) return run.finish(Outcome::Failed, "The mod starter's build returned no build id");
          run.poll(
              buildId, 0, [](Run&, const graphql::Json&) {},
              [buildId](Run& built) { built.deployRider(buildId); },
              [](Run& failed) {
                failed.finish(Outcome::Failed, "The mod starter did not build, so the CLIENT half has no mod '" +
                                                   failed.modName_ + "' to ride");
              });
        }));
  }

  void deployRider(const std::string& buildId) {
    auto impl = impl_.lock();
    if (!impl) return;
    impl->exec->modDeployAsync(request_.scope.appId, request_.scope.gridId, modName_, buildId,
                               guard([](Run& run, graphql::GraphQLOutcome outcome) {
                                 if (!outcome.ok()) {
                                   return run.fail(outcome, "Deploying the mod starter failed", false);
                                 }
                                 run.enableRider();
                               }));
  }

  void enableRider() {
    auto impl = impl_.lock();
    if (!impl) return;
    step(Step::Enabling, CrowdyStudioTarget::Client, "Enabling " + modName_);
    impl->exec->modSetEnabledAsync(request_.scope.appId, request_.scope.gridId, modName_, true,
                                   guard([](Run& run, graphql::GraphQLOutcome outcome) {
                                     if (!outcome.ok()) {
                                       return run.fail(outcome, "Enabling " + run.modName_ + " failed", false);
                                     }
                                     run.attachClient();
                                   }));
  }

  void attachClient() {
    auto impl = impl_.lock();
    if (!impl) return;
    CrowdyStudioRunTarget& client = target(CrowdyStudioTarget::Client);
    client.phase = Step::Attaching;
    step(Step::Attaching, CrowdyStudioTarget::Client, "Attaching " + clientName_ + " to mod '" + modName_ + "'");
    impl->exec->modClientDeployAsync(
        request_.scope.appId, request_.scope.gridId, modName_, client.buildId,
        guard([](Run& run, graphql::GraphQLOutcome outcome) {
          if (!outcome.ok()) return run.fail(outcome, "Attaching " + run.clientName_ + " failed", false);
          CrowdyStudioRunTarget& attached = run.target(CrowdyStudioTarget::Client);
          attached.digest = outcome.data["digest"].asString();
          attached.version = outcome.data["clientVersion"].asInt64();
          attached.capabilityHash = outcome.data["capabilityHash"].asString();
          run.consent(outcome.data["modId"].asString());
        }));
  }

  /// The API serves a CLIENT half only to a player who consented to it or trusts its
  /// author, its author included.
  void consent(const std::string& modId) {
    auto impl = impl_.lock();
    if (!impl) return;
    CrowdyStudioRunTarget& client = target(CrowdyStudioTarget::Client);
    client.phase = Step::Consenting;
    step(Step::Consenting, CrowdyStudioTarget::Client, "Consenting to " + clientName_ + " as its author");
    impl->exec->consentClientModAsync(request_.scope.appId, modId, client.capabilityHash,
                                      guard([](Run& run, graphql::GraphQLOutcome outcome) {
                                        if (!outcome.ok()) {
                                          return run.fail(outcome, "Consenting to " + run.clientName_ + " failed",
                                                          false);
                                        }
                                        run.succeed();
                                      }));
  }

  void succeed() {
    for (auto& entry : result_.targets) entry.phase = Step::Done;
    const std::string message = request_.deployment == CrowdyStudioDeployment::Draft
                                    ? "Draft test is running"
                                    : "Project is live";
    step(Step::Done, std::nullopt, message);
    finish(Outcome::Running, message);
  }

  /// End on a failed request: RATE_LIMITED and, for a build, a 503 are their own outcomes;
  /// any other GraphQL error is a refusal carrying its code.
  void fail(const graphql::GraphQLOutcome& outcome, const std::string& what, bool building) {
    if (outcome.kind == graphql::GraphQLErrorKind::Cancelled) {
      return finish(Outcome::Cancelled, what + ": cancelled");
    }
    if (outcome.kind == graphql::GraphQLErrorKind::GraphQL && !outcome.errors.empty()) {
      const graphql::GraphQLErrorDetail& error = outcome.errors.front();
      Outcome kind = Outcome::Refused;
      if (error.code == "RATE_LIMITED") {
        kind = Outcome::RateLimited;
      } else if (building && error.httpStatus == 503) {
        kind = Outcome::QueueFull;
      }
      return finish(kind, what + ": " + error.message, error);
    }
    finish(Outcome::Failed,
           what + ": " + (outcome.errorMessage.empty() ? std::string("the request failed") : outcome.errorMessage));
  }

  void finish(Outcome outcome, std::string message,
              std::optional<graphql::GraphQLErrorDetail> error = std::nullopt) {
    if (finished_.exchange(true)) return;
    result_.outcome = outcome;
    result_.message = std::move(message);
    result_.error = std::move(error);
    state_->done.store(true, std::memory_order_release);
    auto self = shared_from_this();
    auto done = std::move(done_);
    progress_ = nullptr;
    if (auto impl = impl_.lock()) impl->forget(id_);
    if (done) done(std::move(result_));
  }

  void step(Step at, std::optional<CrowdyStudioTarget> which, std::string message) {
    if (progress_) progress_(CrowdyStudioRunProgress{at, which, std::move(message)});
  }

  CrowdyStudioRunTarget targetEntry(CrowdyStudioTarget which, const std::string& moduleName) const {
    CrowdyStudioRunTarget entry;
    entry.target = which;
    entry.moduleName = moduleName;
    entry.modName = modName_;
    return entry;
  }

  CrowdyStudioRunTarget& target(CrowdyStudioTarget which) {
    for (auto& entry : result_.targets) {
      if (entry.target == which) return entry;
    }
    throw std::logic_error("Crowdy Studio run has no such target");
  }

  std::vector<CrowdyStudioProjectFile> filesOf(CrowdyStudioTarget which) const {
    std::vector<CrowdyStudioProjectFile> files;
    for (const auto& file : project_.files) {
      if (file.target == which) files.push_back(file);
    }
    return files;
  }

  std::weak_ptr<CrowdyStudioModRunner::Impl> impl_;
  const std::uint64_t id_;
  const CrowdyStudioRunRequest request_;
  CrowdyStudioModRunner::DoneCallback done_;
  CrowdyStudioModRunner::ProgressCallback progress_;
  const std::shared_ptr<CrowdyStudioRunHandle::State> state_;
  CrowdyStudioRunResult result_;
  CrowdyStudioProject project_;
  bool hasServer_ = false;
  bool hasClient_ = false;
  std::string serverName_;
  std::string clientName_;
  std::string modName_;
  std::atomic<bool> finished_{false};
};

}  // namespace mod_runner_detail

CrowdyStudioModRunner::CrowdyStudioModRunner(std::shared_ptr<domains::ExecAPI> exec,
                                             std::shared_ptr<domains::CrowdyStudioAPI> projects,
                                             std::shared_ptr<domains::CrowdyStudioAgentAPI> agent,
                                             std::shared_ptr<graphql::Dispatcher> dispatcher)
    : impl_(std::make_shared<Impl>()) {
  if (!exec || !projects || !dispatcher) {
    throw std::invalid_argument("CrowdyStudioModRunner requires ExecAPI, CrowdyStudioAPI and a Dispatcher");
  }
  impl_->exec = std::move(exec);
  impl_->projects = std::move(projects);
  impl_->agent = std::move(agent);
  impl_->dispatcher = dispatcher;
  impl_->timer = std::make_shared<mod_runner_detail::Timer>(std::move(dispatcher));
}

CrowdyStudioModRunner::~CrowdyStudioModRunner() {
  impl_->timer->stop();
  std::map<std::uint64_t, std::shared_ptr<mod_runner_detail::Run>> runs;
  {
    std::lock_guard lock(impl_->mutex);
    runs.swap(impl_->runs);
  }
  for (auto& [id, run] : runs) run->abandon();
}

CrowdyStudioRunHandle CrowdyStudioModRunner::run(CrowdyStudioRunRequest request, DoneCallback onDone,
                                                 ProgressCallback onProgress) {
  if (request.projectId.empty() || request.scope.appId.empty() || request.scope.gridId.empty()) {
    throw std::invalid_argument("A Crowdy Studio run needs an app, a grid and a project");
  }
  if (request.expectedRevisionId.empty() && !request.expectedGithubSha) {
    throw std::invalid_argument(
        "A Crowdy Studio run needs the revision it may build (or a bound project's commit)");
  }
  auto state = std::make_shared<CrowdyStudioRunHandle::State>();
  std::shared_ptr<mod_runner_detail::Run> run;
  {
    std::lock_guard lock(impl_->mutex);
    const std::uint64_t id = ++impl_->nextRun;
    run = std::make_shared<mod_runner_detail::Run>(impl_, id, std::move(request), std::move(onDone),
                                                   std::move(onProgress), state);
    impl_->runs.emplace(id, run);
  }
  run->start();
  return CrowdyStudioRunHandle(std::move(state));
}

}  // namespace crowdy::studio
