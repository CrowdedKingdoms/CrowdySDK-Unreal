#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/domains/types.hpp"
#include "crowdy/generated/enums.hpp"
#include "crowdy/generated/operations.hpp"

/// Authentication and account lifecycle — client.auth(). Passwordless:
/// magic link, social/OIDC, or (dev only) the dev bypass. Every path returns
/// an identity SESSION token, stored on the shared auth state automatically.
/// Gameplay tokens are minted separately via client.portal().
namespace crowdy::domains {

/// registerUser()'s full input (ck-api v2.60.0 adds the age and the country). Unset optional
/// fields and empty strings are not sent.
struct RegisterUserInput {
  std::string email;
  std::string password;
  std::string gamertag;
  /// The player agreed to the platform's current player documents (playerLegalDocuments).
  std::optional<bool> acceptLegal;
  /// The age-of-majority attestation, while the platform still uses it; ignored once it asks
  /// for a birth date.
  std::optional<bool> attestAgeOfMajority;
  /// Birth date, YYYY-MM-DD (YYYY-MM is accepted too); only the year and month are kept. Sent
  /// with `country`. A native client may leave both out and give them later with
  /// recordPlayerAge().
  std::string birthDate;
  /// The country the player lives in, ISO 3166-1 alpha-2 (such as US).
  std::string country;
  /// Under 18: the player confirms a parent or guardian agreed. Required true under 18;
  /// ignored at 18 or over.
  std::optional<bool> parentalConfirmation;
};

/// A birth date, country and parental agreement, as recordPlayerAge() takes them.
struct PlayerAgeInput {
  /// YYYY-MM-DD (YYYY-MM is accepted too); only the year and month are kept, and the age is
  /// counted from the last day of that month.
  std::string birthDate;
  /// The country the player lives in, ISO 3166-1 alpha-2 (such as US).
  std::string country;
  /// Under 18: the player confirms a parent or guardian agreed. Required true under 18;
  /// ignored at 18 or over.
  std::optional<bool> parentalConfirmation;
};

/// recordPlayerConsents()'s full input (ck-api v2.60.0).
struct PlayerConsents {
  /// Must be true: the player agreed to the documents shown.
  bool acceptLegal = false;
  /// Required true while the platform uses the age-of-majority attestation; ignored once
  /// players give a birth date. Unset sends nothing.
  std::optional<bool> attestAgeOfMajority;
  /// The app whose own documents (appRequiredDocuments) the player also accepted; empty for
  /// the platform's alone.
  std::string appId;
  /// How the player accepted: CLICKWRAP (the server's default) on a Crowded Kingdoms page or
  /// an SDK form, IN_GAME on the app's own screen.
  std::optional<gen::LegalAcceptanceMethod> method;
};

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

  // ----- The terms and age gate (ck-api v2.35.0) -------------------------------
  // A gameplay token (mintAppToken, the portal code, refresh) is refused with
  // LEGAL_ACCEPTANCE_REQUIRED until the player has the current required documents (Game
  // Terms, API Terms, SDK Developer Terms, Free Tier and Billing Basis, Overworld Privacy
  // Policy) and the age-of-majority attestation stored. recordPlayerConsents records the
  // PLAYER's agreement: call it only after they ticked both boxes in your own UI.

  /// registerUser with the clickwrap. A request from a browser must send both true, or it
  /// is refused with LEGAL_ACCEPTANCE_REQUIRED before any account exists; the account then
  /// starts accepted. A native client may register without them and call
  /// recordPlayerConsents() before its first gameplay token.
  AuthResponse registerUser(std::string_view email, std::string_view password,
                            std::string_view gamertag, bool acceptLegal,
                            bool attestAgeOfMajority) const {
    graphql::JVal vars;
    vars["registerUserInput"]["email"] = email;
    vars["registerUserInput"]["password"] = password;
    if (!gamertag.empty()) vars["registerUserInput"]["gamertag"] = gamertag;
    vars["registerUserInput"]["acceptLegal"] = acceptLegal;
    vars["registerUserInput"]["attestAgeOfMajority"] = attestAgeOfMajority;
    auto r = AuthResponse::fromJson(execUnwrap(
        "mutation Register($registerUserInput: RegisterUserInput!) {"
        " register(registerUserInput: $registerUserInput) {"
        " token gameTokenId user { userId email gamertag } } }",
        vars));
    if (!r.token.empty()) auth_->setToken(r.token);
    return r;
  }

  /// Store the signed-in player's agreement to the current required legal documents and
  /// their attestation that they are at least 18, or the age of majority where they live
  /// if that is higher. Session token; both must be true; repeating it is harmless.
  bool recordPlayerConsents(bool acceptLegal, bool attestAgeOfMajority) const {
    graphql::JVal vars;
    vars["acceptLegal"] = acceptLegal;
    vars["attestAgeOfMajority"] = attestAgeOfMajority;
    return execUnwrap(
               "mutation RecordPlayerConsents($acceptLegal: Boolean!, $attestAgeOfMajority: Boolean!) {"
               " recordPlayerConsents(acceptLegal: $acceptLegal,"
               " attestAgeOfMajority: $attestAgeOfMajority) }",
               vars)
        .asBool();
  }

  void recordPlayerConsentsAsync(bool acceptLegal, bool attestAgeOfMajority,
                                 std::function<void(graphql::GraphQLOutcome, bool)> cb) const {
    graphql::JVal vars;
    vars["acceptLegal"] = acceptLegal;
    vars["attestAgeOfMajority"] = attestAgeOfMajority;
    execUnwrapAsync(
        "mutation RecordPlayerConsents($acceptLegal: Boolean!, $attestAgeOfMajority: Boolean!) {"
        " recordPlayerConsents(acceptLegal: $acceptLegal,"
        " attestAgeOfMajority: $attestAgeOfMajority) }",
        vars, {}, [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
          bool value = false;
          if (out.ok()) value = out.data.asBool();
          cb(std::move(out), value);
        });
  }

  /// Whether the signed-in player has the current required documents and the age
  /// attestation stored, i.e. whether a gameplay token would be issued. Session token.
  bool playerLegalAcceptance() const {
    return execUnwrap("query PlayerLegalAcceptance { playerLegalAcceptance }").asBool();
  }

  void playerLegalAcceptanceAsync(std::function<void(graphql::GraphQLOutcome, bool)> cb) const {
    execUnwrapAsync("query PlayerLegalAcceptance { playerLegalAcceptance }", graphql::JVal(), {},
                    [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
                      bool value = false;
                      if (out.ok()) value = out.data.asBool();
                      cb(std::move(out), value);
                    });
  }

  // ----- Age, territory and per-app documents (ck-api v2.60.0) ------------------
  // The gate on every gameplay mint and refresh checks the player's country (TERRITORY_NOT_
  // SUPPORTED), then its age (AGE_REQUIRED, AGE_REQUIREMENT_NOT_MET), then its documents:
  // the platform's player documents and, for that app, the app's own
  // (LEGAL_ACCEPTANCE_REQUIRED, with the missing ones in extensions.missingDocuments). The
  // graphql::*Of helpers in crowdy/graphql/errors.hpp read each refusal; the countries are
  // client.platform().territoryPolicies().

  /// Sign up with the full input: the documents, and the birth date, the country and (under
  /// 18) the parental confirmation. A browser sign-up without the age is refused AGE_REQUIRED
  /// once the platform admits players under 18; a native client may leave it out and call
  /// recordPlayerAge() before its first gameplay token. An age under the minimum where the
  /// player is, or a country Crowded Kingdoms does not serve, is refused before any account
  /// exists (AGE_REQUIREMENT_NOT_MET, TERRITORY_NOT_SUPPORTED). Since ck-api v2.61.0 the age
  /// refusal names no age, and for 24 hours the same client (address and user agent) is
  /// refused it whatever it sends.
  AuthResponse registerUser(const RegisterUserInput& input) const {
    auto r = AuthResponse::fromJson(execUnwrap(kRegisterDocument, registerVars(input)));
    if (!r.token.empty()) auth_->setToken(r.token);
    return r;
  }

  void registerUserAsync(const RegisterUserInput& input,
                         std::function<void(graphql::GraphQLOutcome, AuthResponse)> cb) const {
    execUnwrapAsync(kRegisterDocument, registerVars(input), {},
                    [auth = auth_, cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
                      AuthResponse value{};
                      if (out.ok()) {
                        value = AuthResponse::fromJson(out.data);
                        if (!value.token.empty()) auth->setToken(value.token);
                      }
                      cb(std::move(out), value);
                    });
  }

  /// Record the signed-in player's birth date, country and (under 18) parental confirmation,
  /// for an account whose age is not known yet (me's ageBracket is "unknown"). Only the year and
  /// month are kept. Returns the account's bracket and country. Ask on a neutral screen (no age
  /// shown, the parental line only for a player under 18). CONFLICT when the account already
  /// has an age; TERRITORY_NOT_SUPPORTED where Crowded Kingdoms does not serve (the declared
  /// country, or the network's country or region); AGE_REQUIREMENT_NOT_MET under the
  /// platform's youngest age. Since ck-api v2.61.0 that refusal names no age, the account is
  /// deleted and erased as UsersAPI::deleteMyAccount does, and for 24 hours the same client
  /// (address and user agent) is refused here and at sign-up whatever it sends: drop the
  /// session and do not ask again. Session token.
  PlayerAge recordPlayerAge(const PlayerAgeInput& input) const {
    return PlayerAge::fromJson(execUnwrap(gen::auth::kRecordPlayerAgeDocument, ageVars(input)));
  }

  void recordPlayerAgeAsync(const PlayerAgeInput& input,
                            std::function<void(graphql::GraphQLOutcome, PlayerAge)> cb) const {
    execUnwrapAsync(gen::auth::kRecordPlayerAgeDocument, ageVars(input), {},
                    [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
                      PlayerAge value{};
                      if (out.ok()) value = PlayerAge::fromJson(out.data);
                      cb(std::move(out), std::move(value));
                    });
  }

  /// Store the player's acceptance of the platform's current player documents and, with
  /// `appId`, of that app's own, recorded with the app and the method. Call it only after the
  /// player accepted them in your own UI. Session token; repeating it is harmless.
  bool recordPlayerConsents(const PlayerConsents& consents) const {
    return execUnwrap(gen::auth::kRecordPlayerConsentsForDocument, consentsVars(consents))
        .asBool();
  }

  void recordPlayerConsentsAsync(const PlayerConsents& consents,
                                 std::function<void(graphql::GraphQLOutcome, bool)> cb) const {
    execUnwrapAsync(gen::auth::kRecordPlayerConsentsForDocument, consentsVars(consents), {},
                    [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
                      bool value = false;
                      if (out.ok()) value = out.data.asBool();
                      cb(std::move(out), value);
                    });
  }

  /// playerLegalAcceptance() for the app about to be played: its own documents count too.
  bool playerLegalAcceptance(std::string_view appId) const {
    return execUnwrap(gen::auth::kPlayerLegalAcceptanceForDocument, appVars(appId)).asBool();
  }

  void playerLegalAcceptanceAsync(std::string_view appId,
                                  std::function<void(graphql::GraphQLOutcome, bool)> cb) const {
    execUnwrapAsync(gen::auth::kPlayerLegalAcceptanceForDocument, appVars(appId), {},
                    [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
                      bool value = false;
                      if (out.ok()) value = out.data.asBool();
                      cb(std::move(out), value);
                    });
  }

  /// The platform's documents every player accepts before playing, at the versions required
  /// now. Show them, with the app's own (appRequiredDocuments), before recordPlayerConsents().
  /// Public.
  std::vector<LegalDocument> playerLegalDocuments() const {
    return LegalDocument::listFromJson(legal(gen::auth::kPlayerLegalDocumentsOperationName));
  }

  void playerLegalDocumentsAsync(
      std::function<void(graphql::GraphQLOutcome, std::vector<LegalDocument>)> cb) const {
    legalAsync(gen::auth::kPlayerLegalDocumentsOperationName, graphql::JVal(), std::move(cb));
  }

  /// An app's own documents (its rules, an addendum), which its players accept on top of the
  /// platform's before their next mint. Empty when the app requires none. Public.
  std::vector<LegalDocument> appRequiredDocuments(std::string_view appId) const {
    return LegalDocument::listFromJson(
        legal(gen::auth::kAppRequiredDocumentsOperationName, appVars(appId)));
  }

  void appRequiredDocumentsAsync(
      std::string_view appId,
      std::function<void(graphql::GraphQLOutcome, std::vector<LegalDocument>)> cb) const {
    legalAsync(gen::auth::kAppRequiredDocumentsOperationName, appVars(appId), std::move(cb));
  }

  /// The documents a developer accepts before creating or administering an organization. Only
  /// an account of 18 or older may; empty while the platform asks developers for nothing beyond
  /// the player documents. Public.
  std::vector<LegalDocument> developerLegalDocuments() const {
    return LegalDocument::listFromJson(legal(gen::auth::kDeveloperLegalDocumentsOperationName));
  }

  void developerLegalDocumentsAsync(
      std::function<void(graphql::GraphQLOutcome, std::vector<LegalDocument>)> cb) const {
    legalAsync(gen::auth::kDeveloperLegalDocumentsOperationName, graphql::JVal(), std::move(cb));
  }

  /// Whether the signed-in account has accepted the current developer documents. Session token.
  bool developerLegalAcceptance() const {
    return legal(gen::auth::kDeveloperLegalAcceptanceOperationName).asBool();
  }

  void developerLegalAcceptanceAsync(std::function<void(graphql::GraphQLOutcome, bool)> cb) const {
    boolAsync(gen::auth::kDeveloperLegalAcceptanceOperationName, graphql::JVal(), std::move(cb));
  }

  /// Store the developer's acceptance of the current developer documents. Adults only: an
  /// account with no age is refused AGE_REQUIRED and one under 18 AGE_REQUIREMENT_NOT_MET.
  /// Session token; idempotent.
  bool recordDeveloperConsents(bool acceptDeveloperTerms) const {
    graphql::JVal vars;
    vars["acceptDeveloperTerms"] = acceptDeveloperTerms;
    return legal(gen::auth::kRecordDeveloperConsentsOperationName, vars).asBool();
  }

  void recordDeveloperConsentsAsync(bool acceptDeveloperTerms,
                                    std::function<void(graphql::GraphQLOutcome, bool)> cb) const {
    graphql::JVal vars;
    vars["acceptDeveloperTerms"] = acceptDeveloperTerms;
    boolAsync(gen::auth::kRecordDeveloperConsentsOperationName, vars, std::move(cb));
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

  /// Email the confirmation link again: the remedy for EMAIL_CONFIRMATION_REQUIRED
  /// (graphql::emailConfirmationRequiredOf, ck-api v2.65.0). Public, from a client that sends no
  /// browser origin; rate-limited. Always true, whether or not the address has an unconfirmed
  /// account, so a true is not evidence an email was sent.
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
  static constexpr const char* kRegisterDocument =
      "mutation Register($registerUserInput: RegisterUserInput!) {"
      " register(registerUserInput: $registerUserInput) {"
      " token gameTokenId user { userId email gamertag } } }";

  static graphql::JVal registerVars(const RegisterUserInput& input) {
    graphql::JVal vars;
    graphql::JVal& body = vars["registerUserInput"];
    body["email"] = input.email;
    body["password"] = input.password;
    if (!input.gamertag.empty()) body["gamertag"] = input.gamertag;
    if (input.acceptLegal) body["acceptLegal"] = *input.acceptLegal;
    if (input.attestAgeOfMajority) body["attestAgeOfMajority"] = *input.attestAgeOfMajority;
    if (!input.birthDate.empty()) body["birthDate"] = input.birthDate;
    if (!input.country.empty()) body["country"] = input.country;
    if (input.parentalConfirmation) body["parentalConfirmation"] = *input.parentalConfirmation;
    return vars;
  }

  static graphql::JVal ageVars(const PlayerAgeInput& input) {
    graphql::JVal vars;
    graphql::JVal& body = vars["input"];
    body["birthDate"] = input.birthDate;
    body["country"] = input.country;
    if (input.parentalConfirmation) body["parentalConfirmation"] = *input.parentalConfirmation;
    return vars;
  }

  static graphql::JVal consentsVars(const PlayerConsents& consents) {
    graphql::JVal vars;
    vars["acceptLegal"] = consents.acceptLegal;
    if (consents.attestAgeOfMajority) vars["attestAgeOfMajority"] = *consents.attestAgeOfMajority;
    if (!consents.appId.empty()) vars["appId"] = consents.appId;
    if (consents.method) vars["method"] = gen::toString(*consents.method);
    return vars;
  }

  static graphql::JVal appVars(std::string_view appId) {
    graphql::JVal vars;
    vars["appId"] = appId;
    return vars;
  }

  graphql::Json legal(std::string_view operation,
                      const graphql::JVal& vars = graphql::JVal()) const {
    return execUnwrap(gen::auth::documentFor(operation), vars, operation);
  }

  void legalAsync(std::string_view operation, const graphql::JVal& vars,
                  std::function<void(graphql::GraphQLOutcome, std::vector<LegalDocument>)> cb) const {
    execUnwrapAsync(gen::auth::documentFor(operation), vars, operation,
                    [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
                      std::vector<LegalDocument> value;
                      if (out.ok()) value = LegalDocument::listFromJson(out.data);
                      cb(std::move(out), std::move(value));
                    });
  }

  void boolAsync(std::string_view operation, const graphql::JVal& vars,
                 std::function<void(graphql::GraphQLOutcome, bool)> cb) const {
    execUnwrapAsync(gen::auth::documentFor(operation), vars, operation,
                    [cb = std::move(cb)](graphql::GraphQLOutcome out) mutable {
                      bool value = false;
                      if (out.ok()) value = out.data.asBool();
                      cb(std::move(out), value);
                    });
  }

  std::shared_ptr<graphql::AuthState> auth_;
};

}  // namespace crowdy::domains
