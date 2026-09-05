#include "crowdy/replication/connection.hpp"

#include <chrono>
#include <cstring>
#include <thread>

namespace crowdy::replication {

using wire::MessageType;

Connection::Connection(Config config, std::shared_ptr<ISessionProvider> provider,
                       const core::ICrypto& crypto, const core::IClock& clock,
                       const core::ILogger& logger)
    : config_(std::move(config)),
      provider_(std::move(provider)),
      crypto_(crypto),
      clock_(clock),
      logger_(logger),
      ring_(config_.ringCapacity) {
  setToken(config_.token);
}

Connection::~Connection() { disconnect(); }

void Connection::setHandlers(Handlers handlers) {
  std::lock_guard lock(handlersMutex_);
  handlers_ = std::move(handlers);
}

void Connection::setToken(const TokenInfo& token) {
  std::lock_guard lock(tokenMutex_);
  token_ = token;
  if (auto t = wire::Token64::fromString(token.token)) {
    token64_ = *t;
    // Built once per token rather than per datagram. Null when the provider
    // has no pre-keyed form; the codec then signs the way it always did.
    mac_ = crypto_.makeHmacSha256(token64_.bytes());
  }
}

Connection::Credentials Connection::credentials() const {
  std::lock_guard lock(tokenMutex_);
  return {token64_, mac_, token_.gameTokenId};
}

Connection::Snapshot Connection::snapshot() const {
  Snapshot out;
  out.config = config_;
  out.state = state();
  {
    std::lock_guard lock(tokenMutex_);
    out.config.token = token_;
  }
  {
    std::lock_guard lock(handlersMutex_);
    out.handlers = handlers_;
  }
  {
    std::lock_guard lock(assignmentMutex_);
    out.endpoint = assignment_;
  }
  return out;
}

wire::Token64 Connection::tokenSnapshot() const {
  std::lock_guard lock(tokenMutex_);
  return token64_;
}

Connection::Stats Connection::stats() const {
  std::lock_guard lock(statsMutex_);
  return stats_;
}

void Connection::setState(ConnState next) {
  const ConnState prev = state_.exchange(next, std::memory_order_acq_rel);
  if (prev == next) return;
  Event e;
  e.kind = Event::Kind::Status;
  e.statusValue = next;
  e.len = 0;
  if (!ring_.tryPush(e)) {
    // Ring full of data events; status will be observed via state() instead.
  }
}

Status Connection::doAssign() {
  auto assigned = provider_->assignServer();
  if (!assigned.ok()) return assigned.error();
  const Assignment endpoint = assigned.value();

  const std::string& host =
      (config_.preferIpv6 && !endpoint.ip6.empty()) ? endpoint.ip6 : endpoint.ip4;
  if (host.empty() || endpoint.clientPort <= 0) return Errc::Rejected;

  Status st = socket_.open(host, endpoint.clientPort, config_.socketRecvBufferBytes,
                           config_.socketSendBufferBytes);
  if (!st.ok()) return st;
  {
    std::lock_guard lock(assignmentMutex_);
    assignment_ = endpoint;
  }

  readyAtMs_ = clock_.monotonicMillis() + config_.sessionReadyWaitMs;
  lastRecvMs_ = 0;
  lastSendMs_ = 0;
  return Errc::Ok;
}

Status Connection::connect() {
  {
    std::lock_guard lock(tokenMutex_);
    if (token_.token.size() != wire::kTokenOctets) return Errc::InvalidArgument;
  }
  if (state() == ConnState::Connected || state() == ConnState::Connecting)
    return Errc::Ok;

  setState(ConnState::Connecting);
  Status st = doAssign();
  if (!st.ok()) {
    setState(ConnState::Failed);
    return st;
  }

  if (!config_.manualPump) {
    running_.store(true, std::memory_order_release);
    netThread_ = std::thread([this] { netLoop(); });
  }
  return Errc::Ok;
}

void Connection::disconnect() {
  running_.store(false, std::memory_order_release);
  if (netThread_.joinable()) netThread_.join();
  socket_.close();
  if (state() != ConnState::Closed) setState(ConnState::Closed);
}

// ---------------------------------------------------------------------------
// Send path
// ---------------------------------------------------------------------------

Status Connection::transmit(const std::uint8_t* data, std::size_t len) {
  Status st = socket_.send(Bytes(data, len));
  if (st.ok()) {
    lastSendMs_ = clock_.monotonicMillis();
    std::lock_guard lock(statsMutex_);
    ++stats_.datagramsSent;
    // Client datagrams carry exactly one message whose opcode is byte 0.
    ++stats_.messagesSent;
    stats_.bytesSent += len;
    if (len > 0) ++stats_.messagesSentByType[data[0]];
    return st;
  }
  // A deferred datagram never reached the wire, so it must not advance
  // lastSendMs_: the silence watchdog reassigns when sends are flowing and
  // nothing comes back, and backpressure is not traffic going out.
  std::lock_guard lock(statsMutex_);
  if (st.code == Errc::WouldBlock) {
    ++stats_.sendsDeferred;
  } else {
    ++stats_.sendsFailed;
  }
  return st;
}

Result<std::uint8_t> Connection::sendLongSpatial(MessageType type, const SpatialSend& p) {
  if (!socket_.isOpen()) return Errc::NotConnected;
  // One lock for everything the token owns: the id, the key and the pre-keyed
  // MAC used to sign with it. Reading them separately took the same lock twice
  // and could straddle a token rotation.
  const Credentials creds = credentials();

  wire::LongSpatialParams params;
  params.type = type;
  params.appId = config_.appId;
  params.chunk = p.chunk;
  params.distance = p.distance;
  params.decay = p.decay;
  params.uuid = p.uuid;
  params.payload = p.payload;
  params.gameTokenId = creds.gameTokenId;
  params.sequence = nextSequence();

  std::uint8_t buf[wire::kMaxDatagramSize];
  auto n = wire::encodeLongSpatial(crypto_, params, creds.token, MutableBytes(buf, sizeof(buf)),
                                   creds.mac.get());
  if (!n.ok()) return n.error();
  Status st = transmit(buf, n.value());
  if (!st.ok()) return st.code;
  return params.sequence;
}

Result<std::uint8_t> Connection::sendVoxelUpdate(const wire::ChunkCoord& chunk,
                                                 const core::ActorUuid& uuid, std::int16_t x,
                                                 std::int16_t y, std::int16_t z,
                                                 std::int16_t voxelType, Bytes voxelState,
                                                 std::uint8_t distance, wire::DecayRate decay) {
  std::uint8_t payload[wire::kMaxLongSpatialPayload];
  auto pn = wire::encodeVoxelPayload(x, y, z, voxelType, voxelState,
                                     MutableBytes(payload, sizeof(payload)));
  if (!pn.ok()) return pn.error();
  SpatialSend p;
  p.chunk = chunk;
  p.uuid = uuid;
  p.payload = Bytes(payload, pn.value());
  p.distance = distance;
  p.decay = decay;
  return sendLongSpatial(MessageType::VoxelUpdateRequest, p);
}

Result<std::uint8_t> Connection::sendClientEvent(const wire::ChunkCoord& chunk,
                                                 const core::ActorUuid& uuid,
                                                 std::uint16_t eventType, Bytes state,
                                                 std::uint8_t distance, wire::DecayRate decay) {
  std::uint8_t payload[wire::kMaxLongSpatialPayload];
  auto pn = wire::encodeEventPayload(eventType, state, MutableBytes(payload, sizeof(payload)));
  if (!pn.ok()) return pn.error();
  SpatialSend p;
  p.chunk = chunk;
  p.uuid = uuid;
  p.payload = Bytes(payload, pn.value());
  p.distance = distance;
  p.decay = decay;
  return sendLongSpatial(MessageType::ClientEventNotification, p);
}

Result<std::uint8_t> Connection::sendSingleActorMessage(const wire::ChunkCoord& targetChunk,
                                                        const core::ActorUuid& targetUuid,
                                                        Bytes payload) {
  SpatialSend p;
  p.chunk = targetChunk;
  p.uuid = targetUuid;
  p.payload = payload;
  p.distance = 0;  // ignored by the server for single-actor messages
  p.decay = wire::DecayRate::None;
  return sendLongSpatial(MessageType::SingleActorMessage, p);
}

Result<std::uint8_t> Connection::sendChannelMessage(std::int64_t channelId,
                                                    const core::ActorUuid& uuid, Bytes payload) {
  if (!socket_.isOpen()) return Errc::NotConnected;
  const Credentials creds = credentials();

  wire::ChannelMessageParams params;
  params.channelId = channelId;
  params.uuid = uuid;
  params.payload = payload;
  params.gameTokenId = creds.gameTokenId;
  params.sequence = nextSequence();

  std::uint8_t buf[wire::kMaxDatagramSize];
  auto n = wire::encodeChannelMessage(crypto_, params, creds.token, MutableBytes(buf, sizeof(buf)),
                                      creds.mac.get());
  if (!n.ok()) return n.error();
  Status st = transmit(buf, n.value());
  if (!st.ok()) return st.code;
  return params.sequence;
}

Result<std::uint8_t> Connection::sendHeartbeat(const wire::ChunkCoord& chunk,
                                               const core::ActorUuid& uuid) {
  SpatialSend p;
  p.chunk = chunk;
  p.uuid = uuid;
  p.distance = 0;
  p.decay = wire::DecayRate::None;
  return sendLongSpatial(MessageType::ClientActorHeartbeat, p);
}

// ---------------------------------------------------------------------------
// Receive path
// ---------------------------------------------------------------------------

void Connection::handleDatagram(Bytes datagram) {
  {
    std::lock_guard lock(statsMutex_);
    ++stats_.datagramsReceived;
    stats_.bytesReceived += datagram.size();
  }
  lastRecvMs_ = clock_.monotonicMillis();

  // COMMAND_RECONNECT is always its own datagram; verify before acting.
  if (!datagram.empty() &&
      datagram[0] == static_cast<std::uint8_t>(MessageType::CommandReconnect)) {
    const wire::Token64 token = tokenSnapshot();
    if (wire::verifyCommandReconnect(crypto_, datagram, token).ok()) {
      reconnectRequested_.store(true, std::memory_order_release);
    } else {
      std::lock_guard lock(statsMutex_);
      ++stats_.hmacFailures;
    }
    return;
  }

  // Read once for the whole datagram: a bundle can carry many notifications and
  // each one is verified with the same key.
  const Credentials creds = credentials();
  const wire::Token64& token = creds.token;
  Status bundleStatus = wire::forEachMessage(datagram, [&](Bytes message) {
    if (message.empty()) return;
    const std::uint8_t type = message[0];

    Event e;
    if (type == static_cast<std::uint8_t>(MessageType::GenericError)) {
      if (!wire::parseGenericError(message).ok()) {
        std::lock_guard lock(statsMutex_);
        ++stats_.malformed;
        return;
      }
      e.kind = Event::Kind::Error;
    } else if (type == static_cast<std::uint8_t>(MessageType::ChannelMessageNotification)) {
      if (!wire::parseChannelNotification(message).ok()) {
        std::lock_guard lock(statsMutex_);
        ++stats_.malformed;
        return;
      }
      e.kind = Event::Kind::Channel;
    } else if (wire::isLongSpatialLayout(type)) {
      auto parsed = wire::parseLongSpatial(message);
      if (!parsed.ok()) {
        std::lock_guard lock(statsMutex_);
        ++stats_.malformed;
        return;
      }
      if (config_.verifyNotifications &&
          !wire::verifyLongSpatial(crypto_, message, token, creds.mac.get()).ok()) {
        std::lock_guard lock(statsMutex_);
        ++stats_.hmacFailures;
        return;
      }
      {
        std::lock_guard lock(statsMutex_);
        stats_.lastServerEpochMs = parsed->epochMillisOrTokenId;
      }
      e.kind = Event::Kind::Spatial;
    } else {
      // Unknown opcode: ignore (the protocol evolves by adding opcodes).
      return;
    }

    if (message.size() > sizeof(e.data)) {
      std::lock_guard lock(statsMutex_);
      ++stats_.malformed;
      return;
    }
    std::memcpy(e.data, message.data(), message.size());
    e.len = static_cast<std::uint16_t>(message.size());
    {
      std::lock_guard lock(statsMutex_);
      ++stats_.messagesReceived;
      ++stats_.messagesReceivedByType[type];
    }
    if (!ring_.tryPush(e)) {
      std::lock_guard lock(statsMutex_);
      ++stats_.ringDropped;
    }
  });
  if (!bundleStatus.ok()) {
    std::lock_guard lock(statsMutex_);
    ++stats_.malformed;
  }
}

std::size_t Connection::receiveBatch(int timeoutMs) {
  // Batched receive: one syscall per burst on Linux (recvmmsg), a drain loop
  // elsewhere. 2048-byte slots comfortably hold any 1232-byte datagram.
  constexpr std::size_t kSlot = 2048;
  constexpr std::size_t kBatch = 16;
  std::uint8_t slab[kSlot * kBatch];
  std::size_t lengths[kBatch];

  std::size_t processed = 0;
  int wait = timeoutMs;
  for (;;) {
    auto n = socket_.recvBatch(slab, kSlot, kBatch, lengths, wait);
    if (!n.ok() || n.value() == 0) break;
    for (std::size_t i = 0; i < n.value(); ++i) {
      handleDatagram(Bytes(slab + i * kSlot, lengths[i]));
    }
    processed += n.value();
    if (n.value() < kBatch) break;  // drained
    wait = 0;
  }
  return processed;
}

void Connection::housekeeping() {
  const std::int64_t nowMono = clock_.monotonicMillis();

  // Flip Connecting -> Connected once the session-ready wait elapses.
  if (state() == ConnState::Connecting && nowMono >= readyAtMs_) {
    setState(ConnState::Connected);
  }

  // Proactive token refresh.
  std::int64_t expiresAt = 0;
  {
    std::lock_guard lock(tokenMutex_);
    expiresAt = token_.expiresAtEpochMs;
  }
  if (config_.refreshLeadMs > 0 && expiresAt > 0 &&
      clock_.epochMillis() >= expiresAt - config_.refreshLeadMs) {
    auto refreshed = provider_->refreshToken();
    if (refreshed.ok()) {
      setToken(refreshed.value());
      if (logger_.enabled(core::LogLevel::Info))
        logger_.log(core::LogLevel::Info, "app token refreshed");
    } else {
      // Push expiry forward a little so a failing refresh endpoint does not
      // busy-loop; the server will emit TOKEN_EXPIRED if it truly lapses.
      std::lock_guard lock(tokenMutex_);
      token_.expiresAtEpochMs += 30 * 1000;
      logger_.log(core::LogLevel::Warn, "app token refresh failed; will retry");
    }
  }

  // Silent-drop watchdog (opt-in).
  if (config_.watchdogSilenceMs > 0 && state() == ConnState::Connected && lastRecvMs_ > 0 &&
      lastSendMs_ > lastRecvMs_ && nowMono - lastRecvMs_ > config_.watchdogSilenceMs) {
    logger_.log(core::LogLevel::Warn, "watchdog: sends flowing but nothing received; reassigning");
    reconnectRequested_.store(true, std::memory_order_release);
  }

  // Reassignment (COMMAND_RECONNECT, watchdog, TOKEN_EXPIRED recovery).
  if (reconnectRequested_.exchange(false, std::memory_order_acq_rel)) {
    setState(ConnState::Reconnecting);
    {
      std::lock_guard lock(statsMutex_);
      ++stats_.reconnects;
    }
    socket_.close();
    Status st = doAssign();
    if (st.ok()) {
      setState(ConnState::Connecting);
    } else {
      logger_.log(core::LogLevel::Error, "reassignment failed");
      setState(ConnState::Failed);
    }
  }
}

void Connection::netLoop() {
  while (running_.load(std::memory_order_acquire)) {
    receiveBatch(20);
    housekeeping();
    if (state() == ConnState::Failed) break;
  }
}

std::size_t Connection::pump(int timeoutMs) {
  if (!config_.manualPump) return 0;
  const std::size_t n = receiveBatch(timeoutMs);
  housekeeping();
  return n;
}

Connection::WaitOutcome Connection::waitForSequence(std::uint8_t sequence,
                                                    const core::ActorUuid& uuid, int timeoutMs) {
  pendingWait_ = PendingWait{};
  pendingWait_.active = true;
  pendingWait_.sequence = sequence;
  pendingWait_.uuid = uuid;

  const std::int64_t deadline = clock_.monotonicMillis() + timeoutMs;
  while (!pendingWait_.done && clock_.monotonicMillis() < deadline) {
    if (config_.manualPump) pump(5);
    if (poll() == 0 && !config_.manualPump) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  pendingWait_.active = false;
  return pendingWait_.outcome;
}

Connection::WaitOutcome Connection::sendActorUpdateAndWait(const SpatialSend& p, int timeoutMs) {
  auto seq = sendActorUpdate(p);
  if (!seq.ok()) return WaitOutcome{};
  return waitForSequence(seq.value(), p.uuid, timeoutMs);
}

Connection::WaitOutcome Connection::sendVoxelUpdateAndWait(const wire::ChunkCoord& chunk,
                                                           const core::ActorUuid& uuid,
                                                           std::int16_t x, std::int16_t y,
                                                           std::int16_t z, std::int16_t voxelType,
                                                           Bytes voxelState,
                                                           std::uint8_t distance,
                                                           wire::DecayRate decay, int timeoutMs) {
  auto seq = sendVoxelUpdate(chunk, uuid, x, y, z, voxelType, voxelState, distance, decay);
  if (!seq.ok()) return WaitOutcome{};
  return waitForSequence(seq.value(), uuid, timeoutMs);
}

Connection::WaitOutcome Connection::sendTextAndWait(const SpatialSend& p, int timeoutMs) {
  auto seq = sendText(p);
  if (!seq.ok()) return WaitOutcome{};
  // Text has no self-echo: a clean timeout (acknowledged=false, no error)
  // means the send was accepted as far as the wire can tell.
  return waitForSequence(seq.value(), p.uuid, timeoutMs);
}

std::size_t Connection::poll(std::size_t maxEvents) {
  Handlers handlers;
  {
    std::lock_guard lock(handlersMutex_);
    handlers = handlers_;
  }

  std::size_t dispatched = 0;
  while (dispatched < maxEvents) {
    auto event = ring_.tryPop();
    if (!event) break;
    ++dispatched;
    const Event& e = *event;
    Bytes message(e.data, e.len);

    switch (e.kind) {
      case Event::Kind::Status:
        if (handlers.status) handlers.status(e.statusValue);
        break;

      case Event::Kind::Error: {
        auto err = wire::parseGenericError(message);
        if (err.ok()) {
          if (err->code == wire::ErrorCode::TokenExpired) {
            // Refresh + reassign; the session provider re-mints server-side.
            reconnectRequested_.store(true, std::memory_order_release);
          }
          if (pendingWait_.active && !pendingWait_.done &&
              err->sequence == pendingWait_.sequence) {
            pendingWait_.outcome.error = err->code;
            pendingWait_.done = true;
          }
          if (handlers.genericError) handlers.genericError({err->sequence, err->code});
        }
        break;
      }

      case Event::Kind::Channel: {
        auto ch = wire::parseChannelNotification(message);
        if (ch.ok() && handlers.channelMessage) {
          handlers.channelMessage(
              {ch->channelId, ch->senderUuid, ch->payload, ch->epochMillis, ch->sequence});
        }
        break;
      }

      case Event::Kind::Spatial: {
        auto v = wire::parseLongSpatial(message);
        if (!v.ok()) break;
        SpatialNotification n{v->type,   v->appId,
                              v->chunk,  v->uuid,
                              v->payload, v->epochMillisOrTokenId,
                              v->sequence};
        if (pendingWait_.active && !pendingWait_.done && v->sequence == pendingWait_.sequence &&
            std::memcmp(v->uuid, pendingWait_.uuid.data(), wire::kUuidSize) == 0) {
          pendingWait_.outcome.acknowledged = true;
          pendingWait_.outcome.serverEpochMs = v->epochMillisOrTokenId;
          pendingWait_.done = true;
        }
        if (handlers.any) handlers.any(n);
        switch (v->type) {
          case MessageType::ActorUpdateNotification:
            if (handlers.actorUpdate) handlers.actorUpdate(n);
            break;
          case MessageType::VoxelUpdateNotification: {
            if (handlers.voxelUpdate) {
              auto voxel = wire::parseVoxelPayload(v->payload);
              if (voxel.ok()) handlers.voxelUpdate(n, voxel.value());
            }
            break;
          }
          case MessageType::ClientAudioNotification:
            if (handlers.audio) handlers.audio(n);
            break;
          case MessageType::ClientTextNotification:
            if (handlers.text) handlers.text(n);
            break;
          case MessageType::ClientEventNotification: {
            if (handlers.clientEvent) {
              auto ev = wire::parseEventPayload(v->payload);
              if (ev.ok()) handlers.clientEvent(n, ev.value());
            }
            break;
          }
          case MessageType::ServerEventNotification: {
            if (handlers.serverEvent) {
              auto ev = wire::parseEventPayload(v->payload);
              if (ev.ok()) handlers.serverEvent(n, ev.value());
            }
            break;
          }
          case MessageType::GenericSpatial1:
            if (handlers.genericSpatial) handlers.genericSpatial(n);
            break;
          case MessageType::SingleActorMessage:
            if (handlers.singleActorMessage) handlers.singleActorMessage(n);
            break;
          default:
            break;
        }
        break;
      }
    }
  }
  return dispatched;
}

// ---------------------------------------------------------------------------

ReplicationClient::ConnectResult ReplicationClient::connectWithStatus(
    const Config& config, Handlers handlers) {
  auto conn = std::make_shared<Connection>(config, provider_, crypto_);
  conn->setHandlers(std::move(handlers));
  {
    std::lock_guard lock(connectionMutex_);
    activeConnection_ = conn;
  }
  return {conn, conn->connect()};
}

std::shared_ptr<Connection> ReplicationClient::connect(
    const Config& config, Handlers handlers) {
  return connectWithStatus(config, std::move(handlers)).connection;
}

}  // namespace crowdy::replication
