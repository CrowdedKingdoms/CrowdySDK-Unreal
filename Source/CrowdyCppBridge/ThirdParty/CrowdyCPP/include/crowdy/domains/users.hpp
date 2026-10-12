#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

/// client.users() — profile reads + account admin. Targets the Management
/// API with the identity session token (admin methods additionally require
/// the relevant platform flag; the server enforces them).
namespace crowdy::domains {

/// Most ids UsersAPI::playerProfiles() takes in one call.
inline constexpr std::size_t kPlayerProfilesMax = 100;

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

  /// Look up a user by id. The private fields (email, state, isConfirmed, the
  /// early-access grants, orgId, externalId, userType, isSuperAdmin) are null for
  /// anyone but yourself; for another player's nametag use playerProfile().
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

  /// A player's public profile (`userId`, `gamertag`, `disambiguation`), for nametags
  /// and friends lists; null when there is no such user. Needs a game token.
  graphql::Json playerProfile(std::string_view userId) const {
    graphql::JVal vars;
    vars["userId"] = userId;
    return execUnwrap(gen::users::kPlayerProfileDocument, vars);
  }

  void playerProfileAsync(std::string_view userId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["userId"] = userId;
    execUnwrapAsync(gen::users::kPlayerProfileDocument, vars, {}, std::move(cb));
  }

  /// Refusal text of playerProfiles/playerProfilesAsync for more than kPlayerProfilesMax ids.
  static constexpr const char* kTooManyProfilesRefusal =
      "playerProfiles takes at most 100 ids";
  /// Public profiles for up to kPlayerProfilesMax players in one call (duplicates are
  /// read once; unknown ids are left out). No ids is an empty array without a request.
  /// More than 100 are refused before any request: blocking as a graphql::CrowdyError
  /// with code "INVALID_ARGUMENT" (an empty result in a CROWDY_NO_EXCEPTIONS build),
  /// async as an outcome with status Errc::InvalidArgument and kind Protocol.
  graphql::Json playerProfiles(const std::vector<std::string>& userIds) const {
    if (userIds.size() > kPlayerProfilesMax) {
#ifndef CROWDY_NO_EXCEPTIONS
      throw graphql::CrowdyError("INVALID_ARGUMENT", kTooManyProfilesRefusal);
#else
      return {};
#endif
    }
    if (userIds.empty()) return graphql::Json::parse("[]");
    return execUnwrap(gen::users::kPlayerProfilesDocument, profilesVars(userIds));
  }

  void playerProfilesAsync(const std::vector<std::string>& userIds,
                           graphql::GraphQLCallback cb) const {
    if (userIds.size() > kPlayerProfilesMax || userIds.empty()) {
      graphql::GraphQLOutcome out;
      if (userIds.empty()) {
        out.data = graphql::Json::parse("[]");
      } else {
        out.status = Errc::InvalidArgument;
        out.kind = graphql::GraphQLErrorKind::Protocol;
        out.errorMessage = kTooManyProfilesRefusal;
      }
      deliverAsync(std::move(out), std::move(cb));
      return;
    }
    execUnwrapAsync(gen::users::kPlayerProfilesDocument, profilesVars(userIds), {},
                    std::move(cb));
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

 private:
  static graphql::JVal profilesVars(const std::vector<std::string>& userIds) {
    graphql::JArray ids;
    ids.reserve(userIds.size());
    for (const std::string& id : userIds) ids.emplace_back(id);
    graphql::JVal vars;
    vars["userIds"] = graphql::JVal(std::move(ids));
    return vars;
  }
};

}  // namespace crowdy::domains
