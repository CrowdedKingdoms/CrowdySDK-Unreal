#include "crowdy/graphql/websocket.hpp"

namespace crowdy::graphql {

std::shared_ptr<IWebSocketTransport> makeCurlWebSocketTransport() {
  return nullptr;
}

bool curlWebSocketTransportAvailable() noexcept { return false; }

}  // namespace crowdy::graphql
