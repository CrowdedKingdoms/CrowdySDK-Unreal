#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "crowdy/studio/models.hpp"

/// Path arithmetic between a Crowdy Studio project (per-target files such as
/// `SERVER src/lib.rs`) and the bound repository, driven by the layout the
/// game API resolved (`crowdyStudioGitHubLayout`). This module does NOT parse
/// `crowdy.json`: the API is the only layout grammar.
namespace crowdy::studio {

struct CrowdyStudioGitHubLayoutRoots {
  std::string server;
  std::optional<std::string> client;
};

inline std::string trimSlash(std::string_view path) {
  std::size_t start = 0;
  std::size_t end = path.size();
  while (start < end && path[start] == '/') ++start;
  while (end > start && path[end - 1] == '/') --end;
  return std::string(path.substr(start, end - start));
}

inline std::string joinRepo(std::string_view root, std::string_view rel) {
  const std::string base = trimSlash(root);
  const std::string rest = trimSlash(rel);
  if (base.empty() || base == ".") return rest;
  return rest.empty() ? base : base + "/" + rest;
}

inline std::optional<std::string> underRoot(
    std::string_view path, const std::optional<std::string>& root) {
  if (!root) return std::nullopt;
  const std::string base = trimSlash(*root);
  if (base.empty() || base == ".") return std::string(path);
  if (path == base) return std::string{};
  const std::string prefix = base + "/";
  if (path.size() >= prefix.size() &&
      path.substr(0, prefix.size()) == prefix) {
    return std::string(path.substr(prefix.size()));
  }
  return std::nullopt;
}

/// `Cargo.toml` or a `.rs` under `src/`, no traversal — what Crowdy Studio
/// compiles.
inline bool isRustAuthoringPath(std::string_view rel) {
  if (rel.empty() || rel.find("..") != std::string_view::npos ||
      rel.front() == '/') {
    return false;
  }
  if (rel == "Cargo.toml") return true;
  if (rel == "Cargo.lock" || rel == "build.rs") return false;
  if (rel.size() >= 9 && rel.substr(rel.size() - 9) == "/build.rs") {
    return false;
  }
  return rel.size() > 4 && rel.substr(0, 4) == "src/" &&
         rel.size() >= 3 && rel.substr(rel.size() - 3) == ".rs";
}

/// The repository path of a project file, or nullopt when the layout has no
/// root for its target.
inline std::optional<std::string> studioFileToRepoPath(
    const CrowdyStudioGitHubLayoutRoots& layout, CrowdyStudioTarget target,
    std::string_view path) {
  if (target == CrowdyStudioTarget::Server) {
    return joinRepo(layout.server, path);
  }
  if (!layout.client) return std::nullopt;
  return joinRepo(*layout.client, path);
}

/// The project file a repository path is, or nullopt when it is not one
/// (README, assets, …).
inline std::optional<std::pair<CrowdyStudioTarget, std::string>>
repoPathToStudioFile(const CrowdyStudioGitHubLayoutRoots& layout,
                     std::string_view repoPath) {
  const auto client = underRoot(repoPath, layout.client);
  if (client && isRustAuthoringPath(*client)) {
    return std::pair{CrowdyStudioTarget::Client, *client};
  }
  const auto server =
      underRoot(repoPath, std::optional<std::string>(layout.server));
  if (server && isRustAuthoringPath(*server)) {
    if (layout.client && (layout.server == "." || layout.server.empty()) &&
        server->rfind(trimSlash(*layout.client) + "/", 0) == 0) {
      return std::nullopt;
    }
    return std::pair{CrowdyStudioTarget::Server, *server};
  }
  return std::nullopt;
}

}  // namespace crowdy::studio
