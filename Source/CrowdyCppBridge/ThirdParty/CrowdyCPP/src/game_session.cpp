#include "crowdy/game_session.hpp"

#include <chrono>
#include <thread>
#include <utility>

#include "crowdy/core/clock.hpp"
#include "crowdy/domains/portal.hpp"
#include "crowdy/graphql/json.hpp"
#include "crowdy/wire/codec.hpp"

namespace crowdy {

namespace {

bool usableAppToken(const domains::AppTokenResponse& token) {
  return token.token.size() == wire::kTokenOctets &&
         token.token.rfind(domains::PortalAPI::kOrgTokenPrefix, 0) != 0 &&
         graphql::parseBigInt(token.appId).has_value() && token.gameTokenIdInt64().has_value();
}

}  // namespace

replication::TokenInfo replicationTokenOf(const domains::AppTokenResponse& token) {
  replication::TokenInfo info;
  if (!usableAppToken(token)) return info;
  info.token = token.token;
  info.gameTokenId = *token.gameTokenIdInt64();
  info.expiresAtEpochMs =
      token.expiresAt.empty()
          ? 0
          : core::parseIso8601Millis(token.expiresAt.data(), token.expiresAt.size());
  return info;
}

ClientConfig gameClientConfig(const domains::AppTokenResponse& token, ClientConfig base,
                              std::string_view fallbackOrigin) {
  const auto pick = [&](const domains::NullableString& fromToken, const std::string& own) {
    if (!fromToken.empty()) return fromToken.valueOrEmpty();
    if (!fallbackOrigin.empty()) return std::string(fallbackOrigin);
    return own;
  };
  base.httpUrl = pick(token.gameApiUrl, base.httpUrl);
  base.discoveryUrl = pick(token.discoveryUrl, base.discoveryUrl);
  base.wsUrl = token.gameApiWsUrl.valueOrEmpty();
  base.wsEndpoint.clear();
  return base;
}

std::int64_t GameSession::appId() const {
  return graphql::parseBigInt(token.appId).value_or(0);
}

GameSession startGameSession(const domains::AppTokenResponse& token, GameSessionOptions options) {
  GameSession session;
  session.token = token;
  if (!usableAppToken(token)) {
    session.status = Errc::InvalidArgument;
    return session;
  }

  session.client = std::make_shared<CrowdyClient>(
      gameClientConfig(token, std::move(options.client), options.fallbackOrigin));
  session.status = session.client->portal().adoptAppToken(token);
  if (!session.status.ok()) return session;

  replication::Config config = std::move(options.replication);
  config.appId = session.appId();
  config.token = replicationTokenOf(token);
  auto connected = session.client->replication().connectWithStatus(config, std::move(options.handlers));
  session.connection = connected.connection;
  session.status = connected.status;
  for (int attempt = 1; !session.status.ok() && attempt < options.connectAttempts && session.connection;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(options.connectRetryDelayMs));
    session.status = session.connection->connect();
  }
  if (!session.status.ok() || options.connectTimeoutMs <= 0) return session;

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(options.connectTimeoutMs);
  while (session.connection->state() != replication::ConnState::Connected) {
    if (std::chrono::steady_clock::now() >= deadline) {
      session.status = Errc::Timeout;
      break;
    }
    if (config.manualPump) {
      session.connection->pump(10);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  return session;
}

}  // namespace crowdy
