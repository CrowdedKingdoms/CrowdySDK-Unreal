#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/enums.hpp"
#include "crowdy/generated/operations.hpp"
#include "crowdy/studio/github.hpp"

namespace crowdy::domains {

/// client.crowdyStudioGitHub() — GraphQL transport for GitHub-backed Crowdy
/// Studio projects. Status / layout / tree / file / put / delete / refresh
/// work under a game's app token (owner-scoped). connectUrl / repos / bind /
/// unbind need the identity session. A game never needs an identity session
/// to author against a bound repository.
class CrowdyStudioGitHubAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  studio::CrowdyStudioGitHubStatus status(
      std::optional<std::string> appId = std::nullopt,
      std::optional<std::string> projectId = std::nullopt) const {
    graphql::JVal vars;
    if (appId) vars["appId"] = *appId;
    if (projectId) vars["projectId"] = *projectId;
    return mapStatus(run("CrowdyStudioGitHubStatus", vars));
  }

  std::string connectUrl() const {
    const graphql::Json value = run("CrowdyStudioGitHubConnectUrl", {});
    return value["connectUrl"].asString();
  }

  std::vector<studio::CrowdyStudioGitHubRepo> repos() const {
    return mapRepos(run("CrowdyStudioGitHubRepos", {}));
  }

  studio::CrowdyStudioGitHubStatus bind(
      const studio::CrowdyStudioGitHubBindInput& input) const {
    graphql::JVal body;
    body["appId"] = input.appId;
    body["projectId"] = input.projectId;
    body["owner"] = input.owner;
    body["repo"] = input.repo;
    if (input.branch && !input.branch->empty()) body["branch"] = *input.branch;
    body["initial"] = std::string(gen::toString(input.initial));
    return mapStatus(runInput("CrowdyStudioGitHubBind", std::move(body)));
  }

  studio::CrowdyStudioGitHubStatus unbind(
      const studio::CrowdyStudioGitHubProjectScope& scope) const {
    return mapStatus(runInput("CrowdyStudioGitHubUnbind", scopeInput(scope)));
  }

  studio::CrowdyStudioGitHubStatus refresh(
      const studio::CrowdyStudioGitHubProjectScope& scope) const {
    return mapStatus(runInput("CrowdyStudioGitHubRefresh", scopeInput(scope)));
  }

  studio::CrowdyStudioGitHubLayout layout(
      const studio::CrowdyStudioGitHubProjectScope& scope,
      std::optional<std::string> commitSha = std::nullopt) const {
    return mapLayout(runInput("CrowdyStudioGitHubLayout",
                              atCommitInput(scope, commitSha)));
  }

  studio::CrowdyStudioGitHubTree tree(
      const studio::CrowdyStudioGitHubProjectScope& scope,
      std::optional<std::string> commitSha = std::nullopt) const {
    return mapTree(
        runInput("CrowdyStudioGitHubTree", atCommitInput(scope, commitSha)));
  }

  studio::CrowdyStudioGitHubFile getFile(
      const studio::CrowdyStudioGitHubProjectScope& scope,
      std::string_view path,
      std::optional<std::string> commitSha = std::nullopt) const {
    graphql::JVal input = atCommitInput(scope, commitSha);
    input["path"] = path;
    return mapFile(runInput("CrowdyStudioGitHubFile", std::move(input)));
  }

  /// Up to 100 files at one commit (default: the mirror commit) in one operation against the
  /// per-user GitHub budget. Paths that are not files there come back in `missing`; binary or
  /// oversize blobs with no content.
  studio::CrowdyStudioGitHubFiles getFiles(
      const studio::CrowdyStudioGitHubFilesInput& input) const {
    graphql::JVal body;
    body["appId"] = input.appId;
    body["projectId"] = input.projectId;
    graphql::JArray paths;
    paths.reserve(input.paths.size());
    for (const auto& path : input.paths) paths.emplace_back(path);
    body["paths"] = graphql::JVal(std::move(paths));
    if (input.commitSha && !input.commitSha->empty()) body["commitSha"] = *input.commitSha;
    return mapFiles(runInput("CrowdyStudioGitHubFiles", std::move(body)));
  }

  /// Up to 100 writes and deletions as ONE commit on top of `expectedCommitSha` (the project's
  /// githubSha). The returned `commitSha` is the new githubSha.
  studio::CrowdyStudioGitHubCommit commitFiles(
      const studio::CrowdyStudioGitHubCommitFilesInput& input) const {
    graphql::JVal body;
    body["appId"] = input.appId;
    body["projectId"] = input.projectId;
    body["message"] = input.message;
    body["expectedCommitSha"] = input.expectedCommitSha;
    graphql::JArray changes;
    changes.reserve(input.changes.size());
    for (const auto& change : input.changes) {
      graphql::JVal entry;
      entry["path"] = change.path;
      if (change.content) entry["content"] = *change.content;
      if (change.remove) entry["delete"] = true;
      changes.push_back(std::move(entry));
    }
    body["changes"] = graphql::JVal(std::move(changes));
    return mapCommit(runInput("CrowdyStudioGitHubCommitFiles", std::move(body)));
  }

  studio::CrowdyStudioGitHubFile putFile(
      const studio::CrowdyStudioGitHubPutFileInput& input) const {
    graphql::JVal body;
    body["appId"] = input.appId;
    body["projectId"] = input.projectId;
    body["path"] = input.path;
    body["content"] = input.content;
    body["message"] = input.message;
    body["expectedCommitSha"] = input.expectedCommitSha;
    if (input.sha) body["sha"] = *input.sha;
    return mapFile(runInput("CrowdyStudioGitHubPutFile", std::move(body)));
  }

  studio::CrowdyStudioGitHubStatus deleteFile(
      const studio::CrowdyStudioGitHubDeleteFileInput& input) const {
    graphql::JVal body;
    body["appId"] = input.appId;
    body["projectId"] = input.projectId;
    body["path"] = input.path;
    body["message"] = input.message;
    body["expectedCommitSha"] = input.expectedCommitSha;
    if (input.sha) body["sha"] = *input.sha;
    return mapStatus(
        runInput("CrowdyStudioGitHubDeleteFile", std::move(body)));
  }

 private:
  graphql::Json run(std::string_view operation,
                    const graphql::JVal& variables) const {
    return execUnwrap(gen::crowdyStudio::documentFor(operation), variables,
                      operation);
  }

  graphql::Json runInput(std::string_view operation, graphql::JVal input) const {
    graphql::JVal variables;
    variables["input"] = std::move(input);
    return run(operation, variables);
  }

  static graphql::JVal scopeInput(
      const studio::CrowdyStudioGitHubProjectScope& scope) {
    graphql::JVal input;
    input["appId"] = scope.appId;
    input["projectId"] = scope.projectId;
    return input;
  }

  static graphql::JVal atCommitInput(
      const studio::CrowdyStudioGitHubProjectScope& scope,
      const std::optional<std::string>& commitSha) {
    graphql::JVal input = scopeInput(scope);
    if (commitSha && !commitSha->empty()) input["commitSha"] = *commitSha;
    return input;
  }

  static std::string scalarString(const graphql::Json& value) {
    if (!value.ok() || value.isNull()) return {};
    if (value.isString()) return value.asString();
    return std::to_string(value.asInt64());
  }

  static std::optional<std::string> optionalString(const graphql::Json& value) {
    if (!value.ok() || value.isNull()) return std::nullopt;
    return scalarString(value);
  }

  static studio::CrowdyStudioGitHubStatus mapStatus(
      const graphql::Json& value) {
    studio::CrowdyStudioGitHubStatus status;
    status.configured = value["configured"].asBool();
    status.connected = value["connected"].asBool();
    status.accountLogin = optionalString(value["accountLogin"]);
    status.accountType = optionalString(value["accountType"]);
    status.owner = optionalString(value["owner"]);
    status.repo = optionalString(value["repo"]);
    status.branch = optionalString(value["branch"]);
    status.githubSha = optionalString(value["githubSha"]);
    status.installUrl = optionalString(value["installUrl"]);
    return status;
  }

  static std::vector<studio::CrowdyStudioGitHubRepo> mapRepos(
      const graphql::Json& value) {
    std::vector<studio::CrowdyStudioGitHubRepo> repos;
    value.forEach([&](const graphql::Json& entry) {
      studio::CrowdyStudioGitHubRepo repo;
      repo.owner = entry["owner"].asString();
      repo.name = entry["name"].asString();
      repo.fullName = entry["fullName"].asString();
      repo.isPrivate = entry["private"].asBool();
      repo.defaultBranch = optionalString(entry["defaultBranch"]);
      repos.push_back(std::move(repo));
    });
    return repos;
  }

  static studio::CrowdyStudioGitHubLayout mapLayout(
      const graphql::Json& value) {
    studio::CrowdyStudioGitHubLayout layout;
    layout.commitSha = value["commitSha"].asString();
    layout.server = value["server"].asString();
    layout.client = optionalString(value["client"]);
    layout.assets = value["assets"].asString();
    layout.fromFile = value["fromFile"].asBool();
    layout.kind = gen::crowdyStudioGitHubLayoutKindFromString(value["kind"].asStringView());
    return layout;
  }

  static std::vector<std::string> stringList(const graphql::Json& value) {
    std::vector<std::string> out;
    value.forEach([&](const graphql::Json& entry) { out.push_back(entry.asString()); });
    return out;
  }

  static studio::CrowdyStudioGitHubFiles mapFiles(const graphql::Json& value) {
    studio::CrowdyStudioGitHubFiles files;
    files.commitSha = value["commitSha"].asString();
    value["files"].forEach([&](const graphql::Json& entry) {
      studio::CrowdyStudioGitHubFileContent file;
      file.path = entry["path"].asString();
      file.sha = entry["sha"].asString();
      file.size = static_cast<int>(entry["size"].asInt64());
      file.binary = entry["binary"].asBool();
      file.content = optionalString(entry["content"]);
      files.files.push_back(std::move(file));
    });
    files.missing = stringList(value["missing"]);
    return files;
  }

  static studio::CrowdyStudioGitHubCommit mapCommit(const graphql::Json& value) {
    studio::CrowdyStudioGitHubCommit commit;
    commit.commitSha = value["commitSha"].asString();
    commit.parentSha = value["parentSha"].asString();
    commit.written = stringList(value["written"]);
    commit.deleted = stringList(value["deleted"]);
    return commit;
  }

  static studio::CrowdyStudioGitHubTree mapTree(const graphql::Json& value) {
    studio::CrowdyStudioGitHubTree tree;
    tree.commitSha = value["commitSha"].asString();
    value["entries"].forEach([&](const graphql::Json& entry) {
      studio::CrowdyStudioGitHubTreeEntry item;
      item.path = entry["path"].asString();
      item.type = entry["type"].asString();
      item.sha = optionalString(entry["sha"]);
      if (entry["size"].ok() && !entry["size"].isNull()) {
        item.size = static_cast<int>(entry["size"].asInt64());
      }
      tree.entries.push_back(std::move(item));
    });
    return tree;
  }

  static studio::CrowdyStudioGitHubFile mapFile(const graphql::Json& value) {
    studio::CrowdyStudioGitHubFile file;
    file.path = value["path"].asString();
    file.content = value["content"].asString();
    file.sha = value["sha"].asString();
    file.commitSha = optionalString(value["commitSha"]);
    return file;
  }
};

}  // namespace crowdy::domains
