#include "crowdy/kit/mirror.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace crowdy::kit {

namespace {

struct Effects {
  bool changed = false;
  bool resync = false;
  std::vector<std::pair<graphql::Json, KitHeader>> events;
  std::vector<std::pair<KitChanged, KitHeader>> changeds;
};

graphql::JVal topicArgs(const std::string& topic) { return graphql::JVal::object({{"topic", graphql::JVal(topic)}}); }

}  // namespace

// ---- TopicMirror ----

struct TopicMirror::Core : std::enable_shared_from_this<TopicMirror::Core> {
  Core(std::shared_ptr<KitRuntime> rt, std::string nt, std::string k, std::string t, Shape s)
      : runtime(std::move(rt)), nodeType(std::move(nt)), key(std::move(k)), topic(std::move(t)), shape(s) {}

  const std::shared_ptr<KitRuntime> runtime;
  const std::string nodeType, key, topic;
  const Shape shape;

  mutable std::mutex mu;
  graphql::Json state;
  std::map<std::int64_t, KitContainer> members;
  bool hasState = false;
  std::int64_t epoch = -1;
  std::int64_t seq = 0;
  bool syncing = false;
  bool buffering = false;
  std::vector<KitMessage> buffer;
  std::vector<KitCallback> syncWaiters;
  bool starting = false;
  bool started = false;
  std::vector<KitCallback> startWaiters;
  std::uint64_t subHandle = 0;
  std::uint64_t reconnectId = 0;
  std::map<std::uint64_t, std::function<void()>> changeListeners;
  std::map<std::uint64_t, std::function<void(const graphql::Json&, const KitHeader&)>> eventListeners;
  std::map<std::uint64_t, std::function<void(const KitChanged&, const KitHeader&)>> changedListeners;
  std::uint64_t nextId = 0;

  void takeSnapshot(const graphql::Json& body) {
    hasState = true;
    if (shape == Shape::Snapshots) {
      state = body;
      return;
    }
    members.clear();
    body["containers"].forEach([&](const graphql::Json& c) {
      if (c["removed"].asBool(false) || !c["id"].isNumber()) return;
      const std::int64_t id = c["id"].asInt64();
      members[id] = KitContainer{id, c["type_name"].asString(), c["props"].isObject() ? c["props"] : graphql::Json::parse("{}")};
    });
  }

  bool takeDelta(const graphql::Json& body) {
    if (shape != Shape::Collection || !body["id"].isNumber()) return false;
    const std::int64_t id = body["id"].asInt64();
    if (body["removed"].asBool(false)) {
      members.erase(id);
      return true;
    }
    std::string typeName = body["type_name"].asString();
    if (typeName.empty()) {
      auto held = members.find(id);
      if (held != members.end()) typeName = held->second.typeName;
    }
    members[id] = KitContainer{id, std::move(typeName), body["props"].isObject() ? body["props"] : graphql::Json::parse("{}")};
    return true;
  }

  void apply(const KitMessage& m, Effects& e) {
    if (m.header.epoch != epoch) {
      if (m.header.kind == KitMessageKind::Snapshot) {
        epoch = m.header.epoch;
        seq = m.header.seq;
        takeSnapshot(m.body);
        e.changed = true;
      } else {
        e.resync = true;
      }
      return;
    }
    if (m.header.seq <= seq) return;
    const bool gap = m.header.seq != seq + 1;
    seq = m.header.seq;
    switch (m.header.kind) {
      case KitMessageKind::Snapshot:
        takeSnapshot(m.body);
        e.changed = true;
        return;
      case KitMessageKind::Delta:
        if (gap || !hasState || !takeDelta(m.body)) {
          e.resync = true;
        } else {
          e.changed = true;
        }
        return;
      case KitMessageKind::Changed:
        if (auto c = kitChanged(m.body)) e.changeds.emplace_back(std::move(*c), m.header);
        break;
      case KitMessageKind::Event:
        e.events.emplace_back(m.body, m.header);
        break;
    }
    if (gap && shape == Shape::Collection) e.resync = true;
  }

  void run(Effects& e) {
    std::vector<std::function<void()>> changes;
    std::vector<std::function<void(const graphql::Json&, const KitHeader&)>> events;
    std::vector<std::function<void(const KitChanged&, const KitHeader&)>> changeds;
    {
      std::lock_guard lock(mu);
      if (e.changed) {
        for (const auto& [id, l] : changeListeners) changes.push_back(l);
      }
      if (!e.events.empty()) {
        for (const auto& [id, l] : eventListeners) events.push_back(l);
      }
      if (!e.changeds.empty()) {
        for (const auto& [id, l] : changedListeners) changeds.push_back(l);
      }
    }
    for (auto& l : changes) l();
    for (const auto& [body, header] : e.events) {
      for (auto& l : events) l(body, header);
    }
    for (const auto& [changed, header] : e.changeds) {
      for (auto& l : changeds) l(changed, header);
    }
    if (e.resync) resync({});
  }

  void onPush(const graphql::Json& value) {
    auto msg = splitKitMessage(value);
    if (!msg) return;
    Effects e;
    {
      std::lock_guard lock(mu);
      if (buffering) {
        buffer.push_back(std::move(*msg));
        return;
      }
      apply(*msg, e);
    }
    run(e);
  }

  void resync(KitCallback done) {
    {
      std::lock_guard lock(mu);
      if (done) syncWaiters.push_back(std::move(done));
      if (syncing) return;
      syncing = true;
      buffering = true;
      buffer.clear();
    }
    std::weak_ptr<Core> weak = weak_from_this();
    runtime->call(nodeType, key, std::string(kKitSnapshotMethod), topicArgs(topic), [weak](KitReply reply) {
      if (auto self = weak.lock()) self->synced(std::move(reply));
    });
  }

  void synced(KitReply reply) {
    std::optional<KitMessage> msg;
    if (reply.ok()) {
      msg = splitKitMessage(reply.value());
      if (!msg) reply = KitReply::refused(codes::kBadRequest, nodeType + " answered snapshot with something other than a kit message");
    }
    std::vector<KitCallback> waiters;
    Effects e;
    {
      std::lock_guard lock(mu);
      if (msg) {
        epoch = msg->header.epoch;
        seq = msg->header.seq;
        takeSnapshot(msg->body);
        std::vector<KitMessage> pending = std::move(buffer);
        std::stable_sort(pending.begin(), pending.end(), [](const KitMessage& a, const KitMessage& b) { return a.header.seq < b.header.seq; });
        buffer.clear();
        buffering = false;
        syncing = false;
        for (const auto& m : pending) apply(m, e);
        e.changed = true;
      } else {
        buffer.clear();
        buffering = false;
        syncing = false;
      }
      waiters.swap(syncWaiters);
    }
    run(e);
    for (auto& w : waiters) w(reply);
  }

  void start(KitCallback done) {
    {
      std::unique_lock lock(mu);
      if (started) {
        lock.unlock();
        if (done) done(KitReply{domains::ExecStatus::Ok, graphql::Json().toMsgpack(), std::nullopt});
        return;
      }
      if (done) startWaiters.push_back(std::move(done));
      if (starting) return;
      starting = true;
    }
    std::weak_ptr<Core> weak = weak_from_this();
    const std::uint64_t reconnect = runtime->onReconnect([weak](const std::string&) {
      if (auto self = weak.lock()) self->resync({});
    });
    const std::uint64_t handle = runtime->subscribe(
        nodeType, key, topic,
        [weak](const domains::ExecPush& push) {
          if (auto self = weak.lock()) self->onPush(push.value());
        },
        [weak](KitReply reply) {
          auto self = weak.lock();
          if (!self) return;
          if (!reply.ok()) {
            self->finishStart(std::move(reply));
            return;
          }
          self->resync([weak](KitReply snapshot) {
            if (auto again = weak.lock()) again->finishStart(std::move(snapshot));
          });
        });
    {
      std::lock_guard lock(mu);
      reconnectId = reconnect;
      subHandle = handle;
    }
  }

  void finishStart(KitReply reply) {
    std::vector<KitCallback> waiters;
    std::uint64_t handle = 0, reconnect = 0;
    {
      std::lock_guard lock(mu);
      starting = false;
      started = reply.ok();
      if (!started) {
        handle = std::exchange(subHandle, 0);
        reconnect = std::exchange(reconnectId, 0);
      }
      waiters.swap(startWaiters);
    }
    if (handle) runtime->unsubscribe(handle);
    if (reconnect) runtime->removeListener(reconnect);
    for (auto& w : waiters) w(reply);
  }

  void stop() {
    std::uint64_t handle = 0, reconnect = 0;
    {
      std::lock_guard lock(mu);
      handle = std::exchange(subHandle, 0);
      reconnect = std::exchange(reconnectId, 0);
      started = false;
      starting = false;
    }
    if (handle) runtime->unsubscribe(handle);
    if (reconnect) runtime->removeListener(reconnect);
  }
};

TopicMirror::TopicMirror(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key, std::string topic,
                         Shape shape)
    : core_(std::make_shared<Core>(std::move(runtime), std::move(nodeType), std::move(key), std::move(topic), shape)) {}

TopicMirror::~TopicMirror() { core_->stop(); }

void TopicMirror::start(KitCallback done) { core_->start(std::move(done)); }
void TopicMirror::stop() { core_->stop(); }
void TopicMirror::resync(KitCallback done) { core_->resync(std::move(done)); }

bool TopicMirror::started() const {
  std::lock_guard lock(core_->mu);
  return core_->started;
}

std::pair<std::int64_t, std::int64_t> TopicMirror::position() const {
  std::lock_guard lock(core_->mu);
  return {core_->epoch, core_->seq};
}

std::uint64_t TopicMirror::subscribe(std::function<void()> listener) {
  std::lock_guard lock(core_->mu);
  core_->changeListeners.emplace(++core_->nextId, std::move(listener));
  return core_->nextId;
}

std::uint64_t TopicMirror::onEvent(std::function<void(const graphql::Json&, const KitHeader&)> listener) {
  std::lock_guard lock(core_->mu);
  core_->eventListeners.emplace(++core_->nextId, std::move(listener));
  return core_->nextId;
}

std::uint64_t TopicMirror::onChanged(std::function<void(const KitChanged&, const KitHeader&)> listener) {
  std::lock_guard lock(core_->mu);
  core_->changedListeners.emplace(++core_->nextId, std::move(listener));
  return core_->nextId;
}

void TopicMirror::removeListener(std::uint64_t id) {
  std::lock_guard lock(core_->mu);
  core_->changeListeners.erase(id);
  core_->eventListeners.erase(id);
  core_->changedListeners.erase(id);
}

const std::string& TopicMirror::nodeType() const { return core_->nodeType; }
const std::string& TopicMirror::key() const { return core_->key; }
const std::string& TopicMirror::topic() const { return core_->topic; }

HubState::HubState(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key, std::string topic)
    : TopicMirror(std::move(runtime), std::move(nodeType), std::move(key), std::move(topic), Shape::Snapshots) {}

graphql::Json HubState::get() const {
  std::lock_guard lock(core_->mu);
  return core_->state;
}

CollectionMirror::CollectionMirror(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key,
                                   std::string topic)
    : TopicMirror(std::move(runtime), std::move(nodeType), std::move(key), std::move(topic), Shape::Collection) {}

std::vector<KitContainer> CollectionMirror::list(std::string_view typeName) const {
  std::lock_guard lock(core_->mu);
  std::vector<KitContainer> out;
  for (const auto& [id, c] : core_->members) {
    if (typeName.empty() || c.typeName == typeName) out.push_back(c);
  }
  return out;
}

std::optional<KitContainer> CollectionMirror::member(std::int64_t id) const {
  std::lock_guard lock(core_->mu);
  auto found = core_->members.find(id);
  if (found == core_->members.end()) return std::nullopt;
  return found->second;
}

std::size_t CollectionMirror::size() const {
  std::lock_guard lock(core_->mu);
  return core_->members.size();
}

// ---- PrivateState ----

struct PrivateState::Core : std::enable_shared_from_this<PrivateState::Core> {
  Core(std::shared_ptr<KitRuntime> rt, std::string nt, std::string k, std::string t, std::vector<std::string> w,
       std::function<void(KitCallback)> r)
      : runtime(std::move(rt)), nodeType(std::move(nt)), key(std::move(k)), topic(std::move(t)), whats(w.begin(), w.end()), read(std::move(r)) {}

  const std::shared_ptr<KitRuntime> runtime;
  const std::string nodeType, key, topic;
  const std::set<std::string, std::less<>> whats;
  const std::function<void(KitCallback)> read;

  mutable std::mutex mu;
  graphql::Json value;
  bool reading = false;
  bool again = false;
  std::vector<KitCallback> waiters;
  bool starting = false;
  bool started = false;
  std::vector<KitCallback> startWaiters;
  std::uint64_t subHandle = 0;
  std::uint64_t reconnectId = 0;
  std::map<std::uint64_t, std::function<void()>> listeners;
  std::uint64_t nextId = 0;

  void refresh(KitCallback done) {
    {
      std::lock_guard lock(mu);
      if (done) waiters.push_back(std::move(done));
      if (reading) {
        again = true;
        return;
      }
      reading = true;
    }
    readOnce();
  }

  void readOnce() {
    {
      std::lock_guard lock(mu);
      again = false;
    }
    std::weak_ptr<Core> weak = weak_from_this();
    read([weak](KitReply reply) {
      if (auto self = weak.lock()) self->readDone(std::move(reply));
    });
  }

  void readDone(KitReply reply) {
    bool readAgain = false;
    std::vector<KitCallback> done;
    std::vector<std::function<void()>> notify;
    {
      std::lock_guard lock(mu);
      if (again) {
        // A request came during the read: read again, and answer everyone with that.
        readAgain = true;
      } else {
        reading = false;
        if (reply.ok()) {
          value = reply.value();
          for (const auto& [id, l] : listeners) notify.push_back(l);
        }
        done.swap(waiters);
      }
    }
    if (readAgain) {
      readOnce();
      return;
    }
    for (auto& l : notify) l();
    for (auto& w : done) w(reply);
  }

  void onPush(const graphql::Json& pushed) {
    auto msg = splitKitMessage(pushed);
    if (!msg || msg->header.kind != KitMessageKind::Changed) return;
    auto changed = kitChanged(msg->body);
    if (!changed || !whats.count(changed->what)) return;
    const std::string me = runtime->playerId();
    if (me.empty() || std::find(changed->who.begin(), changed->who.end(), me) != changed->who.end()) refresh({});
  }

  void start(KitCallback done) {
    {
      std::unique_lock lock(mu);
      if (started) {
        const std::string held = value.toMsgpack();
        lock.unlock();
        if (done) done(KitReply{domains::ExecStatus::Ok, held, std::nullopt});
        return;
      }
      if (done) startWaiters.push_back(std::move(done));
      if (starting) return;
      starting = true;
    }
    std::weak_ptr<Core> weak = weak_from_this();
    const std::uint64_t reconnect = runtime->onReconnect([weak](const std::string&) {
      if (auto self = weak.lock()) self->refresh({});
    });
    const std::uint64_t handle = runtime->subscribe(
        nodeType, key, topic,
        [weak](const domains::ExecPush& push) {
          if (auto self = weak.lock()) self->onPush(push.value());
        },
        [weak](KitReply reply) {
          auto self = weak.lock();
          if (!self) return;
          if (!reply.ok()) {
            self->finishStart(std::move(reply));
            return;
          }
          self->refresh([weak](KitReply first) {
            if (auto core = weak.lock()) core->finishStart(std::move(first));
          });
        });
    std::lock_guard lock(mu);
    reconnectId = reconnect;
    subHandle = handle;
  }

  void finishStart(KitReply reply) {
    std::vector<KitCallback> waiting;
    std::uint64_t handle = 0, reconnect = 0;
    {
      std::lock_guard lock(mu);
      starting = false;
      started = reply.ok();
      if (!started) {
        handle = std::exchange(subHandle, 0);
        reconnect = std::exchange(reconnectId, 0);
      }
      waiting.swap(startWaiters);
    }
    if (handle) runtime->unsubscribe(handle);
    if (reconnect) runtime->removeListener(reconnect);
    for (auto& w : waiting) w(reply);
  }

  void stop() {
    std::uint64_t handle = 0, reconnect = 0;
    {
      std::lock_guard lock(mu);
      handle = std::exchange(subHandle, 0);
      reconnect = std::exchange(reconnectId, 0);
      started = false;
      starting = false;
    }
    if (handle) runtime->unsubscribe(handle);
    if (reconnect) runtime->removeListener(reconnect);
  }
};

PrivateState::PrivateState(std::shared_ptr<KitRuntime> runtime, std::string nodeType, std::string key, std::string topic,
                           std::vector<std::string> whats, std::function<void(KitCallback)> read)
    : core_(std::make_shared<Core>(std::move(runtime), std::move(nodeType), std::move(key), std::move(topic), std::move(whats),
                                   std::move(read))) {}

PrivateState::~PrivateState() { core_->stop(); }

void PrivateState::start(KitCallback done) { core_->start(std::move(done)); }
void PrivateState::stop() { core_->stop(); }
void PrivateState::refresh(KitCallback done) { core_->refresh(std::move(done)); }

graphql::Json PrivateState::get() const {
  std::lock_guard lock(core_->mu);
  return core_->value;
}

std::uint64_t PrivateState::subscribe(std::function<void()> listener) {
  std::lock_guard lock(core_->mu);
  core_->listeners.emplace(++core_->nextId, std::move(listener));
  return core_->nextId;
}

void PrivateState::removeListener(std::uint64_t id) {
  std::lock_guard lock(core_->mu);
  core_->listeners.erase(id);
}

}  // namespace crowdy::kit
