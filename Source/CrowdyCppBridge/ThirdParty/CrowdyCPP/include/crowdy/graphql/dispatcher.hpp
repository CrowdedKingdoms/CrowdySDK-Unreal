#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

/// Completion pump for the async API layer. Async transports may finish on any
/// thread; posting through a Dispatcher and calling drain() from the game
/// thread makes GraphQL callbacks fire where engine objects are safe to touch.
/// This mirrors the replication layer's pump()/poll() model for the API layer.
namespace crowdy::graphql {

class Dispatcher {
 public:
  /// Enqueue a completion. Thread-safe; callable from any thread.
  void post(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return;
    queue_.push_back(std::move(fn));
  }

  /// Run and clear every queued callback on the calling thread, returning the
  /// number run. The queue is swapped out before running, so callbacks posted
  /// during a drain are deferred to the next drain rather than run re-entrantly.
  std::size_t drain() {
    std::lock_guard<std::recursive_mutex> drainLock(drainMutex_);
    std::vector<std::function<void()>> pending;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) return 0;
      pending.swap(queue_);
    }
    std::size_t dispatched = 0;
    for (auto& fn : pending) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) break;
      }
      fn();
      ++dispatched;
    }
    return dispatched;
  }

  /// Terminally cancel queued and future callbacks. If drain() is currently
  /// invoking one on another thread, wait until it returns. Calling close()
  /// from inside a callback is supported and suppresses the rest of the batch.
  void close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
      queue_.clear();
    }
    // Barrier against a callback already executing on another thread.
    std::lock_guard<std::recursive_mutex> drainLock(drainMutex_);
  }

  bool closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

 private:
  mutable std::mutex mutex_;
  std::recursive_mutex drainMutex_;
  std::vector<std::function<void()>> queue_;
  bool closed_ = false;
};

}  // namespace crowdy::graphql
