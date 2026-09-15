#pragma once

#include <optional>
#include <string>
#include <vector>

#include "crowdy/generated/enums.hpp"
#include "crowdy/studio/github_layout.hpp"

/// Typed GitHub-backed Crowdy Studio project surface. Same GraphQL client and
/// session as everything else: the game API resolves the repository from the
/// project's bind, so no read or write here names an owner/repo except bind,
/// and no GitHub token ever reaches this SDK. GitHub is never required — a
/// project starts in Crowdy Studio and may be bound later.
namespace crowdy::studio {

struct CrowdyStudioGitHubStatus {
  bool configured = false;
  bool connected = false;
  std::optional<std::string> accountLogin;
  std::optional<std::string> accountType;
  std::optional<std::string> owner;
  std::optional<std::string> repo;
  std::optional<std::string> branch;
  /// Commit the project mirror is at; nullopt when the project is not bound.
  std::optional<std::string> githubSha;
  std::optional<std::string> installUrl;
};

struct CrowdyStudioGitHubRepo {
  std::string owner;
  std::string name;
  std::string fullName;
  bool isPrivate = false;
  std::optional<std::string> defaultBranch;
};

struct CrowdyStudioGitHubTreeEntry {
  std::string path;
  /// `blob` or `tree`.
  std::string type;
  std::optional<std::string> sha;
  std::optional<int> size;
};

struct CrowdyStudioGitHubTree {
  std::string commitSha;
  std::vector<CrowdyStudioGitHubTreeEntry> entries;
};

struct CrowdyStudioGitHubFile {
  std::string path;
  std::string content;
  std::string sha;
  /// Commit the file was read at, or the commit a write created.
  std::optional<std::string> commitSha;
};

struct CrowdyStudioGitHubLayout {
  std::string commitSha;
  /// Directory of the SERVER Cargo.toml; `.` is the repository root.
  std::string server;
  /// Directory of the CLIENT Cargo.toml, or nullopt when server-only.
  std::optional<std::string> client;
  std::string assets;
  bool fromFile = false;

  CrowdyStudioGitHubLayoutRoots roots() const { return {server, client}; }
};

struct CrowdyStudioGitHubProjectScope {
  std::string appId;
  std::string projectId;
};

struct CrowdyStudioGitHubBindInput {
  std::string appId;
  std::string projectId;
  std::string owner;
  std::string repo;
  std::optional<std::string> branch;
  gen::CrowdyStudioGitHubBindInitial initial =
      gen::CrowdyStudioGitHubBindInitial::PUSH_PROJECT;
};

struct CrowdyStudioGitHubPutFileInput {
  std::string appId;
  std::string projectId;
  std::string path;
  std::string content;
  std::string message;
  std::string expectedCommitSha;
  std::optional<std::string> sha;
};

struct CrowdyStudioGitHubDeleteFileInput {
  std::string appId;
  std::string projectId;
  std::string path;
  std::string message;
  std::string expectedCommitSha;
  std::optional<std::string> sha;
};

}  // namespace crowdy::studio
