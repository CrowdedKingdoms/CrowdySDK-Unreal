#pragma once

#include <atomic>
#include <memory>

namespace crowdy::graphql {

/// Cancels the requests it is passed to, from any thread: CrowdyJS's `signal`.
///
/// Copies share one flag, so a token handed to a read can be cancelled by
/// whoever kept a copy (say, when the user opens another session and the page
/// in flight will never be shown). A cancelled request answers
/// GraphQLErrorKind::Cancelled (CrowdyCancelledError from the throwing calls)
/// even when the response had already arrived, and a token cancelled before the
/// call sends nothing.
///
/// The token runs ALONGSIDE the client's timeout; it never replaces it. The
/// default libcurl transport ends the transfer within about a second of
/// cancel(), which closes the request on the server and frees whatever the
/// server held for it (the input log's per-user read share, for one). An
/// injected engine transport sees the token on HttpRequest::cancel and may
/// ignore it, in which case the request runs to completion or its timeout and
/// is then reported as cancelled.
///
/// Cancellation is one-way: a cancelled token stays cancelled. Use a new token
/// for the next request.
class CancelToken {
 public:
  CancelToken() : state_(std::make_shared<std::atomic<bool>>(false)) {}

  void cancel() const noexcept { state_->store(true, std::memory_order_release); }
  bool cancelled() const noexcept { return state_->load(std::memory_order_acquire); }

 private:
  std::shared_ptr<std::atomic<bool>> state_;
};

}  // namespace crowdy::graphql
