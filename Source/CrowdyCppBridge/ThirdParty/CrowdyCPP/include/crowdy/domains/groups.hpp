#pragma once

#include <functional>
#include <utility>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

/// client.teams() and client.channels() — app-scoped groups (membership,
/// roles, join policies). Teams are player groups (guilds/parties); channels
/// are messaging groups whose realtime delivery happens over the replication
/// client. Both target the Game API.
namespace crowdy::domains {

/// client.teams()
class TeamsAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  graphql::Json mine(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::teams::kMyTeamsDocument, vars);
  }

  void mineAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(gen::teams::kMyTeamsDocument, vars, {}, std::move(cb));
  }

  graphql::Json list(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::teams::kTeamsDocument, vars);
  }

  void listAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(gen::teams::kTeamsDocument, vars, {}, std::move(cb));
  }

  graphql::Json get(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::teams::kTeamDocument, vars);
  }

  void getAsync(std::string_view groupId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    execUnwrapAsync(gen::teams::kTeamDocument, vars, {}, std::move(cb));
  }

  graphql::Json members(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::teams::kTeamMembersDocument, vars);
  }

  void membersAsync(std::string_view groupId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    execUnwrapAsync(gen::teams::kTeamMembersDocument, vars, {}, std::move(cb));
  }

  graphql::Json roles(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::teams::kTeamRolesDocument, vars);
  }

  void rolesAsync(std::string_view groupId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    execUnwrapAsync(gen::teams::kTeamRolesDocument, vars, {}, std::move(cb));
  }

  graphql::Json policy(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::teams::kTeamPolicyDocument, vars);
  }

  void policyAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(gen::teams::kTeamPolicyDocument, vars, {}, std::move(cb));
  }

  graphql::Json create(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::teams::kCreateTeamDocument, vars);
  }

  void createAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::teams::kCreateTeamDocument, vars, {}, std::move(cb));
  }

  graphql::Json update(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::teams::kUpdateTeamDocument, vars);
  }

  void updateAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::teams::kUpdateTeamDocument, vars, {}, std::move(cb));
  }

  /// Destructive; pass an idempotencyKey for safe retries.
  graphql::Json remove(std::string_view groupId, std::string_view idempotencyKey = {}) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    return execUnwrap(gen::teams::kDeleteTeamDocument, vars);
  }

  void removeAsync(std::string_view groupId, std::string_view idempotencyKey,
                   graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    execUnwrapAsync(gen::teams::kDeleteTeamDocument, vars, {}, std::move(cb));
  }

  graphql::Json setPolicy(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::teams::kSetTeamPolicyDocument, vars);
  }

  void setPolicyAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::teams::kSetTeamPolicyDocument, vars, {}, std::move(cb));
  }

  graphql::Json join(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::teams::kJoinTeamDocument, vars);
  }

  void joinAsync(std::string_view groupId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    execUnwrapAsync(gen::teams::kJoinTeamDocument, vars, {}, std::move(cb));
  }

  graphql::Json requestToJoin(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::teams::kRequestToJoinTeamDocument, vars);
  }

  void requestToJoinAsync(std::string_view groupId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    execUnwrapAsync(gen::teams::kRequestToJoinTeamDocument, vars, {}, std::move(cb));
  }

  graphql::Json leave(std::string_view groupId, std::string_view idempotencyKey = {}) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    return execUnwrap(gen::teams::kLeaveTeamDocument, vars);
  }

  void leaveAsync(std::string_view groupId, std::string_view idempotencyKey,
                  graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    execUnwrapAsync(gen::teams::kLeaveTeamDocument, vars, {}, std::move(cb));
  }

  graphql::Json addMember(std::string_view groupId, std::string_view userId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    vars["userId"] = userId;
    return execUnwrap(gen::teams::kAddTeamMemberDocument, vars);
  }

  void addMemberAsync(std::string_view groupId, std::string_view userId,
                      graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    vars["userId"] = userId;
    execUnwrapAsync(gen::teams::kAddTeamMemberDocument, vars, {}, std::move(cb));
  }

  graphql::Json removeMember(std::string_view groupId, std::string_view userId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    vars["userId"] = userId;
    return execUnwrap(gen::teams::kRemoveTeamMemberDocument, vars);
  }

  void removeMemberAsync(std::string_view groupId, std::string_view userId,
                         graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    vars["userId"] = userId;
    execUnwrapAsync(gen::teams::kRemoveTeamMemberDocument, vars, {}, std::move(cb));
  }

  graphql::Json setMemberRoles(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::teams::kSetTeamMemberRolesDocument, vars);
  }

  void setMemberRolesAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::teams::kSetTeamMemberRolesDocument, vars, {}, std::move(cb));
  }

  graphql::Json createRole(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::teams::kCreateTeamRoleDocument, vars);
  }

  void createRoleAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::teams::kCreateTeamRoleDocument, vars, {}, std::move(cb));
  }

  graphql::Json updateRole(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::teams::kUpdateTeamRoleDocument, vars);
  }

  void updateRoleAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::teams::kUpdateTeamRoleDocument, vars, {}, std::move(cb));
  }

  graphql::Json deleteRole(std::string_view groupRoleId) const {
    graphql::JVal vars;
    vars["groupRoleId"] = groupRoleId;
    return execUnwrap(gen::teams::kDeleteTeamRoleDocument, vars);
  }

  void deleteRoleAsync(std::string_view groupRoleId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["groupRoleId"] = groupRoleId;
    execUnwrapAsync(gen::teams::kDeleteTeamRoleDocument, vars, {}, std::move(cb));
  }
};

/// client.channels()
class ChannelsAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  graphql::Json mine(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::channels::kMyChannelsDocument, vars);
  }

  void mineAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(gen::channels::kMyChannelsDocument, vars, {}, std::move(cb));
  }

  graphql::Json list(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::channels::kChannelsDocument, vars);
  }

  void listAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(gen::channels::kChannelsDocument, vars, {}, std::move(cb));
  }

  graphql::Json get(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::channels::kChannelDocument, vars);
  }

  void getAsync(std::string_view groupId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    execUnwrapAsync(gen::channels::kChannelDocument, vars, {}, std::move(cb));
  }

  graphql::Json members(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::channels::kChannelMembersDocument, vars);
  }

  void membersAsync(std::string_view groupId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    execUnwrapAsync(gen::channels::kChannelMembersDocument, vars, {}, std::move(cb));
  }

  graphql::Json roles(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::channels::kChannelRolesDocument, vars);
  }

  void rolesAsync(std::string_view groupId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    execUnwrapAsync(gen::channels::kChannelRolesDocument, vars, {}, std::move(cb));
  }

  graphql::Json policy(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::channels::kChannelPolicyDocument, vars);
  }

  void policyAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(gen::channels::kChannelPolicyDocument, vars, {}, std::move(cb));
  }

  graphql::Json create(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::channels::kCreateChannelDocument, vars);
  }

  void createAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::channels::kCreateChannelDocument, vars, {}, std::move(cb));
  }

  graphql::Json update(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::channels::kUpdateChannelDocument, vars);
  }

  void updateAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::channels::kUpdateChannelDocument, vars, {}, std::move(cb));
  }

  graphql::Json remove(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::channels::kDeleteChannelDocument, vars);
  }

  void removeAsync(std::string_view groupId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    execUnwrapAsync(gen::channels::kDeleteChannelDocument, vars, {}, std::move(cb));
  }

  graphql::Json setPolicy(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::channels::kSetChannelPolicyDocument, vars);
  }

  void setPolicyAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::channels::kSetChannelPolicyDocument, vars, {}, std::move(cb));
  }

  graphql::Json join(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::channels::kJoinChannelDocument, vars);
  }

  void joinAsync(std::string_view groupId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    execUnwrapAsync(gen::channels::kJoinChannelDocument, vars, {}, std::move(cb));
  }

  graphql::Json requestToJoin(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::channels::kRequestToJoinChannelDocument, vars);
  }

  void requestToJoinAsync(std::string_view groupId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    execUnwrapAsync(gen::channels::kRequestToJoinChannelDocument, vars, {}, std::move(cb));
  }

  graphql::Json leave(std::string_view groupId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    return execUnwrap(gen::channels::kLeaveChannelDocument, vars);
  }

  void leaveAsync(std::string_view groupId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    execUnwrapAsync(gen::channels::kLeaveChannelDocument, vars, {}, std::move(cb));
  }

  graphql::Json addMember(std::string_view groupId, std::string_view userId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    vars["userId"] = userId;
    return execUnwrap(gen::channels::kAddChannelMemberDocument, vars);
  }

  void addMemberAsync(std::string_view groupId, std::string_view userId,
                      graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    vars["userId"] = userId;
    execUnwrapAsync(gen::channels::kAddChannelMemberDocument, vars, {}, std::move(cb));
  }

  graphql::Json removeMember(std::string_view groupId, std::string_view userId) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    vars["userId"] = userId;
    return execUnwrap(gen::channels::kRemoveChannelMemberDocument, vars);
  }

  void removeMemberAsync(std::string_view groupId, std::string_view userId,
                         graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["groupId"] = groupId;
    vars["userId"] = userId;
    execUnwrapAsync(gen::channels::kRemoveChannelMemberDocument, vars, {}, std::move(cb));
  }

  graphql::Json setMemberRoles(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::channels::kSetChannelMemberRolesDocument, vars);
  }

  void setMemberRolesAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::channels::kSetChannelMemberRolesDocument, vars, {}, std::move(cb));
  }

  graphql::Json createRole(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::channels::kCreateChannelRoleDocument, vars);
  }

  void createRoleAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::channels::kCreateChannelRoleDocument, vars, {}, std::move(cb));
  }

  graphql::Json updateRole(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::channels::kUpdateChannelRoleDocument, vars);
  }

  void updateRoleAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::channels::kUpdateChannelRoleDocument, vars, {}, std::move(cb));
  }

  graphql::Json deleteRole(std::string_view groupRoleId) const {
    graphql::JVal vars;
    vars["groupRoleId"] = groupRoleId;
    return execUnwrap(gen::channels::kDeleteChannelRoleDocument, vars);
  }

  void deleteRoleAsync(std::string_view groupRoleId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["groupRoleId"] = groupRoleId;
    execUnwrapAsync(gen::channels::kDeleteChannelRoleDocument, vars, {}, std::move(cb));
  }
};

}  // namespace crowdy::domains
