#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "crowdy/kit/protocol.hpp"
#include "crowdy/kit/runtime.hpp"

/// Hub topics mirrored on the client. CrowdyJS `src/kit/mirror.ts`.
namespace crowdy::kit {

/// A hub topic mirrored on the client: it subscribes, asks the hub for a snapshot, then applies
/// what arrives after the snapshot's `seq`. It asks again when the publishing instance's
/// `epoch` changes, when a collection topic skips a `seq`, and after the connection
/// reconnects. Listeners run through the dispatcher, like every kit callback.
class TopicMirror {
 public:
  virtual ~TopicMirror();
  TopicMirror(const TopicMirror&) = delete;
  TopicMirror& operator=(const TopicMirror&) = delete;

  /// Subscribes and takes the first snapshot; `done` fires with the snapshot's reply (or the
  /// failure). Idempotent: once started, `done` fires at once with Ok.
  void start(KitCallback done = {});
  /// Stops the subscription; the last state stays readable.
  void stop();
  /// Takes a fresh snapshot (concurrent callers share one).
  void resync(KitCallback done = {});
  bool started() const;
  /// The epoch and seq the mirror is at; `-1` before its first snapshot.
  std::pair<std::int64_t, std::int64_t> position() const;

  /// Called after every change of the state.
  std::uint64_t subscribe(std::function<void()> listener);
  /// `event` messages on the topic.
  std::uint64_t onEvent(std::function<void(const graphql::Json& body, const KitHeader& header)> listener);
  /// `changed` messages on the topic: someone's private state to read again.
  std::uint64_t onChanged(std::function<void(const KitChanged& changed, const KitHeader& header)> listener);
  void removeListener(std::uint64_t id);

  const std::string& nodeType() const;
  const std::string& key() const;
  const std::string& topic() const;

 protected:
  enum class Shape { Snapshots, Collection };
  TopicMirror(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key, std::string topic, Shape shape);

  struct Core;
  std::shared_ptr<Core> core_;
};

/// A topic whose messages are its hub's whole public state (`snapshot` messages).
class HubState : public TopicMirror {
 public:
  HubState(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key, std::string topic);
  /// The last snapshot's body; a null Json before the first.
  graphql::Json get() const;
};

/// A topic carrying a collection (`delta` per member; the snapshot's `containers`): the
/// starters' model topics, such as an area's mobs, NPCs and pets.
class CollectionMirror : public TopicMirror {
 public:
  CollectionMirror(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key, std::string topic);
  /// The members, oldest first, of one type or all.
  std::vector<KitContainer> list(std::string_view typeName = {}) const;
  /// One member by id.
  std::optional<KitContainer> member(std::int64_t id) const;
  std::size_t size() const;
};

/// State only its player may read (a hand of cards, an inventory, a pet's bond): read with a
/// caller-scoped call, read again when the hub's `changed` message names this player, and after
/// a reconnect. Nothing private is ever on a topic.
class PrivateState {
 public:
  /// `read` reads the value with a call the hub answers for its caller alone. `whats` are the
  /// `changed` messages' `what` that concern this value.
  PrivateState(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key, std::string topic,
               std::vector<std::string> whats, std::function<void(KitCallback)> read);
  ~PrivateState();
  PrivateState(const PrivateState&) = delete;
  PrivateState& operator=(const PrivateState&) = delete;

  /// Subscribes and reads the first value. Idempotent.
  void start(KitCallback done = {});
  void stop();
  /// Reads the value again (one read at a time; a request during one runs after it).
  void refresh(KitCallback done = {});
  /// The last value read; a null Json before the first.
  graphql::Json get() const;
  std::uint64_t subscribe(std::function<void()> listener);
  void removeListener(std::uint64_t id);

 private:
  struct Core;
  std::shared_ptr<Core> core_;
};

/// The kit's observable value: `get` the current value, `subscribe` to hear each new one.
/// CrowdyJS `WritableKitStore`.
template <typename T>
class WritableKitStore {
 public:
  explicit WritableKitStore(T initial) : value_(std::move(initial)) {}

  T get() const {
    std::lock_guard lock(mu_);
    return value_;
  }

  std::uint64_t subscribe(std::function<void(const T&)> listener) {
    std::lock_guard lock(mu_);
    listeners_.emplace_back(++next_, std::move(listener));
    return next_;
  }

  void removeListener(std::uint64_t id) {
    std::lock_guard lock(mu_);
    std::erase_if(listeners_, [id](const auto& l) { return l.first == id; });
  }

  void set(T value) {
    std::vector<std::function<void(const T&)>> copies;
    {
      std::lock_guard lock(mu_);
      value_ = std::move(value);
      for (const auto& [id, l] : listeners_) copies.push_back(l);
    }
    const T now = get();
    for (auto& l : copies) l(now);
  }

 private:
  mutable std::mutex mu_;
  T value_;
  std::vector<std::pair<std::uint64_t, std::function<void(const T&)>>> listeners_;
  std::uint64_t next_ = 0;
};

}  // namespace crowdy::kit
