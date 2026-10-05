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
