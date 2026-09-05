#pragma once

#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "crowdy/core/bytes.hpp"
#include "crowdy/core/uuid.hpp"
#include "crowdy/domains/game_apps.hpp"
#include "crowdy/domains/groups.hpp"
#include "crowdy/kit/inventory.hpp"
#include "crowdy/kit/objects.hpp"
#include "crowdy/replication/connection.hpp"

/// Social — parties, guilds, and chat rooms in familiar words, wrapped over
/// the platform's teams (membership + roles) and channels
/// (location-independent messaging). Mirrors CrowdyJS's guildBlueprint /
/// kit.social.
namespace crowdy::kit {

// ---------------------------------------------------------------------------
// Guild blueprint (the only deployable in the social kit)
// ---------------------------------------------------------------------------

/// Options for guildBlueprint.
struct GuildBlueprintOptions {
  /// Prefix for the composed type names ("Guild" -> GuildHall /
  /// GuildBankInventory). Defaults to "Guild".
  std::string typePrefix = "Guild";
  /// The guild team's group id — the hall's group_permission policy checks
  /// membership of THIS group. Create the team first (SocialKit::guildCreate),
  /// then deploy the blueprint. Deploy one prefixed blueprint per guild that
  /// needs its own hall.
  std::string guildGroupId;
  /// Optional group permission key the hall requires (e.g. a custom
  /// "use_hall" role permission). Empty admits every guild member.
  std::string hallPermission;
  /// Also compose a guild-bank inventory (<prefix>Bank*). Defaults to true.
  bool bank = true;
};

/// Names derived by guildBlueprint for a given prefix.
struct GuildNames {
  std::string hallType;
  std::string openHallFn;
  std::string closeHallFn;
  std::string bankTypePrefix;
  std::string bankInventoryType;
  std::string bankStackType;
};

/// Compute the type/function names a guild blueprint (and the social kit's
/// guild helpers) uses.
inline GuildNames guildNames(std::string_view typePrefix = "Guild") {
  const LockNames locks = lockNames(std::string(typePrefix) + "Hall");
  const InventoryNames bank = inventoryNames(std::string(typePrefix) + "Bank");
  GuildNames n;
  n.hallType = locks.objectType;
  n.openHallFn = locks.openFn;
  n.closeHallFn = locks.closeFn;
  n.bankTypePrefix = std::string(typePrefix) + "Bank";
  n.bankInventoryType = bank.inventoryType;
  n.bankStackType = bank.stackType;
  return n;
}

/// Composite blueprint for a guild's shared assets — a demonstration of
/// blueprint composition: a GuildHall lockable whose open/close is gated on
/// membership of the guild team (group_permission), plus a prefixed
/// guild-bank inventory (GuildBankInventory / GuildBankItemStack) for shared
/// storage. The guild itself is a team (SocialKit's guild helpers), its chat
/// a channel, and its territory a grid group-grant
/// (SocialKit::guildClaimTerritory) — no new model surface needed for those.
///
/// Runtime counterparts: SocialKit (team/chat/territory), ObjectsKit
/// constructed with guildNames().hallType (the hall), and InventoryKit
/// constructed with guildNames().bankTypePrefix (the bank).
inline KitBlueprint guildBlueprint(const GuildBlueprintOptions& options) {
  if (options.guildGroupId.empty()) {
    throw std::invalid_argument(
        "guildBlueprint requires guildGroupId — create the guild team first, then deploy");
  }
  LockBlueprintOptions hallOptions;
  hallOptions.objectTypeName = options.typePrefix + "Hall";
  hallOptions.authority = {
      LockAuthority::groupPermission(options.guildGroupId, options.hallPermission)};
  std::vector<KitBlueprint> blueprints;
  blueprints.push_back(lockBlueprint(hallOptions));
  if (options.bank) {
    InventoryBlueprintOptions bankOptions;
    bankOptions.typePrefix = options.typePrefix + "Bank";
    blueprints.push_back(inventoryBlueprint(bankOptions));
  }
  std::string name = options.typePrefix;
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return composeBlueprints(std::move(name), blueprints);
}

// ---------------------------------------------------------------------------
// Runtime social kit
// ---------------------------------------------------------------------------

/// Options for SocialKit.
struct SocialKitOptions {
  /// The 32-ASCII-char actor uuid used as the sender id on chat messages.
  /// Empty defaults to a random uuid per kit instance — set it to YOUR actor
  /// uuid so receivers can attribute messages.
  std::string actorUuid;
  /// Name prefix for party teams/channels. Defaults to "party:".
  std::string partyPrefix = "party:";
  /// Name prefix for guild teams/channels. Defaults to "guild:".
  std::string guildPrefix = "guild:";
};

/// A party or guild: the team plus its paired chat channel.
struct KitGroupWithChannel {
  /// The team's group id (membership/roles live here).
  std::string teamId;
  /// The paired chat channel's group id (may be "" if pairing failed).
  std::string channelId;
  std::string name;
};

/// Options for SocialKit::guildCreate.
struct KitGuildCreateOptions {
  std::string membershipPolicy = "request";
  std::string description = "Guild";
};

/// Input for SocialKit::guildCreateRole.
struct KitGuildRoleInput {
  std::string roleName;
  std::optional<std::vector<std::string>> permissions;
  std::optional<int> rank;
};

/// Options for SocialKit::guildClaimTerritory.
struct KitClaimTerritoryOptions {
  std::vector<std::string> permissionKeys{"access", "update_voxel_data"};
  std::string groupRoleId;  ///< empty grants to every guild member
};

/// Runtime social helpers — parties, guilds, and chat rooms wrapped over the
/// platform's teams (membership + roles) and channels (location-independent
/// messaging). No model schema needed; the only deployable is the optional
/// guildBlueprint composite (guild hall + bank).
///
/// Conventions: a party is a team named "party:<name>" paired with an
/// equally-named channel; a guild is "guild:<name>" likewise. Guild
/// territory = a grid group-grant (guildClaimTerritory), enforced by the
/// replication layer.
///
/// Chat sends publish over the native replication connection
/// (Connection::sendChannelMessage). There is no callback subscription here:
/// receive chat by polling the WorldSession channel inbox
/// (session::WorldSession::channelInbox()), or attach a channelMessage
/// handler to your replication::Connection directly.
///
/// The teams/channels domains and the connection are optional; methods that
/// need a missing dependency throw std::runtime_error.
class SocialKit {
 public:
  SocialKit(std::string appId, domains::TeamsAPI* teams, domains::ChannelsAPI* channels,
            domains::GameAppsAPI& gameApps, replication::Connection* connection = nullptr,
            SocialKitOptions options = {})
      : appId_(std::move(appId)),
        teams_(teams),
        channels_(channels),
        gameApps_(gameApps),
        connection_(connection),
        partyPrefix_(std::move(options.partyPrefix)),
        guildPrefix_(std::move(options.guildPrefix)) {
    if (options.actorUuid.empty()) {
      actorUuid_ = core::generateActorUuid();
    } else if (!core::actorUuidFromString(options.actorUuid, actorUuid_)) {
      throw std::invalid_argument("SocialKit actorUuid must be exactly 32 ASCII characters");
    }
  }

  /// The actor uuid stamped on outgoing chat messages.
  const core::ActorUuid& actorUuid() const { return actorUuid_; }

  // ----- Parties: small invite-based groups with their own chat channel ------

  /// Create a party (invite-only by default). The creator becomes leader.
  KitGroupWithChannel partyCreate(std::string_view name) {
    return createPair(partyPrefix_ + std::string(name), "invite", "Party");
  }

  /// Find a party by name.
  std::optional<KitGroupWithChannel> partyFind(std::string_view name) {
    return findPair(partyPrefix_ + std::string(name));
  }

  /// Invite (add) a player to the party — requires the leader's
  /// manage_members. Adds them to the chat channel membership too.
  Json partyInvite(const KitGroupWithChannel& party, std::string_view userId) {
    Json member = requireTeams().addMember(party.teamId, userId);
    if (!party.channelId.empty()) requireChannels().addMember(party.channelId, userId);
    return member;
  }

  /// Join an open party (and its chat channel) as the caller.
  Json partyJoin(const KitGroupWithChannel& party) {
    Json member = requireTeams().join(party.teamId);
    if (!party.channelId.empty()) requireChannels().join(party.channelId);
    return member;
  }

  /// Leave the party (and its chat channel).
  Json partyLeave(const KitGroupWithChannel& party) {
    if (!party.channelId.empty()) requireChannels().leave(party.channelId);
    return requireTeams().leave(party.teamId);
  }

  /// The party roster.
  Json partyMembers(const KitGroupWithChannel& party) {
    return requireTeams().members(party.teamId);
  }

  // ----- Guilds: persistent role-based organizations with chat + territory ---

  /// Create a guild (request-to-join by default). The creator becomes leader.
  KitGroupWithChannel guildCreate(std::string_view name,
                                  const KitGuildCreateOptions& options = {}) {
    return createPair(guildPrefix_ + std::string(name), options.membershipPolicy,
                      options.description);
  }

  /// Find a guild by name.
  std::optional<KitGroupWithChannel> guildFind(std::string_view name) {
    return findPair(guildPrefix_ + std::string(name));
  }

  /// The guild roster (members + pending join requests).
  Json guildRoster(const KitGroupWithChannel& guild) {
    return requireTeams().members(guild.teamId);
  }

  /// The guild's roles (including the system leader role).
  Json guildRoles(const KitGroupWithChannel& guild) {
    return requireTeams().roles(guild.teamId);
  }

  /// Create a custom guild role (requires manage_roles).
  Json guildCreateRole(const KitGroupWithChannel& guild, const KitGuildRoleInput& input) {
    JVal vars;
    vars["groupId"] = guild.teamId;
    vars["roleName"] = input.roleName;
    if (input.permissions) {
      JArray permissions;
      for (const auto& p : *input.permissions) permissions.push_back(JVal(p));
      vars["permissions"] = JVal(std::move(permissions));
    }
    if (input.rank) vars["rank"] = *input.rank;
    return requireTeams().createRole(vars);
  }

  /// Promote/demote a member: REPLACES their role set (requires
  /// manage_roles).
  Json guildPromote(const KitGroupWithChannel& guild, std::string_view userId,
                    const std::vector<std::string>& roleIds) {
    JVal vars;
    vars["groupId"] = guild.teamId;
    vars["userId"] = userId;
    JArray ids;
    for (const auto& id : roleIds) ids.push_back(JVal(id));
    vars["roleIds"] = JVal(std::move(ids));
    return requireTeams().setMemberRoles(vars);
  }

  /// Claim territory for the guild: grants runtime permission keys on a grid
  /// to every guild member (optionally one role) — enforced by the
  /// replication layer on movement/voxel writes. Requires grid admin rights
  /// on the app.
  Json guildClaimTerritory(const KitGroupWithChannel& guild, std::string_view gridId,
                           const KitClaimTerritoryOptions& options = {}) {
    JVal input;
    input["appId"] = appId_;
    input["gridId"] = gridId;
    input["groupId"] = guild.teamId;
    JArray keys;
    for (const auto& key : options.permissionKeys) keys.push_back(JVal(key));
    input["permissionKeys"] = JVal(std::move(keys));
    if (!options.groupRoleId.empty()) input["groupRoleId"] = options.groupRoleId;
    return gameApps_.assignGroup(input);
  }

  // ----- Chat rooms: named channels with realtime text delivery ---------------

  /// Find-or-create a chat room (an open channel) by name.
  Json chatRoom(std::string_view name) {
    domains::ChannelsAPI& channels = requireChannels();
    Json existing;
    channels.list(appId_).forEach([&](Json c) {
      if (existing.ok()) return;
      if (c["name"].asStringView() == name) existing = c;
    });
    if (existing.ok()) return existing;
    JVal input;
    input["appId"] = appId_;
    input["name"] = name;
    return channels.create(input);
  }

  /// Join a chat room.
  Json chatJoin(std::string_view channelId) { return requireChannels().join(channelId); }

  /// Send a UTF-8 text message to a room (requires channel membership with
  /// send_messages). Delivery is fan-out to every active member's
  /// replication connection, regardless of world location. Returns false
  /// when the connection refuses the send (e.g. not connected).
  bool chatSend(std::int64_t channelId, std::string_view text) {
    return requireConnection().sendChannelMessage(channelId, actorUuid_, asBytes(text)).ok();
  }

 private:
  domains::TeamsAPI& requireTeams() {
    if (!teams_) {
      throw std::runtime_error(
          "SocialKit needs the teams domain — construct it with a TeamsAPI");
    }
    return *teams_;
  }

  domains::ChannelsAPI& requireChannels() {
    if (!channels_) {
      throw std::runtime_error(
          "SocialKit needs the channels domain — construct it with a ChannelsAPI");
    }
    return *channels_;
  }

  replication::Connection& requireConnection() {
    if (!connection_) {
      throw std::runtime_error(
          "SocialKit needs a replication connection — construct it with a Connection");
    }
    return *connection_;
  }

  static std::string groupIdString(const Json& group) {
    Json id = group["groupId"];
    if (id.isString()) return id.asString();
    return std::to_string(id.asInt64());
  }

  /// Create a team + equally-named chat channel pair.
  KitGroupWithChannel createPair(const std::string& name, std::string_view membershipPolicy,
                                 std::string_view description) {
    JVal teamInput;
    teamInput["appId"] = appId_;
    teamInput["name"] = name;
    teamInput["description"] = description;
    if (!membershipPolicy.empty()) teamInput["membershipPolicy"] = membershipPolicy;
    Json team = requireTeams().create(teamInput);

    JVal channelInput;
    channelInput["appId"] = appId_;
    channelInput["name"] = name;
    channelInput["description"] = "Chat for " + name;
    Json channel = requireChannels().create(channelInput);

    return {groupIdString(team), groupIdString(channel), name};
  }

  /// Find a team + channel pair by its full name.
  std::optional<KitGroupWithChannel> findPair(const std::string& name) {
    Json team, channel;
    requireTeams().list(appId_).forEach([&](Json t) {
      if (!team.ok() && t["name"].asString() == name) team = t;
    });
    if (!team.ok()) return std::nullopt;
    requireChannels().list(appId_).forEach([&](Json c) {
      if (!channel.ok() && c["name"].asString() == name) channel = c;
    });
    KitGroupWithChannel out;
    out.teamId = groupIdString(team);
    out.channelId = channel.ok() ? groupIdString(channel) : "";
    out.name = name;
    return out;
  }

  std::string appId_;
  domains::TeamsAPI* teams_;
  domains::ChannelsAPI* channels_;
  domains::GameAppsAPI& gameApps_;
  replication::Connection* connection_;
  core::ActorUuid actorUuid_{};
  std::string partyPrefix_;
  std::string guildPrefix_;
};

}  // namespace crowdy::kit
