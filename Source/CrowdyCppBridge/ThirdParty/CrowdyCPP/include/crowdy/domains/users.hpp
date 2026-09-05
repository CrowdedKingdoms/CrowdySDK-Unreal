#pragma once

#include <functional>
#include <utility>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

/// client.users() — profile reads + account admin. Targets the Management
/// API with the identity session token (admin methods additionally require
/// the relevant platform flag; the server enforces them).
namespace crowdy::domains {

class UsersAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  /// The signed-in user's profile.
  graphql::Json me() const { return execUnwrap(gen::users::kMeDocument); }

  void meAsync(graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::users::kMeDocument, graphql::JVal(), {}, std::move(cb));
  }

  graphql::Json updateGamertag(std::string_view gamertag) const {
    graphql::JVal vars;
    vars["input"]["gamertag"] = gamertag;
    return execUnwrap(gen::users::kUpdateGamertagDocument, vars);
  }

  void updateGamertagAsync(std::string_view gamertag, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"]["gamertag"] = gamertag;
    execUnwrapAsync(gen::users::kUpdateGamertagDocument, vars, {}, std::move(cb));
  }

  bool deleteMyAccount() const {
    return execUnwrap(gen::users::kDeleteMyAccountDocument).asBool();
  }

  void deleteMyAccountAsync(std::function<void(graphql::GraphQLOutcome, bool)> cb) const {
    execUnwrapAsync(gen::users::kDeleteMyAccountDocument, graphql::JVal(), {},
                    [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
                      bool value = false;
                      if (out.ok()) value = out.data.asBool();
                      cb(std::move(out), value);
                    });
  }

  graphql::Json freePlayWindow() const {
    return execUnwrap(gen::users::kFreePlayWindowDocument);
  }

  void freePlayWindowAsync(graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::users::kFreePlayWindowDocument, graphql::JVal(), {}, std::move(cb));
  }

  graphql::Json get(std::string_view id) const {
    graphql::JVal vars;
    vars["id"] = id;
    return execUnwrap(gen::users::kUserDocument, vars);
  }

  void getAsync(std::string_view id, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["id"] = id;
    execUnwrapAsync(gen::users::kUserDocument, vars, {}, std::move(cb));
  }

  graphql::Json paginated(std::string_view query = {}, int limit = 50, int offset = 0) const {
    graphql::JVal vars;
    if (!query.empty()) vars["query"] = query;
    vars["limit"] = std::int64_t{limit};
    vars["offset"] = std::int64_t{offset};
    return execUnwrap(gen::users::documentFor("UsersPaginated"), vars, "UsersPaginated");
  }

  void paginatedAsync(std::string_view query, int limit, int offset,
                      graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    if (!query.empty()) vars["query"] = query;
    vars["limit"] = std::int64_t{limit};
    vars["offset"] = std::int64_t{offset};
    execUnwrapAsync(gen::users::documentFor("UsersPaginated"), vars, "UsersPaginated", std::move(cb));
  }

  /// Relay cursor pagination variant.
  graphql::Json listConnection(int first = 50, std::string_view after = {},
                               std::string_view query = {}) const {
    graphql::JVal vars;
    vars["first"] = std::int64_t{first};
    if (!after.empty()) vars["after"] = after;
    if (!query.empty()) vars["query"] = query;
    return execUnwrap(gen::users::documentFor("UsersConnection"), vars, "UsersConnection");
  }

  void listConnectionAsync(int first, std::string_view after, std::string_view query,
                           graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["first"] = std::int64_t{first};
    if (!after.empty()) vars["after"] = after;
    if (!query.empty()) vars["query"] = query;
    execUnwrapAsync(gen::users::documentFor("UsersConnection"), vars, "UsersConnection", std::move(cb));
  }

  graphql::Json setSuperAdmin(std::string_view userId, bool value) const {
    graphql::JVal vars;
    vars["userId"] = userId;
    vars["value"] = value;
    return execUnwrap(gen::users::kSetSuperAdminDocument, vars);
  }

  void setSuperAdminAsync(std::string_view userId, bool value, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["userId"] = userId;
    vars["value"] = value;
    execUnwrapAsync(gen::users::kSetSuperAdminDocument, vars, {}, std::move(cb));
  }

  graphql::Json setOperator(std::string_view userId, bool value) const {
    graphql::JVal vars;
    vars["userId"] = userId;
    vars["value"] = value;
    return execUnwrap(gen::users::kSetOperatorDocument, vars);
  }

  void setOperatorAsync(std::string_view userId, bool value, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["userId"] = userId;
    vars["value"] = value;
    execUnwrapAsync(gen::users::kSetOperatorDocument, vars, {}, std::move(cb));
  }

  graphql::Json setEarlyAccessOverride(std::string_view userId, bool value) const {
    graphql::JVal vars;
    vars["userId"] = userId;
    vars["value"] = value;
    return execUnwrap(gen::users::kSetEarlyAccessOverrideDocument, vars);
  }

  void setEarlyAccessOverrideAsync(std::string_view userId, bool value,
                                   graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["userId"] = userId;
    vars["value"] = value;
    execUnwrapAsync(gen::users::kSetEarlyAccessOverrideDocument, vars, {}, std::move(cb));
  }

  graphql::Json updateType(std::string_view userId, std::string_view value) const {
    graphql::JVal vars;
    vars["userId"] = userId;
    vars["value"] = value;
    return execUnwrap(gen::users::kUpdateUserTypeDocument, vars);
  }

  void updateTypeAsync(std::string_view userId, std::string_view value,
                       graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["userId"] = userId;
    vars["value"] = value;
    execUnwrapAsync(gen::users::kUpdateUserTypeDocument, vars, {}, std::move(cb));
  }

  graphql::Json forceLogout(std::string_view userId) const {
    graphql::JVal vars;
    vars["userId"] = userId;
    return execUnwrap(gen::users::kForceLogoutUserDocument, vars);
  }

  void forceLogoutAsync(std::string_view userId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["userId"] = userId;
    execUnwrapAsync(gen::users::kForceLogoutUserDocument, vars, {}, std::move(cb));
  }

  graphql::Json updateState(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(gen::users::kUpdateUserStateDocument, vars);
  }

  void updateStateAsync(const graphql::JVal& input, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(gen::users::kUpdateUserStateDocument, vars, {}, std::move(cb));
  }
};

}  // namespace crowdy::domains
