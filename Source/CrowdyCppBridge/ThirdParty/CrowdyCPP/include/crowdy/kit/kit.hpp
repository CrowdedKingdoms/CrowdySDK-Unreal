#pragma once

#include <string>

#include "crowdy/client.hpp"
#include "crowdy/domains/game_apps.hpp"
#include "crowdy/domains/groups.hpp"
#include "crowdy/kit/actions.hpp"
#include "crowdy/kit/social.hpp"
#include "crowdy/kit/wire.hpp"

/// The app-scoped Game Kit — client.kit(appId): parties, guilds and chat
/// rooms over the platform's teams and channels (social()). Game rules and
/// state are ck-exec hubs (client.exec()); the wire codecs (kit/wire.hpp) and
/// runOptimisticAction (kit/actions.hpp) stand alone.
namespace crowdy::kit {

struct GameKitOptions {
  SocialKitOptions social;
};

class GameKitClient {
 public:
  /// `teams`/`channels`/`connection` are the social helpers' composition
  /// points; pass what your game uses.
  GameKitClient(std::string appId, domains::GameAppsAPI& gameApps,
                domains::TeamsAPI* teams = nullptr,
                domains::ChannelsAPI* channels = nullptr,
                replication::Connection* connection = nullptr, GameKitOptions options = {})
      : social_(appId, teams, channels, gameApps, connection, options.social) {}

  SocialKit& social() { return social_; }

 private:
  SocialKit social_;
};

/// Build a GameKitClient over a CrowdyClient's domains — the C++ analog of
/// CrowdyJS's client.kit(appId). Pass the replication connection when the
/// social helpers should send channel messages natively.
inline GameKitClient makeKit(CrowdyClient& client, std::string appId,
                             replication::Connection* connection = nullptr,
                             GameKitOptions options = {}) {
  return GameKitClient(std::move(appId), client.gameApps(), &client.teams(),
                       &client.channels(), connection, std::move(options));
}

}  // namespace crowdy::kit
