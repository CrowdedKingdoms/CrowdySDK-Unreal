// GENERATED FILE — do not edit by hand.
// Regenerate with: node scripts/codegen.mjs
// Inputs: operations/**/*.graphql and schema.gql (synced from the published
// SDL at https://docs.crowdedkingdoms.com/schema/game-api.graphql).
// schema.gql sha256: 4b9c470a996a1c6c423849546fc23eda164605753e2aa2083485156b27a8da1c
// operations sha256: 56c3830a074c020782259846f3597932060a419b7b0868ad6b0d1b70e166fa21

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

/// appAccess/AppFeatures.graphql
inline constexpr std::string_view kAppFeaturesDocument = R"gql(query AppFeatures($appId: BigInt!) {
  gameModelFeatures(appId: $appId) {
    appId
    featureKey
    description
  }
})gql";
inline constexpr std::string_view kAppFeaturesIsolatedDocument = R"gql(query AppFeatures($appId: BigInt!) {
  gameModelFeatures(appId: $appId) {
    appId
    featureKey
    description
  }
})gql";
inline constexpr std::string_view kAppFeaturesOperationName = "AppFeatures";

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
    suspendedUntil
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
        suspendedUntil
    suspendedUntil
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
    suspendedUntil
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
        suspendedUntil
        suspendedUntil
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
    suspendedUntil
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
    suspendedUntil
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

/// appAccess/DefineAppFeature.graphql
inline constexpr std::string_view kDefineAppFeatureDocument = R"gql(mutation DefineAppFeature($input: DefineAppFeatureInput!) {
  gameModelDefineFeature(input: $input) {
    appId
    featureKey
    description
  }
})gql";
inline constexpr std::string_view kDefineAppFeatureIsolatedDocument = R"gql(mutation DefineAppFeature($input: DefineAppFeatureInput!) {
  gameModelDefineFeature(input: $input) {
    appId
    featureKey
    description
  }
})gql";
inline constexpr std::string_view kDefineAppFeatureOperationName = "DefineAppFeature";

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
    suspendedUntil
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
    suspendedUntil
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
    suspendedUntil
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
    suspendedUntil
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kGrantMyAppAccessOperationName = "GrantMyAppAccess";

/// appAccess/GrantTierFeature.graphql
inline constexpr std::string_view kGrantTierFeatureDocument = R"gql(mutation GrantTierFeature($input: GrantTierFeatureInput!) {
  gameModelGrantTierFeature(input: $input) {
    appId
    tierId
    featureKey
  }
})gql";
inline constexpr std::string_view kGrantTierFeatureIsolatedDocument = R"gql(mutation GrantTierFeature($input: GrantTierFeatureInput!) {
  gameModelGrantTierFeature(input: $input) {
    appId
    tierId
    featureKey
  }
})gql";
inline constexpr std::string_view kGrantTierFeatureOperationName = "GrantTierFeature";

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
    suspendedUntil
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
    suspendedUntil
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kMyAppAccessOperationName = "MyAppAccess";

/// appAccess/ResyncTierGridPermissions.graphql
inline constexpr std::string_view kResyncTierGridPermissionsDocument = R"gql(mutation ResyncTierGridPermissions($appId: BigInt!) {
  resyncTierGridPermissions(appId: $appId)
})gql";
inline constexpr std::string_view kResyncTierGridPermissionsIsolatedDocument = R"gql(mutation ResyncTierGridPermissions($appId: BigInt!) {
  resyncTierGridPermissions(appId: $appId)
})gql";
inline constexpr std::string_view kResyncTierGridPermissionsOperationName = "ResyncTierGridPermissions";

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
    suspendedUntil
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
    suspendedUntil
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kRevokeAppAccessOperationName = "RevokeAppAccess";

/// appAccess/RevokeTierFeature.graphql
inline constexpr std::string_view kRevokeTierFeatureDocument = R"gql(mutation RevokeTierFeature($input: GrantTierFeatureInput!) {
  gameModelRevokeTierFeature(input: $input)
})gql";
inline constexpr std::string_view kRevokeTierFeatureIsolatedDocument = R"gql(mutation RevokeTierFeature($input: GrantTierFeatureInput!) {
  gameModelRevokeTierFeature(input: $input)
})gql";
inline constexpr std::string_view kRevokeTierFeatureOperationName = "RevokeTierFeature";

/// appAccess/RuntimePermissions.graphql
inline constexpr std::string_view kRuntimePermissionsDocument = R"gql(query RuntimePermissions {
  runtimePermissions
})gql";
inline constexpr std::string_view kRuntimePermissionsIsolatedDocument = R"gql(query RuntimePermissions {
  runtimePermissions
})gql";
inline constexpr std::string_view kRuntimePermissionsOperationName = "RuntimePermissions";

/// appAccess/SuspendAppAccess.graphql
inline constexpr std::string_view kSuspendAppAccessDocument = R"gql(mutation SuspendAppAccess(
  $appId: BigInt!
  $userId: BigInt!
  $until: DateTime!
  $idempotencyKey: String
) {
  suspendAppAccess(
    appId: $appId
    userId: $userId
    until: $until
    idempotencyKey: $idempotencyKey
  ) {
    appUserAccessId
    appId
    userId
    tierId
    status
    grantedBy
    subscriptionId
    expiresAt
    suspendedUntil
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kSuspendAppAccessIsolatedDocument = R"gql(mutation SuspendAppAccess($appId: BigInt!, $userId: BigInt!, $until: DateTime!, $idempotencyKey: String) {
  suspendAppAccess(
    appId: $appId
    userId: $userId
    until: $until
    idempotencyKey: $idempotencyKey
  ) {
    appUserAccessId
    appId
    userId
    tierId
    status
    grantedBy
    subscriptionId
    expiresAt
    suspendedUntil
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kSuspendAppAccessOperationName = "SuspendAppAccess";

/// appAccess/TierFeatures.graphql
inline constexpr std::string_view kTierFeaturesDocument = R"gql(query TierFeatures($appId: BigInt!, $tierId: BigInt) {
  gameModelTierFeatures(appId: $appId, tierId: $tierId) {
    appId
    tierId
    featureKey
  }
})gql";
inline constexpr std::string_view kTierFeaturesIsolatedDocument = R"gql(query TierFeatures($appId: BigInt!, $tierId: BigInt) {
  gameModelTierFeatures(appId: $appId, tierId: $tierId) {
    appId
    tierId
    featureKey
  }
})gql";
inline constexpr std::string_view kTierFeaturesOperationName = "TierFeatures";

/// appAccess/UnsuspendAppAccess.graphql
inline constexpr std::string_view kUnsuspendAppAccessDocument = R"gql(mutation UnsuspendAppAccess($appId: BigInt!, $userId: BigInt!) {
  unsuspendAppAccess(appId: $appId, userId: $userId) {
    appUserAccessId
    appId
    userId
    tierId
    status
    grantedBy
    subscriptionId
    expiresAt
    suspendedUntil
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kUnsuspendAppAccessIsolatedDocument = R"gql(mutation UnsuspendAppAccess($appId: BigInt!, $userId: BigInt!) {
  unsuspendAppAccess(appId: $appId, userId: $userId) {
    appUserAccessId
    appId
    userId
    tierId
    status
    grantedBy
    subscriptionId
    expiresAt
    suspendedUntil
    createdAt
    updatedAt
  }
})gql";
inline constexpr std::string_view kUnsuspendAppAccessOperationName = "UnsuspendAppAccess";

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
  if (operationName == "AppFeatures") return kAppFeaturesIsolatedDocument;
  if (operationName == "AppGrantMemberCandidates") return kAppGrantMemberCandidatesIsolatedDocument;
  if (operationName == "AppUserAccessByApp") return kAppUserAccessByAppIsolatedDocument;
  if (operationName == "AppUserAccessConnection") return kAppUserAccessConnectionIsolatedDocument;
  if (operationName == "ArchiveAccessTier") return kArchiveAccessTierIsolatedDocument;
  if (operationName == "ClaimFreeAppAccess") return kClaimFreeAppAccessIsolatedDocument;
  if (operationName == "CreateAccessTier") return kCreateAccessTierIsolatedDocument;
  if (operationName == "DefineAppFeature") return kDefineAppFeatureIsolatedDocument;
  if (operationName == "GrantAppAccess") return kGrantAppAccessIsolatedDocument;
  if (operationName == "GrantMyAppAccess") return kGrantMyAppAccessIsolatedDocument;
  if (operationName == "GrantTierFeature") return kGrantTierFeatureIsolatedDocument;
  if (operationName == "MyAppAccess") return kMyAppAccessIsolatedDocument;
  if (operationName == "ResyncTierGridPermissions") return kResyncTierGridPermissionsIsolatedDocument;
  if (operationName == "RevokeAppAccess") return kRevokeAppAccessIsolatedDocument;
  if (operationName == "RevokeTierFeature") return kRevokeTierFeatureIsolatedDocument;
  if (operationName == "RuntimePermissions") return kRuntimePermissionsIsolatedDocument;
  if (operationName == "SuspendAppAccess") return kSuspendAppAccessIsolatedDocument;
  if (operationName == "TierFeatures") return kTierFeaturesIsolatedDocument;
  if (operationName == "UnsuspendAppAccess") return kUnsuspendAppAccessIsolatedDocument;
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
    wildernessWritesOpen
    replayLoggingEnabled
    claimOwnerKeys
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
    wildernessWritesOpen
    replayLoggingEnabled
    claimOwnerKeys
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
    wildernessWritesOpen
    replayLoggingEnabled
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
    wildernessWritesOpen
    replayLoggingEnabled
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
    wildernessWritesOpen
    replayLoggingEnabled
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
    wildernessWritesOpen
    replayLoggingEnabled
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
    wildernessWritesOpen
    replayLoggingEnabled
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
    wildernessWritesOpen
    replayLoggingEnabled
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
    wildernessWritesOpen
    replayLoggingEnabled
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
    wildernessWritesOpen
    replayLoggingEnabled
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
    wildernessWritesOpen
    replayLoggingEnabled
    claimOwnerKeys
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
    wildernessWritesOpen
    replayLoggingEnabled
    claimOwnerKeys
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
    balanceMicrousd
    holdsMicrousd
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
    balanceMicrousd
    holdsMicrousd
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
    amountMicrousd
    balanceAfterMicrousd
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
        amountMicrousd
        balanceAfterMicrousd
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
    amountMicrousd
    balanceAfterMicrousd
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
        amountMicrousd
        balanceAfterMicrousd
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
    voxelStatesTruncated
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
    voxelStatesTruncated
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
      voxelStates {
        voxelCoord {
          x
          y
          z
        }
        voxelType
        state
      }
      voxelStatesTruncated
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
      voxelStates {
        voxelCoord {
          x
          y
          z
        }
        voxelType
        state
      }
      voxelStatesTruncated
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

namespace computeUnits {

/// computeUnits/AppComputeBudget.graphql
inline constexpr std::string_view kAppComputeBudgetDocument = R"gql(fragment AppComputeBudgetInfoFields on AppComputeBudgetInfo {
  appId
  unitsPerMinute
  enforce
  note
  updatedAt
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

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "AppComputeBudget") return kAppComputeBudgetIsolatedDocument;
  if (operationName == "SetAppComputeBudget") return kSetAppComputeBudgetIsolatedDocument;
  if (operationName == "ClearAppComputeBudget") return kClearAppComputeBudgetIsolatedDocument;
  return {};
}

}  // namespace computeUnits

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
  source
  githubOwner
  githubRepo
  githubBranch
  githubSha
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
    source
    githubOwner
    githubRepo
    githubBranch
    githubSha
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
    source
    githubOwner
    githubRepo
    githubBranch
    githubSha
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
  source
  githubOwner
  githubRepo
  githubBranch
  githubSha
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
  source
  githubOwner
  githubRepo
  githubBranch
  githubSha
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
  source
  githubOwner
  githubRepo
  githubBranch
  githubSha
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
  source
  githubOwner
  githubRepo
  githubBranch
  githubSha
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
  source
  githubOwner
  githubRepo
  githubBranch
  githubSha
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
  source
  githubOwner
  githubRepo
  githubBranch
  githubSha
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
  source
  githubOwner
  githubRepo
  githubBranch
  githubSha
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

/// crowdyStudio/CrowdyStudioGitHub.graphql
inline constexpr std::string_view kCrowdyStudioGitHubDocument = R"gql(fragment CrowdyStudioGitHubStatusFields on CrowdyStudioGitHubStatus {
  configured
  connected
  accountLogin
  accountType
  owner
  repo
  branch
  githubSha
  installUrl
}

query CrowdyStudioGitHubStatus($appId: BigInt, $projectId: String) {
  crowdyStudioGitHubStatus(appId: $appId, projectId: $projectId) {
    ...CrowdyStudioGitHubStatusFields
  }
}

mutation CrowdyStudioGitHubConnectUrl {
  crowdyStudioGitHubConnectUrl {
    connectUrl
  }
}

query CrowdyStudioGitHubRepos {
  crowdyStudioGitHubRepos {
    owner
    name
    fullName
    private
    defaultBranch
  }
}

mutation CrowdyStudioGitHubBind($input: BindCrowdyStudioGitHubInput!) {
  crowdyStudioGitHubBind(input: $input) {
    ...CrowdyStudioGitHubStatusFields
  }
}

mutation CrowdyStudioGitHubUnbind($input: CrowdyStudioGitHubProjectInput!) {
  crowdyStudioGitHubUnbind(input: $input) {
    ...CrowdyStudioGitHubStatusFields
  }
}

mutation CrowdyStudioGitHubRefresh($input: CrowdyStudioGitHubProjectInput!) {
  crowdyStudioGitHubRefresh(input: $input) {
    ...CrowdyStudioGitHubStatusFields
  }
}

query CrowdyStudioGitHubLayout($input: CrowdyStudioGitHubAtCommitInput!) {
  crowdyStudioGitHubLayout(input: $input) {
    commitSha
    server
    client
    assets
    fromFile
  }
}

query CrowdyStudioGitHubTree($input: CrowdyStudioGitHubAtCommitInput!) {
  crowdyStudioGitHubTree(input: $input) {
    commitSha
    entries {
      path
      type
      sha
      size
    }
  }
}

query CrowdyStudioGitHubFile($input: CrowdyStudioGitHubFileInput!) {
  crowdyStudioGitHubFile(input: $input) {
    path
    content
    sha
    commitSha
  }
}

mutation CrowdyStudioGitHubPutFile($input: CrowdyStudioGitHubPutFileInput!) {
  crowdyStudioGitHubPutFile(input: $input) {
    path
    content
    sha
    commitSha
  }
}

mutation CrowdyStudioGitHubDeleteFile($input: CrowdyStudioGitHubDeleteFileInput!) {
  crowdyStudioGitHubDeleteFile(input: $input) {
    ...CrowdyStudioGitHubStatusFields
  }
})gql";
inline constexpr std::string_view kCrowdyStudioGitHubStatusIsolatedDocument = R"gql(query CrowdyStudioGitHubStatus($appId: BigInt, $projectId: String) {
  crowdyStudioGitHubStatus(appId: $appId, projectId: $projectId) {
    ...CrowdyStudioGitHubStatusFields
  }
}

fragment CrowdyStudioGitHubStatusFields on CrowdyStudioGitHubStatus {
  configured
  connected
  accountLogin
  accountType
  owner
  repo
  branch
  githubSha
  installUrl
})gql";
inline constexpr std::string_view kCrowdyStudioGitHubStatusOperationName = "CrowdyStudioGitHubStatus";
inline constexpr std::string_view kCrowdyStudioGitHubConnectUrlIsolatedDocument = R"gql(mutation CrowdyStudioGitHubConnectUrl {
  crowdyStudioGitHubConnectUrl {
    connectUrl
  }
})gql";
inline constexpr std::string_view kCrowdyStudioGitHubConnectUrlOperationName = "CrowdyStudioGitHubConnectUrl";
inline constexpr std::string_view kCrowdyStudioGitHubReposIsolatedDocument = R"gql(query CrowdyStudioGitHubRepos {
  crowdyStudioGitHubRepos {
    owner
    name
    fullName
    private
    defaultBranch
  }
})gql";
inline constexpr std::string_view kCrowdyStudioGitHubReposOperationName = "CrowdyStudioGitHubRepos";
inline constexpr std::string_view kCrowdyStudioGitHubBindIsolatedDocument = R"gql(mutation CrowdyStudioGitHubBind($input: BindCrowdyStudioGitHubInput!) {
  crowdyStudioGitHubBind(input: $input) {
    ...CrowdyStudioGitHubStatusFields
  }
}

fragment CrowdyStudioGitHubStatusFields on CrowdyStudioGitHubStatus {
  configured
  connected
  accountLogin
  accountType
  owner
  repo
  branch
  githubSha
  installUrl
})gql";
inline constexpr std::string_view kCrowdyStudioGitHubBindOperationName = "CrowdyStudioGitHubBind";
inline constexpr std::string_view kCrowdyStudioGitHubUnbindIsolatedDocument = R"gql(mutation CrowdyStudioGitHubUnbind($input: CrowdyStudioGitHubProjectInput!) {
  crowdyStudioGitHubUnbind(input: $input) {
    ...CrowdyStudioGitHubStatusFields
  }
}

fragment CrowdyStudioGitHubStatusFields on CrowdyStudioGitHubStatus {
  configured
  connected
  accountLogin
  accountType
  owner
  repo
  branch
  githubSha
  installUrl
})gql";
inline constexpr std::string_view kCrowdyStudioGitHubUnbindOperationName = "CrowdyStudioGitHubUnbind";
inline constexpr std::string_view kCrowdyStudioGitHubRefreshIsolatedDocument = R"gql(mutation CrowdyStudioGitHubRefresh($input: CrowdyStudioGitHubProjectInput!) {
  crowdyStudioGitHubRefresh(input: $input) {
    ...CrowdyStudioGitHubStatusFields
  }
}

fragment CrowdyStudioGitHubStatusFields on CrowdyStudioGitHubStatus {
  configured
  connected
  accountLogin
  accountType
  owner
  repo
  branch
  githubSha
  installUrl
})gql";
inline constexpr std::string_view kCrowdyStudioGitHubRefreshOperationName = "CrowdyStudioGitHubRefresh";
inline constexpr std::string_view kCrowdyStudioGitHubLayoutIsolatedDocument = R"gql(query CrowdyStudioGitHubLayout($input: CrowdyStudioGitHubAtCommitInput!) {
  crowdyStudioGitHubLayout(input: $input) {
    commitSha
    server
    client
    assets
    fromFile
  }
})gql";
inline constexpr std::string_view kCrowdyStudioGitHubLayoutOperationName = "CrowdyStudioGitHubLayout";
inline constexpr std::string_view kCrowdyStudioGitHubTreeIsolatedDocument = R"gql(query CrowdyStudioGitHubTree($input: CrowdyStudioGitHubAtCommitInput!) {
  crowdyStudioGitHubTree(input: $input) {
    commitSha
    entries {
      path
      type
      sha
      size
    }
  }
})gql";
inline constexpr std::string_view kCrowdyStudioGitHubTreeOperationName = "CrowdyStudioGitHubTree";
inline constexpr std::string_view kCrowdyStudioGitHubFileIsolatedDocument = R"gql(query CrowdyStudioGitHubFile($input: CrowdyStudioGitHubFileInput!) {
  crowdyStudioGitHubFile(input: $input) {
    path
    content
    sha
    commitSha
  }
})gql";
inline constexpr std::string_view kCrowdyStudioGitHubFileOperationName = "CrowdyStudioGitHubFile";
inline constexpr std::string_view kCrowdyStudioGitHubPutFileIsolatedDocument = R"gql(mutation CrowdyStudioGitHubPutFile($input: CrowdyStudioGitHubPutFileInput!) {
  crowdyStudioGitHubPutFile(input: $input) {
    path
    content
    sha
    commitSha
  }
})gql";
inline constexpr std::string_view kCrowdyStudioGitHubPutFileOperationName = "CrowdyStudioGitHubPutFile";
inline constexpr std::string_view kCrowdyStudioGitHubDeleteFileIsolatedDocument = R"gql(mutation CrowdyStudioGitHubDeleteFile($input: CrowdyStudioGitHubDeleteFileInput!) {
  crowdyStudioGitHubDeleteFile(input: $input) {
    ...CrowdyStudioGitHubStatusFields
  }
}

fragment CrowdyStudioGitHubStatusFields on CrowdyStudioGitHubStatus {
  configured
  connected
  accountLogin
  accountType
  owner
  repo
  branch
  githubSha
  installUrl
})gql";
inline constexpr std::string_view kCrowdyStudioGitHubDeleteFileOperationName = "CrowdyStudioGitHubDeleteFile";

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
  if (operationName == "CrowdyStudioGitHubStatus") return kCrowdyStudioGitHubStatusIsolatedDocument;
  if (operationName == "CrowdyStudioGitHubConnectUrl") return kCrowdyStudioGitHubConnectUrlIsolatedDocument;
  if (operationName == "CrowdyStudioGitHubRepos") return kCrowdyStudioGitHubReposIsolatedDocument;
  if (operationName == "CrowdyStudioGitHubBind") return kCrowdyStudioGitHubBindIsolatedDocument;
  if (operationName == "CrowdyStudioGitHubUnbind") return kCrowdyStudioGitHubUnbindIsolatedDocument;
  if (operationName == "CrowdyStudioGitHubRefresh") return kCrowdyStudioGitHubRefreshIsolatedDocument;
  if (operationName == "CrowdyStudioGitHubLayout") return kCrowdyStudioGitHubLayoutIsolatedDocument;
  if (operationName == "CrowdyStudioGitHubTree") return kCrowdyStudioGitHubTreeIsolatedDocument;
  if (operationName == "CrowdyStudioGitHubFile") return kCrowdyStudioGitHubFileIsolatedDocument;
  if (operationName == "CrowdyStudioGitHubPutFile") return kCrowdyStudioGitHubPutFileIsolatedDocument;
  if (operationName == "CrowdyStudioGitHubDeleteFile") return kCrowdyStudioGitHubDeleteFileIsolatedDocument;
  return {};
}

}  // namespace crowdyStudio

namespace crowdyStudioAgent {

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

/// crowdyStudioAgent/CrowdyStudioModel.graphql
inline constexpr std::string_view kCrowdyStudioModelDocument = R"gql(# The metered model endpoint's GraphQL companions. The endpoint itself is
# REST (`GET /v1/model/models`, `POST /v1/model/chat/completions`, bearer =
# app token) and is what the in-browser Studio agent spends tokens through;
# these operations read what it recorded and gate whether it may run.

query CrowdyStudioProviderConsent($appId: BigInt!) {
  crowdyStudioProviderConsent(appId: $appId) {
    appId
    consented
    consentedAt
  }
}

mutation CrowdyStudioSetProviderConsent($input: SetCrowdyStudioProviderConsentInput!) {
  crowdyStudioSetProviderConsent(input: $input) {
    appId
    consented
    consentedAt
  }
}

query CrowdyStudioModelUsage($appId: BigInt!, $limit: Int) {
  crowdyStudioModelUsage(appId: $appId, limit: $limit) {
    appId
    payerKind
    todayRequests
    todayChargeMicrousd
    dayLimitMicrousd
    recent {
      usageId
      payerKind
      status
      requestedModel
      resolvedModel
      promptTokens
      completionTokens
      reasoningTokens
      chargeMicrousd
      occurredAt
      client
    }
  }
})gql";
inline constexpr std::string_view kCrowdyStudioProviderConsentIsolatedDocument = R"gql(query CrowdyStudioProviderConsent($appId: BigInt!) {
  crowdyStudioProviderConsent(appId: $appId) {
    appId
    consented
    consentedAt
  }
})gql";
inline constexpr std::string_view kCrowdyStudioProviderConsentOperationName = "CrowdyStudioProviderConsent";
inline constexpr std::string_view kCrowdyStudioSetProviderConsentIsolatedDocument = R"gql(mutation CrowdyStudioSetProviderConsent($input: SetCrowdyStudioProviderConsentInput!) {
  crowdyStudioSetProviderConsent(input: $input) {
    appId
    consented
    consentedAt
  }
})gql";
inline constexpr std::string_view kCrowdyStudioSetProviderConsentOperationName = "CrowdyStudioSetProviderConsent";
inline constexpr std::string_view kCrowdyStudioModelUsageIsolatedDocument = R"gql(query CrowdyStudioModelUsage($appId: BigInt!, $limit: Int) {
  crowdyStudioModelUsage(appId: $appId, limit: $limit) {
    appId
    payerKind
    todayRequests
    todayChargeMicrousd
    dayLimitMicrousd
    recent {
      usageId
      payerKind
      status
      requestedModel
      resolvedModel
      promptTokens
      completionTokens
      reasoningTokens
      chargeMicrousd
      occurredAt
      client
    }
  }
})gql";
inline constexpr std::string_view kCrowdyStudioModelUsageOperationName = "CrowdyStudioModelUsage";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "CrowdyStudioAgentPolicy") return kCrowdyStudioAgentPolicyIsolatedDocument;
  if (operationName == "CrowdyStudioAgentEffectivePolicy") return kCrowdyStudioAgentEffectivePolicyIsolatedDocument;
  if (operationName == "CrowdyStudioAgentUsage") return kCrowdyStudioAgentUsageIsolatedDocument;
  if (operationName == "CrowdyStudioAgentSetPolicy") return kCrowdyStudioAgentSetPolicyIsolatedDocument;
  if (operationName == "CrowdyStudioProviderConsent") return kCrowdyStudioProviderConsentIsolatedDocument;
  if (operationName == "CrowdyStudioSetProviderConsent") return kCrowdyStudioSetProviderConsentIsolatedDocument;
  if (operationName == "CrowdyStudioModelUsage") return kCrowdyStudioModelUsageIsolatedDocument;
  return {};
}

}  // namespace crowdyStudioAgent

namespace exec {

/// exec/Exec.graphql
inline constexpr std::string_view kExecDocument = R"gql(mutation ExecConnect($appId: BigInt!, $nodeType: String, $key: String) {
  execConnect(appId: $appId, nodeType: $nodeType, key: $key) {
    gatewayUrl
    token
    host
    expiresAt
  }
}

mutation ExecDeploy($input: ExecDeployInput!) {
  execDeploy(input: $input) {
    version
  }
}

mutation ExecConnectAsDeveloper($appId: BigInt!, $nodeType: String, $key: String) {
  execConnectAsDeveloper(appId: $appId, nodeType: $nodeType, key: $key) {
    gatewayUrl
    token
    host
    expiresAt
  }
}

query ExecLogs(
  $appId: BigInt!
  $nodeType: String
  $key: String
  $maxLevel: Int
  $before: String
  $limit: Int
  $flow: String
) {
  execLogs(
    appId: $appId
    nodeType: $nodeType
    key: $key
    maxLevel: $maxLevel
    before: $before
    limit: $limit
    flow: $flow
  ) {
    id
    nodeType
    key
    level
    host
    at
    text
    flow
  }
}

query ExecInstances($appId: BigInt!) {
  execInstances(appId: $appId) {
    instanceId
    nodeType
    key
    kind
    phase
    host
    epoch
    sinceMs
    heldBack
  }
}

query ExecVersions($appId: BigInt!) {
  execVersions(appId: $appId) {
    version
    createdBy
    createdAt
    types
    active
    manifestJson
  }
}

query ExecEndpointStats($appId: BigInt!, $nodeType: String, $sinceMinutes: Int) {
  execEndpointStats(appId: $appId, nodeType: $nodeType, sinceMinutes: $sinceMinutes) {
    nodeType
    method
    calls
    appErrors
    busy
    denied
    deadlineExceeded
    otherErrors
    timedCalls
    latencyMsAvg
    latencyMsMax
    firstMinute
    lastMinute
  }
}

fragment ExecAppStatusFields on ExecAppStatus {
  activeVersion
  disabled
  disabledTypes
  budgetPaused
  budgetPauseReason
  maxInstances
  maxReservedMb
  instanceLimit
  instances
  reservedMb
}

query ExecAppStatus($appId: BigInt!) {
  execAppStatus(appId: $appId) {
    ...ExecAppStatusFields
  }
}

mutation ExecActivateVersion($appId: BigInt!, $version: Int!) {
  execActivateVersion(appId: $appId, version: $version) {
    ...ExecAppStatusFields
  }
}

mutation ExecSetEnabled($appId: BigInt!, $enabled: Boolean!, $nodeType: String) {
  execSetEnabled(appId: $appId, enabled: $enabled, nodeType: $nodeType) {
    ...ExecAppStatusFields
  }
}

mutation ExecRestartType($appId: BigInt!, $nodeType: String!) {
  execRestartType(appId: $appId, nodeType: $nodeType) {
    nodeType
    stopped
  }
}

query ExecStarters($appId: BigInt!) {
  execStarters(appId: $appId) {
    manifestJson
    starters {
      crate
      nodeType
      description
      files {
        path
        content
      }
    }
  }
}

fragment ExecBuildFields on ExecBuild {
  buildId
  status
  kind
  log
  sdkVersion
  createdAt
  startedAt
  finishedAt
  artifacts {
    crate
    digest
    sizeBytes
    capabilitySummaryJson
    capabilityHash
    tickIntervalMs
  }
}

mutation ExecBuild($input: ExecBuildInput!) {
  execBuild(input: $input) {
    ...ExecBuildFields
  }
}

query ExecBuildStatus($appId: BigInt!, $buildId: String!) {
  execBuildStatus(appId: $appId, buildId: $buildId) {
    ...ExecBuildFields
  }
}

fragment ExecModFields on ExecMod {
  modId
  gridId
  name
  ownerId
  version
  digest
  enabled
  listingId
  blocked
  running
  updatedAt
}

fragment ExecModListingFields on ExecModListing {
  listingId
  title
  description
  publisherId
  sourceModId
  sourceVersion
  digest
  installs
  clientDigest
  clientCapabilitySummaryJson
  clientCapabilityHash
  clientTickIntervalMs
  createdAt
  delistedAt
}

fragment ExecModSwitchFields on ExecModSwitch {
  scope
  target
  reason
  createdBy
  createdAt
}

query ExecModStarter($appId: BigInt!) {
  execModStarter(appId: $appId) {
    crate
    nodeType
    description
    files {
      path
      content
    }
  }
}

mutation ExecModBuild($appId: BigInt!, $crate: ExecBuildCrateInput!) {
  execModBuild(appId: $appId, crate: $crate) {
    ...ExecBuildFields
  }
}

query ExecModBuildStatus($appId: BigInt!, $buildId: String!) {
  execModBuildStatus(appId: $appId, buildId: $buildId) {
    ...ExecBuildFields
  }
}

mutation ExecModDeploy(
  $appId: BigInt!
  $gridId: BigInt!
  $name: String!
  $buildId: String!
) {
  execModDeploy(appId: $appId, gridId: $gridId, name: $name, buildId: $buildId) {
    ...ExecModFields
  }
}

mutation ExecModSetEnabled(
  $appId: BigInt!
  $gridId: BigInt!
  $name: String!
  $enabled: Boolean!
) {
  execModSetEnabled(appId: $appId, gridId: $gridId, name: $name, enabled: $enabled) {
    ...ExecModFields
  }
}

mutation ExecModDelete($appId: BigInt!, $gridId: BigInt!, $name: String!) {
  execModDelete(appId: $appId, gridId: $gridId, name: $name)
}

query ExecMods($appId: BigInt!, $gridId: BigInt!) {
  execMods(appId: $appId, gridId: $gridId) {
    ...ExecModFields
  }
}

query ExecMyMods($appId: BigInt!) {
  execMyMods(appId: $appId) {
    ...ExecModFields
  }
}

query ExecModLogs(
  $appId: BigInt!
  $gridId: BigInt!
  $name: String!
  $maxLevel: Int
  $before: String
  $limit: Int
) {
  execModLogs(
    appId: $appId
    gridId: $gridId
    name: $name
    maxLevel: $maxLevel
    before: $before
    limit: $limit
  ) {
    id
    nodeType
    key
    level
    host
    at
    text
    flow
  }
}

mutation ExecModPublish(
  $appId: BigInt!
  $gridId: BigInt!
  $name: String!
  $title: String!
  $description: String
) {
  execModPublish(
    appId: $appId
    gridId: $gridId
    name: $name
    title: $title
    description: $description
  ) {
    ...ExecModListingFields
  }
}

query ExecModListings($appId: BigInt!) {
  execModListings(appId: $appId) {
    ...ExecModListingFields
  }
}

mutation ExecModUnpublish($appId: BigInt!, $listingId: BigInt!) {
  execModUnpublish(appId: $appId, listingId: $listingId)
}

mutation ExecModInstall(
  $appId: BigInt!
  $gridId: BigInt!
  $name: String!
  $listingId: BigInt!
) {
  execModInstall(appId: $appId, gridId: $gridId, name: $name, listingId: $listingId) {
    ...ExecModFields
  }
}

query ExecAppMods($appId: BigInt!, $gridId: BigInt, $ownerId: BigInt) {
  execAppMods(appId: $appId, gridId: $gridId, ownerId: $ownerId) {
    ...ExecModFields
  }
}

query ExecModSwitches($appId: BigInt!) {
  execModSwitches(appId: $appId) {
    ...ExecModSwitchFields
  }
}

mutation ExecModSetSwitch(
  $appId: BigInt!
  $scope: ExecModScope!
  $off: Boolean!
  $target: String
  $reason: String
) {
  execModSetSwitch(appId: $appId, scope: $scope, off: $off, target: $target, reason: $reason) {
    ...ExecModSwitchFields
  }
}

fragment ExecModClientFields on ExecModClient {
  modId
  gridId
  name
  ownerId
  clientVersion
  digest
  sizeBytes
  capabilitySummaryJson
  capabilityHash
  tickIntervalMs
  updatedAt
}

fragment ExecGridClientModFields on ExecGridClientMod {
  modId
  name
  gridId
  authorId
  listingId
  clientVersion
  digest
  capabilitySummaryJson
  capabilityHash
  tickIntervalMs
  callerConsented
  authorCapabilitySummaryJson
  authorCapabilityHash
  callerTrustsAuthor
  updatedAt
}

mutation ExecModClientBuild($appId: BigInt!, $crate: ExecBuildCrateInput!) {
  execModClientBuild(appId: $appId, crate: $crate) {
    ...ExecBuildFields
  }
}

mutation ExecModClientDeploy(
  $appId: BigInt!
  $gridId: BigInt!
  $name: String!
  $buildId: String!
) {
  execModClientDeploy(appId: $appId, gridId: $gridId, name: $name, buildId: $buildId) {
    ...ExecModClientFields
  }
}

mutation ExecModClientDelete($appId: BigInt!, $gridId: BigInt!, $name: String!) {
  execModClientDelete(appId: $appId, gridId: $gridId, name: $name)
}

query ExecGridClientMods($appId: BigInt!, $gridId: BigInt!) {
  execGridClientMods(appId: $appId, gridId: $gridId) {
    ...ExecGridClientModFields
  }
}

mutation ExecConsentClientMod($appId: BigInt!, $modId: String!, $capabilityHash: String!) {
  execConsentClientMod(appId: $appId, modId: $modId, capabilityHash: $capabilityHash)
}

mutation ExecTrustAuthor(
  $appId: BigInt!
  $gridId: BigInt!
  $authorId: BigInt!
  $capabilityHash: String!
) {
  execTrustAuthor(
    appId: $appId
    gridId: $gridId
    authorId: $authorId
    capabilityHash: $capabilityHash
  )
}

mutation ExecRevokeClientModConsent($appId: BigInt!, $modId: String!) {
  execRevokeClientModConsent(appId: $appId, modId: $modId)
}

mutation ExecRevokeAuthorTrust($appId: BigInt!, $gridId: BigInt!, $authorId: BigInt!) {
  execRevokeAuthorTrust(appId: $appId, gridId: $gridId, authorId: $authorId)
}

query ExecModClientArtifact($appId: BigInt!, $modId: String!) {
  execModClientArtifact(appId: $appId, modId: $modId) {
    modId
    name
    gridId
    clientVersion
    digest
    wasmBase64
    sizeBytes
    capabilitySummaryJson
    capabilityHash
    tickIntervalMs
    fuelPerDispatch
    abiVersion
  }
})gql";
inline constexpr std::string_view kExecConnectIsolatedDocument = R"gql(mutation ExecConnect($appId: BigInt!, $nodeType: String, $key: String) {
  execConnect(appId: $appId, nodeType: $nodeType, key: $key) {
    gatewayUrl
    token
    host
    expiresAt
  }
})gql";
inline constexpr std::string_view kExecConnectOperationName = "ExecConnect";
inline constexpr std::string_view kExecDeployIsolatedDocument = R"gql(mutation ExecDeploy($input: ExecDeployInput!) {
  execDeploy(input: $input) {
    version
  }
})gql";
inline constexpr std::string_view kExecDeployOperationName = "ExecDeploy";
inline constexpr std::string_view kExecConnectAsDeveloperIsolatedDocument = R"gql(mutation ExecConnectAsDeveloper($appId: BigInt!, $nodeType: String, $key: String) {
  execConnectAsDeveloper(appId: $appId, nodeType: $nodeType, key: $key) {
    gatewayUrl
    token
    host
    expiresAt
  }
})gql";
inline constexpr std::string_view kExecConnectAsDeveloperOperationName = "ExecConnectAsDeveloper";
inline constexpr std::string_view kExecLogsIsolatedDocument = R"gql(query ExecLogs($appId: BigInt!, $nodeType: String, $key: String, $maxLevel: Int, $before: String, $limit: Int, $flow: String) {
  execLogs(
    appId: $appId
    nodeType: $nodeType
    key: $key
    maxLevel: $maxLevel
    before: $before
    limit: $limit
    flow: $flow
  ) {
    id
    nodeType
    key
    level
    host
    at
    text
    flow
  }
})gql";
inline constexpr std::string_view kExecLogsOperationName = "ExecLogs";
inline constexpr std::string_view kExecInstancesIsolatedDocument = R"gql(query ExecInstances($appId: BigInt!) {
  execInstances(appId: $appId) {
    instanceId
    nodeType
    key
    kind
    phase
    host
    epoch
    sinceMs
    heldBack
  }
})gql";
inline constexpr std::string_view kExecInstancesOperationName = "ExecInstances";
inline constexpr std::string_view kExecVersionsIsolatedDocument = R"gql(query ExecVersions($appId: BigInt!) {
  execVersions(appId: $appId) {
    version
    createdBy
    createdAt
    types
    active
    manifestJson
  }
})gql";
inline constexpr std::string_view kExecVersionsOperationName = "ExecVersions";
inline constexpr std::string_view kExecEndpointStatsIsolatedDocument = R"gql(query ExecEndpointStats($appId: BigInt!, $nodeType: String, $sinceMinutes: Int) {
  execEndpointStats(
    appId: $appId
    nodeType: $nodeType
    sinceMinutes: $sinceMinutes
  ) {
    nodeType
    method
    calls
    appErrors
    busy
    denied
    deadlineExceeded
    otherErrors
    timedCalls
    latencyMsAvg
    latencyMsMax
    firstMinute
    lastMinute
  }
})gql";
inline constexpr std::string_view kExecEndpointStatsOperationName = "ExecEndpointStats";
inline constexpr std::string_view kExecAppStatusIsolatedDocument = R"gql(query ExecAppStatus($appId: BigInt!) {
  execAppStatus(appId: $appId) {
    ...ExecAppStatusFields
  }
}

fragment ExecAppStatusFields on ExecAppStatus {
  activeVersion
  disabled
  disabledTypes
  budgetPaused
  budgetPauseReason
  maxInstances
  maxReservedMb
  instanceLimit
  instances
  reservedMb
})gql";
inline constexpr std::string_view kExecAppStatusOperationName = "ExecAppStatus";
inline constexpr std::string_view kExecActivateVersionIsolatedDocument = R"gql(mutation ExecActivateVersion($appId: BigInt!, $version: Int!) {
  execActivateVersion(appId: $appId, version: $version) {
    ...ExecAppStatusFields
  }
}

fragment ExecAppStatusFields on ExecAppStatus {
  activeVersion
  disabled
  disabledTypes
  budgetPaused
  budgetPauseReason
  maxInstances
  maxReservedMb
  instanceLimit
  instances
  reservedMb
})gql";
inline constexpr std::string_view kExecActivateVersionOperationName = "ExecActivateVersion";
inline constexpr std::string_view kExecSetEnabledIsolatedDocument = R"gql(mutation ExecSetEnabled($appId: BigInt!, $enabled: Boolean!, $nodeType: String) {
  execSetEnabled(appId: $appId, enabled: $enabled, nodeType: $nodeType) {
    ...ExecAppStatusFields
  }
}

fragment ExecAppStatusFields on ExecAppStatus {
  activeVersion
  disabled
  disabledTypes
  budgetPaused
  budgetPauseReason
  maxInstances
  maxReservedMb
  instanceLimit
  instances
  reservedMb
})gql";
inline constexpr std::string_view kExecSetEnabledOperationName = "ExecSetEnabled";
inline constexpr std::string_view kExecRestartTypeIsolatedDocument = R"gql(mutation ExecRestartType($appId: BigInt!, $nodeType: String!) {
  execRestartType(appId: $appId, nodeType: $nodeType) {
    nodeType
    stopped
  }
})gql";
inline constexpr std::string_view kExecRestartTypeOperationName = "ExecRestartType";
inline constexpr std::string_view kExecStartersIsolatedDocument = R"gql(query ExecStarters($appId: BigInt!) {
  execStarters(appId: $appId) {
    manifestJson
    starters {
      crate
      nodeType
      description
      files {
        path
        content
      }
    }
  }
})gql";
inline constexpr std::string_view kExecStartersOperationName = "ExecStarters";
inline constexpr std::string_view kExecBuildIsolatedDocument = R"gql(mutation ExecBuild($input: ExecBuildInput!) {
  execBuild(input: $input) {
    ...ExecBuildFields
  }
}

fragment ExecBuildFields on ExecBuild {
  buildId
  status
  kind
  log
  sdkVersion
  createdAt
  startedAt
  finishedAt
  artifacts {
    crate
    digest
    sizeBytes
    capabilitySummaryJson
    capabilityHash
    tickIntervalMs
  }
})gql";
inline constexpr std::string_view kExecBuildOperationName = "ExecBuild";
inline constexpr std::string_view kExecBuildStatusIsolatedDocument = R"gql(query ExecBuildStatus($appId: BigInt!, $buildId: String!) {
  execBuildStatus(appId: $appId, buildId: $buildId) {
    ...ExecBuildFields
  }
}

fragment ExecBuildFields on ExecBuild {
  buildId
  status
  kind
  log
  sdkVersion
  createdAt
  startedAt
  finishedAt
  artifacts {
    crate
    digest
    sizeBytes
    capabilitySummaryJson
    capabilityHash
    tickIntervalMs
  }
})gql";
inline constexpr std::string_view kExecBuildStatusOperationName = "ExecBuildStatus";
inline constexpr std::string_view kExecModStarterIsolatedDocument = R"gql(query ExecModStarter($appId: BigInt!) {
  execModStarter(appId: $appId) {
    crate
    nodeType
    description
    files {
      path
      content
    }
  }
})gql";
inline constexpr std::string_view kExecModStarterOperationName = "ExecModStarter";
inline constexpr std::string_view kExecModBuildIsolatedDocument = R"gql(mutation ExecModBuild($appId: BigInt!, $crate: ExecBuildCrateInput!) {
  execModBuild(appId: $appId, crate: $crate) {
    ...ExecBuildFields
  }
}

fragment ExecBuildFields on ExecBuild {
  buildId
  status
  kind
  log
  sdkVersion
  createdAt
  startedAt
  finishedAt
  artifacts {
    crate
    digest
    sizeBytes
    capabilitySummaryJson
    capabilityHash
    tickIntervalMs
  }
})gql";
inline constexpr std::string_view kExecModBuildOperationName = "ExecModBuild";
inline constexpr std::string_view kExecModBuildStatusIsolatedDocument = R"gql(query ExecModBuildStatus($appId: BigInt!, $buildId: String!) {
  execModBuildStatus(appId: $appId, buildId: $buildId) {
    ...ExecBuildFields
  }
}

fragment ExecBuildFields on ExecBuild {
  buildId
  status
  kind
  log
  sdkVersion
  createdAt
  startedAt
  finishedAt
  artifacts {
    crate
    digest
    sizeBytes
    capabilitySummaryJson
    capabilityHash
    tickIntervalMs
  }
})gql";
inline constexpr std::string_view kExecModBuildStatusOperationName = "ExecModBuildStatus";
inline constexpr std::string_view kExecModDeployIsolatedDocument = R"gql(mutation ExecModDeploy($appId: BigInt!, $gridId: BigInt!, $name: String!, $buildId: String!) {
  execModDeploy(appId: $appId, gridId: $gridId, name: $name, buildId: $buildId) {
    ...ExecModFields
  }
}

fragment ExecModFields on ExecMod {
  modId
  gridId
  name
  ownerId
  version
  digest
  enabled
  listingId
  blocked
  running
  updatedAt
})gql";
inline constexpr std::string_view kExecModDeployOperationName = "ExecModDeploy";
inline constexpr std::string_view kExecModSetEnabledIsolatedDocument = R"gql(mutation ExecModSetEnabled($appId: BigInt!, $gridId: BigInt!, $name: String!, $enabled: Boolean!) {
  execModSetEnabled(
    appId: $appId
    gridId: $gridId
    name: $name
    enabled: $enabled
  ) {
    ...ExecModFields
  }
}

fragment ExecModFields on ExecMod {
  modId
  gridId
  name
  ownerId
  version
  digest
  enabled
  listingId
  blocked
  running
  updatedAt
})gql";
inline constexpr std::string_view kExecModSetEnabledOperationName = "ExecModSetEnabled";
inline constexpr std::string_view kExecModDeleteIsolatedDocument = R"gql(mutation ExecModDelete($appId: BigInt!, $gridId: BigInt!, $name: String!) {
  execModDelete(appId: $appId, gridId: $gridId, name: $name)
})gql";
inline constexpr std::string_view kExecModDeleteOperationName = "ExecModDelete";
inline constexpr std::string_view kExecModsIsolatedDocument = R"gql(query ExecMods($appId: BigInt!, $gridId: BigInt!) {
  execMods(appId: $appId, gridId: $gridId) {
    ...ExecModFields
  }
}

fragment ExecModFields on ExecMod {
  modId
  gridId
  name
  ownerId
  version
  digest
  enabled
  listingId
  blocked
  running
  updatedAt
})gql";
inline constexpr std::string_view kExecModsOperationName = "ExecMods";
inline constexpr std::string_view kExecMyModsIsolatedDocument = R"gql(query ExecMyMods($appId: BigInt!) {
  execMyMods(appId: $appId) {
    ...ExecModFields
  }
}

fragment ExecModFields on ExecMod {
  modId
  gridId
  name
  ownerId
  version
  digest
  enabled
  listingId
  blocked
  running
  updatedAt
})gql";
inline constexpr std::string_view kExecMyModsOperationName = "ExecMyMods";
inline constexpr std::string_view kExecModLogsIsolatedDocument = R"gql(query ExecModLogs($appId: BigInt!, $gridId: BigInt!, $name: String!, $maxLevel: Int, $before: String, $limit: Int) {
  execModLogs(
    appId: $appId
    gridId: $gridId
    name: $name
    maxLevel: $maxLevel
    before: $before
    limit: $limit
  ) {
    id
    nodeType
    key
    level
    host
    at
    text
    flow
  }
})gql";
inline constexpr std::string_view kExecModLogsOperationName = "ExecModLogs";
inline constexpr std::string_view kExecModPublishIsolatedDocument = R"gql(mutation ExecModPublish($appId: BigInt!, $gridId: BigInt!, $name: String!, $title: String!, $description: String) {
  execModPublish(
    appId: $appId
    gridId: $gridId
    name: $name
    title: $title
    description: $description
  ) {
    ...ExecModListingFields
  }
}

fragment ExecModListingFields on ExecModListing {
  listingId
  title
  description
  publisherId
  sourceModId
  sourceVersion
  digest
  installs
  clientDigest
  clientCapabilitySummaryJson
  clientCapabilityHash
  clientTickIntervalMs
  createdAt
  delistedAt
})gql";
inline constexpr std::string_view kExecModPublishOperationName = "ExecModPublish";
inline constexpr std::string_view kExecModListingsIsolatedDocument = R"gql(query ExecModListings($appId: BigInt!) {
  execModListings(appId: $appId) {
    ...ExecModListingFields
  }
}

fragment ExecModListingFields on ExecModListing {
  listingId
  title
  description
  publisherId
  sourceModId
  sourceVersion
  digest
  installs
  clientDigest
  clientCapabilitySummaryJson
  clientCapabilityHash
  clientTickIntervalMs
  createdAt
  delistedAt
})gql";
inline constexpr std::string_view kExecModListingsOperationName = "ExecModListings";
inline constexpr std::string_view kExecModUnpublishIsolatedDocument = R"gql(mutation ExecModUnpublish($appId: BigInt!, $listingId: BigInt!) {
  execModUnpublish(appId: $appId, listingId: $listingId)
})gql";
inline constexpr std::string_view kExecModUnpublishOperationName = "ExecModUnpublish";
inline constexpr std::string_view kExecModInstallIsolatedDocument = R"gql(mutation ExecModInstall($appId: BigInt!, $gridId: BigInt!, $name: String!, $listingId: BigInt!) {
  execModInstall(
    appId: $appId
    gridId: $gridId
    name: $name
    listingId: $listingId
  ) {
    ...ExecModFields
  }
}

fragment ExecModFields on ExecMod {
  modId
  gridId
  name
  ownerId
  version
  digest
  enabled
  listingId
  blocked
  running
  updatedAt
})gql";
inline constexpr std::string_view kExecModInstallOperationName = "ExecModInstall";
inline constexpr std::string_view kExecAppModsIsolatedDocument = R"gql(query ExecAppMods($appId: BigInt!, $gridId: BigInt, $ownerId: BigInt) {
  execAppMods(appId: $appId, gridId: $gridId, ownerId: $ownerId) {
    ...ExecModFields
  }
}

fragment ExecModFields on ExecMod {
  modId
  gridId
  name
  ownerId
  version
  digest
  enabled
  listingId
  blocked
  running
  updatedAt
})gql";
inline constexpr std::string_view kExecAppModsOperationName = "ExecAppMods";
inline constexpr std::string_view kExecModSwitchesIsolatedDocument = R"gql(query ExecModSwitches($appId: BigInt!) {
  execModSwitches(appId: $appId) {
    ...ExecModSwitchFields
  }
}

fragment ExecModSwitchFields on ExecModSwitch {
  scope
  target
  reason
  createdBy
  createdAt
})gql";
inline constexpr std::string_view kExecModSwitchesOperationName = "ExecModSwitches";
inline constexpr std::string_view kExecModSetSwitchIsolatedDocument = R"gql(mutation ExecModSetSwitch($appId: BigInt!, $scope: ExecModScope!, $off: Boolean!, $target: String, $reason: String) {
  execModSetSwitch(
    appId: $appId
    scope: $scope
    off: $off
    target: $target
    reason: $reason
  ) {
    ...ExecModSwitchFields
  }
}

fragment ExecModSwitchFields on ExecModSwitch {
  scope
  target
  reason
  createdBy
  createdAt
})gql";
inline constexpr std::string_view kExecModSetSwitchOperationName = "ExecModSetSwitch";
inline constexpr std::string_view kExecModClientBuildIsolatedDocument = R"gql(mutation ExecModClientBuild($appId: BigInt!, $crate: ExecBuildCrateInput!) {
  execModClientBuild(appId: $appId, crate: $crate) {
    ...ExecBuildFields
  }
}

fragment ExecBuildFields on ExecBuild {
  buildId
  status
  kind
  log
  sdkVersion
  createdAt
  startedAt
  finishedAt
  artifacts {
    crate
    digest
    sizeBytes
    capabilitySummaryJson
    capabilityHash
    tickIntervalMs
  }
})gql";
inline constexpr std::string_view kExecModClientBuildOperationName = "ExecModClientBuild";
inline constexpr std::string_view kExecModClientDeployIsolatedDocument = R"gql(mutation ExecModClientDeploy($appId: BigInt!, $gridId: BigInt!, $name: String!, $buildId: String!) {
  execModClientDeploy(
    appId: $appId
    gridId: $gridId
    name: $name
    buildId: $buildId
  ) {
    ...ExecModClientFields
  }
}

fragment ExecModClientFields on ExecModClient {
  modId
  gridId
  name
  ownerId
  clientVersion
  digest
  sizeBytes
  capabilitySummaryJson
  capabilityHash
  tickIntervalMs
  updatedAt
})gql";
inline constexpr std::string_view kExecModClientDeployOperationName = "ExecModClientDeploy";
inline constexpr std::string_view kExecModClientDeleteIsolatedDocument = R"gql(mutation ExecModClientDelete($appId: BigInt!, $gridId: BigInt!, $name: String!) {
  execModClientDelete(appId: $appId, gridId: $gridId, name: $name)
})gql";
inline constexpr std::string_view kExecModClientDeleteOperationName = "ExecModClientDelete";
inline constexpr std::string_view kExecGridClientModsIsolatedDocument = R"gql(query ExecGridClientMods($appId: BigInt!, $gridId: BigInt!) {
  execGridClientMods(appId: $appId, gridId: $gridId) {
    ...ExecGridClientModFields
  }
}

fragment ExecGridClientModFields on ExecGridClientMod {
  modId
  name
  gridId
  authorId
  listingId
  clientVersion
  digest
  capabilitySummaryJson
  capabilityHash
  tickIntervalMs
  callerConsented
  authorCapabilitySummaryJson
  authorCapabilityHash
  callerTrustsAuthor
  updatedAt
})gql";
inline constexpr std::string_view kExecGridClientModsOperationName = "ExecGridClientMods";
inline constexpr std::string_view kExecConsentClientModIsolatedDocument = R"gql(mutation ExecConsentClientMod($appId: BigInt!, $modId: String!, $capabilityHash: String!) {
  execConsentClientMod(
    appId: $appId
    modId: $modId
    capabilityHash: $capabilityHash
  )
})gql";
inline constexpr std::string_view kExecConsentClientModOperationName = "ExecConsentClientMod";
inline constexpr std::string_view kExecTrustAuthorIsolatedDocument = R"gql(mutation ExecTrustAuthor($appId: BigInt!, $gridId: BigInt!, $authorId: BigInt!, $capabilityHash: String!) {
  execTrustAuthor(
    appId: $appId
    gridId: $gridId
    authorId: $authorId
    capabilityHash: $capabilityHash
  )
})gql";
inline constexpr std::string_view kExecTrustAuthorOperationName = "ExecTrustAuthor";
inline constexpr std::string_view kExecRevokeClientModConsentIsolatedDocument = R"gql(mutation ExecRevokeClientModConsent($appId: BigInt!, $modId: String!) {
  execRevokeClientModConsent(appId: $appId, modId: $modId)
})gql";
inline constexpr std::string_view kExecRevokeClientModConsentOperationName = "ExecRevokeClientModConsent";
inline constexpr std::string_view kExecRevokeAuthorTrustIsolatedDocument = R"gql(mutation ExecRevokeAuthorTrust($appId: BigInt!, $gridId: BigInt!, $authorId: BigInt!) {
  execRevokeAuthorTrust(appId: $appId, gridId: $gridId, authorId: $authorId)
})gql";
inline constexpr std::string_view kExecRevokeAuthorTrustOperationName = "ExecRevokeAuthorTrust";
inline constexpr std::string_view kExecModClientArtifactIsolatedDocument = R"gql(query ExecModClientArtifact($appId: BigInt!, $modId: String!) {
  execModClientArtifact(appId: $appId, modId: $modId) {
    modId
    name
    gridId
    clientVersion
    digest
    wasmBase64
    sizeBytes
    capabilitySummaryJson
    capabilityHash
    tickIntervalMs
    fuelPerDispatch
    abiVersion
  }
})gql";
inline constexpr std::string_view kExecModClientArtifactOperationName = "ExecModClientArtifact";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "ExecConnect") return kExecConnectIsolatedDocument;
  if (operationName == "ExecDeploy") return kExecDeployIsolatedDocument;
  if (operationName == "ExecConnectAsDeveloper") return kExecConnectAsDeveloperIsolatedDocument;
  if (operationName == "ExecLogs") return kExecLogsIsolatedDocument;
  if (operationName == "ExecInstances") return kExecInstancesIsolatedDocument;
  if (operationName == "ExecVersions") return kExecVersionsIsolatedDocument;
  if (operationName == "ExecEndpointStats") return kExecEndpointStatsIsolatedDocument;
  if (operationName == "ExecAppStatus") return kExecAppStatusIsolatedDocument;
  if (operationName == "ExecActivateVersion") return kExecActivateVersionIsolatedDocument;
  if (operationName == "ExecSetEnabled") return kExecSetEnabledIsolatedDocument;
  if (operationName == "ExecRestartType") return kExecRestartTypeIsolatedDocument;
  if (operationName == "ExecStarters") return kExecStartersIsolatedDocument;
  if (operationName == "ExecBuild") return kExecBuildIsolatedDocument;
  if (operationName == "ExecBuildStatus") return kExecBuildStatusIsolatedDocument;
  if (operationName == "ExecModStarter") return kExecModStarterIsolatedDocument;
  if (operationName == "ExecModBuild") return kExecModBuildIsolatedDocument;
  if (operationName == "ExecModBuildStatus") return kExecModBuildStatusIsolatedDocument;
  if (operationName == "ExecModDeploy") return kExecModDeployIsolatedDocument;
  if (operationName == "ExecModSetEnabled") return kExecModSetEnabledIsolatedDocument;
  if (operationName == "ExecModDelete") return kExecModDeleteIsolatedDocument;
  if (operationName == "ExecMods") return kExecModsIsolatedDocument;
  if (operationName == "ExecMyMods") return kExecMyModsIsolatedDocument;
  if (operationName == "ExecModLogs") return kExecModLogsIsolatedDocument;
  if (operationName == "ExecModPublish") return kExecModPublishIsolatedDocument;
  if (operationName == "ExecModListings") return kExecModListingsIsolatedDocument;
  if (operationName == "ExecModUnpublish") return kExecModUnpublishIsolatedDocument;
  if (operationName == "ExecModInstall") return kExecModInstallIsolatedDocument;
  if (operationName == "ExecAppMods") return kExecAppModsIsolatedDocument;
  if (operationName == "ExecModSwitches") return kExecModSwitchesIsolatedDocument;
  if (operationName == "ExecModSetSwitch") return kExecModSetSwitchIsolatedDocument;
  if (operationName == "ExecModClientBuild") return kExecModClientBuildIsolatedDocument;
  if (operationName == "ExecModClientDeploy") return kExecModClientDeployIsolatedDocument;
  if (operationName == "ExecModClientDelete") return kExecModClientDeleteIsolatedDocument;
  if (operationName == "ExecGridClientMods") return kExecGridClientModsIsolatedDocument;
  if (operationName == "ExecConsentClientMod") return kExecConsentClientModIsolatedDocument;
  if (operationName == "ExecTrustAuthor") return kExecTrustAuthorIsolatedDocument;
  if (operationName == "ExecRevokeClientModConsent") return kExecRevokeClientModConsentIsolatedDocument;
  if (operationName == "ExecRevokeAuthorTrust") return kExecRevokeAuthorTrustIsolatedDocument;
  if (operationName == "ExecModClientArtifact") return kExecModClientArtifactIsolatedDocument;
  return {};
}

}  // namespace exec

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

query NearbyGrids($input: NearbyGridsInput!) {
  nearbyGrids(input: $input) {
    appId
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
  }
}

query GridPermissionLimits($appId: BigInt!, $gridId: BigInt!) {
  gridPermissionLimits(appId: $appId, gridId: $gridId) {
    appId
    gridId
    permissionKeys
  }
}

query GridOpenPermissions($appId: BigInt!, $gridId: BigInt!) {
  gridOpenPermissions(appId: $appId, gridId: $gridId) {
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

mutation SetGridOpenPermissions($input: SetGridOpenPermissionsInput!) {
  setGridOpenPermissions(input: $input) {
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
inline constexpr std::string_view kNearbyGridsIsolatedDocument = R"gql(query NearbyGrids($input: NearbyGridsInput!) {
  nearbyGrids(input: $input) {
    appId
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
  }
})gql";
inline constexpr std::string_view kNearbyGridsOperationName = "NearbyGrids";
inline constexpr std::string_view kGridPermissionLimitsIsolatedDocument = R"gql(query GridPermissionLimits($appId: BigInt!, $gridId: BigInt!) {
  gridPermissionLimits(appId: $appId, gridId: $gridId) {
    appId
    gridId
    permissionKeys
  }
})gql";
inline constexpr std::string_view kGridPermissionLimitsOperationName = "GridPermissionLimits";
inline constexpr std::string_view kGridOpenPermissionsIsolatedDocument = R"gql(query GridOpenPermissions($appId: BigInt!, $gridId: BigInt!) {
  gridOpenPermissions(appId: $appId, gridId: $gridId) {
    appId
    gridId
    permissionKeys
  }
})gql";
inline constexpr std::string_view kGridOpenPermissionsOperationName = "GridOpenPermissions";
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
inline constexpr std::string_view kSetGridOpenPermissionsIsolatedDocument = R"gql(mutation SetGridOpenPermissions($input: SetGridOpenPermissionsInput!) {
  setGridOpenPermissions(input: $input) {
    appId
    gridId
    permissionKeys
  }
})gql";
inline constexpr std::string_view kSetGridOpenPermissionsOperationName = "SetGridOpenPermissions";
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
  if (operationName == "NearbyGrids") return kNearbyGridsIsolatedDocument;
  if (operationName == "GridPermissionLimits") return kGridPermissionLimitsIsolatedDocument;
  if (operationName == "GridOpenPermissions") return kGridOpenPermissionsIsolatedDocument;
  if (operationName == "GridGroupGrants") return kGridGroupGrantsIsolatedDocument;
  if (operationName == "CreateGrid") return kCreateGridIsolatedDocument;
  if (operationName == "DeleteGrid") return kDeleteGridIsolatedDocument;
  if (operationName == "GrantGridPermissions") return kGrantGridPermissionsIsolatedDocument;
  if (operationName == "RevokeGridPermissions") return kRevokeGridPermissionsIsolatedDocument;
  if (operationName == "SetGridPermissionLimits") return kSetGridPermissionLimitsIsolatedDocument;
  if (operationName == "SetGridOpenPermissions") return kSetGridOpenPermissionsIsolatedDocument;
  if (operationName == "AssignGroupToGrid") return kAssignGroupToGridIsolatedDocument;
  if (operationName == "RevokeGroupFromGrid") return kRevokeGroupFromGridIsolatedDocument;
  return {};
}

}  // namespace gameApps

namespace grids {

/// grids/CreateGridChannel.graphql
inline constexpr std::string_view kCreateGridChannelDocument = R"gql(mutation CreateGridChannel($input: CreateGridChannelInput!) {
  createGridChannel(input: $input) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    gridId
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateGridChannelIsolatedDocument = R"gql(mutation CreateGridChannel($input: CreateGridChannelInput!) {
  createGridChannel(input: $input) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    gridId
    createdAt
  }
})gql";
inline constexpr std::string_view kCreateGridChannelOperationName = "CreateGridChannel";

/// grids/GridChannels.graphql
inline constexpr std::string_view kGridChannelsDocument = R"gql(query GridChannels($appId: BigInt!, $gridId: BigInt!) {
  gridChannels(appId: $appId, gridId: $gridId) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    gridId
    createdAt
  }
})gql";
inline constexpr std::string_view kGridChannelsIsolatedDocument = R"gql(query GridChannels($appId: BigInt!, $gridId: BigInt!) {
  gridChannels(appId: $appId, gridId: $gridId) {
    groupId
    appId
    groupType
    name
    description
    ownerUserId
    membershipPolicy
    status
    defaultRoleId
    gridId
    createdAt
  }
})gql";
inline constexpr std::string_view kGridChannelsOperationName = "GridChannels";

/// grids/MintGridToken.graphql
inline constexpr std::string_view kMintGridTokenDocument = R"gql(mutation MintGridToken($input: MintGridTokenInput!) {
  mintGridToken(input: $input) {
    token
    gameTokenId
    appId
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
    expiresAt
  }
})gql";
inline constexpr std::string_view kMintGridTokenIsolatedDocument = R"gql(mutation MintGridToken($input: MintGridTokenInput!) {
  mintGridToken(input: $input) {
    token
    gameTokenId
    appId
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
    expiresAt
  }
})gql";
inline constexpr std::string_view kMintGridTokenOperationName = "MintGridToken";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "CreateGridChannel") return kCreateGridChannelIsolatedDocument;
  if (operationName == "GridChannels") return kGridChannelsIsolatedDocument;
  if (operationName == "MintGridToken") return kMintGridTokenIsolatedDocument;
  return {};
}

}  // namespace grids

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

namespace inputLog {

/// inputLog/InputLogMessages.graphql
inline constexpr std::string_view kInputLogMessagesDocument = R"gql(query InputLogMessages(
  $appId: BigInt!
  $gameTokenId: BigInt!
  $first: Int
  $after: String
  $filter: InputLogMessageFilter
) {
  inputLogMessages(
    appId: $appId
    gameTokenId: $gameTokenId
    first: $first
    after: $after
    filter: $filter
  ) {
    edges {
      cursor
      node {
        receivedAt
        receivedAtMicros
        userId
        gameTokenId
        messageType
        seq
        fromBundle
        signed
        sizeBytes
        body
        chunkX
        chunkY
        chunkZ
        actorUuid
        channelId
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
inline constexpr std::string_view kInputLogMessagesIsolatedDocument = R"gql(query InputLogMessages($appId: BigInt!, $gameTokenId: BigInt!, $first: Int, $after: String, $filter: InputLogMessageFilter) {
  inputLogMessages(
    appId: $appId
    gameTokenId: $gameTokenId
    first: $first
    after: $after
    filter: $filter
  ) {
    edges {
      cursor
      node {
        receivedAt
        receivedAtMicros
        userId
        gameTokenId
        messageType
        seq
        fromBundle
        signed
        sizeBytes
        body
        chunkX
        chunkY
        chunkZ
        actorUuid
        channelId
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
inline constexpr std::string_view kInputLogMessagesOperationName = "InputLogMessages";

/// inputLog/InputLogSessions.graphql
inline constexpr std::string_view kInputLogSessionsDocument = R"gql(query InputLogSessions(
  $appId: BigInt!
  $first: Int
  $after: String
  $filter: InputLogSessionFilter
) {
  inputLogSessions(appId: $appId, first: $first, after: $after, filter: $filter) {
    edges {
      cursor
      node {
        appId
        gameTokenId
        userId
        datacenter
        startedAt
        lastSeenAt
        endedAt
        endReason
        messageCount
        byteCount
        messageTypes
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
inline constexpr std::string_view kInputLogSessionsIsolatedDocument = R"gql(query InputLogSessions($appId: BigInt!, $first: Int, $after: String, $filter: InputLogSessionFilter) {
  inputLogSessions(appId: $appId, first: $first, after: $after, filter: $filter) {
    edges {
      cursor
      node {
        appId
        gameTokenId
        userId
        datacenter
        startedAt
        lastSeenAt
        endedAt
        endReason
        messageCount
        byteCount
        messageTypes
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
inline constexpr std::string_view kInputLogSessionsOperationName = "InputLogSessions";

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "InputLogMessages") return kInputLogMessagesIsolatedDocument;
  if (operationName == "InputLogSessions") return kInputLogSessionsIsolatedDocument;
  return {};
}

}  // namespace inputLog

namespace marketplace {

/// marketplace/Marketplace.graphql
inline constexpr std::string_view kMarketplaceDocument = R"gql(fragment GridClaimRequestFields on GridClaimRequest {
  requestId
  appId
  gridId
  requesterUserId
  status
  createdAt
}

# ---- Game API: browse / publish / acquire / install / consent -----------------

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
})gql";
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

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "MarketplaceGridClaimPolicy") return kMarketplaceGridClaimPolicyIsolatedDocument;
  if (operationName == "MarketplaceGridClaimRequests") return kMarketplaceGridClaimRequestsIsolatedDocument;
  if (operationName == "MarketplaceClaimGridOwnership") return kMarketplaceClaimGridOwnershipIsolatedDocument;
  if (operationName == "MarketplaceClaimGridChunk") return kMarketplaceClaimGridChunkIsolatedDocument;
  if (operationName == "MarketplaceReleaseClaimedGrid") return kMarketplaceReleaseClaimedGridIsolatedDocument;
  if (operationName == "MarketplaceDecideGridClaim") return kMarketplaceDecideGridClaimIsolatedDocument;
  if (operationName == "MarketplaceIssueGridClaimInvite") return kMarketplaceIssueGridClaimInviteIsolatedDocument;
  if (operationName == "MarketplaceAdmissionQueue") return kMarketplaceAdmissionQueueIsolatedDocument;
  if (operationName == "MarketplaceAppListings") return kMarketplaceAppListingsIsolatedDocument;
  if (operationName == "MarketplaceAppListingVersions") return kMarketplaceAppListingVersionsIsolatedDocument;
  if (operationName == "MarketplaceAppAcquisitions") return kMarketplaceAppAcquisitionsIsolatedDocument;
  if (operationName == "MarketplaceTransferListing") return kMarketplaceTransferListingIsolatedDocument;
  if (operationName == "MarketplaceSetListingStatus") return kMarketplaceSetListingStatusIsolatedDocument;
  if (operationName == "MarketplaceSetGridClaimPolicy") return kMarketplaceSetGridClaimPolicyIsolatedDocument;
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

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "CapturePaypalCheckout") return kCapturePaypalCheckoutIsolatedDocument;
  if (operationName == "CreateCheckout") return kCreateCheckoutIsolatedDocument;
  if (operationName == "MyCheckouts") return kMyCheckoutsIsolatedDocument;
  if (operationName == "MyCheckoutsConnection") return kMyCheckoutsConnectionIsolatedDocument;
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

namespace playerWallet {

/// playerWallet/PlayerWallet.graphql
inline constexpr std::string_view kPlayerWalletDocument = R"gql(fragment PlayerWalletFields on PlayerWallet {
  walletId
  userId
  balanceMicrousd
  holdsMicrousd
  balanceCents
  currency
  createdAt
}

fragment PlayerWalletTransactionFields on PlayerWalletTransaction {
  transactionId
  walletId
  userId
  amountMicrousd
  balanceAfterMicrousd
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
    chargedMicrousd
    chargedCents
  }
}

query AppPlayerMarkupAccrued($appId: BigInt!) {
  appPlayerMarkupAccrued(appId: $appId)
}

query AppPlayerMarkupAccruedMicrousd($appId: BigInt!) {
  appPlayerMarkupAccruedMicrousd(appId: $appId)
})gql";
inline constexpr std::string_view kPlayerWalletBalanceIsolatedDocument = R"gql(query PlayerWalletBalance {
  playerWalletBalance {
    ...PlayerWalletFields
  }
}

fragment PlayerWalletFields on PlayerWallet {
  walletId
  userId
  balanceMicrousd
  holdsMicrousd
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
  amountMicrousd
  balanceAfterMicrousd
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
    chargedMicrousd
    chargedCents
  }
})gql";
inline constexpr std::string_view kAppPlayerUsageOperationName = "AppPlayerUsage";
inline constexpr std::string_view kAppPlayerMarkupAccruedIsolatedDocument = R"gql(query AppPlayerMarkupAccrued($appId: BigInt!) {
  appPlayerMarkupAccrued(appId: $appId)
})gql";
inline constexpr std::string_view kAppPlayerMarkupAccruedOperationName = "AppPlayerMarkupAccrued";
inline constexpr std::string_view kAppPlayerMarkupAccruedMicrousdIsolatedDocument = R"gql(query AppPlayerMarkupAccruedMicrousd($appId: BigInt!) {
  appPlayerMarkupAccruedMicrousd(appId: $appId)
})gql";
inline constexpr std::string_view kAppPlayerMarkupAccruedMicrousdOperationName = "AppPlayerMarkupAccruedMicrousd";

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
  if (operationName == "PlayerRateMarkup") return kPlayerRateMarkupIsolatedDocument;
  if (operationName == "SetPlayerRateMarkup") return kSetPlayerRateMarkupIsolatedDocument;
  if (operationName == "AppPlayerUsage") return kAppPlayerUsageIsolatedDocument;
  if (operationName == "AppPlayerMarkupAccrued") return kAppPlayerMarkupAccruedIsolatedDocument;
  if (operationName == "AppPlayerMarkupAccruedMicrousd") return kAppPlayerMarkupAccruedMicrousdIsolatedDocument;
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
    runtimeGate {
      status
      reason
    }
    wildernessWritesOpen
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
    runtimeGate {
      status
      reason
    }
    wildernessWritesOpen
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

/// users/PlayerProfile.graphql
inline constexpr std::string_view kPlayerProfileDocument = R"gql(query PlayerProfile($userId: BigInt!) {
  playerProfile(userId: $userId) {
    userId
    gamertag
    disambiguation
  }
})gql";
inline constexpr std::string_view kPlayerProfileIsolatedDocument = R"gql(query PlayerProfile($userId: BigInt!) {
  playerProfile(userId: $userId) {
    userId
    gamertag
    disambiguation
  }
})gql";
inline constexpr std::string_view kPlayerProfileOperationName = "PlayerProfile";

/// users/PlayerProfiles.graphql
inline constexpr std::string_view kPlayerProfilesDocument = R"gql(query PlayerProfiles($userIds: [BigInt!]!) {
  playerProfiles(userIds: $userIds) {
    userId
    gamertag
    disambiguation
  }
})gql";
inline constexpr std::string_view kPlayerProfilesIsolatedDocument = R"gql(query PlayerProfiles($userIds: [BigInt!]!) {
  playerProfiles(userIds: $userIds) {
    userId
    gamertag
    disambiguation
  }
})gql";
inline constexpr std::string_view kPlayerProfilesOperationName = "PlayerProfiles";

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

inline constexpr std::string_view documentFor(std::string_view operationName) {
  if (operationName == "DeleteMyAccount") return kDeleteMyAccountIsolatedDocument;
  if (operationName == "FreePlayWindow") return kFreePlayWindowIsolatedDocument;
  if (operationName == "Me") return kMeIsolatedDocument;
  if (operationName == "PlayerProfile") return kPlayerProfileIsolatedDocument;
  if (operationName == "PlayerProfiles") return kPlayerProfilesIsolatedDocument;
  if (operationName == "UpdateGamertag") return kUpdateGamertagIsolatedDocument;
  if (operationName == "UpdateUserState") return kUpdateUserStateIsolatedDocument;
  if (operationName == "User") return kUserIsolatedDocument;
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
