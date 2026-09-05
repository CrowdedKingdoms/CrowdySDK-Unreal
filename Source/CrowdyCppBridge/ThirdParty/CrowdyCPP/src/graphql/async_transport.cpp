#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

#include "crowdy/graphql/errors.hpp"
#include "crowdy/graphql/http.hpp"

namespace crowdy::graphql {

HttpOutcome IHttpTransport::sendOutcome(const HttpRequest& request) noexcept {
  HttpOutcome out;
#ifndef CROWDY_NO_EXCEPTIONS
  try {
    out.response = send(request);
    out.status = Errc::Ok;
  } catch (const CrowdyTimeoutError& error) {
    out.status = Errc::Timeout;
    out.errorMessage = error.what();
  } catch (const std::exception& error) {
    out.status = Errc::SocketError;
    out.errorMessage = error.what();
  } catch (...) {
    out.status = Errc::SocketError;
    out.errorMessage = "HTTP transport failed with an unknown exception";
  }
#else
  out.response = send(request);
  out.status = Errc::Ok;
#endif
  return out;
}

namespace {

class InlineAsyncTransport final : public IAsyncHttpTransport {
 public:
  explicit InlineAsyncTransport(std::shared_ptr<IHttpTransport> sync) : sync_(std::move(sync)) {}

  void sendAsync(const HttpRequest& request, std::function<void(HttpOutcome)> cb) override {
    cb(sync_ ? sync_->sendOutcome(request)
             : HttpOutcome{Errc::NotConnected, {},
                           "No HTTP transport is available"});
  }

 private:
  std::shared_ptr<IHttpTransport> sync_;
};

class ThreadedAsyncTransport final : public IAsyncHttpTransport {
 public:
  explicit ThreadedAsyncTransport(std::shared_ptr<IHttpTransport> sync)
      : sync_(std::move(sync)),
        worker_([this] { run(); }) {}

  ~ThreadedAsyncTransport() override {
    std::deque<Work> abandoned;
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      abandoned.swap(queue_);
    }
    condition_.notify_one();
    for (auto& work : abandoned) {
      work.callback(HttpOutcome{
          Errc::Closed, {},
          "Async HTTP transport stopped before request execution"});
    }
    if (worker_.joinable()) worker_.join();
  }

  void sendAsync(const HttpRequest& request,
                 std::function<void(HttpOutcome)> callback) override {
    if (!callback) return;
    {
      std::lock_guard lock(mutex_);
      if (!stopping_) {
        queue_.push_back(Work{request, std::move(callback)});
        condition_.notify_one();
        return;
      }
    }
    callback(HttpOutcome{
        Errc::Closed, {},
        "Async HTTP transport is stopping"});
  }

 private:
  struct Work {
    HttpRequest request;
    std::function<void(HttpOutcome)> callback;
  };

  void run() {
    while (true) {
      Work work;
      {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] {
          return stopping_ || !queue_.empty();
        });
        if (stopping_ && queue_.empty()) return;
        work = std::move(queue_.front());
        queue_.pop_front();
      }
      work.callback(
          sync_ ? sync_->sendOutcome(work.request)
                : HttpOutcome{Errc::NotConnected, {},
                              "No HTTP transport is available"});
    }
  }

  std::shared_ptr<IHttpTransport> sync_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<Work> queue_;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace

std::shared_ptr<IAsyncHttpTransport> makeInlineAsyncTransport(
    std::shared_ptr<IHttpTransport> sync) {
  return std::make_shared<InlineAsyncTransport>(std::move(sync));
}

std::shared_ptr<IAsyncHttpTransport> makeThreadedAsyncTransport(
    std::shared_ptr<IHttpTransport> sync) {
  return std::make_shared<ThreadedAsyncTransport>(std::move(sync));
}

}  // namespace crowdy::graphql
