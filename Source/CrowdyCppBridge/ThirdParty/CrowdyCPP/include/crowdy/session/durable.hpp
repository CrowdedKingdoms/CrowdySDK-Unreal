#pragma once

#include <atomic>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "crowdy/client.hpp"
#include "crowdy/core/base64.hpp"
#include "crowdy/core/clock.hpp"
#include "crowdy/core/uuid.hpp"

/// Durable-state stores: the per-user app save blob (SaveStateStore), typed
/// avatar state (AvatarStateStore), and pluggable actor-UUID persistence
/// (IUuidStore). These wrap the GraphQL surfaces with local caches — durable
/// and realtime payloads rarely share a layout, so these carry their own
/// bytes independent of the replication codec.
namespace crowdy::session {

// ---------------------------------------------------------------------------
// Actor-UUID persistence
// ---------------------------------------------------------------------------

/// Where the local actor's persistent identity lives. An actor UUID should
/// survive restarts so other players' registries treat you as the same actor.
class IUuidStore {
 public:
  virtual ~IUuidStore() = default;
  virtual std::optional<core::ActorUuid> load() = 0;
  virtual void save(const core::ActorUuid& uuid) = 0;
};

class MemoryUuidStore final : public IUuidStore {
 public:
  std::optional<core::ActorUuid> load() override { return uuid_; }
  void save(const core::ActorUuid& uuid) override { uuid_ = uuid; }

 private:
  std::optional<core::ActorUuid> uuid_;
};

/// Persists the uuid in a file (the native analog of CrowdyJS's
/// localStorage-backed store).
class FileUuidStore final : public IUuidStore {
 public:
  explicit FileUuidStore(std::string path) : path_(std::move(path)) {}

  std::optional<core::ActorUuid> load() override {
    std::ifstream in(path_);
    std::string line;
    if (!in || !std::getline(in, line)) return std::nullopt;
    core::ActorUuid uuid;
    if (!core::actorUuidFromString(line, uuid)) return std::nullopt;
    return uuid;
  }

  void save(const core::ActorUuid& uuid) override {
    std::ofstream out(path_, std::ios::trunc);
    out << core::toString(uuid);
  }

 private:
  std::string path_;
};

/// Load a persisted uuid or mint + persist a fresh one.
inline core::ActorUuid ensureActorUuid(IUuidStore& store) {
  if (auto existing = store.load()) return *existing;
  core::ActorUuid fresh = core::generateActorUuid();
  store.save(fresh);
  return fresh;
}

// ---------------------------------------------------------------------------
// SaveStateStore — the per-user per-app save blob
// ---------------------------------------------------------------------------

/// Local cache over client.state(): explicit load()/save() with the raw
/// bytes held locally between round trips. The wire form is base64; this
/// store deals in bytes.
class SaveStateStore {
 public:
  SaveStateStore(CrowdyClient& client, std::string appId)
      : client_(client), appId_(std::move(appId)) {}

  /// Fetch the blob from the server into the cache (empty when no row).
  const std::vector<std::uint8_t>& load() {
    graphql::Json row = client_.state().getOne(appId_);
    std::vector<std::uint8_t> loaded;
    if (row.ok() && !row.isNull()) {
      if (auto bytes = core::base64Decode(row["state"].asStringView())) {
        loaded = std::move(*bytes);
      }
    }
    std::lock_guard lock(mutex_);
    cache_ = std::move(loaded);
    dirty_ = false;
    loaded_ = true;
    return cache_;
  }

  /// Replace the cached blob and persist it.
  void save(Bytes bytes) {
    std::vector<std::uint8_t> next(bytes.begin(), bytes.end());
    graphql::JVal input;
    input["appId"] = appId_;
    input["state"] = core::base64Encode(bytes);
    client_.state().update(input);
    std::lock_guard lock(mutex_);
    cache_ = std::move(next);
    dirty_ = false;
    lastSavedAt_ = core::systemClock().epochMillis();
    loaded_ = true;
  }

  /// Persist the current cache (after in-place edits via value()).
  void save() {
    std::vector<std::uint8_t> snapshot;
    {
      std::lock_guard lock(mutex_);
      snapshot = cache_;
    }
    save(Bytes(snapshot.data(), snapshot.size()));
  }

  /// Replace the local cache and mark it dirty. save() persists explicitly.
  void set(Bytes bytes) {
    std::lock_guard lock(mutex_);
    cache_.assign(bytes.begin(), bytes.end());
    dirty_ = true;
    loaded_ = true;
  }

  /// Overwrite a byte range of the local cache and mark it dirty (simple
  /// patch analog; grows the cache as needed).
  void patch(std::size_t offset, Bytes bytes) {
    std::lock_guard lock(mutex_);
    if (cache_.size() < offset + bytes.size()) cache_.resize(offset + bytes.size());
    std::copy(bytes.begin(), bytes.end(), cache_.begin() + static_cast<std::ptrdiff_t>(offset));
    dirty_ = true;
    loaded_ = true;
  }

  bool loaded() const {
    std::lock_guard lock(mutex_);
    return loaded_;
  }
  bool dirty() const {
    std::lock_guard lock(mutex_);
    return dirty_;
  }
  std::optional<std::int64_t> lastSavedAt() const {
    std::lock_guard lock(mutex_);
    return lastSavedAt_;
  }
  std::vector<std::uint8_t> snapshot() const {
    std::lock_guard lock(mutex_);
    return cache_;
  }
  /// Game-thread mutable compatibility view. Prefer set()/snapshot() when
  /// state may be observed across threads.
  std::vector<std::uint8_t>& value() { return cache_; }
  const std::vector<std::uint8_t>& value() const { return cache_; }

 private:
  CrowdyClient& client_;
  std::string appId_;
  std::vector<std::uint8_t> cache_;
  bool loaded_ = false;
  bool dirty_ = false;
  std::optional<std::int64_t> lastSavedAt_;
  mutable std::mutex mutex_;
};

// ---------------------------------------------------------------------------
// AvatarStateStore — typed avatar state (identity + per-app)
// ---------------------------------------------------------------------------

/// Local cache over client.avatars() for one avatar: the identity-level
/// state blob and the per-app state blob, loaded/saved explicitly.
class AvatarStateStore {
 public:
  AvatarStateStore(CrowdyClient& client, std::string appId, std::string avatarId)
      : client_(client), appId_(std::move(appId)), avatarId_(std::move(avatarId)) {}

  const std::string& avatarId() const { return avatarId_; }

  /// Fetch both blobs (identity-level publicState + per-app state) into the
  /// cache.
  void load() {
    graphql::Json avatar = client_.avatars().get(avatarId_);
    std::vector<std::uint8_t> identity;
    std::vector<std::uint8_t> privateState;
    if (auto bytes = core::base64Decode(avatar["publicState"].asStringView()))
      identity = std::move(*bytes);
    if (auto bytes = core::base64Decode(avatar["privateState"].asStringView()))
      privateState = std::move(*bytes);
    graphql::Json appState = client_.avatars().appState(appId_, avatarId_);
    std::vector<std::uint8_t> app;
    if (appState.ok() && !appState.isNull()) {
      if (auto bytes = core::base64Decode(appState["state"].asStringView())) {
        app = std::move(*bytes);
      }
    }
    std::lock_guard lock(mutex_);
    identityState_ = std::move(identity);
    privateState_ = std::move(privateState);
    appState_ = std::move(app);
    loaded_ = true;
  }

  /// Replace + persist the avatar's identity-level public state blob.
  void setIdentityState(Bytes bytes) {
    graphql::JVal input;
    input["publicState"] = core::base64Encode(bytes);
    client_.avatars().updateState(avatarId_, input);
    std::lock_guard lock(mutex_);
    identityState_.assign(bytes.begin(), bytes.end());
  }

  /// Replace + persist the owner-only identity state blob.
  void setPrivateState(Bytes bytes) {
    graphql::JVal input;
    input["privateState"] = core::base64Encode(bytes);
    client_.avatars().updateState(avatarId_, input);
    std::lock_guard lock(mutex_);
    privateState_.assign(bytes.begin(), bytes.end());
  }

  /// Replace + persist the avatar's per-app state blob.
  void setAppState(Bytes bytes) {
    graphql::JVal input;
    input["appId"] = appId_;
    input["avatarId"] = avatarId_;
    input["state"] = core::base64Encode(bytes);
    client_.avatars().updateAppState(input);
    std::lock_guard lock(mutex_);
    appState_.assign(bytes.begin(), bytes.end());
  }

  bool loaded() const {
    std::lock_guard lock(mutex_);
    return loaded_;
  }
  /// Owner-only state snapshot; callers receive no mutable cache authority.
  std::vector<std::uint8_t> privateState() const {
    std::lock_guard lock(mutex_);
    return privateState_;
  }
  const std::vector<std::uint8_t>& identityState() const { return identityState_; }
  const std::vector<std::uint8_t>& appState() const { return appState_; }

 private:
  CrowdyClient& client_;
  std::string appId_;
  std::string avatarId_;
  std::vector<std::uint8_t> identityState_;
  std::vector<std::uint8_t> privateState_;
  std::vector<std::uint8_t> appState_;
  bool loaded_ = false;
  mutable std::mutex mutex_;
};

}  // namespace crowdy::session
