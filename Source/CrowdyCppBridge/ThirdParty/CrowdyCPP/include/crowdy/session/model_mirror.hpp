#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "crowdy/client.hpp"
#include "crowdy/core/clock.hpp"

/// ContainerMirror — the client half of the platform's notify-to-pull
/// pattern for game-model state: keep snapshots of the containers you care
/// about, re-pull them on demand or whenever a bound channel pings ("state
/// changed"), and render straight from the cache.
///
/// Model changes are pull-based on this platform (there is no model
/// subscription); functions declare channel/spatial notify effects and
/// clients re-read. Bind the mirror to that channel (via the WorldSession
/// channel inbox or a Connection channel handler feeding notifyChannelPing)
/// and every watched container refreshes itself.
namespace crowdy::session {

/// A snapshot of one watched container.
struct MirroredContainer {
  std::string containerId;
  std::string typeName;
  std::string displayName;
  std::string ownerUserId;
  /// The parsed, caller-visible properties (JSON object).
  graphql::Json value;
  /// Bumped on every refresh that changed the snapshot.
  int revision = 0;
  /// Monotonic time of the last refresh.
  std::int64_t refreshedAtMs = 0;
};

class ContainerMirror {
 public:
  ContainerMirror(CrowdyClient& client, std::string appId)
      : client_(client), appId_(std::move(appId)) {}

  /// Watch a container: pulls the initial snapshot immediately and again on
  /// every refresh()/refreshAll()/bound-channel ping.
  const MirroredContainer& watch(std::string_view containerId) {
    auto [it, inserted] = watched_.try_emplace(std::string(containerId));
    if (inserted) refreshOne(it->second, std::string(containerId));
    return it->second;
  }

  void unwatch(std::string_view containerId) { watched_.erase(std::string(containerId)); }

  /// Current snapshot (nullptr when not watched).
  const MirroredContainer* get(std::string_view containerId) const {
    auto it = watched_.find(std::string(containerId));
    return it == watched_.end() ? nullptr : &it->second;
  }

  /// Snapshot of every watched container.
  std::vector<const MirroredContainer*> list() const {
    std::vector<const MirroredContainer*> out;
    out.reserve(watched_.size());
    for (const auto& [id, c] : watched_) out.push_back(&c);
    return out;
  }

  /// Re-pull one container now. Returns true when the snapshot changed.
  bool refresh(std::string_view containerId) {
    auto it = watched_.find(std::string(containerId));
    if (it == watched_.end()) return false;
    return refreshOne(it->second, std::string(containerId));
  }

  /// Re-pull every watched container.
  std::size_t refreshAll() {
    std::size_t changed = 0;
    for (auto& [id, c] : watched_) {
      if (refreshOne(c, id)) ++changed;
    }
    return changed;
  }

  /// Observe snapshot changes (fired from refresh paths).
  void onChange(std::function<void(const MirroredContainer&)> cb) { onChange_ = std::move(cb); }

  /// Bind to a channel id: feed channel notifications (e.g. from the
  /// WorldSession channel inbox) into notifyChannelPing and every watched
  /// container refreshes when that channel pings.
  void bindToChannel(std::int64_t channelId) { boundChannelId_ = channelId; }

  /// A channel message arrived; refreshes everything when it is the bound
  /// channel. Returns the number of changed snapshots.
  std::size_t notifyChannelPing(std::int64_t channelId) {
    if (boundChannelId_ == 0 || channelId != boundChannelId_) return 0;
    return refreshAll();
  }

 private:
  bool refreshOne(MirroredContainer& snapshot, const std::string& containerId) {
    try {
      graphql::Json container = client_.gameModel().container(appId_, containerId);
      graphql::Json state = client_.gameModel().containerState(appId_, containerId);
      graphql::Json value = graphql::Json::parse(state["propertiesJson"].asStringView());
      const std::string newDump = value.dump();
      const bool changed = snapshot.revision == 0 || newDump != snapshot.value.dump();
      snapshot.containerId = containerId;
      snapshot.typeName = container["typeName"].asString();
      snapshot.displayName = container["displayName"].asString();
      snapshot.ownerUserId = container["ownerUserId"].asString();
      snapshot.value = value;
      snapshot.refreshedAtMs = core::systemClock().monotonicMillis();
      if (changed) {
        ++snapshot.revision;
        if (onChange_) onChange_(snapshot);
      }
      return changed;
    } catch (const std::exception&) {
      return false;  // transient failure keeps the last snapshot
    }
  }

  CrowdyClient& client_;
  std::string appId_;
  std::map<std::string, MirroredContainer> watched_;
  std::int64_t boundChannelId_ = 0;
  std::function<void(const MirroredContainer&)> onChange_;
};

}  // namespace crowdy::session
