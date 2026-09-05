#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/domains/types.hpp"
#include "crowdy/generated/operations.hpp"

/// Authentication and account lifecycle — client.auth(). Passwordless:
/// magic link, social/OIDC, or (dev only) the dev bypass. Every path returns
/// an identity SESSION token, stored on the shared auth state automatically.
/// Gameplay tokens are minted separately via client.portal().
namespace crowdy::domains {

class AuthAPI : public DomainBase {
 public:
  AuthAPI(std::shared_ptr<graphql::GraphQLClient> gql, std::shared_ptr<graphql::AuthState> auth)
      : DomainBase(std::move(gql)), auth_(std::move(auth)) {}

  /// The federated sign-in providers currently enabled (e.g. ["google"]).
  std::vector<std::string> availableLoginProviders() const {
    graphql::Json r = execUnwrap("query AvailableLoginProviders { availableLoginProviders }");
    std::vector<std::string> out;
    r.forEach([&](graphql::Json p) { out.push_back(p.asString()); });
    return out;
  }

  void availableLoginProvidersAsync(
      std::function<void(graphql::GraphQLOutcome, std::vector<std::string>)> cb) const {
    execUnwrapAsync("query AvailableLoginProviders { availableLoginProviders }", graphql::JVal(), {},
                    [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
                      std::vector<std::string> value;
                      if (out.ok())
                        out.data.forEach(
                            [&](graphql::Json p) { value.push_back(p.asString()); });
                      cb(std::move(out), std::move(value));
                    });
  }

  /// Email a one-time magic sign-in link (creates the account on first
  /// sign-in). The token arrives ONLY by email: `devToken` used to hand it back
  /// in the response whenever the server had the dev bypass on, and that field
  /// is gone. An automated caller should registerUser() an account it holds the
  /// password to instead.
  graphql::Json requestLoginLink(std::string_view email, std::string_view redirectUri = {}) const {
    graphql::JVal input;
    input["email"] = email;
    if (!redirectUri.empty()) input["redirectUri"] = redirectUri;
    graphql::JVal vars;
    vars["input"] = input;
    return execUnwrap(
        "mutation RequestLoginLink($input: RequestLoginLinkInput!) {"
        " requestLoginLink(input: $input) { sent } }",
        vars);
  }

  void requestLoginLinkAsync(std::string_view email, std::string_view redirectUri,
                             graphql::GraphQLCallback cb) const {
    graphql::JVal input;
    input["email"] = email;
    if (!redirectUri.empty()) input["redirectUri"] = redirectUri;
    graphql::JVal vars;
    vars["input"] = input;
    execUnwrapAsync(
        "mutation RequestLoginLink($input: RequestLoginLinkInput!) {"
        " requestLoginLink(input: $input) { sent } }",
        vars, {}, std::move(cb));
  }

  /// Complete a magic-link sign-in; stores the session token on success.
  AuthResponse completeLoginLink(std::string_view token) const {
    graphql::JVal vars;
    vars["input"]["token"] = token;
    auto r = AuthResponse::fromJson(execUnwrap(
        "mutation CompleteLoginLink($input: CompleteLoginLinkInput!) {"
        " completeLoginLink(input: $input) { token gameTokenId user { userId email gamertag } } }",
        vars));
    if (!r.token.empty()) auth_->setToken(r.token);
    return r;
  }

  void completeLoginLinkAsync(std::string_view token,
                              std::function<void(graphql::GraphQLOutcome, AuthResponse)> cb) const {
    graphql::JVal vars;
    vars["input"]["token"] = token;
    execUnwrapAsync(
        "mutation CompleteLoginLink($input: CompleteLoginLinkInput!) {"
        " completeLoginLink(input: $input) { token gameTokenId user { userId email gamertag } } }",
        vars, {},
        [auth = auth_, cb = std::move(cb)](
            graphql::GraphQLOutcome out) mutable {
          AuthResponse value{};
          if (out.ok()) {
            value = AuthResponse::fromJson(out.data);
            if (!value.token.empty()) auth->setToken(value.token);
          }
          cb(std::move(out), value);
        });
  }

  /// Begin a federated (social) sign-in: returns { authorizeUrl, state }.
  graphql::Json socialLoginStart(std::string_view provider, std::string_view redirectUri) const {
    graphql::JVal vars;
    vars["input"]["provider"] = provider;
    vars["input"]["redirectUri"] = redirectUri;
    return execUnwrap(
        "mutation SocialLoginStart($input: SocialLoginStartInput!) {"
        " socialLoginStart(input: $input) { authorizeUrl state } }",
        vars);
  }

  void socialLoginStartAsync(std::string_view provider, std::string_view redirectUri,
                             graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"]["provider"] = provider;
    vars["input"]["redirectUri"] = redirectUri;
    execUnwrapAsync(
        "mutation SocialLoginStart($input: SocialLoginStartInput!) {"
        " socialLoginStart(input: $input) { authorizeUrl state } }",
        vars, {}, std::move(cb));
  }

  /// Complete a federated sign-in from the provider callback; stores token.
  AuthResponse socialLoginComplete(std::string_view provider, std::string_view code,
                                   std::string_view state) const {
    graphql::JVal vars;
    vars["input"]["provider"] = provider;
    vars["input"]["code"] = code;
    vars["input"]["state"] = state;
    auto r = AuthResponse::fromJson(execUnwrap(
        "mutation SocialLoginComplete($input: SocialLoginCompleteInput!) {"
        " socialLoginComplete(input: $input) { token gameTokenId user { userId email gamertag } } }",
        vars));
    if (!r.token.empty()) auth_->setToken(r.token);
    return r;
  }

  void socialLoginCompleteAsync(std::string_view provider, std::string_view code,
                                std::string_view state,
                                std::function<void(graphql::GraphQLOutcome, AuthResponse)> cb) const {
    graphql::JVal vars;
    vars["input"]["provider"] = provider;
    vars["input"]["code"] = code;
    vars["input"]["state"] = state;
    execUnwrapAsync(
        "mutation SocialLoginComplete($input: SocialLoginCompleteInput!) {"
        " socialLoginComplete(input: $input) { token gameTokenId user { userId email gamertag } } }",
        vars, {},
        [auth = auth_, cb = std::move(cb)](
            graphql::GraphQLOutcome out) mutable {
          AuthResponse value{};
          if (out.ok()) {
            value = AuthResponse::fromJson(out.data);
            if (!value.token.empty()) auth->setToken(value.token);
          }
          cb(std::move(out), value);
        });
  }

  // devLogin() and devLoginAsync() are REMOVED. They wrapped a server mutation
  // that issued a session for any address with no proof of ownership, gated on
  // DEV_AUTH_BYPASS; ck-api deleted it on 2026-08-20, so a wrapper could only
  // produce a GraphQL validation error. Use login() / registerUser() below --
  // they were already here and need neither an inbox nor a browser.

  /// The signed-in user's linked sign-in identities.
  graphql::Json myIdentities() const {
    return execUnwrap(
        "query MyIdentities { myIdentities {"
        " identityId provider subject email emailVerified createdAt lastLoginAt } }");
  }

  void myIdentitiesAsync(graphql::GraphQLCallback cb) const {
    execUnwrapAsync(
        "query MyIdentities { myIdentities {"
        " identityId provider subject email emailVerified createdAt lastLoginAt } }",
        graphql::JVal(), {}, std::move(cb));
  }

  /// Link an additional federated identity (from a social callback).
  graphql::Json linkIdentity(std::string_view provider, std::string_view code,
                             std::string_view state) const {
    graphql::JVal vars;
    vars["input"]["provider"] = provider;
    vars["input"]["code"] = code;
    vars["input"]["state"] = state;
    return execUnwrap(
        "mutation LinkIdentity($input: LinkIdentityInput!) { linkIdentity(input: $input) {"
        " identityId provider subject email emailVerified createdAt lastLoginAt } }",
        vars);
  }

  void linkIdentityAsync(std::string_view provider, std::string_view code, std::string_view state,
                         graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"]["provider"] = provider;
    vars["input"]["code"] = code;
    vars["input"]["state"] = state;
    execUnwrapAsync(
        "mutation LinkIdentity($input: LinkIdentityInput!) { linkIdentity(input: $input) {"
        " identityId provider subject email emailVerified createdAt lastLoginAt } }",
        vars, {}, std::move(cb));
  }

  /// Unlink a federated identity (cannot remove the last sign-in method).
  bool unlinkIdentity(std::string_view identityId) const {
    graphql::JVal vars;
    vars["identityId"] = identityId;
    return execUnwrap(
               "mutation UnlinkIdentity($identityId: String!) { unlinkIdentity(identityId: $identityId) }",
               vars)
        .asBool();
  }

  void unlinkIdentityAsync(std::string_view identityId,
                           std::function<void(graphql::GraphQLOutcome, bool)> cb) const {
    graphql::JVal vars;
    vars["identityId"] = identityId;
    execUnwrapAsync(
        "mutation UnlinkIdentity($identityId: String!) { unlinkIdentity(identityId: $identityId) }",
        vars, {}, [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
          bool value = false;
          if (out.ok()) value = out.data.asBool();
          cb(std::move(out), value);
        });
  }

  /// Whether the account behind an email has a password set. Public — used
  /// by sign-in UIs to pick a flow (magic link vs legacy password).
  graphql::Json checkAuthMethod(std::string_view email) const {
    graphql::JVal vars;
    vars["input"]["email"] = email;
    return execUnwrap(
        "query CheckAuthMethod($input: CheckAuthMethodInput!) {"
        " checkAuthMethod(input: $input) { hasPassword } }",
        vars);
  }

  void checkAuthMethodAsync(std::string_view email, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"]["email"] = email;
    execUnwrapAsync(
        "query CheckAuthMethod($input: CheckAuthMethodInput!) {"
        " checkAuthMethod(input: $input) { hasPassword } }",
        vars, {}, std::move(cb));
  }

  // ----- Legacy password authentication ---------------------------------------
  // The platform is passwordless-first; these wrap the password surface for
  // apps migrating older accounts. Both return the identity SESSION token.

  AuthResponse login(std::string_view email, std::string_view password) const {
    graphql::JVal vars;
    vars["loginUserInput"]["email"] = email;
    vars["loginUserInput"]["password"] = password;
    auto r = AuthResponse::fromJson(execUnwrap(
        "mutation Login($loginUserInput: LoginUserInput!) {"
        " login(loginUserInput: $loginUserInput) {"
        " token gameTokenId user { userId email gamertag } } }",
        vars));
    if (!r.token.empty()) auth_->setToken(r.token);
    return r;
  }

  void loginAsync(std::string_view email, std::string_view password,
                  std::function<void(graphql::GraphQLOutcome, AuthResponse)> cb) const {
    graphql::JVal vars;
    vars["loginUserInput"]["email"] = email;
    vars["loginUserInput"]["password"] = password;
    execUnwrapAsync(
        "mutation Login($loginUserInput: LoginUserInput!) {"
        " login(loginUserInput: $loginUserInput) {"
        " token gameTokenId user { userId email gamertag } } }",
        vars, {},
        [auth = auth_, cb = std::move(cb)](
            graphql::GraphQLOutcome out) mutable {
          AuthResponse value{};
          if (out.ok()) {
            value = AuthResponse::fromJson(out.data);
            if (!value.token.empty()) auth->setToken(value.token);
          }
          cb(std::move(out), value);
        });
  }

  AuthResponse registerUser(std::string_view email, std::string_view password,
                            std::string_view gamertag = {}) const {
    graphql::JVal vars;
    vars["registerUserInput"]["email"] = email;
    vars["registerUserInput"]["password"] = password;
    if (!gamertag.empty()) vars["registerUserInput"]["gamertag"] = gamertag;
    auto r = AuthResponse::fromJson(execUnwrap(
        "mutation Register($registerUserInput: RegisterUserInput!) {"
        " register(registerUserInput: $registerUserInput) {"
        " token gameTokenId user { userId email gamertag } } }",
        vars));
    if (!r.token.empty()) auth_->setToken(r.token);
    return r;
  }

  void registerUserAsync(std::string_view email, std::string_view password,
                         std::string_view gamertag,
                         std::function<void(graphql::GraphQLOutcome, AuthResponse)> cb) const {
    graphql::JVal vars;
    vars["registerUserInput"]["email"] = email;
    vars["registerUserInput"]["password"] = password;
    if (!gamertag.empty()) vars["registerUserInput"]["gamertag"] = gamertag;
    execUnwrapAsync(
        "mutation Register($registerUserInput: RegisterUserInput!) {"
        " register(registerUserInput: $registerUserInput) {"
        " token gameTokenId user { userId email gamertag } } }",
        vars, {},
        [auth = auth_, cb = std::move(cb)](
            graphql::GraphQLOutcome out) mutable {
          AuthResponse value{};
          if (out.ok()) {
            value = AuthResponse::fromJson(out.data);
            if (!value.token.empty()) auth->setToken(value.token);
          }
          cb(std::move(out), value);
        });
  }

  bool confirmEmail(std::string_view token) const {
    graphql::JVal vars;
    vars["token"] = token;
    return execUnwrap(
               "mutation ConfirmEmail($token: String!) { confirmEmail(token: $token) }", vars)
        .asBool();
  }

  void confirmEmailAsync(std::string_view token,
                         std::function<void(graphql::GraphQLOutcome, bool)> cb) const {
    graphql::JVal vars;
    vars["token"] = token;
    execUnwrapAsync("mutation ConfirmEmail($token: String!) { confirmEmail(token: $token) }", vars,
                    {}, [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
                      bool value = false;
                      if (out.ok()) value = out.data.asBool();
                      cb(std::move(out), value);
                    });
  }

  bool requestPasswordReset(std::string_view email) const {
    graphql::JVal vars;
    vars["email"] = email;
    return execUnwrap(
               "mutation RequestPasswordReset($email: String!) { requestPasswordReset(email: $email) }",
               vars)
        .asBool();
  }

  void requestPasswordResetAsync(std::string_view email,
                                 std::function<void(graphql::GraphQLOutcome, bool)> cb) const {
    graphql::JVal vars;
    vars["email"] = email;
    execUnwrapAsync(
        "mutation RequestPasswordReset($email: String!) { requestPasswordReset(email: $email) }",
        vars, {}, [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
          bool value = false;
          if (out.ok()) value = out.data.asBool();
          cb(std::move(out), value);
        });
  }

  bool resetPassword(std::string_view token, std::string_view newPassword) const {
    graphql::JVal vars;
    vars["resetPasswordInput"]["token"] = token;
    vars["resetPasswordInput"]["newPassword"] = newPassword;
    return execUnwrap(
               "mutation ResetPassword($resetPasswordInput: ResetPasswordInput!) {"
               " resetPassword(resetPasswordInput: $resetPasswordInput) }",
               vars)
        .asBool();
  }

  void resetPasswordAsync(std::string_view token, std::string_view newPassword,
                          std::function<void(graphql::GraphQLOutcome, bool)> cb) const {
    graphql::JVal vars;
    vars["resetPasswordInput"]["token"] = token;
    vars["resetPasswordInput"]["newPassword"] = newPassword;
    execUnwrapAsync(
        "mutation ResetPassword($resetPasswordInput: ResetPasswordInput!) {"
        " resetPassword(resetPasswordInput: $resetPasswordInput) }",
        vars, {}, [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
          bool value = false;
          if (out.ok()) value = out.data.asBool();
          cb(std::move(out), value);
        });
  }

  bool resendConfirmationEmail(std::string_view email) const {
    graphql::JVal vars;
    vars["email"] = email;
    return execUnwrap(
               "mutation ResendConfirmationEmail($email: String!) {"
               " resendConfirmationEmail(email: $email) }",
               vars)
        .asBool();
  }

  void resendConfirmationEmailAsync(std::string_view email,
                                    std::function<void(graphql::GraphQLOutcome, bool)> cb) const {
    graphql::JVal vars;
    vars["email"] = email;
    execUnwrapAsync(
        "mutation ResendConfirmationEmail($email: String!) {"
        " resendConfirmationEmail(email: $email) }",
        vars, {}, [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
          bool value = false;
          if (out.ok()) value = out.data.asBool();
          cb(std::move(out), value);
        });
  }

  bool changePassword(std::string_view currentPassword, std::string_view newPassword) const {
    graphql::JVal vars;
    vars["currentPassword"] = currentPassword;
    vars["newPassword"] = newPassword;
    return execUnwrap(
               "mutation ChangePassword($currentPassword: String!, $newPassword: String!) {"
               " changePassword(currentPassword: $currentPassword, newPassword: $newPassword) }",
               vars)
        .asBool();
  }

  void changePasswordAsync(std::string_view currentPassword, std::string_view newPassword,
                           std::function<void(graphql::GraphQLOutcome, bool)> cb) const {
    graphql::JVal vars;
    vars["currentPassword"] = currentPassword;
    vars["newPassword"] = newPassword;
    execUnwrapAsync(
        "mutation ChangePassword($currentPassword: String!, $newPassword: String!) {"
        " changePassword(currentPassword: $currentPassword, newPassword: $newPassword) }",
        vars, {}, [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
          bool value = false;
          if (out.ok()) value = out.data.asBool();
          cb(std::move(out), value);
        });
  }

  /// Adds a password to a signed-in account that has none.
  ///
  /// Distinct from `changePassword`, which verifies a current password and so
  /// cannot serve an account created by magic link or a social provider. The
  /// session is the proof of account control, so the password works
  /// immediately. Refuses with CONFLICT when a password already exists, which
  /// is the case for `changePassword`.
  bool setInitialPassword(std::string_view newPassword) const {
    graphql::JVal vars;
    vars["newPassword"] = newPassword;
    return execUnwrap(
               "mutation SetInitialPassword($newPassword: String!) {"
               " setInitialPassword(newPassword: $newPassword) }",
               vars)
        .asBool();
  }

  void setInitialPasswordAsync(std::string_view newPassword,
                               std::function<void(graphql::GraphQLOutcome, bool)> cb) const {
    graphql::JVal vars;
    vars["newPassword"] = newPassword;
    execUnwrapAsync(
        "mutation SetInitialPassword($newPassword: String!) {"
        " setInitialPassword(newPassword: $newPassword) }",
        vars, {}, [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
          bool value = false;
          if (out.ok()) value = out.data.asBool();
          cb(std::move(out), value);
        });
  }

  /// Single-device logout; clears the stored token.
  bool logout() const {
    const graphql::Json result = execUnwrap(gen::auth::kLogoutDocument);
    const bool ok = result.asBool();
    if (result.ok()) auth_->clearToken();
    return ok;
  }

  void logoutAsync(std::function<void(graphql::GraphQLOutcome, bool)> cb) const {
    execUnwrapAsync(gen::auth::kLogoutDocument, graphql::JVal(), {},
                    [auth = auth_, cb = std::move(cb)](
                        graphql::GraphQLOutcome out) mutable {
                      bool ok = false;
                      if (out.ok()) {
                        ok = out.data.asBool();
                        auth->clearToken();
                      }
                      cb(std::move(out), ok);
                    });
  }

  /// Revoke every active session for the user.
  bool logoutAllDevices() const {
    return execUnwrap(gen::auth::kLogoutAllDevicesDocument).asBool();
  }

  void logoutAllDevicesAsync(std::function<void(graphql::GraphQLOutcome, bool)> cb) const {
    execUnwrapAsync(gen::auth::kLogoutAllDevicesDocument, graphql::JVal(), {},
                    [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
                      bool value = false;
                      if (out.ok()) value = out.data.asBool();
                      cb(std::move(out), value);
                    });
  }

  void setToken(const std::string& token) const { auth_->setToken(token); }
  std::string getToken() const { return auth_->token(); }

 private:
  std::shared_ptr<graphql::AuthState> auth_;
};

}  // namespace crowdy::domains
