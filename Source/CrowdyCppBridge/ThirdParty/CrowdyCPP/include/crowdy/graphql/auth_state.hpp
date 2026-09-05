#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// Shared auth state observed by the HTTP client and the replication client,
/// so credentials never drift within one CrowdyClient. Mirrors CrowdyJS's
/// AuthState + token store model.
namespace crowdy::graphql {

/// Pluggable token persistence (the CrowdyJS TokenStore analog).
class ITokenStore {
 public:
  virtual ~ITokenStore() = default;
  virtual std::string load() = 0;
  virtual void save(const std::string& token) = 0;
  virtual void clear() = 0;
};

class InMemoryTokenStore final : public ITokenStore {
 public:
  std::string load() override { return token_; }
  void save(const std::string& token) override { token_ = token; }
  void clear() override { token_.clear(); }

 private:
  std::string token_;
};

/// Stores the token in a file (0600). Suitable for native game installs.
///
/// Use sessionPath()/appPath() to name the file rather than inventing a scheme.
/// The two credentials have different scopes and storing them as though they had
/// the same one is a real bug with a confusing symptom — see sessionPath().
class FileTokenStore final : public ITokenStore {
 public:
  explicit FileTokenStore(std::string path) : path_(std::move(path)) {}
  std::string load() override;
  void save(const std::string& token) override;
  void clear() override;

  /// Filename stem for an IDENTITY session token: one per ORIGIN, shared by
  /// every game that talks to it.
  ///
  /// WHY THIS EXISTS AS A NAMED HELPER. Browser games were storing their
  /// credential under the game's own PATH, so two games on one origin had
  /// different keys and could not see each other's login: a player who signed in
  /// for one game was anonymous to the next and got bounced back to the portal.
  /// What was stored was an app token anyway, which is per-game by definition, so
  /// there was nothing cross-app to share even if the keys had matched.
  ///
  /// A native install has the same two credentials and can make the same
  /// mistake, so it gets the same split: the session token is keyed by origin
  /// and nothing else. Deliberately NOT parameterised by anything per-game —
  /// that parameter was the bug.
  static std::string sessionPath(std::string_view directory,
                                 std::string_view apiOrigin) {
    return join(directory, "crowdy-session-" + slug(apiOrigin));
  }

  /// Filename stem for an app-scoped GAMEPLAY token. Per app, on purpose: it
  /// authorises one app and cannot be shared with another.
  static std::string appPath(std::string_view directory,
                             std::string_view appId) {
    return join(directory, "crowdy-app-" + slug(appId));
  }

 private:
  /// Filesystem-safe rendering of an origin or id. Keeps the host recognisable
  /// so an operator can tell which file belongs to what.
  static std::string slug(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    // Drop a scheme so https://ck.example.com and ck.example.com agree; a
    // difference there would silently split one origin into two sessions.
    const std::size_t scheme = value.find("://");
    if (scheme != std::string_view::npos) value = value.substr(scheme + 3);
    for (const char c : value) {
      const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                        c == '_';
      out.push_back(safe ? c : '_');
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
  }

  static std::string join(std::string_view directory, std::string name) {
    if (directory.empty()) return name;
    std::string path(directory);
    if (path.back() != '/' && path.back() != '\\') path.push_back('/');
    return path + name;
  }

  std::string path_;
};

class AuthState {
 public:
  explicit AuthState(std::shared_ptr<ITokenStore> store = nullptr)
      : store_(store ? std::move(store) : std::make_shared<InMemoryTokenStore>()) {}

  std::string token() const {
    std::lock_guard lock(mutex_);
    return token_;
  }
  bool hasToken() const {
    std::lock_guard lock(mutex_);
    return !token_.empty();
  }

  void setToken(const std::string& token);
  void clearToken();
  /// Load a previously persisted token from the store.
  bool restore();

  /// Observe token changes (empty string = cleared). Called with the new
  /// token outside the internal lock.
  void onChange(std::function<void(const std::string&)> listener);

 private:
  mutable std::mutex mutex_;
  std::string token_;
  std::shared_ptr<ITokenStore> store_;
  std::vector<std::function<void(const std::string&)>> listeners_;
};

}  // namespace crowdy::graphql
