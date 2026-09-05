// GENERATED FILE — do not edit by hand.
// Regenerate with: node scripts/codegen.mjs
// Inputs: operations/**/*.graphql and schema.gql (synced from the published
// SDL at https://docs.crowdedkingdoms.com/schema/game-api.graphql).
// schema.gql sha256: c44743c28bfc216017905e65b49cdacabc3d352102aef76dc2d3e88358d38f86
// operations sha256: aace4a3e955d5e4a1f6af1852e917079f9c09b1ede47d90a495807e9f3850a71

#pragma once

#include <string_view>

/// GraphQL operation documents, one namespace per domain. File constants are
/// retained for compatibility; operation constants contain only that operation
/// and its transitive fragments so unrelated roots cannot invalidate a request.
namespace crowdy::gen {

namespace actors {

/// actors/Actor.graphql
inline constexpr std::string_view kActorDocument = R"gql(query Actor($uuid: String!) {
  actor(uuid: $uuid) {
    uuid
    appId
    userId
    avatarId
    chunk {
      x
      y
      z
    }
    privateState
    publicState
    createdAt
  }
})gql";
inline constexpr std::string_view kActorIsolatedDocument = R"gql(query Actor($uuid: String!) {
  actor(uuid: $uuid) {
    uuid
    appId
    userId
    avatarId
    chunk {
      x
      y
      z
    }
    privateState
    publicState
    createdAt
  }
})gql";
inline constexpr std::string_view kActorOperationName = "Actor";

/// actors/Actors.graphql
inline constexpr std::string_view kActorsDocument = R"gql(query Actors($filter: ActorFilterInput) {
  actors(filter: $filter) {
    uuid
    appId
    userId
    avatarId
    chunk {
      x
      y
      z
    }
    privateState
    publicState
    createdAt
  }
}

query ActorsConnection($first: Int, $after: String, $filter: ActorFilterInput) {
  actorsConnection(first: $first, after: $after, filter: $filter) {
    edges {
      cursor
      node {
        uuid
        appId
        userId
        avatarId
        chunk {
          x
          y
          z
        }
        privateState
        publicState
        createdAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kActorsIsolatedDocument = R"gql(query Actors($filter: ActorFilterInput) {
  actors(filter: $filter) {
    uuid
    appId
    userId
    avatarId
    chunk {
      x
      y
      z
    }
    privateState
    publicState
    createdAt
  }
})gql";
inline constexpr std::string_view kActorsOperationName = "Actors";
inline constexpr std::string_view kActorsConnectionIsolatedDocument = R"gql(query ActorsConnection($first: Int, $after: String, $filter: ActorFilterInput) {
  actorsConnection(first: $first, after: $after, filter: $filter) {
    edges {
      cursor
      node {
        uuid
        appId
        userId
        avatarId
        chunk {
          x
          y
          z
        }
        privateState
        publicState
        createdAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kActorsConnectionOperationName = "ActorsConnection";

/// actors/BatchLookupActors.graphql
inline constexpr std::string_view kBatchLookupActorsDocument = R"gql(query BatchLookupActors($input: BatchActorLookupInput!) {
  batchLookupActors(input: $input) {
    uuid
    appId
    userId
    avatarId
    chunk {
      x
      y
      z
    }
    privateState
    publicState
    createdAt
  }
})gql";
inline constexpr std::string_view kBatchLookupActorsIsolatedDocument = R"gql(query BatchLookupActors($input: BatchActorLookupInput!) {
  batchLookupActors(input: $input) {
    uuid
    appId
    userId
    avatarId
    chunk {
      x
      y
      z
    }
    privateState
    publicState
    createdAt
  }
})gql";
inline constexpr std::string_view kBatchLookupActorsOperationName = "BatchLookupActors";

/// actors/CreateActor.graphql
inline constexpr std::string_view kCreateActorDocument = R"gql(mutation CreateActor($input: CreateActorInput!) {
  createActor(input: $input) {
    uuid
    appId
    userId
    avatarId
    chunk {
      x
      y
      z
    }
    privateState
    publicState
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateActorIsolatedDocument = R"gql(mutation CreateActor($input: CreateActorInput!) {
  createActor(input: $input) {
    uuid
    appId
    userId
    avatarId
    chunk {
      x
      y
      z
    }
    privateState
    publicState
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateActorOperationName = "CreateActor";

/// actors/DeleteActor.graphql
inline constexpr std::string_view kDeleteActorDocument = R"gql(mutation DeleteActor($uuid: String!, $idempotencyKey: String) {
  deleteActor(uuid: $uuid, idempotencyKey: $idempotencyKey) {
    uuid
    appId
    userId
  }
})gql";
inline constexpr std::string_view kDeleteActorIsolatedDocument = R"gql(mutation DeleteActor($uuid: String!, $idempotencyKey: String) {
  deleteActor(uuid: $uuid, idempotencyKey: $idempotencyKey) {
    uuid
    appId
    userId
  }
})gql";
inline constexpr std::string_view kDeleteActorOperationName = "DeleteActor";

/// actors/UpdateActor.graphql
inline constexpr std::string_view kUpdateActorDocument = R"gql(mutation UpdateActor($uuid: String!, $input: UpdateActorInput!) {
  updateActor(uuid: $uuid, input: $input) {
    uuid
    appId
    userId
    avatarId
    chunk {
      x
      y
      z
    }
    privateState
    publicState
    createdAt
  }
})gql";
inline constexpr std::string_view kUpdateActorIsolatedDocument = R"gql(mutation UpdateActor($uuid: String!, $input: UpdateActorInput!) {
  updateActor(uuid: $uuid, input: $input) {
    uuid
    appId
    userId
    avatarId
    chunk {
      x
      y
      z
    }
    privateState
    publicState
    createdAt
  }
})gql";
inline constexpr std::string_view kUpdateActorOperationName = "UpdateActor";

/// actors/UpdateActorState.graphql
inline constexpr std::string_view kUpdateActorStateDocument = R"gql(mutation UpdateActorState($uuid: String!, $input: UpdateActorStateInput!) {
  updateActorState(uuid: $uuid, input: $input) {
    uuid
    appId
    userId
    privateState
    publicState
  }
})gql";
inline constexpr std::string_view kUpdateActorStateIsolatedDocument = R"gql(mutation UpdateActorState($uuid: String!, $input: UpdateActorStateInput!) {
  updateActorState(uuid: $uuid, input: $input) {
    uuid
    appId
    userId
    privateState
    publicState
  }
})gql";
inline constexpr std::string_view kUpdateActorStateOperationName = "UpdateActorState";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "Actor") return kActorIsolatedDocument;
  if (operationName == "Actors") return kActorsIsolatedDocument;
  if (operationName == "ActorsConnection") return kActorsConnectionIsolatedDocument;
  if (operationName == "BatchLookupActors") return kBatchLookupActorsIsolatedDocument;
  if (operationName == "CreateActor") return kCreateActorIsolatedDocument;
  if (operationName == "DeleteActor") return kDeleteActorIsolatedDocument;
  if (operationName == "UpdateActor") return kUpdateActorIsolatedDocument;
  if (operationName == "UpdateActorState") return kUpdateActorStateIsolatedDocument;
  return {};
}

}  // namespace actors

namespace appAccess {

/// appAccess/AppAccessTiers.graphql
inline constexpr std::string_view kAppAccessTiersDocument = R"gql(query AppAccessTiers($appId: BigInt!) {
  appAccessTiers(appId: $appId) {
    tierId
    appId
    name
    tierOrder
    isFree
    isDefault
    priceCents
    currency
    billingPeriod
    description
    permissionKeys
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kAppAccessTiersIsolatedDocument = R"gql(query AppAccessTiers($appId: BigInt!) {
  appAccessTiers(appId: $appId) {
    tierId
    appId
    name
    tierOrder
    isFree
    isDefault
    priceCents
    currency
    billingPeriod
    description
    permissionKeys
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kAppAccessTiersOperationName = "AppAccessTiers";

/// appAccess/AppGrantMemberCandidates.graphql
inline constexpr std::string_view kAppGrantMemberCandidatesDocument = R"gql(query AppGrantMemberCandidates($appId: BigInt!) {
  appGrantMemberCandidates(appId: $appId) {
    userId
    email
    gamertag
  }
})gql";
inline constexpr std::string_view kAppGrantMemberCandidatesIsolatedDocument = R"gql(query AppGrantMemberCandidates($appId: BigInt!) {
  appGrantMemberCandidates(appId: $appId) {
    userId
    email
    gamertag
  }
})gql";
inline constexpr std::string_view kAppGrantMemberCandidatesOperationName = "AppGrantMemberCandidates";

/// appAccess/AppUserAccessByApp.graphql
inline constexpr std::string_view kAppUserAccessByAppDocument = R"gql(query AppUserAccessByApp(
  $appId: BigInt!
  $status: String
  $limit: Int
  $offset: Int
) {
  appUserAccessByApp(
    appId: $appId
    status: $status
    limit: $limit
    offset: $offset
  ) {
    appUserAccessId
    appId
    userId
    tierId
    status
    grantedBy
    subscriptionId
    expiresAt
    createdAt
    updatedAt
  }
}

query AppUserAccessConnection(
  $appId: BigInt!
  $first: Int
  $after: String
  $status: String
) {
  appUserAccessConnection(
    appId: $appId
    first: $first
    after: $after
    status: $status
  ) {
    edges {
      cursor
      node {
        appUserAccessId
        appId
        userId
        tierId
        status
        grantedBy
        subscriptionId
        expiresAt
        createdAt
        updatedAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kAppUserAccessByAppIsolatedDocument = R"gql(query AppUserAccessByApp($appId: BigInt!, $status: String, $limit: Int, $offset: Int) {
  appUserAccessByApp(
    appId: $appId
    status: $status
    limit: $limit
    offset: $offset
  ) {
    appUserAccessId
    appId
    userId
    tierId
    status
    grantedBy
    subscriptionId
    expiresAt
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kAppUserAccessByAppOperationName = "AppUserAccessByApp";
inline constexpr std::string_view kAppUserAccessConnectionIsolatedDocument = R"gql(query AppUserAccessConnection($appId: BigInt!, $first: Int, $after: String, $status: String) {
  appUserAccessConnection(
    appId: $appId
    first: $first
    after: $after
    status: $status
  ) {
    edges {
      cursor
      node {
        appUserAccessId
        appId
        userId
        tierId
        status
        grantedBy
        subscriptionId
        expiresAt
        createdAt
        updatedAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kAppUserAccessConnectionOperationName = "AppUserAccessConnection";

/// appAccess/ArchiveAccessTier.graphql
inline constexpr std::string_view kArchiveAccessTierDocument = R"gql(mutation ArchiveAccessTier($tierId: BigInt!) {
  archiveAccessTier(tierId: $tierId) {
    tierId
    status
    updatedAt
  }
})gql";
inline constexpr std::string_view kArchiveAccessTierIsolatedDocument = R"gql(mutation ArchiveAccessTier($tierId: BigInt!) {
  archiveAccessTier(tierId: $tierId) {
    tierId
    status
    updatedAt
  }
})gql";
inline constexpr std::string_view kArchiveAccessTierOperationName = "ArchiveAccessTier";

/// appAccess/ClaimFreeAppAccess.graphql
inline constexpr std::string_view kClaimFreeAppAccessDocument = R"gql(mutation ClaimFreeAppAccess($appId: BigInt!) {
  claimFreeAppAccess(appId: $appId) {
    appUserAccessId
    appId
    userId
    tierId
    status
    grantedBy
    subscriptionId
    expiresAt
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kClaimFreeAppAccessIsolatedDocument = R"gql(mutation ClaimFreeAppAccess($appId: BigInt!) {
  claimFreeAppAccess(appId: $appId) {
    appUserAccessId
    appId
    userId
    tierId
    status
    grantedBy
    subscriptionId
    expiresAt
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kClaimFreeAppAccessOperationName = "ClaimFreeAppAccess";

/// appAccess/CreateAccessTier.graphql
inline constexpr std::string_view kCreateAccessTierDocument = R"gql(mutation CreateAccessTier($input: CreateAccessTierInput!) {
  createAccessTier(input: $input) {
    tierId
    appId
    name
    tierOrder
    isFree
    isDefault
    priceCents
    currency
    billingPeriod
    description
    permissionKeys
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kCreateAccessTierIsolatedDocument = R"gql(mutation CreateAccessTier($input: CreateAccessTierInput!) {
  createAccessTier(input: $input) {
    tierId
    appId
    name
    tierOrder
    isFree
    isDefault
    priceCents
    currency
    billingPeriod
    description
    permissionKeys
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kCreateAccessTierOperationName = "CreateAccessTier";

/// appAccess/GrantAppAccess.graphql
inline constexpr std::string_view kGrantAppAccessDocument = R"gql(mutation GrantAppAccess($input: GrantAppAccessInput!) {
  grantAppAccess(input: $input) {
    appUserAccessId
    appId
    userId
    tierId
    status
    grantedBy
    subscriptionId
    expiresAt
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kGrantAppAccessIsolatedDocument = R"gql(mutation GrantAppAccess($input: GrantAppAccessInput!) {
  grantAppAccess(input: $input) {
    appUserAccessId
    appId
    userId
    tierId
    status
    grantedBy
    subscriptionId
    expiresAt
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kGrantAppAccessOperationName = "GrantAppAccess";

/// appAccess/GrantMyAppAccess.graphql
inline constexpr std::string_view kGrantMyAppAccessDocument = R"gql(mutation GrantMyAppAccess($appId: BigInt!) {
  grantMyAppAccess(appId: $appId) {
    appUserAccessId
    appId
    userId
    tierId
    status
    grantedBy
    subscriptionId
    expiresAt
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kGrantMyAppAccessIsolatedDocument = R"gql(mutation GrantMyAppAccess($appId: BigInt!) {
  grantMyAppAccess(appId: $appId) {
    appUserAccessId
    appId
    userId
    tierId
    status
    grantedBy
    subscriptionId
    expiresAt
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kGrantMyAppAccessOperationName = "GrantMyAppAccess";

/// appAccess/MyAppAccess.graphql
inline constexpr std::string_view kMyAppAccessDocument = R"gql(query MyAppAccess($appId: BigInt!) {
  myAppAccess(appId: $appId) {
    appUserAccessId
    appId
    userId
    tierId
    status
    grantedBy
    subscriptionId
    expiresAt
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kMyAppAccessIsolatedDocument = R"gql(query MyAppAccess($appId: BigInt!) {
  myAppAccess(appId: $appId) {
    appUserAccessId
    appId
    userId
    tierId
    status
    grantedBy
    subscriptionId
    expiresAt
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kMyAppAccessOperationName = "MyAppAccess";

/// appAccess/RevokeAppAccess.graphql
inline constexpr std::string_view kRevokeAppAccessDocument = R"gql(mutation RevokeAppAccess($appId: BigInt!, $userId: BigInt!) {
  revokeAppAccess(appId: $appId, userId: $userId) {
    appUserAccessId
    appId
    userId
    tierId
    status
    grantedBy
    subscriptionId
    expiresAt
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kRevokeAppAccessIsolatedDocument = R"gql(mutation RevokeAppAccess($appId: BigInt!, $userId: BigInt!) {
  revokeAppAccess(appId: $appId, userId: $userId) {
    appUserAccessId
    appId
    userId
    tierId
    status
    grantedBy
    subscriptionId
    expiresAt
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kRevokeAppAccessOperationName = "RevokeAppAccess";

/// appAccess/RuntimePermissions.graphql
inline constexpr std::string_view kRuntimePermissionsDocument = R"gql(query RuntimePermissions {
  runtimePermissions
})gql";
inline constexpr std::string_view kRuntimePermissionsIsolatedDocument = R"gql(query RuntimePermissions {
  runtimePermissions
})gql";
inline constexpr std::string_view kRuntimePermissionsOperationName = "RuntimePermissions";

/// appAccess/UpdateAccessTier.graphql
inline constexpr std::string_view kUpdateAccessTierDocument = R"gql(mutation UpdateAccessTier($tierId: BigInt!, $input: UpdateAccessTierInput!) {
  updateAccessTier(tierId: $tierId, input: $input) {
    tierId
    appId
    name
    tierOrder
    isFree
    isDefault
    priceCents
    currency
    billingPeriod
    description
    permissionKeys
    status
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateAccessTierIsolatedDocument = R"gql(mutation UpdateAccessTier($tierId: BigInt!, $input: UpdateAccessTierInput!) {
  updateAccessTier(tierId: $tierId, input: $input) {
    tierId
    appId
    name
    tierOrder
    isFree
    isDefault
    priceCents
    currency
    billingPeriod
    description
    permissionKeys
    status
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateAccessTierOperationName = "UpdateAccessTier";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "AppAccessTiers") return kAppAccessTiersIsolatedDocument;
  if (operationName == "AppGrantMemberCandidates") return kAppGrantMemberCandidatesIsolatedDocument;
  if (operationName == "AppUserAccessByApp") return kAppUserAccessByAppIsolatedDocument;
  if (operationName == "AppUserAccessConnection") return kAppUserAccessConnectionIsolatedDocument;
  if (operationName == "ArchiveAccessTier") return kArchiveAccessTierIsolatedDocument;
  if (operationName == "ClaimFreeAppAccess") return kClaimFreeAppAccessIsolatedDocument;
  if (operationName == "CreateAccessTier") return kCreateAccessTierIsolatedDocument;
  if (operationName == "GrantAppAccess") return kGrantAppAccessIsolatedDocument;
  if (operationName == "GrantMyAppAccess") return kGrantMyAppAccessIsolatedDocument;
  if (operationName == "MyAppAccess") return kMyAppAccessIsolatedDocument;
  if (operationName == "RevokeAppAccess") return kRevokeAppAccessIsolatedDocument;
  if (operationName == "RuntimePermissions") return kRuntimePermissionsIsolatedDocument;
  if (operationName == "UpdateAccessTier") return kUpdateAccessTierIsolatedDocument;
  return {};
}

}  // namespace appAccess

namespace apps {

/// apps/App.graphql
inline constexpr std::string_view kAppDocument = R"gql(query App($appId: BigInt!) {
  app(appId: $appId) {
    appId
    orgId
    name
    slug
    description
    visibility
    status
    metadata
    splitMode
    deploymentTarget
    reservedUdpBytesPerSec
    reservedGraphqlOpsPerSec
    runtimeStatus
    runtimeDenialReason
    gameApiUrl
    createdAt
    updatedAt
    org {
      orgId
      slug
      name
    }
  }
})gql";
inline constexpr std::string_view kAppIsolatedDocument = R"gql(query App($appId: BigInt!) {
  app(appId: $appId) {
    appId
    orgId
    name
    slug
    description
    visibility
    status
    metadata
    splitMode
    deploymentTarget
    reservedUdpBytesPerSec
    reservedGraphqlOpsPerSec
    runtimeStatus
    runtimeDenialReason
    gameApiUrl
    createdAt
    updatedAt
    org {
      orgId
      slug
      name
    }
  }
})gql";
inline constexpr std::string_view kAppOperationName = "App";

/// apps/AppBySlug.graphql
inline constexpr std::string_view kAppBySlugDocument = R"gql(query AppBySlug($orgSlug: String!, $appSlug: String!) {
  appBySlug(orgSlug: $orgSlug, appSlug: $appSlug) {
    appId
    orgId
    name
    slug
    description
    visibility
    status
    metadata
    splitMode
    gameApiUrl
    createdAt
    updatedAt
    org {
      orgId
      slug
      name
    }
  }
})gql";
inline constexpr std::string_view kAppBySlugIsolatedDocument = R"gql(query AppBySlug($orgSlug: String!, $appSlug: String!) {
  appBySlug(orgSlug: $orgSlug, appSlug: $appSlug) {
    appId
    orgId
    name
    slug
    description
    visibility
    status
    metadata
    splitMode
    gameApiUrl
    createdAt
    updatedAt
    org {
      orgId
      slug
      name
    }
  }
})gql";
inline constexpr std::string_view kAppBySlugOperationName = "AppBySlug";

/// apps/AppDiscovery.graphql
inline constexpr std::string_view kAppDiscoveryDocument = R"gql(query AppDiscovery($appIds: [BigInt!]!) {
  appDiscovery(appIds: $appIds) {
    appId
    datacenterCode
    gameApiUrl
    gameApiWsUrl
  }
})gql";
inline constexpr std::string_view kAppDiscoveryIsolatedDocument = R"gql(query AppDiscovery($appIds: [BigInt!]!) {
  appDiscovery(appIds: $appIds) {
    appId
    datacenterCode
    gameApiUrl
    gameApiWsUrl
  }
})gql";
inline constexpr std::string_view kAppDiscoveryOperationName = "AppDiscovery";

/// apps/AppsForOrg.graphql
inline constexpr std::string_view kAppsForOrgDocument = R"gql(query AppsForOrg($orgSlug: String!) {
  appsForOrg(orgSlug: $orgSlug) {
    appId
    orgId
    name
    slug
    description
    visibility
    status
    metadata
    splitMode
    gameApiUrl
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kAppsForOrgIsolatedDocument = R"gql(query AppsForOrg($orgSlug: String!) {
  appsForOrg(orgSlug: $orgSlug) {
    appId
    orgId
    name
    slug
    description
    visibility
    status
    metadata
    splitMode
    gameApiUrl
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kAppsForOrgOperationName = "AppsForOrg";

/// apps/ArchiveApp.graphql
inline constexpr std::string_view kArchiveAppDocument = R"gql(mutation ArchiveApp($appId: BigInt!) {
  archiveApp(appId: $appId) {
    appId
    status
    updatedAt
  }
})gql";
inline constexpr std::string_view kArchiveAppIsolatedDocument = R"gql(mutation ArchiveApp($appId: BigInt!) {
  archiveApp(appId: $appId) {
    appId
    status
    updatedAt
  }
})gql";
inline constexpr std::string_view kArchiveAppOperationName = "ArchiveApp";

/// apps/CodeAdmissions.graphql
inline constexpr std::string_view kCodeAdmissionsDocument = R"gql(fragment AppCodeAdmissionFields on AppCodeAdmission {
  admissionId
  appId
  subjectKind
  subjectRef
  versionRange
  admittedBy
  admittedAt
  revokedAt
}

query AppCodeAdmissionMode($appId: BigInt!) {
  appCodeAdmissionMode(appId: $appId)
}

query AppCodeAdmissions($appId: BigInt!, $includeRevoked: Boolean) {
  appCodeAdmissions(appId: $appId, includeRevoked: $includeRevoked) {
    ...AppCodeAdmissionFields
  }
}

mutation SetAppCodeAdmissionMode(
  $appId: BigInt!
  $mode: CodeAdmissionMode!
) {
  setAppCodeAdmissionMode(appId: $appId, mode: $mode)
}

mutation AdmitAppCode($input: AdmitAppCodeInput!) {
  admitAppCode(input: $input) {
    ...AppCodeAdmissionFields
  }
}

mutation RevokeAppCodeAdmission(
  $appId: BigInt!
  $admissionId: String!
) {
  revokeAppCodeAdmission(appId: $appId, admissionId: $admissionId) {
    ...AppCodeAdmissionFields
  }
})gql";
inline constexpr std::string_view kAppCodeAdmissionModeIsolatedDocument = R"gql(query AppCodeAdmissionMode($appId: BigInt!) {
  appCodeAdmissionMode(appId: $appId)
})gql";
inline constexpr std::string_view kAppCodeAdmissionModeOperationName = "AppCodeAdmissionMode";
inline constexpr std::string_view kAppCodeAdmissionsIsolatedDocument = R"gql(query AppCodeAdmissions($appId: BigInt!, $includeRevoked: Boolean) {
  appCodeAdmissions(appId: $appId, includeRevoked: $includeRevoked) {
    ...AppCodeAdmissionFields
  }
}

fragment AppCodeAdmissionFields on AppCodeAdmission {
  admissionId
  appId
  subjectKind
  subjectRef
  versionRange
  admittedBy
  admittedAt
  revokedAt
})gql";
inline constexpr std::string_view kAppCodeAdmissionsOperationName = "AppCodeAdmissions";
inline constexpr std::string_view kSetAppCodeAdmissionModeIsolatedDocument = R"gql(mutation SetAppCodeAdmissionMode($appId: BigInt!, $mode: CodeAdmissionMode!) {
  setAppCodeAdmissionMode(appId: $appId, mode: $mode)
})gql";
inline constexpr std::string_view kSetAppCodeAdmissionModeOperationName = "SetAppCodeAdmissionMode";
inline constexpr std::string_view kAdmitAppCodeIsolatedDocument = R"gql(mutation AdmitAppCode($input: AdmitAppCodeInput!) {
  admitAppCode(input: $input) {
    ...AppCodeAdmissionFields
  }
}

fragment AppCodeAdmissionFields on AppCodeAdmission {
  admissionId
  appId
  subjectKind
  subjectRef
  versionRange
  admittedBy
  admittedAt
  revokedAt
})gql";
inline constexpr std::string_view kAdmitAppCodeOperationName = "AdmitAppCode";
inline constexpr std::string_view kRevokeAppCodeAdmissionIsolatedDocument = R"gql(mutation RevokeAppCodeAdmission($appId: BigInt!, $admissionId: String!) {
  revokeAppCodeAdmission(appId: $appId, admissionId: $admissionId) {
    ...AppCodeAdmissionFields
  }
}

fragment AppCodeAdmissionFields on AppCodeAdmission {
  admissionId
  appId
  subjectKind
  subjectRef
  versionRange
  admittedBy
  admittedAt
  revokedAt
})gql";
inline constexpr std::string_view kRevokeAppCodeAdmissionOperationName = "RevokeAppCodeAdmission";

/// apps/CreateApp.graphql
inline constexpr std::string_view kCreateAppDocument = R"gql(mutation CreateApp($input: CreateAppInput!) {
  createApp(input: $input) {
    appId
    orgId
    name
    slug
    description
    visibility
    status
    metadata
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateAppIsolatedDocument = R"gql(mutation CreateApp($input: CreateAppInput!) {
  createApp(input: $input) {
    appId
    orgId
    name
    slug
    description
    visibility
    status
    metadata
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateAppOperationName = "CreateApp";

/// apps/MarketplaceApps.graphql
inline constexpr std::string_view kMarketplaceAppsDocument = R"gql(query MarketplaceApps(
  $filter: AppMarketplaceFilterInput
  $limit: Int
  $offset: Int
) {
  apps(filter: $filter, limit: $limit, offset: $offset) {
    items {
      appId
      orgId
      name
      slug
      description
      visibility
      status
      metadata
      splitMode
      gameApiUrl
      createdAt
      updatedAt
      org {
        orgId
        slug
        name
      }
    }
    pageInfo {
      totalCount
      limit
      offset
    }
  }
}

query AppsConnection(
  $first: Int
  $after: String
  $filter: AppMarketplaceFilterInput
) {
  appsConnection(first: $first, after: $after, filter: $filter) {
    edges {
      cursor
      node {
        appId
        orgId
        name
        slug
        description
        visibility
        status
        metadata
        splitMode
        gameApiUrl
        createdAt
        updatedAt
        org {
          orgId
          slug
          name
        }
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kMarketplaceAppsIsolatedDocument = R"gql(query MarketplaceApps($filter: AppMarketplaceFilterInput, $limit: Int, $offset: Int) {
  apps(filter: $filter, limit: $limit, offset: $offset) {
    items {
      appId
      orgId
      name
      slug
      description
      visibility
      status
      metadata
      splitMode
      gameApiUrl
      createdAt
      updatedAt
      org {
        orgId
        slug
        name
      }
    }
    pageInfo {
      totalCount
      limit
      offset
    }
  }
})gql";
inline constexpr std::string_view kMarketplaceAppsOperationName = "MarketplaceApps";
inline constexpr std::string_view kAppsConnectionIsolatedDocument = R"gql(query AppsConnection($first: Int, $after: String, $filter: AppMarketplaceFilterInput) {
  appsConnection(first: $first, after: $after, filter: $filter) {
    edges {
      cursor
      node {
        appId
        orgId
        name
        slug
        description
        visibility
        status
        metadata
        splitMode
        gameApiUrl
        createdAt
        updatedAt
        org {
          orgId
          slug
          name
        }
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kAppsConnectionOperationName = "AppsConnection";

/// apps/MyApps.graphql
inline constexpr std::string_view kMyAppsDocument = R"gql(query MyApps {
  myApps {
    appId
    orgId
    name
    slug
    description
    visibility
    status
    metadata
    splitMode
    gameApiUrl
    createdAt
    updatedAt
    org {
      orgId
      slug
      name
    }
  }
})gql";
inline constexpr std::string_view kMyAppsIsolatedDocument = R"gql(query MyApps {
  myApps {
    appId
    orgId
    name
    slug
    description
    visibility
    status
    metadata
    splitMode
    gameApiUrl
    createdAt
    updatedAt
    org {
      orgId
      slug
      name
    }
  }
})gql";
inline constexpr std::string_view kMyAppsOperationName = "MyApps";

/// apps/SetAppVisibility.graphql
inline constexpr std::string_view kSetAppVisibilityDocument = R"gql(mutation SetAppVisibility($appId: BigInt!, $visibility: AppVisibility!) {
  setAppVisibility(appId: $appId, visibility: $visibility) {
    appId
    visibility
    updatedAt
  }
})gql";
inline constexpr std::string_view kSetAppVisibilityIsolatedDocument = R"gql(mutation SetAppVisibility($appId: BigInt!, $visibility: AppVisibility!) {
  setAppVisibility(appId: $appId, visibility: $visibility) {
    appId
    visibility
    updatedAt
  }
})gql";
inline constexpr std::string_view kSetAppVisibilityOperationName = "SetAppVisibility";

/// apps/UpdateApp.graphql
inline constexpr std::string_view kUpdateAppDocument = R"gql(mutation UpdateApp($appId: BigInt!, $input: UpdateAppInput!) {
  updateApp(appId: $appId, input: $input) {
    appId
    orgId
    name
    slug
    description
    visibility
    status
    metadata
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateAppIsolatedDocument = R"gql(mutation UpdateApp($appId: BigInt!, $input: UpdateAppInput!) {
  updateApp(appId: $appId, input: $input) {
    appId
    orgId
    name
    slug
    description
    visibility
    status
    metadata
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateAppOperationName = "UpdateApp";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "App") return kAppIsolatedDocument;
  if (operationName == "AppBySlug") return kAppBySlugIsolatedDocument;
  if (operationName == "AppDiscovery") return kAppDiscoveryIsolatedDocument;
  if (operationName == "AppsForOrg") return kAppsForOrgIsolatedDocument;
  if (operationName == "ArchiveApp") return kArchiveAppIsolatedDocument;
  if (operationName == "AppCodeAdmissionMode") return kAppCodeAdmissionModeIsolatedDocument;
  if (operationName == "AppCodeAdmissions") return kAppCodeAdmissionsIsolatedDocument;
  if (operationName == "SetAppCodeAdmissionMode") return kSetAppCodeAdmissionModeIsolatedDocument;
  if (operationName == "AdmitAppCode") return kAdmitAppCodeIsolatedDocument;
  if (operationName == "RevokeAppCodeAdmission") return kRevokeAppCodeAdmissionIsolatedDocument;
  if (operationName == "CreateApp") return kCreateAppIsolatedDocument;
  if (operationName == "MarketplaceApps") return kMarketplaceAppsIsolatedDocument;
  if (operationName == "AppsConnection") return kAppsConnectionIsolatedDocument;
  if (operationName == "MyApps") return kMyAppsIsolatedDocument;
  if (operationName == "SetAppVisibility") return kSetAppVisibilityIsolatedDocument;
  if (operationName == "UpdateApp") return kUpdateAppIsolatedDocument;
  return {};
}

}  // namespace apps

namespace auth {

/// auth/Logout.graphql
inline constexpr std::string_view kLogoutDocument = R"gql(mutation Logout {
  logout
})gql";
inline constexpr std::string_view kLogoutIsolatedDocument = R"gql(mutation Logout {
  logout
})gql";
inline constexpr std::string_view kLogoutOperationName = "Logout";

/// auth/LogoutAllDevices.graphql
inline constexpr std::string_view kLogoutAllDevicesDocument = R"gql(mutation LogoutAllDevices {
  logoutAllDevices
})gql";
inline constexpr std::string_view kLogoutAllDevicesIsolatedDocument = R"gql(mutation LogoutAllDevices {
  logoutAllDevices
})gql";
inline constexpr std::string_view kLogoutAllDevicesOperationName = "LogoutAllDevices";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "Logout") return kLogoutIsolatedDocument;
  if (operationName == "LogoutAllDevices") return kLogoutAllDevicesIsolatedDocument;
  return {};
}

}  // namespace auth

namespace avatars {

/// avatars/Avatars.graphql
inline constexpr std::string_view kAvatarsDocument = R"gql(query UserAvatars($userId: BigInt!) {
  userAvatars(userId: $userId) {
    avatarId
    userId
    name
    publicState
    privateState
    createdAt
  }
}

query AvatarById($id: BigInt!) {
  avatar(id: $id) {
    avatarId
    userId
    name
    publicState
    privateState
    createdAt
  }
}

query MyAvatars {
  myAvatars {
    avatarId
    userId
    name
    publicState
    privateState
    createdAt
  }
}

query AvatarAppState($appId: BigInt!, $avatarId: BigInt!) {
  avatarAppState(appId: $appId, avatarId: $avatarId) {
    appId
    avatarId
    state
    createdAt
    updatedAt
  }
}

query AvatarAppStates($appId: BigInt!, $avatarIds: [BigInt!]!) {
  avatarAppStates(appId: $appId, avatarIds: $avatarIds) {
    appId
    avatarId
    state
    createdAt
    updatedAt
  }
}

mutation CreateAvatar($input: CreateAvatarInput!) {
  createAvatar(input: $input) {
    avatarId
    userId
    name
    publicState
    privateState
    createdAt
  }
}

mutation UpdateAvatar($id: BigInt!, $input: UpdateAvatarInput!) {
  updateAvatar(id: $id, input: $input) {
    avatarId
    userId
    name
    publicState
    privateState
    createdAt
  }
}

mutation DeleteAvatar($id: BigInt!, $idempotencyKey: String) {
  deleteAvatar(id: $id, idempotencyKey: $idempotencyKey) {
    avatarId
    userId
    name
    createdAt
  }
}

mutation UpdateAvatarState($id: BigInt!, $input: UpdateAvatarStateInput!) {
  updateAvatarState(id: $id, input: $input) {
    avatarId
    userId
    name
    publicState
    privateState
    createdAt
  }
}

mutation UpdateAvatarAppState($input: UpdateAvatarAppStateInput!) {
  updateAvatarAppState(input: $input) {
    appId
    avatarId
    state
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kUserAvatarsIsolatedDocument = R"gql(query UserAvatars($userId: BigInt!) {
  userAvatars(userId: $userId) {
    avatarId
    userId
    name
    publicState
    privateState
    createdAt
  }
})gql";
inline constexpr std::string_view kUserAvatarsOperationName = "UserAvatars";
inline constexpr std::string_view kAvatarByIdIsolatedDocument = R"gql(query AvatarById($id: BigInt!) {
  avatar(id: $id) {
    avatarId
    userId
    name
    publicState
    privateState
    createdAt
  }
})gql";
inline constexpr std::string_view kAvatarByIdOperationName = "AvatarById";
inline constexpr std::string_view kMyAvatarsIsolatedDocument = R"gql(query MyAvatars {
  myAvatars {
    avatarId
    userId
    name
    publicState
    privateState
    createdAt
  }
})gql";
inline constexpr std::string_view kMyAvatarsOperationName = "MyAvatars";
inline constexpr std::string_view kAvatarAppStateIsolatedDocument = R"gql(query AvatarAppState($appId: BigInt!, $avatarId: BigInt!) {
  avatarAppState(appId: $appId, avatarId: $avatarId) {
    appId
    avatarId
    state
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kAvatarAppStateOperationName = "AvatarAppState";
inline constexpr std::string_view kAvatarAppStatesIsolatedDocument = R"gql(query AvatarAppStates($appId: BigInt!, $avatarIds: [BigInt!]!) {
  avatarAppStates(appId: $appId, avatarIds: $avatarIds) {
    appId
    avatarId
    state
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kAvatarAppStatesOperationName = "AvatarAppStates";
inline constexpr std::string_view kCreateAvatarIsolatedDocument = R"gql(mutation CreateAvatar($input: CreateAvatarInput!) {
  createAvatar(input: $input) {
    avatarId
    userId
    name
    publicState
    privateState
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateAvatarOperationName = "CreateAvatar";
inline constexpr std::string_view kUpdateAvatarIsolatedDocument = R"gql(mutation UpdateAvatar($id: BigInt!, $input: UpdateAvatarInput!) {
  updateAvatar(id: $id, input: $input) {
    avatarId
    userId
    name
    publicState
    privateState
    createdAt
  }
})gql";
inline constexpr std::string_view kUpdateAvatarOperationName = "UpdateAvatar";
inline constexpr std::string_view kDeleteAvatarIsolatedDocument = R"gql(mutation DeleteAvatar($id: BigInt!, $idempotencyKey: String) {
  deleteAvatar(id: $id, idempotencyKey: $idempotencyKey) {
    avatarId
    userId
    name
    createdAt
  }
})gql";
inline constexpr std::string_view kDeleteAvatarOperationName = "DeleteAvatar";
inline constexpr std::string_view kUpdateAvatarStateIsolatedDocument = R"gql(mutation UpdateAvatarState($id: BigInt!, $input: UpdateAvatarStateInput!) {
  updateAvatarState(id: $id, input: $input) {
    avatarId
    userId
    name
    publicState
    privateState
    createdAt
  }
})gql";
inline constexpr std::string_view kUpdateAvatarStateOperationName = "UpdateAvatarState";
inline constexpr std::string_view kUpdateAvatarAppStateIsolatedDocument = R"gql(mutation UpdateAvatarAppState($input: UpdateAvatarAppStateInput!) {
  updateAvatarAppState(input: $input) {
    appId
    avatarId
    state
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateAvatarAppStateOperationName = "UpdateAvatarAppState";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "UserAvatars") return kUserAvatarsIsolatedDocument;
  if (operationName == "AvatarById") return kAvatarByIdIsolatedDocument;
  if (operationName == "MyAvatars") return kMyAvatarsIsolatedDocument;
  if (operationName == "AvatarAppState") return kAvatarAppStateIsolatedDocument;
  if (operationName == "AvatarAppStates") return kAvatarAppStatesIsolatedDocument;
  if (operationName == "CreateAvatar") return kCreateAvatarIsolatedDocument;
  if (operationName == "UpdateAvatar") return kUpdateAvatarIsolatedDocument;
  if (operationName == "DeleteAvatar") return kDeleteAvatarIsolatedDocument;
  if (operationName == "UpdateAvatarState") return kUpdateAvatarStateIsolatedDocument;
  if (operationName == "UpdateAvatarAppState") return kUpdateAvatarAppStateIsolatedDocument;
  return {};
}

}  // namespace avatars

namespace billing {

/// billing/AppBudget.graphql
inline constexpr std::string_view kAppBudgetDocument = R"gql(query AppBudget($orgId: BigInt!, $appId: BigInt!) {
  appBudget(orgId: $orgId, appId: $appId) {
    appBudgetId
    orgId
    appId
    monthlyLimitCents
    currentMonthUsageCents
    periodStart
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kAppBudgetIsolatedDocument = R"gql(query AppBudget($orgId: BigInt!, $appId: BigInt!) {
  appBudget(orgId: $orgId, appId: $appId) {
    appBudgetId
    orgId
    appId
    monthlyLimitCents
    currentMonthUsageCents
    periodStart
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kAppBudgetOperationName = "AppBudget";

/// billing/AppBudgets.graphql
inline constexpr std::string_view kAppBudgetsDocument = R"gql(query AppBudgets($orgId: BigInt!) {
  appBudgets(orgId: $orgId) {
    appBudgetId
    orgId
    appId
    monthlyLimitCents
    currentMonthUsageCents
    periodStart
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kAppBudgetsIsolatedDocument = R"gql(query AppBudgets($orgId: BigInt!) {
  appBudgets(orgId: $orgId) {
    appBudgetId
    orgId
    appId
    monthlyLimitCents
    currentMonthUsageCents
    periodStart
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kAppBudgetsOperationName = "AppBudgets";

/// billing/SetAppBudget.graphql
inline constexpr std::string_view kSetAppBudgetDocument = R"gql(mutation SetAppBudget($orgId: BigInt!, $appId: BigInt!, $monthlyLimitCents: BigInt!) {
  setAppBudget(orgId: $orgId, appId: $appId, monthlyLimitCents: $monthlyLimitCents) {
    appBudgetId
    orgId
    appId
    monthlyLimitCents
    currentMonthUsageCents
    periodStart
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kSetAppBudgetIsolatedDocument = R"gql(mutation SetAppBudget($orgId: BigInt!, $appId: BigInt!, $monthlyLimitCents: BigInt!) {
  setAppBudget(
    orgId: $orgId
    appId: $appId
    monthlyLimitCents: $monthlyLimitCents
  ) {
    appBudgetId
    orgId
    appId
    monthlyLimitCents
    currentMonthUsageCents
    periodStart
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kSetAppBudgetOperationName = "SetAppBudget";

/// billing/WalletBalance.graphql
inline constexpr std::string_view kWalletBalanceDocument = R"gql(query WalletBalance($orgId: BigInt!) {
  walletBalance(orgId: $orgId) {
    walletId
    orgId
    balanceCents
    currency
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kWalletBalanceIsolatedDocument = R"gql(query WalletBalance($orgId: BigInt!) {
  walletBalance(orgId: $orgId) {
    walletId
    orgId
    balanceCents
    currency
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kWalletBalanceOperationName = "WalletBalance";

/// billing/WalletTransactions.graphql
inline constexpr std::string_view kWalletTransactionsDocument = R"gql(query WalletTransactions($orgId: BigInt!, $limit: Int, $offset: Int) {
  walletTransactions(orgId: $orgId, limit: $limit, offset: $offset) {
    transactionId
    walletId
    orgId
    amountCents
    balanceAfter
    transactionType
    description
    referenceId
    appId
    createdAt
  }
}

query WalletTransactionsConnection(
  $orgId: BigInt!
  $first: Int
  $after: String
) {
  walletTransactionsConnection(orgId: $orgId, first: $first, after: $after) {
    edges {
      cursor
      node {
        transactionId
        walletId
        orgId
        amountCents
        balanceAfter
        transactionType
        description
        referenceId
        appId
        createdAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kWalletTransactionsIsolatedDocument = R"gql(query WalletTransactions($orgId: BigInt!, $limit: Int, $offset: Int) {
  walletTransactions(orgId: $orgId, limit: $limit, offset: $offset) {
    transactionId
    walletId
    orgId
    amountCents
    balanceAfter
    transactionType
    description
    referenceId
    appId
    createdAt
  }
})gql";
inline constexpr std::string_view kWalletTransactionsOperationName = "WalletTransactions";
inline constexpr std::string_view kWalletTransactionsConnectionIsolatedDocument = R"gql(query WalletTransactionsConnection($orgId: BigInt!, $first: Int, $after: String) {
  walletTransactionsConnection(orgId: $orgId, first: $first, after: $after) {
    edges {
      cursor
      node {
        transactionId
        walletId
        orgId
        amountCents
        balanceAfter
        transactionType
        description
        referenceId
        appId
        createdAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kWalletTransactionsConnectionOperationName = "WalletTransactionsConnection";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "AppBudget") return kAppBudgetIsolatedDocument;
  if (operationName == "AppBudgets") return kAppBudgetsIsolatedDocument;
  if (operationName == "SetAppBudget") return kSetAppBudgetIsolatedDocument;
  if (operationName == "WalletBalance") return kWalletBalanceIsolatedDocument;
  if (operationName == "WalletTransactions") return kWalletTransactionsIsolatedDocument;
  if (operationName == "WalletTransactionsConnection") return kWalletTransactionsConnectionIsolatedDocument;
  return {};
}

}  // namespace billing

namespace channels {

/// channels/AddChannelMember.graphql
inline constexpr std::string_view kAddChannelMemberDocument = R"gql(mutation AddChannelMember($groupId: BigInt!, $userId: BigInt!) {
  addChannelMember(groupId: $groupId, userId: $userId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kAddChannelMemberIsolatedDocument = R"gql(mutation AddChannelMember($groupId: BigInt!, $userId: BigInt!) {
  addChannelMember(groupId: $groupId, userId: $userId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kAddChannelMemberOperationName = "AddChannelMember";

/// channels/Channel.graphql
inline constexpr std::string_view kChannelDocument = R"gql(query Channel($groupId: BigInt!) {
  channel(groupId: $groupId) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kChannelIsolatedDocument = R"gql(query Channel($groupId: BigInt!) {
  channel(groupId: $groupId) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kChannelOperationName = "Channel";

/// channels/ChannelMembers.graphql
inline constexpr std::string_view kChannelMembersDocument = R"gql(query ChannelMembers($groupId: BigInt!) {
  channelMembers(groupId: $groupId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kChannelMembersIsolatedDocument = R"gql(query ChannelMembers($groupId: BigInt!) {
  channelMembers(groupId: $groupId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kChannelMembersOperationName = "ChannelMembers";

/// channels/ChannelPolicy.graphql
inline constexpr std::string_view kChannelPolicyDocument = R"gql(query ChannelPolicy($appId: BigInt!) {
  channelPolicy(appId: $appId) {
    appId
    groupType
    creationPolicy
    defaultMembershipPolicy
    maxMembers
    maxGroupsPerUser
  }
})gql";
inline constexpr std::string_view kChannelPolicyIsolatedDocument = R"gql(query ChannelPolicy($appId: BigInt!) {
  channelPolicy(appId: $appId) {
    appId
    groupType
    creationPolicy
    defaultMembershipPolicy
    maxMembers
    maxGroupsPerUser
  }
})gql";
inline constexpr std::string_view kChannelPolicyOperationName = "ChannelPolicy";

/// channels/ChannelRoles.graphql
inline constexpr std::string_view kChannelRolesDocument = R"gql(query ChannelRoles($groupId: BigInt!) {
  channelRoles(groupId: $groupId) {
    groupRoleId
    groupId
    roleName
    rank
    isSystem
    permissions
    createdAt
  }
})gql";
inline constexpr std::string_view kChannelRolesIsolatedDocument = R"gql(query ChannelRoles($groupId: BigInt!) {
  channelRoles(groupId: $groupId) {
    groupRoleId
    groupId
    roleName
    rank
    isSystem
    permissions
    createdAt
  }
})gql";
inline constexpr std::string_view kChannelRolesOperationName = "ChannelRoles";

/// channels/Channels.graphql
inline constexpr std::string_view kChannelsDocument = R"gql(query Channels($appId: BigInt!) {
  channels(appId: $appId) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kChannelsIsolatedDocument = R"gql(query Channels($appId: BigInt!) {
  channels(appId: $appId) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kChannelsOperationName = "Channels";

/// channels/CreateChannel.graphql
inline constexpr std::string_view kCreateChannelDocument = R"gql(mutation CreateChannel($input: CreateChannelInput!) {
  createChannel(input: $input) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateChannelIsolatedDocument = R"gql(mutation CreateChannel($input: CreateChannelInput!) {
  createChannel(input: $input) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateChannelOperationName = "CreateChannel";

/// channels/CreateChannelRole.graphql
inline constexpr std::string_view kCreateChannelRoleDocument = R"gql(mutation CreateChannelRole($input: CreateGroupRoleInput!) {
  createChannelRole(input: $input) {
    groupRoleId
    groupId
    roleName
    rank
    isSystem
    permissions
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateChannelRoleIsolatedDocument = R"gql(mutation CreateChannelRole($input: CreateGroupRoleInput!) {
  createChannelRole(input: $input) {
    groupRoleId
    groupId
    roleName
    rank
    isSystem
    permissions
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateChannelRoleOperationName = "CreateChannelRole";

/// channels/DeleteChannel.graphql
inline constexpr std::string_view kDeleteChannelDocument = R"gql(mutation DeleteChannel($groupId: BigInt!) {
  deleteChannel(groupId: $groupId)
})gql";
inline constexpr std::string_view kDeleteChannelIsolatedDocument = R"gql(mutation DeleteChannel($groupId: BigInt!) {
  deleteChannel(groupId: $groupId)
})gql";
inline constexpr std::string_view kDeleteChannelOperationName = "DeleteChannel";

/// channels/DeleteChannelRole.graphql
inline constexpr std::string_view kDeleteChannelRoleDocument = R"gql(mutation DeleteChannelRole($groupRoleId: BigInt!) {
  deleteChannelRole(groupRoleId: $groupRoleId)
})gql";
inline constexpr std::string_view kDeleteChannelRoleIsolatedDocument = R"gql(mutation DeleteChannelRole($groupRoleId: BigInt!) {
  deleteChannelRole(groupRoleId: $groupRoleId)
})gql";
inline constexpr std::string_view kDeleteChannelRoleOperationName = "DeleteChannelRole";

/// channels/JoinChannel.graphql
inline constexpr std::string_view kJoinChannelDocument = R"gql(mutation JoinChannel($groupId: BigInt!) {
  joinChannel(groupId: $groupId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kJoinChannelIsolatedDocument = R"gql(mutation JoinChannel($groupId: BigInt!) {
  joinChannel(groupId: $groupId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kJoinChannelOperationName = "JoinChannel";

/// channels/LeaveChannel.graphql
inline constexpr std::string_view kLeaveChannelDocument = R"gql(mutation LeaveChannel($groupId: BigInt!) {
  leaveChannel(groupId: $groupId)
})gql";
inline constexpr std::string_view kLeaveChannelIsolatedDocument = R"gql(mutation LeaveChannel($groupId: BigInt!) {
  leaveChannel(groupId: $groupId)
})gql";
inline constexpr std::string_view kLeaveChannelOperationName = "LeaveChannel";

/// channels/MyChannels.graphql
inline constexpr std::string_view kMyChannelsDocument = R"gql(query MyChannels($appId: BigInt!) {
  myChannels(appId: $appId) {
    group {
      groupId
      appId
      groupType
      name
      description
      ownerUserId
      membershipPolicy
      status
      defaultRoleId
      createdAt
    }
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
    permissions
    joinedAt
  }
})gql";
inline constexpr std::string_view kMyChannelsIsolatedDocument = R"gql(query MyChannels($appId: BigInt!) {
  myChannels(appId: $appId) {
    group {
      groupId
      appId
      groupType
      name
      description
      ownerUserId
      membershipPolicy
      status
      defaultRoleId
      createdAt
    }
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
    permissions
    joinedAt
  }
})gql";
inline constexpr std::string_view kMyChannelsOperationName = "MyChannels";

/// channels/RemoveChannelMember.graphql
inline constexpr std::string_view kRemoveChannelMemberDocument = R"gql(mutation RemoveChannelMember($groupId: BigInt!, $userId: BigInt!) {
  removeChannelMember(groupId: $groupId, userId: $userId)
})gql";
inline constexpr std::string_view kRemoveChannelMemberIsolatedDocument = R"gql(mutation RemoveChannelMember($groupId: BigInt!, $userId: BigInt!) {
  removeChannelMember(groupId: $groupId, userId: $userId)
})gql";
inline constexpr std::string_view kRemoveChannelMemberOperationName = "RemoveChannelMember";

/// channels/RequestToJoinChannel.graphql
inline constexpr std::string_view kRequestToJoinChannelDocument = R"gql(mutation RequestToJoinChannel($groupId: BigInt!) {
  requestToJoinChannel(groupId: $groupId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kRequestToJoinChannelIsolatedDocument = R"gql(mutation RequestToJoinChannel($groupId: BigInt!) {
  requestToJoinChannel(groupId: $groupId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kRequestToJoinChannelOperationName = "RequestToJoinChannel";

/// channels/SetChannelMemberRoles.graphql
inline constexpr std::string_view kSetChannelMemberRolesDocument = R"gql(mutation SetChannelMemberRoles($input: SetMemberRolesInput!) {
  setChannelMemberRoles(input: $input) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kSetChannelMemberRolesIsolatedDocument = R"gql(mutation SetChannelMemberRoles($input: SetMemberRolesInput!) {
  setChannelMemberRoles(input: $input) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kSetChannelMemberRolesOperationName = "SetChannelMemberRoles";

/// channels/SetChannelPolicy.graphql
inline constexpr std::string_view kSetChannelPolicyDocument = R"gql(mutation SetChannelPolicy($input: SetChannelPolicyInput!) {
  setChannelPolicy(input: $input) {
    appId
    groupType
    creationPolicy
    defaultMembershipPolicy
    maxMembers
    maxGroupsPerUser
  }
})gql";
inline constexpr std::string_view kSetChannelPolicyIsolatedDocument = R"gql(mutation SetChannelPolicy($input: SetChannelPolicyInput!) {
  setChannelPolicy(input: $input) {
    appId
    groupType
    creationPolicy
    defaultMembershipPolicy
    maxMembers
    maxGroupsPerUser
  }
})gql";
inline constexpr std::string_view kSetChannelPolicyOperationName = "SetChannelPolicy";

/// channels/UpdateChannel.graphql
inline constexpr std::string_view kUpdateChannelDocument = R"gql(mutation UpdateChannel($input: UpdateChannelInput!) {
  updateChannel(input: $input) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kUpdateChannelIsolatedDocument = R"gql(mutation UpdateChannel($input: UpdateChannelInput!) {
  updateChannel(input: $input) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kUpdateChannelOperationName = "UpdateChannel";

/// channels/UpdateChannelRole.graphql
inline constexpr std::string_view kUpdateChannelRoleDocument = R"gql(mutation UpdateChannelRole($input: UpdateGroupRoleInput!) {
  updateChannelRole(input: $input) {
    groupRoleId
    groupId
    roleName
    rank
    isSystem
    permissions
    createdAt
  }
})gql";
inline constexpr std::string_view kUpdateChannelRoleIsolatedDocument = R"gql(mutation UpdateChannelRole($input: UpdateGroupRoleInput!) {
  updateChannelRole(input: $input) {
    groupRoleId
    groupId
    roleName
    rank
    isSystem
    permissions
    createdAt
  }
})gql";
inline constexpr std::string_view kUpdateChannelRoleOperationName = "UpdateChannelRole";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "AddChannelMember") return kAddChannelMemberIsolatedDocument;
  if (operationName == "Channel") return kChannelIsolatedDocument;
  if (operationName == "ChannelMembers") return kChannelMembersIsolatedDocument;
  if (operationName == "ChannelPolicy") return kChannelPolicyIsolatedDocument;
  if (operationName == "ChannelRoles") return kChannelRolesIsolatedDocument;
  if (operationName == "Channels") return kChannelsIsolatedDocument;
  if (operationName == "CreateChannel") return kCreateChannelIsolatedDocument;
  if (operationName == "CreateChannelRole") return kCreateChannelRoleIsolatedDocument;
  if (operationName == "DeleteChannel") return kDeleteChannelIsolatedDocument;
  if (operationName == "DeleteChannelRole") return kDeleteChannelRoleIsolatedDocument;
  if (operationName == "JoinChannel") return kJoinChannelIsolatedDocument;
  if (operationName == "LeaveChannel") return kLeaveChannelIsolatedDocument;
  if (operationName == "MyChannels") return kMyChannelsIsolatedDocument;
  if (operationName == "RemoveChannelMember") return kRemoveChannelMemberIsolatedDocument;
  if (operationName == "RequestToJoinChannel") return kRequestToJoinChannelIsolatedDocument;
  if (operationName == "SetChannelMemberRoles") return kSetChannelMemberRolesIsolatedDocument;
  if (operationName == "SetChannelPolicy") return kSetChannelPolicyIsolatedDocument;
  if (operationName == "UpdateChannel") return kUpdateChannelIsolatedDocument;
  if (operationName == "UpdateChannelRole") return kUpdateChannelRoleIsolatedDocument;
  return {};
}

}  // namespace channels

namespace chunks {

/// chunks/GetChunk.graphql
inline constexpr std::string_view kGetChunkDocument = R"gql(query GetChunk($input: GetChunkInput!) {
  getChunk(input: $input) {
    chunkId
    appId
    coordinates {
      x
      y
      z
    }
    voxels
    voxelStates {
      voxelCoord {
        x
        y
        z
      }
      voxelType
      state
    }
    owner
    createdAt
    updatedAt
    chunkState
    cdnUploadedAt
    lods {
      level
      data
    }
  }
})gql";
inline constexpr std::string_view kGetChunkIsolatedDocument = R"gql(query GetChunk($input: GetChunkInput!) {
  getChunk(input: $input) {
    chunkId
    appId
    coordinates {
      x
      y
      z
    }
    voxels
    voxelStates {
      voxelCoord {
        x
        y
        z
      }
      voxelType
      state
    }
    owner
    createdAt
    updatedAt
    chunkState
    cdnUploadedAt
    lods {
      level
      data
    }
  }
})gql";
inline constexpr std::string_view kGetChunkOperationName = "GetChunk";

/// chunks/GetChunkLods.graphql
inline constexpr std::string_view kGetChunkLodsDocument = R"gql(query GetChunkLods($input: GetChunkLodsInput!) {
  getChunkLods(input: $input) {
    chunkId
    appId
    coordinates {
      x
      y
      z
    }
    lods {
      level
      data
    }
    updatedAt
  }
})gql";
inline constexpr std::string_view kGetChunkLodsIsolatedDocument = R"gql(query GetChunkLods($input: GetChunkLodsInput!) {
  getChunkLods(input: $input) {
    chunkId
    appId
    coordinates {
      x
      y
      z
    }
    lods {
      level
      data
    }
    updatedAt
  }
})gql";
inline constexpr std::string_view kGetChunkLodsOperationName = "GetChunkLods";

/// chunks/GetChunksByDistance.graphql
inline constexpr std::string_view kGetChunksByDistanceDocument = R"gql(query GetChunksByDistance($input: GetChunksByDistanceInput!) {
  getChunksByDistance(input: $input) {
    limit
    skip
    chunks {
      chunkId
      appId
      coordinates {
        x
        y
        z
      }
      voxels
      owner
      createdAt
      updatedAt
      chunkState
      cdnUploadedAt
      lods {
        level
        data
      }
    }
  }
})gql";
inline constexpr std::string_view kGetChunksByDistanceIsolatedDocument = R"gql(query GetChunksByDistance($input: GetChunksByDistanceInput!) {
  getChunksByDistance(input: $input) {
    limit
    skip
    chunks {
      chunkId
      appId
      coordinates {
        x
        y
        z
      }
      voxels
      owner
      createdAt
      updatedAt
      chunkState
      cdnUploadedAt
      lods {
        level
        data
      }
    }
  }
})gql";
inline constexpr std::string_view kGetChunksByDistanceOperationName = "GetChunksByDistance";

/// chunks/GetVoxelList.graphql
inline constexpr std::string_view kGetVoxelListDocument = R"gql(query GetVoxelList($input: GetVoxelListInput!) {
  getVoxelList(input: $input) {
    coordinates {
      x
      y
      z
    }
    voxels {
      voxelUpdateId
      appId
      coordinates {
        x
        y
        z
      }
      location {
        x
        y
        z
      }
      voxelType
      state
      createdBy
      createdAt
    }
  }
})gql";
inline constexpr std::string_view kGetVoxelListIsolatedDocument = R"gql(query GetVoxelList($input: GetVoxelListInput!) {
  getVoxelList(input: $input) {
    coordinates {
      x
      y
      z
    }
    voxels {
      voxelUpdateId
      appId
      coordinates {
        x
        y
        z
      }
      location {
        x
        y
        z
      }
      voxelType
      state
      createdBy
      createdAt
    }
  }
})gql";
inline constexpr std::string_view kGetVoxelListOperationName = "GetVoxelList";

/// chunks/UpdateChunk.graphql
inline constexpr std::string_view kUpdateChunkDocument = R"gql(mutation UpdateChunk($input: ChunkUpdateInput!) {
  updateChunk(input: $input) {
    chunkId
    appId
    coordinates {
      x
      y
      z
    }
    voxels
    chunkState
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateChunkIsolatedDocument = R"gql(mutation UpdateChunk($input: ChunkUpdateInput!) {
  updateChunk(input: $input) {
    chunkId
    appId
    coordinates {
      x
      y
      z
    }
    voxels
    chunkState
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateChunkOperationName = "UpdateChunk";

/// chunks/UpdateChunkLods.graphql
inline constexpr std::string_view kUpdateChunkLodsDocument = R"gql(mutation UpdateChunkLods($input: UpdateChunkLodsInput!) {
  updateChunkLods(input: $input) {
    chunkId
    appId
    coordinates {
      x
      y
      z
    }
    lods {
      level
      data
    }
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateChunkLodsIsolatedDocument = R"gql(mutation UpdateChunkLods($input: UpdateChunkLodsInput!) {
  updateChunkLods(input: $input) {
    chunkId
    appId
    coordinates {
      x
      y
      z
    }
    lods {
      level
      data
    }
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateChunkLodsOperationName = "UpdateChunkLods";

/// chunks/UpdateChunkState.graphql
inline constexpr std::string_view kUpdateChunkStateDocument = R"gql(mutation UpdateChunkState($input: UpdateChunkStateInput!) {
  updateChunkState(input: $input) {
    chunkId
    appId
    coordinates {
      x
      y
      z
    }
    chunkState
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateChunkStateIsolatedDocument = R"gql(mutation UpdateChunkState($input: UpdateChunkStateInput!) {
  updateChunkState(input: $input) {
    chunkId
    appId
    coordinates {
      x
      y
      z
    }
    chunkState
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateChunkStateOperationName = "UpdateChunkState";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "GetChunk") return kGetChunkIsolatedDocument;
  if (operationName == "GetChunkLods") return kGetChunkLodsIsolatedDocument;
  if (operationName == "GetChunksByDistance") return kGetChunksByDistanceIsolatedDocument;
  if (operationName == "GetVoxelList") return kGetVoxelListIsolatedDocument;
  if (operationName == "UpdateChunk") return kUpdateChunkIsolatedDocument;
  if (operationName == "UpdateChunkLods") return kUpdateChunkLodsIsolatedDocument;
  if (operationName == "UpdateChunkState") return kUpdateChunkStateIsolatedDocument;
  return {};
}

}  // namespace chunks

namespace compute {

/// compute/AppComputeBudget.graphql
inline constexpr std::string_view kAppComputeBudgetDocument = R"gql(fragment EngineComputeUnitsFields on EngineComputeUnits {
  engine
  units
}

fragment AppComputeBudgetInfoFields on AppComputeBudgetInfo {
  appId
  unitsPerMinute
  enforce
  note
  updatedAt
}

query AppComputeUsage($appId: BigInt!, $windowMinutes: Int) {
  appComputeUsage(appId: $appId, windowMinutes: $windowMinutes) {
    windowMinutes
    totalUnits
    peakMinuteUnits
    meanUnitsPerMinute
    byEngine {
      ...EngineComputeUnitsFields
    }
  }
}

query AppComputeBudgetStatus($appId: BigInt!) {
  appComputeBudgetStatus(appId: $appId) {
    appId
    unitsUsed
    allowance
    allowanceIsAppOwn
    overBudget
    enforced
    retryAfterMs
    byEngine {
      ...EngineComputeUnitsFields
    }
  }
}

query AppComputeBudget($appId: BigInt!) {
  appComputeBudget(appId: $appId) {
    ...AppComputeBudgetInfoFields
  }
}

mutation SetAppComputeBudget(
  $appId: BigInt!
  $unitsPerMinute: Int!
  $enforce: Boolean
  $note: String
) {
  setAppComputeBudget(
    appId: $appId
    unitsPerMinute: $unitsPerMinute
    enforce: $enforce
    note: $note
  ) {
    ...AppComputeBudgetInfoFields
  }
}

mutation ClearAppComputeBudget($appId: BigInt!) {
  clearAppComputeBudget(appId: $appId)
})gql";
inline constexpr std::string_view kAppComputeUsageIsolatedDocument = R"gql(query AppComputeUsage($appId: BigInt!, $windowMinutes: Int) {
  appComputeUsage(appId: $appId, windowMinutes: $windowMinutes) {
    windowMinutes
    totalUnits
    peakMinuteUnits
    meanUnitsPerMinute
    byEngine {
      ...EngineComputeUnitsFields
    }
  }
}

fragment EngineComputeUnitsFields on EngineComputeUnits {
  engine
  units
})gql";
inline constexpr std::string_view kAppComputeUsageOperationName = "AppComputeUsage";
inline constexpr std::string_view kAppComputeBudgetStatusIsolatedDocument = R"gql(query AppComputeBudgetStatus($appId: BigInt!) {
  appComputeBudgetStatus(appId: $appId) {
    appId
    unitsUsed
    allowance
    allowanceIsAppOwn
    overBudget
    enforced
    retryAfterMs
    byEngine {
      ...EngineComputeUnitsFields
    }
  }
}

fragment EngineComputeUnitsFields on EngineComputeUnits {
  engine
  units
})gql";
inline constexpr std::string_view kAppComputeBudgetStatusOperationName = "AppComputeBudgetStatus";
inline constexpr std::string_view kAppComputeBudgetIsolatedDocument = R"gql(query AppComputeBudget($appId: BigInt!) {
  appComputeBudget(appId: $appId) {
    ...AppComputeBudgetInfoFields
  }
}

fragment AppComputeBudgetInfoFields on AppComputeBudgetInfo {
  appId
  unitsPerMinute
  enforce
  note
  updatedAt
})gql";
inline constexpr std::string_view kAppComputeBudgetOperationName = "AppComputeBudget";
inline constexpr std::string_view kSetAppComputeBudgetIsolatedDocument = R"gql(mutation SetAppComputeBudget($appId: BigInt!, $unitsPerMinute: Int!, $enforce: Boolean, $note: String) {
  setAppComputeBudget(
    appId: $appId
    unitsPerMinute: $unitsPerMinute
    enforce: $enforce
    note: $note
  ) {
    ...AppComputeBudgetInfoFields
  }
}

fragment AppComputeBudgetInfoFields on AppComputeBudgetInfo {
  appId
  unitsPerMinute
  enforce
  note
  updatedAt
})gql";
inline constexpr std::string_view kSetAppComputeBudgetOperationName = "SetAppComputeBudget";
inline constexpr std::string_view kClearAppComputeBudgetIsolatedDocument = R"gql(mutation ClearAppComputeBudget($appId: BigInt!) {
  clearAppComputeBudget(appId: $appId)
})gql";
inline constexpr std::string_view kClearAppComputeBudgetOperationName = "ClearAppComputeBudget";

/// compute/ComputeModules.graphql
inline constexpr std::string_view kComputeModulesDocument = R"gql(fragment ComputeModuleFields on WasmModule {
  moduleId
  appId
  name
  description
  enabled
  # `alwaysOn` was selected here and is gone (2026-09-01). Deprecated
  # server-side and now always false: a module runs only while its app has a
  # player present. Selecting it would hand every caller a meaningless value.
  currentVersionId
  circuitState
  consecutiveFailures
  cooldownUntil
  breakerLatchedAt
  breakerReason
  lastError
  createdAt
  updatedAt
}

fragment ComputeVersionFields on WasmModuleVersion {
  versionId
  moduleId
  appId
  versionNo
  sourceHash
  sdkVersion
  abiVersion
  compileStatus
  compileLog
  compiledSizeBytes
  publishedAt
  createdAt
}

fragment ComputeTriggerFields on WasmModuleTrigger {
  triggerId
  appId
  moduleId
  triggerType
  tickHz
  onEvent
  functionName
  containerTypeName
  propertyKey
  eventName
  debounceMs
  exportName
  invokePolicyJson
  contractJson
  createdAt
}

fragment ComputePolicyFields on WasmModulePolicy {
  appId
  enabled
  maxModules
  maxTickHz
  fuelPerTick
  fuelPerInvoke
  maxMemoryMb
  maxRunMs
  maxDbOpsPerTick
  maxEgressMsgsPerMin
  maxEgressBytesPerMin
  failureThreshold
  cooldownMs
  statePersistMinIntervalMs
  maxStateWritesPerMin
  maxStateBytesPerMin
}

fragment ComputeRunFields on WasmModuleRun {
  runId
  appId
  flowId
  moduleId
  moduleName
  triggerSource
  entry
  startedAt
  durationUs
  fuelUsed
  dbReads
  dbWrites
  egressMsgs
  egressBytes
  success
  errorMessage
  circuitAction
}

mutation ComputeUpsertModule($input: UpsertComputeModuleInput!) {
  computeUpsertModule(input: $input) {
    ...ComputeModuleFields
  }
}

mutation ComputeDeployVersion($input: DeployComputeVersionInput!) {
  computeDeployVersion(input: $input) {
    ...ComputeVersionFields
  }
}

mutation ComputeSetModuleEnabled($appId: BigInt!, $name: String!, $enabled: Boolean!) {
  computeSetModuleEnabled(appId: $appId, name: $name, enabled: $enabled) {
    ...ComputeModuleFields
  }
}

mutation ComputeResetBreaker($appId: BigInt!, $name: String!, $reason: String!) {
  computeResetBreaker(appId: $appId, name: $name, reason: $reason) {
    ...ComputeModuleFields
  }
}

mutation ComputeDeleteModule($appId: BigInt!, $name: String!) {
  computeDeleteModule(appId: $appId, name: $name)
}

mutation ComputeUpsertTrigger($input: UpsertComputeTriggerInput!) {
  computeUpsertTrigger(input: $input) {
    ...ComputeTriggerFields
  }
}

mutation ComputeDeleteTrigger($appId: BigInt!, $triggerId: String!) {
  computeDeleteTrigger(appId: $appId, triggerId: $triggerId)
}

mutation ComputeSetPolicy($input: SetComputePolicyInput!) {
  computeSetPolicy(input: $input) {
    ...ComputePolicyFields
  }
}

mutation ComputeInvoke(
  $appId: BigInt!
  $moduleName: String!
  $exportName: String!
  $paramsJson: String
) {
  computeInvoke(
    appId: $appId
    moduleName: $moduleName
    exportName: $exportName
    paramsJson: $paramsJson
  ) {
    resultBase64
    resultJson
    fuelUsed
    durationUs
  }
}

query ComputeModules($appId: BigInt!) {
  computeModules(appId: $appId) {
    ...ComputeModuleFields
  }
}

query ComputeModule($appId: BigInt!, $name: String!) {
  computeModule(appId: $appId, name: $name) {
    ...ComputeModuleFields
  }
}

query ComputeModuleVersions($appId: BigInt!, $moduleName: String!, $limit: Int) {
  computeModuleVersions(appId: $appId, moduleName: $moduleName, limit: $limit) {
    ...ComputeVersionFields
  }
}

query ComputeModuleTriggers($appId: BigInt!, $moduleName: String) {
  computeModuleTriggers(appId: $appId, moduleName: $moduleName) {
    ...ComputeTriggerFields
  }
}

query ComputeModulePolicy($appId: BigInt!) {
  computeModulePolicy(appId: $appId) {
    ...ComputePolicyFields
  }
}

query ComputeModuleRuns(
  $appId: BigInt!
  $moduleName: String
  $success: Boolean
  $limit: Int
  $offset: Int
) {
  computeModuleRuns(
    appId: $appId
    moduleName: $moduleName
    success: $success
    limit: $limit
    offset: $offset
  ) {
    ...ComputeRunFields
  }
}

query ComputeModuleStats($appId: BigInt!, $windowMinutes: Int) {
  computeModuleStats(appId: $appId, windowMinutes: $windowMinutes) {
    windowMinutes
    totalRuns
    failedRuns
    failureRatePct
    totalFuelUsed
    totalEgressMsgs
    avgDurationUs
    byModule {
      moduleName
      runs
      failures
      fuelUsed
      avgDurationUs
      circuitState
    }
  }
}

query ComputeModuleLogs($appId: BigInt!, $moduleName: String, $limit: Int) {
  computeModuleLogs(appId: $appId, moduleName: $moduleName, limit: $limit) {
    ts
    moduleName
    level
    message
    triggerSource
  }
}

query ComputeAppDiagnostics($appId: BigInt!) {
  computeAppDiagnostics(appId: $appId) {
    appId
    moduleCount
    enabledModuleCount
    versionCount
    triggerCount
    runs24h
    failedRuns24h
    fuelUsed24h
    topModules {
      moduleName
      runs
      failures
    }
    toolchainRustVersion
    toolchainWasmOptVersion
  }
}

query ComputeTemplates($appId: BigInt!) {
  computeTemplates(appId: $appId) {
    name
    description
    exports
  }
}

mutation ComputeDeployTemplate($appId: BigInt!, $templateName: String!, $moduleName: String) {
  computeDeployTemplate(appId: $appId, templateName: $templateName, moduleName: $moduleName) {
    ...ComputeModuleFields
  }
})gql";
inline constexpr std::string_view kComputeUpsertModuleIsolatedDocument = R"gql(mutation ComputeUpsertModule($input: UpsertComputeModuleInput!) {
  computeUpsertModule(input: $input) {
    ...ComputeModuleFields
  }
}

fragment ComputeModuleFields on WasmModule {
  moduleId
  appId
  name
  description
  enabled
  currentVersionId
  circuitState
  consecutiveFailures
  cooldownUntil
  breakerLatchedAt
  breakerReason
  lastError
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kComputeUpsertModuleOperationName = "ComputeUpsertModule";
inline constexpr std::string_view kComputeDeployVersionIsolatedDocument = R"gql(mutation ComputeDeployVersion($input: DeployComputeVersionInput!) {
  computeDeployVersion(input: $input) {
    ...ComputeVersionFields
  }
}

fragment ComputeVersionFields on WasmModuleVersion {
  versionId
  moduleId
  appId
  versionNo
  sourceHash
  sdkVersion
  abiVersion
  compileStatus
  compileLog
  compiledSizeBytes
  publishedAt
  createdAt
})gql";
inline constexpr std::string_view kComputeDeployVersionOperationName = "ComputeDeployVersion";
inline constexpr std::string_view kComputeSetModuleEnabledIsolatedDocument = R"gql(mutation ComputeSetModuleEnabled($appId: BigInt!, $name: String!, $enabled: Boolean!) {
  computeSetModuleEnabled(appId: $appId, name: $name, enabled: $enabled) {
    ...ComputeModuleFields
  }
}

fragment ComputeModuleFields on WasmModule {
  moduleId
  appId
  name
  description
  enabled
  currentVersionId
  circuitState
  consecutiveFailures
  cooldownUntil
  breakerLatchedAt
  breakerReason
  lastError
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kComputeSetModuleEnabledOperationName = "ComputeSetModuleEnabled";
inline constexpr std::string_view kComputeResetBreakerIsolatedDocument = R"gql(mutation ComputeResetBreaker($appId: BigInt!, $name: String!, $reason: String!) {
  computeResetBreaker(appId: $appId, name: $name, reason: $reason) {
    ...ComputeModuleFields
  }
}

fragment ComputeModuleFields on WasmModule {
  moduleId
  appId
  name
  description
  enabled
  currentVersionId
  circuitState
  consecutiveFailures
  cooldownUntil
  breakerLatchedAt
  breakerReason
  lastError
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kComputeResetBreakerOperationName = "ComputeResetBreaker";
inline constexpr std::string_view kComputeDeleteModuleIsolatedDocument = R"gql(mutation ComputeDeleteModule($appId: BigInt!, $name: String!) {
  computeDeleteModule(appId: $appId, name: $name)
})gql";
inline constexpr std::string_view kComputeDeleteModuleOperationName = "ComputeDeleteModule";
inline constexpr std::string_view kComputeUpsertTriggerIsolatedDocument = R"gql(mutation ComputeUpsertTrigger($input: UpsertComputeTriggerInput!) {
  computeUpsertTrigger(input: $input) {
    ...ComputeTriggerFields
  }
}

fragment ComputeTriggerFields on WasmModuleTrigger {
  triggerId
  appId
  moduleId
  triggerType
  tickHz
  onEvent
  functionName
  containerTypeName
  propertyKey
  eventName
  debounceMs
  exportName
  invokePolicyJson
  contractJson
  createdAt
})gql";
inline constexpr std::string_view kComputeUpsertTriggerOperationName = "ComputeUpsertTrigger";
inline constexpr std::string_view kComputeDeleteTriggerIsolatedDocument = R"gql(mutation ComputeDeleteTrigger($appId: BigInt!, $triggerId: String!) {
  computeDeleteTrigger(appId: $appId, triggerId: $triggerId)
})gql";
inline constexpr std::string_view kComputeDeleteTriggerOperationName = "ComputeDeleteTrigger";
inline constexpr std::string_view kComputeSetPolicyIsolatedDocument = R"gql(mutation ComputeSetPolicy($input: SetComputePolicyInput!) {
  computeSetPolicy(input: $input) {
    ...ComputePolicyFields
  }
}

fragment ComputePolicyFields on WasmModulePolicy {
  appId
  enabled
  maxModules
  maxTickHz
  fuelPerTick
  fuelPerInvoke
  maxMemoryMb
  maxRunMs
  maxDbOpsPerTick
  maxEgressMsgsPerMin
  maxEgressBytesPerMin
  failureThreshold
  cooldownMs
  statePersistMinIntervalMs
  maxStateWritesPerMin
  maxStateBytesPerMin
})gql";
inline constexpr std::string_view kComputeSetPolicyOperationName = "ComputeSetPolicy";
inline constexpr std::string_view kComputeInvokeIsolatedDocument = R"gql(mutation ComputeInvoke($appId: BigInt!, $moduleName: String!, $exportName: String!, $paramsJson: String) {
  computeInvoke(
    appId: $appId
    moduleName: $moduleName
    exportName: $exportName
    paramsJson: $paramsJson
  ) {
    resultBase64
    resultJson
    fuelUsed
    durationUs
  }
})gql";
inline constexpr std::string_view kComputeInvokeOperationName = "ComputeInvoke";
inline constexpr std::string_view kComputeModulesIsolatedDocument = R"gql(query ComputeModules($appId: BigInt!) {
  computeModules(appId: $appId) {
    ...ComputeModuleFields
  }
}

fragment ComputeModuleFields on WasmModule {
  moduleId
  appId
  name
  description
  enabled
  currentVersionId
  circuitState
  consecutiveFailures
  cooldownUntil
  breakerLatchedAt
  breakerReason
  lastError
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kComputeModulesOperationName = "ComputeModules";
inline constexpr std::string_view kComputeModuleIsolatedDocument = R"gql(query ComputeModule($appId: BigInt!, $name: String!) {
  computeModule(appId: $appId, name: $name) {
    ...ComputeModuleFields
  }
}

fragment ComputeModuleFields on WasmModule {
  moduleId
  appId
  name
  description
  enabled
  currentVersionId
  circuitState
  consecutiveFailures
  cooldownUntil
  breakerLatchedAt
  breakerReason
  lastError
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kComputeModuleOperationName = "ComputeModule";
inline constexpr std::string_view kComputeModuleVersionsIsolatedDocument = R"gql(query ComputeModuleVersions($appId: BigInt!, $moduleName: String!, $limit: Int) {
  computeModuleVersions(appId: $appId, moduleName: $moduleName, limit: $limit) {
    ...ComputeVersionFields
  }
}

fragment ComputeVersionFields on WasmModuleVersion {
  versionId
  moduleId
  appId
  versionNo
  sourceHash
  sdkVersion
  abiVersion
  compileStatus
  compileLog
  compiledSizeBytes
  publishedAt
  createdAt
})gql";
inline constexpr std::string_view kComputeModuleVersionsOperationName = "ComputeModuleVersions";
inline constexpr std::string_view kComputeModuleTriggersIsolatedDocument = R"gql(query ComputeModuleTriggers($appId: BigInt!, $moduleName: String) {
  computeModuleTriggers(appId: $appId, moduleName: $moduleName) {
    ...ComputeTriggerFields
  }
}

fragment ComputeTriggerFields on WasmModuleTrigger {
  triggerId
  appId
  moduleId
  triggerType
  tickHz
  onEvent
  functionName
  containerTypeName
  propertyKey
  eventName
  debounceMs
  exportName
  invokePolicyJson
  contractJson
  createdAt
})gql";
inline constexpr std::string_view kComputeModuleTriggersOperationName = "ComputeModuleTriggers";
inline constexpr std::string_view kComputeModulePolicyIsolatedDocument = R"gql(query ComputeModulePolicy($appId: BigInt!) {
  computeModulePolicy(appId: $appId) {
    ...ComputePolicyFields
  }
}

fragment ComputePolicyFields on WasmModulePolicy {
  appId
  enabled
  maxModules
  maxTickHz
  fuelPerTick
  fuelPerInvoke
  maxMemoryMb
  maxRunMs
  maxDbOpsPerTick
  maxEgressMsgsPerMin
  maxEgressBytesPerMin
  failureThreshold
  cooldownMs
  statePersistMinIntervalMs
  maxStateWritesPerMin
  maxStateBytesPerMin
})gql";
inline constexpr std::string_view kComputeModulePolicyOperationName = "ComputeModulePolicy";
inline constexpr std::string_view kComputeModuleRunsIsolatedDocument = R"gql(query ComputeModuleRuns($appId: BigInt!, $moduleName: String, $success: Boolean, $limit: Int, $offset: Int) {
  computeModuleRuns(
    appId: $appId
    moduleName: $moduleName
    success: $success
    limit: $limit
    offset: $offset
  ) {
    ...ComputeRunFields
  }
}

fragment ComputeRunFields on WasmModuleRun {
  runId
  appId
  flowId
  moduleId
  moduleName
  triggerSource
  entry
  startedAt
  durationUs
  fuelUsed
  dbReads
  dbWrites
  egressMsgs
  egressBytes
  success
  errorMessage
  circuitAction
})gql";
inline constexpr std::string_view kComputeModuleRunsOperationName = "ComputeModuleRuns";
inline constexpr std::string_view kComputeModuleStatsIsolatedDocument = R"gql(query ComputeModuleStats($appId: BigInt!, $windowMinutes: Int) {
  computeModuleStats(appId: $appId, windowMinutes: $windowMinutes) {
    windowMinutes
    totalRuns
    failedRuns
    failureRatePct
    totalFuelUsed
    totalEgressMsgs
    avgDurationUs
    byModule {
      moduleName
      runs
      failures
      fuelUsed
      avgDurationUs
      circuitState
    }
  }
})gql";
inline constexpr std::string_view kComputeModuleStatsOperationName = "ComputeModuleStats";
inline constexpr std::string_view kComputeModuleLogsIsolatedDocument = R"gql(query ComputeModuleLogs($appId: BigInt!, $moduleName: String, $limit: Int) {
  computeModuleLogs(appId: $appId, moduleName: $moduleName, limit: $limit) {
    ts
    moduleName
    level
    message
    triggerSource
  }
})gql";
inline constexpr std::string_view kComputeModuleLogsOperationName = "ComputeModuleLogs";
inline constexpr std::string_view kComputeAppDiagnosticsIsolatedDocument = R"gql(query ComputeAppDiagnostics($appId: BigInt!) {
  computeAppDiagnostics(appId: $appId) {
    appId
    moduleCount
    enabledModuleCount
    versionCount
    triggerCount
    runs24h
    failedRuns24h
    fuelUsed24h
    topModules {
      moduleName
      runs
      failures
    }
    toolchainRustVersion
    toolchainWasmOptVersion
  }
})gql";
inline constexpr std::string_view kComputeAppDiagnosticsOperationName = "ComputeAppDiagnostics";
inline constexpr std::string_view kComputeTemplatesIsolatedDocument = R"gql(query ComputeTemplates($appId: BigInt!) {
  computeTemplates(appId: $appId) {
    name
    description
    exports
  }
})gql";
inline constexpr std::string_view kComputeTemplatesOperationName = "ComputeTemplates";
inline constexpr std::string_view kComputeDeployTemplateIsolatedDocument = R"gql(mutation ComputeDeployTemplate($appId: BigInt!, $templateName: String!, $moduleName: String) {
  computeDeployTemplate(
    appId: $appId
    templateName: $templateName
    moduleName: $moduleName
  ) {
    ...ComputeModuleFields
  }
}

fragment ComputeModuleFields on WasmModule {
  moduleId
  appId
  name
  description
  enabled
  currentVersionId
  circuitState
  consecutiveFailures
  cooldownUntil
  breakerLatchedAt
  breakerReason
  lastError
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kComputeDeployTemplateOperationName = "ComputeDeployTemplate";

/// compute/UserCodeFaults.graphql
inline constexpr std::string_view kUserCodeFaultsDocument = R"gql(fragment UserCodeFaultRecordFields on UserCodeFaultRecord {
  faultId
  appId
  engine
  kind
  blame
  retryable
  subject
  entryPoint
  flowId
  gridId
  actingUserId
  durationUs
  budgetUs
  unitsUsed
  unitsLimit
  stepsCompleted
  detail
  instanceId
  occurredAt
}

query UserCodeFaults(
  $appId: BigInt!
  $engine: UserCodeFaultEngine
  $kind: UserCodeFaultKind
  $blame: UserCodeFaultBlame
  $subject: String
  $windowMinutes: Int
  $limit: Int
  $offset: Int
) {
  userCodeFaults(
    appId: $appId
    engine: $engine
    kind: $kind
    blame: $blame
    subject: $subject
    windowMinutes: $windowMinutes
    limit: $limit
    offset: $offset
  ) {
    ...UserCodeFaultRecordFields
  }
}

query UserCodeFaultSummary($appId: BigInt!, $windowMinutes: Int) {
  userCodeFaultSummary(appId: $appId, windowMinutes: $windowMinutes) {
    engine
    kind
    blame
    subject
    faults
    lastAt
  }
})gql";
inline constexpr std::string_view kUserCodeFaultsIsolatedDocument = R"gql(query UserCodeFaults($appId: BigInt!, $engine: UserCodeFaultEngine, $kind: UserCodeFaultKind, $blame: UserCodeFaultBlame, $subject: String, $windowMinutes: Int, $limit: Int, $offset: Int) {
  userCodeFaults(
    appId: $appId
    engine: $engine
    kind: $kind
    blame: $blame
    subject: $subject
    windowMinutes: $windowMinutes
    limit: $limit
    offset: $offset
  ) {
    ...UserCodeFaultRecordFields
  }
}

fragment UserCodeFaultRecordFields on UserCodeFaultRecord {
  faultId
  appId
  engine
  kind
  blame
  retryable
  subject
  entryPoint
  flowId
  gridId
  actingUserId
  durationUs
  budgetUs
  unitsUsed
  unitsLimit
  stepsCompleted
  detail
  instanceId
  occurredAt
})gql";
inline constexpr std::string_view kUserCodeFaultsOperationName = "UserCodeFaults";
inline constexpr std::string_view kUserCodeFaultSummaryIsolatedDocument = R"gql(query UserCodeFaultSummary($appId: BigInt!, $windowMinutes: Int) {
  userCodeFaultSummary(appId: $appId, windowMinutes: $windowMinutes) {
    engine
    kind
    blame
    subject
    faults
    lastAt
  }
})gql";
inline constexpr std::string_view kUserCodeFaultSummaryOperationName = "UserCodeFaultSummary";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "AppComputeUsage") return kAppComputeUsageIsolatedDocument;
  if (operationName == "AppComputeBudgetStatus") return kAppComputeBudgetStatusIsolatedDocument;
  if (operationName == "AppComputeBudget") return kAppComputeBudgetIsolatedDocument;
  if (operationName == "SetAppComputeBudget") return kSetAppComputeBudgetIsolatedDocument;
  if (operationName == "ClearAppComputeBudget") return kClearAppComputeBudgetIsolatedDocument;
  if (operationName == "ComputeUpsertModule") return kComputeUpsertModuleIsolatedDocument;
  if (operationName == "ComputeDeployVersion") return kComputeDeployVersionIsolatedDocument;
  if (operationName == "ComputeSetModuleEnabled") return kComputeSetModuleEnabledIsolatedDocument;
  if (operationName == "ComputeResetBreaker") return kComputeResetBreakerIsolatedDocument;
  if (operationName == "ComputeDeleteModule") return kComputeDeleteModuleIsolatedDocument;
  if (operationName == "ComputeUpsertTrigger") return kComputeUpsertTriggerIsolatedDocument;
  if (operationName == "ComputeDeleteTrigger") return kComputeDeleteTriggerIsolatedDocument;
  if (operationName == "ComputeSetPolicy") return kComputeSetPolicyIsolatedDocument;
  if (operationName == "ComputeInvoke") return kComputeInvokeIsolatedDocument;
  if (operationName == "ComputeModules") return kComputeModulesIsolatedDocument;
  if (operationName == "ComputeModule") return kComputeModuleIsolatedDocument;
  if (operationName == "ComputeModuleVersions") return kComputeModuleVersionsIsolatedDocument;
  if (operationName == "ComputeModuleTriggers") return kComputeModuleTriggersIsolatedDocument;
  if (operationName == "ComputeModulePolicy") return kComputeModulePolicyIsolatedDocument;
  if (operationName == "ComputeModuleRuns") return kComputeModuleRunsIsolatedDocument;
  if (operationName == "ComputeModuleStats") return kComputeModuleStatsIsolatedDocument;
  if (operationName == "ComputeModuleLogs") return kComputeModuleLogsIsolatedDocument;
  if (operationName == "ComputeAppDiagnostics") return kComputeAppDiagnosticsIsolatedDocument;
  if (operationName == "ComputeTemplates") return kComputeTemplatesIsolatedDocument;
  if (operationName == "ComputeDeployTemplate") return kComputeDeployTemplateIsolatedDocument;
  if (operationName == "UserCodeFaults") return kUserCodeFaultsIsolatedDocument;
  if (operationName == "UserCodeFaultSummary") return kUserCodeFaultSummaryIsolatedDocument;
  return {};
}

}  // namespace compute

namespace controlPlane {

/// controlPlane/ControlPlane.graphql
inline constexpr std::string_view kControlPlaneDocument = R"gql(query CpComputePlatformCeilings {
  cpComputePlatformCeilings {
    maxModules
    maxTickHz
    fuelPerTick
    fuelPerInvoke
    maxMemoryMb
    maxRunMs
    maxDbOpsPerTick
    maxEgressMsgsPerMin
    maxEgressBytesPerMin
    maxStateWritesPerMin
    maxStateBytesPerMin
    updatedAt
    updatedByUserId
  }
}

mutation CpSetComputePlatformCeilings($input: CpSetComputePlatformCeilingsInput!) {
  cpSetComputePlatformCeilings(input: $input) {
    maxModules
    maxTickHz
    fuelPerTick
    fuelPerInvoke
    maxMemoryMb
    maxRunMs
    maxDbOpsPerTick
    maxEgressMsgsPerMin
    maxEgressBytesPerMin
    maxStateWritesPerMin
    maxStateBytesPerMin
    updatedAt
    updatedByUserId
  }
}

# The SANCTIONED way to put funds in an org wallet.
#
# A hand-written INSERT INTO org_wallets is silently wrong rather than an error:
# nothing assigns the wallet_id surrogate and the unique index tolerates repeated
# NULLs, so the row inserts and every later lookup by wallet_id misses. This
# mutation also re-evaluates the runtime out-of-funds decision, which is a stored
# verdict rather than a live read of the balance — funding without re-evaluating
# leaves a funded app still refusing every client.
#
# referenceId makes it idempotent: replaying one returns the original transaction
# instead of crediting twice.
mutation CpCreditOrgWallet(
  $orgId: BigInt!
  $amountCents: BigInt!
  $reason: String!
  $referenceId: String
) {
  creditOrgWallet(
    orgId: $orgId
    amountCents: $amountCents
    reason: $reason
    referenceId: $referenceId
  ) {
    transactionId
    walletId
    orgId
    amountCents
    balanceAfter
    transactionType
    description
    referenceId
    appId
    createdAt
  }
})gql";
inline constexpr std::string_view kCpComputePlatformCeilingsIsolatedDocument = R"gql(query CpComputePlatformCeilings {
  cpComputePlatformCeilings {
    maxModules
    maxTickHz
    fuelPerTick
    fuelPerInvoke
    maxMemoryMb
    maxRunMs
    maxDbOpsPerTick
    maxEgressMsgsPerMin
    maxEgressBytesPerMin
    maxStateWritesPerMin
    maxStateBytesPerMin
    updatedAt
    updatedByUserId
  }
})gql";
inline constexpr std::string_view kCpComputePlatformCeilingsOperationName = "CpComputePlatformCeilings";
inline constexpr std::string_view kCpSetComputePlatformCeilingsIsolatedDocument = R"gql(mutation CpSetComputePlatformCeilings($input: CpSetComputePlatformCeilingsInput!) {
  cpSetComputePlatformCeilings(input: $input) {
    maxModules
    maxTickHz
    fuelPerTick
    fuelPerInvoke
    maxMemoryMb
    maxRunMs
    maxDbOpsPerTick
    maxEgressMsgsPerMin
    maxEgressBytesPerMin
    maxStateWritesPerMin
    maxStateBytesPerMin
    updatedAt
    updatedByUserId
  }
})gql";
inline constexpr std::string_view kCpSetComputePlatformCeilingsOperationName = "CpSetComputePlatformCeilings";
inline constexpr std::string_view kCpCreditOrgWalletIsolatedDocument = R"gql(mutation CpCreditOrgWallet($orgId: BigInt!, $amountCents: BigInt!, $reason: String!, $referenceId: String) {
  creditOrgWallet(
    orgId: $orgId
    amountCents: $amountCents
    reason: $reason
    referenceId: $referenceId
  ) {
    transactionId
    walletId
    orgId
    amountCents
    balanceAfter
    transactionType
    description
    referenceId
    appId
    createdAt
  }
})gql";
inline constexpr std::string_view kCpCreditOrgWalletOperationName = "CpCreditOrgWallet";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "CpComputePlatformCeilings") return kCpComputePlatformCeilingsIsolatedDocument;
  if (operationName == "CpSetComputePlatformCeilings") return kCpSetComputePlatformCeilingsIsolatedDocument;
  if (operationName == "CpCreditOrgWallet") return kCpCreditOrgWalletIsolatedDocument;
  return {};
}

}  // namespace controlPlane

namespace crowdyStudio {

/// crowdyStudio/CrowdyStudio.graphql
inline constexpr std::string_view kCrowdyStudioDocument = R"gql(fragment CrowdyStudioProjectFields on CrowdyStudioProject {
  projectId
  appId
  ownerUserId
  gridId
  name
  description
  serverModuleName
  clientModuleName
  pairingPreference
  sdkVersion
  abiVersion
  revision
  archived
  archivedAt
  fileCount
  totalBytes
  createdAt
  updatedAt
  files {
    target
    path
    content
    revision
    provenance
    provenanceLibraryFileId
    provenanceLibraryRevision
    provenanceCommonVersionId
    createdAt
    updatedAt
  }
}

fragment CrowdyStudioLibraryFileFields on CrowdyStudioLibraryFile {
  libraryFileId
  appId
  ownerUserId
  title
  pathHint
  target
  tags
  content
  revision
  archived
  archivedAt
  createdAt
  updatedAt
}

fragment CrowdyStudioCommonFileFields on CrowdyStudioCommonFile {
  commonFileId
  appId
  slug
  title
  description
  path
  target
  tags
  status
  versionId
  versionNo
  content
  contentSha256
  publishedByUserId
  publishedAt
  createdAt
  updatedAt
}

query CrowdyStudioProjects(
  $appId: BigInt!
  $includeArchived: Boolean
  $limit: Int
  $offset: Int
) {
  crowdyStudioProjects(
    appId: $appId
    includeArchived: $includeArchived
    limit: $limit
    offset: $offset
  ) {
    projectId
    gridId
    name
    serverModuleName
    clientModuleName
    pairingPreference
    revision
    archived
    updatedAt
  }
}

query CrowdyStudioProject($appId: BigInt!, $projectId: String!) {
  crowdyStudioProject(appId: $appId, projectId: $projectId) {
    ...CrowdyStudioProjectFields
  }
}

mutation CrowdyStudioProjectCreate($input: CreateCrowdyStudioProjectInput!) {
  crowdyStudioProjectCreate(input: $input) {
    ...CrowdyStudioProjectFields
  }
}

mutation CrowdyStudioProjectSaveMetadata(
  $input: SaveCrowdyStudioProjectMetadataInput!
) {
  crowdyStudioProjectSaveMetadata(input: $input) {
    ...CrowdyStudioProjectFields
  }
}

mutation CrowdyStudioProjectSave($input: SaveCrowdyStudioProjectInput!) {
  crowdyStudioProjectSave(input: $input) {
    ...CrowdyStudioProjectFields
  }
}

mutation CrowdyStudioProjectSaveFiles(
  $input: SaveCrowdyStudioProjectFilesInput!
) {
  crowdyStudioProjectSaveFiles(input: $input) {
    ...CrowdyStudioProjectFields
  }
}

mutation CrowdyStudioProjectSetArchived(
  $input: SetCrowdyStudioProjectArchivedInput!
) {
  crowdyStudioProjectSetArchived(input: $input) {
    ...CrowdyStudioProjectFields
  }
}

query CrowdyStudioLibraryFiles(
  $appId: BigInt!
  $includeArchived: Boolean
  $limit: Int
  $offset: Int
) {
  crowdyStudioLibraryFiles(
    appId: $appId
    includeArchived: $includeArchived
    limit: $limit
    offset: $offset
  ) {
    ...CrowdyStudioLibraryFileFields
  }
}

mutation CrowdyStudioLibrarySave($input: SaveCrowdyStudioLibraryFileInput!) {
  crowdyStudioLibrarySave(input: $input) {
    ...CrowdyStudioLibraryFileFields
  }
}

mutation CrowdyStudioLibrarySetArchived(
  $input: SetCrowdyStudioLibraryFileArchivedInput!
) {
  crowdyStudioLibrarySetArchived(input: $input) {
    ...CrowdyStudioLibraryFileFields
  }
}

query CrowdyStudioCommonFiles(
  $appId: BigInt!
  $target: CrowdyStudioTarget
  $limit: Int
  $offset: Int
) {
  crowdyStudioCommonFiles(
    appId: $appId
    target: $target
    limit: $limit
    offset: $offset
  ) {
    ...CrowdyStudioCommonFileFields
  }
}

mutation CrowdyStudioProjectImportFile(
  $input: ImportCrowdyStudioProjectFileInput!
) {
  crowdyStudioProjectImportFile(input: $input) {
    ...CrowdyStudioProjectFields
  }
}

mutation CrowdyStudioCommonPublish(
  $input: PublishCrowdyStudioCommonFileInput!
) {
  crowdyStudioCommonPublish(input: $input) {
    ...CrowdyStudioCommonFileFields
  }
}

mutation CrowdyStudioProjectCreateFromModules(
  $input: CreateCrowdyStudioProjectFromModulesInput!
) {
  crowdyStudioProjectCreateFromModules(input: $input) {
    ...CrowdyStudioProjectFields
  }
})gql";
inline constexpr std::string_view kCrowdyStudioProjectsIsolatedDocument = R"gql(query CrowdyStudioProjects($appId: BigInt!, $includeArchived: Boolean, $limit: Int, $offset: Int) {
  crowdyStudioProjects(
    appId: $appId
    includeArchived: $includeArchived
    limit: $limit
    offset: $offset
  ) {
    projectId
    gridId
    name
    serverModuleName
    clientModuleName
    pairingPreference
    revision
    archived
    updatedAt
  }
})gql";
inline constexpr std::string_view kCrowdyStudioProjectsOperationName = "CrowdyStudioProjects";
inline constexpr std::string_view kCrowdyStudioProjectIsolatedDocument = R"gql(query CrowdyStudioProject($appId: BigInt!, $projectId: String!) {
  crowdyStudioProject(appId: $appId, projectId: $projectId) {
    ...CrowdyStudioProjectFields
  }
}

fragment CrowdyStudioProjectFields on CrowdyStudioProject {
  projectId
  appId
  ownerUserId
  gridId
  name
  description
  serverModuleName
  clientModuleName
  pairingPreference
  sdkVersion
  abiVersion
  revision
  archived
  archivedAt
  fileCount
  totalBytes
  createdAt
  updatedAt
  files {
    target
    path
    content
    revision
    provenance
    provenanceLibraryFileId
    provenanceLibraryRevision
    provenanceCommonVersionId
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kCrowdyStudioProjectOperationName = "CrowdyStudioProject";
inline constexpr std::string_view kCrowdyStudioProjectCreateIsolatedDocument = R"gql(mutation CrowdyStudioProjectCreate($input: CreateCrowdyStudioProjectInput!) {
  crowdyStudioProjectCreate(input: $input) {
    ...CrowdyStudioProjectFields
  }
}

fragment CrowdyStudioProjectFields on CrowdyStudioProject {
  projectId
  appId
  ownerUserId
  gridId
  name
  description
  serverModuleName
  clientModuleName
  pairingPreference
  sdkVersion
  abiVersion
  revision
  archived
  archivedAt
  fileCount
  totalBytes
  createdAt
  updatedAt
  files {
    target
    path
    content
    revision
    provenance
    provenanceLibraryFileId
    provenanceLibraryRevision
    provenanceCommonVersionId
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kCrowdyStudioProjectCreateOperationName = "CrowdyStudioProjectCreate";
inline constexpr std::string_view kCrowdyStudioProjectSaveMetadataIsolatedDocument = R"gql(mutation CrowdyStudioProjectSaveMetadata($input: SaveCrowdyStudioProjectMetadataInput!) {
  crowdyStudioProjectSaveMetadata(input: $input) {
    ...CrowdyStudioProjectFields
  }
}

fragment CrowdyStudioProjectFields on CrowdyStudioProject {
  projectId
  appId
  ownerUserId
  gridId
  name
  description
  serverModuleName
  clientModuleName
  pairingPreference
  sdkVersion
  abiVersion
  revision
  archived
  archivedAt
  fileCount
  totalBytes
  createdAt
  updatedAt
  files {
    target
    path
    content
    revision
    provenance
    provenanceLibraryFileId
    provenanceLibraryRevision
    provenanceCommonVersionId
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kCrowdyStudioProjectSaveMetadataOperationName = "CrowdyStudioProjectSaveMetadata";
inline constexpr std::string_view kCrowdyStudioProjectSaveIsolatedDocument = R"gql(mutation CrowdyStudioProjectSave($input: SaveCrowdyStudioProjectInput!) {
  crowdyStudioProjectSave(input: $input) {
    ...CrowdyStudioProjectFields
  }
}

fragment CrowdyStudioProjectFields on CrowdyStudioProject {
  projectId
  appId
  ownerUserId
  gridId
  name
  description
  serverModuleName
  clientModuleName
  pairingPreference
  sdkVersion
  abiVersion
  revision
  archived
  archivedAt
  fileCount
  totalBytes
  createdAt
  updatedAt
  files {
    target
    path
    content
    revision
    provenance
    provenanceLibraryFileId
    provenanceLibraryRevision
    provenanceCommonVersionId
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kCrowdyStudioProjectSaveOperationName = "CrowdyStudioProjectSave";
inline constexpr std::string_view kCrowdyStudioProjectSaveFilesIsolatedDocument = R"gql(mutation CrowdyStudioProjectSaveFiles($input: SaveCrowdyStudioProjectFilesInput!) {
  crowdyStudioProjectSaveFiles(input: $input) {
    ...CrowdyStudioProjectFields
  }
}

fragment CrowdyStudioProjectFields on CrowdyStudioProject {
  projectId
  appId
  ownerUserId
  gridId
  name
  description
  serverModuleName
  clientModuleName
  pairingPreference
  sdkVersion
  abiVersion
  revision
  archived
  archivedAt
  fileCount
  totalBytes
  createdAt
  updatedAt
  files {
    target
    path
    content
    revision
    provenance
    provenanceLibraryFileId
    provenanceLibraryRevision
    provenanceCommonVersionId
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kCrowdyStudioProjectSaveFilesOperationName = "CrowdyStudioProjectSaveFiles";
inline constexpr std::string_view kCrowdyStudioProjectSetArchivedIsolatedDocument = R"gql(mutation CrowdyStudioProjectSetArchived($input: SetCrowdyStudioProjectArchivedInput!) {
  crowdyStudioProjectSetArchived(input: $input) {
    ...CrowdyStudioProjectFields
  }
}

fragment CrowdyStudioProjectFields on CrowdyStudioProject {
  projectId
  appId
  ownerUserId
  gridId
  name
  description
  serverModuleName
  clientModuleName
  pairingPreference
  sdkVersion
  abiVersion
  revision
  archived
  archivedAt
  fileCount
  totalBytes
  createdAt
  updatedAt
  files {
    target
    path
    content
    revision
    provenance
    provenanceLibraryFileId
    provenanceLibraryRevision
    provenanceCommonVersionId
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kCrowdyStudioProjectSetArchivedOperationName = "CrowdyStudioProjectSetArchived";
inline constexpr std::string_view kCrowdyStudioLibraryFilesIsolatedDocument = R"gql(query CrowdyStudioLibraryFiles($appId: BigInt!, $includeArchived: Boolean, $limit: Int, $offset: Int) {
  crowdyStudioLibraryFiles(
    appId: $appId
    includeArchived: $includeArchived
    limit: $limit
    offset: $offset
  ) {
    ...CrowdyStudioLibraryFileFields
  }
}

fragment CrowdyStudioLibraryFileFields on CrowdyStudioLibraryFile {
  libraryFileId
  appId
  ownerUserId
  title
  pathHint
  target
  tags
  content
  revision
  archived
  archivedAt
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kCrowdyStudioLibraryFilesOperationName = "CrowdyStudioLibraryFiles";
inline constexpr std::string_view kCrowdyStudioLibrarySaveIsolatedDocument = R"gql(mutation CrowdyStudioLibrarySave($input: SaveCrowdyStudioLibraryFileInput!) {
  crowdyStudioLibrarySave(input: $input) {
    ...CrowdyStudioLibraryFileFields
  }
}

fragment CrowdyStudioLibraryFileFields on CrowdyStudioLibraryFile {
  libraryFileId
  appId
  ownerUserId
  title
  pathHint
  target
  tags
  content
  revision
  archived
  archivedAt
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kCrowdyStudioLibrarySaveOperationName = "CrowdyStudioLibrarySave";
inline constexpr std::string_view kCrowdyStudioLibrarySetArchivedIsolatedDocument = R"gql(mutation CrowdyStudioLibrarySetArchived($input: SetCrowdyStudioLibraryFileArchivedInput!) {
  crowdyStudioLibrarySetArchived(input: $input) {
    ...CrowdyStudioLibraryFileFields
  }
}

fragment CrowdyStudioLibraryFileFields on CrowdyStudioLibraryFile {
  libraryFileId
  appId
  ownerUserId
  title
  pathHint
  target
  tags
  content
  revision
  archived
  archivedAt
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kCrowdyStudioLibrarySetArchivedOperationName = "CrowdyStudioLibrarySetArchived";
inline constexpr std::string_view kCrowdyStudioCommonFilesIsolatedDocument = R"gql(query CrowdyStudioCommonFiles($appId: BigInt!, $target: CrowdyStudioTarget, $limit: Int, $offset: Int) {
  crowdyStudioCommonFiles(
    appId: $appId
    target: $target
    limit: $limit
    offset: $offset
  ) {
    ...CrowdyStudioCommonFileFields
  }
}

fragment CrowdyStudioCommonFileFields on CrowdyStudioCommonFile {
  commonFileId
  appId
  slug
  title
  description
  path
  target
  tags
  status
  versionId
  versionNo
  content
  contentSha256
  publishedByUserId
  publishedAt
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kCrowdyStudioCommonFilesOperationName = "CrowdyStudioCommonFiles";
inline constexpr std::string_view kCrowdyStudioProjectImportFileIsolatedDocument = R"gql(mutation CrowdyStudioProjectImportFile($input: ImportCrowdyStudioProjectFileInput!) {
  crowdyStudioProjectImportFile(input: $input) {
    ...CrowdyStudioProjectFields
  }
}

fragment CrowdyStudioProjectFields on CrowdyStudioProject {
  projectId
  appId
  ownerUserId
  gridId
  name
  description
  serverModuleName
  clientModuleName
  pairingPreference
  sdkVersion
  abiVersion
  revision
  archived
  archivedAt
  fileCount
  totalBytes
  createdAt
  updatedAt
  files {
    target
    path
    content
    revision
    provenance
    provenanceLibraryFileId
    provenanceLibraryRevision
    provenanceCommonVersionId
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kCrowdyStudioProjectImportFileOperationName = "CrowdyStudioProjectImportFile";
inline constexpr std::string_view kCrowdyStudioCommonPublishIsolatedDocument = R"gql(mutation CrowdyStudioCommonPublish($input: PublishCrowdyStudioCommonFileInput!) {
  crowdyStudioCommonPublish(input: $input) {
    ...CrowdyStudioCommonFileFields
  }
}

fragment CrowdyStudioCommonFileFields on CrowdyStudioCommonFile {
  commonFileId
  appId
  slug
  title
  description
  path
  target
  tags
  status
  versionId
  versionNo
  content
  contentSha256
  publishedByUserId
  publishedAt
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kCrowdyStudioCommonPublishOperationName = "CrowdyStudioCommonPublish";
inline constexpr std::string_view kCrowdyStudioProjectCreateFromModulesIsolatedDocument = R"gql(mutation CrowdyStudioProjectCreateFromModules($input: CreateCrowdyStudioProjectFromModulesInput!) {
  crowdyStudioProjectCreateFromModules(input: $input) {
    ...CrowdyStudioProjectFields
  }
}

fragment CrowdyStudioProjectFields on CrowdyStudioProject {
  projectId
  appId
  ownerUserId
  gridId
  name
  description
  serverModuleName
  clientModuleName
  pairingPreference
  sdkVersion
  abiVersion
  revision
  archived
  archivedAt
  fileCount
  totalBytes
  createdAt
  updatedAt
  files {
    target
    path
    content
    revision
    provenance
    provenanceLibraryFileId
    provenanceLibraryRevision
    provenanceCommonVersionId
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kCrowdyStudioProjectCreateFromModulesOperationName = "CrowdyStudioProjectCreateFromModules";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "CrowdyStudioProjects") return kCrowdyStudioProjectsIsolatedDocument;
  if (operationName == "CrowdyStudioProject") return kCrowdyStudioProjectIsolatedDocument;
  if (operationName == "CrowdyStudioProjectCreate") return kCrowdyStudioProjectCreateIsolatedDocument;
  if (operationName == "CrowdyStudioProjectSaveMetadata") return kCrowdyStudioProjectSaveMetadataIsolatedDocument;
  if (operationName == "CrowdyStudioProjectSave") return kCrowdyStudioProjectSaveIsolatedDocument;
  if (operationName == "CrowdyStudioProjectSaveFiles") return kCrowdyStudioProjectSaveFilesIsolatedDocument;
  if (operationName == "CrowdyStudioProjectSetArchived") return kCrowdyStudioProjectSetArchivedIsolatedDocument;
  if (operationName == "CrowdyStudioLibraryFiles") return kCrowdyStudioLibraryFilesIsolatedDocument;
  if (operationName == "CrowdyStudioLibrarySave") return kCrowdyStudioLibrarySaveIsolatedDocument;
  if (operationName == "CrowdyStudioLibrarySetArchived") return kCrowdyStudioLibrarySetArchivedIsolatedDocument;
  if (operationName == "CrowdyStudioCommonFiles") return kCrowdyStudioCommonFilesIsolatedDocument;
  if (operationName == "CrowdyStudioProjectImportFile") return kCrowdyStudioProjectImportFileIsolatedDocument;
  if (operationName == "CrowdyStudioCommonPublish") return kCrowdyStudioCommonPublishIsolatedDocument;
  if (operationName == "CrowdyStudioProjectCreateFromModules") return kCrowdyStudioProjectCreateFromModulesIsolatedDocument;
  return {};
}

}  // namespace crowdyStudio

namespace crowdyStudioAgent {

/// crowdyStudioAgent/CrowdyStudioAgent.graphql
inline constexpr std::string_view kCrowdyStudioAgentDocument = R"gql(fragment CrowdyAgentErrorFields on AgentError {
  code
  message
  retryable
  remediation
  field
  requiredScope
}

fragment CrowdyAgentRunFields on AgentRun {
  runId
  status
  providerRounds
  toolCalls
  errorCode
  terminalReason
  reason
  startedAt
  finishedAt
  createdAt
  cancelled
}

fragment CrowdyAgentLeaseFields on AgentLease {
  leaseId
  kind
  status
  clientEpoch
  scopes
  holder
  contextVersion
  controlledEntityId
  hostCapabilityRevision
  expectedProjectRevision
  grantedAt
  expiresAt
  revokedReason
}

fragment CrowdyAgentApprovalFields on AgentApproval {
  approvalId
  toolCallId
  argumentHash
  status
  safeSummary
  clientEpoch
  expiresAt
  approved
  rejected
}

fragment CrowdyAgentSessionFields on AgentSession {
  contractVersion
  sessionId
  appId
  projectId
  gridId
  mode
  requestedModel
  model
  resolvedModel
  status
  providerDataConsent
  registryDigest
  providerPolicyVersion
  appPolicyVersion
  contextVersion
  currentClientEpoch
  clientEpoch
  lastEventSeq
  currentRun {
    ...CrowdyAgentRunFields
  }
  activeLeases {
    ...CrowdyAgentLeaseFields
  }
  pendingApproval {
    ...CrowdyAgentApprovalFields
  }
  createdAt
  updatedAt
  closedAt
}

fragment CrowdyAgentBudgetFields on AgentBudget {
  dimensions {
    name
    scope
    limit
    reserved
    consumed
    remaining
    unit
  }
  resetAt
  platformFunded
  payer
}

fragment CrowdyAgentToolDescriptorFields on AgentToolDescriptor {
  schemaVersion
  name
  wireName
  version
  summary
  executor
  modes
  risk
  riskEffects
  riskReversible
  scopes
  scopeRequirementsJson
  approvalRequired
  approvalPolicy
  approvalReasons
  approvalMaxTtlSeconds
  idempotencyClass
  idempotencyKeyScope
  timeoutMs
  inputSchemaJson
  outputSchemaJson
  inputRedactionJson
  outputRedactionJson
  maxPersistedBytes
  descriptorJson
  descriptorDigest
}

fragment CrowdyAgentEventBaseFields on AgentEventBase {
  protocolVersion
  eventId
  sessionId
  seq
  type
  runId
  version
  createdAt
}

fragment CrowdyAgentEventFields on CrowdyStudioAgentEvent {
  __typename
  ...CrowdyAgentEventBaseFields
  ... on AgentLifecycleEvent {
    lifecycleMode: mode
    lifecycleClientEpoch: clientEpoch
    lifecycleReplayAfterSeq: replayAfterSeq
    lifecycleReason: reason
    lifecycleContextVersion: contextVersion
  }
  ... on AgentMessageEvent {
    messageEventId: messageId
    messageRole: role
    messageContent: content
  }
  ... on AgentRunEvent {
    runStatus: status
    runCode: code
    runReason: reason
    runError: error {
      ...CrowdyAgentErrorFields
    }
  }
  ... on AgentToolEvent {
    toolEventCallId: toolCallId
    toolEventName: toolName
    toolEventVersion: toolVersion
    toolStatus: status
    toolSafeSummary: safeSummary
    toolDescriptorDigest: descriptorDigest
    toolArgumentHash: argumentHash
    toolExecutor: executor
    toolContextVersion: contextVersion
    toolClientEpoch: clientEpoch
    toolArgumentsJson: argumentsJson
    toolLeaseId: leaseId
    toolApprovalGrant: approvalGrant
    toolIdempotencyKey: idempotencyKey
    toolResultJson: resultJson
    toolInvocation: invocation {
      protocolVersion
      sessionId
      runId
      toolCallId
      name
      version
      descriptorDigest
      argumentsJson
      argumentHash
      contextVersion
      clientEpoch
      leaseId
      approvalGrant
      idempotencyKey
      deadline
    }
    toolResult: result {
      protocolVersion
      toolCallId
      status
      outputJson
      error {
        ...CrowdyAgentErrorFields
      }
      observedContextVersion
      startedAt
      finishedAt
    }
    toolError: error {
      ...CrowdyAgentErrorFields
    }
    toolDeadline: deadline
  }
  ... on AgentApprovalEvent {
    approvalEventId: approvalId
    approvalToolCallId: toolCallId
    approvalArgumentHash: argumentHash
    approvalStatus: status
    approvalSafeSummary: safeSummary
    approvalReasons: reasons
    approvalExpiresAt: expiresAt
  }
  ... on AgentLeaseEvent {
    leaseEventId: leaseId
    leaseKind: kind
    leaseStatus: status
    leaseClientEpoch: clientEpoch
    leaseScopes: scopes
    leaseHolder: holder
    leaseContextVersion: contextVersion
    leaseControlledEntityId: controlledEntityId
    leaseHostCapabilityRevision: hostCapabilityRevision
    leaseExpectedProjectRevision: expectedProjectRevision
    leaseGrantedAt: grantedAt
    leaseExpiresAt: expiresAt
    leaseReason: reason
  }
  ... on AgentCheckpointEvent {
    checkpointEventId: checkpointId
    checkpointProjectRevision: projectRevision
    checkpointContentHash: contentHash
    checkpointReason: reason
    checkpointFiles: files {
      target
      path
      contentHash
      byteLength
    }
    checkpointRestoredAt: restoredAt
  }
  ... on AgentBudgetEvent {
    budgetSnapshot: budget {
      ...CrowdyAgentBudgetFields
    }
  }
}

query CrowdyStudioAgentSession($sessionId: String!) {
  crowdyStudioAgentSession(sessionId: $sessionId) {
    ...CrowdyAgentSessionFields
  }
}

query CrowdyStudioAgentSessions($appId: BigInt!, $after: String, $first: Int) {
  crowdyStudioAgentSessions(appId: $appId, after: $after, first: $first) {
    edges {
      cursor
      node {
        ...CrowdyAgentSessionFields
      }
    }
    pageInfo {
      hasNextPage
      endCursor
    }
    nodes {
      ...CrowdyAgentSessionFields
    }
    endCursor
    hasNextPage
  }
}

query CrowdyStudioAgentHistory(
  $sessionId: String!
  $afterSeq: BigInt
  $first: Int
) {
  crowdyStudioAgentHistory(
    sessionId: $sessionId
    afterSeq: $afterSeq
    first: $first
  ) {
    edges {
      cursor
      node {
        ...CrowdyAgentEventFields
      }
    }
    pageInfo {
      hasNextPage
      endCursor
    }
    events {
      ...CrowdyAgentEventFields
    }
    hasMore
  }
}

query CrowdyStudioAgentToolDescriptors($sessionId: String!) {
  crowdyStudioAgentToolDescriptors(sessionId: $sessionId) {
    registryDigest
    tools {
      ...CrowdyAgentToolDescriptorFields
    }
  }
}

query CrowdyStudioAgentBudget($sessionId: String!) {
  crowdyStudioAgentBudget(sessionId: $sessionId) {
    ...CrowdyAgentBudgetFields
  }
}

mutation CrowdyStudioAgentCreateSession($input: CreateAgentSessionInput!) {
  crowdyStudioAgentCreateSession(input: $input) {
    ...CrowdyAgentSessionFields
  }
}

mutation CrowdyStudioAgentAttachClient($input: AttachAgentClientInput!) {
  crowdyStudioAgentAttachClient(input: $input) {
    session {
      ...CrowdyAgentSessionFields
    }
    clientEpoch
    replayAfterSeq
  }
}

mutation CrowdyStudioAgentSetMode($input: SetAgentModeInput!) {
  crowdyStudioAgentSetMode(input: $input) {
    ...CrowdyAgentSessionFields
  }
}

mutation CrowdyStudioAgentAcknowledgeEvents(
  $input: AcknowledgeAgentEventsInput!
) {
  crowdyStudioAgentAcknowledgeEvents(input: $input) {
    throughSeq
  }
}

mutation CrowdyStudioAgentHeartbeat($input: AgentHeartbeatInput!) {
  crowdyStudioAgentHeartbeat(input: $input) {
    serverTime
    playLeaseFreshUntil
    workspaceLeaseExpiresAt
  }
}

mutation CrowdyStudioAgentSendMessage($input: SendAgentMessageInput!) {
  crowdyStudioAgentSendMessage(input: $input) {
    ...CrowdyAgentRunFields
  }
}

mutation CrowdyStudioAgentApproveTool($input: DecideAgentToolInput!) {
  crowdyStudioAgentApproveTool(input: $input) {
    ...CrowdyAgentApprovalFields
  }
}

mutation CrowdyStudioAgentRejectTool($input: DecideAgentToolInput!) {
  crowdyStudioAgentRejectTool(input: $input) {
    ...CrowdyAgentApprovalFields
  }
}

mutation CrowdyStudioAgentToolResult($input: AgentToolResultInput!) {
  crowdyStudioAgentToolResult(input: $input) {
    toolCallId
    toolName
    status
    argumentHash
    error {
      ...CrowdyAgentErrorFields
    }
    accepted
  }
}

mutation CrowdyStudioAgentGrantLease($input: GrantAgentLeaseInput!) {
  crowdyStudioAgentGrantLease(input: $input) {
    ...CrowdyAgentLeaseFields
  }
}

mutation CrowdyStudioAgentRevokeLease($input: RevokeAgentLeaseInput!) {
  crowdyStudioAgentRevokeLease(input: $input) {
    ...CrowdyAgentLeaseFields
  }
}

mutation CrowdyStudioAgentPause($input: AgentSessionControlInput!) {
  crowdyStudioAgentPause(input: $input) {
    ...CrowdyAgentSessionFields
  }
}

mutation CrowdyStudioAgentResume($input: AgentSessionControlInput!) {
  crowdyStudioAgentResume(input: $input) {
    ...CrowdyAgentSessionFields
  }
}

mutation CrowdyStudioAgentCancelRun($input: CancelAgentRunInput!) {
  crowdyStudioAgentCancelRun(input: $input) {
    ...CrowdyAgentRunFields
  }
}

mutation CrowdyStudioAgentCloseSession($input: AgentSessionControlInput!) {
  crowdyStudioAgentCloseSession(input: $input) {
    ...CrowdyAgentSessionFields
  }
}

subscription CrowdyStudioAgentEvents(
  $sessionId: String!
  $afterSeq: BigInt!
  $clientEpoch: BigInt!
) {
  crowdyStudioAgentEvents(
    sessionId: $sessionId
    afterSeq: $afterSeq
    clientEpoch: $clientEpoch
  ) {
    ...CrowdyAgentEventFields
  }
})gql";
inline constexpr std::string_view kCrowdyStudioAgentSessionIsolatedDocument = R"gql(query CrowdyStudioAgentSession($sessionId: String!) {
  crowdyStudioAgentSession(sessionId: $sessionId) {
    ...CrowdyAgentSessionFields
  }
}

fragment CrowdyAgentSessionFields on AgentSession {
  contractVersion
  sessionId
  appId
  projectId
  gridId
  mode
  requestedModel
  model
  resolvedModel
  status
  providerDataConsent
  registryDigest
  providerPolicyVersion
  appPolicyVersion
  contextVersion
  currentClientEpoch
  clientEpoch
  lastEventSeq
  currentRun {
    ...CrowdyAgentRunFields
  }
  activeLeases {
    ...CrowdyAgentLeaseFields
  }
  pendingApproval {
    ...CrowdyAgentApprovalFields
  }
  createdAt
  updatedAt
  closedAt
}

fragment CrowdyAgentRunFields on AgentRun {
  runId
  status
  providerRounds
  toolCalls
  errorCode
  terminalReason
  reason
  startedAt
  finishedAt
  createdAt
  cancelled
}

fragment CrowdyAgentLeaseFields on AgentLease {
  leaseId
  kind
  status
  clientEpoch
  scopes
  holder
  contextVersion
  controlledEntityId
  hostCapabilityRevision
  expectedProjectRevision
  grantedAt
  expiresAt
  revokedReason
}

fragment CrowdyAgentApprovalFields on AgentApproval {
  approvalId
  toolCallId
  argumentHash
  status
  safeSummary
  clientEpoch
  expiresAt
  approved
  rejected
})gql";
inline constexpr std::string_view kCrowdyStudioAgentSessionOperationName = "CrowdyStudioAgentSession";
inline constexpr std::string_view kCrowdyStudioAgentSessionsIsolatedDocument = R"gql(query CrowdyStudioAgentSessions($appId: BigInt!, $after: String, $first: Int) {
  crowdyStudioAgentSessions(appId: $appId, after: $after, first: $first) {
    edges {
      cursor
      node {
        ...CrowdyAgentSessionFields
      }
    }
    pageInfo {
      hasNextPage
      endCursor
    }
    nodes {
      ...CrowdyAgentSessionFields
    }
    endCursor
    hasNextPage
  }
}

fragment CrowdyAgentSessionFields on AgentSession {
  contractVersion
  sessionId
  appId
  projectId
  gridId
  mode
  requestedModel
  model
  resolvedModel
  status
  providerDataConsent
  registryDigest
  providerPolicyVersion
  appPolicyVersion
  contextVersion
  currentClientEpoch
  clientEpoch
  lastEventSeq
  currentRun {
    ...CrowdyAgentRunFields
  }
  activeLeases {
    ...CrowdyAgentLeaseFields
  }
  pendingApproval {
    ...CrowdyAgentApprovalFields
  }
  createdAt
  updatedAt
  closedAt
}

fragment CrowdyAgentRunFields on AgentRun {
  runId
  status
  providerRounds
  toolCalls
  errorCode
  terminalReason
  reason
  startedAt
  finishedAt
  createdAt
  cancelled
}

fragment CrowdyAgentLeaseFields on AgentLease {
  leaseId
  kind
  status
  clientEpoch
  scopes
  holder
  contextVersion
  controlledEntityId
  hostCapabilityRevision
  expectedProjectRevision
  grantedAt
  expiresAt
  revokedReason
}

fragment CrowdyAgentApprovalFields on AgentApproval {
  approvalId
  toolCallId
  argumentHash
  status
  safeSummary
  clientEpoch
  expiresAt
  approved
  rejected
})gql";
inline constexpr std::string_view kCrowdyStudioAgentSessionsOperationName = "CrowdyStudioAgentSessions";
inline constexpr std::string_view kCrowdyStudioAgentHistoryIsolatedDocument = R"gql(query CrowdyStudioAgentHistory($sessionId: String!, $afterSeq: BigInt, $first: Int) {
  crowdyStudioAgentHistory(
    sessionId: $sessionId
    afterSeq: $afterSeq
    first: $first
  ) {
    edges {
      cursor
      node {
        ...CrowdyAgentEventFields
      }
    }
    pageInfo {
      hasNextPage
      endCursor
    }
    events {
      ...CrowdyAgentEventFields
    }
    hasMore
  }
}

fragment CrowdyAgentEventFields on CrowdyStudioAgentEvent {
  __typename
  ...CrowdyAgentEventBaseFields
  ... on AgentLifecycleEvent {
    lifecycleMode: mode
    lifecycleClientEpoch: clientEpoch
    lifecycleReplayAfterSeq: replayAfterSeq
    lifecycleReason: reason
    lifecycleContextVersion: contextVersion
  }
  ... on AgentMessageEvent {
    messageEventId: messageId
    messageRole: role
    messageContent: content
  }
  ... on AgentRunEvent {
    runStatus: status
    runCode: code
    runReason: reason
    runError: error {
      ...CrowdyAgentErrorFields
    }
  }
  ... on AgentToolEvent {
    toolEventCallId: toolCallId
    toolEventName: toolName
    toolEventVersion: toolVersion
    toolStatus: status
    toolSafeSummary: safeSummary
    toolDescriptorDigest: descriptorDigest
    toolArgumentHash: argumentHash
    toolExecutor: executor
    toolContextVersion: contextVersion
    toolClientEpoch: clientEpoch
    toolArgumentsJson: argumentsJson
    toolLeaseId: leaseId
    toolApprovalGrant: approvalGrant
    toolIdempotencyKey: idempotencyKey
    toolResultJson: resultJson
    toolInvocation: invocation {
      protocolVersion
      sessionId
      runId
      toolCallId
      name
      version
      descriptorDigest
      argumentsJson
      argumentHash
      contextVersion
      clientEpoch
      leaseId
      approvalGrant
      idempotencyKey
      deadline
    }
    toolResult: result {
      protocolVersion
      toolCallId
      status
      outputJson
      error {
        ...CrowdyAgentErrorFields
      }
      observedContextVersion
      startedAt
      finishedAt
    }
    toolError: error {
      ...CrowdyAgentErrorFields
    }
    toolDeadline: deadline
  }
  ... on AgentApprovalEvent {
    approvalEventId: approvalId
    approvalToolCallId: toolCallId
    approvalArgumentHash: argumentHash
    approvalStatus: status
    approvalSafeSummary: safeSummary
    approvalReasons: reasons
    approvalExpiresAt: expiresAt
  }
  ... on AgentLeaseEvent {
    leaseEventId: leaseId
    leaseKind: kind
    leaseStatus: status
    leaseClientEpoch: clientEpoch
    leaseScopes: scopes
    leaseHolder: holder
    leaseContextVersion: contextVersion
    leaseControlledEntityId: controlledEntityId
    leaseHostCapabilityRevision: hostCapabilityRevision
    leaseExpectedProjectRevision: expectedProjectRevision
    leaseGrantedAt: grantedAt
    leaseExpiresAt: expiresAt
    leaseReason: reason
  }
  ... on AgentCheckpointEvent {
    checkpointEventId: checkpointId
    checkpointProjectRevision: projectRevision
    checkpointContentHash: contentHash
    checkpointReason: reason
    checkpointFiles: files {
      target
      path
      contentHash
      byteLength
    }
    checkpointRestoredAt: restoredAt
  }
  ... on AgentBudgetEvent {
    budgetSnapshot: budget {
      ...CrowdyAgentBudgetFields
    }
  }
}

fragment CrowdyAgentEventBaseFields on AgentEventBase {
  protocolVersion
  eventId
  sessionId
  seq
  type
  runId
  version
  createdAt
}

fragment CrowdyAgentErrorFields on AgentError {
  code
  message
  retryable
  remediation
  field
  requiredScope
}

fragment CrowdyAgentBudgetFields on AgentBudget {
  dimensions {
    name
    scope
    limit
    reserved
    consumed
    remaining
    unit
  }
  resetAt
  platformFunded
  payer
})gql";
inline constexpr std::string_view kCrowdyStudioAgentHistoryOperationName = "CrowdyStudioAgentHistory";
inline constexpr std::string_view kCrowdyStudioAgentToolDescriptorsIsolatedDocument = R"gql(query CrowdyStudioAgentToolDescriptors($sessionId: String!) {
  crowdyStudioAgentToolDescriptors(sessionId: $sessionId) {
    registryDigest
    tools {
      ...CrowdyAgentToolDescriptorFields
    }
  }
}

fragment CrowdyAgentToolDescriptorFields on AgentToolDescriptor {
  schemaVersion
  name
  wireName
  version
  summary
  executor
  modes
  risk
  riskEffects
  riskReversible
  scopes
  scopeRequirementsJson
  approvalRequired
  approvalPolicy
  approvalReasons
  approvalMaxTtlSeconds
  idempotencyClass
  idempotencyKeyScope
  timeoutMs
  inputSchemaJson
  outputSchemaJson
  inputRedactionJson
  outputRedactionJson
  maxPersistedBytes
  descriptorJson
  descriptorDigest
})gql";
inline constexpr std::string_view kCrowdyStudioAgentToolDescriptorsOperationName = "CrowdyStudioAgentToolDescriptors";
inline constexpr std::string_view kCrowdyStudioAgentBudgetIsolatedDocument = R"gql(query CrowdyStudioAgentBudget($sessionId: String!) {
  crowdyStudioAgentBudget(sessionId: $sessionId) {
    ...CrowdyAgentBudgetFields
  }
}

fragment CrowdyAgentBudgetFields on AgentBudget {
  dimensions {
    name
    scope
    limit
    reserved
    consumed
    remaining
    unit
  }
  resetAt
  platformFunded
  payer
})gql";
inline constexpr std::string_view kCrowdyStudioAgentBudgetOperationName = "CrowdyStudioAgentBudget";
inline constexpr std::string_view kCrowdyStudioAgentCreateSessionIsolatedDocument = R"gql(mutation CrowdyStudioAgentCreateSession($input: CreateAgentSessionInput!) {
  crowdyStudioAgentCreateSession(input: $input) {
    ...CrowdyAgentSessionFields
  }
}

fragment CrowdyAgentSessionFields on AgentSession {
  contractVersion
  sessionId
  appId
  projectId
  gridId
  mode
  requestedModel
  model
  resolvedModel
  status
  providerDataConsent
  registryDigest
  providerPolicyVersion
  appPolicyVersion
  contextVersion
  currentClientEpoch
  clientEpoch
  lastEventSeq
  currentRun {
    ...CrowdyAgentRunFields
  }
  activeLeases {
    ...CrowdyAgentLeaseFields
  }
  pendingApproval {
    ...CrowdyAgentApprovalFields
  }
  createdAt
  updatedAt
  closedAt
}

fragment CrowdyAgentRunFields on AgentRun {
  runId
  status
  providerRounds
  toolCalls
  errorCode
  terminalReason
  reason
  startedAt
  finishedAt
  createdAt
  cancelled
}

fragment CrowdyAgentLeaseFields on AgentLease {
  leaseId
  kind
  status
  clientEpoch
  scopes
  holder
  contextVersion
  controlledEntityId
  hostCapabilityRevision
  expectedProjectRevision
  grantedAt
  expiresAt
  revokedReason
}

fragment CrowdyAgentApprovalFields on AgentApproval {
  approvalId
  toolCallId
  argumentHash
  status
  safeSummary
  clientEpoch
  expiresAt
  approved
  rejected
})gql";
inline constexpr std::string_view kCrowdyStudioAgentCreateSessionOperationName = "CrowdyStudioAgentCreateSession";
inline constexpr std::string_view kCrowdyStudioAgentAttachClientIsolatedDocument = R"gql(mutation CrowdyStudioAgentAttachClient($input: AttachAgentClientInput!) {
  crowdyStudioAgentAttachClient(input: $input) {
    session {
      ...CrowdyAgentSessionFields
    }
    clientEpoch
    replayAfterSeq
  }
}

fragment CrowdyAgentSessionFields on AgentSession {
  contractVersion
  sessionId
  appId
  projectId
  gridId
  mode
  requestedModel
  model
  resolvedModel
  status
  providerDataConsent
  registryDigest
  providerPolicyVersion
  appPolicyVersion
  contextVersion
  currentClientEpoch
  clientEpoch
  lastEventSeq
  currentRun {
    ...CrowdyAgentRunFields
  }
  activeLeases {
    ...CrowdyAgentLeaseFields
  }
  pendingApproval {
    ...CrowdyAgentApprovalFields
  }
  createdAt
  updatedAt
  closedAt
}

fragment CrowdyAgentRunFields on AgentRun {
  runId
  status
  providerRounds
  toolCalls
  errorCode
  terminalReason
  reason
  startedAt
  finishedAt
  createdAt
  cancelled
}

fragment CrowdyAgentLeaseFields on AgentLease {
  leaseId
  kind
  status
  clientEpoch
  scopes
  holder
  contextVersion
  controlledEntityId
  hostCapabilityRevision
  expectedProjectRevision
  grantedAt
  expiresAt
  revokedReason
}

fragment CrowdyAgentApprovalFields on AgentApproval {
  approvalId
  toolCallId
  argumentHash
  status
  safeSummary
  clientEpoch
  expiresAt
  approved
  rejected
})gql";
inline constexpr std::string_view kCrowdyStudioAgentAttachClientOperationName = "CrowdyStudioAgentAttachClient";
inline constexpr std::string_view kCrowdyStudioAgentSetModeIsolatedDocument = R"gql(mutation CrowdyStudioAgentSetMode($input: SetAgentModeInput!) {
  crowdyStudioAgentSetMode(input: $input) {
    ...CrowdyAgentSessionFields
  }
}

fragment CrowdyAgentSessionFields on AgentSession {
  contractVersion
  sessionId
  appId
  projectId
  gridId
  mode
  requestedModel
  model
  resolvedModel
  status
  providerDataConsent
  registryDigest
  providerPolicyVersion
  appPolicyVersion
  contextVersion
  currentClientEpoch
  clientEpoch
  lastEventSeq
  currentRun {
    ...CrowdyAgentRunFields
  }
  activeLeases {
    ...CrowdyAgentLeaseFields
  }
  pendingApproval {
    ...CrowdyAgentApprovalFields
  }
  createdAt
  updatedAt
  closedAt
}

fragment CrowdyAgentRunFields on AgentRun {
  runId
  status
  providerRounds
  toolCalls
  errorCode
  terminalReason
  reason
  startedAt
  finishedAt
  createdAt
  cancelled
}

fragment CrowdyAgentLeaseFields on AgentLease {
  leaseId
  kind
  status
  clientEpoch
  scopes
  holder
  contextVersion
  controlledEntityId
  hostCapabilityRevision
  expectedProjectRevision
  grantedAt
  expiresAt
  revokedReason
}

fragment CrowdyAgentApprovalFields on AgentApproval {
  approvalId
  toolCallId
  argumentHash
  status
  safeSummary
  clientEpoch
  expiresAt
  approved
  rejected
})gql";
inline constexpr std::string_view kCrowdyStudioAgentSetModeOperationName = "CrowdyStudioAgentSetMode";
inline constexpr std::string_view kCrowdyStudioAgentAcknowledgeEventsIsolatedDocument = R"gql(mutation CrowdyStudioAgentAcknowledgeEvents($input: AcknowledgeAgentEventsInput!) {
  crowdyStudioAgentAcknowledgeEvents(input: $input) {
    throughSeq
  }
})gql";
inline constexpr std::string_view kCrowdyStudioAgentAcknowledgeEventsOperationName = "CrowdyStudioAgentAcknowledgeEvents";
inline constexpr std::string_view kCrowdyStudioAgentHeartbeatIsolatedDocument = R"gql(mutation CrowdyStudioAgentHeartbeat($input: AgentHeartbeatInput!) {
  crowdyStudioAgentHeartbeat(input: $input) {
    serverTime
    playLeaseFreshUntil
    workspaceLeaseExpiresAt
  }
})gql";
inline constexpr std::string_view kCrowdyStudioAgentHeartbeatOperationName = "CrowdyStudioAgentHeartbeat";
inline constexpr std::string_view kCrowdyStudioAgentSendMessageIsolatedDocument = R"gql(mutation CrowdyStudioAgentSendMessage($input: SendAgentMessageInput!) {
  crowdyStudioAgentSendMessage(input: $input) {
    ...CrowdyAgentRunFields
  }
}

fragment CrowdyAgentRunFields on AgentRun {
  runId
  status
  providerRounds
  toolCalls
  errorCode
  terminalReason
  reason
  startedAt
  finishedAt
  createdAt
  cancelled
})gql";
inline constexpr std::string_view kCrowdyStudioAgentSendMessageOperationName = "CrowdyStudioAgentSendMessage";
inline constexpr std::string_view kCrowdyStudioAgentApproveToolIsolatedDocument = R"gql(mutation CrowdyStudioAgentApproveTool($input: DecideAgentToolInput!) {
  crowdyStudioAgentApproveTool(input: $input) {
    ...CrowdyAgentApprovalFields
  }
}

fragment CrowdyAgentApprovalFields on AgentApproval {
  approvalId
  toolCallId
  argumentHash
  status
  safeSummary
  clientEpoch
  expiresAt
  approved
  rejected
})gql";
inline constexpr std::string_view kCrowdyStudioAgentApproveToolOperationName = "CrowdyStudioAgentApproveTool";
inline constexpr std::string_view kCrowdyStudioAgentRejectToolIsolatedDocument = R"gql(mutation CrowdyStudioAgentRejectTool($input: DecideAgentToolInput!) {
  crowdyStudioAgentRejectTool(input: $input) {
    ...CrowdyAgentApprovalFields
  }
}

fragment CrowdyAgentApprovalFields on AgentApproval {
  approvalId
  toolCallId
  argumentHash
  status
  safeSummary
  clientEpoch
  expiresAt
  approved
  rejected
})gql";
inline constexpr std::string_view kCrowdyStudioAgentRejectToolOperationName = "CrowdyStudioAgentRejectTool";
inline constexpr std::string_view kCrowdyStudioAgentToolResultIsolatedDocument = R"gql(mutation CrowdyStudioAgentToolResult($input: AgentToolResultInput!) {
  crowdyStudioAgentToolResult(input: $input) {
    toolCallId
    toolName
    status
    argumentHash
    error {
      ...CrowdyAgentErrorFields
    }
    accepted
  }
}

fragment CrowdyAgentErrorFields on AgentError {
  code
  message
  retryable
  remediation
  field
  requiredScope
})gql";
inline constexpr std::string_view kCrowdyStudioAgentToolResultOperationName = "CrowdyStudioAgentToolResult";
inline constexpr std::string_view kCrowdyStudioAgentGrantLeaseIsolatedDocument = R"gql(mutation CrowdyStudioAgentGrantLease($input: GrantAgentLeaseInput!) {
  crowdyStudioAgentGrantLease(input: $input) {
    ...CrowdyAgentLeaseFields
  }
}

fragment CrowdyAgentLeaseFields on AgentLease {
  leaseId
  kind
  status
  clientEpoch
  scopes
  holder
  contextVersion
  controlledEntityId
  hostCapabilityRevision
  expectedProjectRevision
  grantedAt
  expiresAt
  revokedReason
})gql";
inline constexpr std::string_view kCrowdyStudioAgentGrantLeaseOperationName = "CrowdyStudioAgentGrantLease";
inline constexpr std::string_view kCrowdyStudioAgentRevokeLeaseIsolatedDocument = R"gql(mutation CrowdyStudioAgentRevokeLease($input: RevokeAgentLeaseInput!) {
  crowdyStudioAgentRevokeLease(input: $input) {
    ...CrowdyAgentLeaseFields
  }
}

fragment CrowdyAgentLeaseFields on AgentLease {
  leaseId
  kind
  status
  clientEpoch
  scopes
  holder
  contextVersion
  controlledEntityId
  hostCapabilityRevision
  expectedProjectRevision
  grantedAt
  expiresAt
  revokedReason
})gql";
inline constexpr std::string_view kCrowdyStudioAgentRevokeLeaseOperationName = "CrowdyStudioAgentRevokeLease";
inline constexpr std::string_view kCrowdyStudioAgentPauseIsolatedDocument = R"gql(mutation CrowdyStudioAgentPause($input: AgentSessionControlInput!) {
  crowdyStudioAgentPause(input: $input) {
    ...CrowdyAgentSessionFields
  }
}

fragment CrowdyAgentSessionFields on AgentSession {
  contractVersion
  sessionId
  appId
  projectId
  gridId
  mode
  requestedModel
  model
  resolvedModel
  status
  providerDataConsent
  registryDigest
  providerPolicyVersion
  appPolicyVersion
  contextVersion
  currentClientEpoch
  clientEpoch
  lastEventSeq
  currentRun {
    ...CrowdyAgentRunFields
  }
  activeLeases {
    ...CrowdyAgentLeaseFields
  }
  pendingApproval {
    ...CrowdyAgentApprovalFields
  }
  createdAt
  updatedAt
  closedAt
}

fragment CrowdyAgentRunFields on AgentRun {
  runId
  status
  providerRounds
  toolCalls
  errorCode
  terminalReason
  reason
  startedAt
  finishedAt
  createdAt
  cancelled
}

fragment CrowdyAgentLeaseFields on AgentLease {
  leaseId
  kind
  status
  clientEpoch
  scopes
  holder
  contextVersion
  controlledEntityId
  hostCapabilityRevision
  expectedProjectRevision
  grantedAt
  expiresAt
  revokedReason
}

fragment CrowdyAgentApprovalFields on AgentApproval {
  approvalId
  toolCallId
  argumentHash
  status
  safeSummary
  clientEpoch
  expiresAt
  approved
  rejected
})gql";
inline constexpr std::string_view kCrowdyStudioAgentPauseOperationName = "CrowdyStudioAgentPause";
inline constexpr std::string_view kCrowdyStudioAgentResumeIsolatedDocument = R"gql(mutation CrowdyStudioAgentResume($input: AgentSessionControlInput!) {
  crowdyStudioAgentResume(input: $input) {
    ...CrowdyAgentSessionFields
  }
}

fragment CrowdyAgentSessionFields on AgentSession {
  contractVersion
  sessionId
  appId
  projectId
  gridId
  mode
  requestedModel
  model
  resolvedModel
  status
  providerDataConsent
  registryDigest
  providerPolicyVersion
  appPolicyVersion
  contextVersion
  currentClientEpoch
  clientEpoch
  lastEventSeq
  currentRun {
    ...CrowdyAgentRunFields
  }
  activeLeases {
    ...CrowdyAgentLeaseFields
  }
  pendingApproval {
    ...CrowdyAgentApprovalFields
  }
  createdAt
  updatedAt
  closedAt
}

fragment CrowdyAgentRunFields on AgentRun {
  runId
  status
  providerRounds
  toolCalls
  errorCode
  terminalReason
  reason
  startedAt
  finishedAt
  createdAt
  cancelled
}

fragment CrowdyAgentLeaseFields on AgentLease {
  leaseId
  kind
  status
  clientEpoch
  scopes
  holder
  contextVersion
  controlledEntityId
  hostCapabilityRevision
  expectedProjectRevision
  grantedAt
  expiresAt
  revokedReason
}

fragment CrowdyAgentApprovalFields on AgentApproval {
  approvalId
  toolCallId
  argumentHash
  status
  safeSummary
  clientEpoch
  expiresAt
  approved
  rejected
})gql";
inline constexpr std::string_view kCrowdyStudioAgentResumeOperationName = "CrowdyStudioAgentResume";
inline constexpr std::string_view kCrowdyStudioAgentCancelRunIsolatedDocument = R"gql(mutation CrowdyStudioAgentCancelRun($input: CancelAgentRunInput!) {
  crowdyStudioAgentCancelRun(input: $input) {
    ...CrowdyAgentRunFields
  }
}

fragment CrowdyAgentRunFields on AgentRun {
  runId
  status
  providerRounds
  toolCalls
  errorCode
  terminalReason
  reason
  startedAt
  finishedAt
  createdAt
  cancelled
})gql";
inline constexpr std::string_view kCrowdyStudioAgentCancelRunOperationName = "CrowdyStudioAgentCancelRun";
inline constexpr std::string_view kCrowdyStudioAgentCloseSessionIsolatedDocument = R"gql(mutation CrowdyStudioAgentCloseSession($input: AgentSessionControlInput!) {
  crowdyStudioAgentCloseSession(input: $input) {
    ...CrowdyAgentSessionFields
  }
}

fragment CrowdyAgentSessionFields on AgentSession {
  contractVersion
  sessionId
  appId
  projectId
  gridId
  mode
  requestedModel
  model
  resolvedModel
  status
  providerDataConsent
  registryDigest
  providerPolicyVersion
  appPolicyVersion
  contextVersion
  currentClientEpoch
  clientEpoch
  lastEventSeq
  currentRun {
    ...CrowdyAgentRunFields
  }
  activeLeases {
    ...CrowdyAgentLeaseFields
  }
  pendingApproval {
    ...CrowdyAgentApprovalFields
  }
  createdAt
  updatedAt
  closedAt
}

fragment CrowdyAgentRunFields on AgentRun {
  runId
  status
  providerRounds
  toolCalls
  errorCode
  terminalReason
  reason
  startedAt
  finishedAt
  createdAt
  cancelled
}

fragment CrowdyAgentLeaseFields on AgentLease {
  leaseId
  kind
  status
  clientEpoch
  scopes
  holder
  contextVersion
  controlledEntityId
  hostCapabilityRevision
  expectedProjectRevision
  grantedAt
  expiresAt
  revokedReason
}

fragment CrowdyAgentApprovalFields on AgentApproval {
  approvalId
  toolCallId
  argumentHash
  status
  safeSummary
  clientEpoch
  expiresAt
  approved
  rejected
})gql";
inline constexpr std::string_view kCrowdyStudioAgentCloseSessionOperationName = "CrowdyStudioAgentCloseSession";
inline constexpr std::string_view kCrowdyStudioAgentEventsIsolatedDocument = R"gql(subscription CrowdyStudioAgentEvents($sessionId: String!, $afterSeq: BigInt!, $clientEpoch: BigInt!) {
  crowdyStudioAgentEvents(
    sessionId: $sessionId
    afterSeq: $afterSeq
    clientEpoch: $clientEpoch
  ) {
    ...CrowdyAgentEventFields
  }
}

fragment CrowdyAgentEventFields on CrowdyStudioAgentEvent {
  __typename
  ...CrowdyAgentEventBaseFields
  ... on AgentLifecycleEvent {
    lifecycleMode: mode
    lifecycleClientEpoch: clientEpoch
    lifecycleReplayAfterSeq: replayAfterSeq
    lifecycleReason: reason
    lifecycleContextVersion: contextVersion
  }
  ... on AgentMessageEvent {
    messageEventId: messageId
    messageRole: role
    messageContent: content
  }
  ... on AgentRunEvent {
    runStatus: status
    runCode: code
    runReason: reason
    runError: error {
      ...CrowdyAgentErrorFields
    }
  }
  ... on AgentToolEvent {
    toolEventCallId: toolCallId
    toolEventName: toolName
    toolEventVersion: toolVersion
    toolStatus: status
    toolSafeSummary: safeSummary
    toolDescriptorDigest: descriptorDigest
    toolArgumentHash: argumentHash
    toolExecutor: executor
    toolContextVersion: contextVersion
    toolClientEpoch: clientEpoch
    toolArgumentsJson: argumentsJson
    toolLeaseId: leaseId
    toolApprovalGrant: approvalGrant
    toolIdempotencyKey: idempotencyKey
    toolResultJson: resultJson
    toolInvocation: invocation {
      protocolVersion
      sessionId
      runId
      toolCallId
      name
      version
      descriptorDigest
      argumentsJson
      argumentHash
      contextVersion
      clientEpoch
      leaseId
      approvalGrant
      idempotencyKey
      deadline
    }
    toolResult: result {
      protocolVersion
      toolCallId
      status
      outputJson
      error {
        ...CrowdyAgentErrorFields
      }
      observedContextVersion
      startedAt
      finishedAt
    }
    toolError: error {
      ...CrowdyAgentErrorFields
    }
    toolDeadline: deadline
  }
  ... on AgentApprovalEvent {
    approvalEventId: approvalId
    approvalToolCallId: toolCallId
    approvalArgumentHash: argumentHash
    approvalStatus: status
    approvalSafeSummary: safeSummary
    approvalReasons: reasons
    approvalExpiresAt: expiresAt
  }
  ... on AgentLeaseEvent {
    leaseEventId: leaseId
    leaseKind: kind
    leaseStatus: status
    leaseClientEpoch: clientEpoch
    leaseScopes: scopes
    leaseHolder: holder
    leaseContextVersion: contextVersion
    leaseControlledEntityId: controlledEntityId
    leaseHostCapabilityRevision: hostCapabilityRevision
    leaseExpectedProjectRevision: expectedProjectRevision
    leaseGrantedAt: grantedAt
    leaseExpiresAt: expiresAt
    leaseReason: reason
  }
  ... on AgentCheckpointEvent {
    checkpointEventId: checkpointId
    checkpointProjectRevision: projectRevision
    checkpointContentHash: contentHash
    checkpointReason: reason
    checkpointFiles: files {
      target
      path
      contentHash
      byteLength
    }
    checkpointRestoredAt: restoredAt
  }
  ... on AgentBudgetEvent {
    budgetSnapshot: budget {
      ...CrowdyAgentBudgetFields
    }
  }
}

fragment CrowdyAgentEventBaseFields on AgentEventBase {
  protocolVersion
  eventId
  sessionId
  seq
  type
  runId
  version
  createdAt
}

fragment CrowdyAgentErrorFields on AgentError {
  code
  message
  retryable
  remediation
  field
  requiredScope
}

fragment CrowdyAgentBudgetFields on AgentBudget {
  dimensions {
    name
    scope
    limit
    reserved
    consumed
    remaining
    unit
  }
  resetAt
  platformFunded
  payer
})gql";
inline constexpr std::string_view kCrowdyStudioAgentEventsOperationName = "CrowdyStudioAgentEvents";

/// crowdyStudioAgent/CrowdyStudioAgentCatalog.graphql
inline constexpr std::string_view kCrowdyStudioAgentCatalogDocument = R"gql(query CpCrowdyStudioAgentCatalog {
  cpCrowdyStudioAgentCatalog {
    instanceEnabled
    instanceId
    datacenterCode
    provider
    platformPolicyVersion
    defaultModelId
    registryDigest
    modes
    riskClasses
    models {
      modelId
      isDefault
      selectable
      inputMicrosPerMillion
      outputMicrosPerMillion
    }
    tools {
      name
      summary
      executor
      modes
      riskClass
      approvalRequired
    }
  }
})gql";
inline constexpr std::string_view kCpCrowdyStudioAgentCatalogIsolatedDocument = R"gql(query CpCrowdyStudioAgentCatalog {
  cpCrowdyStudioAgentCatalog {
    instanceEnabled
    instanceId
    datacenterCode
    provider
    platformPolicyVersion
    defaultModelId
    registryDigest
    modes
    riskClasses
    models {
      modelId
      isDefault
      selectable
      inputMicrosPerMillion
      outputMicrosPerMillion
    }
    tools {
      name
      summary
      executor
      modes
      riskClass
      approvalRequired
    }
  }
})gql";
inline constexpr std::string_view kCpCrowdyStudioAgentCatalogOperationName = "CpCrowdyStudioAgentCatalog";

/// crowdyStudioAgent/CrowdyStudioAgentManagement.graphql
inline constexpr std::string_view kCrowdyStudioAgentManagementDocument = R"gql(fragment CrowdyStudioAgentPolicyFields on CrowdyStudioAgentPolicy {
  kind
  appId
  enabled
  killSwitch
  operatorKillSwitch
  disableReasonCode
  disableReason
  allowedModelIds
  allowedToolNames
  allowedModes
  allowedRiskClasses
  turnLimits {
    providerRequests
    inputTokens
    outputTokens
    reasoningTokens
    totalTokens
    providerCostMicrousd
    toolCalls
    toolRounds
    compiles
    wallClockMs
    concurrentProviderRequests
  }
  sessionLimits {
    providerRequests
    inputTokens
    outputTokens
    reasoningTokens
    totalTokens
    providerCostMicrousd
    toolCalls
    compiles
    concurrentRuns
  }
  playerDayLimits {
    providerRequests
    inputTokens
    outputTokens
    reasoningTokens
    totalTokens
    providerCostMicrousd
    toolCalls
    compiles
    concurrentSessions
    concurrentRunsPerApp
  }
  retention {
    assistantChunkHours
    detailedContextHours
    sessionDataDays
    usageDays
  }
  privacy {
    requireZdr
    denyDataCollection
    allowPrivateSource
    requirePrivateSourceConsent
    persistProviderBodies
  }
  funding {
    billingMode
    payerKind
    rateCardId
    walletDebitEnabled
  }
  capabilityGaps {
    mode
    code
    detail
  }
  revision
  platformRevision
  appRevision
  effectiveRevision
  createdAt
  updatedAt
}

fragment CrowdyStudioAgentUsageFields on CrowdyStudioAgentUsagePage {
  records {
    usageId
    appId
    userId
    sessionId
    runId
    provider
    providerGenerationId
    resolvedModelId
    accountingStatus
    requestCount
    promptTokens
    completionTokens
    reasoningTokens
    cachedTokens
    nativePromptTokens
    nativeCompletionTokens
    nativeReasoningTokens
    nativeCachedTokens
    toolCalls
    toolRounds
    compileCount
    wallClockMs
    reservedCostMicrousd
    providerCostUsd
    upstreamInferenceCostUsd
    zdrEnforced
    dataCollectionDenied
    platformPolicyRevision
    appPolicyRevision
    billingMode
    payerKind
    occurredAt
    ingestedAt
  }
  summary {
    requestCount
    promptTokens
    completionTokens
    reasoningTokens
    cachedTokens
    toolCalls
    toolRounds
    compileCount
    wallClockMs
    providerCostUsd
  }
  since
  until
}

query CrowdyStudioAgentPolicy($appId: BigInt!) {
  crowdyStudioAgentPolicy(appId: $appId) {
    ...CrowdyStudioAgentPolicyFields
  }
}

query CrowdyStudioAgentEffectivePolicy($appId: BigInt!) {
  crowdyStudioAgentEffectivePolicy(appId: $appId) {
    ...CrowdyStudioAgentPolicyFields
  }
}

query CrowdyStudioAgentUsage(
  $appId: BigInt!
  $since: DateTime
  $until: DateTime
  $limit: Int
) {
  crowdyStudioAgentUsage(
    appId: $appId
    since: $since
    until: $until
    limit: $limit
  ) {
    ...CrowdyStudioAgentUsageFields
  }
}

mutation CrowdyStudioAgentSetPolicy(
  $input: SetCrowdyStudioAgentAppPolicyInput!
) {
  setCrowdyStudioAgentPolicy(input: $input) {
    ...CrowdyStudioAgentPolicyFields
  }
}

query CpCrowdyStudioAgentPlatformPolicy {
  cpCrowdyStudioAgentPlatformPolicy {
    ...CrowdyStudioAgentPolicyFields
  }
}

mutation CpSetCrowdyStudioAgentPlatformPolicy(
  $input: SetCrowdyStudioAgentPlatformPolicyInput!
) {
  cpSetCrowdyStudioAgentPlatformPolicy(input: $input) {
    ...CrowdyStudioAgentPolicyFields
  }
}

mutation CpSetCrowdyStudioAgentAppKill(
  $input: SetCrowdyStudioAgentOperatorAppKillInput!
) {
  cpSetCrowdyStudioAgentAppKill(input: $input) {
    ...CrowdyStudioAgentPolicyFields
  }
})gql";
inline constexpr std::string_view kCrowdyStudioAgentPolicyIsolatedDocument = R"gql(query CrowdyStudioAgentPolicy($appId: BigInt!) {
  crowdyStudioAgentPolicy(appId: $appId) {
    ...CrowdyStudioAgentPolicyFields
  }
}

fragment CrowdyStudioAgentPolicyFields on CrowdyStudioAgentPolicy {
  kind
  appId
  enabled
  killSwitch
  operatorKillSwitch
  disableReasonCode
  disableReason
  allowedModelIds
  allowedToolNames
  allowedModes
  allowedRiskClasses
  turnLimits {
    providerRequests
    inputTokens
    outputTokens
    reasoningTokens
    totalTokens
    providerCostMicrousd
    toolCalls
    toolRounds
    compiles
    wallClockMs
    concurrentProviderRequests
  }
  sessionLimits {
    providerRequests
    inputTokens
    outputTokens
    reasoningTokens
    totalTokens
    providerCostMicrousd
    toolCalls
    compiles
    concurrentRuns
  }
  playerDayLimits {
    providerRequests
    inputTokens
    outputTokens
    reasoningTokens
    totalTokens
    providerCostMicrousd
    toolCalls
    compiles
    concurrentSessions
    concurrentRunsPerApp
  }
  retention {
    assistantChunkHours
    detailedContextHours
    sessionDataDays
    usageDays
  }
  privacy {
    requireZdr
    denyDataCollection
    allowPrivateSource
    requirePrivateSourceConsent
    persistProviderBodies
  }
  funding {
    billingMode
    payerKind
    rateCardId
    walletDebitEnabled
  }
  capabilityGaps {
    mode
    code
    detail
  }
  revision
  platformRevision
  appRevision
  effectiveRevision
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kCrowdyStudioAgentPolicyOperationName = "CrowdyStudioAgentPolicy";
inline constexpr std::string_view kCrowdyStudioAgentEffectivePolicyIsolatedDocument = R"gql(query CrowdyStudioAgentEffectivePolicy($appId: BigInt!) {
  crowdyStudioAgentEffectivePolicy(appId: $appId) {
    ...CrowdyStudioAgentPolicyFields
  }
}

fragment CrowdyStudioAgentPolicyFields on CrowdyStudioAgentPolicy {
  kind
  appId
  enabled
  killSwitch
  operatorKillSwitch
  disableReasonCode
  disableReason
  allowedModelIds
  allowedToolNames
  allowedModes
  allowedRiskClasses
  turnLimits {
    providerRequests
    inputTokens
    outputTokens
    reasoningTokens
    totalTokens
    providerCostMicrousd
    toolCalls
    toolRounds
    compiles
    wallClockMs
    concurrentProviderRequests
  }
  sessionLimits {
    providerRequests
    inputTokens
    outputTokens
    reasoningTokens
    totalTokens
    providerCostMicrousd
    toolCalls
    compiles
    concurrentRuns
  }
  playerDayLimits {
    providerRequests
    inputTokens
    outputTokens
    reasoningTokens
    totalTokens
    providerCostMicrousd
    toolCalls
    compiles
    concurrentSessions
    concurrentRunsPerApp
  }
  retention {
    assistantChunkHours
    detailedContextHours
    sessionDataDays
    usageDays
  }
  privacy {
    requireZdr
    denyDataCollection
    allowPrivateSource
    requirePrivateSourceConsent
    persistProviderBodies
  }
  funding {
    billingMode
    payerKind
    rateCardId
    walletDebitEnabled
  }
  capabilityGaps {
    mode
    code
    detail
  }
  revision
  platformRevision
  appRevision
  effectiveRevision
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kCrowdyStudioAgentEffectivePolicyOperationName = "CrowdyStudioAgentEffectivePolicy";
inline constexpr std::string_view kCrowdyStudioAgentUsageIsolatedDocument = R"gql(query CrowdyStudioAgentUsage($appId: BigInt!, $since: DateTime, $until: DateTime, $limit: Int) {
  crowdyStudioAgentUsage(
    appId: $appId
    since: $since
    until: $until
    limit: $limit
  ) {
    ...CrowdyStudioAgentUsageFields
  }
}

fragment CrowdyStudioAgentUsageFields on CrowdyStudioAgentUsagePage {
  records {
    usageId
    appId
    userId
    sessionId
    runId
    provider
    providerGenerationId
    resolvedModelId
    accountingStatus
    requestCount
    promptTokens
    completionTokens
    reasoningTokens
    cachedTokens
    nativePromptTokens
    nativeCompletionTokens
    nativeReasoningTokens
    nativeCachedTokens
    toolCalls
    toolRounds
    compileCount
    wallClockMs
    reservedCostMicrousd
    providerCostUsd
    upstreamInferenceCostUsd
    zdrEnforced
    dataCollectionDenied
    platformPolicyRevision
    appPolicyRevision
    billingMode
    payerKind
    occurredAt
    ingestedAt
  }
  summary {
    requestCount
    promptTokens
    completionTokens
    reasoningTokens
    cachedTokens
    toolCalls
    toolRounds
    compileCount
    wallClockMs
    providerCostUsd
  }
  since
  until
})gql";
inline constexpr std::string_view kCrowdyStudioAgentUsageOperationName = "CrowdyStudioAgentUsage";
inline constexpr std::string_view kCrowdyStudioAgentSetPolicyIsolatedDocument = R"gql(mutation CrowdyStudioAgentSetPolicy($input: SetCrowdyStudioAgentAppPolicyInput!) {
  setCrowdyStudioAgentPolicy(input: $input) {
    ...CrowdyStudioAgentPolicyFields
  }
}

fragment CrowdyStudioAgentPolicyFields on CrowdyStudioAgentPolicy {
  kind
  appId
  enabled
  killSwitch
  operatorKillSwitch
  disableReasonCode
  disableReason
  allowedModelIds
  allowedToolNames
  allowedModes
  allowedRiskClasses
  turnLimits {
    providerRequests
    inputTokens
    outputTokens
    reasoningTokens
    totalTokens
    providerCostMicrousd
    toolCalls
    toolRounds
    compiles
    wallClockMs
    concurrentProviderRequests
  }
  sessionLimits {
    providerRequests
    inputTokens
    outputTokens
    reasoningTokens
    totalTokens
    providerCostMicrousd
    toolCalls
    compiles
    concurrentRuns
  }
  playerDayLimits {
    providerRequests
    inputTokens
    outputTokens
    reasoningTokens
    totalTokens
    providerCostMicrousd
    toolCalls
    compiles
    concurrentSessions
    concurrentRunsPerApp
  }
  retention {
    assistantChunkHours
    detailedContextHours
    sessionDataDays
    usageDays
  }
  privacy {
    requireZdr
    denyDataCollection
    allowPrivateSource
    requirePrivateSourceConsent
    persistProviderBodies
  }
  funding {
    billingMode
    payerKind
    rateCardId
    walletDebitEnabled
  }
  capabilityGaps {
    mode
    code
    detail
  }
  revision
  platformRevision
  appRevision
  effectiveRevision
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kCrowdyStudioAgentSetPolicyOperationName = "CrowdyStudioAgentSetPolicy";
inline constexpr std::string_view kCpCrowdyStudioAgentPlatformPolicyIsolatedDocument = R"gql(query CpCrowdyStudioAgentPlatformPolicy {
  cpCrowdyStudioAgentPlatformPolicy {
    ...CrowdyStudioAgentPolicyFields
  }
}

fragment CrowdyStudioAgentPolicyFields on CrowdyStudioAgentPolicy {
  kind
  appId
  enabled
  killSwitch
  operatorKillSwitch
  disableReasonCode
  disableReason
  allowedModelIds
  allowedToolNames
  allowedModes
  allowedRiskClasses
  turnLimits {
    providerRequests
    inputTokens
    outputTokens
    reasoningTokens
    totalTokens
    providerCostMicrousd
    toolCalls
    toolRounds
    compiles
    wallClockMs
    concurrentProviderRequests
  }
  sessionLimits {
    providerRequests
    inputTokens
    outputTokens
    reasoningTokens
    totalTokens
    providerCostMicrousd
    toolCalls
    compiles
    concurrentRuns
  }
  playerDayLimits {
    providerRequests
    inputTokens
    outputTokens
    reasoningTokens
    totalTokens
    providerCostMicrousd
    toolCalls
    compiles
    concurrentSessions
    concurrentRunsPerApp
  }
  retention {
    assistantChunkHours
    detailedContextHours
    sessionDataDays
    usageDays
  }
  privacy {
    requireZdr
    denyDataCollection
    allowPrivateSource
    requirePrivateSourceConsent
    persistProviderBodies
  }
  funding {
    billingMode
    payerKind
    rateCardId
    walletDebitEnabled
  }
  capabilityGaps {
    mode
    code
    detail
  }
  revision
  platformRevision
  appRevision
  effectiveRevision
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kCpCrowdyStudioAgentPlatformPolicyOperationName = "CpCrowdyStudioAgentPlatformPolicy";
inline constexpr std::string_view kCpSetCrowdyStudioAgentPlatformPolicyIsolatedDocument = R"gql(mutation CpSetCrowdyStudioAgentPlatformPolicy($input: SetCrowdyStudioAgentPlatformPolicyInput!) {
  cpSetCrowdyStudioAgentPlatformPolicy(input: $input) {
    ...CrowdyStudioAgentPolicyFields
  }
}

fragment CrowdyStudioAgentPolicyFields on CrowdyStudioAgentPolicy {
  kind
  appId
  enabled
  killSwitch
  operatorKillSwitch
  disableReasonCode
  disableReason
  allowedModelIds
  allowedToolNames
  allowedModes
  allowedRiskClasses
  turnLimits {
    providerRequests
    inputTokens
    outputTokens
    reasoningTokens
    totalTokens
    providerCostMicrousd
    toolCalls
    toolRounds
    compiles
    wallClockMs
    concurrentProviderRequests
  }
  sessionLimits {
    providerRequests
    inputTokens
    outputTokens
    reasoningTokens
    totalTokens
    providerCostMicrousd
    toolCalls
    compiles
    concurrentRuns
  }
  playerDayLimits {
    providerRequests
    inputTokens
    outputTokens
    reasoningTokens
    totalTokens
    providerCostMicrousd
    toolCalls
    compiles
    concurrentSessions
    concurrentRunsPerApp
  }
  retention {
    assistantChunkHours
    detailedContextHours
    sessionDataDays
    usageDays
  }
  privacy {
    requireZdr
    denyDataCollection
    allowPrivateSource
    requirePrivateSourceConsent
    persistProviderBodies
  }
  funding {
    billingMode
    payerKind
    rateCardId
    walletDebitEnabled
  }
  capabilityGaps {
    mode
    code
    detail
  }
  revision
  platformRevision
  appRevision
  effectiveRevision
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kCpSetCrowdyStudioAgentPlatformPolicyOperationName = "CpSetCrowdyStudioAgentPlatformPolicy";
inline constexpr std::string_view kCpSetCrowdyStudioAgentAppKillIsolatedDocument = R"gql(mutation CpSetCrowdyStudioAgentAppKill($input: SetCrowdyStudioAgentOperatorAppKillInput!) {
  cpSetCrowdyStudioAgentAppKill(input: $input) {
    ...CrowdyStudioAgentPolicyFields
  }
}

fragment CrowdyStudioAgentPolicyFields on CrowdyStudioAgentPolicy {
  kind
  appId
  enabled
  killSwitch
  operatorKillSwitch
  disableReasonCode
  disableReason
  allowedModelIds
  allowedToolNames
  allowedModes
  allowedRiskClasses
  turnLimits {
    providerRequests
    inputTokens
    outputTokens
    reasoningTokens
    totalTokens
    providerCostMicrousd
    toolCalls
    toolRounds
    compiles
    wallClockMs
    concurrentProviderRequests
  }
  sessionLimits {
    providerRequests
    inputTokens
    outputTokens
    reasoningTokens
    totalTokens
    providerCostMicrousd
    toolCalls
    compiles
    concurrentRuns
  }
  playerDayLimits {
    providerRequests
    inputTokens
    outputTokens
    reasoningTokens
    totalTokens
    providerCostMicrousd
    toolCalls
    compiles
    concurrentSessions
    concurrentRunsPerApp
  }
  retention {
    assistantChunkHours
    detailedContextHours
    sessionDataDays
    usageDays
  }
  privacy {
    requireZdr
    denyDataCollection
    allowPrivateSource
    requirePrivateSourceConsent
    persistProviderBodies
  }
  funding {
    billingMode
    payerKind
    rateCardId
    walletDebitEnabled
  }
  capabilityGaps {
    mode
    code
    detail
  }
  revision
  platformRevision
  appRevision
  effectiveRevision
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kCpSetCrowdyStudioAgentAppKillOperationName = "CpSetCrowdyStudioAgentAppKill";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "CrowdyStudioAgentSession") return kCrowdyStudioAgentSessionIsolatedDocument;
  if (operationName == "CrowdyStudioAgentSessions") return kCrowdyStudioAgentSessionsIsolatedDocument;
  if (operationName == "CrowdyStudioAgentHistory") return kCrowdyStudioAgentHistoryIsolatedDocument;
  if (operationName == "CrowdyStudioAgentToolDescriptors") return kCrowdyStudioAgentToolDescriptorsIsolatedDocument;
  if (operationName == "CrowdyStudioAgentBudget") return kCrowdyStudioAgentBudgetIsolatedDocument;
  if (operationName == "CrowdyStudioAgentCreateSession") return kCrowdyStudioAgentCreateSessionIsolatedDocument;
  if (operationName == "CrowdyStudioAgentAttachClient") return kCrowdyStudioAgentAttachClientIsolatedDocument;
  if (operationName == "CrowdyStudioAgentSetMode") return kCrowdyStudioAgentSetModeIsolatedDocument;
  if (operationName == "CrowdyStudioAgentAcknowledgeEvents") return kCrowdyStudioAgentAcknowledgeEventsIsolatedDocument;
  if (operationName == "CrowdyStudioAgentHeartbeat") return kCrowdyStudioAgentHeartbeatIsolatedDocument;
  if (operationName == "CrowdyStudioAgentSendMessage") return kCrowdyStudioAgentSendMessageIsolatedDocument;
  if (operationName == "CrowdyStudioAgentApproveTool") return kCrowdyStudioAgentApproveToolIsolatedDocument;
  if (operationName == "CrowdyStudioAgentRejectTool") return kCrowdyStudioAgentRejectToolIsolatedDocument;
  if (operationName == "CrowdyStudioAgentToolResult") return kCrowdyStudioAgentToolResultIsolatedDocument;
  if (operationName == "CrowdyStudioAgentGrantLease") return kCrowdyStudioAgentGrantLeaseIsolatedDocument;
  if (operationName == "CrowdyStudioAgentRevokeLease") return kCrowdyStudioAgentRevokeLeaseIsolatedDocument;
  if (operationName == "CrowdyStudioAgentPause") return kCrowdyStudioAgentPauseIsolatedDocument;
  if (operationName == "CrowdyStudioAgentResume") return kCrowdyStudioAgentResumeIsolatedDocument;
  if (operationName == "CrowdyStudioAgentCancelRun") return kCrowdyStudioAgentCancelRunIsolatedDocument;
  if (operationName == "CrowdyStudioAgentCloseSession") return kCrowdyStudioAgentCloseSessionIsolatedDocument;
  if (operationName == "CrowdyStudioAgentEvents") return kCrowdyStudioAgentEventsIsolatedDocument;
  if (operationName == "CpCrowdyStudioAgentCatalog") return kCpCrowdyStudioAgentCatalogIsolatedDocument;
  if (operationName == "CrowdyStudioAgentPolicy") return kCrowdyStudioAgentPolicyIsolatedDocument;
  if (operationName == "CrowdyStudioAgentEffectivePolicy") return kCrowdyStudioAgentEffectivePolicyIsolatedDocument;
  if (operationName == "CrowdyStudioAgentUsage") return kCrowdyStudioAgentUsageIsolatedDocument;
  if (operationName == "CrowdyStudioAgentSetPolicy") return kCrowdyStudioAgentSetPolicyIsolatedDocument;
  if (operationName == "CpCrowdyStudioAgentPlatformPolicy") return kCpCrowdyStudioAgentPlatformPolicyIsolatedDocument;
  if (operationName == "CpSetCrowdyStudioAgentPlatformPolicy") return kCpSetCrowdyStudioAgentPlatformPolicyIsolatedDocument;
  if (operationName == "CpSetCrowdyStudioAgentAppKill") return kCpSetCrowdyStudioAgentAppKillIsolatedDocument;
  return {};
}

}  // namespace crowdyStudioAgent

namespace gameApps {

/// gameApps/GameApps.graphql
inline constexpr std::string_view kGameAppsDocument = R"gql(fragment GridOwnershipFields on GridOwnership {
  gridOwnershipId
  gridId
  appId
  ownerKind
  ownerRef
  tenure
  acquiredVia
  acquiredAt
  expiresAt
}

query GridOwnership($appId: BigInt!, $gridId: BigInt!) {
  gridOwnership(appId: $appId, gridId: $gridId) {
    ...GridOwnershipFields
  }
}

mutation AssignGridOwnership($input: AssignGridOwnershipInput!) {
  assignGridOwnership(input: $input) {
    ...GridOwnershipFields
  }
}

mutation TransferGridOwnership($input: TransferGridOwnershipInput!) {
  transferGridOwnership(input: $input) {
    ...GridOwnershipFields
  }
}

query GridUserPermissions($appId: BigInt!, $gridId: BigInt!, $userId: BigInt!) {
  gridUserPermissions(appId: $appId, gridId: $gridId, userId: $userId) {
    appId
    gridId
    userId
    permissionKeys
  }
}

query NearbyGridPermissions($input: NearbyGridPermissionsInput!) {
  nearbyGridPermissions(input: $input) {
    appId
    gridId
    userId
    lowChunk {
      x
      y
      z
    }
    highChunk {
      x
      y
      z
    }
    permissionKeys
  }
}

query GridPermissionLimits($appId: BigInt!, $gridId: BigInt!) {
  gridPermissionLimits(appId: $appId, gridId: $gridId) {
    appId
    gridId
    permissionKeys
  }
}

query GridGroupGrants($appId: BigInt!, $gridId: BigInt!, $groupId: BigInt!) {
  gridGroupGrants(appId: $appId, gridId: $gridId, groupId: $groupId) {
    appId
    gridId
    groupId
    groupRoleId
    permissionKey
    expiresAt
  }
}

mutation CreateGrid($input: CreateGridInput!) {
  createGrid(input: $input) {
    grid {
      grid_id
      app_id
      low_chunk {
        x
        y
        z
      }
      high_chunk {
        x
        y
        z
      }
    }
    error
  }
}

mutation DeleteGrid($input: DeleteGridInput!) {
  deleteGrid(input: $input) {
    gridId
    error
  }
}

mutation GrantGridPermissions($input: GrantGridPermissionsInput!) {
  grantGridPermissions(input: $input) {
    appId
    gridId
    userId
    permissionKeys
  }
}

mutation RevokeGridPermissions($input: RevokeGridPermissionsInput!) {
  revokeGridPermissions(input: $input) {
    appId
    gridId
    userId
    permissionKeys
  }
}

mutation SetGridPermissionLimits($input: SetGridPermissionLimitsInput!) {
  setGridPermissionLimits(input: $input) {
    appId
    gridId
    permissionKeys
  }
}

mutation AssignGroupToGrid($input: AssignGroupToGridInput!) {
  assignGroupToGrid(input: $input) {
    appId
    gridId
    groupId
    groupRoleId
    permissionKey
    expiresAt
  }
}

mutation RevokeGroupFromGrid($input: RevokeGroupFromGridInput!) {
  revokeGroupFromGrid(input: $input) {
    appId
    gridId
    groupId
    groupRoleId
    permissionKey
    expiresAt
  }
})gql";
inline constexpr std::string_view kGridOwnershipIsolatedDocument = R"gql(query GridOwnership($appId: BigInt!, $gridId: BigInt!) {
  gridOwnership(appId: $appId, gridId: $gridId) {
    ...GridOwnershipFields
  }
}

fragment GridOwnershipFields on GridOwnership {
  gridOwnershipId
  gridId
  appId
  ownerKind
  ownerRef
  tenure
  acquiredVia
  acquiredAt
  expiresAt
})gql";
inline constexpr std::string_view kGridOwnershipOperationName = "GridOwnership";
inline constexpr std::string_view kAssignGridOwnershipIsolatedDocument = R"gql(mutation AssignGridOwnership($input: AssignGridOwnershipInput!) {
  assignGridOwnership(input: $input) {
    ...GridOwnershipFields
  }
}

fragment GridOwnershipFields on GridOwnership {
  gridOwnershipId
  gridId
  appId
  ownerKind
  ownerRef
  tenure
  acquiredVia
  acquiredAt
  expiresAt
})gql";
inline constexpr std::string_view kAssignGridOwnershipOperationName = "AssignGridOwnership";
inline constexpr std::string_view kTransferGridOwnershipIsolatedDocument = R"gql(mutation TransferGridOwnership($input: TransferGridOwnershipInput!) {
  transferGridOwnership(input: $input) {
    ...GridOwnershipFields
  }
}

fragment GridOwnershipFields on GridOwnership {
  gridOwnershipId
  gridId
  appId
  ownerKind
  ownerRef
  tenure
  acquiredVia
  acquiredAt
  expiresAt
})gql";
inline constexpr std::string_view kTransferGridOwnershipOperationName = "TransferGridOwnership";
inline constexpr std::string_view kGridUserPermissionsIsolatedDocument = R"gql(query GridUserPermissions($appId: BigInt!, $gridId: BigInt!, $userId: BigInt!) {
  gridUserPermissions(appId: $appId, gridId: $gridId, userId: $userId) {
    appId
    gridId
    userId
    permissionKeys
  }
})gql";
inline constexpr std::string_view kGridUserPermissionsOperationName = "GridUserPermissions";
inline constexpr std::string_view kNearbyGridPermissionsIsolatedDocument = R"gql(query NearbyGridPermissions($input: NearbyGridPermissionsInput!) {
  nearbyGridPermissions(input: $input) {
    appId
    gridId
    userId
    lowChunk {
      x
      y
      z
    }
    highChunk {
      x
      y
      z
    }
    permissionKeys
  }
})gql";
inline constexpr std::string_view kNearbyGridPermissionsOperationName = "NearbyGridPermissions";
inline constexpr std::string_view kGridPermissionLimitsIsolatedDocument = R"gql(query GridPermissionLimits($appId: BigInt!, $gridId: BigInt!) {
  gridPermissionLimits(appId: $appId, gridId: $gridId) {
    appId
    gridId
    permissionKeys
  }
})gql";
inline constexpr std::string_view kGridPermissionLimitsOperationName = "GridPermissionLimits";
inline constexpr std::string_view kGridGroupGrantsIsolatedDocument = R"gql(query GridGroupGrants($appId: BigInt!, $gridId: BigInt!, $groupId: BigInt!) {
  gridGroupGrants(appId: $appId, gridId: $gridId, groupId: $groupId) {
    appId
    gridId
    groupId
    groupRoleId
    permissionKey
    expiresAt
  }
})gql";
inline constexpr std::string_view kGridGroupGrantsOperationName = "GridGroupGrants";
inline constexpr std::string_view kCreateGridIsolatedDocument = R"gql(mutation CreateGrid($input: CreateGridInput!) {
  createGrid(input: $input) {
    grid {
      grid_id
      app_id
      low_chunk {
        x
        y
        z
      }
      high_chunk {
        x
        y
        z
      }
    }
    error
  }
})gql";
inline constexpr std::string_view kCreateGridOperationName = "CreateGrid";
inline constexpr std::string_view kDeleteGridIsolatedDocument = R"gql(mutation DeleteGrid($input: DeleteGridInput!) {
  deleteGrid(input: $input) {
    gridId
    error
  }
})gql";
inline constexpr std::string_view kDeleteGridOperationName = "DeleteGrid";
inline constexpr std::string_view kGrantGridPermissionsIsolatedDocument = R"gql(mutation GrantGridPermissions($input: GrantGridPermissionsInput!) {
  grantGridPermissions(input: $input) {
    appId
    gridId
    userId
    permissionKeys
  }
})gql";
inline constexpr std::string_view kGrantGridPermissionsOperationName = "GrantGridPermissions";
inline constexpr std::string_view kRevokeGridPermissionsIsolatedDocument = R"gql(mutation RevokeGridPermissions($input: RevokeGridPermissionsInput!) {
  revokeGridPermissions(input: $input) {
    appId
    gridId
    userId
    permissionKeys
  }
})gql";
inline constexpr std::string_view kRevokeGridPermissionsOperationName = "RevokeGridPermissions";
inline constexpr std::string_view kSetGridPermissionLimitsIsolatedDocument = R"gql(mutation SetGridPermissionLimits($input: SetGridPermissionLimitsInput!) {
  setGridPermissionLimits(input: $input) {
    appId
    gridId
    permissionKeys
  }
})gql";
inline constexpr std::string_view kSetGridPermissionLimitsOperationName = "SetGridPermissionLimits";
inline constexpr std::string_view kAssignGroupToGridIsolatedDocument = R"gql(mutation AssignGroupToGrid($input: AssignGroupToGridInput!) {
  assignGroupToGrid(input: $input) {
    appId
    gridId
    groupId
    groupRoleId
    permissionKey
    expiresAt
  }
})gql";
inline constexpr std::string_view kAssignGroupToGridOperationName = "AssignGroupToGrid";
inline constexpr std::string_view kRevokeGroupFromGridIsolatedDocument = R"gql(mutation RevokeGroupFromGrid($input: RevokeGroupFromGridInput!) {
  revokeGroupFromGrid(input: $input) {
    appId
    gridId
    groupId
    groupRoleId
    permissionKey
    expiresAt
  }
})gql";
inline constexpr std::string_view kRevokeGroupFromGridOperationName = "RevokeGroupFromGrid";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "GridOwnership") return kGridOwnershipIsolatedDocument;
  if (operationName == "AssignGridOwnership") return kAssignGridOwnershipIsolatedDocument;
  if (operationName == "TransferGridOwnership") return kTransferGridOwnershipIsolatedDocument;
  if (operationName == "GridUserPermissions") return kGridUserPermissionsIsolatedDocument;
  if (operationName == "NearbyGridPermissions") return kNearbyGridPermissionsIsolatedDocument;
  if (operationName == "GridPermissionLimits") return kGridPermissionLimitsIsolatedDocument;
  if (operationName == "GridGroupGrants") return kGridGroupGrantsIsolatedDocument;
  if (operationName == "CreateGrid") return kCreateGridIsolatedDocument;
  if (operationName == "DeleteGrid") return kDeleteGridIsolatedDocument;
  if (operationName == "GrantGridPermissions") return kGrantGridPermissionsIsolatedDocument;
  if (operationName == "RevokeGridPermissions") return kRevokeGridPermissionsIsolatedDocument;
  if (operationName == "SetGridPermissionLimits") return kSetGridPermissionLimitsIsolatedDocument;
  if (operationName == "AssignGroupToGrid") return kAssignGroupToGridIsolatedDocument;
  if (operationName == "RevokeGroupFromGrid") return kRevokeGroupFromGridIsolatedDocument;
  return {};
}

}  // namespace gameApps

namespace gameModel {

/// gameModel/GameModelAutomations.graphql
inline constexpr std::string_view kGameModelAutomationsDocument = R"gql(fragment GmAutomationFields on GmAutomation {
  automationId
  appId
  name
  description
  enabled
  actionKind
  functionName
  computeModuleName
  computeExport
  targetMode
  selfContainerId
  targetTypeName
  sessionId
  paramsJson
  selectorJson
  runAsUserId
  triggerType
  scheduleKind
  intervalMs
  cronExpr
  maxTargets
  maxFnDepth
  gasLimit
  runTimeoutMs
  maxRunsPerMinute
  failureThreshold
  cooldownMs
  circuitState
  consecutiveFailures
  pausedUntil
  lastError
  lastRunAt
  nextRunAt
}

fragment GmAutomationTriggerFields on GmAutomationTrigger {
  triggerId
  appId
  automationId
  onEvent
  functionName
  containerTypeName
  propertyKey
  writeSource
  debounceMs
  lastMatchedAt
  matchCount24h
  warnings
}

fragment GmAutomationPolicyFields on GmAutomationPolicy {
  appId
  enabled
  maxAutomations
  minIntervalMs
  maxFanout
  maxCascadeDepth
  globalRunsPerMinute
  minTimerDelayMs
  maxPendingTimers
}

fragment GmAutomationRunFields on GmAutomationRun {
  runId
  appId
  flowId
  automationId
  automationName
  triggerSource
  triggerId
  parentRunId
  cascadeDepth
  startedAt
  finishedAt
  durationUs
  targets
  invocations
  mutations
  fnCalls
  gasUsed
  success
  errorMessage
  circuitAction
  computeUnits
}

mutation GameModelUpsertAutomation($input: UpsertAutomationInput!) {
  gameModelUpsertAutomation(input: $input) {
    ...GmAutomationFields
  }
}

mutation GameModelDeleteAutomation($appId: BigInt!, $name: String!) {
  gameModelDeleteAutomation(appId: $appId, name: $name)
}

mutation GameModelSetAutomationEnabled($appId: BigInt!, $name: String!, $enabled: Boolean!) {
  gameModelSetAutomationEnabled(appId: $appId, name: $name, enabled: $enabled) {
    ...GmAutomationFields
  }
}

mutation GameModelUpsertAutomationTrigger($input: UpsertAutomationTriggerInput!) {
  gameModelUpsertAutomationTrigger(input: $input) {
    ...GmAutomationTriggerFields
  }
}

mutation GameModelDeleteAutomationTrigger($appId: BigInt!, $triggerId: String!) {
  gameModelDeleteAutomationTrigger(appId: $appId, triggerId: $triggerId)
}

mutation GameModelSetAutomationPolicy($input: SetAutomationPolicyInput!) {
  gameModelSetAutomationPolicy(input: $input) {
    ...GmAutomationPolicyFields
  }
}

mutation GameModelRunAutomation($appId: BigInt!, $name: String!) {
  gameModelRunAutomation(appId: $appId, name: $name) {
    ...GmAutomationRunFields
  }
}

query GameModelAutomations($appId: BigInt!) {
  gameModelAutomations(appId: $appId) {
    ...GmAutomationFields
  }
}

query GameModelAutomation($appId: BigInt!, $name: String!) {
  gameModelAutomation(appId: $appId, name: $name) {
    ...GmAutomationFields
  }
}

query GameModelAutomationTriggers($appId: BigInt!, $automationName: String) {
  gameModelAutomationTriggers(appId: $appId, automationName: $automationName) {
    ...GmAutomationTriggerFields
  }
}

query GameModelAutomationPolicy($appId: BigInt!) {
  gameModelAutomationPolicy(appId: $appId) {
    ...GmAutomationPolicyFields
  }
}

query GameModelAutomationRuns(
  $appId: BigInt!
  $automationName: String
  $success: Boolean
  $limit: Int
  $offset: Int
) {
  gameModelAutomationRuns(
    appId: $appId
    automationName: $automationName
    success: $success
    limit: $limit
    offset: $offset
  ) {
    ...GmAutomationRunFields
  }
}

query GameModelAutomationStats($appId: BigInt!, $windowMinutes: Int) {
  gameModelAutomationStats(appId: $appId, windowMinutes: $windowMinutes) {
    windowMinutes
    totalRuns
    failedRuns
    failureRatePct
    runsPerMinute
    totalInvocations
    totalMutations
    totalComputeUnits
    avgDurationUs
    byAutomation {
      automationName
      runs
      failures
      invocations
      computeUnits
      avgDurationUs
      circuitState
    }
  }
}

query GameModelAppDiagnostics($appId: BigInt!) {
  gameModelAppDiagnostics(appId: $appId) {
    appId
    containerCount
    propertyCount
    edgeCount
    sessionCount
    functionCount
    automationCount
    eventCount
    events24h
    failedEvents24h
    automationEvents24h
    notificationsEmitted24h
    notificationsUndeliverable24h
    topFunctions {
      functionName
      invocations
      failures
    }
  }
}

fragment GmTimerFields on GmTimer {
  timerId
  appId
  sessionId
  selfContainerId
  functionName
  paramsJson
  fireAt
  dedupeKey
  cascadeDepth
  flowId
  armedBy
  createdAt
}

mutation GameModelScheduleInvoke($input: ScheduleInvokeInput!) {
  gameModelScheduleInvoke(input: $input) {
    ...GmTimerFields
  }
}

mutation GameModelCancelTimer($appId: BigInt!, $timerId: String, $dedupeKey: String) {
  gameModelCancelTimer(appId: $appId, timerId: $timerId, dedupeKey: $dedupeKey)
}

query GameModelTimers($appId: BigInt!, $sessionId: String, $limit: Int) {
  gameModelTimers(appId: $appId, sessionId: $sessionId, limit: $limit) {
    ...GmTimerFields
  }
})gql";
inline constexpr std::string_view kGameModelUpsertAutomationIsolatedDocument = R"gql(mutation GameModelUpsertAutomation($input: UpsertAutomationInput!) {
  gameModelUpsertAutomation(input: $input) {
    ...GmAutomationFields
  }
}

fragment GmAutomationFields on GmAutomation {
  automationId
  appId
  name
  description
  enabled
  actionKind
  functionName
  computeModuleName
  computeExport
  targetMode
  selfContainerId
  targetTypeName
  sessionId
  paramsJson
  selectorJson
  runAsUserId
  triggerType
  scheduleKind
  intervalMs
  cronExpr
  maxTargets
  maxFnDepth
  gasLimit
  runTimeoutMs
  maxRunsPerMinute
  failureThreshold
  cooldownMs
  circuitState
  consecutiveFailures
  pausedUntil
  lastError
  lastRunAt
  nextRunAt
})gql";
inline constexpr std::string_view kGameModelUpsertAutomationOperationName = "GameModelUpsertAutomation";
inline constexpr std::string_view kGameModelDeleteAutomationIsolatedDocument = R"gql(mutation GameModelDeleteAutomation($appId: BigInt!, $name: String!) {
  gameModelDeleteAutomation(appId: $appId, name: $name)
})gql";
inline constexpr std::string_view kGameModelDeleteAutomationOperationName = "GameModelDeleteAutomation";
inline constexpr std::string_view kGameModelSetAutomationEnabledIsolatedDocument = R"gql(mutation GameModelSetAutomationEnabled($appId: BigInt!, $name: String!, $enabled: Boolean!) {
  gameModelSetAutomationEnabled(appId: $appId, name: $name, enabled: $enabled) {
    ...GmAutomationFields
  }
}

fragment GmAutomationFields on GmAutomation {
  automationId
  appId
  name
  description
  enabled
  actionKind
  functionName
  computeModuleName
  computeExport
  targetMode
  selfContainerId
  targetTypeName
  sessionId
  paramsJson
  selectorJson
  runAsUserId
  triggerType
  scheduleKind
  intervalMs
  cronExpr
  maxTargets
  maxFnDepth
  gasLimit
  runTimeoutMs
  maxRunsPerMinute
  failureThreshold
  cooldownMs
  circuitState
  consecutiveFailures
  pausedUntil
  lastError
  lastRunAt
  nextRunAt
})gql";
inline constexpr std::string_view kGameModelSetAutomationEnabledOperationName = "GameModelSetAutomationEnabled";
inline constexpr std::string_view kGameModelUpsertAutomationTriggerIsolatedDocument = R"gql(mutation GameModelUpsertAutomationTrigger($input: UpsertAutomationTriggerInput!) {
  gameModelUpsertAutomationTrigger(input: $input) {
    ...GmAutomationTriggerFields
  }
}

fragment GmAutomationTriggerFields on GmAutomationTrigger {
  triggerId
  appId
  automationId
  onEvent
  functionName
  containerTypeName
  propertyKey
  writeSource
  debounceMs
  lastMatchedAt
  matchCount24h
  warnings
})gql";
inline constexpr std::string_view kGameModelUpsertAutomationTriggerOperationName = "GameModelUpsertAutomationTrigger";
inline constexpr std::string_view kGameModelDeleteAutomationTriggerIsolatedDocument = R"gql(mutation GameModelDeleteAutomationTrigger($appId: BigInt!, $triggerId: String!) {
  gameModelDeleteAutomationTrigger(appId: $appId, triggerId: $triggerId)
})gql";
inline constexpr std::string_view kGameModelDeleteAutomationTriggerOperationName = "GameModelDeleteAutomationTrigger";
inline constexpr std::string_view kGameModelSetAutomationPolicyIsolatedDocument = R"gql(mutation GameModelSetAutomationPolicy($input: SetAutomationPolicyInput!) {
  gameModelSetAutomationPolicy(input: $input) {
    ...GmAutomationPolicyFields
  }
}

fragment GmAutomationPolicyFields on GmAutomationPolicy {
  appId
  enabled
  maxAutomations
  minIntervalMs
  maxFanout
  maxCascadeDepth
  globalRunsPerMinute
  minTimerDelayMs
  maxPendingTimers
})gql";
inline constexpr std::string_view kGameModelSetAutomationPolicyOperationName = "GameModelSetAutomationPolicy";
inline constexpr std::string_view kGameModelRunAutomationIsolatedDocument = R"gql(mutation GameModelRunAutomation($appId: BigInt!, $name: String!) {
  gameModelRunAutomation(appId: $appId, name: $name) {
    ...GmAutomationRunFields
  }
}

fragment GmAutomationRunFields on GmAutomationRun {
  runId
  appId
  flowId
  automationId
  automationName
  triggerSource
  triggerId
  parentRunId
  cascadeDepth
  startedAt
  finishedAt
  durationUs
  targets
  invocations
  mutations
  fnCalls
  gasUsed
  success
  errorMessage
  circuitAction
  computeUnits
})gql";
inline constexpr std::string_view kGameModelRunAutomationOperationName = "GameModelRunAutomation";
inline constexpr std::string_view kGameModelAutomationsIsolatedDocument = R"gql(query GameModelAutomations($appId: BigInt!) {
  gameModelAutomations(appId: $appId) {
    ...GmAutomationFields
  }
}

fragment GmAutomationFields on GmAutomation {
  automationId
  appId
  name
  description
  enabled
  actionKind
  functionName
  computeModuleName
  computeExport
  targetMode
  selfContainerId
  targetTypeName
  sessionId
  paramsJson
  selectorJson
  runAsUserId
  triggerType
  scheduleKind
  intervalMs
  cronExpr
  maxTargets
  maxFnDepth
  gasLimit
  runTimeoutMs
  maxRunsPerMinute
  failureThreshold
  cooldownMs
  circuitState
  consecutiveFailures
  pausedUntil
  lastError
  lastRunAt
  nextRunAt
})gql";
inline constexpr std::string_view kGameModelAutomationsOperationName = "GameModelAutomations";
inline constexpr std::string_view kGameModelAutomationIsolatedDocument = R"gql(query GameModelAutomation($appId: BigInt!, $name: String!) {
  gameModelAutomation(appId: $appId, name: $name) {
    ...GmAutomationFields
  }
}

fragment GmAutomationFields on GmAutomation {
  automationId
  appId
  name
  description
  enabled
  actionKind
  functionName
  computeModuleName
  computeExport
  targetMode
  selfContainerId
  targetTypeName
  sessionId
  paramsJson
  selectorJson
  runAsUserId
  triggerType
  scheduleKind
  intervalMs
  cronExpr
  maxTargets
  maxFnDepth
  gasLimit
  runTimeoutMs
  maxRunsPerMinute
  failureThreshold
  cooldownMs
  circuitState
  consecutiveFailures
  pausedUntil
  lastError
  lastRunAt
  nextRunAt
})gql";
inline constexpr std::string_view kGameModelAutomationOperationName = "GameModelAutomation";
inline constexpr std::string_view kGameModelAutomationTriggersIsolatedDocument = R"gql(query GameModelAutomationTriggers($appId: BigInt!, $automationName: String) {
  gameModelAutomationTriggers(appId: $appId, automationName: $automationName) {
    ...GmAutomationTriggerFields
  }
}

fragment GmAutomationTriggerFields on GmAutomationTrigger {
  triggerId
  appId
  automationId
  onEvent
  functionName
  containerTypeName
  propertyKey
  writeSource
  debounceMs
  lastMatchedAt
  matchCount24h
  warnings
})gql";
inline constexpr std::string_view kGameModelAutomationTriggersOperationName = "GameModelAutomationTriggers";
inline constexpr std::string_view kGameModelAutomationPolicyIsolatedDocument = R"gql(query GameModelAutomationPolicy($appId: BigInt!) {
  gameModelAutomationPolicy(appId: $appId) {
    ...GmAutomationPolicyFields
  }
}

fragment GmAutomationPolicyFields on GmAutomationPolicy {
  appId
  enabled
  maxAutomations
  minIntervalMs
  maxFanout
  maxCascadeDepth
  globalRunsPerMinute
  minTimerDelayMs
  maxPendingTimers
})gql";
inline constexpr std::string_view kGameModelAutomationPolicyOperationName = "GameModelAutomationPolicy";
inline constexpr std::string_view kGameModelAutomationRunsIsolatedDocument = R"gql(query GameModelAutomationRuns($appId: BigInt!, $automationName: String, $success: Boolean, $limit: Int, $offset: Int) {
  gameModelAutomationRuns(
    appId: $appId
    automationName: $automationName
    success: $success
    limit: $limit
    offset: $offset
  ) {
    ...GmAutomationRunFields
  }
}

fragment GmAutomationRunFields on GmAutomationRun {
  runId
  appId
  flowId
  automationId
  automationName
  triggerSource
  triggerId
  parentRunId
  cascadeDepth
  startedAt
  finishedAt
  durationUs
  targets
  invocations
  mutations
  fnCalls
  gasUsed
  success
  errorMessage
  circuitAction
  computeUnits
})gql";
inline constexpr std::string_view kGameModelAutomationRunsOperationName = "GameModelAutomationRuns";
inline constexpr std::string_view kGameModelAutomationStatsIsolatedDocument = R"gql(query GameModelAutomationStats($appId: BigInt!, $windowMinutes: Int) {
  gameModelAutomationStats(appId: $appId, windowMinutes: $windowMinutes) {
    windowMinutes
    totalRuns
    failedRuns
    failureRatePct
    runsPerMinute
    totalInvocations
    totalMutations
    totalComputeUnits
    avgDurationUs
    byAutomation {
      automationName
      runs
      failures
      invocations
      computeUnits
      avgDurationUs
      circuitState
    }
  }
})gql";
inline constexpr std::string_view kGameModelAutomationStatsOperationName = "GameModelAutomationStats";
inline constexpr std::string_view kGameModelAppDiagnosticsIsolatedDocument = R"gql(query GameModelAppDiagnostics($appId: BigInt!) {
  gameModelAppDiagnostics(appId: $appId) {
    appId
    containerCount
    propertyCount
    edgeCount
    sessionCount
    functionCount
    automationCount
    eventCount
    events24h
    failedEvents24h
    automationEvents24h
    notificationsEmitted24h
    notificationsUndeliverable24h
    topFunctions {
      functionName
      invocations
      failures
    }
  }
})gql";
inline constexpr std::string_view kGameModelAppDiagnosticsOperationName = "GameModelAppDiagnostics";
inline constexpr std::string_view kGameModelScheduleInvokeIsolatedDocument = R"gql(mutation GameModelScheduleInvoke($input: ScheduleInvokeInput!) {
  gameModelScheduleInvoke(input: $input) {
    ...GmTimerFields
  }
}

fragment GmTimerFields on GmTimer {
  timerId
  appId
  sessionId
  selfContainerId
  functionName
  paramsJson
  fireAt
  dedupeKey
  cascadeDepth
  flowId
  armedBy
  createdAt
})gql";
inline constexpr std::string_view kGameModelScheduleInvokeOperationName = "GameModelScheduleInvoke";
inline constexpr std::string_view kGameModelCancelTimerIsolatedDocument = R"gql(mutation GameModelCancelTimer($appId: BigInt!, $timerId: String, $dedupeKey: String) {
  gameModelCancelTimer(appId: $appId, timerId: $timerId, dedupeKey: $dedupeKey)
})gql";
inline constexpr std::string_view kGameModelCancelTimerOperationName = "GameModelCancelTimer";
inline constexpr std::string_view kGameModelTimersIsolatedDocument = R"gql(query GameModelTimers($appId: BigInt!, $sessionId: String, $limit: Int) {
  gameModelTimers(appId: $appId, sessionId: $sessionId, limit: $limit) {
    ...GmTimerFields
  }
}

fragment GmTimerFields on GmTimer {
  timerId
  appId
  sessionId
  selfContainerId
  functionName
  paramsJson
  fireAt
  dedupeKey
  cascadeDepth
  flowId
  armedBy
  createdAt
})gql";
inline constexpr std::string_view kGameModelTimersOperationName = "GameModelTimers";

/// gameModel/GameModelFunctionCircuits.graphql
inline constexpr std::string_view kGameModelFunctionCircuitsDocument = R"gql(query GameModelFunctionCircuits($appId: BigInt!, $name: String, $limit: Int) {
  gameModelFunctionCircuits(appId: $appId, name: $name, limit: $limit) {
    mode
    failureThreshold
    cooldownMs
    circuits {
      appId
      functionName
      circuitState
      consecutiveFailures
      cooldownUntil
      openedAt
      totalOpens
      shadowRefusals
      updatedAt
    }
  }
})gql";
inline constexpr std::string_view kGameModelFunctionCircuitsIsolatedDocument = R"gql(query GameModelFunctionCircuits($appId: BigInt!, $name: String, $limit: Int) {
  gameModelFunctionCircuits(appId: $appId, name: $name, limit: $limit) {
    mode
    failureThreshold
    cooldownMs
    circuits {
      appId
      functionName
      circuitState
      consecutiveFailures
      cooldownUntil
      openedAt
      totalOpens
      shadowRefusals
      updatedAt
    }
  }
})gql";
inline constexpr std::string_view kGameModelFunctionCircuitsOperationName = "GameModelFunctionCircuits";

/// gameModel/GameModelLint.graphql
inline constexpr std::string_view kGameModelLintDocument = R"gql(# The studio lint query. It lived as an inline string constant in
# include/crowdy/studio/model_lint.hpp, which meant the codegen operation-isolation test
# could never validate it against the schema -- the one query asking "is this app's model
# coherent" was itself unchecked. Mirrors CrowdyJS's GameModelLint.graphql field for field.
query CrowdyModelLint($appId: BigInt!) {
  gameModelLint(appId: $appId) {
    appId
    errorCount
    warningCount
    clean
    findings {
      code
      severity
      subjectKind
      subject
      message
      remedy
      count
    }
  }
})gql";
inline constexpr std::string_view kCrowdyModelLintIsolatedDocument = R"gql(query CrowdyModelLint($appId: BigInt!) {
  gameModelLint(appId: $appId) {
    appId
    errorCount
    warningCount
    clean
    findings {
      code
      severity
      subjectKind
      subject
      message
      remedy
      count
    }
  }
})gql";
inline constexpr std::string_view kCrowdyModelLintOperationName = "CrowdyModelLint";

/// gameModel/GameModelRuntime.graphql
inline constexpr std::string_view kGameModelRuntimeDocument = R"gql(fragment GmSessionFields on GmSession {
  sessionId
  appId
  name
  status
  createdByUserId
  currentTurnUserId
  metadataJson
}

fragment GmContainerFields on GmContainer {
  containerId
  appId
  sessionId
  typeName
  displayName
  description
  ownerUserId
  metadataJson
  bindingKey
}

fragment GmInvokeResultFields on GmInvokeResult {
  eventId
  functionName
  success
  returnValueJson
  errorMessage
  fault {
    code
    blame
    retryable
  }
  mutationsApplied {
    containerId
    key
    valueType
    oldValueJson
    newValueJson
  }
}

mutation GameModelCreateSession($input: CreateSessionInput!) {
  gameModelCreateSession(input: $input) {
    ...GmSessionFields
  }
}

mutation GameModelJoinSession($input: JoinSessionInput!) {
  gameModelJoinSession(input: $input) {
    sessionId
    userId
    role
  }
}

mutation GameModelSetSessionTurn($input: SetSessionTurnInput!) {
  gameModelSetSessionTurn(input: $input) {
    ...GmSessionFields
  }
}

mutation GameModelCreateContainer($input: CreateContainerInput!) {
  gameModelCreateContainer(input: $input) {
    ...GmContainerFields
  }
}

mutation GameModelEnsureContainer($input: EnsureContainerInput!) {
  gameModelEnsureContainer(input: $input) {
    container {
      ...GmContainerFields
    }
    created
  }
}

mutation GameModelDeleteContainer($appId: BigInt!, $containerId: String!) {
  gameModelDeleteContainer(appId: $appId, containerId: $containerId)
}

mutation GameModelSetProperty($input: SetContainerPropertyInput!) {
  gameModelSetProperty(input: $input) {
    ...GmContainerFields
  }
}

mutation GameModelAddEdge($input: AddEdgeInput!) {
  gameModelAddEdge(input: $input) {
    edgeId
    fromContainerId
    toContainerId
    relationshipType
    weight
  }
}

mutation GameModelDeleteEdge($appId: BigInt!, $edgeId: String!) {
  gameModelDeleteEdge(appId: $appId, edgeId: $edgeId)
}

mutation GameModelInvoke($input: InvokeFunctionInput!) {
  gameModelInvoke(input: $input) {
    ...GmInvokeResultFields
  }
}

query GameModelContainer($appId: BigInt!, $containerId: String!) {
  gameModelContainer(appId: $appId, containerId: $containerId) {
    ...GmContainerFields
  }
}

query GameModelContainers(
  $appId: BigInt!
  $typeName: String
  $sessionId: String
  $bindingKey: String
  $where: [GmPropertyPredicateInput!]
  $limit: Int
  $offset: Int
) {
  gameModelContainers(
    appId: $appId
    typeName: $typeName
    sessionId: $sessionId
    bindingKey: $bindingKey
    where: $where
    limit: $limit
    offset: $offset
  ) {
    ...GmContainerFields
  }
}

query GameModelContainerState($appId: BigInt!, $containerId: String!) {
  gameModelContainerState(appId: $appId, containerId: $containerId) {
    containerId
    appId
    sessionId
    typeName
    displayName
    ownerUserId
    propertiesJson
  }
}

query GameModelTraverse(
  $appId: BigInt!
  $rootId: String!
  $relationshipType: String!
  $depth: Int
) {
  gameModelTraverse(
    appId: $appId
    rootId: $rootId
    relationshipType: $relationshipType
    depth: $depth
  ) {
    rootId
    nodes {
      ...GmContainerFields
    }
    edges {
      edgeId
      fromContainerId
      toContainerId
      relationshipType
      weight
    }
  }
}

query GameModelSession($appId: BigInt!, $sessionId: String!) {
  gameModelSession(appId: $appId, sessionId: $sessionId) {
    ...GmSessionFields
  }
}

query GameModelSessions($appId: BigInt!, $status: String) {
  gameModelSessions(appId: $appId, status: $status) {
    ...GmSessionFields
  }
}

query GameModelEvents(
  $appId: BigInt!
  $sessionId: String
  $selfContainerId: String
  $functionName: String
  $success: Boolean
  $limit: Int
  $offset: Int
) {
  gameModelEvents(
    appId: $appId
    sessionId: $sessionId
    selfContainerId: $selfContainerId
    functionName: $functionName
    success: $success
    limit: $limit
    offset: $offset
  ) {
    eventId
    flowId
    sessionId
    functionName
    selfContainerId
    callerUserId
    callerKind
    automationId
    paramsJson
    mutationsAppliedJson
    permissionEffectsAppliedJson
    returnValueJson
    success
    errorMessage
    executedAt
  }
}

query GameModelEventsConnection(
  $appId: BigInt!
  $first: Int
  $after: String
  $sessionId: String
  $selfContainerId: String
  $functionName: String
  $success: Boolean
) {
  gameModelEventsConnection(
    appId: $appId
    first: $first
    after: $after
    sessionId: $sessionId
    selfContainerId: $selfContainerId
    functionName: $functionName
    success: $success
  ) {
    edges {
      cursor
      node {
        eventId
        flowId
        sessionId
        functionName
        selfContainerId
        callerUserId
        callerKind
        automationId
        paramsJson
        mutationsAppliedJson
        permissionEffectsAppliedJson
        returnValueJson
        success
        errorMessage
        executedAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
}

query GameModelActivePlayerCount($appId: BigInt!) {
  gameModelActivePlayerCount(appId: $appId) {
    appId
    activePlayerCount
    status
    observedAt
    revision
  }
}

# Cross-engine flow timeline (diagnostics; requires manage_apps and a
# game-api with gameModelFlow, 2026-07-19+). Selections are inlined because
# each operations file is shipped as one self-contained document.
query GameModelFlow($appId: BigInt!, $flowId: String!) {
  gameModelFlow(appId: $appId, flowId: $flowId) {
    flowId
    events {
      eventId
      flowId
      sessionId
      functionName
      selfContainerId
      callerUserId
      callerKind
      automationId
      paramsJson
      mutationsAppliedJson
      permissionEffectsAppliedJson
      returnValueJson
      success
      errorMessage
      executedAt
    }
    automationRuns {
      runId
      appId
      flowId
      automationId
      automationName
      triggerSource
      parentRunId
      cascadeDepth
      startedAt
      finishedAt
      durationUs
      targets
      invocations
      mutations
      fnCalls
      gasUsed
      success
      errorMessage
      circuitAction
      computeUnits
    }
    moduleRuns {
      runId
      appId
      flowId
      moduleId
      moduleName
      triggerSource
      entry
      startedAt
      durationUs
      fuelUsed
      dbReads
      dbWrites
      egressMsgs
      egressBytes
      success
      errorMessage
      circuitAction
    }
  }
}

subscription GameModelActivePlayerCountChanged($appId: BigInt!) {
  gameModelActivePlayerCountChanged(appId: $appId) {
    appId
    previousCount
    currentCount
    delta
    revision
    observedAt
  }
}

subscription GameModelContainerChanged(
  $appId: BigInt!
  $typeName: String
  $sessionId: String
) {
  gameModelContainerChanged(
    appId: $appId
    typeName: $typeName
    sessionId: $sessionId
  ) {
    appId
    containerId
    typeName
    sessionId
    source
    functionName
    changedKeys
    occurredAt
  }
})gql";
inline constexpr std::string_view kGameModelCreateSessionIsolatedDocument = R"gql(mutation GameModelCreateSession($input: CreateSessionInput!) {
  gameModelCreateSession(input: $input) {
    ...GmSessionFields
  }
}

fragment GmSessionFields on GmSession {
  sessionId
  appId
  name
  status
  createdByUserId
  currentTurnUserId
  metadataJson
})gql";
inline constexpr std::string_view kGameModelCreateSessionOperationName = "GameModelCreateSession";
inline constexpr std::string_view kGameModelJoinSessionIsolatedDocument = R"gql(mutation GameModelJoinSession($input: JoinSessionInput!) {
  gameModelJoinSession(input: $input) {
    sessionId
    userId
    role
  }
})gql";
inline constexpr std::string_view kGameModelJoinSessionOperationName = "GameModelJoinSession";
inline constexpr std::string_view kGameModelSetSessionTurnIsolatedDocument = R"gql(mutation GameModelSetSessionTurn($input: SetSessionTurnInput!) {
  gameModelSetSessionTurn(input: $input) {
    ...GmSessionFields
  }
}

fragment GmSessionFields on GmSession {
  sessionId
  appId
  name
  status
  createdByUserId
  currentTurnUserId
  metadataJson
})gql";
inline constexpr std::string_view kGameModelSetSessionTurnOperationName = "GameModelSetSessionTurn";
inline constexpr std::string_view kGameModelCreateContainerIsolatedDocument = R"gql(mutation GameModelCreateContainer($input: CreateContainerInput!) {
  gameModelCreateContainer(input: $input) {
    ...GmContainerFields
  }
}

fragment GmContainerFields on GmContainer {
  containerId
  appId
  sessionId
  typeName
  displayName
  description
  ownerUserId
  metadataJson
  bindingKey
})gql";
inline constexpr std::string_view kGameModelCreateContainerOperationName = "GameModelCreateContainer";
inline constexpr std::string_view kGameModelEnsureContainerIsolatedDocument = R"gql(mutation GameModelEnsureContainer($input: EnsureContainerInput!) {
  gameModelEnsureContainer(input: $input) {
    container {
      ...GmContainerFields
    }
    created
  }
}

fragment GmContainerFields on GmContainer {
  containerId
  appId
  sessionId
  typeName
  displayName
  description
  ownerUserId
  metadataJson
  bindingKey
})gql";
inline constexpr std::string_view kGameModelEnsureContainerOperationName = "GameModelEnsureContainer";
inline constexpr std::string_view kGameModelDeleteContainerIsolatedDocument = R"gql(mutation GameModelDeleteContainer($appId: BigInt!, $containerId: String!) {
  gameModelDeleteContainer(appId: $appId, containerId: $containerId)
})gql";
inline constexpr std::string_view kGameModelDeleteContainerOperationName = "GameModelDeleteContainer";
inline constexpr std::string_view kGameModelSetPropertyIsolatedDocument = R"gql(mutation GameModelSetProperty($input: SetContainerPropertyInput!) {
  gameModelSetProperty(input: $input) {
    ...GmContainerFields
  }
}

fragment GmContainerFields on GmContainer {
  containerId
  appId
  sessionId
  typeName
  displayName
  description
  ownerUserId
  metadataJson
  bindingKey
})gql";
inline constexpr std::string_view kGameModelSetPropertyOperationName = "GameModelSetProperty";
inline constexpr std::string_view kGameModelAddEdgeIsolatedDocument = R"gql(mutation GameModelAddEdge($input: AddEdgeInput!) {
  gameModelAddEdge(input: $input) {
    edgeId
    fromContainerId
    toContainerId
    relationshipType
    weight
  }
})gql";
inline constexpr std::string_view kGameModelAddEdgeOperationName = "GameModelAddEdge";
inline constexpr std::string_view kGameModelDeleteEdgeIsolatedDocument = R"gql(mutation GameModelDeleteEdge($appId: BigInt!, $edgeId: String!) {
  gameModelDeleteEdge(appId: $appId, edgeId: $edgeId)
})gql";
inline constexpr std::string_view kGameModelDeleteEdgeOperationName = "GameModelDeleteEdge";
inline constexpr std::string_view kGameModelInvokeIsolatedDocument = R"gql(mutation GameModelInvoke($input: InvokeFunctionInput!) {
  gameModelInvoke(input: $input) {
    ...GmInvokeResultFields
  }
}

fragment GmInvokeResultFields on GmInvokeResult {
  eventId
  functionName
  success
  returnValueJson
  errorMessage
  fault {
    code
    blame
    retryable
  }
  mutationsApplied {
    containerId
    key
    valueType
    oldValueJson
    newValueJson
  }
})gql";
inline constexpr std::string_view kGameModelInvokeOperationName = "GameModelInvoke";
inline constexpr std::string_view kGameModelContainerIsolatedDocument = R"gql(query GameModelContainer($appId: BigInt!, $containerId: String!) {
  gameModelContainer(appId: $appId, containerId: $containerId) {
    ...GmContainerFields
  }
}

fragment GmContainerFields on GmContainer {
  containerId
  appId
  sessionId
  typeName
  displayName
  description
  ownerUserId
  metadataJson
  bindingKey
})gql";
inline constexpr std::string_view kGameModelContainerOperationName = "GameModelContainer";
inline constexpr std::string_view kGameModelContainersIsolatedDocument = R"gql(query GameModelContainers($appId: BigInt!, $typeName: String, $sessionId: String, $bindingKey: String, $where: [GmPropertyPredicateInput!], $limit: Int, $offset: Int) {
  gameModelContainers(
    appId: $appId
    typeName: $typeName
    sessionId: $sessionId
    bindingKey: $bindingKey
    where: $where
    limit: $limit
    offset: $offset
  ) {
    ...GmContainerFields
  }
}

fragment GmContainerFields on GmContainer {
  containerId
  appId
  sessionId
  typeName
  displayName
  description
  ownerUserId
  metadataJson
  bindingKey
})gql";
inline constexpr std::string_view kGameModelContainersOperationName = "GameModelContainers";
inline constexpr std::string_view kGameModelContainerStateIsolatedDocument = R"gql(query GameModelContainerState($appId: BigInt!, $containerId: String!) {
  gameModelContainerState(appId: $appId, containerId: $containerId) {
    containerId
    appId
    sessionId
    typeName
    displayName
    ownerUserId
    propertiesJson
  }
})gql";
inline constexpr std::string_view kGameModelContainerStateOperationName = "GameModelContainerState";
inline constexpr std::string_view kGameModelTraverseIsolatedDocument = R"gql(query GameModelTraverse($appId: BigInt!, $rootId: String!, $relationshipType: String!, $depth: Int) {
  gameModelTraverse(
    appId: $appId
    rootId: $rootId
    relationshipType: $relationshipType
    depth: $depth
  ) {
    rootId
    nodes {
      ...GmContainerFields
    }
    edges {
      edgeId
      fromContainerId
      toContainerId
      relationshipType
      weight
    }
  }
}

fragment GmContainerFields on GmContainer {
  containerId
  appId
  sessionId
  typeName
  displayName
  description
  ownerUserId
  metadataJson
  bindingKey
})gql";
inline constexpr std::string_view kGameModelTraverseOperationName = "GameModelTraverse";
inline constexpr std::string_view kGameModelSessionIsolatedDocument = R"gql(query GameModelSession($appId: BigInt!, $sessionId: String!) {
  gameModelSession(appId: $appId, sessionId: $sessionId) {
    ...GmSessionFields
  }
}

fragment GmSessionFields on GmSession {
  sessionId
  appId
  name
  status
  createdByUserId
  currentTurnUserId
  metadataJson
})gql";
inline constexpr std::string_view kGameModelSessionOperationName = "GameModelSession";
inline constexpr std::string_view kGameModelSessionsIsolatedDocument = R"gql(query GameModelSessions($appId: BigInt!, $status: String) {
  gameModelSessions(appId: $appId, status: $status) {
    ...GmSessionFields
  }
}

fragment GmSessionFields on GmSession {
  sessionId
  appId
  name
  status
  createdByUserId
  currentTurnUserId
  metadataJson
})gql";
inline constexpr std::string_view kGameModelSessionsOperationName = "GameModelSessions";
inline constexpr std::string_view kGameModelEventsIsolatedDocument = R"gql(query GameModelEvents($appId: BigInt!, $sessionId: String, $selfContainerId: String, $functionName: String, $success: Boolean, $limit: Int, $offset: Int) {
  gameModelEvents(
    appId: $appId
    sessionId: $sessionId
    selfContainerId: $selfContainerId
    functionName: $functionName
    success: $success
    limit: $limit
    offset: $offset
  ) {
    eventId
    flowId
    sessionId
    functionName
    selfContainerId
    callerUserId
    callerKind
    automationId
    paramsJson
    mutationsAppliedJson
    permissionEffectsAppliedJson
    returnValueJson
    success
    errorMessage
    executedAt
  }
})gql";
inline constexpr std::string_view kGameModelEventsOperationName = "GameModelEvents";
inline constexpr std::string_view kGameModelEventsConnectionIsolatedDocument = R"gql(query GameModelEventsConnection($appId: BigInt!, $first: Int, $after: String, $sessionId: String, $selfContainerId: String, $functionName: String, $success: Boolean) {
  gameModelEventsConnection(
    appId: $appId
    first: $first
    after: $after
    sessionId: $sessionId
    selfContainerId: $selfContainerId
    functionName: $functionName
    success: $success
  ) {
    edges {
      cursor
      node {
        eventId
        flowId
        sessionId
        functionName
        selfContainerId
        callerUserId
        callerKind
        automationId
        paramsJson
        mutationsAppliedJson
        permissionEffectsAppliedJson
        returnValueJson
        success
        errorMessage
        executedAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kGameModelEventsConnectionOperationName = "GameModelEventsConnection";
inline constexpr std::string_view kGameModelActivePlayerCountIsolatedDocument = R"gql(query GameModelActivePlayerCount($appId: BigInt!) {
  gameModelActivePlayerCount(appId: $appId) {
    appId
    activePlayerCount
    status
    observedAt
    revision
  }
})gql";
inline constexpr std::string_view kGameModelActivePlayerCountOperationName = "GameModelActivePlayerCount";
inline constexpr std::string_view kGameModelFlowIsolatedDocument = R"gql(query GameModelFlow($appId: BigInt!, $flowId: String!) {
  gameModelFlow(appId: $appId, flowId: $flowId) {
    flowId
    events {
      eventId
      flowId
      sessionId
      functionName
      selfContainerId
      callerUserId
      callerKind
      automationId
      paramsJson
      mutationsAppliedJson
      permissionEffectsAppliedJson
      returnValueJson
      success
      errorMessage
      executedAt
    }
    automationRuns {
      runId
      appId
      flowId
      automationId
      automationName
      triggerSource
      parentRunId
      cascadeDepth
      startedAt
      finishedAt
      durationUs
      targets
      invocations
      mutations
      fnCalls
      gasUsed
      success
      errorMessage
      circuitAction
      computeUnits
    }
    moduleRuns {
      runId
      appId
      flowId
      moduleId
      moduleName
      triggerSource
      entry
      startedAt
      durationUs
      fuelUsed
      dbReads
      dbWrites
      egressMsgs
      egressBytes
      success
      errorMessage
      circuitAction
    }
  }
})gql";
inline constexpr std::string_view kGameModelFlowOperationName = "GameModelFlow";
inline constexpr std::string_view kGameModelActivePlayerCountChangedIsolatedDocument = R"gql(subscription GameModelActivePlayerCountChanged($appId: BigInt!) {
  gameModelActivePlayerCountChanged(appId: $appId) {
    appId
    previousCount
    currentCount
    delta
    revision
    observedAt
  }
})gql";
inline constexpr std::string_view kGameModelActivePlayerCountChangedOperationName = "GameModelActivePlayerCountChanged";
inline constexpr std::string_view kGameModelContainerChangedIsolatedDocument = R"gql(subscription GameModelContainerChanged($appId: BigInt!, $typeName: String, $sessionId: String) {
  gameModelContainerChanged(
    appId: $appId
    typeName: $typeName
    sessionId: $sessionId
  ) {
    appId
    containerId
    typeName
    sessionId
    source
    functionName
    changedKeys
    occurredAt
  }
})gql";
inline constexpr std::string_view kGameModelContainerChangedOperationName = "GameModelContainerChanged";

/// gameModel/GameModelStudio.graphql
inline constexpr std::string_view kGameModelStudioDocument = R"gql(fragment GmFunctionFields on GmFunction {
  functionId
  appId
  name
  containerTypeName
  description
  returnType
  invokeScope
  invokePolicyJson
  autonomousInvocable
  returnExpression
  warnings
  parameters {
    name
    valueType
    required
    defaultValueJson
    description
    sortOrder
  }
  mutations {
    target
    property
    expression
  }
  notifications {
    kind
    emitAs
    args {
      name
      expression
    }
  }
  permissionEffects {
    action
    permissionKeys
    userExpression
    gridIdExpression
    ttlSecondsExpression
  }
  timers {
    functionName
    target
    delayMsExpression
    dedupeKeyExpression
    params {
      name
      expression
    }
  }
}

fragment GmPropertyDefFields on GmPropertyDef {
  appId
  containerTypeName
  key
  valueType
  defaultValueJson
  visibility
  writable
  description
}

mutation GameModelSeed($input: SeedGameModelInput!) {
  gameModelSeed(input: $input) {
    containerTypesCreated
    propertyDefinitionsCreated
    functionsCreated
    containersCreated
    edgesCreated
    warnings
    idMapJson
  }
}

mutation GameModelUpsertContainerType($input: UpsertContainerTypeInput!) {
  gameModelUpsertContainerType(input: $input) {
    appId
    typeName
    displayName
    description
    instantiableBy
    defaultPropertyVisibility
    metadataJson
  }
}

mutation GameModelUpsertPropertyDef($input: UpsertPropertyDefInput!) {
  gameModelUpsertPropertyDef(input: $input) {
    ...GmPropertyDefFields
  }
}

mutation GameModelDeletePropertyDef(
  $appId: BigInt!
  $containerTypeName: String!
  $key: String!
) {
  gameModelDeletePropertyDef(
    appId: $appId
    containerTypeName: $containerTypeName
    key: $key
  )
}

mutation GameModelDeleteContainerType($appId: BigInt!, $typeName: String!) {
  gameModelDeleteContainerType(appId: $appId, typeName: $typeName)
}

mutation GameModelUpsertFunction($input: UpsertFunctionInput!) {
  gameModelUpsertFunction(input: $input) {
    ...GmFunctionFields
  }
}

mutation GameModelDeleteFunction($appId: BigInt!, $name: String!) {
  gameModelDeleteFunction(appId: $appId, name: $name)
}

mutation GameModelDefineFeature($input: DefineAppFeatureInput!) {
  gameModelDefineFeature(input: $input) {
    appId
    featureKey
    description
  }
}

mutation GameModelGrantTierFeature($input: GrantTierFeatureInput!) {
  gameModelGrantTierFeature(input: $input) {
    appId
    tierId
    featureKey
  }
}

mutation GameModelSetPolicy($input: SetGameModelPolicyInput!) {
  gameModelSetPolicy(input: $input) {
    appId
    sessionCreationPolicy
    defaultParticipantRole
  }
}

query GameModelTypeSchema($appId: BigInt!, $typeName: String!) {
  gameModelTypeSchema(appId: $appId, typeName: $typeName) {
    typeName
    propertyDefinitions {
      ...GmPropertyDefFields
    }
    functions {
      ...GmFunctionFields
    }
  }
}

query GameModelContainerTypes($appId: BigInt!) {
  gameModelContainerTypes(appId: $appId) {
    appId
    typeName
    displayName
    description
    instantiableBy
    defaultPropertyVisibility
    metadataJson
  }
}

query GameModelPropertyDefs($appId: BigInt!, $typeName: String!) {
  gameModelPropertyDefs(appId: $appId, typeName: $typeName) {
    ...GmPropertyDefFields
  }
}

query GameModelFunction($appId: BigInt!, $name: String!) {
  gameModelFunction(appId: $appId, name: $name) {
    ...GmFunctionFields
  }
}

query GameModelFunctions($appId: BigInt!, $containerTypeName: String) {
  gameModelFunctions(appId: $appId, containerTypeName: $containerTypeName) {
    ...GmFunctionFields
  }
}

query GameModelFeatures($appId: BigInt!) {
  gameModelFeatures(appId: $appId) {
    appId
    featureKey
    description
  }
}

query GameModelTierFeatures($appId: BigInt!, $tierId: BigInt) {
  gameModelTierFeatures(appId: $appId, tierId: $tierId) {
    appId
    tierId
    featureKey
  }
}

query GameModelPolicy($appId: BigInt!) {
  gameModelPolicy(appId: $appId) {
    appId
    sessionCreationPolicy
    defaultParticipantRole
  }
}

mutation GameModelRevokeTierFeature($input: GrantTierFeatureInput!) {
  gameModelRevokeTierFeature(input: $input)
})gql";
inline constexpr std::string_view kGameModelSeedIsolatedDocument = R"gql(mutation GameModelSeed($input: SeedGameModelInput!) {
  gameModelSeed(input: $input) {
    containerTypesCreated
    propertyDefinitionsCreated
    functionsCreated
    containersCreated
    edgesCreated
    warnings
    idMapJson
  }
})gql";
inline constexpr std::string_view kGameModelSeedOperationName = "GameModelSeed";
inline constexpr std::string_view kGameModelUpsertContainerTypeIsolatedDocument = R"gql(mutation GameModelUpsertContainerType($input: UpsertContainerTypeInput!) {
  gameModelUpsertContainerType(input: $input) {
    appId
    typeName
    displayName
    description
    instantiableBy
    defaultPropertyVisibility
    metadataJson
  }
})gql";
inline constexpr std::string_view kGameModelUpsertContainerTypeOperationName = "GameModelUpsertContainerType";
inline constexpr std::string_view kGameModelUpsertPropertyDefIsolatedDocument = R"gql(mutation GameModelUpsertPropertyDef($input: UpsertPropertyDefInput!) {
  gameModelUpsertPropertyDef(input: $input) {
    ...GmPropertyDefFields
  }
}

fragment GmPropertyDefFields on GmPropertyDef {
  appId
  containerTypeName
  key
  valueType
  defaultValueJson
  visibility
  writable
  description
})gql";
inline constexpr std::string_view kGameModelUpsertPropertyDefOperationName = "GameModelUpsertPropertyDef";
inline constexpr std::string_view kGameModelDeletePropertyDefIsolatedDocument = R"gql(mutation GameModelDeletePropertyDef($appId: BigInt!, $containerTypeName: String!, $key: String!) {
  gameModelDeletePropertyDef(
    appId: $appId
    containerTypeName: $containerTypeName
    key: $key
  )
})gql";
inline constexpr std::string_view kGameModelDeletePropertyDefOperationName = "GameModelDeletePropertyDef";
inline constexpr std::string_view kGameModelDeleteContainerTypeIsolatedDocument = R"gql(mutation GameModelDeleteContainerType($appId: BigInt!, $typeName: String!) {
  gameModelDeleteContainerType(appId: $appId, typeName: $typeName)
})gql";
inline constexpr std::string_view kGameModelDeleteContainerTypeOperationName = "GameModelDeleteContainerType";
inline constexpr std::string_view kGameModelUpsertFunctionIsolatedDocument = R"gql(mutation GameModelUpsertFunction($input: UpsertFunctionInput!) {
  gameModelUpsertFunction(input: $input) {
    ...GmFunctionFields
  }
}

fragment GmFunctionFields on GmFunction {
  functionId
  appId
  name
  containerTypeName
  description
  returnType
  invokeScope
  invokePolicyJson
  autonomousInvocable
  returnExpression
  warnings
  parameters {
    name
    valueType
    required
    defaultValueJson
    description
    sortOrder
  }
  mutations {
    target
    property
    expression
  }
  notifications {
    kind
    emitAs
    args {
      name
      expression
    }
  }
  permissionEffects {
    action
    permissionKeys
    userExpression
    gridIdExpression
    ttlSecondsExpression
  }
  timers {
    functionName
    target
    delayMsExpression
    dedupeKeyExpression
    params {
      name
      expression
    }
  }
})gql";
inline constexpr std::string_view kGameModelUpsertFunctionOperationName = "GameModelUpsertFunction";
inline constexpr std::string_view kGameModelDeleteFunctionIsolatedDocument = R"gql(mutation GameModelDeleteFunction($appId: BigInt!, $name: String!) {
  gameModelDeleteFunction(appId: $appId, name: $name)
})gql";
inline constexpr std::string_view kGameModelDeleteFunctionOperationName = "GameModelDeleteFunction";
inline constexpr std::string_view kGameModelDefineFeatureIsolatedDocument = R"gql(mutation GameModelDefineFeature($input: DefineAppFeatureInput!) {
  gameModelDefineFeature(input: $input) {
    appId
    featureKey
    description
  }
})gql";
inline constexpr std::string_view kGameModelDefineFeatureOperationName = "GameModelDefineFeature";
inline constexpr std::string_view kGameModelGrantTierFeatureIsolatedDocument = R"gql(mutation GameModelGrantTierFeature($input: GrantTierFeatureInput!) {
  gameModelGrantTierFeature(input: $input) {
    appId
    tierId
    featureKey
  }
})gql";
inline constexpr std::string_view kGameModelGrantTierFeatureOperationName = "GameModelGrantTierFeature";
inline constexpr std::string_view kGameModelSetPolicyIsolatedDocument = R"gql(mutation GameModelSetPolicy($input: SetGameModelPolicyInput!) {
  gameModelSetPolicy(input: $input) {
    appId
    sessionCreationPolicy
    defaultParticipantRole
  }
})gql";
inline constexpr std::string_view kGameModelSetPolicyOperationName = "GameModelSetPolicy";
inline constexpr std::string_view kGameModelTypeSchemaIsolatedDocument = R"gql(query GameModelTypeSchema($appId: BigInt!, $typeName: String!) {
  gameModelTypeSchema(appId: $appId, typeName: $typeName) {
    typeName
    propertyDefinitions {
      ...GmPropertyDefFields
    }
    functions {
      ...GmFunctionFields
    }
  }
}

fragment GmPropertyDefFields on GmPropertyDef {
  appId
  containerTypeName
  key
  valueType
  defaultValueJson
  visibility
  writable
  description
}

fragment GmFunctionFields on GmFunction {
  functionId
  appId
  name
  containerTypeName
  description
  returnType
  invokeScope
  invokePolicyJson
  autonomousInvocable
  returnExpression
  warnings
  parameters {
    name
    valueType
    required
    defaultValueJson
    description
    sortOrder
  }
  mutations {
    target
    property
    expression
  }
  notifications {
    kind
    emitAs
    args {
      name
      expression
    }
  }
  permissionEffects {
    action
    permissionKeys
    userExpression
    gridIdExpression
    ttlSecondsExpression
  }
  timers {
    functionName
    target
    delayMsExpression
    dedupeKeyExpression
    params {
      name
      expression
    }
  }
})gql";
inline constexpr std::string_view kGameModelTypeSchemaOperationName = "GameModelTypeSchema";
inline constexpr std::string_view kGameModelContainerTypesIsolatedDocument = R"gql(query GameModelContainerTypes($appId: BigInt!) {
  gameModelContainerTypes(appId: $appId) {
    appId
    typeName
    displayName
    description
    instantiableBy
    defaultPropertyVisibility
    metadataJson
  }
})gql";
inline constexpr std::string_view kGameModelContainerTypesOperationName = "GameModelContainerTypes";
inline constexpr std::string_view kGameModelPropertyDefsIsolatedDocument = R"gql(query GameModelPropertyDefs($appId: BigInt!, $typeName: String!) {
  gameModelPropertyDefs(appId: $appId, typeName: $typeName) {
    ...GmPropertyDefFields
  }
}

fragment GmPropertyDefFields on GmPropertyDef {
  appId
  containerTypeName
  key
  valueType
  defaultValueJson
  visibility
  writable
  description
})gql";
inline constexpr std::string_view kGameModelPropertyDefsOperationName = "GameModelPropertyDefs";
inline constexpr std::string_view kGameModelFunctionIsolatedDocument = R"gql(query GameModelFunction($appId: BigInt!, $name: String!) {
  gameModelFunction(appId: $appId, name: $name) {
    ...GmFunctionFields
  }
}

fragment GmFunctionFields on GmFunction {
  functionId
  appId
  name
  containerTypeName
  description
  returnType
  invokeScope
  invokePolicyJson
  autonomousInvocable
  returnExpression
  warnings
  parameters {
    name
    valueType
    required
    defaultValueJson
    description
    sortOrder
  }
  mutations {
    target
    property
    expression
  }
  notifications {
    kind
    emitAs
    args {
      name
      expression
    }
  }
  permissionEffects {
    action
    permissionKeys
    userExpression
    gridIdExpression
    ttlSecondsExpression
  }
  timers {
    functionName
    target
    delayMsExpression
    dedupeKeyExpression
    params {
      name
      expression
    }
  }
})gql";
inline constexpr std::string_view kGameModelFunctionOperationName = "GameModelFunction";
inline constexpr std::string_view kGameModelFunctionsIsolatedDocument = R"gql(query GameModelFunctions($appId: BigInt!, $containerTypeName: String) {
  gameModelFunctions(appId: $appId, containerTypeName: $containerTypeName) {
    ...GmFunctionFields
  }
}

fragment GmFunctionFields on GmFunction {
  functionId
  appId
  name
  containerTypeName
  description
  returnType
  invokeScope
  invokePolicyJson
  autonomousInvocable
  returnExpression
  warnings
  parameters {
    name
    valueType
    required
    defaultValueJson
    description
    sortOrder
  }
  mutations {
    target
    property
    expression
  }
  notifications {
    kind
    emitAs
    args {
      name
      expression
    }
  }
  permissionEffects {
    action
    permissionKeys
    userExpression
    gridIdExpression
    ttlSecondsExpression
  }
  timers {
    functionName
    target
    delayMsExpression
    dedupeKeyExpression
    params {
      name
      expression
    }
  }
})gql";
inline constexpr std::string_view kGameModelFunctionsOperationName = "GameModelFunctions";
inline constexpr std::string_view kGameModelFeaturesIsolatedDocument = R"gql(query GameModelFeatures($appId: BigInt!) {
  gameModelFeatures(appId: $appId) {
    appId
    featureKey
    description
  }
})gql";
inline constexpr std::string_view kGameModelFeaturesOperationName = "GameModelFeatures";
inline constexpr std::string_view kGameModelTierFeaturesIsolatedDocument = R"gql(query GameModelTierFeatures($appId: BigInt!, $tierId: BigInt) {
  gameModelTierFeatures(appId: $appId, tierId: $tierId) {
    appId
    tierId
    featureKey
  }
})gql";
inline constexpr std::string_view kGameModelTierFeaturesOperationName = "GameModelTierFeatures";
inline constexpr std::string_view kGameModelPolicyIsolatedDocument = R"gql(query GameModelPolicy($appId: BigInt!) {
  gameModelPolicy(appId: $appId) {
    appId
    sessionCreationPolicy
    defaultParticipantRole
  }
})gql";
inline constexpr std::string_view kGameModelPolicyOperationName = "GameModelPolicy";
inline constexpr std::string_view kGameModelRevokeTierFeatureIsolatedDocument = R"gql(mutation GameModelRevokeTierFeature($input: GrantTierFeatureInput!) {
  gameModelRevokeTierFeature(input: $input)
})gql";
inline constexpr std::string_view kGameModelRevokeTierFeatureOperationName = "GameModelRevokeTierFeature";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "GameModelUpsertAutomation") return kGameModelUpsertAutomationIsolatedDocument;
  if (operationName == "GameModelDeleteAutomation") return kGameModelDeleteAutomationIsolatedDocument;
  if (operationName == "GameModelSetAutomationEnabled") return kGameModelSetAutomationEnabledIsolatedDocument;
  if (operationName == "GameModelUpsertAutomationTrigger") return kGameModelUpsertAutomationTriggerIsolatedDocument;
  if (operationName == "GameModelDeleteAutomationTrigger") return kGameModelDeleteAutomationTriggerIsolatedDocument;
  if (operationName == "GameModelSetAutomationPolicy") return kGameModelSetAutomationPolicyIsolatedDocument;
  if (operationName == "GameModelRunAutomation") return kGameModelRunAutomationIsolatedDocument;
  if (operationName == "GameModelAutomations") return kGameModelAutomationsIsolatedDocument;
  if (operationName == "GameModelAutomation") return kGameModelAutomationIsolatedDocument;
  if (operationName == "GameModelAutomationTriggers") return kGameModelAutomationTriggersIsolatedDocument;
  if (operationName == "GameModelAutomationPolicy") return kGameModelAutomationPolicyIsolatedDocument;
  if (operationName == "GameModelAutomationRuns") return kGameModelAutomationRunsIsolatedDocument;
  if (operationName == "GameModelAutomationStats") return kGameModelAutomationStatsIsolatedDocument;
  if (operationName == "GameModelAppDiagnostics") return kGameModelAppDiagnosticsIsolatedDocument;
  if (operationName == "GameModelScheduleInvoke") return kGameModelScheduleInvokeIsolatedDocument;
  if (operationName == "GameModelCancelTimer") return kGameModelCancelTimerIsolatedDocument;
  if (operationName == "GameModelTimers") return kGameModelTimersIsolatedDocument;
  if (operationName == "GameModelFunctionCircuits") return kGameModelFunctionCircuitsIsolatedDocument;
  if (operationName == "CrowdyModelLint") return kCrowdyModelLintIsolatedDocument;
  if (operationName == "GameModelCreateSession") return kGameModelCreateSessionIsolatedDocument;
  if (operationName == "GameModelJoinSession") return kGameModelJoinSessionIsolatedDocument;
  if (operationName == "GameModelSetSessionTurn") return kGameModelSetSessionTurnIsolatedDocument;
  if (operationName == "GameModelCreateContainer") return kGameModelCreateContainerIsolatedDocument;
  if (operationName == "GameModelEnsureContainer") return kGameModelEnsureContainerIsolatedDocument;
  if (operationName == "GameModelDeleteContainer") return kGameModelDeleteContainerIsolatedDocument;
  if (operationName == "GameModelSetProperty") return kGameModelSetPropertyIsolatedDocument;
  if (operationName == "GameModelAddEdge") return kGameModelAddEdgeIsolatedDocument;
  if (operationName == "GameModelDeleteEdge") return kGameModelDeleteEdgeIsolatedDocument;
  if (operationName == "GameModelInvoke") return kGameModelInvokeIsolatedDocument;
  if (operationName == "GameModelContainer") return kGameModelContainerIsolatedDocument;
  if (operationName == "GameModelContainers") return kGameModelContainersIsolatedDocument;
  if (operationName == "GameModelContainerState") return kGameModelContainerStateIsolatedDocument;
  if (operationName == "GameModelTraverse") return kGameModelTraverseIsolatedDocument;
  if (operationName == "GameModelSession") return kGameModelSessionIsolatedDocument;
  if (operationName == "GameModelSessions") return kGameModelSessionsIsolatedDocument;
  if (operationName == "GameModelEvents") return kGameModelEventsIsolatedDocument;
  if (operationName == "GameModelEventsConnection") return kGameModelEventsConnectionIsolatedDocument;
  if (operationName == "GameModelActivePlayerCount") return kGameModelActivePlayerCountIsolatedDocument;
  if (operationName == "GameModelFlow") return kGameModelFlowIsolatedDocument;
  if (operationName == "GameModelActivePlayerCountChanged") return kGameModelActivePlayerCountChangedIsolatedDocument;
  if (operationName == "GameModelContainerChanged") return kGameModelContainerChangedIsolatedDocument;
  if (operationName == "GameModelSeed") return kGameModelSeedIsolatedDocument;
  if (operationName == "GameModelUpsertContainerType") return kGameModelUpsertContainerTypeIsolatedDocument;
  if (operationName == "GameModelUpsertPropertyDef") return kGameModelUpsertPropertyDefIsolatedDocument;
  if (operationName == "GameModelDeletePropertyDef") return kGameModelDeletePropertyDefIsolatedDocument;
  if (operationName == "GameModelDeleteContainerType") return kGameModelDeleteContainerTypeIsolatedDocument;
  if (operationName == "GameModelUpsertFunction") return kGameModelUpsertFunctionIsolatedDocument;
  if (operationName == "GameModelDeleteFunction") return kGameModelDeleteFunctionIsolatedDocument;
  if (operationName == "GameModelDefineFeature") return kGameModelDefineFeatureIsolatedDocument;
  if (operationName == "GameModelGrantTierFeature") return kGameModelGrantTierFeatureIsolatedDocument;
  if (operationName == "GameModelSetPolicy") return kGameModelSetPolicyIsolatedDocument;
  if (operationName == "GameModelTypeSchema") return kGameModelTypeSchemaIsolatedDocument;
  if (operationName == "GameModelContainerTypes") return kGameModelContainerTypesIsolatedDocument;
  if (operationName == "GameModelPropertyDefs") return kGameModelPropertyDefsIsolatedDocument;
  if (operationName == "GameModelFunction") return kGameModelFunctionIsolatedDocument;
  if (operationName == "GameModelFunctions") return kGameModelFunctionsIsolatedDocument;
  if (operationName == "GameModelFeatures") return kGameModelFeaturesIsolatedDocument;
  if (operationName == "GameModelTierFeatures") return kGameModelTierFeaturesIsolatedDocument;
  if (operationName == "GameModelPolicy") return kGameModelPolicyIsolatedDocument;
  if (operationName == "GameModelRevokeTierFeature") return kGameModelRevokeTierFeatureIsolatedDocument;
  return {};
}

}  // namespace gameModel

namespace host {

/// host/Host.graphql
inline constexpr std::string_view kHostDocument = R"gql(query GameHost($appId: BigInt!) {
  gameHost(appId: $appId) {
    hostUserId
    actorCount
    earliestActorJoinedAt
  }
}

query AmIGameHost($appId: BigInt!) {
  amIGameHost(appId: $appId)
}

mutation ActorHeartbeat($appId: BigInt!) {
  actorHeartbeat(appId: $appId) {
    hostUserId
    actorCount
    earliestActorJoinedAt
  }
})gql";
inline constexpr std::string_view kGameHostIsolatedDocument = R"gql(query GameHost($appId: BigInt!) {
  gameHost(appId: $appId) {
    hostUserId
    actorCount
    earliestActorJoinedAt
  }
})gql";
inline constexpr std::string_view kGameHostOperationName = "GameHost";
inline constexpr std::string_view kAmIGameHostIsolatedDocument = R"gql(query AmIGameHost($appId: BigInt!) {
  amIGameHost(appId: $appId)
})gql";
inline constexpr std::string_view kAmIGameHostOperationName = "AmIGameHost";
inline constexpr std::string_view kActorHeartbeatIsolatedDocument = R"gql(mutation ActorHeartbeat($appId: BigInt!) {
  actorHeartbeat(appId: $appId) {
    hostUserId
    actorCount
    earliestActorJoinedAt
  }
})gql";
inline constexpr std::string_view kActorHeartbeatOperationName = "ActorHeartbeat";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "GameHost") return kGameHostIsolatedDocument;
  if (operationName == "AmIGameHost") return kAmIGameHostIsolatedDocument;
  if (operationName == "ActorHeartbeat") return kActorHeartbeatIsolatedDocument;
  return {};
}

}  // namespace host

namespace marketplace {

/// marketplace/Marketplace.graphql
inline constexpr std::string_view kMarketplaceDocument = R"gql(fragment PlayerCodeListingFields on PlayerCodeListing {
  listingId
  appId
  ownerKind
  ownerRef
  name
  description
  mediaJson
  licenseMode
  acquisitionMode
  priceCents
  rentIntervalDays
  windowDays
  unitBudget
  status
  createdAt
}

fragment PlayerCodeListingVersionFields on PlayerCodeListingVersion {
  versionId
  listingId
  versionNo
  serverArtifactHashes
  clientArtifactHashes
  requirements {
    serverArtifactHash
    clientArtifactHash
  }
  capabilitySummaryJson
  capabilityHash
  openSource
  licenseText
  createdAt
}

fragment PlayerCodeAcquisitionFields on PlayerCodeAcquisition {
  acquisitionId
  listingId
  appId
  mode
  status
  expiresAt
  unitBudget
  unitsConsumed
  acquiredAt
}

fragment PlayerCodeInstallFields on PlayerCodeInstall {
  installId
  acquisitionId
  listingId
  appId
  pinnedVersionId
  targetGridId
  consentedCapabilityHash
  status
  createdAt
}

fragment GridClaimRequestFields on GridClaimRequest {
  requestId
  appId
  gridId
  requesterUserId
  status
  createdAt
}

# ---- Game API: browse / publish / acquire / install / consent -----------------

query MarketplaceListings($appId: BigInt!) {
  playerCodeListings(appId: $appId) {
    ...PlayerCodeListingFields
    admissionState
    latestVersionId
  }
}

query MarketplaceListingVersions($appId: BigInt!, $listingId: String!) {
  playerCodeListingVersions(appId: $appId, listingId: $listingId) {
    ...PlayerCodeListingVersionFields
  }
}

query MarketplaceMyAcquisitions($appId: BigInt!) {
  myPlayerCodeAcquisitions(appId: $appId) {
    ...PlayerCodeAcquisitionFields
  }
}

query MarketplaceMyInstalls($appId: BigInt!) {
  myPlayerCodeInstalls(appId: $appId) {
    ...PlayerCodeInstallFields
  }
}

query MarketplaceGridClientMods($appId: BigInt!, $gridId: BigInt!) {
  gridClientMods(appId: $appId, gridId: $gridId) {
    attachmentId
    listingId
    listingName
    versionId
    sourceKind
    authorKind
    authorRef
    serverVersionId
    clientVersionId
    clientArtifactHash
    gridId
    capabilitySummaryJson
    capabilityHash
    authorCapabilitySummaryJson
    authorCapabilityHash
    callerConsented
    callerTrustsAuthor
  }
}

query MarketplaceClientArtifact(
  $appId: BigInt!
  $listingId: String
  $attachmentId: String
  $versionId: String
) {
  playerCodeClientArtifact(
    appId: $appId
    listingId: $listingId
    attachmentId: $attachmentId
    versionId: $versionId
  ) {
    versionId
    artifactHash
    artifactBase64
    sizeBytes
    abiVersion
    contractJson
    clientFuelPerDispatch
  }
}

mutation MarketplaceTrustGridAuthor(
  $appId: BigInt!
  $gridId: BigInt!
  $authorKind: PlayerCodeOwnerKind!
  $authorRef: BigInt!
  $consentCapabilityHash: String!
) {
  trustGridAuthor(
    appId: $appId
    gridId: $gridId
    authorKind: $authorKind
    authorRef: $authorRef
    consentCapabilityHash: $consentCapabilityHash
  )
}

mutation MarketplacePublishListing($input: PublishPlayerCodeInput!) {
  publishPlayerCode(input: $input) {
    ...PlayerCodeListingFields
    admissionState
    latestVersionId
  }
}

mutation MarketplacePublishVersion($input: PublishPlayerCodeVersionInput!) {
  publishPlayerCodeVersion(input: $input) {
    ...PlayerCodeListingVersionFields
  }
}

mutation MarketplaceAcquire($appId: BigInt!, $listingId: String!) {
  acquirePlayerCode(appId: $appId, listingId: $listingId) {
    ...PlayerCodeAcquisitionFields
  }
}

mutation MarketplaceInstall(
  $appId: BigInt!
  $acquisitionId: String!
  $consentCapabilityHash: String!
  $gridId: BigInt
  $versionId: String
) {
  installPlayerCode(
    appId: $appId
    acquisitionId: $acquisitionId
    consentCapabilityHash: $consentCapabilityHash
    gridId: $gridId
    versionId: $versionId
  ) {
    ...PlayerCodeInstallFields
  }
}

mutation MarketplaceUninstall($appId: BigInt!, $installId: String!) {
  uninstallPlayerCode(appId: $appId, installId: $installId)
}

mutation MarketplaceConsentGridClientMod(
  $appId: BigInt!
  $attachmentId: String!
  $consentCapabilityHash: String!
) {
  consentGridClientMod(
    appId: $appId
    attachmentId: $attachmentId
    consentCapabilityHash: $consentCapabilityHash
  )
}

# ---- Game API: D4 grid claim flows --------------------------------------------

query MarketplaceGridClaimPolicy($appId: BigInt!) {
  gridClaimPolicy(appId: $appId)
}

query MarketplaceGridClaimRequests($appId: BigInt!) {
  gridClaimRequests(appId: $appId) {
    ...GridClaimRequestFields
  }
}

mutation MarketplaceClaimGridOwnership($appId: BigInt!, $gridId: BigInt!) {
  claimGridOwnership(appId: $appId, gridId: $gridId) {
    policy
    ownershipAssigned
    claimRequestId
  }
}

mutation MarketplaceClaimGridChunk(
  $appId: BigInt!
  $chunk: ChunkCoordinatesInput!
) {
  claimGridChunk(appId: $appId, chunk: $chunk) {
    gridId
    lowChunk {
      x
      y
      z
    }
    highChunk {
      x
      y
      z
    }
    policy
    ownership {
      gridOwnershipId
      ownerKind
      ownerRef
      tenure
      acquiredVia
      acquiredAt
      expiresAt
    }
    moddable
    effectivePermissionKeys
  }
}

mutation MarketplaceReleaseClaimedGrid($appId: BigInt!, $gridId: BigInt!) {
  releaseClaimedGrid(appId: $appId, gridId: $gridId) {
    gridId
    lowChunk {
      x
      y
      z
    }
    highChunk {
      x
      y
      z
    }
    policy
    released
  }
}

mutation MarketplaceDecideGridClaim(
  $appId: BigInt!
  $requestId: String!
  $approve: Boolean!
) {
  decideGridClaim(appId: $appId, requestId: $requestId, approve: $approve) {
    ...GridClaimRequestFields
  }
}

mutation MarketplaceIssueGridClaimInvite(
  $appId: BigInt!
  $gridId: BigInt!
  $inviteeUserId: BigInt!
) {
  issueGridClaimInvite(
    appId: $appId
    gridId: $gridId
    inviteeUserId: $inviteeUserId
  )
}

# ---- P4b: paid modes, grid commerce, seller payouts ---------------------------

mutation MarketplaceRenewAcquisition($appId: BigInt!, $acquisitionId: String!) {
  renewPlayerCodeAcquisition(appId: $appId, acquisitionId: $acquisitionId) {
    ...PlayerCodeAcquisitionFields
  }
}

mutation MarketplaceTopUpAcquisition($appId: BigInt!, $acquisitionId: String!) {
  topUpPlayerCodeAcquisition(appId: $appId, acquisitionId: $acquisitionId) {
    ...PlayerCodeAcquisitionFields
  }
}

mutation MarketplaceRefundAcquisition($appId: BigInt!, $acquisitionId: String!) {
  refundPlayerCodeAcquisition(appId: $appId, acquisitionId: $acquisitionId)
}

query MarketplaceGridListings($appId: BigInt!) {
  gridListings(appId: $appId) {
    gridListingId
    appId
    kind
    name
    description
    priceCents
    conferredPermissionKeys
    resalePolicy
  }
}

mutation MarketplacePurchaseGrid(
  $appId: BigInt!
  $gridListingId: String!
  $chunkX: Int
  $chunkY: Int
  $chunkZ: Int
) {
  purchaseGrid(
    appId: $appId
    gridListingId: $gridListingId
    chunkX: $chunkX
    chunkY: $chunkY
    chunkZ: $chunkZ
  ) {
    gridId
    ownershipAssigned
  }
})gql";
inline constexpr std::string_view kMarketplaceListingsIsolatedDocument = R"gql(query MarketplaceListings($appId: BigInt!) {
  playerCodeListings(appId: $appId) {
    ...PlayerCodeListingFields
    admissionState
    latestVersionId
  }
}

fragment PlayerCodeListingFields on PlayerCodeListing {
  listingId
  appId
  ownerKind
  ownerRef
  name
  description
  mediaJson
  licenseMode
  acquisitionMode
  priceCents
  rentIntervalDays
  windowDays
  unitBudget
  status
  createdAt
})gql";
inline constexpr std::string_view kMarketplaceListingsOperationName = "MarketplaceListings";
inline constexpr std::string_view kMarketplaceListingVersionsIsolatedDocument = R"gql(query MarketplaceListingVersions($appId: BigInt!, $listingId: String!) {
  playerCodeListingVersions(appId: $appId, listingId: $listingId) {
    ...PlayerCodeListingVersionFields
  }
}

fragment PlayerCodeListingVersionFields on PlayerCodeListingVersion {
  versionId
  listingId
  versionNo
  serverArtifactHashes
  clientArtifactHashes
  requirements {
    serverArtifactHash
    clientArtifactHash
  }
  capabilitySummaryJson
  capabilityHash
  openSource
  licenseText
  createdAt
})gql";
inline constexpr std::string_view kMarketplaceListingVersionsOperationName = "MarketplaceListingVersions";
inline constexpr std::string_view kMarketplaceMyAcquisitionsIsolatedDocument = R"gql(query MarketplaceMyAcquisitions($appId: BigInt!) {
  myPlayerCodeAcquisitions(appId: $appId) {
    ...PlayerCodeAcquisitionFields
  }
}

fragment PlayerCodeAcquisitionFields on PlayerCodeAcquisition {
  acquisitionId
  listingId
  appId
  mode
  status
  expiresAt
  unitBudget
  unitsConsumed
  acquiredAt
})gql";
inline constexpr std::string_view kMarketplaceMyAcquisitionsOperationName = "MarketplaceMyAcquisitions";
inline constexpr std::string_view kMarketplaceMyInstallsIsolatedDocument = R"gql(query MarketplaceMyInstalls($appId: BigInt!) {
  myPlayerCodeInstalls(appId: $appId) {
    ...PlayerCodeInstallFields
  }
}

fragment PlayerCodeInstallFields on PlayerCodeInstall {
  installId
  acquisitionId
  listingId
  appId
  pinnedVersionId
  targetGridId
  consentedCapabilityHash
  status
  createdAt
})gql";
inline constexpr std::string_view kMarketplaceMyInstallsOperationName = "MarketplaceMyInstalls";
inline constexpr std::string_view kMarketplaceGridClientModsIsolatedDocument = R"gql(query MarketplaceGridClientMods($appId: BigInt!, $gridId: BigInt!) {
  gridClientMods(appId: $appId, gridId: $gridId) {
    attachmentId
    listingId
    listingName
    versionId
    sourceKind
    authorKind
    authorRef
    serverVersionId
    clientVersionId
    clientArtifactHash
    gridId
    capabilitySummaryJson
    capabilityHash
    authorCapabilitySummaryJson
    authorCapabilityHash
    callerConsented
    callerTrustsAuthor
  }
})gql";
inline constexpr std::string_view kMarketplaceGridClientModsOperationName = "MarketplaceGridClientMods";
inline constexpr std::string_view kMarketplaceClientArtifactIsolatedDocument = R"gql(query MarketplaceClientArtifact($appId: BigInt!, $listingId: String, $attachmentId: String, $versionId: String) {
  playerCodeClientArtifact(
    appId: $appId
    listingId: $listingId
    attachmentId: $attachmentId
    versionId: $versionId
  ) {
    versionId
    artifactHash
    artifactBase64
    sizeBytes
    abiVersion
    contractJson
    clientFuelPerDispatch
  }
})gql";
inline constexpr std::string_view kMarketplaceClientArtifactOperationName = "MarketplaceClientArtifact";
inline constexpr std::string_view kMarketplaceTrustGridAuthorIsolatedDocument = R"gql(mutation MarketplaceTrustGridAuthor($appId: BigInt!, $gridId: BigInt!, $authorKind: PlayerCodeOwnerKind!, $authorRef: BigInt!, $consentCapabilityHash: String!) {
  trustGridAuthor(
    appId: $appId
    gridId: $gridId
    authorKind: $authorKind
    authorRef: $authorRef
    consentCapabilityHash: $consentCapabilityHash
  )
})gql";
inline constexpr std::string_view kMarketplaceTrustGridAuthorOperationName = "MarketplaceTrustGridAuthor";
inline constexpr std::string_view kMarketplacePublishListingIsolatedDocument = R"gql(mutation MarketplacePublishListing($input: PublishPlayerCodeInput!) {
  publishPlayerCode(input: $input) {
    ...PlayerCodeListingFields
    admissionState
    latestVersionId
  }
}

fragment PlayerCodeListingFields on PlayerCodeListing {
  listingId
  appId
  ownerKind
  ownerRef
  name
  description
  mediaJson
  licenseMode
  acquisitionMode
  priceCents
  rentIntervalDays
  windowDays
  unitBudget
  status
  createdAt
})gql";
inline constexpr std::string_view kMarketplacePublishListingOperationName = "MarketplacePublishListing";
inline constexpr std::string_view kMarketplacePublishVersionIsolatedDocument = R"gql(mutation MarketplacePublishVersion($input: PublishPlayerCodeVersionInput!) {
  publishPlayerCodeVersion(input: $input) {
    ...PlayerCodeListingVersionFields
  }
}

fragment PlayerCodeListingVersionFields on PlayerCodeListingVersion {
  versionId
  listingId
  versionNo
  serverArtifactHashes
  clientArtifactHashes
  requirements {
    serverArtifactHash
    clientArtifactHash
  }
  capabilitySummaryJson
  capabilityHash
  openSource
  licenseText
  createdAt
})gql";
inline constexpr std::string_view kMarketplacePublishVersionOperationName = "MarketplacePublishVersion";
inline constexpr std::string_view kMarketplaceAcquireIsolatedDocument = R"gql(mutation MarketplaceAcquire($appId: BigInt!, $listingId: String!) {
  acquirePlayerCode(appId: $appId, listingId: $listingId) {
    ...PlayerCodeAcquisitionFields
  }
}

fragment PlayerCodeAcquisitionFields on PlayerCodeAcquisition {
  acquisitionId
  listingId
  appId
  mode
  status
  expiresAt
  unitBudget
  unitsConsumed
  acquiredAt
})gql";
inline constexpr std::string_view kMarketplaceAcquireOperationName = "MarketplaceAcquire";
inline constexpr std::string_view kMarketplaceInstallIsolatedDocument = R"gql(mutation MarketplaceInstall($appId: BigInt!, $acquisitionId: String!, $consentCapabilityHash: String!, $gridId: BigInt, $versionId: String) {
  installPlayerCode(
    appId: $appId
    acquisitionId: $acquisitionId
    consentCapabilityHash: $consentCapabilityHash
    gridId: $gridId
    versionId: $versionId
  ) {
    ...PlayerCodeInstallFields
  }
}

fragment PlayerCodeInstallFields on PlayerCodeInstall {
  installId
  acquisitionId
  listingId
  appId
  pinnedVersionId
  targetGridId
  consentedCapabilityHash
  status
  createdAt
})gql";
inline constexpr std::string_view kMarketplaceInstallOperationName = "MarketplaceInstall";
inline constexpr std::string_view kMarketplaceUninstallIsolatedDocument = R"gql(mutation MarketplaceUninstall($appId: BigInt!, $installId: String!) {
  uninstallPlayerCode(appId: $appId, installId: $installId)
})gql";
inline constexpr std::string_view kMarketplaceUninstallOperationName = "MarketplaceUninstall";
inline constexpr std::string_view kMarketplaceConsentGridClientModIsolatedDocument = R"gql(mutation MarketplaceConsentGridClientMod($appId: BigInt!, $attachmentId: String!, $consentCapabilityHash: String!) {
  consentGridClientMod(
    appId: $appId
    attachmentId: $attachmentId
    consentCapabilityHash: $consentCapabilityHash
  )
})gql";
inline constexpr std::string_view kMarketplaceConsentGridClientModOperationName = "MarketplaceConsentGridClientMod";
inline constexpr std::string_view kMarketplaceGridClaimPolicyIsolatedDocument = R"gql(query MarketplaceGridClaimPolicy($appId: BigInt!) {
  gridClaimPolicy(appId: $appId)
})gql";
inline constexpr std::string_view kMarketplaceGridClaimPolicyOperationName = "MarketplaceGridClaimPolicy";
inline constexpr std::string_view kMarketplaceGridClaimRequestsIsolatedDocument = R"gql(query MarketplaceGridClaimRequests($appId: BigInt!) {
  gridClaimRequests(appId: $appId) {
    ...GridClaimRequestFields
  }
}

fragment GridClaimRequestFields on GridClaimRequest {
  requestId
  appId
  gridId
  requesterUserId
  status
  createdAt
})gql";
inline constexpr std::string_view kMarketplaceGridClaimRequestsOperationName = "MarketplaceGridClaimRequests";
inline constexpr std::string_view kMarketplaceClaimGridOwnershipIsolatedDocument = R"gql(mutation MarketplaceClaimGridOwnership($appId: BigInt!, $gridId: BigInt!) {
  claimGridOwnership(appId: $appId, gridId: $gridId) {
    policy
    ownershipAssigned
    claimRequestId
  }
})gql";
inline constexpr std::string_view kMarketplaceClaimGridOwnershipOperationName = "MarketplaceClaimGridOwnership";
inline constexpr std::string_view kMarketplaceClaimGridChunkIsolatedDocument = R"gql(mutation MarketplaceClaimGridChunk($appId: BigInt!, $chunk: ChunkCoordinatesInput!) {
  claimGridChunk(appId: $appId, chunk: $chunk) {
    gridId
    lowChunk {
      x
      y
      z
    }
    highChunk {
      x
      y
      z
    }
    policy
    ownership {
      gridOwnershipId
      ownerKind
      ownerRef
      tenure
      acquiredVia
      acquiredAt
      expiresAt
    }
    moddable
    effectivePermissionKeys
  }
})gql";
inline constexpr std::string_view kMarketplaceClaimGridChunkOperationName = "MarketplaceClaimGridChunk";
inline constexpr std::string_view kMarketplaceReleaseClaimedGridIsolatedDocument = R"gql(mutation MarketplaceReleaseClaimedGrid($appId: BigInt!, $gridId: BigInt!) {
  releaseClaimedGrid(appId: $appId, gridId: $gridId) {
    gridId
    lowChunk {
      x
      y
      z
    }
    highChunk {
      x
      y
      z
    }
    policy
    released
  }
})gql";
inline constexpr std::string_view kMarketplaceReleaseClaimedGridOperationName = "MarketplaceReleaseClaimedGrid";
inline constexpr std::string_view kMarketplaceDecideGridClaimIsolatedDocument = R"gql(mutation MarketplaceDecideGridClaim($appId: BigInt!, $requestId: String!, $approve: Boolean!) {
  decideGridClaim(appId: $appId, requestId: $requestId, approve: $approve) {
    ...GridClaimRequestFields
  }
}

fragment GridClaimRequestFields on GridClaimRequest {
  requestId
  appId
  gridId
  requesterUserId
  status
  createdAt
})gql";
inline constexpr std::string_view kMarketplaceDecideGridClaimOperationName = "MarketplaceDecideGridClaim";
inline constexpr std::string_view kMarketplaceIssueGridClaimInviteIsolatedDocument = R"gql(mutation MarketplaceIssueGridClaimInvite($appId: BigInt!, $gridId: BigInt!, $inviteeUserId: BigInt!) {
  issueGridClaimInvite(
    appId: $appId
    gridId: $gridId
    inviteeUserId: $inviteeUserId
  )
})gql";
inline constexpr std::string_view kMarketplaceIssueGridClaimInviteOperationName = "MarketplaceIssueGridClaimInvite";
inline constexpr std::string_view kMarketplaceRenewAcquisitionIsolatedDocument = R"gql(mutation MarketplaceRenewAcquisition($appId: BigInt!, $acquisitionId: String!) {
  renewPlayerCodeAcquisition(appId: $appId, acquisitionId: $acquisitionId) {
    ...PlayerCodeAcquisitionFields
  }
}

fragment PlayerCodeAcquisitionFields on PlayerCodeAcquisition {
  acquisitionId
  listingId
  appId
  mode
  status
  expiresAt
  unitBudget
  unitsConsumed
  acquiredAt
})gql";
inline constexpr std::string_view kMarketplaceRenewAcquisitionOperationName = "MarketplaceRenewAcquisition";
inline constexpr std::string_view kMarketplaceTopUpAcquisitionIsolatedDocument = R"gql(mutation MarketplaceTopUpAcquisition($appId: BigInt!, $acquisitionId: String!) {
  topUpPlayerCodeAcquisition(appId: $appId, acquisitionId: $acquisitionId) {
    ...PlayerCodeAcquisitionFields
  }
}

fragment PlayerCodeAcquisitionFields on PlayerCodeAcquisition {
  acquisitionId
  listingId
  appId
  mode
  status
  expiresAt
  unitBudget
  unitsConsumed
  acquiredAt
})gql";
inline constexpr std::string_view kMarketplaceTopUpAcquisitionOperationName = "MarketplaceTopUpAcquisition";
inline constexpr std::string_view kMarketplaceRefundAcquisitionIsolatedDocument = R"gql(mutation MarketplaceRefundAcquisition($appId: BigInt!, $acquisitionId: String!) {
  refundPlayerCodeAcquisition(appId: $appId, acquisitionId: $acquisitionId)
})gql";
inline constexpr std::string_view kMarketplaceRefundAcquisitionOperationName = "MarketplaceRefundAcquisition";
inline constexpr std::string_view kMarketplaceGridListingsIsolatedDocument = R"gql(query MarketplaceGridListings($appId: BigInt!) {
  gridListings(appId: $appId) {
    gridListingId
    appId
    kind
    name
    description
    priceCents
    conferredPermissionKeys
    resalePolicy
  }
})gql";
inline constexpr std::string_view kMarketplaceGridListingsOperationName = "MarketplaceGridListings";
inline constexpr std::string_view kMarketplacePurchaseGridIsolatedDocument = R"gql(mutation MarketplacePurchaseGrid($appId: BigInt!, $gridListingId: String!, $chunkX: Int, $chunkY: Int, $chunkZ: Int) {
  purchaseGrid(
    appId: $appId
    gridListingId: $gridListingId
    chunkX: $chunkX
    chunkY: $chunkY
    chunkZ: $chunkZ
  ) {
    gridId
    ownershipAssigned
  }
})gql";
inline constexpr std::string_view kMarketplacePurchaseGridOperationName = "MarketplacePurchaseGrid";

/// marketplace/MarketplaceAdmin.graphql
inline constexpr std::string_view kMarketplaceAdminDocument = R"gql(fragment PlayerCodeListingAdminFields on PlayerCodeListing {
  listingId
  appId
  ownerKind
  ownerRef
  name
  description
  mediaJson
  licenseMode
  acquisitionMode
  status
  createdAt
  updatedAt
}

fragment PlayerCodeAcquisitionAdminFields on PlayerCodeAcquisition {
  acquisitionId
  listingId
  appId
  acquirerUserId
  mode
  status
  acquiredAt
  revokedAt
}

query MarketplaceAdmissionQueue($appId: BigInt!) {
  appCodeAdmissionQueue(appId: $appId) {
    listing {
      ...PlayerCodeListingAdminFields
    }
    admissionState
    admissionId
    matchedSubjectKind
  }
}

query MarketplaceAppListings($appId: BigInt!, $includeDelisted: Boolean) {
  appPlayerCodeListings(appId: $appId, includeDelisted: $includeDelisted) {
    ...PlayerCodeListingAdminFields
  }
}

query MarketplaceAppListingVersions($appId: BigInt!, $listingId: String!) {
  appPlayerCodeListingVersions(appId: $appId, listingId: $listingId) {
    versionId
    listingId
    versionNo
    serverArtifactHashes
    clientArtifactHashes
    requirements {
      serverArtifactHash
      clientArtifactHash
    }
    capabilitySummaryJson
    capabilityHash
    openSource
    licenseText
    createdAt
  }
}

query MarketplaceAppAcquisitions($appId: BigInt!) {
  appPlayerCodeAcquisitions(appId: $appId) {
    ...PlayerCodeAcquisitionAdminFields
  }
}

mutation MarketplaceTransferListing($input: TransferPlayerCodeListingInput!) {
  transferPlayerCodeListing(input: $input) {
    ...PlayerCodeListingAdminFields
  }
}

mutation MarketplaceSetListingStatus(
  $appId: BigInt!
  $listingId: String!
  $status: PlayerCodeListingStatus!
) {
  setPlayerCodeListingStatus(
    appId: $appId
    listingId: $listingId
    status: $status
  ) {
    ...PlayerCodeListingAdminFields
  }
}

mutation MarketplaceSetGridClaimPolicy(
  $appId: BigInt!
  $policy: GridClaimPolicy!
  $approverUserIds: [BigInt!]
) {
  setAppGridClaimPolicy(
    appId: $appId
    policy: $policy
    approverUserIds: $approverUserIds
  )
}

mutation MarketplaceSetListingPricing($input: SetListingPricingInput!) {
  setListingPricing(input: $input)
}

mutation MarketplaceSetOrgShare($appId: BigInt!, $bps: Int!) {
  setAppMarketplaceOrgShare(appId: $appId, bps: $bps)
}

mutation MarketplaceBeginSellerOnboarding($country: String!) {
  beginSellerOnboarding(country: $country) {
    status
    onboardingUrl
    unavailableReason
  }
}

mutation MarketplaceCreateAccountSession($country: String!) {
  createSellerAccountSession(country: $country) {
    clientSecret
    publishableKey
    accountRef
    onboardingComplete
    expiresAt
  }
}

mutation MarketplaceCreateOrgAccountSession($orgId: BigInt!, $country: String!) {
  createOrgSellerAccountSession(orgId: $orgId, country: $country) {
    clientSecret
    publishableKey
    accountRef
    onboardingComplete
    expiresAt
  }
}

mutation MarketplaceBeginOrgSellerOnboarding($orgId: BigInt!, $country: String!) {
  beginOrgSellerOnboarding(orgId: $orgId, country: $country) {
    status
    onboardingUrl
    unavailableReason
  }
}

query MarketplaceMySellerBalance {
  mySellerPayoutBalance {
    partyKind
    partyRef
    pendingCents
    payableCents
    reservedCents
    onboardingStatus
    payoutsFrozen
  }
}

mutation MarketplaceRequestPayout {
  requestSellerPayout
}

mutation MarketplaceSpendPayoutToWallet($amountCents: Int!) {
  spendPayoutBalanceToWallet(amountCents: $amountCents)
}

query MarketplaceCommerceRiskQueue($appId: BigInt!) {
  commerceRiskQueue(appId: $appId) {
    flagId
    appId
    kind
    orderId
    subjectKind
    subjectRef
    detail
    status
    createdAt
  }
}

mutation MarketplaceDecideRiskFlag(
  $appId: BigInt!
  $flagId: String!
  $release: Boolean!
) {
  decideCommerceRiskFlag(appId: $appId, flagId: $flagId, release: $release)
}

mutation MarketplaceCreateGridListing($input: CreateGridListingInput!) {
  createGridListing(input: $input) {
    gridListingId
    appId
    kind
    name
    priceCents
    resalePolicy
    status
  }
})gql";
inline constexpr std::string_view kMarketplaceAdmissionQueueIsolatedDocument = R"gql(query MarketplaceAdmissionQueue($appId: BigInt!) {
  appCodeAdmissionQueue(appId: $appId) {
    listing {
      ...PlayerCodeListingAdminFields
    }
    admissionState
    admissionId
    matchedSubjectKind
  }
}

fragment PlayerCodeListingAdminFields on PlayerCodeListing {
  listingId
  appId
  ownerKind
  ownerRef
  name
  description
  mediaJson
  licenseMode
  acquisitionMode
  status
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kMarketplaceAdmissionQueueOperationName = "MarketplaceAdmissionQueue";
inline constexpr std::string_view kMarketplaceAppListingsIsolatedDocument = R"gql(query MarketplaceAppListings($appId: BigInt!, $includeDelisted: Boolean) {
  appPlayerCodeListings(appId: $appId, includeDelisted: $includeDelisted) {
    ...PlayerCodeListingAdminFields
  }
}

fragment PlayerCodeListingAdminFields on PlayerCodeListing {
  listingId
  appId
  ownerKind
  ownerRef
  name
  description
  mediaJson
  licenseMode
  acquisitionMode
  status
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kMarketplaceAppListingsOperationName = "MarketplaceAppListings";
inline constexpr std::string_view kMarketplaceAppListingVersionsIsolatedDocument = R"gql(query MarketplaceAppListingVersions($appId: BigInt!, $listingId: String!) {
  appPlayerCodeListingVersions(appId: $appId, listingId: $listingId) {
    versionId
    listingId
    versionNo
    serverArtifactHashes
    clientArtifactHashes
    requirements {
      serverArtifactHash
      clientArtifactHash
    }
    capabilitySummaryJson
    capabilityHash
    openSource
    licenseText
    createdAt
  }
})gql";
inline constexpr std::string_view kMarketplaceAppListingVersionsOperationName = "MarketplaceAppListingVersions";
inline constexpr std::string_view kMarketplaceAppAcquisitionsIsolatedDocument = R"gql(query MarketplaceAppAcquisitions($appId: BigInt!) {
  appPlayerCodeAcquisitions(appId: $appId) {
    ...PlayerCodeAcquisitionAdminFields
  }
}

fragment PlayerCodeAcquisitionAdminFields on PlayerCodeAcquisition {
  acquisitionId
  listingId
  appId
  acquirerUserId
  mode
  status
  acquiredAt
  revokedAt
})gql";
inline constexpr std::string_view kMarketplaceAppAcquisitionsOperationName = "MarketplaceAppAcquisitions";
inline constexpr std::string_view kMarketplaceTransferListingIsolatedDocument = R"gql(mutation MarketplaceTransferListing($input: TransferPlayerCodeListingInput!) {
  transferPlayerCodeListing(input: $input) {
    ...PlayerCodeListingAdminFields
  }
}

fragment PlayerCodeListingAdminFields on PlayerCodeListing {
  listingId
  appId
  ownerKind
  ownerRef
  name
  description
  mediaJson
  licenseMode
  acquisitionMode
  status
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kMarketplaceTransferListingOperationName = "MarketplaceTransferListing";
inline constexpr std::string_view kMarketplaceSetListingStatusIsolatedDocument = R"gql(mutation MarketplaceSetListingStatus($appId: BigInt!, $listingId: String!, $status: PlayerCodeListingStatus!) {
  setPlayerCodeListingStatus(
    appId: $appId
    listingId: $listingId
    status: $status
  ) {
    ...PlayerCodeListingAdminFields
  }
}

fragment PlayerCodeListingAdminFields on PlayerCodeListing {
  listingId
  appId
  ownerKind
  ownerRef
  name
  description
  mediaJson
  licenseMode
  acquisitionMode
  status
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kMarketplaceSetListingStatusOperationName = "MarketplaceSetListingStatus";
inline constexpr std::string_view kMarketplaceSetGridClaimPolicyIsolatedDocument = R"gql(mutation MarketplaceSetGridClaimPolicy($appId: BigInt!, $policy: GridClaimPolicy!, $approverUserIds: [BigInt!]) {
  setAppGridClaimPolicy(
    appId: $appId
    policy: $policy
    approverUserIds: $approverUserIds
  )
})gql";
inline constexpr std::string_view kMarketplaceSetGridClaimPolicyOperationName = "MarketplaceSetGridClaimPolicy";
inline constexpr std::string_view kMarketplaceSetListingPricingIsolatedDocument = R"gql(mutation MarketplaceSetListingPricing($input: SetListingPricingInput!) {
  setListingPricing(input: $input)
})gql";
inline constexpr std::string_view kMarketplaceSetListingPricingOperationName = "MarketplaceSetListingPricing";
inline constexpr std::string_view kMarketplaceSetOrgShareIsolatedDocument = R"gql(mutation MarketplaceSetOrgShare($appId: BigInt!, $bps: Int!) {
  setAppMarketplaceOrgShare(appId: $appId, bps: $bps)
})gql";
inline constexpr std::string_view kMarketplaceSetOrgShareOperationName = "MarketplaceSetOrgShare";
inline constexpr std::string_view kMarketplaceBeginSellerOnboardingIsolatedDocument = R"gql(mutation MarketplaceBeginSellerOnboarding($country: String!) {
  beginSellerOnboarding(country: $country) {
    status
    onboardingUrl
    unavailableReason
  }
})gql";
inline constexpr std::string_view kMarketplaceBeginSellerOnboardingOperationName = "MarketplaceBeginSellerOnboarding";
inline constexpr std::string_view kMarketplaceCreateAccountSessionIsolatedDocument = R"gql(mutation MarketplaceCreateAccountSession($country: String!) {
  createSellerAccountSession(country: $country) {
    clientSecret
    publishableKey
    accountRef
    onboardingComplete
    expiresAt
  }
})gql";
inline constexpr std::string_view kMarketplaceCreateAccountSessionOperationName = "MarketplaceCreateAccountSession";
inline constexpr std::string_view kMarketplaceCreateOrgAccountSessionIsolatedDocument = R"gql(mutation MarketplaceCreateOrgAccountSession($orgId: BigInt!, $country: String!) {
  createOrgSellerAccountSession(orgId: $orgId, country: $country) {
    clientSecret
    publishableKey
    accountRef
    onboardingComplete
    expiresAt
  }
})gql";
inline constexpr std::string_view kMarketplaceCreateOrgAccountSessionOperationName = "MarketplaceCreateOrgAccountSession";
inline constexpr std::string_view kMarketplaceBeginOrgSellerOnboardingIsolatedDocument = R"gql(mutation MarketplaceBeginOrgSellerOnboarding($orgId: BigInt!, $country: String!) {
  beginOrgSellerOnboarding(orgId: $orgId, country: $country) {
    status
    onboardingUrl
    unavailableReason
  }
})gql";
inline constexpr std::string_view kMarketplaceBeginOrgSellerOnboardingOperationName = "MarketplaceBeginOrgSellerOnboarding";
inline constexpr std::string_view kMarketplaceMySellerBalanceIsolatedDocument = R"gql(query MarketplaceMySellerBalance {
  mySellerPayoutBalance {
    partyKind
    partyRef
    pendingCents
    payableCents
    reservedCents
    onboardingStatus
    payoutsFrozen
  }
})gql";
inline constexpr std::string_view kMarketplaceMySellerBalanceOperationName = "MarketplaceMySellerBalance";
inline constexpr std::string_view kMarketplaceRequestPayoutIsolatedDocument = R"gql(mutation MarketplaceRequestPayout {
  requestSellerPayout
})gql";
inline constexpr std::string_view kMarketplaceRequestPayoutOperationName = "MarketplaceRequestPayout";
inline constexpr std::string_view kMarketplaceSpendPayoutToWalletIsolatedDocument = R"gql(mutation MarketplaceSpendPayoutToWallet($amountCents: Int!) {
  spendPayoutBalanceToWallet(amountCents: $amountCents)
})gql";
inline constexpr std::string_view kMarketplaceSpendPayoutToWalletOperationName = "MarketplaceSpendPayoutToWallet";
inline constexpr std::string_view kMarketplaceCommerceRiskQueueIsolatedDocument = R"gql(query MarketplaceCommerceRiskQueue($appId: BigInt!) {
  commerceRiskQueue(appId: $appId) {
    flagId
    appId
    kind
    orderId
    subjectKind
    subjectRef
    detail
    status
    createdAt
  }
})gql";
inline constexpr std::string_view kMarketplaceCommerceRiskQueueOperationName = "MarketplaceCommerceRiskQueue";
inline constexpr std::string_view kMarketplaceDecideRiskFlagIsolatedDocument = R"gql(mutation MarketplaceDecideRiskFlag($appId: BigInt!, $flagId: String!, $release: Boolean!) {
  decideCommerceRiskFlag(appId: $appId, flagId: $flagId, release: $release)
})gql";
inline constexpr std::string_view kMarketplaceDecideRiskFlagOperationName = "MarketplaceDecideRiskFlag";
inline constexpr std::string_view kMarketplaceCreateGridListingIsolatedDocument = R"gql(mutation MarketplaceCreateGridListing($input: CreateGridListingInput!) {
  createGridListing(input: $input) {
    gridListingId
    appId
    kind
    name
    priceCents
    resalePolicy
    status
  }
})gql";
inline constexpr std::string_view kMarketplaceCreateGridListingOperationName = "MarketplaceCreateGridListing";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "MarketplaceListings") return kMarketplaceListingsIsolatedDocument;
  if (operationName == "MarketplaceListingVersions") return kMarketplaceListingVersionsIsolatedDocument;
  if (operationName == "MarketplaceMyAcquisitions") return kMarketplaceMyAcquisitionsIsolatedDocument;
  if (operationName == "MarketplaceMyInstalls") return kMarketplaceMyInstallsIsolatedDocument;
  if (operationName == "MarketplaceGridClientMods") return kMarketplaceGridClientModsIsolatedDocument;
  if (operationName == "MarketplaceClientArtifact") return kMarketplaceClientArtifactIsolatedDocument;
  if (operationName == "MarketplaceTrustGridAuthor") return kMarketplaceTrustGridAuthorIsolatedDocument;
  if (operationName == "MarketplacePublishListing") return kMarketplacePublishListingIsolatedDocument;
  if (operationName == "MarketplacePublishVersion") return kMarketplacePublishVersionIsolatedDocument;
  if (operationName == "MarketplaceAcquire") return kMarketplaceAcquireIsolatedDocument;
  if (operationName == "MarketplaceInstall") return kMarketplaceInstallIsolatedDocument;
  if (operationName == "MarketplaceUninstall") return kMarketplaceUninstallIsolatedDocument;
  if (operationName == "MarketplaceConsentGridClientMod") return kMarketplaceConsentGridClientModIsolatedDocument;
  if (operationName == "MarketplaceGridClaimPolicy") return kMarketplaceGridClaimPolicyIsolatedDocument;
  if (operationName == "MarketplaceGridClaimRequests") return kMarketplaceGridClaimRequestsIsolatedDocument;
  if (operationName == "MarketplaceClaimGridOwnership") return kMarketplaceClaimGridOwnershipIsolatedDocument;
  if (operationName == "MarketplaceClaimGridChunk") return kMarketplaceClaimGridChunkIsolatedDocument;
  if (operationName == "MarketplaceReleaseClaimedGrid") return kMarketplaceReleaseClaimedGridIsolatedDocument;
  if (operationName == "MarketplaceDecideGridClaim") return kMarketplaceDecideGridClaimIsolatedDocument;
  if (operationName == "MarketplaceIssueGridClaimInvite") return kMarketplaceIssueGridClaimInviteIsolatedDocument;
  if (operationName == "MarketplaceRenewAcquisition") return kMarketplaceRenewAcquisitionIsolatedDocument;
  if (operationName == "MarketplaceTopUpAcquisition") return kMarketplaceTopUpAcquisitionIsolatedDocument;
  if (operationName == "MarketplaceRefundAcquisition") return kMarketplaceRefundAcquisitionIsolatedDocument;
  if (operationName == "MarketplaceGridListings") return kMarketplaceGridListingsIsolatedDocument;
  if (operationName == "MarketplacePurchaseGrid") return kMarketplacePurchaseGridIsolatedDocument;
  if (operationName == "MarketplaceAdmissionQueue") return kMarketplaceAdmissionQueueIsolatedDocument;
  if (operationName == "MarketplaceAppListings") return kMarketplaceAppListingsIsolatedDocument;
  if (operationName == "MarketplaceAppListingVersions") return kMarketplaceAppListingVersionsIsolatedDocument;
  if (operationName == "MarketplaceAppAcquisitions") return kMarketplaceAppAcquisitionsIsolatedDocument;
  if (operationName == "MarketplaceTransferListing") return kMarketplaceTransferListingIsolatedDocument;
  if (operationName == "MarketplaceSetListingStatus") return kMarketplaceSetListingStatusIsolatedDocument;
  if (operationName == "MarketplaceSetGridClaimPolicy") return kMarketplaceSetGridClaimPolicyIsolatedDocument;
  if (operationName == "MarketplaceSetListingPricing") return kMarketplaceSetListingPricingIsolatedDocument;
  if (operationName == "MarketplaceSetOrgShare") return kMarketplaceSetOrgShareIsolatedDocument;
  if (operationName == "MarketplaceBeginSellerOnboarding") return kMarketplaceBeginSellerOnboardingIsolatedDocument;
  if (operationName == "MarketplaceCreateAccountSession") return kMarketplaceCreateAccountSessionIsolatedDocument;
  if (operationName == "MarketplaceCreateOrgAccountSession") return kMarketplaceCreateOrgAccountSessionIsolatedDocument;
  if (operationName == "MarketplaceBeginOrgSellerOnboarding") return kMarketplaceBeginOrgSellerOnboardingIsolatedDocument;
  if (operationName == "MarketplaceMySellerBalance") return kMarketplaceMySellerBalanceIsolatedDocument;
  if (operationName == "MarketplaceRequestPayout") return kMarketplaceRequestPayoutIsolatedDocument;
  if (operationName == "MarketplaceSpendPayoutToWallet") return kMarketplaceSpendPayoutToWalletIsolatedDocument;
  if (operationName == "MarketplaceCommerceRiskQueue") return kMarketplaceCommerceRiskQueueIsolatedDocument;
  if (operationName == "MarketplaceDecideRiskFlag") return kMarketplaceDecideRiskFlagIsolatedDocument;
  if (operationName == "MarketplaceCreateGridListing") return kMarketplaceCreateGridListingIsolatedDocument;
  return {};
}

}  // namespace marketplace

namespace organizations {

/// organizations/CreateOrgRole.graphql
inline constexpr std::string_view kCreateOrgRoleDocument = R"gql(mutation CreateOrgRole($input: CreateOrgRoleInput!) {
  createOrgRole(input: $input) {
    orgRoleId
    orgId
    roleName
    isSystem
    permissions
    description
  }
})gql";
inline constexpr std::string_view kCreateOrgRoleIsolatedDocument = R"gql(mutation CreateOrgRole($input: CreateOrgRoleInput!) {
  createOrgRole(input: $input) {
    orgRoleId
    orgId
    roleName
    isSystem
    permissions
    description
  }
})gql";
inline constexpr std::string_view kCreateOrgRoleOperationName = "CreateOrgRole";

/// organizations/CreateOrgToken.graphql
inline constexpr std::string_view kCreateOrgTokenDocument = R"gql(mutation CreateOrgToken($input: CreateOrgTokenInput!) {
  createOrgToken(input: $input) {
    orgTokenId
    orgId
    token
    label
    isActive
    expiresAt
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateOrgTokenIsolatedDocument = R"gql(mutation CreateOrgToken($input: CreateOrgTokenInput!) {
  createOrgToken(input: $input) {
    orgTokenId
    orgId
    token
    label
    isActive
    expiresAt
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateOrgTokenOperationName = "CreateOrgToken";

/// organizations/CreateOrganization.graphql
inline constexpr std::string_view kCreateOrganizationDocument = R"gql(mutation CreateOrganization($input: CreateOrganizationInput!) {
  createOrganization(input: $input) {
    orgId
    name
    slug
    ownerUserId
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kCreateOrganizationIsolatedDocument = R"gql(mutation CreateOrganization($input: CreateOrganizationInput!) {
  createOrganization(input: $input) {
    orgId
    name
    slug
    ownerUserId
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kCreateOrganizationOperationName = "CreateOrganization";

/// organizations/DeleteOrgRole.graphql
inline constexpr std::string_view kDeleteOrgRoleDocument = R"gql(mutation DeleteOrgRole($orgRoleId: BigInt!) {
  deleteOrgRole(orgRoleId: $orgRoleId)
})gql";
inline constexpr std::string_view kDeleteOrgRoleIsolatedDocument = R"gql(mutation DeleteOrgRole($orgRoleId: BigInt!) {
  deleteOrgRole(orgRoleId: $orgRoleId)
})gql";
inline constexpr std::string_view kDeleteOrgRoleOperationName = "DeleteOrgRole";

/// organizations/InviteOrgMember.graphql
inline constexpr std::string_view kInviteOrgMemberDocument = R"gql(mutation InviteOrgMember($input: InviteOrgMemberInput!) {
  inviteOrgMember(input: $input) {
    orgMemberId
    orgId
    userId
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kInviteOrgMemberIsolatedDocument = R"gql(mutation InviteOrgMember($input: InviteOrgMemberInput!) {
  inviteOrgMember(input: $input) {
    orgMemberId
    orgId
    userId
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kInviteOrgMemberOperationName = "InviteOrgMember";

/// organizations/MemberRoles.graphql
inline constexpr std::string_view kMemberRolesDocument = R"gql(query MemberRoles($orgMemberId: BigInt!) {
  memberRoles(orgMemberId: $orgMemberId) {
    orgRoleId
    orgId
    roleName
    isSystem
    permissions
    description
  }
})gql";
inline constexpr std::string_view kMemberRolesIsolatedDocument = R"gql(query MemberRoles($orgMemberId: BigInt!) {
  memberRoles(orgMemberId: $orgMemberId) {
    orgRoleId
    orgId
    roleName
    isSystem
    permissions
    description
  }
})gql";
inline constexpr std::string_view kMemberRolesOperationName = "MemberRoles";

/// organizations/MyOrganizations.graphql
inline constexpr std::string_view kMyOrganizationsDocument = R"gql(query MyOrganizations {
  myOrganizations {
    org {
      orgId
      slug
      name
      ownerUserId
      status
      createdAt
      updatedAt
    }
    permissions
    roles {
      orgRoleId
      orgId
      roleName
      isSystem
      permissions
    }
    joinedAt
  }
})gql";
inline constexpr std::string_view kMyOrganizationsIsolatedDocument = R"gql(query MyOrganizations {
  myOrganizations {
    org {
      orgId
      slug
      name
      ownerUserId
      status
      createdAt
      updatedAt
    }
    permissions
    roles {
      orgRoleId
      orgId
      roleName
      isSystem
      permissions
    }
    joinedAt
  }
})gql";
inline constexpr std::string_view kMyOrganizationsOperationName = "MyOrganizations";

/// organizations/OrgMembers.graphql
inline constexpr std::string_view kOrgMembersDocument = R"gql(query OrgMembers($orgId: BigInt!) {
  orgMembers(orgId: $orgId) {
    orgMemberId
    orgId
    userId
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kOrgMembersIsolatedDocument = R"gql(query OrgMembers($orgId: BigInt!) {
  orgMembers(orgId: $orgId) {
    orgMemberId
    orgId
    userId
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kOrgMembersOperationName = "OrgMembers";

/// organizations/OrgPermissions.graphql
inline constexpr std::string_view kOrgPermissionsDocument = R"gql(query OrgPermissions {
  orgPermissions {
    permissionKey
    description
    category
  }
})gql";
inline constexpr std::string_view kOrgPermissionsIsolatedDocument = R"gql(query OrgPermissions {
  orgPermissions {
    permissionKey
    description
    category
  }
})gql";
inline constexpr std::string_view kOrgPermissionsOperationName = "OrgPermissions";

/// organizations/OrgRoles.graphql
inline constexpr std::string_view kOrgRolesDocument = R"gql(query OrgRoles($orgId: BigInt!) {
  orgRoles(orgId: $orgId) {
    orgRoleId
    orgId
    roleName
    isSystem
    permissions
    description
  }
})gql";
inline constexpr std::string_view kOrgRolesIsolatedDocument = R"gql(query OrgRoles($orgId: BigInt!) {
  orgRoles(orgId: $orgId) {
    orgRoleId
    orgId
    roleName
    isSystem
    permissions
    description
  }
})gql";
inline constexpr std::string_view kOrgRolesOperationName = "OrgRoles";

/// organizations/OrgTokens.graphql
inline constexpr std::string_view kOrgTokensDocument = R"gql(query OrgTokens($orgId: BigInt!) {
  orgTokens(orgId: $orgId) {
    orgTokenId
    orgId
    label
    isActive
    lastUsedAt
    revokedAt
    expiresAt
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kOrgTokensIsolatedDocument = R"gql(query OrgTokens($orgId: BigInt!) {
  orgTokens(orgId: $orgId) {
    orgTokenId
    orgId
    label
    isActive
    lastUsedAt
    revokedAt
    expiresAt
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kOrgTokensOperationName = "OrgTokens";

/// organizations/Organization.graphql
inline constexpr std::string_view kOrganizationDocument = R"gql(query Organization($id: BigInt!) {
  organization(id: $id) {
    orgId
    name
    slug
    ownerUserId
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kOrganizationIsolatedDocument = R"gql(query Organization($id: BigInt!) {
  organization(id: $id) {
    orgId
    name
    slug
    ownerUserId
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kOrganizationOperationName = "Organization";

/// organizations/OrganizationBySlug.graphql
inline constexpr std::string_view kOrganizationBySlugDocument = R"gql(query OrganizationBySlug($slug: String!) {
  organizationBySlug(slug: $slug) {
    orgId
    name
    slug
    ownerUserId
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kOrganizationBySlugIsolatedDocument = R"gql(query OrganizationBySlug($slug: String!) {
  organizationBySlug(slug: $slug) {
    orgId
    name
    slug
    ownerUserId
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kOrganizationBySlugOperationName = "OrganizationBySlug";

/// organizations/RemoveOrgMember.graphql
inline constexpr std::string_view kRemoveOrgMemberDocument = R"gql(mutation RemoveOrgMember($orgId: BigInt!, $userId: BigInt!) {
  removeOrgMember(orgId: $orgId, userId: $userId)
})gql";
inline constexpr std::string_view kRemoveOrgMemberIsolatedDocument = R"gql(mutation RemoveOrgMember($orgId: BigInt!, $userId: BigInt!) {
  removeOrgMember(orgId: $orgId, userId: $userId)
})gql";
inline constexpr std::string_view kRemoveOrgMemberOperationName = "RemoveOrgMember";

/// organizations/RevokeOrgToken.graphql
inline constexpr std::string_view kRevokeOrgTokenDocument = R"gql(mutation RevokeOrgToken($orgTokenId: BigInt!) {
  revokeOrgToken(orgTokenId: $orgTokenId)
})gql";
inline constexpr std::string_view kRevokeOrgTokenIsolatedDocument = R"gql(mutation RevokeOrgToken($orgTokenId: BigInt!) {
  revokeOrgToken(orgTokenId: $orgTokenId)
})gql";
inline constexpr std::string_view kRevokeOrgTokenOperationName = "RevokeOrgToken";

/// organizations/SetOrgStatus.graphql
inline constexpr std::string_view kSetOrgStatusDocument = R"gql(mutation SetOrgStatus($orgId: BigInt!, $status: String!) {
  setOrgStatus(orgId: $orgId, status: $status) {
    orgId
    status
    updatedAt
  }
})gql";
inline constexpr std::string_view kSetOrgStatusIsolatedDocument = R"gql(mutation SetOrgStatus($orgId: BigInt!, $status: String!) {
  setOrgStatus(orgId: $orgId, status: $status) {
    orgId
    status
    updatedAt
  }
})gql";
inline constexpr std::string_view kSetOrgStatusOperationName = "SetOrgStatus";

/// organizations/UpdateOrgMemberRoles.graphql
inline constexpr std::string_view kUpdateOrgMemberRolesDocument = R"gql(mutation UpdateOrgMemberRoles(
  $orgId: BigInt!
  $userId: BigInt!
  $roleIds: [BigInt!]!
) {
  updateOrgMemberRoles(orgId: $orgId, userId: $userId, roleIds: $roleIds) {
    orgMemberId
    orgId
    userId
    status
  }
})gql";
inline constexpr std::string_view kUpdateOrgMemberRolesIsolatedDocument = R"gql(mutation UpdateOrgMemberRoles($orgId: BigInt!, $userId: BigInt!, $roleIds: [BigInt!]!) {
  updateOrgMemberRoles(orgId: $orgId, userId: $userId, roleIds: $roleIds) {
    orgMemberId
    orgId
    userId
    status
  }
})gql";
inline constexpr std::string_view kUpdateOrgMemberRolesOperationName = "UpdateOrgMemberRoles";

/// organizations/UpdateOrgRole.graphql
inline constexpr std::string_view kUpdateOrgRoleDocument = R"gql(mutation UpdateOrgRole($orgRoleId: BigInt!, $input: UpdateOrgRoleInput!) {
  updateOrgRole(orgRoleId: $orgRoleId, input: $input) {
    orgRoleId
    orgId
    roleName
    isSystem
    permissions
    description
  }
})gql";
inline constexpr std::string_view kUpdateOrgRoleIsolatedDocument = R"gql(mutation UpdateOrgRole($orgRoleId: BigInt!, $input: UpdateOrgRoleInput!) {
  updateOrgRole(orgRoleId: $orgRoleId, input: $input) {
    orgRoleId
    orgId
    roleName
    isSystem
    permissions
    description
  }
})gql";
inline constexpr std::string_view kUpdateOrgRoleOperationName = "UpdateOrgRole";

/// organizations/UpdateOrgToken.graphql
inline constexpr std::string_view kUpdateOrgTokenDocument = R"gql(mutation UpdateOrgToken($orgTokenId: BigInt!, $input: UpdateOrgTokenInput!) {
  updateOrgToken(orgTokenId: $orgTokenId, input: $input) {
    orgTokenId
    label
    isActive
    expiresAt
    revokedAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateOrgTokenIsolatedDocument = R"gql(mutation UpdateOrgToken($orgTokenId: BigInt!, $input: UpdateOrgTokenInput!) {
  updateOrgToken(orgTokenId: $orgTokenId, input: $input) {
    orgTokenId
    label
    isActive
    expiresAt
    revokedAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateOrgTokenOperationName = "UpdateOrgToken";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "CreateOrgRole") return kCreateOrgRoleIsolatedDocument;
  if (operationName == "CreateOrgToken") return kCreateOrgTokenIsolatedDocument;
  if (operationName == "CreateOrganization") return kCreateOrganizationIsolatedDocument;
  if (operationName == "DeleteOrgRole") return kDeleteOrgRoleIsolatedDocument;
  if (operationName == "InviteOrgMember") return kInviteOrgMemberIsolatedDocument;
  if (operationName == "MemberRoles") return kMemberRolesIsolatedDocument;
  if (operationName == "MyOrganizations") return kMyOrganizationsIsolatedDocument;
  if (operationName == "OrgMembers") return kOrgMembersIsolatedDocument;
  if (operationName == "OrgPermissions") return kOrgPermissionsIsolatedDocument;
  if (operationName == "OrgRoles") return kOrgRolesIsolatedDocument;
  if (operationName == "OrgTokens") return kOrgTokensIsolatedDocument;
  if (operationName == "Organization") return kOrganizationIsolatedDocument;
  if (operationName == "OrganizationBySlug") return kOrganizationBySlugIsolatedDocument;
  if (operationName == "RemoveOrgMember") return kRemoveOrgMemberIsolatedDocument;
  if (operationName == "RevokeOrgToken") return kRevokeOrgTokenIsolatedDocument;
  if (operationName == "SetOrgStatus") return kSetOrgStatusIsolatedDocument;
  if (operationName == "UpdateOrgMemberRoles") return kUpdateOrgMemberRolesIsolatedDocument;
  if (operationName == "UpdateOrgRole") return kUpdateOrgRoleIsolatedDocument;
  if (operationName == "UpdateOrgToken") return kUpdateOrgTokenIsolatedDocument;
  return {};
}

}  // namespace organizations

namespace payments {

/// payments/CapturePaypalCheckout.graphql
inline constexpr std::string_view kCapturePaypalCheckoutDocument = R"gql(mutation CapturePaypalCheckout($orderId: String!, $idempotencyKey: String) {
  capturePaypalCheckout(orderId: $orderId, idempotencyKey: $idempotencyKey) {
    checkoutId
    userId
    provider
    purpose
    status
    amountCents
    currency
    externalId
    externalUrl
    orgId
    appId
    tierId
    error
    createdAt
    completedAt
    expiresAt
  }
})gql";
inline constexpr std::string_view kCapturePaypalCheckoutIsolatedDocument = R"gql(mutation CapturePaypalCheckout($orderId: String!, $idempotencyKey: String) {
  capturePaypalCheckout(orderId: $orderId, idempotencyKey: $idempotencyKey) {
    checkoutId
    userId
    provider
    purpose
    status
    amountCents
    currency
    externalId
    externalUrl
    orgId
    appId
    tierId
    error
    createdAt
    completedAt
    expiresAt
  }
})gql";
inline constexpr std::string_view kCapturePaypalCheckoutOperationName = "CapturePaypalCheckout";

/// payments/Checkouts.graphql
inline constexpr std::string_view kCheckoutsDocument = R"gql(query Checkouts($filter: CheckoutFilterInput, $limit: Int, $offset: Int) {
  checkouts(filter: $filter, limit: $limit, offset: $offset) {
    items {
      checkoutId
      userId
      provider
      purpose
      status
      amountCents
      currency
      externalId
      externalUrl
      orgId
      appId
      tierId
      createdAt
      completedAt
      expiresAt
    }
    pageInfo {
      totalCount
      limit
      offset
    }
  }
}

query CheckoutsConnection(
  $first: Int
  $after: String
  $filter: CheckoutFilterInput
) {
  checkoutsConnection(first: $first, after: $after, filter: $filter) {
    edges {
      cursor
      node {
        checkoutId
        userId
        provider
        purpose
        status
        amountCents
        currency
        externalId
        externalUrl
        orgId
        appId
        tierId
        error
        createdAt
        completedAt
        expiresAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kCheckoutsIsolatedDocument = R"gql(query Checkouts($filter: CheckoutFilterInput, $limit: Int, $offset: Int) {
  checkouts(filter: $filter, limit: $limit, offset: $offset) {
    items {
      checkoutId
      userId
      provider
      purpose
      status
      amountCents
      currency
      externalId
      externalUrl
      orgId
      appId
      tierId
      createdAt
      completedAt
      expiresAt
    }
    pageInfo {
      totalCount
      limit
      offset
    }
  }
})gql";
inline constexpr std::string_view kCheckoutsOperationName = "Checkouts";
inline constexpr std::string_view kCheckoutsConnectionIsolatedDocument = R"gql(query CheckoutsConnection($first: Int, $after: String, $filter: CheckoutFilterInput) {
  checkoutsConnection(first: $first, after: $after, filter: $filter) {
    edges {
      cursor
      node {
        checkoutId
        userId
        provider
        purpose
        status
        amountCents
        currency
        externalId
        externalUrl
        orgId
        appId
        tierId
        error
        createdAt
        completedAt
        expiresAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kCheckoutsConnectionOperationName = "CheckoutsConnection";

/// payments/CreateCheckout.graphql
inline constexpr std::string_view kCreateCheckoutDocument = R"gql(mutation CreateCheckout($input: CreateCheckoutInput!) {
  createCheckout(input: $input) {
    checkoutId
    userId
    provider
    purpose
    status
    amountCents
    currency
    externalId
    externalUrl
    orgId
    appId
    tierId
    error
    createdAt
    completedAt
    expiresAt
  }
})gql";
inline constexpr std::string_view kCreateCheckoutIsolatedDocument = R"gql(mutation CreateCheckout($input: CreateCheckoutInput!) {
  createCheckout(input: $input) {
    checkoutId
    userId
    provider
    purpose
    status
    amountCents
    currency
    externalId
    externalUrl
    orgId
    appId
    tierId
    error
    createdAt
    completedAt
    expiresAt
  }
})gql";
inline constexpr std::string_view kCreateCheckoutOperationName = "CreateCheckout";

/// payments/MyCheckouts.graphql
inline constexpr std::string_view kMyCheckoutsDocument = R"gql(query MyCheckouts($limit: Int, $offset: Int) {
  myCheckouts(limit: $limit, offset: $offset) {
    items {
      checkoutId
      userId
      provider
      purpose
      status
      amountCents
      currency
      externalId
      externalUrl
      orgId
      appId
      tierId
      error
      createdAt
      completedAt
      expiresAt
    }
    pageInfo {
      totalCount
      limit
      offset
    }
  }
}

query MyCheckoutsConnection($first: Int, $after: String) {
  myCheckoutsConnection(first: $first, after: $after) {
    edges {
      cursor
      node {
        checkoutId
        userId
        provider
        purpose
        status
        amountCents
        currency
        externalId
        externalUrl
        orgId
        appId
        tierId
        error
        createdAt
        completedAt
        expiresAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kMyCheckoutsIsolatedDocument = R"gql(query MyCheckouts($limit: Int, $offset: Int) {
  myCheckouts(limit: $limit, offset: $offset) {
    items {
      checkoutId
      userId
      provider
      purpose
      status
      amountCents
      currency
      externalId
      externalUrl
      orgId
      appId
      tierId
      error
      createdAt
      completedAt
      expiresAt
    }
    pageInfo {
      totalCount
      limit
      offset
    }
  }
})gql";
inline constexpr std::string_view kMyCheckoutsOperationName = "MyCheckouts";
inline constexpr std::string_view kMyCheckoutsConnectionIsolatedDocument = R"gql(query MyCheckoutsConnection($first: Int, $after: String) {
  myCheckoutsConnection(first: $first, after: $after) {
    edges {
      cursor
      node {
        checkoutId
        userId
        provider
        purpose
        status
        amountCents
        currency
        externalId
        externalUrl
        orgId
        appId
        tierId
        error
        createdAt
        completedAt
        expiresAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kMyCheckoutsConnectionOperationName = "MyCheckoutsConnection";

/// payments/PaymentEvents.graphql
inline constexpr std::string_view kPaymentEventsDocument = R"gql(query PaymentEvents($limit: Int, $offset: Int) {
  paymentEvents(limit: $limit, offset: $offset) {
    items {
      eventId
      provider
      externalEventId
      eventType
      checkoutId
      processedAt
      error
      createdAt
    }
    pageInfo {
      totalCount
      limit
      offset
    }
  }
}

query PaymentEventsConnection($first: Int, $after: String) {
  paymentEventsConnection(first: $first, after: $after) {
    edges {
      cursor
      node {
        eventId
        provider
        externalEventId
        eventType
        checkoutId
        processedAt
        error
        createdAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kPaymentEventsIsolatedDocument = R"gql(query PaymentEvents($limit: Int, $offset: Int) {
  paymentEvents(limit: $limit, offset: $offset) {
    items {
      eventId
      provider
      externalEventId
      eventType
      checkoutId
      processedAt
      error
      createdAt
    }
    pageInfo {
      totalCount
      limit
      offset
    }
  }
})gql";
inline constexpr std::string_view kPaymentEventsOperationName = "PaymentEvents";
inline constexpr std::string_view kPaymentEventsConnectionIsolatedDocument = R"gql(query PaymentEventsConnection($first: Int, $after: String) {
  paymentEventsConnection(first: $first, after: $after) {
    edges {
      cursor
      node {
        eventId
        provider
        externalEventId
        eventType
        checkoutId
        processedAt
        error
        createdAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kPaymentEventsConnectionOperationName = "PaymentEventsConnection";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "CapturePaypalCheckout") return kCapturePaypalCheckoutIsolatedDocument;
  if (operationName == "Checkouts") return kCheckoutsIsolatedDocument;
  if (operationName == "CheckoutsConnection") return kCheckoutsConnectionIsolatedDocument;
  if (operationName == "CreateCheckout") return kCreateCheckoutIsolatedDocument;
  if (operationName == "MyCheckouts") return kMyCheckoutsIsolatedDocument;
  if (operationName == "MyCheckoutsConnection") return kMyCheckoutsConnectionIsolatedDocument;
  if (operationName == "PaymentEvents") return kPaymentEventsIsolatedDocument;
  if (operationName == "PaymentEventsConnection") return kPaymentEventsConnectionIsolatedDocument;
  return {};
}

}  // namespace payments

namespace platform {

/// platform/PlaceableDatacenters.graphql
inline constexpr std::string_view kPlaceableDatacentersDocument = R"gql(query PlaceableDatacenters {
  placeableDatacenters {
    placementEnforced
    servedBy
    datacenters {
      code
      gameApiUrl
      gameApiWsUrl
      placeable
      appShardCount
      serving
    }
  }
})gql";
inline constexpr std::string_view kPlaceableDatacentersIsolatedDocument = R"gql(query PlaceableDatacenters {
  placeableDatacenters {
    placementEnforced
    servedBy
    datacenters {
      code
      gameApiUrl
      gameApiWsUrl
      placeable
      appShardCount
      serving
    }
  }
})gql";
inline constexpr std::string_view kPlaceableDatacentersOperationName = "PlaceableDatacenters";

/// platform/PlatformConfig.graphql
inline constexpr std::string_view kPlatformConfigDocument = R"gql(query PlatformConfig {
  platformConfig {
    sharedGameApiUrl
    sharedGameApiWsUrl
    freeAppsPerOrg
  }
})gql";
inline constexpr std::string_view kPlatformConfigIsolatedDocument = R"gql(query PlatformConfig {
  platformConfig {
    sharedGameApiUrl
    sharedGameApiWsUrl
    freeAppsPerOrg
  }
})gql";
inline constexpr std::string_view kPlatformConfigOperationName = "PlatformConfig";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "PlaceableDatacenters") return kPlaceableDatacentersIsolatedDocument;
  if (operationName == "PlatformConfig") return kPlatformConfigIsolatedDocument;
  return {};
}

}  // namespace platform

namespace playerCompute {

/// playerCompute/PlayerCompute.graphql
inline constexpr std::string_view kPlayerComputeDocument = R"gql(fragment PlayerWasmModuleFields on PlayerWasmModule {
  moduleId
  appId
  gridId
  name
  description
  authorUserId
  authorOrgId
  enabled
  draft
  currentVersionId
  currentTarget
  circuitState
  lastError
  createdAt
  updatedAt
}

fragment PlayerWasmModuleVersionFields on PlayerWasmModuleVersion {
  versionId
  moduleId
  versionNo
  target
  sourceFilesJson
  openSource
  compileStatus
  compileLog
  compiledSizeBytes
  createdAt
}

mutation PlayerComputeDeploy($input: DeployPlayerComputeInput!) {
  playerComputeDeploy(input: $input) {
    ...PlayerWasmModuleVersionFields
  }
}

mutation PlayerComputeSetEnabled(
  $appId: BigInt!
  $gridId: BigInt!
  $name: String!
  $enabled: Boolean!
) {
  playerComputeSetEnabled(
    appId: $appId
    gridId: $gridId
    name: $name
    enabled: $enabled
  ) {
    ...PlayerWasmModuleFields
  }
}

mutation PlayerComputeSetRequires(
  $appId: BigInt!
  $gridId: BigInt!
  $serverName: String!
  $requiredClientName: String
) {
  playerComputeSetRequires(
    appId: $appId
    gridId: $gridId
    serverName: $serverName
    requiredClientName: $requiredClientName
  )
}

query PlayerComputeMyModules($appId: BigInt!) {
  playerComputeMyModules(appId: $appId) {
    ...PlayerWasmModuleFields
  }
}

query PlayerComputeVersions(
  $appId: BigInt!
  $gridId: BigInt!
  $name: String!
) {
  playerComputeVersions(appId: $appId, gridId: $gridId, name: $name) {
    ...PlayerWasmModuleVersionFields
  }
}

mutation PlayerComputeDelete(
  $appId: BigInt!
  $gridId: BigInt!
  $name: String!
) {
  playerComputeDelete(appId: $appId, gridId: $gridId, name: $name)
}

mutation PlayerComputeInvoke(
  $appId: BigInt!
  $gridId: BigInt!
  $moduleName: String!
  $exportName: String!
  $paramsJson: String
) {
  playerComputeInvoke(
    appId: $appId
    gridId: $gridId
    moduleName: $moduleName
    exportName: $exportName
    paramsJson: $paramsJson
  ) {
    resultBase64
    resultJson
    fuelUsed
    durationUs
  }
}

fragment PlayerWasmModuleRunFields on PlayerWasmModuleRun {
  runId
  appId
  gridId
  moduleId
  moduleName
  executedAsUserId
  flowId
  triggerSource
  startedAt
  durationUs
  fuelUsed
  success
  errorMessage
}

query PlayerComputeUsage($appId: BigInt!) {
  playerComputeUsage(appId: $appId) {
    appId
    hourUnitsUsed
    dayUnitsUsed
    unitsPerHour
    unitsPerDay
    compilesThisHour
    maxCompilesPerHour
    gateStatus
    gateReason
  }
}

query PlayerComputeRuns(
  $appId: BigInt!
  $gridId: BigInt!
  $moduleName: String
  $success: Boolean
  $limit: Int
  $offset: Int
) {
  playerComputeRuns(
    appId: $appId
    gridId: $gridId
    moduleName: $moduleName
    success: $success
    limit: $limit
    offset: $offset
  ) {
    ...PlayerWasmModuleRunFields
  }
}

query PlayerComputeLogs(
  $appId: BigInt!
  $gridId: BigInt!
  $moduleName: String
  $limit: Int
) {
  playerComputeLogs(
    appId: $appId
    gridId: $gridId
    moduleName: $moduleName
    limit: $limit
  ) {
    ...PlayerWasmModuleRunFields
  }
}

mutation PlayerComputeSetSwitch(
  $appId: BigInt!
  $scope: String!
  $disabled: Boolean!
  $scopeRef: BigInt
  $reason: String
) {
  playerComputeSetSwitch(
    appId: $appId
    scope: $scope
    disabled: $disabled
    scopeRef: $scopeRef
    reason: $reason
  )
}

query PlayerComputeSwitches($appId: BigInt!) {
  playerComputeSwitches(appId: $appId) {
    switchId
    appId
    scope
    scopeRef
    reason
    disabledAt
  }
}

query PlayerComputeArtifact(
  $appId: BigInt!
  $gridId: BigInt!
  $name: String!
  $versionId: String
) {
  playerComputeArtifact(
    appId: $appId
    gridId: $gridId
    name: $name
    versionId: $versionId
  ) {
    versionId
    artifactHash
    artifactBase64
    sizeBytes
    abiVersion
    contractJson
    clientFuelPerDispatch
  }
})gql";
inline constexpr std::string_view kPlayerComputeDeployIsolatedDocument = R"gql(mutation PlayerComputeDeploy($input: DeployPlayerComputeInput!) {
  playerComputeDeploy(input: $input) {
    ...PlayerWasmModuleVersionFields
  }
}

fragment PlayerWasmModuleVersionFields on PlayerWasmModuleVersion {
  versionId
  moduleId
  versionNo
  target
  sourceFilesJson
  openSource
  compileStatus
  compileLog
  compiledSizeBytes
  createdAt
})gql";
inline constexpr std::string_view kPlayerComputeDeployOperationName = "PlayerComputeDeploy";
inline constexpr std::string_view kPlayerComputeSetEnabledIsolatedDocument = R"gql(mutation PlayerComputeSetEnabled($appId: BigInt!, $gridId: BigInt!, $name: String!, $enabled: Boolean!) {
  playerComputeSetEnabled(
    appId: $appId
    gridId: $gridId
    name: $name
    enabled: $enabled
  ) {
    ...PlayerWasmModuleFields
  }
}

fragment PlayerWasmModuleFields on PlayerWasmModule {
  moduleId
  appId
  gridId
  name
  description
  authorUserId
  authorOrgId
  enabled
  draft
  currentVersionId
  currentTarget
  circuitState
  lastError
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kPlayerComputeSetEnabledOperationName = "PlayerComputeSetEnabled";
inline constexpr std::string_view kPlayerComputeSetRequiresIsolatedDocument = R"gql(mutation PlayerComputeSetRequires($appId: BigInt!, $gridId: BigInt!, $serverName: String!, $requiredClientName: String) {
  playerComputeSetRequires(
    appId: $appId
    gridId: $gridId
    serverName: $serverName
    requiredClientName: $requiredClientName
  )
})gql";
inline constexpr std::string_view kPlayerComputeSetRequiresOperationName = "PlayerComputeSetRequires";
inline constexpr std::string_view kPlayerComputeMyModulesIsolatedDocument = R"gql(query PlayerComputeMyModules($appId: BigInt!) {
  playerComputeMyModules(appId: $appId) {
    ...PlayerWasmModuleFields
  }
}

fragment PlayerWasmModuleFields on PlayerWasmModule {
  moduleId
  appId
  gridId
  name
  description
  authorUserId
  authorOrgId
  enabled
  draft
  currentVersionId
  currentTarget
  circuitState
  lastError
  createdAt
  updatedAt
})gql";
inline constexpr std::string_view kPlayerComputeMyModulesOperationName = "PlayerComputeMyModules";
inline constexpr std::string_view kPlayerComputeVersionsIsolatedDocument = R"gql(query PlayerComputeVersions($appId: BigInt!, $gridId: BigInt!, $name: String!) {
  playerComputeVersions(appId: $appId, gridId: $gridId, name: $name) {
    ...PlayerWasmModuleVersionFields
  }
}

fragment PlayerWasmModuleVersionFields on PlayerWasmModuleVersion {
  versionId
  moduleId
  versionNo
  target
  sourceFilesJson
  openSource
  compileStatus
  compileLog
  compiledSizeBytes
  createdAt
})gql";
inline constexpr std::string_view kPlayerComputeVersionsOperationName = "PlayerComputeVersions";
inline constexpr std::string_view kPlayerComputeDeleteIsolatedDocument = R"gql(mutation PlayerComputeDelete($appId: BigInt!, $gridId: BigInt!, $name: String!) {
  playerComputeDelete(appId: $appId, gridId: $gridId, name: $name)
})gql";
inline constexpr std::string_view kPlayerComputeDeleteOperationName = "PlayerComputeDelete";
inline constexpr std::string_view kPlayerComputeInvokeIsolatedDocument = R"gql(mutation PlayerComputeInvoke($appId: BigInt!, $gridId: BigInt!, $moduleName: String!, $exportName: String!, $paramsJson: String) {
  playerComputeInvoke(
    appId: $appId
    gridId: $gridId
    moduleName: $moduleName
    exportName: $exportName
    paramsJson: $paramsJson
  ) {
    resultBase64
    resultJson
    fuelUsed
    durationUs
  }
})gql";
inline constexpr std::string_view kPlayerComputeInvokeOperationName = "PlayerComputeInvoke";
inline constexpr std::string_view kPlayerComputeUsageIsolatedDocument = R"gql(query PlayerComputeUsage($appId: BigInt!) {
  playerComputeUsage(appId: $appId) {
    appId
    hourUnitsUsed
    dayUnitsUsed
    unitsPerHour
    unitsPerDay
    compilesThisHour
    maxCompilesPerHour
    gateStatus
    gateReason
  }
})gql";
inline constexpr std::string_view kPlayerComputeUsageOperationName = "PlayerComputeUsage";
inline constexpr std::string_view kPlayerComputeRunsIsolatedDocument = R"gql(query PlayerComputeRuns($appId: BigInt!, $gridId: BigInt!, $moduleName: String, $success: Boolean, $limit: Int, $offset: Int) {
  playerComputeRuns(
    appId: $appId
    gridId: $gridId
    moduleName: $moduleName
    success: $success
    limit: $limit
    offset: $offset
  ) {
    ...PlayerWasmModuleRunFields
  }
}

fragment PlayerWasmModuleRunFields on PlayerWasmModuleRun {
  runId
  appId
  gridId
  moduleId
  moduleName
  executedAsUserId
  flowId
  triggerSource
  startedAt
  durationUs
  fuelUsed
  success
  errorMessage
})gql";
inline constexpr std::string_view kPlayerComputeRunsOperationName = "PlayerComputeRuns";
inline constexpr std::string_view kPlayerComputeLogsIsolatedDocument = R"gql(query PlayerComputeLogs($appId: BigInt!, $gridId: BigInt!, $moduleName: String, $limit: Int) {
  playerComputeLogs(
    appId: $appId
    gridId: $gridId
    moduleName: $moduleName
    limit: $limit
  ) {
    ...PlayerWasmModuleRunFields
  }
}

fragment PlayerWasmModuleRunFields on PlayerWasmModuleRun {
  runId
  appId
  gridId
  moduleId
  moduleName
  executedAsUserId
  flowId
  triggerSource
  startedAt
  durationUs
  fuelUsed
  success
  errorMessage
})gql";
inline constexpr std::string_view kPlayerComputeLogsOperationName = "PlayerComputeLogs";
inline constexpr std::string_view kPlayerComputeSetSwitchIsolatedDocument = R"gql(mutation PlayerComputeSetSwitch($appId: BigInt!, $scope: String!, $disabled: Boolean!, $scopeRef: BigInt, $reason: String) {
  playerComputeSetSwitch(
    appId: $appId
    scope: $scope
    disabled: $disabled
    scopeRef: $scopeRef
    reason: $reason
  )
})gql";
inline constexpr std::string_view kPlayerComputeSetSwitchOperationName = "PlayerComputeSetSwitch";
inline constexpr std::string_view kPlayerComputeSwitchesIsolatedDocument = R"gql(query PlayerComputeSwitches($appId: BigInt!) {
  playerComputeSwitches(appId: $appId) {
    switchId
    appId
    scope
    scopeRef
    reason
    disabledAt
  }
})gql";
inline constexpr std::string_view kPlayerComputeSwitchesOperationName = "PlayerComputeSwitches";
inline constexpr std::string_view kPlayerComputeArtifactIsolatedDocument = R"gql(query PlayerComputeArtifact($appId: BigInt!, $gridId: BigInt!, $name: String!, $versionId: String) {
  playerComputeArtifact(
    appId: $appId
    gridId: $gridId
    name: $name
    versionId: $versionId
  ) {
    versionId
    artifactHash
    artifactBase64
    sizeBytes
    abiVersion
    contractJson
    clientFuelPerDispatch
  }
})gql";
inline constexpr std::string_view kPlayerComputeArtifactOperationName = "PlayerComputeArtifact";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "PlayerComputeDeploy") return kPlayerComputeDeployIsolatedDocument;
  if (operationName == "PlayerComputeSetEnabled") return kPlayerComputeSetEnabledIsolatedDocument;
  if (operationName == "PlayerComputeSetRequires") return kPlayerComputeSetRequiresIsolatedDocument;
  if (operationName == "PlayerComputeMyModules") return kPlayerComputeMyModulesIsolatedDocument;
  if (operationName == "PlayerComputeVersions") return kPlayerComputeVersionsIsolatedDocument;
  if (operationName == "PlayerComputeDelete") return kPlayerComputeDeleteIsolatedDocument;
  if (operationName == "PlayerComputeInvoke") return kPlayerComputeInvokeIsolatedDocument;
  if (operationName == "PlayerComputeUsage") return kPlayerComputeUsageIsolatedDocument;
  if (operationName == "PlayerComputeRuns") return kPlayerComputeRunsIsolatedDocument;
  if (operationName == "PlayerComputeLogs") return kPlayerComputeLogsIsolatedDocument;
  if (operationName == "PlayerComputeSetSwitch") return kPlayerComputeSetSwitchIsolatedDocument;
  if (operationName == "PlayerComputeSwitches") return kPlayerComputeSwitchesIsolatedDocument;
  if (operationName == "PlayerComputeArtifact") return kPlayerComputeArtifactIsolatedDocument;
  return {};
}

}  // namespace playerCompute

namespace playerModel {

/// playerModel/PlayerModel.graphql
inline constexpr std::string_view kPlayerModelDocument = R"gql(query PlayerModelContainers($appId: BigInt!, $gridId: BigInt!) {
  playerModelContainers(appId: $appId, gridId: $gridId) {
    containerId appId gridId ownerUserId typeKey displayName
    stateJson propertiesJson createdAt updatedAt
  }
}

query PlayerModelContainer($input: PlayerModelContainerRefInput!) {
  playerModelContainer(input: $input) {
    containerId appId gridId ownerUserId typeKey displayName
    stateJson propertiesJson createdAt updatedAt
  }
}

mutation PlayerModelCreateContainer($input: CreatePlayerModelContainerInput!) {
  playerModelCreateContainer(input: $input) {
    containerId appId gridId ownerUserId typeKey displayName
    stateJson propertiesJson createdAt updatedAt
  }
}

mutation PlayerModelSetProperty($input: SetPlayerModelPropertyInput!) {
  playerModelSetProperty(input: $input) {
    containerId appId gridId ownerUserId typeKey displayName
    stateJson propertiesJson createdAt updatedAt
  }
}

mutation PlayerModelDeleteContainer($input: PlayerModelContainerRefInput!) {
  playerModelDeleteContainer(input: $input)
}

query PlayerAutomations($appId: BigInt!, $gridId: BigInt!) {
  playerAutomations(appId: $appId, gridId: $gridId) {
    automationId appId gridId ownerUserId name description enabled
    triggerJson actionJson maxRunsPerMinute failureThreshold cooldownMs
    circuitState consecutiveFailures pausedUntil lastError lastRunAt
    nextRunAt createdAt updatedAt
  }
}

mutation PlayerAutomationCreate($input: CreatePlayerAutomationInput!) {
  playerAutomationCreate(input: $input) {
    automationId appId gridId ownerUserId name description enabled
    triggerJson actionJson maxRunsPerMinute failureThreshold cooldownMs
    circuitState consecutiveFailures pausedUntil lastError lastRunAt
    nextRunAt createdAt updatedAt
  }
}

mutation PlayerAutomationSetEnabled($input: SetPlayerAutomationEnabledInput!) {
  playerAutomationSetEnabled(input: $input) {
    automationId appId gridId ownerUserId name description enabled
    triggerJson actionJson maxRunsPerMinute failureThreshold cooldownMs
    circuitState consecutiveFailures pausedUntil lastError lastRunAt
    nextRunAt createdAt updatedAt
  }
}

mutation PlayerAutomationDelete($input: PlayerAutomationRefInput!) {
  playerAutomationDelete(input: $input)
})gql";
inline constexpr std::string_view kPlayerModelContainersIsolatedDocument = R"gql(query PlayerModelContainers($appId: BigInt!, $gridId: BigInt!) {
  playerModelContainers(appId: $appId, gridId: $gridId) {
    containerId
    appId
    gridId
    ownerUserId
    typeKey
    displayName
    stateJson
    propertiesJson
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kPlayerModelContainersOperationName = "PlayerModelContainers";
inline constexpr std::string_view kPlayerModelContainerIsolatedDocument = R"gql(query PlayerModelContainer($input: PlayerModelContainerRefInput!) {
  playerModelContainer(input: $input) {
    containerId
    appId
    gridId
    ownerUserId
    typeKey
    displayName
    stateJson
    propertiesJson
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kPlayerModelContainerOperationName = "PlayerModelContainer";
inline constexpr std::string_view kPlayerModelCreateContainerIsolatedDocument = R"gql(mutation PlayerModelCreateContainer($input: CreatePlayerModelContainerInput!) {
  playerModelCreateContainer(input: $input) {
    containerId
    appId
    gridId
    ownerUserId
    typeKey
    displayName
    stateJson
    propertiesJson
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kPlayerModelCreateContainerOperationName = "PlayerModelCreateContainer";
inline constexpr std::string_view kPlayerModelSetPropertyIsolatedDocument = R"gql(mutation PlayerModelSetProperty($input: SetPlayerModelPropertyInput!) {
  playerModelSetProperty(input: $input) {
    containerId
    appId
    gridId
    ownerUserId
    typeKey
    displayName
    stateJson
    propertiesJson
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kPlayerModelSetPropertyOperationName = "PlayerModelSetProperty";
inline constexpr std::string_view kPlayerModelDeleteContainerIsolatedDocument = R"gql(mutation PlayerModelDeleteContainer($input: PlayerModelContainerRefInput!) {
  playerModelDeleteContainer(input: $input)
})gql";
inline constexpr std::string_view kPlayerModelDeleteContainerOperationName = "PlayerModelDeleteContainer";
inline constexpr std::string_view kPlayerAutomationsIsolatedDocument = R"gql(query PlayerAutomations($appId: BigInt!, $gridId: BigInt!) {
  playerAutomations(appId: $appId, gridId: $gridId) {
    automationId
    appId
    gridId
    ownerUserId
    name
    description
    enabled
    triggerJson
    actionJson
    maxRunsPerMinute
    failureThreshold
    cooldownMs
    circuitState
    consecutiveFailures
    pausedUntil
    lastError
    lastRunAt
    nextRunAt
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kPlayerAutomationsOperationName = "PlayerAutomations";
inline constexpr std::string_view kPlayerAutomationCreateIsolatedDocument = R"gql(mutation PlayerAutomationCreate($input: CreatePlayerAutomationInput!) {
  playerAutomationCreate(input: $input) {
    automationId
    appId
    gridId
    ownerUserId
    name
    description
    enabled
    triggerJson
    actionJson
    maxRunsPerMinute
    failureThreshold
    cooldownMs
    circuitState
    consecutiveFailures
    pausedUntil
    lastError
    lastRunAt
    nextRunAt
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kPlayerAutomationCreateOperationName = "PlayerAutomationCreate";
inline constexpr std::string_view kPlayerAutomationSetEnabledIsolatedDocument = R"gql(mutation PlayerAutomationSetEnabled($input: SetPlayerAutomationEnabledInput!) {
  playerAutomationSetEnabled(input: $input) {
    automationId
    appId
    gridId
    ownerUserId
    name
    description
    enabled
    triggerJson
    actionJson
    maxRunsPerMinute
    failureThreshold
    cooldownMs
    circuitState
    consecutiveFailures
    pausedUntil
    lastError
    lastRunAt
    nextRunAt
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kPlayerAutomationSetEnabledOperationName = "PlayerAutomationSetEnabled";
inline constexpr std::string_view kPlayerAutomationDeleteIsolatedDocument = R"gql(mutation PlayerAutomationDelete($input: PlayerAutomationRefInput!) {
  playerAutomationDelete(input: $input)
})gql";
inline constexpr std::string_view kPlayerAutomationDeleteOperationName = "PlayerAutomationDelete";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "PlayerModelContainers") return kPlayerModelContainersIsolatedDocument;
  if (operationName == "PlayerModelContainer") return kPlayerModelContainerIsolatedDocument;
  if (operationName == "PlayerModelCreateContainer") return kPlayerModelCreateContainerIsolatedDocument;
  if (operationName == "PlayerModelSetProperty") return kPlayerModelSetPropertyIsolatedDocument;
  if (operationName == "PlayerModelDeleteContainer") return kPlayerModelDeleteContainerIsolatedDocument;
  if (operationName == "PlayerAutomations") return kPlayerAutomationsIsolatedDocument;
  if (operationName == "PlayerAutomationCreate") return kPlayerAutomationCreateIsolatedDocument;
  if (operationName == "PlayerAutomationSetEnabled") return kPlayerAutomationSetEnabledIsolatedDocument;
  if (operationName == "PlayerAutomationDelete") return kPlayerAutomationDeleteIsolatedDocument;
  return {};
}

}  // namespace playerModel

namespace playerWallet {

/// playerWallet/PlayerWallet.graphql
inline constexpr std::string_view kPlayerWalletDocument = R"gql(fragment PlayerWalletFields on PlayerWallet {
  walletId
  userId
  balanceCents
  currency
  createdAt
}

fragment PlayerWalletTransactionFields on PlayerWalletTransaction {
  transactionId
  walletId
  userId
  amountCents
  balanceAfter
  transactionType
  description
  referenceId
  appId
  createdAt
}

fragment PlayerSpendCapFields on PlayerSpendCap {
  userId
  scope
  scopeRef
  dailyLimitCents
  monthlyLimitCents
  currentDayUsageCents
  currentMonthUsageCents
}

fragment PlayerAutoBillingFields on PlayerAutoBilling {
  userId
  enabled
  limitCents
  autoBilledThisPeriodCents
  rechargeAmountCents
  lowWaterThresholdCents
  hasPaymentMethod
  lastError
}

fragment PlayerUsageChargeFields on PlayerUsageCharge {
  chargeId
  userId
  appId
  periodStart
  periodEnd
  amountCents
  platformCents
  markupCents
  currency
  usageSnapshotJson
  createdAt
}

fragment PlayerWasmPolicyFields on PlayerWasmPolicy {
  policyId
  appId
  scope
  scopeRef
  enabled
  maxModulesPerGrid
  maxModulesTotal
  maxTickHz
  fuelPerTick
  fuelPerInvoke
  maxMemoryMb
  maxRunMs
  maxDbOpsPerTick
  maxEgressMsgsPerMin
  maxEgressBytesPerMin
  unitsPerHour
  unitsPerDay
  maxCompilesPerHour
  maxContainerCreatesDay
  clientFuelPerDispatch
}

query PlayerWalletBalance {
  playerWalletBalance {
    ...PlayerWalletFields
  }
}

query PlayerWalletTransactions($limit: Int, $offset: Int) {
  playerWalletTransactions(limit: $limit, offset: $offset) {
    ...PlayerWalletTransactionFields
  }
}

query PlayerUsageCharges($appId: BigInt, $limit: Int) {
  playerUsageCharges(appId: $appId, limit: $limit) {
    ...PlayerUsageChargeFields
  }
}

query PlayerSpendCaps {
  playerSpendCaps {
    ...PlayerSpendCapFields
  }
}

mutation SetPlayerSpendCap(
  $scope: String!
  $appId: BigInt
  $dailyLimitCents: BigInt
  $monthlyLimitCents: BigInt
) {
  setPlayerSpendCap(
    scope: $scope
    appId: $appId
    dailyLimitCents: $dailyLimitCents
    monthlyLimitCents: $monthlyLimitCents
  ) {
    ...PlayerSpendCapFields
  }
}

query PlayerAutoBilling {
  playerAutoBilling {
    ...PlayerAutoBillingFields
  }
}

mutation BeginPlayerCardSetup {
  beginPlayerCardSetup {
    clientSecret
    publishableKey
    externalCustomerId
  }
}

mutation SetPlayerAutoBilling(
  $enabled: Boolean!
  $limitCents: BigInt
  $rechargeAmountCents: BigInt
  $lowWaterThresholdCents: BigInt
) {
  setPlayerAutoBilling(
    enabled: $enabled
    limitCents: $limitCents
    rechargeAmountCents: $rechargeAmountCents
    lowWaterThresholdCents: $lowWaterThresholdCents
  ) {
    ...PlayerAutoBillingFields
  }
}

query PlayerRuntimeStates {
  playerRuntimeStates {
    userId
    appId
    status
    reason
    updatedAt
  }
}

query PlayerWasmPolicies($appId: BigInt!) {
  playerWasmPolicies(appId: $appId) {
    ...PlayerWasmPolicyFields
  }
}

mutation SetPlayerWasmPolicy($input: SetPlayerWasmPolicyInput!) {
  setPlayerWasmPolicy(input: $input) {
    ...PlayerWasmPolicyFields
  }
}

mutation DeletePlayerWasmPolicy(
  $appId: BigInt!
  $scope: String!
  $scopeRef: BigInt
) {
  deletePlayerWasmPolicy(appId: $appId, scope: $scope, scopeRef: $scopeRef)
}

query PlayerRateMarkup($appId: BigInt!) {
  playerRateMarkup(appId: $appId)
}

mutation SetPlayerRateMarkup($appId: BigInt!, $markupBps: Int!) {
  setPlayerRateMarkup(appId: $appId, markupBps: $markupBps)
}

query AppPlayerUsage($appId: BigInt!, $hours: Int) {
  appPlayerUsage(appId: $appId, hours: $hours) {
    userId
    computeUnits
    automationUnits
    compileCount
    chargedCents
  }
}

query AppPlayerMarkupAccrued($appId: BigInt!) {
  appPlayerMarkupAccrued(appId: $appId)
})gql";
inline constexpr std::string_view kPlayerWalletBalanceIsolatedDocument = R"gql(query PlayerWalletBalance {
  playerWalletBalance {
    ...PlayerWalletFields
  }
}

fragment PlayerWalletFields on PlayerWallet {
  walletId
  userId
  balanceCents
  currency
  createdAt
})gql";
inline constexpr std::string_view kPlayerWalletBalanceOperationName = "PlayerWalletBalance";
inline constexpr std::string_view kPlayerWalletTransactionsIsolatedDocument = R"gql(query PlayerWalletTransactions($limit: Int, $offset: Int) {
  playerWalletTransactions(limit: $limit, offset: $offset) {
    ...PlayerWalletTransactionFields
  }
}

fragment PlayerWalletTransactionFields on PlayerWalletTransaction {
  transactionId
  walletId
  userId
  amountCents
  balanceAfter
  transactionType
  description
  referenceId
  appId
  createdAt
})gql";
inline constexpr std::string_view kPlayerWalletTransactionsOperationName = "PlayerWalletTransactions";
inline constexpr std::string_view kPlayerUsageChargesIsolatedDocument = R"gql(query PlayerUsageCharges($appId: BigInt, $limit: Int) {
  playerUsageCharges(appId: $appId, limit: $limit) {
    ...PlayerUsageChargeFields
  }
}

fragment PlayerUsageChargeFields on PlayerUsageCharge {
  chargeId
  userId
  appId
  periodStart
  periodEnd
  amountCents
  platformCents
  markupCents
  currency
  usageSnapshotJson
  createdAt
})gql";
inline constexpr std::string_view kPlayerUsageChargesOperationName = "PlayerUsageCharges";
inline constexpr std::string_view kPlayerSpendCapsIsolatedDocument = R"gql(query PlayerSpendCaps {
  playerSpendCaps {
    ...PlayerSpendCapFields
  }
}

fragment PlayerSpendCapFields on PlayerSpendCap {
  userId
  scope
  scopeRef
  dailyLimitCents
  monthlyLimitCents
  currentDayUsageCents
  currentMonthUsageCents
})gql";
inline constexpr std::string_view kPlayerSpendCapsOperationName = "PlayerSpendCaps";
inline constexpr std::string_view kSetPlayerSpendCapIsolatedDocument = R"gql(mutation SetPlayerSpendCap($scope: String!, $appId: BigInt, $dailyLimitCents: BigInt, $monthlyLimitCents: BigInt) {
  setPlayerSpendCap(
    scope: $scope
    appId: $appId
    dailyLimitCents: $dailyLimitCents
    monthlyLimitCents: $monthlyLimitCents
  ) {
    ...PlayerSpendCapFields
  }
}

fragment PlayerSpendCapFields on PlayerSpendCap {
  userId
  scope
  scopeRef
  dailyLimitCents
  monthlyLimitCents
  currentDayUsageCents
  currentMonthUsageCents
})gql";
inline constexpr std::string_view kSetPlayerSpendCapOperationName = "SetPlayerSpendCap";
inline constexpr std::string_view kPlayerAutoBillingIsolatedDocument = R"gql(query PlayerAutoBilling {
  playerAutoBilling {
    ...PlayerAutoBillingFields
  }
}

fragment PlayerAutoBillingFields on PlayerAutoBilling {
  userId
  enabled
  limitCents
  autoBilledThisPeriodCents
  rechargeAmountCents
  lowWaterThresholdCents
  hasPaymentMethod
  lastError
})gql";
inline constexpr std::string_view kPlayerAutoBillingOperationName = "PlayerAutoBilling";
inline constexpr std::string_view kBeginPlayerCardSetupIsolatedDocument = R"gql(mutation BeginPlayerCardSetup {
  beginPlayerCardSetup {
    clientSecret
    publishableKey
    externalCustomerId
  }
})gql";
inline constexpr std::string_view kBeginPlayerCardSetupOperationName = "BeginPlayerCardSetup";
inline constexpr std::string_view kSetPlayerAutoBillingIsolatedDocument = R"gql(mutation SetPlayerAutoBilling($enabled: Boolean!, $limitCents: BigInt, $rechargeAmountCents: BigInt, $lowWaterThresholdCents: BigInt) {
  setPlayerAutoBilling(
    enabled: $enabled
    limitCents: $limitCents
    rechargeAmountCents: $rechargeAmountCents
    lowWaterThresholdCents: $lowWaterThresholdCents
  ) {
    ...PlayerAutoBillingFields
  }
}

fragment PlayerAutoBillingFields on PlayerAutoBilling {
  userId
  enabled
  limitCents
  autoBilledThisPeriodCents
  rechargeAmountCents
  lowWaterThresholdCents
  hasPaymentMethod
  lastError
})gql";
inline constexpr std::string_view kSetPlayerAutoBillingOperationName = "SetPlayerAutoBilling";
inline constexpr std::string_view kPlayerRuntimeStatesIsolatedDocument = R"gql(query PlayerRuntimeStates {
  playerRuntimeStates {
    userId
    appId
    status
    reason
    updatedAt
  }
})gql";
inline constexpr std::string_view kPlayerRuntimeStatesOperationName = "PlayerRuntimeStates";
inline constexpr std::string_view kPlayerWasmPoliciesIsolatedDocument = R"gql(query PlayerWasmPolicies($appId: BigInt!) {
  playerWasmPolicies(appId: $appId) {
    ...PlayerWasmPolicyFields
  }
}

fragment PlayerWasmPolicyFields on PlayerWasmPolicy {
  policyId
  appId
  scope
  scopeRef
  enabled
  maxModulesPerGrid
  maxModulesTotal
  maxTickHz
  fuelPerTick
  fuelPerInvoke
  maxMemoryMb
  maxRunMs
  maxDbOpsPerTick
  maxEgressMsgsPerMin
  maxEgressBytesPerMin
  unitsPerHour
  unitsPerDay
  maxCompilesPerHour
  maxContainerCreatesDay
  clientFuelPerDispatch
})gql";
inline constexpr std::string_view kPlayerWasmPoliciesOperationName = "PlayerWasmPolicies";
inline constexpr std::string_view kSetPlayerWasmPolicyIsolatedDocument = R"gql(mutation SetPlayerWasmPolicy($input: SetPlayerWasmPolicyInput!) {
  setPlayerWasmPolicy(input: $input) {
    ...PlayerWasmPolicyFields
  }
}

fragment PlayerWasmPolicyFields on PlayerWasmPolicy {
  policyId
  appId
  scope
  scopeRef
  enabled
  maxModulesPerGrid
  maxModulesTotal
  maxTickHz
  fuelPerTick
  fuelPerInvoke
  maxMemoryMb
  maxRunMs
  maxDbOpsPerTick
  maxEgressMsgsPerMin
  maxEgressBytesPerMin
  unitsPerHour
  unitsPerDay
  maxCompilesPerHour
  maxContainerCreatesDay
  clientFuelPerDispatch
})gql";
inline constexpr std::string_view kSetPlayerWasmPolicyOperationName = "SetPlayerWasmPolicy";
inline constexpr std::string_view kDeletePlayerWasmPolicyIsolatedDocument = R"gql(mutation DeletePlayerWasmPolicy($appId: BigInt!, $scope: String!, $scopeRef: BigInt) {
  deletePlayerWasmPolicy(appId: $appId, scope: $scope, scopeRef: $scopeRef)
})gql";
inline constexpr std::string_view kDeletePlayerWasmPolicyOperationName = "DeletePlayerWasmPolicy";
inline constexpr std::string_view kPlayerRateMarkupIsolatedDocument = R"gql(query PlayerRateMarkup($appId: BigInt!) {
  playerRateMarkup(appId: $appId)
})gql";
inline constexpr std::string_view kPlayerRateMarkupOperationName = "PlayerRateMarkup";
inline constexpr std::string_view kSetPlayerRateMarkupIsolatedDocument = R"gql(mutation SetPlayerRateMarkup($appId: BigInt!, $markupBps: Int!) {
  setPlayerRateMarkup(appId: $appId, markupBps: $markupBps)
})gql";
inline constexpr std::string_view kSetPlayerRateMarkupOperationName = "SetPlayerRateMarkup";
inline constexpr std::string_view kAppPlayerUsageIsolatedDocument = R"gql(query AppPlayerUsage($appId: BigInt!, $hours: Int) {
  appPlayerUsage(appId: $appId, hours: $hours) {
    userId
    computeUnits
    automationUnits
    compileCount
    chargedCents
  }
})gql";
inline constexpr std::string_view kAppPlayerUsageOperationName = "AppPlayerUsage";
inline constexpr std::string_view kAppPlayerMarkupAccruedIsolatedDocument = R"gql(query AppPlayerMarkupAccrued($appId: BigInt!) {
  appPlayerMarkupAccrued(appId: $appId)
})gql";
inline constexpr std::string_view kAppPlayerMarkupAccruedOperationName = "AppPlayerMarkupAccrued";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "PlayerWalletBalance") return kPlayerWalletBalanceIsolatedDocument;
  if (operationName == "PlayerWalletTransactions") return kPlayerWalletTransactionsIsolatedDocument;
  if (operationName == "PlayerUsageCharges") return kPlayerUsageChargesIsolatedDocument;
  if (operationName == "PlayerSpendCaps") return kPlayerSpendCapsIsolatedDocument;
  if (operationName == "SetPlayerSpendCap") return kSetPlayerSpendCapIsolatedDocument;
  if (operationName == "PlayerAutoBilling") return kPlayerAutoBillingIsolatedDocument;
  if (operationName == "BeginPlayerCardSetup") return kBeginPlayerCardSetupIsolatedDocument;
  if (operationName == "SetPlayerAutoBilling") return kSetPlayerAutoBillingIsolatedDocument;
  if (operationName == "PlayerRuntimeStates") return kPlayerRuntimeStatesIsolatedDocument;
  if (operationName == "PlayerWasmPolicies") return kPlayerWasmPoliciesIsolatedDocument;
  if (operationName == "SetPlayerWasmPolicy") return kSetPlayerWasmPolicyIsolatedDocument;
  if (operationName == "DeletePlayerWasmPolicy") return kDeletePlayerWasmPolicyIsolatedDocument;
  if (operationName == "PlayerRateMarkup") return kPlayerRateMarkupIsolatedDocument;
  if (operationName == "SetPlayerRateMarkup") return kSetPlayerRateMarkupIsolatedDocument;
  if (operationName == "AppPlayerUsage") return kAppPlayerUsageIsolatedDocument;
  if (operationName == "AppPlayerMarkupAccrued") return kAppPlayerMarkupAccruedIsolatedDocument;
  return {};
}

}  // namespace playerWallet

namespace quotas {

/// quotas/DeleteQuota.graphql
inline constexpr std::string_view kDeleteQuotaDocument = R"gql(mutation DeleteQuota($quotaId: BigInt!) {
  deleteQuota(quotaId: $quotaId)
})gql";
inline constexpr std::string_view kDeleteQuotaIsolatedDocument = R"gql(mutation DeleteQuota($quotaId: BigInt!) {
  deleteQuota(quotaId: $quotaId)
})gql";
inline constexpr std::string_view kDeleteQuotaOperationName = "DeleteQuota";

/// quotas/EffectiveQuota.graphql
inline constexpr std::string_view kEffectiveQuotaDocument = R"gql(query EffectiveQuota(
  $metric: String!
  $orgId: BigInt
  $appId: BigInt
  $tierId: BigInt
) {
  effectiveQuota(
    metric: $metric
    orgId: $orgId
    appId: $appId
    tierId: $tierId
  ) {
    quotaId
    orgId
    appId
    tierId
    metric
    limitValue
    period
    actionOnExceed
  }
})gql";
inline constexpr std::string_view kEffectiveQuotaIsolatedDocument = R"gql(query EffectiveQuota($metric: String!, $orgId: BigInt, $appId: BigInt, $tierId: BigInt) {
  effectiveQuota(metric: $metric, orgId: $orgId, appId: $appId, tierId: $tierId) {
    quotaId
    orgId
    appId
    tierId
    metric
    limitValue
    period
    actionOnExceed
  }
})gql";
inline constexpr std::string_view kEffectiveQuotaOperationName = "EffectiveQuota";

/// quotas/QuotasForApp.graphql
inline constexpr std::string_view kQuotasForAppDocument = R"gql(query QuotasForApp($appId: BigInt!) {
  quotasForApp(appId: $appId) {
    quotaId
    orgId
    appId
    tierId
    metric
    limitValue
    period
    actionOnExceed
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kQuotasForAppIsolatedDocument = R"gql(query QuotasForApp($appId: BigInt!) {
  quotasForApp(appId: $appId) {
    quotaId
    orgId
    appId
    tierId
    metric
    limitValue
    period
    actionOnExceed
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kQuotasForAppOperationName = "QuotasForApp";

/// quotas/QuotasForOrg.graphql
inline constexpr std::string_view kQuotasForOrgDocument = R"gql(query QuotasForOrg($orgId: BigInt!) {
  quotasForOrg(orgId: $orgId) {
    quotaId
    orgId
    appId
    tierId
    metric
    limitValue
    period
    actionOnExceed
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kQuotasForOrgIsolatedDocument = R"gql(query QuotasForOrg($orgId: BigInt!) {
  quotasForOrg(orgId: $orgId) {
    quotaId
    orgId
    appId
    tierId
    metric
    limitValue
    period
    actionOnExceed
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kQuotasForOrgOperationName = "QuotasForOrg";

/// quotas/SetQuota.graphql
inline constexpr std::string_view kSetQuotaDocument = R"gql(mutation SetQuota($input: SetQuotaInput!) {
  setQuota(input: $input) {
    quotaId
    orgId
    appId
    tierId
    metric
    limitValue
    period
    actionOnExceed
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kSetQuotaIsolatedDocument = R"gql(mutation SetQuota($input: SetQuotaInput!) {
  setQuota(input: $input) {
    quotaId
    orgId
    appId
    tierId
    metric
    limitValue
    period
    actionOnExceed
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kSetQuotaOperationName = "SetQuota";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "DeleteQuota") return kDeleteQuotaIsolatedDocument;
  if (operationName == "EffectiveQuota") return kEffectiveQuotaIsolatedDocument;
  if (operationName == "QuotasForApp") return kQuotasForAppIsolatedDocument;
  if (operationName == "QuotasForOrg") return kQuotasForOrgIsolatedDocument;
  if (operationName == "SetQuota") return kSetQuotaIsolatedDocument;
  return {};
}

}  // namespace quotas

namespace realtime {

/// realtime/RealtimeControlEvents.graphql
inline constexpr std::string_view kRealtimeControlEventsDocument = R"gql(# Control frames ONLY.
#
# udpNotifications is a union whose other members carry the browser UDP proxy's
# gameplay fan-out. CrowdyCPP does not use that path — it speaks UDP natively —
# so this selects nothing but RealtimeConnectionEvent, and every other member
# contributes only its __typename.
#
# Subscribing at all is what a native client was missing. SERVER_DRAINING, the
# advance warning that this API instance is being taken out of service, is
# delivered here and nowhere else. Without it, a client pinned to one instance
# under direct connect first learns the instance is going away when it stops
# answering, which is precisely the case re-discovery is meant to get ahead of.
#
# Must be app-scoped or the server answers APP_ID_REQUIRED: game tokens are
# app-agnostic and one socket is shared across apps.
subscription RealtimeControlEvents {
  udpNotifications {
    __typename
    ... on RealtimeConnectionEvent {
      status
      code
      message
      retryable
    }
  }
})gql";
inline constexpr std::string_view kRealtimeControlEventsIsolatedDocument = R"gql(subscription RealtimeControlEvents {
  udpNotifications {
    __typename
    ... on RealtimeConnectionEvent {
      status
      code
      message
      retryable
    }
  }
})gql";
inline constexpr std::string_view kRealtimeControlEventsOperationName = "RealtimeControlEvents";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "RealtimeControlEvents") return kRealtimeControlEventsIsolatedDocument;
  return {};
}

}  // namespace realtime

namespace serverStatus {

/// serverStatus/ActiveGraphQLServers.graphql
inline constexpr std::string_view kActiveGraphQLServersDocument = R"gql(query ActiveGraphQLServers {
  activeGraphQLServers {
    graphqlServerId
    ip4
    ip6
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kActiveGraphQLServersIsolatedDocument = R"gql(query ActiveGraphQLServers {
  activeGraphQLServers {
    graphqlServerId
    ip4
    ip6
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kActiveGraphQLServersOperationName = "ActiveGraphQLServers";

/// serverStatus/GameClientBootstrap.graphql
inline constexpr std::string_view kGameClientBootstrapDocument = R"gql(query GameClientBootstrap($appId: BigInt!) {
  gameClientBootstrap(appId: $appId) {
    appId
    gameApiUrl
    gameApiWsUrl
    discoveryUrl
    realtimeProtocol
    subscriptionName
    maxReplicationDistance
    maxDecayRate
    sequenceNumberModulo
    udpProxyConnectionStatus {
      connected
      serverIp6
      serverClientPort
      lastMessageTime
    }
    versionInfo {
      serverVersion {
        major
        minor
        patch
        build
      }
      minimumClientVersion {
        major
        minor
        patch
        build
      }
    }
    me {
      userId
      email
      gamertag
      disambiguation
      state
      isConfirmed
      createdAt
      grantEarlyAccess
      grantEarlyAccessOverride
      orgId
      externalId
      userType
      isSuperAdmin
    }
  }
})gql";
inline constexpr std::string_view kGameClientBootstrapIsolatedDocument = R"gql(query GameClientBootstrap($appId: BigInt!) {
  gameClientBootstrap(appId: $appId) {
    appId
    gameApiUrl
    gameApiWsUrl
    discoveryUrl
    realtimeProtocol
    subscriptionName
    maxReplicationDistance
    maxDecayRate
    sequenceNumberModulo
    udpProxyConnectionStatus {
      connected
      serverIp6
      serverClientPort
      lastMessageTime
    }
    versionInfo {
      serverVersion {
        major
        minor
        patch
        build
      }
      minimumClientVersion {
        major
        minor
        patch
        build
      }
    }
    me {
      userId
      email
      gamertag
      disambiguation
      state
      isConfirmed
      createdAt
      grantEarlyAccess
      grantEarlyAccessOverride
      orgId
      externalId
      userType
      isSuperAdmin
    }
  }
})gql";
inline constexpr std::string_view kGameClientBootstrapOperationName = "GameClientBootstrap";

/// serverStatus/GameClientRediscover.graphql
inline constexpr std::string_view kGameClientRediscoverDocument = R"gql(# Deliberately the narrowest possible bootstrap: re-discovery runs when the
# endpoint a client is pinned to has stopped answering, so it asks the shared
# origin for nothing but where to go next. GameClientBootstrap also selects
# `me`, version info and proxy status, any of which can fail for reasons that
# have nothing to do with the question being asked here.
query GameClientRediscover($appId: BigInt!) {
  gameClientBootstrap(appId: $appId) {
    gameApiUrl
    gameApiWsUrl
    discoveryUrl
  }
})gql";
inline constexpr std::string_view kGameClientRediscoverIsolatedDocument = R"gql(query GameClientRediscover($appId: BigInt!) {
  gameClientBootstrap(appId: $appId) {
    gameApiUrl
    gameApiWsUrl
    discoveryUrl
  }
})gql";
inline constexpr std::string_view kGameClientRediscoverOperationName = "GameClientRediscover";

/// serverStatus/GraphqlServers.graphql
inline constexpr std::string_view kGraphqlServersDocument = R"gql(query GraphqlServers {
  graphqlServers {
    graphqlServerId
    ip4
    ip6
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kGraphqlServersIsolatedDocument = R"gql(query GraphqlServers {
  graphqlServers {
    graphqlServerId
    ip4
    ip6
    status
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kGraphqlServersOperationName = "GraphqlServers";

/// serverStatus/ServerWithLeastClients.graphql
inline constexpr std::string_view kServerWithLeastClientsDocument = R"gql(query ServerWithLeastClients {
  serverWithLeastClients {
    serverId
    ip4
    ip6
    clientPort
    status
    peers
    clients
    cpuPeakPct
    updatedAt
    createdAt
  }
})gql";
inline constexpr std::string_view kServerWithLeastClientsIsolatedDocument = R"gql(query ServerWithLeastClients {
  serverWithLeastClients {
    serverId
    ip4
    ip6
    clientPort
    status
    peers
    clients
    cpuPeakPct
    updatedAt
    createdAt
  }
})gql";
inline constexpr std::string_view kServerWithLeastClientsOperationName = "ServerWithLeastClients";

/// serverStatus/VersionInfo.graphql
inline constexpr std::string_view kVersionInfoDocument = R"gql(query VersionInfo {
  versionInfo {
    serverVersion {
      major
      minor
      patch
      build
    }
    minimumClientVersion {
      major
      minor
      patch
      build
    }
  }
})gql";
inline constexpr std::string_view kVersionInfoIsolatedDocument = R"gql(query VersionInfo {
  versionInfo {
    serverVersion {
      major
      minor
      patch
      build
    }
    minimumClientVersion {
      major
      minor
      patch
      build
    }
  }
})gql";
inline constexpr std::string_view kVersionInfoOperationName = "VersionInfo";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "ActiveGraphQLServers") return kActiveGraphQLServersIsolatedDocument;
  if (operationName == "GameClientBootstrap") return kGameClientBootstrapIsolatedDocument;
  if (operationName == "GameClientRediscover") return kGameClientRediscoverIsolatedDocument;
  if (operationName == "GraphqlServers") return kGraphqlServersIsolatedDocument;
  if (operationName == "ServerWithLeastClients") return kServerWithLeastClientsIsolatedDocument;
  if (operationName == "VersionInfo") return kVersionInfoIsolatedDocument;
  return {};
}

}  // namespace serverStatus

namespace sharedEnvironment {

/// sharedEnvironment/SharedEnvironment.graphql
inline constexpr std::string_view kSharedEnvironmentDocument = R"gql(query SharedEnvPlans {
  sharedEnvPlans {
    planId
    code
    name
    description
    priceCents
    currency
    billingInterval
    status
  }
}

query OrgFreeAppQuota($orgId: BigInt!) {
  orgFreeAppQuota(orgId: $orgId) {
    orgId
    quota
    usedFree
    paidApps
    remainingFree
  }
}

query AppSharedSubscription($appId: BigInt!) {
  appSharedSubscription(appId: $appId) {
    appId
    orgId
    planId
    provider
    status
    currentPeriodEnd
  }
}

query AppRuntimeState($appId: BigInt!) {
  appRuntimeState(appId: $appId) {
    appId
    deploymentTarget
    runtimeStatus
    runtimeDenialReason
    walletBalanceCents
    currentHourUsageCents
    currentDayUsageCents
    hourlyLimitCents
    dailyLimitCents
  }
}

query OrgAutoBilling($orgId: BigInt!) {
  orgAutoBilling(orgId: $orgId) {
    orgId
    enabled
    limitCents
    period
    autoBilledThisPeriodCents
    rechargeAmountCents
    lowWaterThresholdCents
    hasPaymentMethod
    lastError
  }
}

query OrgPaymentMethods($orgId: BigInt!) {
  orgPaymentMethods(orgId: $orgId) {
    paymentMethodId
    provider
    brand
    last4
    isDefault
    status
  }
}

mutation PublishAppToShared(
  $appId: BigInt!
  $planId: BigInt
  $provider: PaymentProvider
  $successUrl: String
  $cancelUrl: String
  $idempotencyKey: String
) {
  publishAppToShared(
    appId: $appId
    planId: $planId
    provider: $provider
    successUrl: $successUrl
    cancelUrl: $cancelUrl
    idempotencyKey: $idempotencyKey
  ) {
    appId
    free
    checkout {
      checkoutId
      provider
      status
      amountCents
      currency
      externalUrl
    }
  }
}

mutation CancelSharedSubscription($appId: BigInt!, $idempotencyKey: String) {
  cancelSharedSubscription(appId: $appId, idempotencyKey: $idempotencyKey) {
    appId
    orgId
    planId
    provider
    status
    currentPeriodEnd
  }
}

mutation SetAppSpendCaps(
  $appId: BigInt!
  $hourlyLimitCents: BigInt
  $dailyLimitCents: BigInt
) {
  setAppSpendCaps(
    appId: $appId
    hourlyLimitCents: $hourlyLimitCents
    dailyLimitCents: $dailyLimitCents
  ) {
    appId
    runtimeStatus
    runtimeDenialReason
    hourlyLimitCents
    dailyLimitCents
  }
}

mutation SetAutoBilling(
  $orgId: BigInt!
  $enabled: Boolean!
  $limitCents: BigInt
  $rechargeAmountCents: BigInt
  $lowWaterThresholdCents: BigInt
  $idempotencyKey: String
) {
  setAutoBilling(
    orgId: $orgId
    enabled: $enabled
    limitCents: $limitCents
    rechargeAmountCents: $rechargeAmountCents
    lowWaterThresholdCents: $lowWaterThresholdCents
    idempotencyKey: $idempotencyKey
  ) {
    orgId
    enabled
    limitCents
    rechargeAmountCents
    lowWaterThresholdCents
    hasPaymentMethod
  }
}

mutation SetupSharedPaymentMethod($orgId: BigInt!, $idempotencyKey: String) {
  setupSharedPaymentMethod(orgId: $orgId, idempotencyKey: $idempotencyKey) {
    externalCustomerId
    clientSecret
    publishableKey
  }
}

mutation RemoveSharedPaymentMethod(
  $orgId: BigInt!
  $paymentMethodId: BigInt!
  $idempotencyKey: String
) {
  removeSharedPaymentMethod(
    orgId: $orgId
    paymentMethodId: $paymentMethodId
    idempotencyKey: $idempotencyKey
  )
})gql";
inline constexpr std::string_view kSharedEnvPlansIsolatedDocument = R"gql(query SharedEnvPlans {
  sharedEnvPlans {
    planId
    code
    name
    description
    priceCents
    currency
    billingInterval
    status
  }
})gql";
inline constexpr std::string_view kSharedEnvPlansOperationName = "SharedEnvPlans";
inline constexpr std::string_view kOrgFreeAppQuotaIsolatedDocument = R"gql(query OrgFreeAppQuota($orgId: BigInt!) {
  orgFreeAppQuota(orgId: $orgId) {
    orgId
    quota
    usedFree
    paidApps
    remainingFree
  }
})gql";
inline constexpr std::string_view kOrgFreeAppQuotaOperationName = "OrgFreeAppQuota";
inline constexpr std::string_view kAppSharedSubscriptionIsolatedDocument = R"gql(query AppSharedSubscription($appId: BigInt!) {
  appSharedSubscription(appId: $appId) {
    appId
    orgId
    planId
    provider
    status
    currentPeriodEnd
  }
})gql";
inline constexpr std::string_view kAppSharedSubscriptionOperationName = "AppSharedSubscription";
inline constexpr std::string_view kAppRuntimeStateIsolatedDocument = R"gql(query AppRuntimeState($appId: BigInt!) {
  appRuntimeState(appId: $appId) {
    appId
    deploymentTarget
    runtimeStatus
    runtimeDenialReason
    walletBalanceCents
    currentHourUsageCents
    currentDayUsageCents
    hourlyLimitCents
    dailyLimitCents
  }
})gql";
inline constexpr std::string_view kAppRuntimeStateOperationName = "AppRuntimeState";
inline constexpr std::string_view kOrgAutoBillingIsolatedDocument = R"gql(query OrgAutoBilling($orgId: BigInt!) {
  orgAutoBilling(orgId: $orgId) {
    orgId
    enabled
    limitCents
    period
    autoBilledThisPeriodCents
    rechargeAmountCents
    lowWaterThresholdCents
    hasPaymentMethod
    lastError
  }
})gql";
inline constexpr std::string_view kOrgAutoBillingOperationName = "OrgAutoBilling";
inline constexpr std::string_view kOrgPaymentMethodsIsolatedDocument = R"gql(query OrgPaymentMethods($orgId: BigInt!) {
  orgPaymentMethods(orgId: $orgId) {
    paymentMethodId
    provider
    brand
    last4
    isDefault
    status
  }
})gql";
inline constexpr std::string_view kOrgPaymentMethodsOperationName = "OrgPaymentMethods";
inline constexpr std::string_view kPublishAppToSharedIsolatedDocument = R"gql(mutation PublishAppToShared($appId: BigInt!, $planId: BigInt, $provider: PaymentProvider, $successUrl: String, $cancelUrl: String, $idempotencyKey: String) {
  publishAppToShared(
    appId: $appId
    planId: $planId
    provider: $provider
    successUrl: $successUrl
    cancelUrl: $cancelUrl
    idempotencyKey: $idempotencyKey
  ) {
    appId
    free
    checkout {
      checkoutId
      provider
      status
      amountCents
      currency
      externalUrl
    }
  }
})gql";
inline constexpr std::string_view kPublishAppToSharedOperationName = "PublishAppToShared";
inline constexpr std::string_view kCancelSharedSubscriptionIsolatedDocument = R"gql(mutation CancelSharedSubscription($appId: BigInt!, $idempotencyKey: String) {
  cancelSharedSubscription(appId: $appId, idempotencyKey: $idempotencyKey) {
    appId
    orgId
    planId
    provider
    status
    currentPeriodEnd
  }
})gql";
inline constexpr std::string_view kCancelSharedSubscriptionOperationName = "CancelSharedSubscription";
inline constexpr std::string_view kSetAppSpendCapsIsolatedDocument = R"gql(mutation SetAppSpendCaps($appId: BigInt!, $hourlyLimitCents: BigInt, $dailyLimitCents: BigInt) {
  setAppSpendCaps(
    appId: $appId
    hourlyLimitCents: $hourlyLimitCents
    dailyLimitCents: $dailyLimitCents
  ) {
    appId
    runtimeStatus
    runtimeDenialReason
    hourlyLimitCents
    dailyLimitCents
  }
})gql";
inline constexpr std::string_view kSetAppSpendCapsOperationName = "SetAppSpendCaps";
inline constexpr std::string_view kSetAutoBillingIsolatedDocument = R"gql(mutation SetAutoBilling($orgId: BigInt!, $enabled: Boolean!, $limitCents: BigInt, $rechargeAmountCents: BigInt, $lowWaterThresholdCents: BigInt, $idempotencyKey: String) {
  setAutoBilling(
    orgId: $orgId
    enabled: $enabled
    limitCents: $limitCents
    rechargeAmountCents: $rechargeAmountCents
    lowWaterThresholdCents: $lowWaterThresholdCents
    idempotencyKey: $idempotencyKey
  ) {
    orgId
    enabled
    limitCents
    rechargeAmountCents
    lowWaterThresholdCents
    hasPaymentMethod
  }
})gql";
inline constexpr std::string_view kSetAutoBillingOperationName = "SetAutoBilling";
inline constexpr std::string_view kSetupSharedPaymentMethodIsolatedDocument = R"gql(mutation SetupSharedPaymentMethod($orgId: BigInt!, $idempotencyKey: String) {
  setupSharedPaymentMethod(orgId: $orgId, idempotencyKey: $idempotencyKey) {
    externalCustomerId
    clientSecret
    publishableKey
  }
})gql";
inline constexpr std::string_view kSetupSharedPaymentMethodOperationName = "SetupSharedPaymentMethod";
inline constexpr std::string_view kRemoveSharedPaymentMethodIsolatedDocument = R"gql(mutation RemoveSharedPaymentMethod($orgId: BigInt!, $paymentMethodId: BigInt!, $idempotencyKey: String) {
  removeSharedPaymentMethod(
    orgId: $orgId
    paymentMethodId: $paymentMethodId
    idempotencyKey: $idempotencyKey
  )
})gql";
inline constexpr std::string_view kRemoveSharedPaymentMethodOperationName = "RemoveSharedPaymentMethod";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "SharedEnvPlans") return kSharedEnvPlansIsolatedDocument;
  if (operationName == "OrgFreeAppQuota") return kOrgFreeAppQuotaIsolatedDocument;
  if (operationName == "AppSharedSubscription") return kAppSharedSubscriptionIsolatedDocument;
  if (operationName == "AppRuntimeState") return kAppRuntimeStateIsolatedDocument;
  if (operationName == "OrgAutoBilling") return kOrgAutoBillingIsolatedDocument;
  if (operationName == "OrgPaymentMethods") return kOrgPaymentMethodsIsolatedDocument;
  if (operationName == "PublishAppToShared") return kPublishAppToSharedIsolatedDocument;
  if (operationName == "CancelSharedSubscription") return kCancelSharedSubscriptionIsolatedDocument;
  if (operationName == "SetAppSpendCaps") return kSetAppSpendCapsIsolatedDocument;
  if (operationName == "SetAutoBilling") return kSetAutoBillingIsolatedDocument;
  if (operationName == "SetupSharedPaymentMethod") return kSetupSharedPaymentMethodIsolatedDocument;
  if (operationName == "RemoveSharedPaymentMethod") return kRemoveSharedPaymentMethodIsolatedDocument;
  return {};
}

}  // namespace sharedEnvironment

namespace state {

/// state/DeleteUserAppState.graphql
inline constexpr std::string_view kDeleteUserAppStateDocument = R"gql(mutation DeleteUserAppState($appId: BigInt!) {
  deleteUserAppState(appId: $appId) {
    userId
    appId
    state
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kDeleteUserAppStateIsolatedDocument = R"gql(mutation DeleteUserAppState($appId: BigInt!) {
  deleteUserAppState(appId: $appId) {
    userId
    appId
    state
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kDeleteUserAppStateOperationName = "DeleteUserAppState";

/// state/UpdateUserAppState.graphql
inline constexpr std::string_view kUpdateUserAppStateDocument = R"gql(mutation UpdateUserAppState($input: CreateUserAppStateInput!) {
  updateUserAppState(input: $input) {
    userId
    appId
    state
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateUserAppStateIsolatedDocument = R"gql(mutation UpdateUserAppState($input: CreateUserAppStateInput!) {
  updateUserAppState(input: $input) {
    userId
    appId
    state
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kUpdateUserAppStateOperationName = "UpdateUserAppState";

/// state/UserAppState.graphql
inline constexpr std::string_view kUserAppStateDocument = R"gql(query UserAppState($appId: BigInt!) {
  userAppState(appId: $appId) {
    userId
    appId
    state
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kUserAppStateIsolatedDocument = R"gql(query UserAppState($appId: BigInt!) {
  userAppState(appId: $appId) {
    userId
    appId
    state
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kUserAppStateOperationName = "UserAppState";

/// state/UserAppStates.graphql
inline constexpr std::string_view kUserAppStatesDocument = R"gql(query UserAppStates {
  userAppStates {
    userId
    appId
    state
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kUserAppStatesIsolatedDocument = R"gql(query UserAppStates {
  userAppStates {
    userId
    appId
    state
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kUserAppStatesOperationName = "UserAppStates";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "DeleteUserAppState") return kDeleteUserAppStateIsolatedDocument;
  if (operationName == "UpdateUserAppState") return kUpdateUserAppStateIsolatedDocument;
  if (operationName == "UserAppState") return kUserAppStateIsolatedDocument;
  if (operationName == "UserAppStates") return kUserAppStatesIsolatedDocument;
  return {};
}

}  // namespace state

namespace teams {

/// teams/AddTeamMember.graphql
inline constexpr std::string_view kAddTeamMemberDocument = R"gql(mutation AddTeamMember($groupId: BigInt!, $userId: BigInt!) {
  addTeamMember(groupId: $groupId, userId: $userId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kAddTeamMemberIsolatedDocument = R"gql(mutation AddTeamMember($groupId: BigInt!, $userId: BigInt!) {
  addTeamMember(groupId: $groupId, userId: $userId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kAddTeamMemberOperationName = "AddTeamMember";

/// teams/CreateTeam.graphql
inline constexpr std::string_view kCreateTeamDocument = R"gql(mutation CreateTeam($input: CreateTeamInput!) {
  createTeam(input: $input) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateTeamIsolatedDocument = R"gql(mutation CreateTeam($input: CreateTeamInput!) {
  createTeam(input: $input) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateTeamOperationName = "CreateTeam";

/// teams/CreateTeamRole.graphql
inline constexpr std::string_view kCreateTeamRoleDocument = R"gql(mutation CreateTeamRole($input: CreateGroupRoleInput!) {
  createTeamRole(input: $input) {
    groupRoleId
    groupId
    roleName
    rank
    isSystem
    permissions
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateTeamRoleIsolatedDocument = R"gql(mutation CreateTeamRole($input: CreateGroupRoleInput!) {
  createTeamRole(input: $input) {
    groupRoleId
    groupId
    roleName
    rank
    isSystem
    permissions
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateTeamRoleOperationName = "CreateTeamRole";

/// teams/DeleteTeam.graphql
inline constexpr std::string_view kDeleteTeamDocument = R"gql(mutation DeleteTeam($groupId: BigInt!, $idempotencyKey: String) {
  deleteTeam(groupId: $groupId, idempotencyKey: $idempotencyKey)
})gql";
inline constexpr std::string_view kDeleteTeamIsolatedDocument = R"gql(mutation DeleteTeam($groupId: BigInt!, $idempotencyKey: String) {
  deleteTeam(groupId: $groupId, idempotencyKey: $idempotencyKey)
})gql";
inline constexpr std::string_view kDeleteTeamOperationName = "DeleteTeam";

/// teams/DeleteTeamRole.graphql
inline constexpr std::string_view kDeleteTeamRoleDocument = R"gql(mutation DeleteTeamRole($groupRoleId: BigInt!) {
  deleteTeamRole(groupRoleId: $groupRoleId)
})gql";
inline constexpr std::string_view kDeleteTeamRoleIsolatedDocument = R"gql(mutation DeleteTeamRole($groupRoleId: BigInt!) {
  deleteTeamRole(groupRoleId: $groupRoleId)
})gql";
inline constexpr std::string_view kDeleteTeamRoleOperationName = "DeleteTeamRole";

/// teams/JoinTeam.graphql
inline constexpr std::string_view kJoinTeamDocument = R"gql(mutation JoinTeam($groupId: BigInt!) {
  joinTeam(groupId: $groupId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kJoinTeamIsolatedDocument = R"gql(mutation JoinTeam($groupId: BigInt!) {
  joinTeam(groupId: $groupId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kJoinTeamOperationName = "JoinTeam";

/// teams/LeaveTeam.graphql
inline constexpr std::string_view kLeaveTeamDocument = R"gql(mutation LeaveTeam($groupId: BigInt!, $idempotencyKey: String) {
  leaveTeam(groupId: $groupId, idempotencyKey: $idempotencyKey)
})gql";
inline constexpr std::string_view kLeaveTeamIsolatedDocument = R"gql(mutation LeaveTeam($groupId: BigInt!, $idempotencyKey: String) {
  leaveTeam(groupId: $groupId, idempotencyKey: $idempotencyKey)
})gql";
inline constexpr std::string_view kLeaveTeamOperationName = "LeaveTeam";

/// teams/MyTeams.graphql
inline constexpr std::string_view kMyTeamsDocument = R"gql(query MyTeams($appId: BigInt!) {
  myTeams(appId: $appId) {
    group {
      groupId
      appId
      groupType
      name
      description
      ownerUserId
      membershipPolicy
      status
      defaultRoleId
      createdAt
    }
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
    permissions
    joinedAt
  }
})gql";
inline constexpr std::string_view kMyTeamsIsolatedDocument = R"gql(query MyTeams($appId: BigInt!) {
  myTeams(appId: $appId) {
    group {
      groupId
      appId
      groupType
      name
      description
      ownerUserId
      membershipPolicy
      status
      defaultRoleId
      createdAt
    }
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
    permissions
    joinedAt
  }
})gql";
inline constexpr std::string_view kMyTeamsOperationName = "MyTeams";

/// teams/RemoveTeamMember.graphql
inline constexpr std::string_view kRemoveTeamMemberDocument = R"gql(mutation RemoveTeamMember($groupId: BigInt!, $userId: BigInt!) {
  removeTeamMember(groupId: $groupId, userId: $userId)
})gql";
inline constexpr std::string_view kRemoveTeamMemberIsolatedDocument = R"gql(mutation RemoveTeamMember($groupId: BigInt!, $userId: BigInt!) {
  removeTeamMember(groupId: $groupId, userId: $userId)
})gql";
inline constexpr std::string_view kRemoveTeamMemberOperationName = "RemoveTeamMember";

/// teams/RequestToJoinTeam.graphql
inline constexpr std::string_view kRequestToJoinTeamDocument = R"gql(mutation RequestToJoinTeam($groupId: BigInt!) {
  requestToJoinTeam(groupId: $groupId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kRequestToJoinTeamIsolatedDocument = R"gql(mutation RequestToJoinTeam($groupId: BigInt!) {
  requestToJoinTeam(groupId: $groupId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kRequestToJoinTeamOperationName = "RequestToJoinTeam";

/// teams/SetTeamMemberRoles.graphql
inline constexpr std::string_view kSetTeamMemberRolesDocument = R"gql(mutation SetTeamMemberRoles($input: SetMemberRolesInput!) {
  setTeamMemberRoles(input: $input) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kSetTeamMemberRolesIsolatedDocument = R"gql(mutation SetTeamMemberRoles($input: SetMemberRolesInput!) {
  setTeamMemberRoles(input: $input) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kSetTeamMemberRolesOperationName = "SetTeamMemberRoles";

/// teams/SetTeamPolicy.graphql
inline constexpr std::string_view kSetTeamPolicyDocument = R"gql(mutation SetTeamPolicy($input: SetTeamPolicyInput!) {
  setTeamPolicy(input: $input) {
    appId
    groupType
    creationPolicy
    defaultMembershipPolicy
    maxMembers
    maxGroupsPerUser
  }
})gql";
inline constexpr std::string_view kSetTeamPolicyIsolatedDocument = R"gql(mutation SetTeamPolicy($input: SetTeamPolicyInput!) {
  setTeamPolicy(input: $input) {
    appId
    groupType
    creationPolicy
    defaultMembershipPolicy
    maxMembers
    maxGroupsPerUser
  }
})gql";
inline constexpr std::string_view kSetTeamPolicyOperationName = "SetTeamPolicy";

/// teams/Team.graphql
inline constexpr std::string_view kTeamDocument = R"gql(query Team($groupId: BigInt!) {
  team(groupId: $groupId) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kTeamIsolatedDocument = R"gql(query Team($groupId: BigInt!) {
  team(groupId: $groupId) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kTeamOperationName = "Team";

/// teams/TeamMembers.graphql
inline constexpr std::string_view kTeamMembersDocument = R"gql(query TeamMembers($groupId: BigInt!) {
  teamMembers(groupId: $groupId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kTeamMembersIsolatedDocument = R"gql(query TeamMembers($groupId: BigInt!) {
  teamMembers(groupId: $groupId) {
    groupMemberId
    groupId
    userId
    status
    createdAt
    roles {
      groupRoleId
      roleName
      rank
      isSystem
      permissions
    }
  }
})gql";
inline constexpr std::string_view kTeamMembersOperationName = "TeamMembers";

/// teams/TeamPolicy.graphql
inline constexpr std::string_view kTeamPolicyDocument = R"gql(query TeamPolicy($appId: BigInt!) {
  teamPolicy(appId: $appId) {
    appId
    groupType
    creationPolicy
    defaultMembershipPolicy
    maxMembers
    maxGroupsPerUser
  }
})gql";
inline constexpr std::string_view kTeamPolicyIsolatedDocument = R"gql(query TeamPolicy($appId: BigInt!) {
  teamPolicy(appId: $appId) {
    appId
    groupType
    creationPolicy
    defaultMembershipPolicy
    maxMembers
    maxGroupsPerUser
  }
})gql";
inline constexpr std::string_view kTeamPolicyOperationName = "TeamPolicy";

/// teams/TeamRoles.graphql
inline constexpr std::string_view kTeamRolesDocument = R"gql(query TeamRoles($groupId: BigInt!) {
  teamRoles(groupId: $groupId) {
    groupRoleId
    groupId
    roleName
    rank
    isSystem
    permissions
    createdAt
  }
})gql";
inline constexpr std::string_view kTeamRolesIsolatedDocument = R"gql(query TeamRoles($groupId: BigInt!) {
  teamRoles(groupId: $groupId) {
    groupRoleId
    groupId
    roleName
    rank
    isSystem
    permissions
    createdAt
  }
})gql";
inline constexpr std::string_view kTeamRolesOperationName = "TeamRoles";

/// teams/Teams.graphql
inline constexpr std::string_view kTeamsDocument = R"gql(query Teams($appId: BigInt!) {
  teams(appId: $appId) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kTeamsIsolatedDocument = R"gql(query Teams($appId: BigInt!) {
  teams(appId: $appId) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kTeamsOperationName = "Teams";

/// teams/UpdateTeam.graphql
inline constexpr std::string_view kUpdateTeamDocument = R"gql(mutation UpdateTeam($input: UpdateTeamInput!) {
  updateTeam(input: $input) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kUpdateTeamIsolatedDocument = R"gql(mutation UpdateTeam($input: UpdateTeamInput!) {
  updateTeam(input: $input) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    createdAt
  }
})gql";
inline constexpr std::string_view kUpdateTeamOperationName = "UpdateTeam";

/// teams/UpdateTeamRole.graphql
inline constexpr std::string_view kUpdateTeamRoleDocument = R"gql(mutation UpdateTeamRole($input: UpdateGroupRoleInput!) {
  updateTeamRole(input: $input) {
    groupRoleId
    groupId
    roleName
    rank
    isSystem
    permissions
    createdAt
  }
})gql";
inline constexpr std::string_view kUpdateTeamRoleIsolatedDocument = R"gql(mutation UpdateTeamRole($input: UpdateGroupRoleInput!) {
  updateTeamRole(input: $input) {
    groupRoleId
    groupId
    roleName
    rank
    isSystem
    permissions
    createdAt
  }
})gql";
inline constexpr std::string_view kUpdateTeamRoleOperationName = "UpdateTeamRole";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "AddTeamMember") return kAddTeamMemberIsolatedDocument;
  if (operationName == "CreateTeam") return kCreateTeamIsolatedDocument;
  if (operationName == "CreateTeamRole") return kCreateTeamRoleIsolatedDocument;
  if (operationName == "DeleteTeam") return kDeleteTeamIsolatedDocument;
  if (operationName == "DeleteTeamRole") return kDeleteTeamRoleIsolatedDocument;
  if (operationName == "JoinTeam") return kJoinTeamIsolatedDocument;
  if (operationName == "LeaveTeam") return kLeaveTeamIsolatedDocument;
  if (operationName == "MyTeams") return kMyTeamsIsolatedDocument;
  if (operationName == "RemoveTeamMember") return kRemoveTeamMemberIsolatedDocument;
  if (operationName == "RequestToJoinTeam") return kRequestToJoinTeamIsolatedDocument;
  if (operationName == "SetTeamMemberRoles") return kSetTeamMemberRolesIsolatedDocument;
  if (operationName == "SetTeamPolicy") return kSetTeamPolicyIsolatedDocument;
  if (operationName == "Team") return kTeamIsolatedDocument;
  if (operationName == "TeamMembers") return kTeamMembersIsolatedDocument;
  if (operationName == "TeamPolicy") return kTeamPolicyIsolatedDocument;
  if (operationName == "TeamRoles") return kTeamRolesIsolatedDocument;
  if (operationName == "Teams") return kTeamsIsolatedDocument;
  if (operationName == "UpdateTeam") return kUpdateTeamIsolatedDocument;
  if (operationName == "UpdateTeamRole") return kUpdateTeamRoleIsolatedDocument;
  return {};
}

}  // namespace teams

namespace teleport {

/// teleport/TeleportRequest.graphql
inline constexpr std::string_view kTeleportRequestDocument = R"gql(mutation TeleportRequest($input: TeleportRequestInput!) {
  teleportRequest(input: $input) {
    success
    errorCode
  }
})gql";
inline constexpr std::string_view kTeleportRequestIsolatedDocument = R"gql(mutation TeleportRequest($input: TeleportRequestInput!) {
  teleportRequest(input: $input) {
    success
    errorCode
  }
})gql";
inline constexpr std::string_view kTeleportRequestOperationName = "TeleportRequest";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "TeleportRequest") return kTeleportRequestIsolatedDocument;
  return {};
}

}  // namespace teleport

namespace usage {

/// usage/AppGraphqlOperations.graphql
inline constexpr std::string_view kAppGraphqlOperationsDocument = R"gql(query AppGraphqlOperations(
  $orgId: BigInt!
  $appId: BigInt!
  $since: DateTime!
  $limit: Int
) {
  appGraphqlOperations(
    orgId: $orgId
    appId: $appId
    since: $since
    limit: $limit
  ) {
    operationName
    totalOps
    sendBytes
    recvBytes
  }
})gql";
inline constexpr std::string_view kAppGraphqlOperationsIsolatedDocument = R"gql(query AppGraphqlOperations($orgId: BigInt!, $appId: BigInt!, $since: DateTime!, $limit: Int) {
  appGraphqlOperations(orgId: $orgId, appId: $appId, since: $since, limit: $limit) {
    operationName
    totalOps
    sendBytes
    recvBytes
  }
})gql";
inline constexpr std::string_view kAppGraphqlOperationsOperationName = "AppGraphqlOperations";

/// usage/AppUsageSummary.graphql
inline constexpr std::string_view kAppUsageSummaryDocument = R"gql(query AppUsageSummary(
  $orgId: BigInt!
  $appId: BigInt!
  $since: DateTime!
  $operationLimit: Int
) {
  appUsageSummary(
    orgId: $orgId
    appId: $appId
    since: $since
    operationLimit: $operationLimit
  ) {
    appId
    replicationSendBytes
    replicationRecvBytes
    graphqlSendBytes
    graphqlRecvBytes
    automationRuns
    automationInvocations
    automationComputeUnits
    topGraphqlOperations {
      operationName
      totalOps
      sendBytes
      recvBytes
    }
  }
})gql";
inline constexpr std::string_view kAppUsageSummaryIsolatedDocument = R"gql(query AppUsageSummary($orgId: BigInt!, $appId: BigInt!, $since: DateTime!, $operationLimit: Int) {
  appUsageSummary(
    orgId: $orgId
    appId: $appId
    since: $since
    operationLimit: $operationLimit
  ) {
    appId
    replicationSendBytes
    replicationRecvBytes
    graphqlSendBytes
    graphqlRecvBytes
    automationRuns
    automationInvocations
    automationComputeUnits
    topGraphqlOperations {
      operationName
      totalOps
      sendBytes
      recvBytes
    }
  }
})gql";
inline constexpr std::string_view kAppUsageSummaryOperationName = "AppUsageSummary";

/// usage/PlayerPulse.graphql
inline constexpr std::string_view kPlayerPulseDocument = R"gql(query PlayerPulse($orgId: BigInt!) {
  playerPulse(orgId: $orgId) {
    orgLivePlayers
    orgAllTimePeak
    orgAllTimePeakAt
    globalLivePlayers
    percentile
    poolSize
  }
})gql";
inline constexpr std::string_view kPlayerPulseIsolatedDocument = R"gql(query PlayerPulse($orgId: BigInt!) {
  playerPulse(orgId: $orgId) {
    orgLivePlayers
    orgAllTimePeak
    orgAllTimePeakAt
    globalLivePlayers
    percentile
    poolSize
  }
})gql";
inline constexpr std::string_view kPlayerPulseOperationName = "PlayerPulse";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "AppGraphqlOperations") return kAppGraphqlOperationsIsolatedDocument;
  if (operationName == "AppUsageSummary") return kAppUsageSummaryIsolatedDocument;
  if (operationName == "PlayerPulse") return kPlayerPulseIsolatedDocument;
  return {};
}

}  // namespace usage

namespace users {

/// users/DeleteMyAccount.graphql
inline constexpr std::string_view kDeleteMyAccountDocument = R"gql(mutation DeleteMyAccount {
  deleteMyAccount
})gql";
inline constexpr std::string_view kDeleteMyAccountIsolatedDocument = R"gql(mutation DeleteMyAccount {
  deleteMyAccount
})gql";
inline constexpr std::string_view kDeleteMyAccountOperationName = "DeleteMyAccount";

/// users/ForceLogoutUser.graphql
inline constexpr std::string_view kForceLogoutUserDocument = R"gql(mutation ForceLogoutUser($userId: BigInt!) {
  forceLogoutUser(userId: $userId)
})gql";
inline constexpr std::string_view kForceLogoutUserIsolatedDocument = R"gql(mutation ForceLogoutUser($userId: BigInt!) {
  forceLogoutUser(userId: $userId)
})gql";
inline constexpr std::string_view kForceLogoutUserOperationName = "ForceLogoutUser";

/// users/FreePlayWindow.graphql
inline constexpr std::string_view kFreePlayWindowDocument = R"gql(query FreePlayWindow {
  freePlayWindowInfo {
    isCurrentlyActive
    description
    nextWindowStart
  }
})gql";
inline constexpr std::string_view kFreePlayWindowIsolatedDocument = R"gql(query FreePlayWindow {
  freePlayWindowInfo {
    isCurrentlyActive
    description
    nextWindowStart
  }
})gql";
inline constexpr std::string_view kFreePlayWindowOperationName = "FreePlayWindow";

/// users/Me.graphql
inline constexpr std::string_view kMeDocument = R"gql(query Me {
  me {
    userId
    email
    gamertag
    disambiguation
    state
    isConfirmed
    createdAt
    grantEarlyAccess
    grantEarlyAccessOverride
    orgId
    externalId
    userType
    isSuperAdmin
  }
})gql";
inline constexpr std::string_view kMeIsolatedDocument = R"gql(query Me {
  me {
    userId
    email
    gamertag
    disambiguation
    state
    isConfirmed
    createdAt
    grantEarlyAccess
    grantEarlyAccessOverride
    orgId
    externalId
    userType
    isSuperAdmin
  }
})gql";
inline constexpr std::string_view kMeOperationName = "Me";

/// users/SetEarlyAccessOverride.graphql
inline constexpr std::string_view kSetEarlyAccessOverrideDocument = R"gql(mutation SetEarlyAccessOverride($userId: BigInt!, $value: Boolean!) {
  setEarlyAccessOverride(userId: $userId, value: $value) {
    userId
    grantEarlyAccessOverride
  }
})gql";
inline constexpr std::string_view kSetEarlyAccessOverrideIsolatedDocument = R"gql(mutation SetEarlyAccessOverride($userId: BigInt!, $value: Boolean!) {
  setEarlyAccessOverride(userId: $userId, value: $value) {
    userId
    grantEarlyAccessOverride
  }
})gql";
inline constexpr std::string_view kSetEarlyAccessOverrideOperationName = "SetEarlyAccessOverride";

/// users/SetOperator.graphql
inline constexpr std::string_view kSetOperatorDocument = R"gql(mutation SetOperator($userId: BigInt!, $value: Boolean!) {
  setOperator(userId: $userId, value: $value) {
    userId
    isOperator
    isSuperAdmin
  }
})gql";
inline constexpr std::string_view kSetOperatorIsolatedDocument = R"gql(mutation SetOperator($userId: BigInt!, $value: Boolean!) {
  setOperator(userId: $userId, value: $value) {
    userId
    isOperator
    isSuperAdmin
  }
})gql";
inline constexpr std::string_view kSetOperatorOperationName = "SetOperator";

/// users/SetSuperAdmin.graphql
inline constexpr std::string_view kSetSuperAdminDocument = R"gql(mutation SetSuperAdmin($userId: BigInt!, $value: Boolean!) {
  setSuperAdmin(userId: $userId, value: $value) {
    userId
    isSuperAdmin
  }
})gql";
inline constexpr std::string_view kSetSuperAdminIsolatedDocument = R"gql(mutation SetSuperAdmin($userId: BigInt!, $value: Boolean!) {
  setSuperAdmin(userId: $userId, value: $value) {
    userId
    isSuperAdmin
  }
})gql";
inline constexpr std::string_view kSetSuperAdminOperationName = "SetSuperAdmin";

/// users/UpdateGamertag.graphql
inline constexpr std::string_view kUpdateGamertagDocument = R"gql(mutation UpdateGamertag($input: UpdateGamertagInput!) {
  updateGamertag(input: $input) {
    userId
    gamertag
    disambiguation
    userType
  }
})gql";
inline constexpr std::string_view kUpdateGamertagIsolatedDocument = R"gql(mutation UpdateGamertag($input: UpdateGamertagInput!) {
  updateGamertag(input: $input) {
    userId
    gamertag
    disambiguation
    userType
  }
})gql";
inline constexpr std::string_view kUpdateGamertagOperationName = "UpdateGamertag";

/// users/UpdateUserState.graphql
inline constexpr std::string_view kUpdateUserStateDocument = R"gql(mutation UpdateUserState($input: UpdateUserStateInput!) {
  updateUserState(input: $input) {
    userId
    state
    userType
  }
})gql";
inline constexpr std::string_view kUpdateUserStateIsolatedDocument = R"gql(mutation UpdateUserState($input: UpdateUserStateInput!) {
  updateUserState(input: $input) {
    userId
    state
    userType
  }
})gql";
inline constexpr std::string_view kUpdateUserStateOperationName = "UpdateUserState";

/// users/UpdateUserType.graphql
inline constexpr std::string_view kUpdateUserTypeDocument = R"gql(mutation UpdateUserType($userId: BigInt!, $value: String!) {
  updateUserType(userId: $userId, value: $value) {
    userId
    userType
  }
})gql";
inline constexpr std::string_view kUpdateUserTypeIsolatedDocument = R"gql(mutation UpdateUserType($userId: BigInt!, $value: String!) {
  updateUserType(userId: $userId, value: $value) {
    userId
    userType
  }
})gql";
inline constexpr std::string_view kUpdateUserTypeOperationName = "UpdateUserType";

/// users/User.graphql
inline constexpr std::string_view kUserDocument = R"gql(query User($id: BigInt!) {
  user(id: $id) {
    userId
    email
    gamertag
    disambiguation
    state
    isConfirmed
    createdAt
    grantEarlyAccess
    grantEarlyAccessOverride
    orgId
    externalId
    userType
    isSuperAdmin
  }
})gql";
inline constexpr std::string_view kUserIsolatedDocument = R"gql(query User($id: BigInt!) {
  user(id: $id) {
    userId
    email
    gamertag
    disambiguation
    state
    isConfirmed
    createdAt
    grantEarlyAccess
    grantEarlyAccessOverride
    orgId
    externalId
    userType
    isSuperAdmin
  }
})gql";
inline constexpr std::string_view kUserOperationName = "User";

/// users/UsersPaginated.graphql
inline constexpr std::string_view kUsersPaginatedDocument = R"gql(query UsersPaginated($query: String, $limit: Int, $offset: Int) {
  usersPaginated(query: $query, limit: $limit, offset: $offset) {
    items {
      userId
      email
      gamertag
      disambiguation
      isConfirmed
      createdAt
      grantEarlyAccess
      grantEarlyAccessOverride
      orgId
      externalId
      userType
      isSuperAdmin
    }
    pageInfo {
      totalCount
      limit
      offset
    }
  }
}

query UsersConnection($first: Int, $after: String, $query: String) {
  usersConnection(first: $first, after: $after, query: $query) {
    edges {
      cursor
      node {
        userId
        email
        gamertag
        disambiguation
        isConfirmed
        createdAt
        grantEarlyAccess
        grantEarlyAccessOverride
        orgId
        externalId
        userType
        isSuperAdmin
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kUsersPaginatedIsolatedDocument = R"gql(query UsersPaginated($query: String, $limit: Int, $offset: Int) {
  usersPaginated(query: $query, limit: $limit, offset: $offset) {
    items {
      userId
      email
      gamertag
      disambiguation
      isConfirmed
      createdAt
      grantEarlyAccess
      grantEarlyAccessOverride
      orgId
      externalId
      userType
      isSuperAdmin
    }
    pageInfo {
      totalCount
      limit
      offset
    }
  }
})gql";
inline constexpr std::string_view kUsersPaginatedOperationName = "UsersPaginated";
inline constexpr std::string_view kUsersConnectionIsolatedDocument = R"gql(query UsersConnection($first: Int, $after: String, $query: String) {
  usersConnection(first: $first, after: $after, query: $query) {
    edges {
      cursor
      node {
        userId
        email
        gamertag
        disambiguation
        isConfirmed
        createdAt
        grantEarlyAccess
        grantEarlyAccessOverride
        orgId
        externalId
        userType
        isSuperAdmin
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kUsersConnectionOperationName = "UsersConnection";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "DeleteMyAccount") return kDeleteMyAccountIsolatedDocument;
  if (operationName == "ForceLogoutUser") return kForceLogoutUserIsolatedDocument;
  if (operationName == "FreePlayWindow") return kFreePlayWindowIsolatedDocument;
  if (operationName == "Me") return kMeIsolatedDocument;
  if (operationName == "SetEarlyAccessOverride") return kSetEarlyAccessOverrideIsolatedDocument;
  if (operationName == "SetOperator") return kSetOperatorIsolatedDocument;
  if (operationName == "SetSuperAdmin") return kSetSuperAdminIsolatedDocument;
  if (operationName == "UpdateGamertag") return kUpdateGamertagIsolatedDocument;
  if (operationName == "UpdateUserState") return kUpdateUserStateIsolatedDocument;
  if (operationName == "UpdateUserType") return kUpdateUserTypeIsolatedDocument;
  if (operationName == "User") return kUserIsolatedDocument;
  if (operationName == "UsersPaginated") return kUsersPaginatedIsolatedDocument;
  if (operationName == "UsersConnection") return kUsersConnectionIsolatedDocument;
  return {};
}

}  // namespace users

namespace voxels {

/// voxels/ListVoxelUpdatesByDistance.graphql
inline constexpr std::string_view kListVoxelUpdatesByDistanceDocument = R"gql(query ListVoxelUpdatesByDistance($input: ListVoxelUpdatesByDistanceInput!) {
  listVoxelUpdatesByDistance(input: $input) {
    centerCoordinate {
      x
      y
      z
    }
    limit
    skip
    chunks {
      coordinates {
        x
        y
        z
      }
      voxels {
        voxelUpdateId
        appId
        location {
          x
          y
          z
        }
        voxelType
        state
        createdBy
        createdAt
      }
    }
  }
})gql";
inline constexpr std::string_view kListVoxelUpdatesByDistanceIsolatedDocument = R"gql(query ListVoxelUpdatesByDistance($input: ListVoxelUpdatesByDistanceInput!) {
  listVoxelUpdatesByDistance(input: $input) {
    centerCoordinate {
      x
      y
      z
    }
    limit
    skip
    chunks {
      coordinates {
        x
        y
        z
      }
      voxels {
        voxelUpdateId
        appId
        location {
          x
          y
          z
        }
        voxelType
        state
        createdBy
        createdAt
      }
    }
  }
})gql";
inline constexpr std::string_view kListVoxelUpdatesByDistanceOperationName = "ListVoxelUpdatesByDistance";

/// voxels/ListVoxels.graphql
inline constexpr std::string_view kListVoxelsDocument = R"gql(query ListVoxels($input: ListVoxelsInput!) {
  listVoxels(input: $input) {
    voxelUpdateId
    appId
    coordinates {
      x
      y
      z
    }
    location {
      x
      y
      z
    }
    voxelType
    state
    createdBy
    createdAt
  }
})gql";
inline constexpr std::string_view kListVoxelsIsolatedDocument = R"gql(query ListVoxels($input: ListVoxelsInput!) {
  listVoxels(input: $input) {
    voxelUpdateId
    appId
    coordinates {
      x
      y
      z
    }
    location {
      x
      y
      z
    }
    voxelType
    state
    createdBy
    createdAt
  }
})gql";
inline constexpr std::string_view kListVoxelsOperationName = "ListVoxels";

/// voxels/RollbackVoxelUpdates.graphql
inline constexpr std::string_view kRollbackVoxelUpdatesDocument = R"gql(mutation RollbackVoxelUpdates($input: RollbackVoxelUpdatesInput!) {
  rollbackVoxelUpdates(input: $input) {
    appId
    coordinates {
      x
      y
      z
    }
    location {
      x
      y
      z
    }
    fromVoxelType
    toVoxelType
    plannedAction
    applied
    reason
  }
})gql";
inline constexpr std::string_view kRollbackVoxelUpdatesIsolatedDocument = R"gql(mutation RollbackVoxelUpdates($input: RollbackVoxelUpdatesInput!) {
  rollbackVoxelUpdates(input: $input) {
    appId
    coordinates {
      x
      y
      z
    }
    location {
      x
      y
      z
    }
    fromVoxelType
    toVoxelType
    plannedAction
    applied
    reason
  }
})gql";
inline constexpr std::string_view kRollbackVoxelUpdatesOperationName = "RollbackVoxelUpdates";

/// voxels/UpdateVoxel.graphql
inline constexpr std::string_view kUpdateVoxelDocument = R"gql(mutation UpdateVoxel($input: UpdateVoxelInput!) {
  updateVoxel(input: $input) {
    voxelUpdateId
    appId
    coordinates {
      x
      y
      z
    }
    location {
      x
      y
      z
    }
    voxelType
    state
    createdBy
    createdAt
  }
})gql";
inline constexpr std::string_view kUpdateVoxelIsolatedDocument = R"gql(mutation UpdateVoxel($input: UpdateVoxelInput!) {
  updateVoxel(input: $input) {
    voxelUpdateId
    appId
    coordinates {
      x
      y
      z
    }
    location {
      x
      y
      z
    }
    voxelType
    state
    createdBy
    createdAt
  }
})gql";
inline constexpr std::string_view kUpdateVoxelOperationName = "UpdateVoxel";

/// voxels/VoxelUpdateHistory.graphql
inline constexpr std::string_view kVoxelUpdateHistoryDocument = R"gql(query VoxelUpdateHistory(
  $appId: BigInt!
  $userId: BigInt
  $from: DateTime
  $to: DateTime
  $limit: Int
  $offset: Int
) {
  voxelUpdateHistory(
    appId: $appId
    userId: $userId
    from: $from
    to: $to
    limit: $limit
    offset: $offset
  ) {
    id
    appId
    coordinates {
      x
      y
      z
    }
    location {
      x
      y
      z
    }
    oldVoxelType
    newVoxelType
    changedBy
    changedAt
  }
}

query VoxelUpdateHistoryConnection(
  $appId: BigInt!
  $userId: BigInt
  $from: DateTime
  $to: DateTime
  $first: Int
  $after: String
) {
  voxelUpdateHistoryConnection(
    appId: $appId
    userId: $userId
    from: $from
    to: $to
    first: $first
    after: $after
  ) {
    edges {
      cursor
      node {
        id
        appId
        coordinates {
          x
          y
          z
        }
        location {
          x
          y
          z
        }
        oldVoxelType
        newVoxelType
        changedBy
        changedAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kVoxelUpdateHistoryIsolatedDocument = R"gql(query VoxelUpdateHistory($appId: BigInt!, $userId: BigInt, $from: DateTime, $to: DateTime, $limit: Int, $offset: Int) {
  voxelUpdateHistory(
    appId: $appId
    userId: $userId
    from: $from
    to: $to
    limit: $limit
    offset: $offset
  ) {
    id
    appId
    coordinates {
      x
      y
      z
    }
    location {
      x
      y
      z
    }
    oldVoxelType
    newVoxelType
    changedBy
    changedAt
  }
})gql";
inline constexpr std::string_view kVoxelUpdateHistoryOperationName = "VoxelUpdateHistory";
inline constexpr std::string_view kVoxelUpdateHistoryConnectionIsolatedDocument = R"gql(query VoxelUpdateHistoryConnection($appId: BigInt!, $userId: BigInt, $from: DateTime, $to: DateTime, $first: Int, $after: String) {
  voxelUpdateHistoryConnection(
    appId: $appId
    userId: $userId
    from: $from
    to: $to
    first: $first
    after: $after
  ) {
    edges {
      cursor
      node {
        id
        appId
        coordinates {
          x
          y
          z
        }
        location {
          x
          y
          z
        }
        oldVoxelType
        newVoxelType
        changedBy
        changedAt
      }
    }
    pageInfo {
      hasNextPage
      hasPreviousPage
      startCursor
      endCursor
    }
    totalCount
  }
})gql";
inline constexpr std::string_view kVoxelUpdateHistoryConnectionOperationName = "VoxelUpdateHistoryConnection";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "ListVoxelUpdatesByDistance") return kListVoxelUpdatesByDistanceIsolatedDocument;
  if (operationName == "ListVoxels") return kListVoxelsIsolatedDocument;
  if (operationName == "RollbackVoxelUpdates") return kRollbackVoxelUpdatesIsolatedDocument;
  if (operationName == "UpdateVoxel") return kUpdateVoxelIsolatedDocument;
  if (operationName == "VoxelUpdateHistory") return kVoxelUpdateHistoryIsolatedDocument;
  if (operationName == "VoxelUpdateHistoryConnection") return kVoxelUpdateHistoryConnectionIsolatedDocument;
  return {};
}

}  // namespace voxels

}  // namespace crowdy::gen
