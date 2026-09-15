#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "crowdy/domains/crowdy_studio_github.hpp"
#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"
#include "crowdy/studio/github_layout.hpp"
#include "crowdy/studio/models.hpp"

namespace crowdy::domains {

/// Typed Game API adapter for caller-owned Crowdy Studio projects, personal
/// library files, and the app-curated common catalog. Every operation carries
/// an app id; no method accepts an owner override, grid authority override, or
/// raw GraphQL document.
///
/// A project's `source` decides how saveProject persists files: STUDIO
/// projects go through crowdyStudioProjectSave under the project revision;
/// GITHUB projects commit each changed file through
/// crowdyStudioGitHubPutFile / DeleteFile under github.sha
/// (expectedCommitSha), because their files are a server-maintained mirror
/// of the repository and the Studio file mutations refuse them. Either way a
/// lost race is a CrowdyStudioRevisionConflictError and the editor's
/// "the remote moved" recovery applies. The controller never learns which.
class CrowdyStudioAPI final : public DomainBase,
                              public studio::ICrowdyStudioProjectProvider {
 public:
  explicit CrowdyStudioAPI(std::shared_ptr<graphql::GraphQLClient> gql)
      : DomainBase(gql), github_(gql) {}

  using ProjectsCallback = std::function<void(
      graphql::GraphQLOutcome,
      std::vector<studio::CrowdyStudioProjectSummary>)>;
  using ProjectCallback = std::function<void(
      graphql::GraphQLOutcome, studio::CrowdyStudioProject)>;
  using ReferencesCallback = std::function<void(
      graphql::GraphQLOutcome,
      std::vector<studio::CrowdyStudioReferenceFile>)>;
  using ReferenceCallback = std::function<void(
      graphql::GraphQLOutcome, studio::CrowdyStudioReferenceFile)>;

  std::vector<studio::CrowdyStudioProjectSummary> listProjects(
      const studio::CrowdyStudioProjectScope& scope) override {
    return listProjects(scope, {});
  }

  std::vector<studio::CrowdyStudioProjectSummary> listProjects(
      const studio::CrowdyStudioProjectScope& scope,
      const studio::CrowdyStudioListOptions& options) {
    graphql::JVal variables;
    variables["appId"] = scope.appId;
    variables["includeArchived"] = options.includeArchived;
    variables["limit"] = options.limit;
    variables["offset"] = options.offset;
    return request<std::vector<studio::CrowdyStudioProjectSummary>>(
        "CrowdyStudioProjects", variables,
        [](const graphql::Json& value) { return mapProjects(value); });
  }

  void listProjectsAsync(
      const studio::CrowdyStudioProjectScope& scope,
      const studio::CrowdyStudioListOptions& options,
      ProjectsCallback callback) {
    graphql::JVal variables;
    variables["appId"] = scope.appId;
    variables["includeArchived"] = options.includeArchived;
    variables["limit"] = options.limit;
    variables["offset"] = options.offset;
    requestAsync<std::vector<studio::CrowdyStudioProjectSummary>>(
        "CrowdyStudioProjects", variables,
        [](const graphql::Json& value) { return mapProjects(value); },
        std::move(callback));
  }

  studio::CrowdyStudioProject getProject(
      const studio::CrowdyStudioProjectScope& scope,
      std::string_view projectId) override {
    graphql::JVal variables;
    variables["appId"] = scope.appId;
    variables["projectId"] = projectId;
    return remember(request<studio::CrowdyStudioProject>(
        "CrowdyStudioProject", variables,
        [](const graphql::Json& value) { return mapProject(value); }));
  }

  void getProjectAsync(const studio::CrowdyStudioProjectScope& scope,
                       std::string_view projectId,
                       ProjectCallback callback) {
    graphql::JVal variables;
    variables["appId"] = scope.appId;
    variables["projectId"] = projectId;
    requestAsync<studio::CrowdyStudioProject>(
        "CrowdyStudioProject", variables,
        [](const graphql::Json& value) { return mapProject(value); },
        [this, callback = std::move(callback)](
            graphql::GraphQLOutcome outcome,
            studio::CrowdyStudioProject project) mutable {
          if (outcome.ok()) project = remember(std::move(project));
          callback(std::move(outcome), std::move(project));
        });
  }

  studio::CrowdyStudioProject createProject(
      const studio::CreateCrowdyStudioProjectInput& input) override {
    const graphql::JVal variables = oneInput(createProjectInput(input));
    return remember(request<studio::CrowdyStudioProject>(
        "CrowdyStudioProjectCreate", variables,
        [](const graphql::Json& value) { return mapProject(value); }));
  }

  void createProjectAsync(
      const studio::CreateCrowdyStudioProjectInput& input,
      ProjectCallback callback) {
    const graphql::JVal variables = oneInput(createProjectInput(input));
    requestAsync<studio::CrowdyStudioProject>(
        "CrowdyStudioProjectCreate", variables,
        [](const graphql::Json& value) { return mapProject(value); },
        [this, callback = std::move(callback)](
            graphql::GraphQLOutcome outcome,
            studio::CrowdyStudioProject project) mutable {
          if (outcome.ok()) project = remember(std::move(project));
          callback(std::move(outcome), std::move(project));
        });
  }

  /// Atomic full-project save used by the controller. Only changed files are
  /// sent. A STUDIO project shares one revision precondition and one
  /// transaction; a GITHUB project commits each changed file in turn.
  studio::CrowdyStudioProject saveProject(
      const studio::SaveCrowdyStudioProjectInput& input) override {
    if (!baselines_.contains(input.projectId)) {
      (void)getProject({input.appId, input.gridId}, input.projectId);
    }
    const auto baseline = baselines_.find(input.projectId);
    if (baseline != baselines_.end() &&
        baseline->second.source == studio::CrowdyStudioProjectSource::GitHub &&
        baseline->second.github && baseline->second.github->sha &&
        !baseline->second.github->sha->empty()) {
      return saveBoundProject(baseline->second, input);
    }
    const graphql::JVal variables = oneInput(saveProjectInput(input));
    try {
      return remember(request<studio::CrowdyStudioProject>(
          "CrowdyStudioProjectSave", variables,
          [](const graphql::Json& value) { return mapProject(value); }));
    } catch (const studio::CrowdyStudioRevisionConflictError& error) {
      throw withRemote(error, input);
    }
  }

  /// Non-throwing twin of saveProject. A STUDIO project posts one
  /// CrowdyStudioProjectSave on the async transport and delivers the
  /// callback from poll(). A GITHUB project still runs the commit loop
  /// on the caller's thread (one HTTP round trip per changed file plus
  /// metadata) and invokes the callback before this function returns —
  /// the same blocking contract as saveProject. A host that picked the
  /// Async variant to keep a frame from stalling should treat a bound
  /// save like saveProject until that path is itself async.
  void saveProjectAsync(const studio::SaveCrowdyStudioProjectInput& input,
                        ProjectCallback callback) {
    if (!baselines_.contains(input.projectId)) {
      getProjectAsync(
          {input.appId, input.gridId}, input.projectId,
          [this, input, callback = std::move(callback)](
              graphql::GraphQLOutcome outcome,
              studio::CrowdyStudioProject) mutable {
            if (!outcome.ok()) {
              callback(std::move(outcome), {});
              return;
            }
            saveProjectAsync(input, std::move(callback));
          });
      return;
    }
    const auto baseline = baselines_.find(input.projectId);
    if (baseline != baselines_.end() &&
        baseline->second.source == studio::CrowdyStudioProjectSource::GitHub &&
        baseline->second.github && baseline->second.github->sha &&
        !baseline->second.github->sha->empty()) {
      graphql::GraphQLOutcome outcome;
      studio::CrowdyStudioProject project;
      try {
        project = saveBoundProject(baseline->second, input);
        outcome.status = {};
      } catch (const studio::CrowdyStudioRevisionConflictError& error) {
        outcome.status = Errc::Rejected;
        outcome.kind = graphql::GraphQLErrorKind::GraphQL;
        outcome.errorMessage = error.what();
      } catch (const std::exception& error) {
        outcome.status = Errc::Rejected;
        outcome.kind = graphql::GraphQLErrorKind::Protocol;
        outcome.errorMessage = error.what();
      }
      callback(std::move(outcome), std::move(project));
      return;
    }
    const graphql::JVal variables = oneInput(saveProjectInput(input));
    requestAsync<studio::CrowdyStudioProject>(
        "CrowdyStudioProjectSave", variables,
        [](const graphql::Json& value) { return mapProject(value); },
        [this, callback = std::move(callback)](
            graphql::GraphQLOutcome outcome,
            studio::CrowdyStudioProject project) mutable {
          if (outcome.ok()) project = remember(std::move(project));
          callback(std::move(outcome), std::move(project));
        });
  }

  studio::CrowdyStudioProject saveProjectMetadata(
      const studio::SaveCrowdyStudioProjectMetadataInput& input) {
    return remember(request<studio::CrowdyStudioProject>(
        "CrowdyStudioProjectSaveMetadata",
        oneInput(saveProjectMetadataInput(input)),
        [](const graphql::Json& value) { return mapProject(value); }));
  }

  void saveProjectMetadataAsync(
      const studio::SaveCrowdyStudioProjectMetadataInput& input,
      ProjectCallback callback) {
    requestProjectAsync("CrowdyStudioProjectSaveMetadata",
                        oneInput(saveProjectMetadataInput(input)),
                        std::move(callback));
  }

  studio::CrowdyStudioProject saveProjectFiles(
      const studio::SaveCrowdyStudioProjectFilesInput& input) {
    return remember(request<studio::CrowdyStudioProject>(
        "CrowdyStudioProjectSaveFiles", oneInput(saveProjectFilesInput(input)),
        [](const graphql::Json& value) { return mapProject(value); }));
  }

  void saveProjectFilesAsync(
      const studio::SaveCrowdyStudioProjectFilesInput& input,
      ProjectCallback callback) {
    requestProjectAsync("CrowdyStudioProjectSaveFiles",
                        oneInput(saveProjectFilesInput(input)),
                        std::move(callback));
  }

  studio::CrowdyStudioProject setProjectArchived(
      const studio::SetCrowdyStudioProjectArchivedInput& input) {
    return remember(request<studio::CrowdyStudioProject>(
        "CrowdyStudioProjectSetArchived",
        oneInput(setProjectArchivedInput(input)),
        [](const graphql::Json& value) { return mapProject(value); }));
  }

  void setProjectArchivedAsync(
      const studio::SetCrowdyStudioProjectArchivedInput& input,
      ProjectCallback callback) {
    requestProjectAsync("CrowdyStudioProjectSetArchived",
                        oneInput(setProjectArchivedInput(input)),
                        std::move(callback));
  }

  std::vector<studio::CrowdyStudioReferenceFile>
  listPersonalLibraryFiles(
      const studio::CrowdyStudioProjectScope& scope) override {
    studio::CrowdyStudioListOptions options;
    options.limit = 100;
    return listPersonalLibraryFiles(scope, options);
  }

  std::vector<studio::CrowdyStudioReferenceFile>
  listPersonalLibraryFiles(
      const studio::CrowdyStudioProjectScope& scope,
      const studio::CrowdyStudioListOptions& options) {
    graphql::JVal variables;
    variables["appId"] = scope.appId;
    variables["includeArchived"] = options.includeArchived;
    variables["limit"] = options.limit;
    variables["offset"] = options.offset;
    return request<std::vector<studio::CrowdyStudioReferenceFile>>(
        "CrowdyStudioLibraryFiles", variables,
        [](const graphql::Json& value) { return mapLibraryFiles(value); });
  }

  void listPersonalLibraryFilesAsync(
      const studio::CrowdyStudioProjectScope& scope,
      const studio::CrowdyStudioListOptions& options,
      ReferencesCallback callback) {
    graphql::JVal variables;
    variables["appId"] = scope.appId;
    variables["includeArchived"] = options.includeArchived;
    variables["limit"] = options.limit;
    variables["offset"] = options.offset;
    requestAsync<std::vector<studio::CrowdyStudioReferenceFile>>(
        "CrowdyStudioLibraryFiles", variables,
        [](const graphql::Json& value) { return mapLibraryFiles(value); },
        std::move(callback));
  }

  studio::CrowdyStudioReferenceFile savePersonalLibraryFile(
      const studio::SaveCrowdyStudioLibraryFileInput& input) override {
    return request<studio::CrowdyStudioReferenceFile>(
        "CrowdyStudioLibrarySave", oneInput(saveLibraryFileInput(input)),
        [](const graphql::Json& value) { return mapLibraryFile(value); });
  }

  void savePersonalLibraryFileAsync(
      const studio::SaveCrowdyStudioLibraryFileInput& input,
      ReferenceCallback callback) {
    requestAsync<studio::CrowdyStudioReferenceFile>(
        "CrowdyStudioLibrarySave", oneInput(saveLibraryFileInput(input)),
        [](const graphql::Json& value) { return mapLibraryFile(value); },
        std::move(callback));
  }

  studio::CrowdyStudioReferenceFile setPersonalLibraryFileArchived(
      const studio::SetCrowdyStudioLibraryFileArchivedInput& input) {
    return request<studio::CrowdyStudioReferenceFile>(
        "CrowdyStudioLibrarySetArchived",
        oneInput(setLibraryFileArchivedInput(input)),
        [](const graphql::Json& value) { return mapLibraryFile(value); });
  }

  void setPersonalLibraryFileArchivedAsync(
      const studio::SetCrowdyStudioLibraryFileArchivedInput& input,
      ReferenceCallback callback) {
    requestAsync<studio::CrowdyStudioReferenceFile>(
        "CrowdyStudioLibrarySetArchived",
        oneInput(setLibraryFileArchivedInput(input)),
        [](const graphql::Json& value) { return mapLibraryFile(value); },
        std::move(callback));
  }

  std::vector<studio::CrowdyStudioReferenceFile> listCommonFiles(
      const studio::CrowdyStudioProjectScope& scope) override {
    return listCommonFiles(scope, {});
  }

  std::vector<studio::CrowdyStudioReferenceFile> listCommonFiles(
      const studio::CrowdyStudioProjectScope& scope,
      const studio::CrowdyStudioCommonListOptions& options) {
    graphql::JVal variables;
    variables["appId"] = scope.appId;
    if (options.target) {
      variables["target"] = studio::toString(*options.target);
    }
    variables["limit"] = options.limit;
    variables["offset"] = options.offset;
    return request<std::vector<studio::CrowdyStudioReferenceFile>>(
        "CrowdyStudioCommonFiles", variables,
        [](const graphql::Json& value) { return mapCommonFiles(value); });
  }

  void listCommonFilesAsync(
      const studio::CrowdyStudioProjectScope& scope,
      const studio::CrowdyStudioCommonListOptions& options,
      ReferencesCallback callback) {
    graphql::JVal variables;
    variables["appId"] = scope.appId;
    if (options.target) {
      variables["target"] = studio::toString(*options.target);
    }
    variables["limit"] = options.limit;
    variables["offset"] = options.offset;
    requestAsync<std::vector<studio::CrowdyStudioReferenceFile>>(
        "CrowdyStudioCommonFiles", variables,
        [](const graphql::Json& value) { return mapCommonFiles(value); },
        std::move(callback));
  }

  studio::CrowdyStudioProject importReferenceFile(
      const studio::ImportCrowdyStudioReferenceFileInput& input) override {
    return remember(request<studio::CrowdyStudioProject>(
        "CrowdyStudioProjectImportFile",
        oneInput(importReferenceFileInput(input)),
        [](const graphql::Json& value) { return mapProject(value); }));
  }

  void importReferenceFileAsync(
      const studio::ImportCrowdyStudioReferenceFileInput& input,
      ProjectCallback callback) {
    requestProjectAsync("CrowdyStudioProjectImportFile",
                        oneInput(importReferenceFileInput(input)),
                        std::move(callback));
  }

  /// Curated publication remains server-authorized (`manage_compute`). This
  /// wrapper never accepts or synthesizes an authority override.
  studio::CrowdyStudioReferenceFile publishCommonFile(
      const studio::PublishCrowdyStudioCommonFileInput& input) {
    return request<studio::CrowdyStudioReferenceFile>(
        "CrowdyStudioCommonPublish", oneInput(publishCommonFileInput(input)),
        [](const graphql::Json& value) { return mapCommonFile(value); });
  }

  void publishCommonFileAsync(
      const studio::PublishCrowdyStudioCommonFileInput& input,
      ReferenceCallback callback) {
    requestAsync<studio::CrowdyStudioReferenceFile>(
        "CrowdyStudioCommonPublish", oneInput(publishCommonFileInput(input)),
        [](const graphql::Json& value) { return mapCommonFile(value); },
        std::move(callback));
  }

  studio::CrowdyStudioProject createProjectFromModules(
      const studio::CreateCrowdyStudioProjectFromModulesInput& input) {
    return remember(request<studio::CrowdyStudioProject>(
        "CrowdyStudioProjectCreateFromModules",
        oneInput(createProjectFromModulesInput(input)),
        [](const graphql::Json& value) { return mapProject(value); }));
  }

  void createProjectFromModulesAsync(
      const studio::CreateCrowdyStudioProjectFromModulesInput& input,
      ProjectCallback callback) {
    requestProjectAsync("CrowdyStudioProjectCreateFromModules",
                        oneInput(createProjectFromModulesInput(input)),
                        std::move(callback));
  }

 private:
  template <typename T, typename Mapper>
  T request(std::string_view operation, const graphql::JVal& variables,
            Mapper mapper) {
    try {
      return mapper(execUnwrap(gen::crowdyStudio::documentFor(operation),
                               variables, operation));
    } catch (const graphql::CrowdyGraphQLError& error) {
      throwMapped(error);
    } catch (const graphql::CrowdyNetworkError& error) {
      throw studio::CrowdyStudioOfflineError(error.what());
    } catch (const graphql::CrowdyTimeoutError& error) {
      throw studio::CrowdyStudioOfflineError(error.what());
    } catch (const graphql::CrowdyHttpError& error) {
      if (error.status() >= 500) {
        throw studio::CrowdyStudioOfflineError(error.what());
      }
      throw;
    }
  }

  template <typename T, typename Mapper, typename Callback>
  void requestAsync(std::string_view operation,
                    const graphql::JVal& variables, Mapper mapper,
                    Callback callback) {
    execUnwrapAsync(
        gen::crowdyStudio::documentFor(operation), variables, operation,
        [mapper = std::move(mapper), callback = std::move(callback)](
            graphql::GraphQLOutcome outcome) mutable {
          T value{};
          if (outcome.ok()) {
            try {
              value = mapper(outcome.data);
            } catch (const std::exception& error) {
              outcome.status = Errc::Malformed;
              outcome.kind = graphql::GraphQLErrorKind::Protocol;
              outcome.errorMessage = error.what();
            }
          }
          callback(std::move(outcome), std::move(value));
        });
  }

  void requestProjectAsync(std::string_view operation,
                           const graphql::JVal& variables,
                           ProjectCallback callback) {
    requestAsync<studio::CrowdyStudioProject>(
        operation, variables,
        [](const graphql::Json& value) { return mapProject(value); },
        [this, callback = std::move(callback)](
            graphql::GraphQLOutcome outcome,
            studio::CrowdyStudioProject project) mutable {
          if (outcome.ok()) project = remember(std::move(project));
          callback(std::move(outcome), std::move(project));
        });
  }

  [[noreturn]] static void throwMapped(
      const graphql::CrowdyGraphQLError& error) {
    const std::string message = error.what();
    if (error.code() == "CROWDY_STUDIO_REVISION_CONFLICT" ||
        error.code() == "GITHUB_STALE_SHA" ||
        (error.code() == "CONFLICT" &&
         message.find("CROWDY_STUDIO_REVISION_CONFLICT") !=
             std::string::npos) ||
        message.find("CROWDY_STUDIO_REVISION_CONFLICT") !=
            std::string::npos ||
        message.find("GITHUB_STALE_SHA") != std::string::npos) {
      throw studio::CrowdyStudioRevisionConflictError(message);
    }
    if (error.code() == "IDEMPOTENCY_CONFLICT") {
      throw studio::CrowdyStudioIdempotencyConflictError(message);
    }
    throw error;
  }

  [[noreturn]] studio::CrowdyStudioProject withRemote(
      const studio::CrowdyStudioRevisionConflictError& error,
      const studio::SaveCrowdyStudioProjectInput& input) {
    std::optional<studio::CrowdyStudioProject> remote;
    try {
      remote = getProject({input.appId, input.gridId}, input.projectId);
    } catch (...) {
      // The conflict remains actionable when the follow-up read fails.
    }
    throw studio::CrowdyStudioRevisionConflictError(error.what(),
                                                     std::move(remote));
  }

  /// Persist a GITHUB project: metadata through the project save (a CAS on
  /// the revision, no file bodies), then each changed file as its own
  /// commit on the bound branch, each carrying the commit the previous one
  /// produced.
  studio::CrowdyStudioProject saveBoundProject(
      const studio::CrowdyStudioProject& baseline,
      const studio::SaveCrowdyStudioProjectInput& input) {
    const std::optional<std::string> startSha =
        baseline.github ? baseline.github->sha : std::nullopt;
    if (!startSha || startSha->empty()) {
      throw std::runtime_error(
          "The bound project has no mirror commit; refresh it from GitHub "
          "first.");
    }
    if (input.expectedRevisionId != baseline.revision.id) {
      std::optional<studio::CrowdyStudioProject> remote;
      try {
        remote = getProject({input.appId, input.gridId}, input.projectId);
      } catch (...) {
      }
      throw studio::CrowdyStudioRevisionConflictError(
          "CROWDY_STUDIO_REVISION_CONFLICT: expected project revision " +
              input.expectedRevisionId + "; current revision is " +
              baseline.revision.id + ".",
          std::move(remote));
    }
    try {
      if (metadataChanged(baseline, input)) {
        graphql::JVal body = saveProjectInput(input);
        body["upserts"] = graphql::JVal::array({});
        body["deletes"] = graphql::JVal::array({});
        (void)request<studio::CrowdyStudioProject>(
            "CrowdyStudioProjectSave", oneInput(std::move(body)),
            [](const graphql::Json& value) { return mapProject(value); });
      }
      const auto delta = projectFileDelta(baseline, input);
      std::string sha = *startSha;
      if (!delta.upserts.empty() || !delta.deletes.empty()) {
        const studio::CrowdyStudioGitHubLayout layout =
            layoutAt({input.appId, input.projectId}, sha);
        for (const auto& file : delta.upserts) {
          const std::string path =
              studio::normalizeCrowdyStudioPath(file.path);
          const auto repoPath = studio::studioFileToRepoPath(
              layout.roots(), file.target, path);
          if (!repoPath) {
            throw std::runtime_error(
                "The bound repository's crowdy.json has no " +
                std::string(studio::toString(file.target)) +
                " directory, so " + path + " has nowhere to go.");
          }
          const auto written = github_.putFile({
              input.appId,
              input.projectId,
              *repoPath,
              file.content,
              "studio: update " + *repoPath,
              sha,
              std::nullopt,
          });
          if (written.commitSha) sha = *written.commitSha;
        }
        for (const auto& file : delta.deletes) {
          const auto repoPath = studio::studioFileToRepoPath(
              layout.roots(), file.target,
              studio::normalizeCrowdyStudioPath(file.path));
          if (!repoPath) continue;
          const auto status = github_.deleteFile({
              input.appId,
              input.projectId,
              *repoPath,
              "studio: delete " + *repoPath,
              sha,
              std::nullopt,
          });
          if (status.githubSha) sha = *status.githubSha;
        }
      }
      return getProject({input.appId, input.gridId}, input.projectId);
    } catch (const studio::CrowdyStudioRevisionConflictError& error) {
      throw withRemote(error, input);
    } catch (const graphql::CrowdyGraphQLError& error) {
      try {
        throwMapped(error);
      } catch (const studio::CrowdyStudioRevisionConflictError& mapped) {
        throw withRemote(mapped, input);
      }
    }
  }

  studio::CrowdyStudioGitHubLayout layoutAt(
      const studio::CrowdyStudioGitHubProjectScope& scope,
      const std::string& commitSha) {
    const std::string key = scope.projectId + "@" + commitSha;
    const auto cached = layouts_.find(key);
    if (cached != layouts_.end()) return cached->second;
    auto layout = github_.layout(scope, commitSha);
    if (layouts_.size() > 64) {
      layouts_.erase(layoutOrder_.front());
      layoutOrder_.pop_front();
    }
    layouts_[key] = layout;
    layoutOrder_.push_back(key);
    return layout;
  }

  static bool metadataChanged(
      const studio::CrowdyStudioProject& baseline,
      const studio::SaveCrowdyStudioProjectInput& input) {
    const auto& a = baseline.metadata;
    const auto& b = input.metadata;
    const std::string baselineGrid = baseline.gridId.value_or("");
    if (baselineGrid != input.gridId) return true;
    if (a.name != b.name) return true;
    if ((a.description.value_or("")) != (b.description.value_or(""))) {
      return true;
    }
    if ((a.serverModuleName.value_or("")) !=
        (b.serverModuleName.value_or(""))) {
      return true;
    }
    if ((a.clientModuleName.value_or("")) !=
        (b.clientModuleName.value_or(""))) {
      return true;
    }
    const auto before = studio::apiPairing(baseline.kind, a.pairingPreference);
    const auto after =
        studio::apiPairing(kindFromFiles(input.files), b.pairingPreference);
    return before != after;
  }

  struct ProjectFileDelta {
    std::vector<studio::CrowdyStudioProjectFile> upserts;
    std::vector<studio::CrowdyStudioProjectFileDelete> deletes;
  };

  static ProjectFileDelta projectFileDelta(
      const studio::CrowdyStudioProject& baseline,
      const studio::SaveCrowdyStudioProjectInput& input) {
    std::unordered_map<std::string, const studio::CrowdyStudioProjectFile*>
        previous;
    for (const auto& file : baseline.files) {
      previous[studio::crowdyStudioFileKey(file.target, file.path)] = &file;
    }
    std::unordered_map<std::string, const studio::CrowdyStudioProjectFile*>
        current;
    ProjectFileDelta delta;
    for (const auto& file : input.files) {
      const std::string key =
          studio::crowdyStudioFileKey(file.target, file.path);
      current[key] = &file;
      const auto before = previous.find(key);
      if (before == previous.end() ||
          before->second->content != file.content) {
        delta.upserts.push_back(file);
      }
    }
    for (const auto& file : baseline.files) {
      if (!current.contains(
              studio::crowdyStudioFileKey(file.target, file.path))) {
        delta.deletes.push_back({file.target, file.path});
      }
    }
    return delta;
  }

  studio::CrowdyStudioProject remember(
      studio::CrowdyStudioProject project) {
    baselines_[project.projectId] = project;
    return project;
  }

  CrowdyStudioGitHubAPI github_;
  std::unordered_map<std::string, studio::CrowdyStudioGitHubLayout> layouts_;
  std::deque<std::string> layoutOrder_;

  static graphql::JVal oneInput(graphql::JVal input) {
    graphql::JVal variables;
    variables["input"] = std::move(input);
    return variables;
  }

  static graphql::JVal fileInput(
      const studio::CrowdyStudioProjectFile& file) {
    graphql::JVal value;
    value["target"] = studio::toString(file.target);
    value["path"] = studio::normalizeCrowdyStudioPath(file.path);
    value["content"] = file.content;
    return value;
  }

  static graphql::JVal deleteInput(
      const studio::CrowdyStudioProjectFileDelete& file) {
    graphql::JVal value;
    value["target"] = studio::toString(file.target);
    value["path"] = studio::normalizeCrowdyStudioPath(file.path);
    return value;
  }

  template <typename T, typename Mapper>
  static graphql::JVal arrayInput(const std::vector<T>& values,
                                  Mapper mapper) {
    graphql::JArray array;
    array.reserve(values.size());
    for (const T& value : values) array.push_back(mapper(value));
    return graphql::JVal(std::move(array));
  }

  static void putIdempotency(
      graphql::JVal& input, const std::optional<std::string>& key) {
    if (key) input["idempotencyKey"] = *key;
  }

  static void putPatch(graphql::JVal& input, std::string_view name,
                       const studio::CrowdyStudioPatchField<std::string>& field) {
    if (field.isNull()) input[name] = nullptr;
    else if (field.hasValue()) input[name] = field.get();
  }

  static graphql::JVal createProjectInput(
      const studio::CreateCrowdyStudioProjectInput& value) {
    graphql::JVal input;
    input["appId"] = value.appId;
    if (value.gridId) input["gridId"] = *value.gridId;
    input["name"] = value.metadata.name;
    if (value.metadata.description) {
      input["description"] = *value.metadata.description;
    }
    if (value.metadata.serverModuleName) {
      input["serverModuleName"] = *value.metadata.serverModuleName;
    }
    if (value.metadata.clientModuleName) {
      input["clientModuleName"] = *value.metadata.clientModuleName;
    }
    input["pairingPreference"] = studio::toString(
        studio::apiPairing(value.kind, value.metadata.pairingPreference));
    input["sdkVersion"] = value.sdkVersion;
    input["abiVersion"] = value.abiVersion;
    input["initialFiles"] =
        arrayInput(value.files, [](const auto& file) { return fileInput(file); });
    putIdempotency(input, value.idempotencyKey);
    return input;
  }

  static graphql::JVal metadataPatchInput(
      const studio::SaveCrowdyStudioProjectMetadataInput& value) {
    graphql::JVal input;
    input["appId"] = value.appId;
    input["projectId"] = value.projectId;
    input["expectedRevision"] = value.expectedRevisionId;
    putPatch(input, "gridId", value.patch.gridId);
    if (value.patch.name) input["name"] = *value.patch.name;
    putPatch(input, "description", value.patch.description);
    putPatch(input, "serverModuleName", value.patch.serverModuleName);
    putPatch(input, "clientModuleName", value.patch.clientModuleName);
    if (value.patch.pairingPreference) {
      input["pairingPreference"] =
          studio::toString(*value.patch.pairingPreference);
    }
    if (value.patch.sdkVersion) {
      input["sdkVersion"] = *value.patch.sdkVersion;
    }
    if (value.patch.abiVersion) {
      input["abiVersion"] = *value.patch.abiVersion;
    }
    putIdempotency(input, value.idempotencyKey);
    return input;
  }

  static graphql::JVal saveProjectMetadataInput(
      const studio::SaveCrowdyStudioProjectMetadataInput& value) {
    return metadataPatchInput(value);
  }

  static graphql::JVal saveProjectFilesInput(
      const studio::SaveCrowdyStudioProjectFilesInput& value) {
    graphql::JVal input;
    input["appId"] = value.appId;
    input["projectId"] = value.projectId;
    input["expectedRevision"] = value.expectedRevisionId;
    input["upserts"] =
        arrayInput(value.upserts, [](const auto& file) { return fileInput(file); });
    input["deletes"] =
        arrayInput(value.deletes, [](const auto& file) { return deleteInput(file); });
    putIdempotency(input, value.idempotencyKey);
    return input;
  }

  graphql::JVal saveProjectInput(
      const studio::SaveCrowdyStudioProjectInput& value) const {
    graphql::JVal input;
    input["appId"] = value.appId;
    input["projectId"] = value.projectId;
    input["expectedRevision"] = value.expectedRevisionId;
    input["gridId"] = value.gridId;
    input["name"] = value.metadata.name;
    input["description"] =
        value.metadata.description
            ? graphql::JVal(*value.metadata.description)
            : graphql::JVal(nullptr);
    input["serverModuleName"] =
        value.metadata.serverModuleName
            ? graphql::JVal(*value.metadata.serverModuleName)
            : graphql::JVal(nullptr);
    input["clientModuleName"] =
        value.metadata.clientModuleName
            ? graphql::JVal(*value.metadata.clientModuleName)
            : graphql::JVal(nullptr);
    const auto baseline = baselines_.find(value.projectId);
    const studio::CrowdyStudioProjectKind kind =
        baseline == baselines_.end() ? kindFromFiles(value.files)
                                     : baseline->second.kind;
    input["pairingPreference"] = studio::toString(
        studio::apiPairing(kind, value.metadata.pairingPreference));
    input["sdkVersion"] = value.sdkVersion;
    input["abiVersion"] = value.abiVersion;

    std::unordered_map<std::string, const studio::CrowdyStudioProjectFile*>
        previous;
    if (baseline != baselines_.end()) {
      for (const auto& file : baseline->second.files) {
        previous[studio::crowdyStudioFileKey(file.target, file.path)] = &file;
      }
    }
    std::unordered_map<std::string, const studio::CrowdyStudioProjectFile*>
        current;
    std::vector<studio::CrowdyStudioProjectFile> upserts;
    for (const auto& file : value.files) {
      const std::string key =
          studio::crowdyStudioFileKey(file.target, file.path);
      current[key] = &file;
      const auto before = previous.find(key);
      if (before == previous.end() ||
          before->second->content != file.content) {
        upserts.push_back(file);
      }
    }
    std::vector<studio::CrowdyStudioProjectFileDelete> deletes;
    if (baseline != baselines_.end()) {
      for (const auto& file : baseline->second.files) {
        if (!current.contains(
                studio::crowdyStudioFileKey(file.target, file.path))) {
          deletes.push_back({file.target, file.path});
        }
      }
    }
    input["upserts"] =
        arrayInput(upserts, [](const auto& file) { return fileInput(file); });
    input["deletes"] =
        arrayInput(deletes, [](const auto& file) { return deleteInput(file); });
    putIdempotency(input, value.idempotencyKey);
    return input;
  }

  static graphql::JVal setProjectArchivedInput(
      const studio::SetCrowdyStudioProjectArchivedInput& value) {
    graphql::JVal input;
    input["appId"] = value.appId;
    input["projectId"] = value.projectId;
    input["expectedRevision"] = value.expectedRevisionId;
    input["archived"] = value.archived;
    putIdempotency(input, value.idempotencyKey);
    return input;
  }

  static graphql::JVal saveLibraryFileInput(
      const studio::SaveCrowdyStudioLibraryFileInput& value) {
    graphql::JVal input;
    input["appId"] = value.appId;
    if (value.libraryFileId) input["libraryFileId"] = *value.libraryFileId;
    if (value.expectedRevisionId) {
      input["expectedRevision"] = *value.expectedRevisionId;
    }
    input["title"] = value.title;
    input["pathHint"] = studio::normalizeCrowdyStudioPath(value.path);
    input["target"] = studio::toString(value.target);
    input["tags"] = arrayInput(
        value.tags,
        [](const std::string& tag) { return graphql::JVal(tag); });
    input["content"] = value.content;
    putIdempotency(input, value.idempotencyKey);
    return input;
  }

  static graphql::JVal setLibraryFileArchivedInput(
      const studio::SetCrowdyStudioLibraryFileArchivedInput& value) {
    graphql::JVal input;
    input["appId"] = value.appId;
    input["libraryFileId"] = value.libraryFileId;
    input["expectedRevision"] = value.expectedRevisionId;
    input["archived"] = value.archived;
    putIdempotency(input, value.idempotencyKey);
    return input;
  }

  static graphql::JVal importReferenceFileInput(
      const studio::ImportCrowdyStudioReferenceFileInput& value) {
    graphql::JVal input;
    input["appId"] = value.appId;
    input["projectId"] = value.projectId;
    input["expectedProjectRevision"] = value.expectedRevisionId;
    input["source"] = studio::toString(value.source);
    if (value.source ==
        studio::CrowdyStudioReferenceSource::PersonalLibrary) {
      input["libraryFileId"] = value.referenceId;
    } else {
      input["commonVersionId"] = value.referenceId;
    }
    if (value.destinationPath) {
      input["destinationPath"] =
          studio::normalizeCrowdyStudioPath(*value.destinationPath);
    }
    putIdempotency(input, value.idempotencyKey);
    return input;
  }

  static graphql::JVal publishCommonFileInput(
      const studio::PublishCrowdyStudioCommonFileInput& value) {
    graphql::JVal input;
    input["appId"] = value.appId;
    if (value.commonFileId) input["commonFileId"] = *value.commonFileId;
    input["slug"] = value.slug;
    input["title"] = value.title;
    putPatch(input, "description", value.description);
    input["path"] = studio::normalizeCrowdyStudioPath(value.path);
    input["target"] = studio::toString(value.target);
    input["tags"] = arrayInput(
        value.tags,
        [](const std::string& tag) { return graphql::JVal(tag); });
    input["content"] = value.content;
    putIdempotency(input, value.idempotencyKey);
    return input;
  }

  static graphql::JVal createProjectFromModulesInput(
      const studio::CreateCrowdyStudioProjectFromModulesInput& value) {
    graphql::JVal input;
    input["appId"] = value.appId;
    input["gridId"] = value.gridId;
    if (value.serverModuleName) {
      input["serverModuleName"] = *value.serverModuleName;
    }
    if (value.clientModuleName) {
      input["clientModuleName"] = *value.clientModuleName;
    }
    if (value.projectName) input["projectName"] = *value.projectName;
    putIdempotency(input, value.idempotencyKey);
    return input;
  }

  static std::string scalarString(const graphql::Json& value) {
    if (!value.ok() || value.isNull()) return {};
    if (value.isString()) return value.asString();
    return std::to_string(value.asInt64());
  }

  static std::optional<std::string> optionalString(
      const graphql::Json& value) {
    if (!value.ok() || value.isNull()) return std::nullopt;
    return scalarString(value);
  }

  static studio::CrowdyStudioTarget requiredTarget(
      const graphql::Json& value) {
    const auto target = studio::targetFromString(value.asStringView());
    if (!target) {
      throw graphql::CrowdyProtocolError(
          "Crowdy Studio response contains an unknown target");
    }
    return *target;
  }

  static studio::CrowdyStudioApiPairing requiredPairing(
      const graphql::Json& value) {
    const auto pairing = studio::apiPairingFromString(value.asStringView());
    if (!pairing) {
      throw graphql::CrowdyProtocolError(
          "Crowdy Studio response contains an unknown pairing preference");
    }
    return *pairing;
  }

  static studio::CrowdyStudioProjectFile mapProjectFile(
      const graphql::Json& value) {
    studio::CrowdyStudioProjectFile file;
    file.target = requiredTarget(value["target"]);
    file.path = studio::normalizeCrowdyStudioPath(value["path"].asString());
    file.content = value["content"].asString();
    file.revision = scalarString(value["revision"]);
    file.provenance =
        studio::provenanceFromString(value["provenance"].asStringView());
    file.provenanceLibraryFileId =
        optionalString(value["provenanceLibraryFileId"]);
    file.provenanceLibraryRevision =
        optionalString(value["provenanceLibraryRevision"]);
    file.provenanceCommonVersionId =
        optionalString(value["provenanceCommonVersionId"]);
    file.createdAt = value["createdAt"].asString();
    file.updatedAt = value["updatedAt"].asString();
    return file;
  }

  static studio::CrowdyStudioProject mapProject(
      const graphql::Json& value) {
    studio::CrowdyStudioProject project;
    project.projectId = value["projectId"].asString();
    project.appId = scalarString(value["appId"]);
    project.ownerUserId = scalarString(value["ownerUserId"]);
    project.gridId = optionalString(value["gridId"]);
    const auto pairing = requiredPairing(value["pairingPreference"]);
    project.kind = studio::projectKind(pairing);
    project.metadata.name = value["name"].asString();
    project.metadata.description = optionalString(value["description"]);
    project.metadata.serverModuleName =
        optionalString(value["serverModuleName"]);
    project.metadata.clientModuleName =
        optionalString(value["clientModuleName"]);
    project.metadata.pairingPreference =
        studio::pairingPreference(pairing);
    project.sdkVersion = value["sdkVersion"].asString();
    project.abiVersion = static_cast<int>(value["abiVersion"].asInt64());
    project.revision.id = scalarString(value["revision"]);
    project.revision.savedAt = value["updatedAt"].asString();
    project.archived = value["archived"].asBool();
    project.archivedAt = optionalString(value["archivedAt"]);
    project.fileCount = static_cast<int>(value["fileCount"].asInt64());
    project.totalBytes = scalarString(value["totalBytes"]);
    const auto owner = optionalString(value["githubOwner"]);
    const auto repo = optionalString(value["githubRepo"]);
    const auto branch = optionalString(value["githubBranch"]);
    const bool bound = value["source"].ok() &&
                       value["source"].asString() == "GITHUB" && owner &&
                       repo && branch;
    project.source = bound ? studio::CrowdyStudioProjectSource::GitHub
                           : studio::CrowdyStudioProjectSource::Studio;
    if (bound) {
      project.github = studio::CrowdyStudioProjectGitHub{
          *owner, *repo, *branch, optionalString(value["githubSha"])};
    }
    value["files"].forEach([&](const graphql::Json& file) {
      project.files.push_back(mapProjectFile(file));
    });
    project.createdAt = value["createdAt"].asString();
    project.updatedAt = value["updatedAt"].asString();
    return project;
  }

  static studio::CrowdyStudioProjectSummary mapProjectSummary(
      const graphql::Json& value) {
    studio::CrowdyStudioProjectSummary summary;
    summary.projectId = value["projectId"].asString();
    summary.gridId = optionalString(value["gridId"]);
    summary.name = value["name"].asString();
    summary.kind =
        studio::projectKind(requiredPairing(value["pairingPreference"]));
    summary.revisionId = scalarString(value["revision"]);
    summary.serverModuleName = optionalString(value["serverModuleName"]);
    summary.clientModuleName = optionalString(value["clientModuleName"]);
    summary.archived = value["archived"].asBool();
    summary.updatedAt = value["updatedAt"].asString();
    const auto owner = optionalString(value["githubOwner"]);
    const auto repo = optionalString(value["githubRepo"]);
    const auto branch = optionalString(value["githubBranch"]);
    const bool bound = value["source"].ok() &&
                       value["source"].asString() == "GITHUB" && owner &&
                       repo && branch;
    summary.source = bound ? studio::CrowdyStudioProjectSource::GitHub
                           : studio::CrowdyStudioProjectSource::Studio;
    if (bound) {
      summary.github = *owner + "/" + *repo + "@" + *branch;
      summary.githubSha = optionalString(value["githubSha"]);
    }
    return summary;
  }

  static std::vector<studio::CrowdyStudioProjectSummary> mapProjects(
      const graphql::Json& value) {
    std::vector<studio::CrowdyStudioProjectSummary> projects;
    value.forEach([&](const graphql::Json& project) {
      projects.push_back(mapProjectSummary(project));
    });
    return projects;
  }

  static std::vector<std::string> mapStrings(
      const graphql::Json& value) {
    std::vector<std::string> strings;
    value.forEach([&](const graphql::Json& entry) {
      strings.push_back(entry.asString());
    });
    return strings;
  }

  static studio::CrowdyStudioReferenceFile mapLibraryFile(
      const graphql::Json& value) {
    studio::CrowdyStudioReferenceFile file;
    file.id = value["libraryFileId"].asString();
    file.source = studio::CrowdyStudioReferenceSource::PersonalLibrary;
    file.appId = scalarString(value["appId"]);
    file.ownerUserId = optionalString(value["ownerUserId"]);
    file.title = value["title"].asString();
    file.target = requiredTarget(value["target"]);
    file.path =
        studio::normalizeCrowdyStudioPath(value["pathHint"].asString());
    file.content = value["content"].asString();
    file.tags = mapStrings(value["tags"]);
    file.revision = scalarString(value["revision"]);
    file.archived = value["archived"].asBool();
    file.archivedAt = optionalString(value["archivedAt"]);
    file.createdAt = value["createdAt"].asString();
    file.updatedAt = value["updatedAt"].asString();
    return file;
  }

  static std::vector<studio::CrowdyStudioReferenceFile> mapLibraryFiles(
      const graphql::Json& value) {
    std::vector<studio::CrowdyStudioReferenceFile> files;
    value.forEach([&](const graphql::Json& file) {
      files.push_back(mapLibraryFile(file));
    });
    return files;
  }

  static studio::CrowdyStudioReferenceFile mapCommonFile(
      const graphql::Json& value) {
    studio::CrowdyStudioReferenceFile file;
    file.id = value["versionId"].asString();
    file.source = studio::CrowdyStudioReferenceSource::Common;
    file.appId = scalarString(value["appId"]);
    file.commonFileId = optionalString(value["commonFileId"]);
    file.slug = optionalString(value["slug"]);
    file.title = value["title"].asString();
    file.description = optionalString(value["description"]);
    file.target = requiredTarget(value["target"]);
    file.path = studio::normalizeCrowdyStudioPath(value["path"].asString());
    file.content = value["content"].asString();
    file.tags = mapStrings(value["tags"]);
    file.revision = scalarString(value["versionNo"]);
    file.commonStatus =
        studio::commonStatusFromString(value["status"].asStringView());
    file.contentSha256 = optionalString(value["contentSha256"]);
    file.publishedByUserId = optionalString(value["publishedByUserId"]);
    file.publishedAt = optionalString(value["publishedAt"]);
    file.createdAt = value["createdAt"].asString();
    file.updatedAt = value["updatedAt"].asString();
    return file;
  }

  static std::vector<studio::CrowdyStudioReferenceFile> mapCommonFiles(
      const graphql::Json& value) {
    std::vector<studio::CrowdyStudioReferenceFile> files;
    value.forEach([&](const graphql::Json& file) {
      files.push_back(mapCommonFile(file));
    });
    return files;
  }

  static studio::CrowdyStudioProjectKind kindFromFiles(
      const std::vector<studio::CrowdyStudioProjectFile>& files) {
    bool server = false;
    bool client = false;
    for (const auto& file : files) {
      server = server || file.target == studio::CrowdyStudioTarget::Server;
      client = client || file.target == studio::CrowdyStudioTarget::Client;
    }
    if (server && client) return studio::CrowdyStudioProjectKind::FullStack;
    if (client) return studio::CrowdyStudioProjectKind::Client;
    return studio::CrowdyStudioProjectKind::Server;
  }

  std::unordered_map<std::string, studio::CrowdyStudioProject> baselines_;
};

}  // namespace crowdy::domains
