#pragma once

#include <functional>
#include <mutex>
#include <string>

/// Finding another endpoint when the one you hold has stopped answering.
///
/// Under direct connect `gameApiUrl` names ONE ck-api instance, which is what
/// keeps gameplay off the shared balancer. The cost is that a client is pinned:
/// when that instance goes away, retrying its hostname can never recover, and
/// the symptom is a session that connects and then carries nothing. Re-discovery
/// asks something that does NOT die with it — the shared origin — where to go.
///
/// Mirrors CrowdyJS src/rediscover.ts and src/bootstrap-rediscover.ts.
namespace crowdy::graphql {

/// A re-discovery answer. Empty fields mean "no answer", not "clear the URL".
struct RediscoveredEndpoint {
  std::string httpUrl;
  std::string wsUrl;

  bool empty() const { return httpUrl.empty() && wsUrl.empty(); }
};

/// Given the app being played (may be empty), where should the client go?
///
/// MUST NOT throw and MUST NOT block indefinitely. Returning an empty result
/// leaves the client on its current endpoint with its normal retry, which is
/// strictly better than failing out of a reconnect path.
using RediscoverFn = std::function<RediscoveredEndpoint(const std::string&)>;

/// Serialises re-discovery so concurrent triggers share one attempt.
///
/// Coalescing is not an optimisation. Several things notice a dead endpoint at
/// once — the subscription socket, the UDP watchdog, a failed request — and
/// without this they would each ask, then each apply a possibly different
/// answer, moving the client repeatedly while it is already struggling.
class RediscoverCoordinator {
 public:
  /// Consecutive failures before re-discovery is worth trying. Matches
  /// CrowdyJS's rediscoverAfterFailures default: a single blip is what ordinary
  /// reconnect backoff is for, and moving on the first one would relocate
  /// clients across a datacenter on any transient loss.
  static constexpr int kDefaultAfterFailures = 3;

  void setCallback(RediscoverFn fn) {
    std::lock_guard lock(mutex_);
    callback_ = std::move(fn);
  }

  bool hasCallback() const {
    std::lock_guard lock(mutex_);
    return static_cast<bool>(callback_);
  }

  /// Run re-discovery unless another caller already is. Returns the answer, or
  /// an empty result when there is no callback, one is already running, or the
  /// callback declined or failed.
  RediscoveredEndpoint attempt(const std::string& appId) {
    RediscoverFn callback;
    {
      std::lock_guard lock(mutex_);
      if (inFlight_) return {};
      if (!callback_) return {};
      callback = callback_;
      inFlight_ = true;
    }
    RediscoveredEndpoint result;
#ifndef CROWDY_NO_EXCEPTIONS
    // A throwing callback must not escape into a reconnect path: the caller is
    // already handling a failure and has nowhere to put a second one.
    try {
      result = callback(appId);
    } catch (...) {
      result = {};
    }
#else
    result = callback(appId);
#endif
    {
      std::lock_guard lock(mutex_);
      inFlight_ = false;
    }
    return result;
  }

 private:
  mutable std::mutex mutex_;
  RediscoverFn callback_;
  bool inFlight_ = false;
};

}  // namespace crowdy::graphql
