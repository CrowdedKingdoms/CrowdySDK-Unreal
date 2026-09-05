#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/enums.hpp"
#include "crowdy/generated/operations.hpp"
#include "crowdy/graphql/subscription_client.hpp"

/// client.gameModel() — the abstract game model: containers, properties,
/// functions with invoke policies, sessions, edges, events, and automations
/// (server-side NPCs / world ticks). Schema authoring (seed/upsert*) is a
/// studio-admin step run before players connect; runtime ops execute with a
/// player's app token. JSON payloads cross the wire as *Json strings.
/// See https://docs.crowdedkingdoms.com/game-api/game-models and
/// https://docs.crowdedkingdoms.com/game-api/autonomous-processes.
namespace crowdy::domains {

using GameModelPlayerCountStatus = gen::GameModelPlayerCountStatus;

struct GameModelActivePlayerCountSnapshot {
  std::string appId;
  std::int32_t activePlayerCount = 0;
  GameModelPlayerCountStatus status =
      GameModelPlayerCountStatus::UNAVAILABLE;
  std::optional<std::string> observedAt;
  /// Decimal GraphQL BigInt text; never narrowed to a native integer.
  std::string revision;
};

using GameModelActivePlayerCountCallback = std::function<void(
    graphql::GraphQLOutcome, GameModelActivePlayerCountSnapshot)>;

struct GameModelActivePlayerCountChange {
  std::string appId;
  std::int32_t previousCount = 0;
  std::int32_t currentCount = 0;
  std::int32_t delta = 0;
  /// Decimal GraphQL BigInt text; use it to deduplicate and detect gaps.
  std::string revision;
  std::string observedAt;
};

struct GameModelActivePlayerCountChangedCallbacks {
  std::function<void(GameModelActivePlayerCountChange)> next;
  std::function<void(graphql::GraphQLSubscriptionError)> error;
  std::function<void()> complete;
  std::function<void(graphql::GraphQLReconnectInfo)> reconnect;
};

struct GameModelContainerChange {
  std::string appId;
  std::string containerId;
  std::optional<std::string> typeName;
  std::optional<std::string> sessionId;
  std::string source;
  std::optional<std::string> functionName;
  std::vector<std::string> changedKeys;
  std::string occurredAt;
};

struct GameModelContainerChangedCallbacks {
  std::function<void(GameModelContainerChange)> next;
  std::function<void(graphql::GraphQLSubscriptionError)> error;
  std::function<void()> complete;
  std::function<void(graphql::GraphQLReconnectInfo)> reconnect;
};

class GameModelAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;
  GameModelAPI(
      std::shared_ptr<graphql::GraphQLClient> gql,
      std::shared_ptr<graphql::GraphQLSubscriptionClient> subscriptions)
      : DomainBase(std::move(gql)),
        subscriptions_(std::move(subscriptions)) {}

  // ----- Studio authoring (requires manage_apps) ----------------------------

  /// Idempotent bulk seed: container types, property defs, and functions in
  /// one call. The reference pattern for "load the rules" scripts.
  graphql::Json seed(const graphql::JVal& input) const {
    return studio("GameModelSeed", input);
  }
  void seedAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    studioAsync("GameModelSeed", input, std::move(cb));
  }
  graphql::Json upsertContainerType(const graphql::JVal& input) const {
    return studio("GameModelUpsertContainerType", input);
  }
  void upsertContainerTypeAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    studioAsync("GameModelUpsertContainerType", input, std::move(cb));
  }
  graphql::Json upsertPropertyDef(const graphql::JVal& input) const {
    return studio("GameModelUpsertPropertyDef", input);
  }
  void upsertPropertyDefAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    studioAsync("GameModelUpsertPropertyDef", input, std::move(cb));
  }
  graphql::Json deletePropertyDef(std::string_view appId, const graphql::JVal& args) const {
    graphql::JVal vars = args;
    vars["appId"] = appId;
    return execUnwrap(gen::gameModel::documentFor("GameModelDeletePropertyDef"), vars, "GameModelDeletePropertyDef");
  }
  void deletePropertyDefAsync(std::string_view appId, const graphql::JVal& args,
                              graphql::GraphQLCallback cb) const {
    graphql::JVal vars = args;
    vars["appId"] = appId;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelDeletePropertyDef"), vars, "GameModelDeletePropertyDef",
                    std::move(cb));
  }
  graphql::Json deleteContainerType(std::string_view appId, std::string_view typeName) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["typeName"] = typeName;
    return execUnwrap(gen::gameModel::documentFor("GameModelDeleteContainerType"), vars,
                      "GameModelDeleteContainerType");
  }
  void deleteContainerTypeAsync(std::string_view appId, std::string_view typeName,
                                graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["typeName"] = typeName;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelDeleteContainerType"), vars, "GameModelDeleteContainerType",
                    std::move(cb));
  }
  graphql::Json upsertFunction(const graphql::JVal& input) const {
    return studio("GameModelUpsertFunction", input);
  }
  void upsertFunctionAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    studioAsync("GameModelUpsertFunction", input, std::move(cb));
  }
  graphql::Json deleteFunction(std::string_view appId, std::string_view name) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    return execUnwrap(gen::gameModel::documentFor("GameModelDeleteFunction"), vars, "GameModelDeleteFunction");
  }
  void deleteFunctionAsync(std::string_view appId, std::string_view name,
                           graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelDeleteFunction"), vars, "GameModelDeleteFunction",
                    std::move(cb));
  }
  graphql::Json setPolicy(const graphql::JVal& input) const {
    return studio("GameModelSetPolicy", input);
  }
  void setPolicyAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    studioAsync("GameModelSetPolicy", input, std::move(cb));
  }
  graphql::Json defineFeature(const graphql::JVal& input) const {
    return studio("GameModelDefineFeature", input);
  }
  void defineFeatureAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    studioAsync("GameModelDefineFeature", input, std::move(cb));
  }
  graphql::Json grantTierFeature(const graphql::JVal& input) const {
    return studio("GameModelGrantTierFeature", input);
  }
  void grantTierFeatureAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    studioAsync("GameModelGrantTierFeature", input, std::move(cb));
  }
  graphql::Json revokeTierFeature(const graphql::JVal& input) const {
    return studio("GameModelRevokeTierFeature", input);
  }
  void revokeTierFeatureAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    studioAsync("GameModelRevokeTierFeature", input, std::move(cb));
  }

  // ----- Studio reads --------------------------------------------------------

  graphql::Json typeSchema(std::string_view appId, std::string_view typeName) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["typeName"] = typeName;
    return execUnwrap(gen::gameModel::documentFor("GameModelTypeSchema"), vars, "GameModelTypeSchema");
  }
  void typeSchemaAsync(std::string_view appId, std::string_view typeName,
                       graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["typeName"] = typeName;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelTypeSchema"), vars, "GameModelTypeSchema",
                    std::move(cb));
  }
  graphql::Json containerTypes(std::string_view appId) const {
    return studioByApp("GameModelContainerTypes", appId);
  }
  void containerTypesAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    studioByAppAsync("GameModelContainerTypes", appId, std::move(cb));
  }
  graphql::Json propertyDefs(std::string_view appId, std::string_view typeName) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["typeName"] = typeName;
    return execUnwrap(gen::gameModel::documentFor("GameModelPropertyDefs"), vars, "GameModelPropertyDefs");
  }
  void propertyDefsAsync(std::string_view appId, std::string_view typeName,
                         graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["typeName"] = typeName;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelPropertyDefs"), vars, "GameModelPropertyDefs",
                    std::move(cb));
  }
  graphql::Json function(std::string_view appId, std::string_view name) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    return execUnwrap(gen::gameModel::documentFor("GameModelFunction"), vars, "GameModelFunction");
  }
  void functionAsync(std::string_view appId, std::string_view name,
                     graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelFunction"), vars, "GameModelFunction",
                    std::move(cb));
  }
  graphql::Json functions(std::string_view appId, std::string_view containerTypeName = {}) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!containerTypeName.empty()) vars["containerTypeName"] = containerTypeName;
    return execUnwrap(gen::gameModel::documentFor("GameModelFunctions"), vars, "GameModelFunctions");
  }
  void functionsAsync(std::string_view appId, std::string_view containerTypeName,
                      graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!containerTypeName.empty()) vars["containerTypeName"] = containerTypeName;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelFunctions"), vars, "GameModelFunctions",
                    std::move(cb));
  }
  graphql::Json features(std::string_view appId) const {
    return studioByApp("GameModelFeatures", appId);
  }
  void featuresAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    studioByAppAsync("GameModelFeatures", appId, std::move(cb));
  }
  graphql::Json tierFeatures(std::string_view appId, std::string_view tierId = {}) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!tierId.empty()) vars["tierId"] = tierId;
    return execUnwrap(gen::gameModel::documentFor("GameModelTierFeatures"), vars, "GameModelTierFeatures");
  }
  void tierFeaturesAsync(std::string_view appId, std::string_view tierId,
                         graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!tierId.empty()) vars["tierId"] = tierId;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelTierFeatures"), vars, "GameModelTierFeatures",
                    std::move(cb));
  }
  graphql::Json policy(std::string_view appId) const {
    return studioByApp("GameModelPolicy", appId);
  }
  void policyAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    studioByAppAsync("GameModelPolicy", appId, std::move(cb));
  }

  // ----- Runtime (player app token) ------------------------------------------

  graphql::Json createContainer(const graphql::JVal& input) const {
    return runtime("GameModelCreateContainer", input);
  }
  void createContainerAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    runtimeAsync("GameModelCreateContainer", input, std::move(cb));
  }
  /// Atomic get-or-create by bindingKey. Creation-only fields are ignored
  /// when the key already exists; inspect `created` in the typed result row.
  graphql::Json ensureContainer(const graphql::JVal& input) const {
    return runtime("GameModelEnsureContainer", input);
  }
  void ensureContainerAsync(const graphql::JVal& input,
                            graphql::GraphQLCallback cb) const {
    runtimeAsync("GameModelEnsureContainer", input, std::move(cb));
  }
  graphql::Json deleteContainer(std::string_view appId, std::string_view containerId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["containerId"] = containerId;
    return execUnwrap(gen::gameModel::documentFor("GameModelDeleteContainer"), vars, "GameModelDeleteContainer");
  }
  void deleteContainerAsync(std::string_view appId, std::string_view containerId,
                            graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["containerId"] = containerId;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelDeleteContainer"), vars, "GameModelDeleteContainer",
                    std::move(cb));
  }
  graphql::Json setProperty(const graphql::JVal& input) const {
    return runtime("GameModelSetProperty", input);
  }
  void setPropertyAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    runtimeAsync("GameModelSetProperty", input, std::move(cb));
  }
  graphql::Json addEdge(const graphql::JVal& input) const {
    return runtime("GameModelAddEdge", input);
  }
  void addEdgeAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    runtimeAsync("GameModelAddEdge", input, std::move(cb));
  }
  graphql::Json deleteEdge(std::string_view appId, std::string_view edgeId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["edgeId"] = edgeId;
    return execUnwrap(gen::gameModel::documentFor("GameModelDeleteEdge"), vars, "GameModelDeleteEdge");
  }
  void deleteEdgeAsync(std::string_view appId, std::string_view edgeId,
                       graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["edgeId"] = edgeId;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelDeleteEdge"), vars, "GameModelDeleteEdge",
                    std::move(cb));
  }

  /// Transactional server-side mutation: invoke a seeded function. Invoke
  /// policies (owner_of_self, condition, is_host, is_current_turn, allow,
  /// and/or) are the authority model.
  graphql::Json invoke(const graphql::JVal& input) const {
    return runtime("GameModelInvoke", input);
  }
  void invokeAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    runtimeAsync("GameModelInvoke", input, std::move(cb));
  }

  /// Read the best-known app-scoped active gameplay-session count. FRESH is a
  /// complete fleet count; PARTIAL and UNAVAILABLE must not be treated as one.
  GameModelActivePlayerCountSnapshot activePlayerCount(
      std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    auto parsed = parseActivePlayerCountSnapshot(execUnwrap(
        gen::gameModel::documentFor("GameModelActivePlayerCount"), vars,
        "GameModelActivePlayerCount"));
    if (parsed) return std::move(*parsed);
#ifndef CROWDY_NO_EXCEPTIONS
    throw graphql::CrowdyProtocolError(
        "gameModelActivePlayerCount returned a malformed payload");
#else
    return {};
#endif
  }
  void activePlayerCountAsync(
      std::string_view appId, GameModelActivePlayerCountCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(
        gen::gameModel::documentFor("GameModelActivePlayerCount"), vars,
        "GameModelActivePlayerCount",
        [cb = std::move(cb)](graphql::GraphQLOutcome outcome) mutable {
          GameModelActivePlayerCountSnapshot snapshot;
          if (outcome.ok()) {
            auto parsed = parseActivePlayerCountSnapshot(outcome.data);
            if (parsed) {
              snapshot = std::move(*parsed);
            } else {
              outcome.status = Errc::Malformed;
              outcome.kind = graphql::GraphQLErrorKind::Protocol;
              outcome.errorMessage =
                  "gameModelActivePlayerCount returned a malformed payload";
            }
          }
          cb(std::move(outcome), std::move(snapshot));
        });
  }

  graphql::Json container(std::string_view appId, std::string_view containerId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["containerId"] = containerId;
    return execUnwrap(gen::gameModel::documentFor("GameModelContainer"), vars, "GameModelContainer");
  }
  void containerAsync(std::string_view appId, std::string_view containerId,
                      graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["containerId"] = containerId;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelContainer"), vars, "GameModelContainer",
                    std::move(cb));
  }
  graphql::Json containers(std::string_view appId,
                           std::string_view typeName = {},
                           std::string_view sessionId = {},
                           std::string_view bindingKey = {}) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!typeName.empty()) vars["typeName"] = typeName;
    if (!sessionId.empty()) vars["sessionId"] = sessionId;
    if (!bindingKey.empty()) vars["bindingKey"] = bindingKey;
    return execUnwrap(gen::gameModel::documentFor("GameModelContainers"), vars, "GameModelContainers");
  }
  void containersAsync(std::string_view appId, std::string_view typeName,
                       std::string_view sessionId, graphql::GraphQLCallback cb) const {
    containersAsync(appId, typeName, sessionId, {}, std::move(cb));
  }
  void containersAsync(std::string_view appId, std::string_view typeName,
                       std::string_view sessionId,
                       std::string_view bindingKey,
                       graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!typeName.empty()) vars["typeName"] = typeName;
    if (!sessionId.empty()) vars["sessionId"] = sessionId;
    if (!bindingKey.empty()) vars["bindingKey"] = bindingKey;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelContainers"), vars, "GameModelContainers",
                    std::move(cb));
  }
  /// Get-by-key convenience. The schema uniqueness constraint means this
  /// returns either one container or null.
  graphql::Json containerByBindingKey(
      std::string_view appId, std::string_view typeName,
      std::string_view bindingKey,
      std::string_view sessionId = {}) const {
    const auto rows = containers(appId, typeName, sessionId, bindingKey);
    return rows.isArray() && rows.size() > 0 ? rows.at(0) : graphql::Json();
  }
  /// Filtered/paged container list (2026-07+ servers): `where` is an array of
  /// up to 8 AND-combined `{key, op, valueJson}` predicates (ops ==, !=, <,
  /// >, <=, >=; requires typeName; missing properties fall back to the type
  /// default — the same shape automation selectors use); limit/offset page
  /// after filtering over the stable created-at ordering (pass -1 to omit).
  graphql::Json containersWhere(std::string_view appId, std::string_view typeName,
                                const graphql::JVal& where, int limit = -1, int offset = -1,
                                std::string_view sessionId = {},
                                std::string_view bindingKey = {}) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!typeName.empty()) vars["typeName"] = typeName;
    if (!sessionId.empty()) vars["sessionId"] = sessionId;
    if (!bindingKey.empty()) vars["bindingKey"] = bindingKey;
    if (!where.isNull()) vars["where"] = where;
    if (limit >= 0) vars["limit"] = limit;
    if (offset >= 0) vars["offset"] = offset;
    return execUnwrap(gen::gameModel::documentFor("GameModelContainers"), vars, "GameModelContainers");
  }
  void containersWhereAsync(std::string_view appId, std::string_view typeName,
                            const graphql::JVal& where, int limit, int offset,
                            std::string_view sessionId, graphql::GraphQLCallback cb) const {
    containersWhereAsync(appId, typeName, where, limit, offset, sessionId, {},
                         std::move(cb));
  }
  void containersWhereAsync(std::string_view appId, std::string_view typeName,
                            const graphql::JVal& where, int limit, int offset,
                            std::string_view sessionId,
                            std::string_view bindingKey,
                            graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!typeName.empty()) vars["typeName"] = typeName;
    if (!sessionId.empty()) vars["sessionId"] = sessionId;
    if (!bindingKey.empty()) vars["bindingKey"] = bindingKey;
    if (!where.isNull()) vars["where"] = where;
    if (limit >= 0) vars["limit"] = limit;
    if (offset >= 0) vars["offset"] = offset;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelContainers"), vars, "GameModelContainers",
                    std::move(cb));
  }

  /// Best-effort transition feed with no bootstrap event. Establish the stream,
  /// then query activePlayerCount(), deduplicate by decimal revision, and
  /// requery after reconnect or a detected revision gap.
  graphql::SubscriptionHandle activePlayerCountChanged(
      std::string_view appId,
      GameModelActivePlayerCountChangedCallbacks callbacks) const {
    if (!subscriptions_) {
      if (callbacks.error) {
        graphql::GraphQLSubscriptionError error;
        error.status = Errc::NotConnected;
        error.kind =
            graphql::GraphQLSubscriptionErrorKind::TransportUnavailable;
        error.code = "WEBSOCKET_TRANSPORT_UNAVAILABLE";
        error.message =
            "GameModelAPI has no GraphQL subscription client";
        callbacks.error(std::move(error));
      }
      return {};
    }
    graphql::JVal vars;
    vars["appId"] = appId;
    auto shared =
        std::make_shared<GameModelActivePlayerCountChangedCallbacks>(
            std::move(callbacks));
    graphql::GraphQLSubscriptionCallbacks graph;
    graph.onNext =
        [shared](graphql::GraphQLSubscriptionOutcome outcome) mutable {
          if (!outcome.ok()) {
            if (shared->error) {
              graphql::GraphQLSubscriptionError error;
              error.status = outcome.status;
              error.kind =
                  graphql::GraphQLSubscriptionErrorKind::GraphQL;
              error.errors = outcome.errors;
              error.code = outcome.errors.empty()
                               ? "GRAPHQL_SUBSCRIPTION_ERROR"
                               : outcome.errors.front().code;
              error.message =
                  outcome.errors.empty()
                      ? "Active-player-count subscription failed"
                      : outcome.errors.front().message;
              error.terminal = true;
              shared->error(std::move(error));
            }
            return;
          }
          auto change = parseActivePlayerCountChange(
              outcome.data["gameModelActivePlayerCountChanged"]);
          if (!change) {
            if (shared->error) {
              graphql::GraphQLSubscriptionError error;
              error.status = Errc::Malformed;
              error.kind =
                  graphql::GraphQLSubscriptionErrorKind::Protocol;
              error.code = "INVALID_ACTIVE_PLAYER_COUNT_CHANGE";
              error.message =
                  "Active-player-count subscription payload is malformed";
              error.terminal = true;
              shared->error(std::move(error));
            }
            return;
          }
          if (shared->next) shared->next(std::move(*change));
        };
    graph.onError =
        [shared](graphql::GraphQLSubscriptionError error) mutable {
          if (shared->error) shared->error(std::move(error));
        };
    graph.onComplete = [shared] {
      if (shared->complete) shared->complete();
    };
    graph.onReconnect =
        [shared](graphql::GraphQLReconnectInfo info) mutable {
          if (shared->reconnect) shared->reconnect(std::move(info));
        };
    return subscriptions_->subscribe(
        gen::gameModel::documentFor("GameModelActivePlayerCountChanged"),
        vars, "GameModelActivePlayerCountChanged", std::move(graph));
  }

  /// Typed GraphQL-WS wrapper for the notify-to-pull container feed.
  graphql::SubscriptionHandle containerChanged(
      std::string_view appId, std::string_view typeName,
      std::string_view sessionId,
      GameModelContainerChangedCallbacks callbacks) const {
    if (!subscriptions_) {
      if (callbacks.error) {
        graphql::GraphQLSubscriptionError error;
        error.status = Errc::NotConnected;
        error.kind =
            graphql::GraphQLSubscriptionErrorKind::TransportUnavailable;
        error.code = "WEBSOCKET_TRANSPORT_UNAVAILABLE";
        error.message =
            "GameModelAPI has no GraphQL subscription client";
        callbacks.error(std::move(error));
      }
      return {};
    }
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!typeName.empty()) vars["typeName"] = typeName;
    if (!sessionId.empty()) vars["sessionId"] = sessionId;
    auto shared =
        std::make_shared<GameModelContainerChangedCallbacks>(
            std::move(callbacks));
    graphql::GraphQLSubscriptionCallbacks graph;
    graph.onNext =
        [shared](graphql::GraphQLSubscriptionOutcome outcome) mutable {
          if (!outcome.ok()) {
            if (shared->error) {
              graphql::GraphQLSubscriptionError error;
              error.status = outcome.status;
              error.kind =
                  graphql::GraphQLSubscriptionErrorKind::GraphQL;
              error.errors = outcome.errors;
              error.code = outcome.errors.empty()
                               ? "GRAPHQL_SUBSCRIPTION_ERROR"
                               : outcome.errors.front().code;
              error.message =
                  outcome.errors.empty()
                      ? "Container-change subscription failed"
                      : outcome.errors.front().message;
              error.terminal = true;
              shared->error(std::move(error));
            }
            return;
          }
          const auto row = outcome.data["gameModelContainerChanged"];
          if (!row.isObject()) {
            if (shared->error) {
              graphql::GraphQLSubscriptionError error;
              error.status = Errc::Malformed;
              error.kind =
                  graphql::GraphQLSubscriptionErrorKind::Protocol;
              error.code = "INVALID_CONTAINER_CHANGE";
              error.message =
                  "Container-change subscription payload is malformed";
              error.terminal = true;
              shared->error(std::move(error));
            }
            return;
          }
          GameModelContainerChange change;
          change.appId = row["appId"].isString()
                             ? row["appId"].asString()
                             : row["appId"].dump();
          change.containerId = row["containerId"].asString();
          if (row["typeName"].isString()) {
            change.typeName = row["typeName"].asString();
          }
          if (row["sessionId"].isString()) {
            change.sessionId = row["sessionId"].asString();
          }
          change.source = row["source"].asString();
          if (row["functionName"].isString()) {
            change.functionName = row["functionName"].asString();
          }
          if (row["changedKeys"].isArray()) {
            row["changedKeys"].forEach([&](graphql::Json key) {
              if (key.isString()) change.changedKeys.push_back(key.asString());
            });
          }
          change.occurredAt = row["occurredAt"].asString();
          if (shared->next) shared->next(std::move(change));
        };
    graph.onError =
        [shared](graphql::GraphQLSubscriptionError error) mutable {
          if (shared->error) shared->error(std::move(error));
        };
    graph.onComplete = [shared] {
      if (shared->complete) shared->complete();
    };
    graph.onReconnect =
        [shared](graphql::GraphQLReconnectInfo info) mutable {
          if (shared->reconnect) shared->reconnect(std::move(info));
        };
    return subscriptions_->subscribe(
        gen::gameModel::documentFor("GameModelContainerChanged"), vars,
        "GameModelContainerChanged", std::move(graph));
  }
  graphql::SubscriptionHandle containerChanged(
      std::string_view appId,
      GameModelContainerChangedCallbacks callbacks) const {
    return containerChanged(appId, {}, {}, std::move(callbacks));
  }
  graphql::Json containerState(std::string_view appId, std::string_view containerId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["containerId"] = containerId;
    return execUnwrap(gen::gameModel::documentFor("GameModelContainerState"), vars, "GameModelContainerState");
  }
  void containerStateAsync(std::string_view appId, std::string_view containerId,
                           graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["containerId"] = containerId;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelContainerState"), vars, "GameModelContainerState",
                    std::move(cb));
  }
  graphql::Json traverse(const graphql::JVal& vars) const {
    return execUnwrap(gen::gameModel::documentFor("GameModelTraverse"), vars, "GameModelTraverse");
  }
  void traverseAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::gameModel::documentFor("GameModelTraverse"), vars, "GameModelTraverse",
                    std::move(cb));
  }

  graphql::Json createSession(const graphql::JVal& input) const {
    return runtime("GameModelCreateSession", input);
  }
  void createSessionAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    runtimeAsync("GameModelCreateSession", input, std::move(cb));
  }
  graphql::Json joinSession(const graphql::JVal& input) const {
    return runtime("GameModelJoinSession", input);
  }
  void joinSessionAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    runtimeAsync("GameModelJoinSession", input, std::move(cb));
  }
  graphql::Json setSessionTurn(const graphql::JVal& input) const {
    return runtime("GameModelSetSessionTurn", input);
  }
  void setSessionTurnAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    runtimeAsync("GameModelSetSessionTurn", input, std::move(cb));
  }
  graphql::Json session(std::string_view appId, std::string_view sessionId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["sessionId"] = sessionId;
    return execUnwrap(gen::gameModel::documentFor("GameModelSession"), vars, "GameModelSession");
  }
  void sessionAsync(std::string_view appId, std::string_view sessionId,
                    graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["sessionId"] = sessionId;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelSession"), vars, "GameModelSession",
                    std::move(cb));
  }
  graphql::Json sessions(std::string_view appId, std::string_view status = {}) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!status.empty()) vars["status"] = status;
    return execUnwrap(gen::gameModel::documentFor("GameModelSessions"), vars, "GameModelSessions");
  }
  void sessionsAsync(std::string_view appId, std::string_view status,
                     graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!status.empty()) vars["status"] = status;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelSessions"), vars, "GameModelSessions",
                    std::move(cb));
  }
  graphql::Json events(const graphql::JVal& vars) const {
    return execUnwrap(gen::gameModel::documentFor("GameModelEvents"), vars, "GameModelEvents");
  }
  void eventsAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::gameModel::documentFor("GameModelEvents"), vars, "GameModelEvents",
                    std::move(cb));
  }
  graphql::Json eventsConnection(const graphql::JVal& vars) const {
    return execUnwrap(gen::gameModel::documentFor("GameModelEventsConnection"), vars, "GameModelEventsConnection");
  }
  void eventsConnectionAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::gameModel::documentFor("GameModelEventsConnection"), vars, "GameModelEventsConnection",
                    std::move(cb));
  }
  /// Diagnostics: stitch one flow correlation id (a UUID from the flowId
  /// field of a GmEvent, GmAutomationRun, or WasmModuleRun) into a single
  /// cross-engine timeline — the model events, automation runs, and compute
  /// module runs sharing the flowId minted at the entry edge, each array
  /// ordered by time ascending. Requires the app-admin manage_apps
  /// permission and a game-api with gameModelFlow (2026-07-19+; older
  /// servers reject the operation with a validation error). An unknown
  /// flowId returns three empty arrays.
  graphql::Json flow(std::string_view appId, std::string_view flowId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["flowId"] = flowId;
    return execUnwrap(gen::gameModel::documentFor("GameModelFlow"), vars, "GameModelFlow");
  }
  void flowAsync(std::string_view appId, std::string_view flowId,
                 graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["flowId"] = flowId;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelFlow"), vars, "GameModelFlow",
                    std::move(cb));
  }

  // ----- Automations (admin-authored; diagnostics readable at runtime) -------

  graphql::Json upsertAutomation(const graphql::JVal& input) const {
    return automations("GameModelUpsertAutomation", input);
  }
  void upsertAutomationAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    automationsAsync("GameModelUpsertAutomation", input, std::move(cb));
  }
  graphql::Json deleteAutomation(std::string_view appId, std::string_view name) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    return execUnwrap(gen::gameModel::documentFor("GameModelDeleteAutomation"), vars,
                      "GameModelDeleteAutomation");
  }
  void deleteAutomationAsync(std::string_view appId, std::string_view name,
                             graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelDeleteAutomation"), vars, "GameModelDeleteAutomation",
                    std::move(cb));
  }
  graphql::Json setAutomationEnabled(std::string_view appId, std::string_view name,
                                     bool enabled) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    vars["enabled"] = enabled;
    return execUnwrap(gen::gameModel::documentFor("GameModelSetAutomationEnabled"), vars,
                      "GameModelSetAutomationEnabled");
  }
  void setAutomationEnabledAsync(std::string_view appId, std::string_view name, bool enabled,
                                 graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    vars["enabled"] = enabled;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelSetAutomationEnabled"), vars,
                    "GameModelSetAutomationEnabled", std::move(cb));
  }
  graphql::Json upsertAutomationTrigger(const graphql::JVal& input) const {
    return automations("GameModelUpsertAutomationTrigger", input);
  }
  void upsertAutomationTriggerAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    automationsAsync("GameModelUpsertAutomationTrigger", input, std::move(cb));
  }
  graphql::Json deleteAutomationTrigger(std::string_view appId, std::string_view triggerId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["triggerId"] = triggerId;
    return execUnwrap(gen::gameModel::documentFor("GameModelDeleteAutomationTrigger"), vars,
                      "GameModelDeleteAutomationTrigger");
  }
  void deleteAutomationTriggerAsync(std::string_view appId, std::string_view triggerId,
                                    graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["triggerId"] = triggerId;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelDeleteAutomationTrigger"), vars,
                    "GameModelDeleteAutomationTrigger", std::move(cb));
  }
  graphql::Json setAutomationPolicy(const graphql::JVal& input) const {
    return automations("GameModelSetAutomationPolicy", input);
  }
  void setAutomationPolicyAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    automationsAsync("GameModelSetAutomationPolicy", input, std::move(cb));
  }
  graphql::Json runAutomation(std::string_view appId, std::string_view name) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    return execUnwrap(gen::gameModel::documentFor("GameModelRunAutomation"), vars,
                      "GameModelRunAutomation");
  }
  void runAutomationAsync(std::string_view appId, std::string_view name,
                          graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelRunAutomation"), vars, "GameModelRunAutomation",
                    std::move(cb));
  }
  graphql::Json automationsList(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::gameModel::documentFor("GameModelAutomations"), vars, "GameModelAutomations");
  }
  void automationsListAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelAutomations"), vars, "GameModelAutomations",
                    std::move(cb));
  }
  graphql::Json automation(std::string_view appId, std::string_view name) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    return execUnwrap(gen::gameModel::documentFor("GameModelAutomation"), vars, "GameModelAutomation");
  }
  void automationAsync(std::string_view appId, std::string_view name,
                       graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["name"] = name;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelAutomation"), vars, "GameModelAutomation",
                    std::move(cb));
  }
  graphql::Json automationTriggers(std::string_view appId,
                                   std::string_view automationName = {}) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!automationName.empty()) vars["automationName"] = automationName;
    return execUnwrap(gen::gameModel::documentFor("GameModelAutomationTriggers"), vars,
                      "GameModelAutomationTriggers");
  }
  void automationTriggersAsync(std::string_view appId, std::string_view automationName,
                               graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (!automationName.empty()) vars["automationName"] = automationName;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelAutomationTriggers"), vars,
                    "GameModelAutomationTriggers", std::move(cb));
  }
  graphql::Json automationPolicy(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::gameModel::documentFor("GameModelAutomationPolicy"), vars,
                      "GameModelAutomationPolicy");
  }
  void automationPolicyAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelAutomationPolicy"), vars, "GameModelAutomationPolicy",
                    std::move(cb));
  }
  graphql::Json automationRuns(const graphql::JVal& vars) const {
    return execUnwrap(gen::gameModel::documentFor("GameModelAutomationRuns"), vars,
                      "GameModelAutomationRuns");
  }
  void automationRunsAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::gameModel::documentFor("GameModelAutomationRuns"), vars, "GameModelAutomationRuns",
                    std::move(cb));
  }
  graphql::Json automationStats(std::string_view appId, int windowMinutes = 0) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (windowMinutes > 0) vars["windowMinutes"] = std::int64_t{windowMinutes};
    return execUnwrap(gen::gameModel::documentFor("GameModelAutomationStats"), vars,
                      "GameModelAutomationStats");
  }
  void automationStatsAsync(std::string_view appId, int windowMinutes,
                            graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    if (windowMinutes > 0) vars["windowMinutes"] = std::int64_t{windowMinutes};
    execUnwrapAsync(gen::gameModel::documentFor("GameModelAutomationStats"), vars, "GameModelAutomationStats",
                    std::move(cb));
  }
  graphql::Json appDiagnostics(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::gameModel::documentFor("GameModelAppDiagnostics"), vars,
                      "GameModelAppDiagnostics");
  }
  void appDiagnosticsAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(gen::gameModel::documentFor("GameModelAppDiagnostics"), vars, "GameModelAppDiagnostics",
                    std::move(cb));
  }

  // ----- Timers (requires manage_apps) ---------------------------------------

  /// Arm a one-shot delayed invocation. Durable and claimed by exactly one API
  /// replica, so it fires once; the target function must be autonomousInvocable
  /// because the fire is headless. For player-driven delays declare a `timers`
  /// effect on the function instead, so the delay is armed atomically with that
  /// invocation's mutations. `dedupeKey` re-arms in place ("reset the
  /// countdown") rather than queueing another fire.
  graphql::Json scheduleInvoke(const graphql::JVal& input) const {
    return automations("GameModelScheduleInvoke", input);
  }
  void scheduleInvokeAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    automationsAsync("GameModelScheduleInvoke", input, std::move(cb));
  }
  /// Cancel pending timers by id or dedupe key; `vars` carries appId plus at
  /// least one selector. Returns how many were removed.
  graphql::Json cancelTimer(const graphql::JVal& vars) const {
    return execUnwrap(gen::gameModel::documentFor("GameModelCancelTimer"), vars,
                      "GameModelCancelTimer");
  }
  void cancelTimerAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::gameModel::documentFor("GameModelCancelTimer"), vars, "GameModelCancelTimer",
                    std::move(cb));
  }
  /// Pending timers, soonest first. A timer leaves this list the instant it is
  /// claimed, so an empty list means nothing is scheduled, not that nothing ran.
  graphql::Json timers(const graphql::JVal& vars) const {
    return execUnwrap(gen::gameModel::documentFor("GameModelTimers"), vars, "GameModelTimers");
  }
  void timersAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::gameModel::documentFor("GameModelTimers"), vars, "GameModelTimers",
                    std::move(cb));
  }

 private:
  static std::optional<std::int32_t> parseGraphQLInt(
      const graphql::Json& value) {
    if (!value.isNumber()) return std::nullopt;
    const auto parsed = value.tryAsBigInt();
    if (!parsed ||
        *parsed < std::numeric_limits<std::int32_t>::min() ||
        *parsed > std::numeric_limits<std::int32_t>::max()) {
      return std::nullopt;
    }
    return static_cast<std::int32_t>(*parsed);
  }

  static std::optional<std::string> parseNonNegativeBigInt(
      const graphql::Json& value) {
    std::string decimal = value.asBigIntString();
    if (decimal.empty() || decimal.front() == '-') return std::nullopt;
    return decimal;
  }

  static std::optional<GameModelActivePlayerCountSnapshot>
  parseActivePlayerCountSnapshot(const graphql::Json& row) {
    if (!row.isObject()) return std::nullopt;
    auto appId = parseNonNegativeBigInt(row["appId"]);
    auto count = parseGraphQLInt(row["activePlayerCount"]);
    auto status = row["status"].isString()
                      ? gen::gameModelPlayerCountStatusFromString(
                            row["status"].asStringView())
                      : std::nullopt;
    auto revision = parseNonNegativeBigInt(row["revision"]);
    const auto observedAt = row["observedAt"];
    if (!appId || !count || *count < 0 || !status || !revision ||
        !observedAt.ok() ||
        (!observedAt.isNull() && !observedAt.isString())) {
      return std::nullopt;
    }

    GameModelActivePlayerCountSnapshot snapshot;
    snapshot.appId = std::move(*appId);
    snapshot.activePlayerCount = *count;
    snapshot.status = *status;
    if (observedAt.isString()) {
      snapshot.observedAt = observedAt.asString();
    }
    snapshot.revision = std::move(*revision);
    return snapshot;
  }

  static std::optional<GameModelActivePlayerCountChange>
  parseActivePlayerCountChange(const graphql::Json& row) {
    if (!row.isObject()) return std::nullopt;
    auto appId = parseNonNegativeBigInt(row["appId"]);
    auto previous = parseGraphQLInt(row["previousCount"]);
    auto current = parseGraphQLInt(row["currentCount"]);
    auto delta = parseGraphQLInt(row["delta"]);
    auto revision = parseNonNegativeBigInt(row["revision"]);
    const auto observedAt = row["observedAt"];
    if (!appId || !previous || *previous < 0 || !current || *current < 0 ||
        !delta || !revision || !observedAt.isString() ||
        static_cast<std::int64_t>(*current) -
                static_cast<std::int64_t>(*previous) !=
            *delta) {
      return std::nullopt;
    }

    GameModelActivePlayerCountChange change;
    change.appId = std::move(*appId);
    change.previousCount = *previous;
    change.currentCount = *current;
    change.delta = *delta;
    change.revision = std::move(*revision);
    change.observedAt = observedAt.asString();
    return change;
  }

  graphql::Json studio(std::string_view op, const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::gameModel::documentFor(op), vars, op);
  }
  void studioAsync(std::string_view op, const graphql::JVal& input,
                   graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::gameModel::documentFor(op), vars, op, std::move(cb));
  }
  graphql::Json studioByApp(std::string_view op, std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return execUnwrap(gen::gameModel::documentFor(op), vars, op);
  }
  void studioByAppAsync(std::string_view op, std::string_view appId,
                        graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    execUnwrapAsync(gen::gameModel::documentFor(op), vars, op, std::move(cb));
  }
  graphql::Json runtime(std::string_view op, const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::gameModel::documentFor(op), vars, op);
  }
  void runtimeAsync(std::string_view op, const graphql::JVal& input,
                    graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::gameModel::documentFor(op), vars, op, std::move(cb));
  }
  graphql::Json automations(std::string_view op, const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::gameModel::documentFor(op), vars, op);
  }
  void automationsAsync(std::string_view op, const graphql::JVal& input,
                        graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::gameModel::documentFor(op), vars, op, std::move(cb));
  }

  std::shared_ptr<graphql::GraphQLSubscriptionClient> subscriptions_;
};

}  // namespace crowdy::domains
