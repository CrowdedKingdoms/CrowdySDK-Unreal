#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

namespace crowdy::domains {

/// client.grids() — grid-scoped platform surfaces (DN-10): grid tokens and
/// grid channels. Mirrors CrowdyJS `GridsAPI`.
class GridsAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  /// Narrow the app token this client holds to one grid. The result admits a
  /// short fixed list of gameplay fields, each confined to the grid, and the
  /// binary relay refuses it. The caller must own the grid or hold
  /// `run_client_code` on it. `ttlSeconds` 60-3600 (server default 900).
  graphql::Json mintToken(std::string_view appId, std::string_view gridId,
                          std::optional<int> ttlSeconds = std::nullopt) const {
    return execUnwrap(gen::grids::kMintGridTokenDocument,
                      mintVars(appId, gridId, ttlSeconds));
  }

  void mintTokenAsync(std::string_view appId, std::string_view gridId,
                      std::optional<int> ttlSeconds, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::grids::kMintGridTokenDocument,
                    mintVars(appId, gridId, ttlSeconds), {}, std::move(cb));
  }

  /// Create a channel that belongs to a grid you own; the grid's player
  /// modules may `emit_channel` into it.
  graphql::Json createChannel(std::string_view appId, std::string_view gridId,
                              std::string_view name) const {
    return execUnwrap(gen::grids::kCreateGridChannelDocument,
                      channelVars(appId, gridId, name));
  }

  void createChannelAsync(std::string_view appId, std::string_view gridId,
                          std::string_view name, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::grids::kCreateGridChannelDocument,
                    channelVars(appId, gridId, name), {}, std::move(cb));
  }

  /// The active channels of one grid, oldest first.
  graphql::Json channels(std::string_view appId, std::string_view gridId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    return execUnwrap(gen::grids::kGridChannelsDocument, vars);
  }

  void channelsAsync(std::string_view appId, std::string_view gridId,
                     graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    execUnwrapAsync(gen::grids::kGridChannelsDocument, vars, {}, std::move(cb));
  }

 private:
  static graphql::JVal mintVars(std::string_view appId, std::string_view gridId,
                                std::optional<int> ttlSeconds) {
    graphql::JVal input;
    input["appId"] = appId;
    input["gridId"] = gridId;
    if (ttlSeconds) input["ttlSeconds"] = *ttlSeconds;
    graphql::JVal vars;
    vars["input"] = std::move(input);
    return vars;
  }
  static graphql::JVal channelVars(std::string_view appId, std::string_view gridId,
                                   std::string_view name) {
    graphql::JVal input;
    input["appId"] = appId;
    input["gridId"] = gridId;
    input["name"] = name;
    graphql::JVal vars;
    vars["input"] = std::move(input);
    return vars;
  }
};

}  // namespace crowdy::domains
