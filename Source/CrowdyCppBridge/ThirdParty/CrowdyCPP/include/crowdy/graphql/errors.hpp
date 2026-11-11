#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

/// Structured error classes for the GraphQL layer, mirroring CrowdyJS
/// (https://github.com/CrowdedKingdoms/CrowdyJS): branch on `code()` /
/// `extensions.code`, never on message text.
///
/// The replication (UDP) layer never throws — see crowdy/core/result.hpp.
namespace crowdy::graphql {

/// Base class for every SDK error.
class CrowdyError : public std::runtime_error {
 public:
  CrowdyError(std::string code, const std::string& message)
      : std::runtime_error(message), code_(std::move(code)) {}
  /// Stable machine-readable code (e.g. "UNAUTHENTICATED", "HTTP_ERROR").
  const std::string& code() const { return code_; }

 private:
  std::string code_;
};

/// Non-2xx HTTP response from a GraphQL endpoint.
class CrowdyHttpError : public CrowdyError {
 public:
  CrowdyHttpError(int status, std::string body)
      : CrowdyError("HTTP_ERROR", "GraphQL endpoint returned HTTP " + std::to_string(status)),
        status_(status),
        body_(std::move(body)) {}
  int status() const { return status_; }
  const std::string& body() const { return body_; }

 private:
  int status_;
  std::string body_;
};

/// extensions.code for "this app lives in another datacenter, go there".
inline constexpr std::string_view kWrongDatacenterCode = "WRONG_DATACENTER";
/// extensions.code for "this app's datacenter cannot serve it right now".
/// Carries NO endpoint, deliberately: there is nowhere to move to.
inline constexpr std::string_view kAppUnavailableCode = "APP_UNAVAILABLE";

/// One document a LEGAL_ACCEPTANCE_REQUIRED refusal names (extensions.missingDocuments,
/// ck-api v2.60.0): the player has not accepted this version yet.
struct MissingLegalDocument {
  std::string documentId;
  std::string version;
  std::string appId;  ///< the app whose own document it is; empty for a platform document
};

/// One entry of a GraphQL `errors` array.
struct GraphQLErrorDetail {
  std::string message;
  std::string code;         ///< extensions.code (stable, enumerated)
  std::string remediation;  ///< extensions.remediation when provided
  std::string path;         ///< dotted path, e.g. "mintAppToken"

  // Datacenter-routing extensions. Present on WRONG_DATACENTER and
  // APP_UNAVAILABLE; empty elsewhere.
  std::string appId;
  std::string appDatacenter;  ///< where the app IS served
  std::string servedBy;       ///< the datacenter that refused
  std::string gameApiUrl;     ///< move target; only on WRONG_DATACENTER
  std::string gameApiWsUrl;
  /// extensions.retryable. Defaults TRUE to match the server contract, where
  /// only an explicit `false` means "do not bother trying again".
  bool retryable = true;
  /// extensions.blame: "PLATFORM" (ours, a retry is reasonable), "AUTHOR" (the
  /// app's own code, an identical call fails identically) or "BUDGET" (a spent
  /// allowance). Empty when the error carried no attribution, which is NOT the
  /// same as PLATFORM: pair it with `retryable` rather than reading either
  /// alone, since `retryable` defaults true and an unattributed transport
  /// failure must not read as a licensed retry.
  std::string blame;
  /// extensions.retryAfterMs: how long to wait before trying again, in
  /// milliseconds measured when the server built the refusal.
  ///
  /// OPTIONAL rather than an int defaulting to 0, and the `retryable` default
  /// above must NOT be copied here: "retry immediately" and "the server named
  /// no wait" are different instructions, and a zero standing for both would
  /// turn silence into a busy loop. Absent whenever the key is missing or is
  /// not a JSON number.
  ///
  /// On the invoke rate limit this is what REMAINS of a fixed window, not a
  /// fixed backoff, so a later refusal in the same window carries a smaller
  /// number. Treat it as a deadline from receipt, not as an interval to
  /// reuse.
  std::optional<std::int64_t> retryAfterMs;
  /// extensions.httpStatus: the HTTP status the server maps this code to, which a
  /// GraphQL error carries even when the response itself was a 200. Absent when the
  /// key is missing or not a JSON number.
  std::optional<int> httpStatus;
  /// extensions.cause: why an open circuit opened, when the server knows.
  /// `watchdog_timeout` means the failures were watchdog kills. Empty when
  /// the key is absent.
  std::string cause;
  /// extensions.ownedByCaller on ACTOR_EXISTS: true when the taken uuid is already the
  /// caller's actor (a retried create), false when another player holds it. Absent
  /// when the server did not say.
  std::optional<bool> ownedByCaller;
  /// extensions.suspendedUntil (ISO-8601) on ACCESS_SUSPENDED; empty elsewhere.
  std::string suspendedUntil;
  /// extensions.blockedUntil (ISO-8601) on ACCOUNT_BLOCKED for a timed block; empty
  /// elsewhere, and while a block lasts until lifted.
  std::string blockedUntil;
  /// extensions.reason on APP_PAUSED (why the app's runtime gate is not ACTIVE), on
  /// ASSERTION_REJECTED (the check that failed), on MINECRAFT_VERIFICATION_FAILED and on
  /// PARENTAL_CONSENT_UNAVAILABLE (why a parent cannot be asked); empty elsewhere.
  std::string reason;
  /// extensions.country on TERRITORY_NOT_SUPPORTED: the country refused (ISO 3166-1
  /// alpha-2), the one declared or asserted or the network's. Empty elsewhere.
  std::string country;
  /// extensions.missingDocuments on LEGAL_ACCEPTANCE_REQUIRED: the documents still to accept.
  /// Empty elsewhere, and from an API older than v2.60.0.
  std::vector<MissingLegalDocument> missingDocuments;
  /// extensions.requiredMicrousd / spendableMicrousd on INPUT_LOG_FUNDS_NEEDED:
  /// the balance turning replay logging on needs, and what the org wallet can
  /// spend, as decimal micro-USD strings. Empty elsewhere.
  std::string requiredMicrousd;
  std::string spendableMicrousd;
};

/// extensions.code for creating an actor whose uuid is taken (see actorExistsOf()).
inline constexpr std::string_view kActorExistsCode = "ACTOR_EXISTS";
/// extensions.code for a player whose access to the app was revoked (HTTP 403).
inline constexpr std::string_view kAccessRevokedCode = "ACCESS_REVOKED";
/// extensions.code for a player suspended from the app until `suspendedUntil` (HTTP 403).
inline constexpr std::string_view kAccessSuspendedCode = "ACCESS_SUSPENDED";
/// extensions.code for a player with no access to a paid or invite-only app (HTTP 403).
inline constexpr std::string_view kAccessNotGrantedCode = "ACCESS_NOT_GRANTED";
/// extensions.code for an app whose runtime gate is not ACTIVE (HTTP 403).
inline constexpr std::string_view kAppPausedCode = "APP_PAUSED";
/// extensions.code for an account a platform operator has blocked (HTTP 403), on every
/// sign-in and every request of a blocked account; see accountBlockedOf().
inline constexpr std::string_view kAccountBlockedCode = "ACCOUNT_BLOCKED";
/// extensions.code for a user a channel or team has banned (HTTP 403), on a join, a
/// request, an add or an invite; see groupMemberBannedOf().
inline constexpr std::string_view kGroupMemberBannedCode = "GROUP_MEMBER_BANNED";
/// extensions.code for something an org-managed player cannot do (HTTP 403): it has no
/// wallet, and its mods are refused where its organization has not switched them on for the
/// app; see managedPlayerNotAllowedOf().
inline constexpr std::string_view kManagedPlayerNotAllowedCode = "MANAGED_PLAYER_NOT_ALLOWED";
/// extensions.code for a refused org-signed player assertion (HTTP 401), with the failed
/// check in extensions.reason; see assertionRejectedOf().
inline constexpr std::string_view kAssertionRejectedCode = "ASSERTION_REJECTED";
/// extensions.code for "the player's organization has not accepted the current Org-Managed
/// Players Addendum" (HTTP 403), on an org-managed player's sign-in and refresh and on
/// provisioning; see orgLegalAcceptanceRequiredOf().
inline constexpr std::string_view kOrgLegalAcceptanceRequiredCode =
    "ORG_LEGAL_ACCEPTANCE_REQUIRED";
/// extensions.code for an input-log read that cannot be served right now (the log
/// is unreachable); retryable with the same cursor.
inline constexpr std::string_view kInputLogTemporarilyUnavailableCode =
    "INPUT_LOG_TEMPORARILY_UNAVAILABLE";
/// extensions.code for an input-log read that waited out its deadline behind
/// earlier reads (one per user, two per app and per org at a time); retryable.
inline constexpr std::string_view kInputLogRateLimitedCode = "INPUT_LOG_RATE_LIMITED";
/// extensions.code for turning replay logging on without the balance it needs
/// (HTTP 402); see inputLogFundsNeededOf().
inline constexpr std::string_view kInputLogFundsNeededCode = "INPUT_LOG_FUNDS_NEEDED";

// The admission gate (ck-api v2.60.0): every gameplay mint and refresh, sign-up, the
// org-managed sign-ins and the Minecraft sign-in check territory, then age, then documents.
/// extensions.code for a player who has not accepted the current documents (HTTP 403), with
/// what is missing in extensions.missingDocuments; see legalAcceptanceRequiredOf().
inline constexpr std::string_view kLegalAcceptanceRequiredCode = "LEGAL_ACCEPTANCE_REQUIRED";
/// extensions.code for an account or managed player with no age on record (HTTP 403); see
/// ageRequiredOf().
inline constexpr std::string_view kAgeRequiredCode = "AGE_REQUIRED";
/// extensions.code for a player below the age the platform, the app or the country requires
/// (HTTP 403); see ageRequirementNotMetOf().
inline constexpr std::string_view kAgeRequirementNotMetCode = "AGE_REQUIREMENT_NOT_MET";
/// extensions.code for a country Crowded Kingdoms does not serve (HTTP 403), with the country
/// in extensions.country; see territoryNotSupportedOf().
inline constexpr std::string_view kTerritoryNotSupportedCode = "TERRITORY_NOT_SUPPORTED";
/// extensions.code for an app that admits players under 18 without its safety log on (HTTP
/// 403): a minor's mint there, or turning the log off; see safetyLogRequiredOf().
inline constexpr std::string_view kSafetyLogRequiredCode = "SAFETY_LOG_REQUIRED";
/// extensions.code for an app that admits players under 18 while its organization has appointed
/// no safety moderator (HTTP 403, ck-api v2.61.0): a minor's mint there; see
/// safetyModeratorRequiredOf().
inline constexpr std::string_view kSafetyModeratorRequiredCode = "SAFETY_MODERATOR_REQUIRED";
/// extensions.code for a gameplay token refused because a password account made since
/// 2026-11-11 has not confirmed its email (HTTP 403, ck-api v2.65.0); see
/// emailConfirmationRequiredOf().
inline constexpr std::string_view kEmailConfirmationRequiredCode = "EMAIL_CONFIRMATION_REQUIRED";
/// extensions.code for a parental-consent request that cannot be sent right now (HTTP 503), with
/// why in extensions.reason; see parentalConsentUnavailableOf().
inline constexpr std::string_view kParentalConsentUnavailableCode = "PARENTAL_CONSENT_UNAVAILABLE";
/// extensions.code for a contact or block over its limit (HTTP 409): 100 waiting requests,
/// 1,000 contacts or 1,000 blocks per app; see socialLimitReachedOf().
inline constexpr std::string_view kSocialLimitReachedCode = "SOCIAL_LIMIT_REACHED";
/// extensions.code for a Minecraft sign-in Mojang did not confirm (HTTP 401), with the failed
/// check in extensions.reason; see minecraftVerificationFailedOf().
inline constexpr std::string_view kMinecraftVerificationFailedCode =
    "MINECRAFT_VERIFICATION_FAILED";
/// extensions.code for a Minecraft sign-in Mojang did not answer (HTTP 503, retryable); see
/// minecraftVerificationUnavailableOf().
inline constexpr std::string_view kMinecraftVerificationUnavailableCode =
    "MINECRAFT_VERIFICATION_UNAVAILABLE";

/// The first error with `code`, or nullptr.
inline const GraphQLErrorDetail* errorWithCode(const std::vector<GraphQLErrorDetail>& errors,
                                               std::string_view code) {
  for (const auto& error : errors) {
    if (error.code == code) return &error;
  }
  return nullptr;
}

/// An ACTOR_EXISTS refusal: `ownedByCaller` true means the actor is already yours (a
/// retried create; read it rather than failing), false that another player holds the
/// uuid, absent when the server did not say.
struct ActorExists {
  std::optional<bool> ownedByCaller;
};
inline std::optional<ActorExists> actorExistsOf(const std::vector<GraphQLErrorDetail>& errors) {
  const GraphQLErrorDetail* e = errorWithCode(errors, kActorExistsCode);
  if (!e) return std::nullopt;
  return ActorExists{e->ownedByCaller};
}

/// A refusal of the player's own access to an app: ACCESS_REVOKED, ACCESS_SUSPENDED
/// (with `suspendedUntil`, after which access returns by itself) or ACCESS_NOT_GRANTED.
/// None is fixed by retrying; tell the player.
struct AccessRefusal {
  std::string code;
  std::string suspendedUntil;
};
inline std::optional<AccessRefusal> accessRefusalOf(const std::vector<GraphQLErrorDetail>& errors) {
  for (std::string_view code : {kAccessSuspendedCode, kAccessRevokedCode, kAccessNotGrantedCode}) {
    if (const GraphQLErrorDetail* e = errorWithCode(errors, code)) {
      return AccessRefusal{std::string(code), e->suspendedUntil};
    }
  }
  return std::nullopt;
}

/// An APP_PAUSED refusal; `reason` is extensions.reason (empty when none). Show the
/// player that the world is paused instead of retrying in a loop.
struct AppPaused {
  std::string reason;
};
inline std::optional<AppPaused> appPausedOf(const std::vector<GraphQLErrorDetail>& errors) {
  const GraphQLErrorDetail* e = errorWithCode(errors, kAppPausedCode);
  if (!e) return std::nullopt;
  return AppPaused{e->reason};
}

/// An ACCOUNT_BLOCKED refusal: a platform operator blocked the account. `blockedUntil` is
/// extensions.blockedUntil (ISO-8601) for a timed block, which lapses by itself; empty while
/// the block lasts until lifted. Not fixed by retrying; sign out and tell the player.
struct AccountBlocked {
  std::string blockedUntil;
};
inline std::optional<AccountBlocked> accountBlockedOf(const std::vector<GraphQLErrorDetail>& errors) {
  const GraphQLErrorDetail* e = errorWithCode(errors, kAccountBlockedCode);
  if (!e) return std::nullopt;
  return AccountBlocked{e->blockedUntil};
}

/// True when a channel or team refused the call with GROUP_MEMBER_BANNED; only an unban
/// lifts it.
inline bool groupMemberBannedOf(const std::vector<GraphQLErrorDetail>& errors) {
  return errorWithCode(errors, kGroupMemberBannedCode) != nullptr;
}

/// True when an org-managed player was refused with MANAGED_PLAYER_NOT_ALLOWED: a wallet, or a
/// mod, a Crowdy Studio write or the Studio agent in an app whose organization has not switched
/// its managed players' mods on. Not fixed by retrying.
inline bool managedPlayerNotAllowedOf(const std::vector<GraphQLErrorDetail>& errors) {
  return errorWithCode(errors, kManagedPlayerNotAllowedCode) != nullptr;
}

/// An ASSERTION_REJECTED refusal from portal().exchangeManagedPlayerAssertion(). `reason` is
/// the check that failed (extensions.reason: for example `expired`, `replayed`, `signature`,
/// `audience` or `unknown_key`). Resending the same assertion cannot succeed; ask the
/// organization's sign-in service for a new one.
struct AssertionRejected {
  std::string reason;
};
inline std::optional<AssertionRejected> assertionRejectedOf(
    const std::vector<GraphQLErrorDetail>& errors) {
  const GraphQLErrorDetail* e = errorWithCode(errors, kAssertionRejectedCode);
  if (!e) return std::nullopt;
  return AssertionRejected{e->reason};
}

/// True when the player's organization has not accepted the current Org-Managed Players
/// Addendum (ORG_LEGAL_ACCEPTANCE_REQUIRED, ck-api v2.53.0): it signs no player in, provisions
/// none, and its players' tokens do not refresh. Neither the player nor the organization's
/// server can fix it: an admin of the organization accepts it in Studio. Not fixed by
/// retrying. Mirrors CrowdyJS orgLegalAcceptanceRequiredOf.
inline bool orgLegalAcceptanceRequiredOf(const std::vector<GraphQLErrorDetail>& errors) {
  return errorWithCode(errors, kOrgLegalAcceptanceRequiredCode) != nullptr;
}

/// True when an input-log read failed in a way worth repeating with the SAME cursor
/// after a short back-off: INPUT_LOG_TEMPORARILY_UNAVAILABLE or INPUT_LOG_RATE_LIMITED,
/// unless the server marked it `retryable: false`. Bound the retries; past a few in a
/// row, give up. Any other refusal (BAD_USER_INPUT for a malformed cursor or one from
/// another session, NOT_FOUND, FORBIDDEN) fails the same way again.
inline bool inputLogReadRetryable(const std::vector<GraphQLErrorDetail>& errors) {
  for (std::string_view code : {kInputLogTemporarilyUnavailableCode, kInputLogRateLimitedCode}) {
    if (const GraphQLErrorDetail* e = errorWithCode(errors, code)) return e->retryable;
  }
  return false;
}

/// An INPUT_LOG_FUNDS_NEEDED refusal of `replayLoggingEnabled: true`: the wallet
/// balance it needs (what keeping one retention period of the app's recent traffic
/// costs) and what the org wallet can spend, both decimal micro-USD strings.
struct InputLogFundsNeeded {
  std::string requiredMicrousd;
  std::string spendableMicrousd;
};
inline std::optional<InputLogFundsNeeded> inputLogFundsNeededOf(
    const std::vector<GraphQLErrorDetail>& errors) {
  const GraphQLErrorDetail* e = errorWithCode(errors, kInputLogFundsNeededCode);
  if (!e) return std::nullopt;
  return InputLogFundsNeeded{e->requiredMicrousd, e->spendableMicrousd};
}

/// A LEGAL_ACCEPTANCE_REQUIRED refusal: the player still has documents to accept. Show them
/// (AuthAPI::playerLegalDocuments, appRequiredDocuments), record the player's acceptance
/// (AuthAPI::recordPlayerConsents), then ask again. `missingDocuments` names them on ck-api
/// v2.60.0 and later; an older API names none.
struct LegalAcceptanceRequired {
  std::vector<MissingLegalDocument> missingDocuments;
};
inline std::optional<LegalAcceptanceRequired> legalAcceptanceRequiredOf(
    const std::vector<GraphQLErrorDetail>& errors) {
  const GraphQLErrorDetail* e = errorWithCode(errors, kLegalAcceptanceRequiredCode);
  if (!e) return std::nullopt;
  return LegalAcceptanceRequired{e->missingDocuments};
}

/// True when the account or managed player has no age on record (AGE_REQUIRED). A
/// Crowded Kingdoms account gives it with AuthAPI::recordPlayerAge; a managed player's
/// organization sends ageYears when it signs the player in.
inline bool ageRequiredOf(const std::vector<GraphQLErrorDetail>& errors) {
  return errorWithCode(errors, kAgeRequiredCode) != nullptr;
}

/// True when the player is younger than the platform, the app or the country requires
/// (AGE_REQUIREMENT_NOT_MET). Not fixed by retrying, and asking for the age again changes
/// nothing: since ck-api v2.61.0 the same client (address and user agent) is refused at
/// sign-up and AuthAPI::recordPlayerAge for 24 hours whatever it sends, and an org-managed
/// player at its sign-ins; an account that answers recordPlayerAge under 13 is deleted.
/// Deliberately a bool: no refusal names an age. Tell the player they cannot play this,
/// without naming an age or a minimum, and do not offer to enter the date again.
inline bool ageRequirementNotMetOf(const std::vector<GraphQLErrorDetail>& errors) {
  return errorWithCode(errors, kAgeRequirementNotMetCode) != nullptr;
}

/// A TERRITORY_NOT_SUPPORTED refusal: Crowded Kingdoms does not serve `country`
/// (extensions.country), the country declared or asserted or the network's. Not fixed by
/// retrying.
struct TerritoryNotSupported {
  std::string country;
};
inline std::optional<TerritoryNotSupported> territoryNotSupportedOf(
    const std::vector<GraphQLErrorDetail>& errors) {
  const GraphQLErrorDetail* e = errorWithCode(errors, kTerritoryNotSupportedCode);
  if (!e) return std::nullopt;
  return TerritoryNotSupported{e->country};
}

/// True when the app admits players under 18 and its safety log is off (SAFETY_LOG_REQUIRED):
/// a minor's mint there is refused until the app's organization turns it on, and turning it
/// off is refused while the app admits them.
inline bool safetyLogRequiredOf(const std::vector<GraphQLErrorDetail>& errors) {
  return errorWithCode(errors, kSafetyLogRequiredCode) != nullptr;
}

/// True when a player under 18 was refused a token (SAFETY_MODERATOR_REQUIRED, ck-api v2.61.0)
/// in an app that admits players under 18 while the app's organization has appointed no safety
/// moderator (a member whose own role grants moderate_safety; an admin alone does not count).
/// The player cannot fix it: tell them the app cannot admit them yet. The organization appoints
/// one; `hasSafetyModerator` on AppsAPI::get says whether it has, for its members.
inline bool safetyModeratorRequiredOf(const std::vector<GraphQLErrorDetail>& errors) {
  return errorWithCode(errors, kSafetyModeratorRequiredCode) != nullptr;
}

/// True when a gameplay token was refused because a password account made since 2026-11-11 has
/// not confirmed its email (EMAIL_CONFIRMATION_REQUIRED, ck-api v2.65.0): the player gate's last
/// check, at a mint, a portal code or a refresh. Retrying cannot succeed: ask the player to open
/// the emailed link, offer AuthAPI::resendConfirmationEmail, then mint again. Accounts made
/// before then, managed players, staff, and dev or test accounts made with a provisioning token
/// are not asked.
inline bool emailConfirmationRequiredOf(const std::vector<GraphQLErrorDetail>& errors) {
  return errorWithCode(errors, kEmailConfirmationRequiredCode) != nullptr;
}

/// extensions.reason values on PARENTAL_CONSENT_UNAVAILABLE, from the Game API release after
/// v2.61.0. The set is open: the server may add one, so compare against these and treat
/// anything else as unknown.
inline constexpr std::string_view kParentalConsentNotConfigured = "not_configured";
inline constexpr std::string_view kParentalConsentProviderRefused = "provider_refused";
inline constexpr std::string_view kParentalConsentProviderUnavailable = "provider_unavailable";

/// A PARENTAL_CONSENT_UNAVAILABLE refusal of SafetyAPI::requestParentalConsent: a parent cannot
/// be asked right now. `reason` is extensions.reason: `not_configured` (the deployment has no
/// consent service; retrying soon does not help), `provider_refused` or `provider_unavailable`
/// (the service refused or did not answer); empty from an API that does not say (v2.61.0 and
/// older). Voice and video stay off for the player meanwhile.
struct ParentalConsentUnavailable {
  std::string reason;
};
inline std::optional<ParentalConsentUnavailable> parentalConsentUnavailableOf(
    const std::vector<GraphQLErrorDetail>& errors) {
  const GraphQLErrorDetail* e = errorWithCode(errors, kParentalConsentUnavailableCode);
  if (!e) return std::nullopt;
  return ParentalConsentUnavailable{e->reason};
}

/// True when a contact request or a block is over its limit (SOCIAL_LIMIT_REACHED).
inline bool socialLimitReachedOf(const std::vector<GraphQLErrorDetail>& errors) {
  return errorWithCode(errors, kSocialLimitReachedCode) != nullptr;
}

/// A MINECRAFT_VERIFICATION_FAILED refusal of PortalAPI::signInWithMinecraft: Mojang did not
/// confirm the join. `reason` is extensions.reason: `malformed`, `forged`, `expired`,
/// `not_joined` or `replayed`. Start again from minecraftChallenge.
struct MinecraftVerificationFailed {
  std::string reason;
};
inline std::optional<MinecraftVerificationFailed> minecraftVerificationFailedOf(
    const std::vector<GraphQLErrorDetail>& errors) {
  const GraphQLErrorDetail* e = errorWithCode(errors, kMinecraftVerificationFailedCode);
  if (!e) return std::nullopt;
  return MinecraftVerificationFailed{e->reason};
}

/// True when Mojang did not answer a Minecraft sign-in (MINECRAFT_VERIFICATION_UNAVAILABLE).
/// The server id is not used up by it: retry shortly with the same one while it is valid.
inline bool minecraftVerificationUnavailableOf(const std::vector<GraphQLErrorDetail>& errors) {
  return errorWithCode(errors, kMinecraftVerificationUnavailableCode) != nullptr;
}

/// The server returned GraphQL errors. Preserves every error including
/// extensions.
class CrowdyGraphQLError : public CrowdyError {
 public:
  explicit CrowdyGraphQLError(std::vector<GraphQLErrorDetail> errors)
      : CrowdyError(errors.empty() ? "GRAPHQL_ERROR" : firstCode(errors),
                    errors.empty() ? "GraphQL error" : errors.front().message),
        errors_(std::move(errors)) {}
  const std::vector<GraphQLErrorDetail>& errors() const { return errors_; }

 private:
  static std::string firstCode(const std::vector<GraphQLErrorDetail>& errors) {
    return errors.front().code.empty() ? "GRAPHQL_ERROR" : errors.front().code;
  }
  std::vector<GraphQLErrorDetail> errors_;
};

/// The app's own datacenter cannot serve it right now, and there is nowhere to
/// move to — so this is a typed error rather than a redirect. Distinct from
/// WRONG_DATACENTER, which the client handles silently by moving.
///
/// `message` is written for a player to read; show it rather than substituting
/// generic text, because the server knows why and the client does not.
class CrowdyAppUnavailableError : public CrowdyGraphQLError {
 public:
  explicit CrowdyAppUnavailableError(std::vector<GraphQLErrorDetail> errors)
      : CrowdyGraphQLError(std::move(errors)) {}

  const std::string& appId() const { return detail().appId; }
  const std::string& appDatacenter() const { return detail().appDatacenter; }
  const std::string& servedBy() const { return detail().servedBy; }
  /// False only when the server said so explicitly.
  bool retryable() const { return detail().retryable; }

 private:
  const GraphQLErrorDetail& detail() const {
    for (const auto& error : errors()) {
      if (error.code == kAppUnavailableCode) return error;
    }
    return errors().front();
  }
};

/// Network-level failure (DNS, TLS, connection refused).
class CrowdyNetworkError : public CrowdyError {
 public:
  explicit CrowdyNetworkError(const std::string& message)
      : CrowdyError("NETWORK_ERROR", message) {}
};

/// Request timed out.
class CrowdyTimeoutError : public CrowdyError {
 public:
  explicit CrowdyTimeoutError(const std::string& message)
      : CrowdyError("TIMEOUT", message) {}
};

/// The caller cancelled the request through its CancelToken.
class CrowdyCancelledError : public CrowdyError {
 public:
  explicit CrowdyCancelledError(const std::string& message)
      : CrowdyError("CANCELLED", message) {}
};

/// Server response failed validation (not JSON, missing data, wrong shape).
class CrowdyProtocolError : public CrowdyError {
 public:
  explicit CrowdyProtocolError(const std::string& message)
      : CrowdyError("PROTOCOL_ERROR", message) {}
};

}  // namespace crowdy::graphql
