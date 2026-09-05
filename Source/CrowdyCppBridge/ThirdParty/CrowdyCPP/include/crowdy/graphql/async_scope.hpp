#pragma once

#include <functional>
#include <mutex>
#include <utility>

namespace crowdy::graphql {

/// Synchronized lifetime fence for callbacks retained by asynchronous
/// transports. close() is terminal: callbacks that have not started are
/// suppressed, and it waits for a callback already running on another thread.
///
/// The recursive mutex permits an owner to be destroyed from its own user
/// callback. Callers must not access owner state after run() invokes user code.
class AsyncScope {
 public:
  AsyncScope() = default;

  AsyncScope(const AsyncScope&) = delete;
  AsyncScope& operator=(const AsyncScope&) = delete;

  template <typename Fn>
  bool run(Fn&& fn) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!open_) return false;
    std::invoke(std::forward<Fn>(fn));
    return true;
  }

  void close() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    open_ = false;
  }

  bool open() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return open_;
  }

 private:
  mutable std::recursive_mutex mutex_;
  bool open_ = true;
};

}  // namespace crowdy::graphql
