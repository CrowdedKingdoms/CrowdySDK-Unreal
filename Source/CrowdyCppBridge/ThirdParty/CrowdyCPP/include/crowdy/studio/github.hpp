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
  /// What the repository is at that commit: a Studio mod's crates (STUDIO_CRATES) or a game
  /// made from the Construct template (CONSTRUCT_GAME), whose editor is browser-only. Unset
  /// from an API older than v2.59.0, or for a kind this SDK does not know.
  std::optional<gen::CrowdyStudioGitHubLayoutKind> kind;

  CrowdyStudioGitHubLayoutRoots roots() const { return {server, client}; }
};

/// One file of a batched read. `content` is unset when the blob is not UTF-8 text or is larger
/// than the tier reads; `sha` and `size` are always set.
struct CrowdyStudioGitHubFileContent {
  std::string path;
  std::string sha;
  int size = 0;
  bool binary = false;
  std::optional<std::string> content;
};

/// Up to 100 files of the bound repository at one commit, read in one call.
struct CrowdyStudioGitHubFiles {
  std::string commitSha;
  std::vector<CrowdyStudioGitHubFileContent> files;
  /// Requested paths that are not files at that commit.
  std::vector<std::string> missing;
};

/// The commit a multi-file write created; every change in it landed, or none did.
struct CrowdyStudioGitHubCommit {
  std::string commitSha;
  /// The expectedCommitSha it builds on.
  std::string parentSha;
  std::vector<std::string> written;
  std::vector<std::string> deleted;
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

/// Read up to 100 files at one commit (default: the project's githubSha, never the branch
/// head).
struct CrowdyStudioGitHubFilesInput {
  std::string appId;
  std::string projectId;
  std::vector<std::string> paths;
  std::optional<std::string> commitSha;
};

/// One change of a multi-file commit: `content` writes the path, `remove` deletes it.
struct CrowdyStudioGitHubCommitChange {
  std::string path;
  std::optional<std::string> content;
  bool remove = false;
};

/// Write and delete up to 100 files as ONE commit on top of `expectedCommitSha` (the project's
/// githubSha). A moved project or branch is refused with GITHUB_STALE_SHA and nothing is
/// written.
struct CrowdyStudioGitHubCommitFilesInput {
  std::string appId;
  std::string projectId;
  std::string message;
  std::string expectedCommitSha;
  std::vector<CrowdyStudioGitHubCommitChange> changes;
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
