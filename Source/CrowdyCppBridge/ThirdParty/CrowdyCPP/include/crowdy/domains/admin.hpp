#pragma once

#include <functional>
#include <utility>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/domains/game_apps.hpp"
#include "crowdy/generated/operations.hpp"

/// Studio-admin surface — client.admin().*. Privileged organization/app
/// administration: drive from a trusted context (studio backend / tooling)
/// with an org-scoped or admin token; the server enforces the relevant org or
/// app permission on every call.
namespace crowdy::domains {

namespace detail {
/// Shared helpers for the admin wrappers.
class AdminDomain : public DomainBase {
 public:
  using DomainBase::DomainBase;

 protected:
  graphql::JVal one(std::string_view k, const graphql::JVal& v) const {
    graphql::JVal vars;
    vars[k] = v;
    return vars;
  }
  graphql::JVal two(std::string_view k1, const graphql::JVal& v1, std::string_view k2,
                    const graphql::JVal& v2) const {
    graphql::JVal vars;
    vars[k1] = v1;
    vars[k2] = v2;
    return vars;
  }
};
}  // namespace detail

/// client.admin().organizations() — orgs, members, RBAC roles, org tokens.
class OrganizationsAPI : public detail::AdminDomain {
 public:
  using AdminDomain::AdminDomain;
  graphql::Json mine() const { return execUnwrap(gen::organizations::kMyOrganizationsDocument); }
  void mineAsync(graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::organizations::kMyOrganizationsDocument, graphql::JVal(), {},
                    std::move(cb));
  }
  graphql::Json get(std::string_view id) const {
    return execUnwrap(gen::organizations::kOrganizationDocument, one("id", graphql::JVal(id)));
  }
  void getAsync(std::string_view id, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::organizations::kOrganizationDocument, one("id", graphql::JVal(id)), {},
                    std::move(cb));
  }
  graphql::Json getBySlug(std::string_view slug) const {
    return execUnwrap(gen::organizations::kOrganizationBySlugDocument,
                      one("slug", graphql::JVal(slug)));
  }
  void getBySlugAsync(std::string_view slug, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::organizations::kOrganizationBySlugDocument,
                    one("slug", graphql::JVal(slug)), {}, std::move(cb));
  }
  graphql::Json create(const graphql::JVal& input) const {
    return execUnwrap(gen::organizations::kCreateOrganizationDocument, one("input", input));
  }
  void createAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::organizations::kCreateOrganizationDocument, one("input", input), {},
                    std::move(cb));
  }
  graphql::Json members(std::string_view orgId) const {
    return execUnwrap(gen::organizations::kOrgMembersDocument, one("orgId", graphql::JVal(orgId)));
  }
  void membersAsync(std::string_view orgId, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::organizations::kOrgMembersDocument, one("orgId", graphql::JVal(orgId)), {},
                    std::move(cb));
  }
  graphql::Json inviteMember(const graphql::JVal& input) const {
    return execUnwrap(gen::organizations::kInviteOrgMemberDocument, one("input", input));
  }
  void inviteMemberAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::organizations::kInviteOrgMemberDocument, one("input", input), {},
                    std::move(cb));
  }
  graphql::Json removeMember(std::string_view orgId, std::string_view userId) const {
    return execUnwrap(gen::organizations::kRemoveOrgMemberDocument,
                      two("orgId", graphql::JVal(orgId), "userId", graphql::JVal(userId)));
  }
  void removeMemberAsync(std::string_view orgId, std::string_view userId,
                         graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::organizations::kRemoveOrgMemberDocument,
                    two("orgId", graphql::JVal(orgId), "userId", graphql::JVal(userId)), {},
                    std::move(cb));
  }
  graphql::Json memberRoles(std::string_view orgMemberId) const {
    return execUnwrap(gen::organizations::kMemberRolesDocument,
                      one("orgMemberId", graphql::JVal(orgMemberId)));
  }
  void memberRolesAsync(std::string_view orgMemberId, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::organizations::kMemberRolesDocument,
                    one("orgMemberId", graphql::JVal(orgMemberId)), {}, std::move(cb));
  }
  graphql::Json updateMemberRoles(const graphql::JVal& vars) const {
    return execUnwrap(gen::organizations::kUpdateOrgMemberRolesDocument, vars);
  }
  void updateMemberRolesAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::organizations::kUpdateOrgMemberRolesDocument, vars, {}, std::move(cb));
  }
  graphql::Json permissions() const {
    return execUnwrap(gen::organizations::kOrgPermissionsDocument);
  }
  void permissionsAsync(graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::organizations::kOrgPermissionsDocument, graphql::JVal(), {},
                    std::move(cb));
  }
  graphql::Json roles(std::string_view orgId) const {
    return execUnwrap(gen::organizations::kOrgRolesDocument, one("orgId", graphql::JVal(orgId)));
  }
  void rolesAsync(std::string_view orgId, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::organizations::kOrgRolesDocument, one("orgId", graphql::JVal(orgId)), {},
                    std::move(cb));
  }
  graphql::Json createRole(const graphql::JVal& input) const {
    return execUnwrap(gen::organizations::kCreateOrgRoleDocument, one("input", input));
  }
  void createRoleAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::organizations::kCreateOrgRoleDocument, one("input", input), {},
                    std::move(cb));
  }
  graphql::Json updateRole(std::string_view orgRoleId, const graphql::JVal& input) const {
    return execUnwrap(gen::organizations::kUpdateOrgRoleDocument,
                      two("orgRoleId", graphql::JVal(orgRoleId), "input", input));
  }
  void updateRoleAsync(std::string_view orgRoleId, const graphql::JVal& input,
                       graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::organizations::kUpdateOrgRoleDocument,
                    two("orgRoleId", graphql::JVal(orgRoleId), "input", input), {}, std::move(cb));
  }
  graphql::Json deleteRole(std::string_view orgRoleId) const {
    return execUnwrap(gen::organizations::kDeleteOrgRoleDocument,
                      one("orgRoleId", graphql::JVal(orgRoleId)));
  }
  void deleteRoleAsync(std::string_view orgRoleId, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::organizations::kDeleteOrgRoleDocument,
                    one("orgRoleId", graphql::JVal(orgRoleId)), {}, std::move(cb));
  }
  graphql::Json tokens(std::string_view orgId) const {
    return execUnwrap(gen::organizations::kOrgTokensDocument, one("orgId", graphql::JVal(orgId)));
  }
  void tokensAsync(std::string_view orgId, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::organizations::kOrgTokensDocument, one("orgId", graphql::JVal(orgId)), {},
                    std::move(cb));
  }
  graphql::Json createToken(const graphql::JVal& input) const {
    return execUnwrap(gen::organizations::kCreateOrgTokenDocument, one("input", input));
  }
  void createTokenAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::organizations::kCreateOrgTokenDocument, one("input", input), {},
                    std::move(cb));
  }
  graphql::Json updateToken(std::string_view orgTokenId, const graphql::JVal& input) const {
    return execUnwrap(gen::organizations::kUpdateOrgTokenDocument,
                      two("orgTokenId", graphql::JVal(orgTokenId), "input", input));
  }
  void updateTokenAsync(std::string_view orgTokenId, const graphql::JVal& input,
                        graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::organizations::kUpdateOrgTokenDocument,
                    two("orgTokenId", graphql::JVal(orgTokenId), "input", input), {},
                    std::move(cb));
  }
  graphql::Json revokeToken(std::string_view orgTokenId) const {
    return execUnwrap(gen::organizations::kRevokeOrgTokenDocument,
                      one("orgTokenId", graphql::JVal(orgTokenId)));
  }
  void revokeTokenAsync(std::string_view orgTokenId, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::organizations::kRevokeOrgTokenDocument,
                    one("orgTokenId", graphql::JVal(orgTokenId)), {}, std::move(cb));
  }
};

/// client.admin().apps() — app registry + routing.
class AppsAPI : public detail::AdminDomain {
 public:
  using AdminDomain::AdminDomain;

  /// Read the app's player-code admission mode. Requires
  /// view_compute_diagnostics.
  graphql::Json codeAdmissionMode(std::string_view appId) const {
    return execUnwrap(gen::apps::documentFor("AppCodeAdmissionMode"),
                      one("appId", graphql::JVal(appId)),
                      "AppCodeAdmissionMode");
  }
  void codeAdmissionModeAsync(std::string_view appId,
                              graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::apps::documentFor("AppCodeAdmissionMode"),
                    one("appId", graphql::JVal(appId)),
                    "AppCodeAdmissionMode", std::move(cb));
  }

  /// List code/author/org allow-list entries newest-first. Revoked audit rows
  /// are omitted unless includeRevoked is true.
  graphql::Json codeAdmissions(std::string_view appId,
                               bool includeRevoked = false) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["includeRevoked"] = includeRevoked;
    return execUnwrap(gen::apps::documentFor("AppCodeAdmissions"), vars,
                      "AppCodeAdmissions");
  }
  void codeAdmissionsAsync(std::string_view appId, bool includeRevoked,
                           graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["includeRevoked"] = includeRevoked;
    execUnwrapAsync(gen::apps::documentFor("AppCodeAdmissions"), vars,
                    "AppCodeAdmissions", std::move(cb));
  }

  /// Set IMPLICIT_ALLOW or ALLOW_LIST. Switching to ALLOW_LIST drains
  /// unadmitted code at activation while deploy/compile remain available.
  graphql::Json setCodeAdmissionMode(std::string_view appId,
                                     std::string_view mode) const {
    return execUnwrap(
        gen::apps::documentFor("SetAppCodeAdmissionMode"),
        two("appId", graphql::JVal(appId), "mode", graphql::JVal(mode)),
        "SetAppCodeAdmissionMode");
  }
  void setCodeAdmissionModeAsync(std::string_view appId, std::string_view mode,
                                 graphql::GraphQLCallback cb) const {
    execUnwrapAsync(
        gen::apps::documentFor("SetAppCodeAdmissionMode"),
        two("appId", graphql::JVal(appId), "mode", graphql::JVal(mode)),
        "SetAppCodeAdmissionMode", std::move(cb));
  }

  /// Admit one code listing, author, or org. Admission controls execution only
  /// and never grants source visibility.
  graphql::Json admitCode(const graphql::JVal& input) const {
    return execUnwrap(gen::apps::documentFor("AdmitAppCode"), one("input", input),
                      "AdmitAppCode");
  }
  void admitCodeAsync(const graphql::JVal& input,
                      graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::apps::documentFor("AdmitAppCode"), one("input", input),
                    "AdmitAppCode", std::move(cb));
  }

  /// Revoke an active admission; the server audit-logs and replica-syncs the
  /// change before draining or blocking affected artifacts.
  graphql::Json revokeCodeAdmission(std::string_view appId,
                                    std::string_view admissionId) const {
    return execUnwrap(
        gen::apps::documentFor("RevokeAppCodeAdmission"),
        two("appId", graphql::JVal(appId), "admissionId",
            graphql::JVal(admissionId)),
        "RevokeAppCodeAdmission");
  }
  void revokeCodeAdmissionAsync(std::string_view appId,
                                std::string_view admissionId,
                                graphql::GraphQLCallback cb) const {
    execUnwrapAsync(
        gen::apps::documentFor("RevokeAppCodeAdmission"),
        two("appId", graphql::JVal(appId), "admissionId",
            graphql::JVal(admissionId)),
        "RevokeAppCodeAdmission", std::move(cb));
  }

  graphql::Json mine() const { return execUnwrap(gen::apps::kMyAppsDocument); }
  void mineAsync(graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::apps::kMyAppsDocument, graphql::JVal(), {}, std::move(cb));
  }
  graphql::Json get(std::string_view appId) const {
    return execUnwrap(gen::apps::kAppDocument, one("appId", graphql::JVal(appId)));
  }
  void getAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::apps::kAppDocument, one("appId", graphql::JVal(appId)), {},
                    std::move(cb));
  }
  graphql::Json getBySlug(std::string_view orgSlug, std::string_view appSlug) const {
    return execUnwrap(gen::apps::kAppBySlugDocument,
                      two("orgSlug", graphql::JVal(orgSlug), "appSlug", graphql::JVal(appSlug)));
  }
  void getBySlugAsync(std::string_view orgSlug, std::string_view appSlug,
                      graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::apps::kAppBySlugDocument,
                    two("orgSlug", graphql::JVal(orgSlug), "appSlug", graphql::JVal(appSlug)), {},
                    std::move(cb));
  }
  /// The datacenters this deployment can place a new app in.
  ///
  /// `createApp`'s `datacenter` is required and PERMANENT -- an app is
  /// distributed on app_id, so its shard lands where the id says and moving it
  /// afterwards is a physical shard move. Read the accepted codes from here
  /// rather than hardcoding one.
  graphql::Json placeableDatacenters() const {
    return execUnwrap(gen::platform::kPlaceableDatacentersDocument, graphql::JVal{});
  }
  void placeableDatacentersAsync(graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::platform::kPlaceableDatacentersDocument, graphql::JVal{}, {},
                    std::move(cb));
  }
  graphql::Json forOrg(std::string_view orgSlug) const {
    return execUnwrap(gen::apps::kAppsForOrgDocument, one("orgSlug", graphql::JVal(orgSlug)));
  }
  void forOrgAsync(std::string_view orgSlug, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::apps::kAppsForOrgDocument, one("orgSlug", graphql::JVal(orgSlug)), {},
                    std::move(cb));
  }
  graphql::Json marketplace() const {
    return execUnwrap(gen::apps::documentFor("MarketplaceApps"), graphql::JVal(), "MarketplaceApps");
  }
  void marketplaceAsync(graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::apps::documentFor("MarketplaceApps"), graphql::JVal(), "MarketplaceApps",
                    std::move(cb));
  }
  /// Relay cursor pagination variant of marketplace().
  graphql::Json marketplaceConnection(const graphql::JVal& vars = graphql::JVal()) const {
    return execUnwrap(gen::apps::documentFor("AppsConnection"), vars, "AppsConnection");
  }
  void marketplaceConnectionAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::apps::documentFor("AppsConnection"), vars, "AppsConnection", std::move(cb));
  }
  /// Routing tuple for an app: which game-api endpoint should serve it.
  /// Returns { appId, splitMode, deploymentTarget, gameApiUrl } fields from
  /// the app row; route gameplay to gameApiUrl when non-null.
  graphql::Json routeFor(std::string_view appId) const { return get(appId); }
  void routeForAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    getAsync(appId, std::move(cb));
  }
  graphql::Json create(const graphql::JVal& input) const {
    return execUnwrap(gen::apps::kCreateAppDocument, one("input", input));
  }
  void createAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::apps::kCreateAppDocument, one("input", input), {}, std::move(cb));
  }
  graphql::Json update(std::string_view appId, const graphql::JVal& input) const {
    return execUnwrap(gen::apps::kUpdateAppDocument,
                      two("appId", graphql::JVal(appId), "input", input));
  }
  void updateAsync(std::string_view appId, const graphql::JVal& input,
                   graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::apps::kUpdateAppDocument,
                    two("appId", graphql::JVal(appId), "input", input), {}, std::move(cb));
  }
  graphql::Json archive(std::string_view appId) const {
    return execUnwrap(gen::apps::kArchiveAppDocument, one("appId", graphql::JVal(appId)));
  }
  void archiveAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::apps::kArchiveAppDocument, one("appId", graphql::JVal(appId)), {},
                    std::move(cb));
  }
};

/// client.admin().appAccess() — access tiers + per-user grants.
class AppAccessAPI : public detail::AdminDomain {
 public:
  using AdminDomain::AdminDomain;
  graphql::Json tiers(std::string_view appId) const {
    return execUnwrap(gen::appAccess::kAppAccessTiersDocument, one("appId", graphql::JVal(appId)));
  }
  void tiersAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::appAccess::kAppAccessTiersDocument, one("appId", graphql::JVal(appId)), {},
                    std::move(cb));
  }
  graphql::Json myAccess(std::string_view appId) const {
    return execUnwrap(gen::appAccess::kMyAppAccessDocument, one("appId", graphql::JVal(appId)));
  }
  void myAccessAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::appAccess::kMyAppAccessDocument, one("appId", graphql::JVal(appId)), {},
                    std::move(cb));
  }
  graphql::Json userAccessByApp(const graphql::JVal& vars) const {
    return execUnwrap(gen::appAccess::documentFor("AppUserAccessByApp"), vars, "AppUserAccessByApp");
  }
  void userAccessByAppAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::appAccess::documentFor("AppUserAccessByApp"), vars, "AppUserAccessByApp",
                    std::move(cb));
  }
  /// Relay cursor pagination variant of userAccessByApp().
  graphql::Json userAccessConnection(const graphql::JVal& vars) const {
    return execUnwrap(gen::appAccess::documentFor("AppUserAccessConnection"), vars,
                      "AppUserAccessConnection");
  }
  void userAccessConnectionAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::appAccess::documentFor("AppUserAccessConnection"), vars, "AppUserAccessConnection",
                    std::move(cb));
  }
  graphql::Json grantMemberCandidates(std::string_view appId) const {
    return execUnwrap(gen::appAccess::kAppGrantMemberCandidatesDocument,
                      one("appId", graphql::JVal(appId)));
  }
  void grantMemberCandidatesAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::appAccess::kAppGrantMemberCandidatesDocument,
                    one("appId", graphql::JVal(appId)), {}, std::move(cb));
  }
  graphql::Json runtimePermissions() const {
    return execUnwrap(gen::appAccess::kRuntimePermissionsDocument);
  }
  void runtimePermissionsAsync(graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::appAccess::kRuntimePermissionsDocument, graphql::JVal(), {},
                    std::move(cb));
  }
  graphql::Json createTier(const graphql::JVal& input) const {
    return execUnwrap(gen::appAccess::kCreateAccessTierDocument, one("input", input));
  }
  void createTierAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::appAccess::kCreateAccessTierDocument, one("input", input), {},
                    std::move(cb));
  }
  graphql::Json updateTier(std::string_view tierId, const graphql::JVal& input) const {
    return execUnwrap(gen::appAccess::kUpdateAccessTierDocument,
                      two("tierId", graphql::JVal(tierId), "input", input));
  }
  void updateTierAsync(std::string_view tierId, const graphql::JVal& input,
                       graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::appAccess::kUpdateAccessTierDocument,
                    two("tierId", graphql::JVal(tierId), "input", input), {}, std::move(cb));
  }
  graphql::Json archiveTier(std::string_view tierId) const {
    return execUnwrap(gen::appAccess::kArchiveAccessTierDocument,
                      one("tierId", graphql::JVal(tierId)));
  }
  void archiveTierAsync(std::string_view tierId, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::appAccess::kArchiveAccessTierDocument,
                    one("tierId", graphql::JVal(tierId)), {}, std::move(cb));
  }
  graphql::Json grant(const graphql::JVal& input) const {
    return execUnwrap(gen::appAccess::kGrantAppAccessDocument, one("input", input));
  }
  void grantAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::appAccess::kGrantAppAccessDocument, one("input", input), {},
                    std::move(cb));
  }
  graphql::Json grantMine(std::string_view appId) const {
    return execUnwrap(gen::appAccess::kGrantMyAppAccessDocument,
                      one("appId", graphql::JVal(appId)));
  }
  void grantMineAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::appAccess::kGrantMyAppAccessDocument, one("appId", graphql::JVal(appId)),
                    {}, std::move(cb));
  }
  graphql::Json claimFree(std::string_view appId) const {
    return execUnwrap(gen::appAccess::kClaimFreeAppAccessDocument,
                      one("appId", graphql::JVal(appId)));
  }
  void claimFreeAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::appAccess::kClaimFreeAppAccessDocument, one("appId", graphql::JVal(appId)),
                    {}, std::move(cb));
  }
  graphql::Json revoke(std::string_view appId, std::string_view userId) const {
    return execUnwrap(gen::appAccess::kRevokeAppAccessDocument,
                      two("appId", graphql::JVal(appId), "userId", graphql::JVal(userId)));
  }
  void revokeAsync(std::string_view appId, std::string_view userId,
                   graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::appAccess::kRevokeAppAccessDocument,
                    two("appId", graphql::JVal(appId), "userId", graphql::JVal(userId)), {},
                    std::move(cb));
  }
  /// Suspend a player's access to an app until `until` (ISO-8601): a timed ban that
  /// lifts by itself. Until then their gameplay tokens are refused with
  /// ACCESS_SUSPENDED (graphql::accessRefusalOf() reads its `suspendedUntil`), and
  /// the tokens they hold for the app are deleted and their realtime sessions ended at
  /// once. `until` must be in the future and at most 365 days away; the player must
  /// have an access record. A replay with the same non-empty `idempotencyKey` and
  /// arguments returns the first result. Returns the access record, `suspendedUntil`
  /// set. Requires the `manage_access_tiers` app permission.
  graphql::Json suspend(std::string_view appId, std::string_view userId, std::string_view until,
                        std::string_view idempotencyKey = {}) const {
    return execUnwrap(gen::appAccess::kSuspendAppAccessDocument,
                      suspendVars(appId, userId, until, idempotencyKey));
  }
  void suspendAsync(std::string_view appId, std::string_view userId, std::string_view until,
                    std::string_view idempotencyKey, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::appAccess::kSuspendAppAccessDocument,
                    suspendVars(appId, userId, until, idempotencyKey), {}, std::move(cb));
  }
  /// Lift a player's suspension before it lapses (a no-op for a player who is not
  /// suspended). Returns the access record, `suspendedUntil` cleared. Requires
  /// `manage_access_tiers`.
  graphql::Json unsuspend(std::string_view appId, std::string_view userId) const {
    return execUnwrap(gen::appAccess::kUnsuspendAppAccessDocument,
                      two("appId", graphql::JVal(appId), "userId", graphql::JVal(userId)));
  }
  void unsuspendAsync(std::string_view appId, std::string_view userId,
                      graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::appAccess::kUnsuspendAppAccessDocument,
                    two("appId", graphql::JVal(appId), "userId", graphql::JVal(userId)), {},
                    std::move(cb));
  }
  /// Re-apply every player's tier keys that follow onto the app's world grid and
  /// rebuild the world grid's permissions. Tier changes made through this API already
  /// do it; call it after a change made another way. Requires `manage_access_tiers`.
  bool resyncTierGridPermissions(std::string_view appId) const {
    return execUnwrap(gen::appAccess::kResyncTierGridPermissionsDocument,
                      one("appId", graphql::JVal(appId)))
        .asBool();
  }
  void resyncTierGridPermissionsAsync(std::string_view appId,
                                      std::function<void(graphql::GraphQLOutcome, bool)> cb) const {
    execUnwrapAsync(gen::appAccess::kResyncTierGridPermissionsDocument,
                    one("appId", graphql::JVal(appId)), {},
                    [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
                      bool value = false;
                      if (out.ok()) value = out.data.asBool();
                      cb(std::move(out), value);
                    });
  }

  // Tier features: feature keys an access tier grants. A ck-exec hub checks a player's with the
  // node API's players.features. The fields keep the game model's names
  // (gameModelDefineFeature, ...); they are served beside access tiers. All need manage_apps.

  /// Define (or re-describe) a feature key; idempotent on (appId, featureKey).
  graphql::Json defineFeature(const graphql::JVal& input) const {
    return execUnwrap(gen::appAccess::kDefineAppFeatureDocument, one("input", input));
  }
  void defineFeatureAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::appAccess::kDefineAppFeatureDocument, one("input", input), {},
                    std::move(cb));
  }
  /// The feature keys defined for an app.
  graphql::Json features(std::string_view appId) const {
    return execUnwrap(gen::appAccess::kAppFeaturesDocument, one("appId", graphql::JVal(appId)));
  }
  void featuresAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::appAccess::kAppFeaturesDocument, one("appId", graphql::JVal(appId)), {},
                    std::move(cb));
  }
  /// Grant a feature key to an access tier.
  graphql::Json grantTierFeature(const graphql::JVal& input) const {
    return execUnwrap(gen::appAccess::kGrantTierFeatureDocument, one("input", input));
  }
  void grantTierFeatureAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::appAccess::kGrantTierFeatureDocument, one("input", input), {},
                    std::move(cb));
  }
  /// Revoke a feature key from an access tier; true when a grant was removed.
  graphql::Json revokeTierFeature(const graphql::JVal& input) const {
    return execUnwrap(gen::appAccess::kRevokeTierFeatureDocument, one("input", input));
  }
  void revokeTierFeatureAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::appAccess::kRevokeTierFeatureDocument, one("input", input), {},
                    std::move(cb));
  }
  /// Tier-to-feature grants for an app, optionally for one tier.
  graphql::Json tierFeatures(std::string_view appId, std::string_view tierId = {}) const {
    return execUnwrap(gen::appAccess::kTierFeaturesDocument, tierFeatureVars(appId, tierId));
  }
  void tierFeaturesAsync(std::string_view appId, std::string_view tierId,
                         graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::appAccess::kTierFeaturesDocument, tierFeatureVars(appId, tierId), {},
                    std::move(cb));
  }

 private:
  static graphql::JVal suspendVars(std::string_view appId, std::string_view userId,
                                   std::string_view until, std::string_view idempotencyKey) {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["userId"] = userId;
    vars["until"] = until;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    return vars;
  }
  graphql::JVal tierFeatureVars(std::string_view appId, std::string_view tierId) const {
    graphql::JVal vars = one("appId", graphql::JVal(appId));
    if (!tierId.empty()) vars["tierId"] = tierId;
    return vars;
  }
};

/// client.admin().billing() — org wallet + per-app budgets.
class BillingAPI : public detail::AdminDomain {
 public:
  using AdminDomain::AdminDomain;
  graphql::Json walletBalance(std::string_view orgId) const {
    return execUnwrap(gen::billing::kWalletBalanceDocument, one("orgId", graphql::JVal(orgId)));
  }
  void walletBalanceAsync(std::string_view orgId, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::billing::kWalletBalanceDocument, one("orgId", graphql::JVal(orgId)), {},
                    std::move(cb));
  }
  graphql::Json walletTransactions(std::string_view orgId, int limit = 50, int offset = 0) const {
    graphql::JVal vars;
    vars["orgId"] = orgId;
    vars["limit"] = std::int64_t{limit};
    vars["offset"] = std::int64_t{offset};
    return execUnwrap(gen::billing::documentFor("WalletTransactions"), vars, "WalletTransactions");
  }
  void walletTransactionsAsync(std::string_view orgId, int limit, int offset,
                               graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["orgId"] = orgId;
    vars["limit"] = std::int64_t{limit};
    vars["offset"] = std::int64_t{offset};
    execUnwrapAsync(gen::billing::documentFor("WalletTransactions"), vars, "WalletTransactions",
                    std::move(cb));
  }
  /// Relay cursor pagination variant of walletTransactions().
  graphql::Json walletTransactionsConnection(std::string_view orgId, int first = 50,
                                             std::string_view after = {}) const {
    graphql::JVal vars;
    vars["orgId"] = orgId;
    vars["first"] = std::int64_t{first};
    if (!after.empty()) vars["after"] = after;
    return execUnwrap(gen::billing::documentFor("WalletTransactionsConnection"), vars,
                      "WalletTransactionsConnection");
  }
  void walletTransactionsConnectionAsync(std::string_view orgId, int first, std::string_view after,
                                         graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["orgId"] = orgId;
    vars["first"] = std::int64_t{first};
    if (!after.empty()) vars["after"] = after;
    execUnwrapAsync(gen::billing::documentFor("WalletTransactionsConnection"), vars, "WalletTransactionsConnection",
                    std::move(cb));
  }
  graphql::Json appBudget(std::string_view orgId, std::string_view appId) const {
    return execUnwrap(gen::billing::kAppBudgetDocument,
                      two("orgId", graphql::JVal(orgId), "appId", graphql::JVal(appId)));
  }
  void appBudgetAsync(std::string_view orgId, std::string_view appId,
                      graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::billing::kAppBudgetDocument,
                    two("orgId", graphql::JVal(orgId), "appId", graphql::JVal(appId)), {},
                    std::move(cb));
  }
  graphql::Json appBudgets(std::string_view orgId) const {
    return execUnwrap(gen::billing::kAppBudgetsDocument, one("orgId", graphql::JVal(orgId)));
  }
  void appBudgetsAsync(std::string_view orgId, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::billing::kAppBudgetsDocument, one("orgId", graphql::JVal(orgId)), {},
                    std::move(cb));
  }
  /// monthlyLimitCents is a BigInt string of minor currency units.
  graphql::Json setAppBudget(std::string_view orgId, std::string_view appId,
                             std::string_view monthlyLimitCents) const {
    graphql::JVal vars;
    vars["orgId"] = orgId;
    vars["appId"] = appId;
    vars["monthlyLimitCents"] = monthlyLimitCents;
    return execUnwrap(gen::billing::kSetAppBudgetDocument, vars);
  }
  void setAppBudgetAsync(std::string_view orgId, std::string_view appId,
                         std::string_view monthlyLimitCents, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["orgId"] = orgId;
    vars["appId"] = appId;
    vars["monthlyLimitCents"] = monthlyLimitCents;
    execUnwrapAsync(gen::billing::kSetAppBudgetDocument, vars, {}, std::move(cb));
  }
  // NOTE (unified galaxy API, v13): the per-environment capacity billing-tier
  // catalogs (buddy/graphql/postgres) were retired with dedicated customer
  // environments.
};

/// client.admin().payments() — checkouts (wallet top-ups, plan purchases).
class PaymentsAPI : public detail::AdminDomain {
 public:
  using AdminDomain::AdminDomain;
  /// Economy-sensitive: pass an idempotencyKey inside the input.
  graphql::Json createCheckout(const graphql::JVal& input) const {
    return execUnwrap(gen::payments::kCreateCheckoutDocument, one("input", input));
  }
  void createCheckoutAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::payments::kCreateCheckoutDocument, one("input", input), {},
                    std::move(cb));
  }
  graphql::Json capturePaypalCheckout(std::string_view orderId,
                                      std::string_view idempotencyKey = {}) const {
    graphql::JVal vars;
    vars["orderId"] = orderId;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    return execUnwrap(gen::payments::kCapturePaypalCheckoutDocument, vars);
  }
  void capturePaypalCheckoutAsync(std::string_view orderId, std::string_view idempotencyKey,
                                  graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["orderId"] = orderId;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    execUnwrapAsync(gen::payments::kCapturePaypalCheckoutDocument, vars, {}, std::move(cb));
  }
  graphql::Json myCheckouts(int limit = 50, int offset = 0) const {
    graphql::JVal vars;
    vars["limit"] = std::int64_t{limit};
    vars["offset"] = std::int64_t{offset};
    return execUnwrap(gen::payments::documentFor("MyCheckouts"), vars, "MyCheckouts");
  }
  void myCheckoutsAsync(int limit, int offset, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["limit"] = std::int64_t{limit};
    vars["offset"] = std::int64_t{offset};
    execUnwrapAsync(gen::payments::documentFor("MyCheckouts"), vars, "MyCheckouts", std::move(cb));
  }
  graphql::Json myCheckoutsConnection(int first = 50, std::string_view after = {}) const {
    graphql::JVal vars;
    vars["first"] = std::int64_t{first};
    if (!after.empty()) vars["after"] = after;
    return execUnwrap(gen::payments::documentFor("MyCheckoutsConnection"), vars, "MyCheckoutsConnection");
  }
  void myCheckoutsConnectionAsync(int first, std::string_view after,
                                  graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["first"] = std::int64_t{first};
    if (!after.empty()) vars["after"] = after;
    execUnwrapAsync(gen::payments::documentFor("MyCheckoutsConnection"), vars, "MyCheckoutsConnection",
                    std::move(cb));
  }
};

/// client.admin().quotas() — usage quotas at org/app scope.
class QuotasAPI : public detail::AdminDomain {
 public:
  using AdminDomain::AdminDomain;
  graphql::Json forOrg(std::string_view orgId) const {
    return execUnwrap(gen::quotas::kQuotasForOrgDocument, one("orgId", graphql::JVal(orgId)));
  }
  void forOrgAsync(std::string_view orgId, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::quotas::kQuotasForOrgDocument, one("orgId", graphql::JVal(orgId)), {},
                    std::move(cb));
  }
  graphql::Json forApp(std::string_view appId) const {
    return execUnwrap(gen::quotas::kQuotasForAppDocument, one("appId", graphql::JVal(appId)));
  }
  void forAppAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::quotas::kQuotasForAppDocument, one("appId", graphql::JVal(appId)), {},
                    std::move(cb));
  }
  graphql::Json effective(const graphql::JVal& vars) const {
    return execUnwrap(gen::quotas::kEffectiveQuotaDocument, vars);
  }
  void effectiveAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::quotas::kEffectiveQuotaDocument, vars, {}, std::move(cb));
  }
  /// Refusal text of set/setAsync for an input naming neither an app nor an org.
  static constexpr const char* kUnscopedSetRefusal =
      "quotas.set needs an appId or an orgId: the SDK sets app and org quotas only";
  /// Create or update a quota at an org or app scope (`appId` or `orgId`, optionally
  /// `tierId`); requires `manage_quotas` there. The SDK does not set platform-global
  /// rules: an input naming neither an app nor an org is refused before any request,
  /// blocking as a graphql::CrowdyError with code "INVALID_ARGUMENT" (an empty result
  /// in a CROWDY_NO_EXCEPTIONS build), async as an outcome with status
  /// Errc::InvalidArgument, kind Protocol and the refusal in `errorMessage`.
  graphql::Json set(const graphql::JVal& input) const {
    if (!scoped(input)) {
#ifndef CROWDY_NO_EXCEPTIONS
      throw graphql::CrowdyError("INVALID_ARGUMENT", kUnscopedSetRefusal);
#else
      return {};
#endif
    }
    return execUnwrap(gen::quotas::kSetQuotaDocument, one("input", input));
  }
  void setAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    if (!scoped(input)) {
      graphql::GraphQLOutcome out;
      out.status = Errc::InvalidArgument;
      out.kind = graphql::GraphQLErrorKind::Protocol;
      out.errorMessage = kUnscopedSetRefusal;
      deliverAsync(std::move(out), std::move(cb));
      return;
    }
    execUnwrapAsync(gen::quotas::kSetQuotaDocument, one("input", input), {}, std::move(cb));
  }
  graphql::Json remove(std::string_view quotaId) const {
    return execUnwrap(gen::quotas::kDeleteQuotaDocument, one("quotaId", graphql::JVal(quotaId)));
  }
  void removeAsync(std::string_view quotaId, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::quotas::kDeleteQuotaDocument, one("quotaId", graphql::JVal(quotaId)), {},
                    std::move(cb));
  }

 private:
  static bool scoped(const graphql::JVal& input) {
    if (!input.isObject()) return false;
    for (const char* key : {"appId", "orgId"}) {
      auto it = input.obj().find(key);
      if (it == input.obj().end() || it->second.isNull()) continue;
      const std::string* s = it->second.asStringPtr();
      if (s == nullptr || !s->empty()) return true;
    }
    return false;
  }
};

// NOTE (unified galaxy API, v13): client.admin().environments() (dedicated
// customer environments) was removed — every app runs on the shared platform,
// and infrastructure provisioning moved to the separate infra-control-plane
// service.

/// client.admin().usage() — replication + GraphQL usage reporting.
class UsageAPI : public detail::AdminDomain {
 public:
  using AdminDomain::AdminDomain;
  graphql::Json appSummary(const graphql::JVal& vars) const {
    return execUnwrap(gen::usage::kAppUsageSummaryDocument, vars);
  }
  void appSummaryAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::usage::kAppUsageSummaryDocument, vars, {}, std::move(cb));
  }
  graphql::Json appGraphqlOperations(const graphql::JVal& vars) const {
    return execUnwrap(gen::usage::kAppGraphqlOperationsDocument, vars);
  }
  void appGraphqlOperationsAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::usage::kAppGraphqlOperationsDocument, vars, {}, std::move(cb));
  }
  // NOTE (unified galaxy API, v13): the per-environment usage rollups
  // (environmentSummary / environmentByApp / orgByEnvironment) were retired
  // with dedicated customer environments; usage is org/app-scoped.
  graphql::Json playerPulse(std::string_view orgId) const {
    return execUnwrap(gen::usage::kPlayerPulseDocument, one("orgId", graphql::JVal(orgId)));
  }
  void playerPulseAsync(std::string_view orgId, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::usage::kPlayerPulseDocument, one("orgId", graphql::JVal(orgId)), {},
                    std::move(cb));
  }
  /// Org-wide usage rollup for a window ("since" = ISO-8601 or relative like
  /// "7d"; defaults server-side to 7d).
  ///
  /// WHICH OF THESE BYTES YOU PAY FOR. The `*RecvBytes` fields are metered but
  /// are NOT counted toward Aggregate Data Volume, the monthly free-tier and
  /// overage measure -- that is egress only. The `*SendBytes` fields are the
  /// billable direction. All byte figures are wire bytes measured at the
  /// platform's network interface, including transport and network headers, and
  /// after any compression the service applies; they will not match a client's
  /// own counters (see replication::Connection::Stats).
  graphql::Json orgSummary(std::string_view orgId, std::string_view since = {}) const {
    graphql::JVal vars;
    vars["orgId"] = orgId;
    if (!since.empty()) vars["since"] = since;
    return execUnwrap(
        "query OrgUsageSummary($orgId: BigInt!, $since: DateTime) {"
        " orgUsageSummary(orgId: $orgId, since: $since) {"
        " orgId replicationSendBytes replicationRecvBytes graphqlSendBytes graphqlRecvBytes"
        " totalOps } }",
        vars);
  }
  void orgSummaryAsync(std::string_view orgId, std::string_view since,
                       graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["orgId"] = orgId;
    if (!since.empty()) vars["since"] = since;
    execUnwrapAsync(
        "query OrgUsageSummary($orgId: BigInt!, $since: DateTime) {"
        " orgUsageSummary(orgId: $orgId, since: $since) {"
        " orgId replicationSendBytes replicationRecvBytes graphqlSendBytes graphqlRecvBytes"
        " totalOps } }",
        vars, {}, std::move(cb));
  }
  /// Projected month-end egress for one shared app vs its free allowance.
  graphql::Json appProjection(std::string_view orgId, std::string_view appId) const {
    graphql::JVal vars;
    vars["orgId"] = orgId;
    vars["appId"] = appId;
    return execUnwrap(
        "query AppUsageProjection($orgId: BigInt!, $appId: BigInt!) {"
        " appUsageProjection(orgId: $orgId, appId: $appId) {"
        " appId currentEgressBytes sufficientData daysElapsed projectedBytes"
        " freeAllowanceBytes projectedPctOfFree onTrackToExceed } }",
        vars);
  }
  void appProjectionAsync(std::string_view orgId, std::string_view appId,
                          graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["orgId"] = orgId;
    vars["appId"] = appId;
    execUnwrapAsync(
        "query AppUsageProjection($orgId: BigInt!, $appId: BigInt!) {"
        " appUsageProjection(orgId: $orgId, appId: $appId) {"
        " appId currentEgressBytes sufficientData daysElapsed projectedBytes"
        " freeAllowanceBytes projectedPctOfFree onTrackToExceed } }",
        vars, {}, std::move(cb));
  }
  /// Org-level rollup of per-app usage projections.
  graphql::Json orgProjection(std::string_view orgId) const {
    graphql::JVal vars;
    vars["orgId"] = orgId;
    return execUnwrap(
        "query OrgUsageProjection($orgId: BigInt!) {"
        " orgUsageProjection(orgId: $orgId) {"
        " sufficientData daysElapsed totalProjectedBytes totalFreeAllowanceBytes"
        " apps { appId appName currentEgressBytes projectedBytes onTrackToExceed } } }",
        vars);
  }
  void orgProjectionAsync(std::string_view orgId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["orgId"] = orgId;
    execUnwrapAsync(
        "query OrgUsageProjection($orgId: BigInt!) {"
        " orgUsageProjection(orgId: $orgId) {"
        " sufficientData daysElapsed totalProjectedBytes totalFreeAllowanceBytes"
        " apps { appId appName currentEgressBytes projectedBytes onTrackToExceed } } }",
        vars, {}, std::move(cb));
  }
};

/// client.admin().sharedEnvironment() — publish-to-shared, runtime gating,
/// spend caps, auto-billing.
class SharedEnvironmentAPI : public detail::AdminDomain {
 public:
  using AdminDomain::AdminDomain;
  graphql::Json plans() const { return run("SharedEnvPlans", graphql::JVal()); }
  void plansAsync(graphql::GraphQLCallback cb) const {
    runAsync("SharedEnvPlans", graphql::JVal(), std::move(cb));
  }
  graphql::Json orgFreeAppQuota(std::string_view orgId) const {
    return run("OrgFreeAppQuota", one("orgId", graphql::JVal(orgId)));
  }
  void orgFreeAppQuotaAsync(std::string_view orgId, graphql::GraphQLCallback cb) const {
    runAsync("OrgFreeAppQuota", one("orgId", graphql::JVal(orgId)), std::move(cb));
  }
  graphql::Json appSubscription(std::string_view appId) const {
    return run("AppSharedSubscription", one("appId", graphql::JVal(appId)));
  }
  void appSubscriptionAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    runAsync("AppSharedSubscription", one("appId", graphql::JVal(appId)), std::move(cb));
  }
  graphql::Json appRuntimeState(std::string_view appId) const {
    return run("AppRuntimeState", one("appId", graphql::JVal(appId)));
  }
  void appRuntimeStateAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    runAsync("AppRuntimeState", one("appId", graphql::JVal(appId)), std::move(cb));
  }
  graphql::Json orgAutoBilling(std::string_view orgId) const {
    return run("OrgAutoBilling", one("orgId", graphql::JVal(orgId)));
  }
  void orgAutoBillingAsync(std::string_view orgId, graphql::GraphQLCallback cb) const {
    runAsync("OrgAutoBilling", one("orgId", graphql::JVal(orgId)), std::move(cb));
  }
  graphql::Json orgPaymentMethods(std::string_view orgId) const {
    return run("OrgPaymentMethods", one("orgId", graphql::JVal(orgId)));
  }
  void orgPaymentMethodsAsync(std::string_view orgId, graphql::GraphQLCallback cb) const {
    runAsync("OrgPaymentMethods", one("orgId", graphql::JVal(orgId)), std::move(cb));
  }
  graphql::Json publishApp(const graphql::JVal& input) const {
    return run("PublishAppToShared", one("input", input));
  }
  void publishAppAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    runAsync("PublishAppToShared", one("input", input), std::move(cb));
  }
  graphql::Json cancelSubscription(std::string_view appId,
                                   std::string_view idempotencyKey = {}) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    return run("CancelSharedSubscription", vars);
  }
  void cancelSubscriptionAsync(std::string_view appId, std::string_view idempotencyKey,
                               graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    runAsync("CancelSharedSubscription", vars, std::move(cb));
  }
  graphql::Json setSpendCaps(const graphql::JVal& input) const {
    return run("SetAppSpendCaps", one("input", input));
  }
  void setSpendCapsAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    runAsync("SetAppSpendCaps", one("input", input), std::move(cb));
  }
  graphql::Json setAutoBilling(const graphql::JVal& input) const {
    return run("SetAutoBilling", one("input", input));
  }
  void setAutoBillingAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    runAsync("SetAutoBilling", one("input", input), std::move(cb));
  }
  graphql::Json setupPaymentMethod(std::string_view orgId,
                                   std::string_view idempotencyKey = {}) const {
    graphql::JVal vars;
    vars["orgId"] = orgId;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    return run("SetupSharedPaymentMethod", vars);
  }
  void setupPaymentMethodAsync(std::string_view orgId, std::string_view idempotencyKey,
                               graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["orgId"] = orgId;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    runAsync("SetupSharedPaymentMethod", vars, std::move(cb));
  }
  graphql::Json removePaymentMethod(const graphql::JVal& vars) const {
    return run("RemoveSharedPaymentMethod", vars);
  }
  void removePaymentMethodAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    runAsync("RemoveSharedPaymentMethod", vars, std::move(cb));
  }
  /// ECONOMY-SENSITIVE: reserve realtime (UDP) capacity for a shared app
  /// (bytes/s, decimal: 1'000'000 = 1 MB/s; 0 clears the reservation). Pass an
  /// idempotencyKey.
  ///
  /// A reservation is a CAPACITY FLOOR, not a ceiling and not a rate-limit
  /// bypass. It obliges the platform to keep that much capacity provisioned for
  /// you by raising the fleet's autoscaling minimum; it does not cap what you may
  /// send, and traffic above the reserved rate is metered rather than refused.
  ///
  /// It buys capacity, NOT VOLUME: the monthly fee is charged in addition to
  /// metered usage and carries no data allowance. Reserving 5 MB/s does not make
  /// the first 5 MB/s free.
  ///
  /// It is NOT how the free-tier cap is lifted. Unfunded free apps are shaped at
  /// roughly 1 MB/s; funding the org wallet or enabling auto-billing lifts that.
  /// Before 2026-09-01 a reservation did double duty as the bypass, which is why
  /// this comment said "clears back to the free tier" -- clearing a reservation
  /// changes what is provisioned, not what is shaped.
  ///
  /// Sold in two independent dimensions since 2026-09-01: realtime throughput
  /// (this mutation) and GraphQL operations/sec. Reserving one does not reserve
  /// the other. Read both back from App.reservedUdpBytesPerSec and
  /// App.reservedGraphqlOpsPerSec.
  graphql::Json setAppReservedThroughput(const graphql::JVal& input,
                                         std::string_view idempotencyKey = {}) const {
    graphql::JVal vars;
    vars["input"] = input;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    return execUnwrap(
        "mutation SetAppReservedThroughput($input: SetAppReservedThroughputInput!,"
        " $idempotencyKey: String) {"
        " setAppReservedThroughput(input: $input, idempotencyKey: $idempotencyKey) {"
        " app { appId } chargedCents } }",
        vars);
  }
  void setAppReservedThroughputAsync(const graphql::JVal& input, std::string_view idempotencyKey,
                                     graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    if (!idempotencyKey.empty()) vars["idempotencyKey"] = idempotencyKey;
    execUnwrapAsync(
        "mutation SetAppReservedThroughput($input: SetAppReservedThroughputInput!,"
        " $idempotencyKey: String) {"
        " setAppReservedThroughput(input: $input, idempotencyKey: $idempotencyKey) {"
        " app { appId } chargedCents } }",
        vars, {}, std::move(cb));
  }

 private:
  graphql::Json run(std::string_view op, const graphql::JVal& vars) const {
    return execUnwrap(gen::sharedEnvironment::documentFor(op), vars, op);
  }
  void runAsync(std::string_view op, const graphql::JVal& vars,
                graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::sharedEnvironment::documentFor(op), vars, op, std::move(cb));
  }
};

/// Aggregate accessor: client.admin().
class AdminAPI {
 public:
  /// `gameApps` points at the Game-API-plane grids domain (same instance as
  /// client.gameApps()), grouped here for discoverability like CrowdyJS's
  /// admin.grids.
  explicit AdminAPI(std::shared_ptr<graphql::GraphQLClient> management,
                    GameAppsAPI* gameApps = nullptr)
      : organizations_(management),
        apps_(management),
        appAccess_(management),
        billing_(management),
        payments_(management),
        quotas_(management),
        usage_(management),
        sharedEnvironment_(management),
        grids_(gameApps) {}

  OrganizationsAPI& organizations() { return organizations_; }
  AppsAPI& apps() { return apps_; }
  AppAccessAPI& appAccess() { return appAccess_; }
  BillingAPI& billing() { return billing_; }
  PaymentsAPI& payments() { return payments_; }
  QuotasAPI& quotas() { return quotas_; }
  UsageAPI& usage() { return usage_; }
  SharedEnvironmentAPI& sharedEnvironment() { return sharedEnvironment_; }
  /// App grids + grid runtime-permission administration (same instance as
  /// client.gameApps(); requires a per-game client / app token).
  GameAppsAPI& grids() { return *grids_; }

 private:
  OrganizationsAPI organizations_;
  AppsAPI apps_;
  AppAccessAPI appAccess_;
  BillingAPI billing_;
  PaymentsAPI payments_;
  QuotasAPI quotas_;
  UsageAPI usage_;
  SharedEnvironmentAPI sharedEnvironment_;
  GameAppsAPI* grids_;
};

}  // namespace crowdy::domains
