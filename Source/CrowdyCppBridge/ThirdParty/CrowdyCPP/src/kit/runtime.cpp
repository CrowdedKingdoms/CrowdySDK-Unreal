#include "crowdy/kit/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace crowdy::kit {

using Clock = std::chrono::steady_clock;
using domains::ExecReply;
using domains::ExecStatus;

class KitRuntime::Impl : public std::enable_shared_from_this<KitRuntime::Impl> {
 public:
  Impl(std::string appId, domains::ExecAPI* exec, std::shared_ptr<graphql::Dispatcher> dispatcher,
       KitRuntimeOptions options)
      : appId_(std::move(appId)),
        exec_(exec),
        dispatcher_(std::move(dispatcher)),
        options_(std::move(options)),
        capacity_(std::max(1, options_.callBudget)),
        windowMs_(std::max(1L, options_.budgetWindowMs)),
        retries_(std::max(0, options_.retries)),
        tokens_(static_cast<double>(capacity_)),
        last_(Clock::now()),
        playerId_(options_.playerId) {}

  ~Impl() { stopTimer(); }

  const std::string appId_;
  domains::ExecAPI* const exec_;
  const std::shared_ptr<graphql::Dispatcher> dispatcher_;
  const KitRuntimeOptions options_;
  const int capacity_;
  const long windowMs_;
  const int retries_;

  // ---- delivery and the timer ----

  void deliver(std::function<void()> fn) {
    if (dispatcher_) {
      dispatcher_->post(std::move(fn));
    } else {
      fn();
    }
  }

  std::uint64_t after(long delayMs, std::function<void()> fn) {
    std::lock_guard lock(tmu_);
    if (stopping_) return 0;
    const std::uint64_t id = ++nextTimer_;
    due_.emplace(Clock::now() + std::chrono::milliseconds(std::max(0L, delayMs)), id);
    fns_.emplace(id, std::move(fn));
    if (!worker_.joinable()) {
      worker_ = std::thread([self = shared_from_this()] { self->run(); });
    }
    tcv_.notify_all();
    return id;
  }

  void cancel(std::uint64_t id) {
    std::lock_guard lock(tmu_);
    fns_.erase(id);
  }

  void run() {
    std::unique_lock lock(tmu_);
    while (!stopping_) {
      if (due_.empty()) {
        tcv_.wait(lock);
        continue;
      }
      const auto next = due_.begin()->first;
      if (Clock::now() < next) {
        tcv_.wait_until(lock, next);
        continue;
      }
      const std::uint64_t id = due_.begin()->second;
      due_.erase(due_.begin());
      auto found = fns_.find(id);
      if (found == fns_.end()) continue;
      auto fn = std::move(found->second);
      fns_.erase(found);
      lock.unlock();
      deliver(std::move(fn));
      lock.lock();
    }
  }

  void stopTimer() {
    std::thread worker;
    {
      std::lock_guard lock(tmu_);
      stopping_ = true;
      due_.clear();
      fns_.clear();
      tcv_.notify_all();
      worker = std::move(worker_);
    }
    if (worker.joinable()) {
      if (worker.get_id() == std::this_thread::get_id()) {
        worker.detach();
      } else {
        worker.join();
      }
    }
  }

  // ---- the budget ----

  /// 0 when a token was taken; otherwise how long to wait for one, in milliseconds.
  long takeOrWait() {
    std::lock_guard lock(bmu_);
    const auto now = Clock::now();
    if (now < blockedUntil_) {
      return std::max(1L, static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(blockedUntil_ - now).count()));
    }
    const double elapsed = std::chrono::duration<double, std::milli>(now - last_).count();
    tokens_ = std::min(static_cast<double>(capacity_), tokens_ + elapsed * capacity_ / static_cast<double>(windowMs_));
    last_ = now;
    if (tokens_ >= 1) {
      tokens_ -= 1;
      return 0;
    }
    return std::max(1L, static_cast<long>((1 - tokens_) * static_cast<double>(windowMs_) / capacity_) + 1);
  }

  void hold(long ms) {
    std::lock_guard lock(bmu_);
    blockedUntil_ = std::max(blockedUntil_, Clock::now() + std::chrono::milliseconds(ms));
    tokens_ = 0;
  }

  /// Runs `fn` once the budget has a token for it.
  void paced(std::function<void()> fn) {
    const long wait = takeOrWait();
    if (wait == 0) {
      fn();
      return;
    }
    std::weak_ptr<Impl> weak = weak_from_this();
    after(wait, [weak, fn = std::move(fn)]() mutable {
      if (auto self = weak.lock()) self->paced(std::move(fn));
    });
  }

  // ---- the connection ----

  std::shared_ptr<domains::ExecConnection> connection() {
    std::lock_guard lock(cmu_);
    if (!ended_.empty()) return nullptr;
    if (conn_) return conn_;
    std::shared_ptr<domains::ExecConnection> c = options_.connection;
    if (!c) {
      if (!exec_) return nullptr;
      c = options_.developer ? exec_->connectAsDeveloper(appId_, options_.connect) : exec_->connect(appId_, options_.connect);
      if (!c) return nullptr;
    }
    std::weak_ptr<Impl> weak = weak_from_this();
    c->onReconnect([weak](std::string host) {
      if (auto self = weak.lock()) self->notify(self->reconnectListeners_, host);
    });
    c->onAccessEnded([weak](std::string reason) {
      if (auto self = weak.lock()) {
        {
          std::lock_guard ended(self->cmu_);
          self->ended_ = reason.empty() ? std::string("access ended") : reason;
        }
        self->notify(self->endedListeners_, reason);
      }
    });
    conn_ = c;
    return c;
  }

  std::string endedReason() {
    std::lock_guard lock(cmu_);
    return ended_;
  }

  void notify(const std::map<std::uint64_t, std::function<void(std::string)>>& listeners, const std::string& text) {
    std::vector<std::function<void(std::string)>> copies;
    {
      std::lock_guard lock(lmu_);
      for (const auto& [id, l] : listeners) copies.push_back(l);
    }
    for (auto& l : copies) l(text);
  }

  // ---- calls ----

  struct Attempt {
    std::string nodeType, key, method;
    graphql::JVal args;
    bool hasActionId = false;
    long timeoutMs = 0;
    KitCallback done;
    int attempt = 0;
  };

  void send(std::shared_ptr<Attempt> a) {
    std::weak_ptr<Impl> weak = weak_from_this();
    paced([weak, a] {
      auto self = weak.lock();
      if (!self) return;
      auto conn = self->connection();
      if (!conn) {
        const std::string ended = self->endedReason();
        KitReply r = ended.empty() ? KitReply::from(ExecReply{ExecStatus::Unavailable, "the kit has no exec client to connect with"})
                                   : KitReply::from(ExecReply{ExecStatus::Denied, "access ended: " + ended});
        auto done = a->done;
        self->deliver([done, r] {
          if (done) done(r);
        });
        return;
      }
      conn->call(
          a->nodeType, a->key, a->method, a->args,
          [weak, a](ExecReply reply) {
            if (auto runtime = weak.lock()) runtime->answered(a, std::move(reply));
          },
          a->timeoutMs);
    });
  }

  void answered(const std::shared_ptr<Attempt>& a, ExecReply reply) {
    if (!reply.ok() && a->attempt < retries_) {
      const bool coded = reply.status == ExecStatus::AppError;
      // A hub that could not hear from another one did not finish, and keeps no receipt.
      if (coded && a->hasActionId && KitReply::from(reply).code() == codes::kUnavailable) {
        const long base = std::min(2000L, 200L << a->attempt);
        a->attempt += 1;
        std::weak_ptr<Impl> weak = weak_from_this();
        after(static_cast<long>(static_cast<double>(base) * jitter()), [weak, a] {
          if (auto self = weak.lock()) self->send(a);
        });
        return;
      }
      if (!coded) {
        if (reply.status == ExecStatus::Busy && reply.rateLimited()) {
          hold(reply.retryAfterMs().value_or(1000));
          a->attempt += 1;
          send(a);
          return;
        }
        const bool didNotRun = reply.status == ExecStatus::Busy || reply.status == ExecStatus::RateLimited;
        const bool unknown = reply.status == ExecStatus::DeadlineExceeded || reply.status == ExecStatus::Unavailable ||
                             reply.status == ExecStatus::Moved;
        if (didNotRun || (a->hasActionId && unknown)) {
          const long base = std::min(2000L, 200L << a->attempt);
          a->attempt += 1;
          std::weak_ptr<Impl> weak = weak_from_this();
          after(static_cast<long>(static_cast<double>(base) * jitter()), [weak, a] {
            if (auto self = weak.lock()) self->send(a);
          });
          return;
        }
      }
    }
    if (a->done) a->done(KitReply::from(std::move(reply)));
  }

  double jitter() {
    std::lock_guard lock(bmu_);
    return 0.5 + std::uniform_real_distribution<double>(0, 0.5)(rng_);
  }

  // ---- subscriptions ----

  struct Handle {
    std::string topicKey;
    std::uint64_t connHandle = 0;
    bool cancelled = false;
  };

  std::uint64_t subscribe(std::string nodeType, std::string key, std::string topic, domains::ExecPushHandler onPush,
                          KitCallback done) {
    const std::string topicKey = nodeType + '\0' + key + '\0' + topic;
    std::uint64_t handle = 0;
    bool first = false;
    {
      std::lock_guard lock(smu_);
      const auto held = counts_.find(topicKey);
      if (held == counts_.end() && counts_.size() >= kKitSubscriptionsMax) {
        KitReply r = KitReply::refused(codes::kLimit, "a connection holds at most " + std::to_string(kKitSubscriptionsMax) + " subscriptions");
        deliver([done, r] {
          if (done) done(r);
        });
        return 0;
      }
      first = held == counts_.end();
      counts_[topicKey] += 1;
      handle = ++nextHandle_;
      handles_.emplace(handle, Handle{topicKey});
    }
    std::weak_ptr<Impl> weak = weak_from_this();
    auto go = [weak, handle, nodeType = std::move(nodeType), key = std::move(key), topic = std::move(topic),
               onPush = std::move(onPush), done = std::move(done)]() mutable {
      auto self = weak.lock();
      if (!self) return;
      auto conn = self->connection();
      if (!conn) {
        KitReply r = KitReply::from(ExecReply{ExecStatus::Unavailable, "the kit has no connection"});
        self->deliver([done, r] {
          if (done) done(r);
        });
        return;
      }
      const std::uint64_t ch = conn->subscribe(std::move(nodeType), std::move(key), std::move(topic), std::move(onPush),
                                               [done](ExecReply reply) {
                                                 if (done) done(KitReply::from(std::move(reply)));
                                               });
      bool drop = false;
      {
        std::lock_guard lock(self->smu_);
        auto h = self->handles_.find(handle);
        if (h == self->handles_.end()) {
          drop = true;
        } else {
          h->second.connHandle = ch;
        }
      }
      if (drop) conn->unsubscribe(ch);
    };
    if (first) {
      paced(std::move(go));
    } else {
      go();
    }
    return handle;
  }

  void unsubscribe(std::uint64_t handle) {
    std::uint64_t ch = 0;
    bool last = false;
    {
      std::lock_guard lock(smu_);
      auto h = handles_.find(handle);
      if (h == handles_.end()) return;
      ch = h->second.connHandle;
      auto count = counts_.find(h->second.topicKey);
      if (count != counts_.end() && --count->second <= 0) {
        counts_.erase(count);
        last = true;
      }
      handles_.erase(h);
    }
    if (ch == 0) return;
    std::shared_ptr<domains::ExecConnection> conn;
    {
      std::lock_guard lock(cmu_);
      conn = conn_;
    }
    if (!conn) return;
    if (last) {
      paced([conn, ch] { conn->unsubscribe(ch); });
    } else {
      conn->unsubscribe(ch);
    }
  }

  std::size_t subscriptions() const {
    std::lock_guard lock(smu_);
    return counts_.size();
  }

  std::uint64_t listen(std::map<std::uint64_t, std::function<void(std::string)>>& to, std::function<void(std::string)> l) {
    std::lock_guard lock(lmu_);
    const std::uint64_t id = ++nextListener_;
    to.emplace(id, std::move(l));
    return id;
  }

  void removeListener(std::uint64_t id) {
    std::lock_guard lock(lmu_);
    reconnectListeners_.erase(id);
    endedListeners_.erase(id);
  }

  void close() {
    stopTimer();
    std::shared_ptr<domains::ExecConnection> conn;
    {
      std::lock_guard lock(cmu_);
      conn = std::move(conn_);
      conn_.reset();
    }
    if (conn && !options_.connection) conn->close();
    std::lock_guard lock(smu_);
    counts_.clear();
    handles_.clear();
  }

  // ---- state ----

  std::mutex tmu_;
  std::condition_variable tcv_;
  std::multimap<Clock::time_point, std::uint64_t> due_;
  std::map<std::uint64_t, std::function<void()>> fns_;
  std::uint64_t nextTimer_ = 0;
  bool stopping_ = false;
  std::thread worker_;

  std::mutex bmu_;
  double tokens_;
  Clock::time_point last_;
  Clock::time_point blockedUntil_{};
  std::mt19937 rng_{std::random_device{}()};

  std::mutex cmu_;
  std::shared_ptr<domains::ExecConnection> conn_;
  std::string ended_;

  mutable std::mutex smu_;
  std::map<std::string, int> counts_;
  std::map<std::uint64_t, Handle> handles_;
  std::uint64_t nextHandle_ = 0;

  std::mutex lmu_;
  std::map<std::uint64_t, std::function<void(std::string)>> reconnectListeners_;
  std::map<std::uint64_t, std::function<void(std::string)>> endedListeners_;
  std::uint64_t nextListener_ = 0;

  mutable std::mutex pmu_;
  std::string playerId_;
};

KitRuntime::KitRuntime(std::string appId, domains::ExecAPI* exec, std::shared_ptr<graphql::Dispatcher> dispatcher,
                       KitRuntimeOptions options)
    : impl_(std::make_shared<Impl>(std::move(appId), exec, std::move(dispatcher), std::move(options))) {}

KitRuntime::~KitRuntime() { impl_->close(); }

const std::string& KitRuntime::appId() const { return impl_->appId_; }

std::shared_ptr<domains::ExecConnection> KitRuntime::connection() { return impl_->connection(); }

std::string KitRuntime::playerId() const {
  std::lock_guard lock(impl_->pmu_);
  return impl_->playerId_;
}

void KitRuntime::setPlayerId(std::string playerId) {
  std::lock_guard lock(impl_->pmu_);
  impl_->playerId_ = std::move(playerId);
}

void KitRuntime::call(std::string nodeType, std::string key, std::string method, graphql::JVal args, KitCallback done,
                      KitCallOptions options) {
  auto a = std::make_shared<Impl::Attempt>();
  a->nodeType = std::move(nodeType);
  a->key = std::move(key);
  a->method = std::move(method);
  std::string actionId = !options.actionId.empty() ? options.actionId : options.action ? newActionId() : std::string();
  a->hasActionId = !actionId.empty();
  a->args = a->hasActionId ? withActionId(std::move(args), actionId) : std::move(args);
  a->timeoutMs = options.timeoutMs;
  a->done = std::move(done);
  impl_->send(std::move(a));
}

std::uint64_t KitRuntime::subscribe(std::string nodeType, std::string key, std::string topic,
                                    domains::ExecPushHandler onPush, KitCallback done) {
  return impl_->subscribe(std::move(nodeType), std::move(key), std::move(topic), std::move(onPush), std::move(done));
}

void KitRuntime::unsubscribe(std::uint64_t handle) { impl_->unsubscribe(handle); }

std::size_t KitRuntime::subscriptions() const { return impl_->subscriptions(); }

std::uint64_t KitRuntime::onReconnect(std::function<void(std::string)> listener) {
  return impl_->listen(impl_->reconnectListeners_, std::move(listener));
}

std::uint64_t KitRuntime::onAccessEnded(std::function<void(std::string)> listener) {
  return impl_->listen(impl_->endedListeners_, std::move(listener));
}

void KitRuntime::removeListener(std::uint64_t id) { impl_->removeListener(id); }

std::uint64_t KitRuntime::after(long delayMs, std::function<void()> fn) { return impl_->after(delayMs, std::move(fn)); }

void KitRuntime::cancel(std::uint64_t timer) { impl_->cancel(timer); }

void KitRuntime::close() { impl_->close(); }

}  // namespace crowdy::kit
