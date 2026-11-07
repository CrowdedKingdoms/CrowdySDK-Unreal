#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/domains/types.hpp"
#include "crowdy/generated/enums.hpp"
#include "crowdy/generated/operations.hpp"

/// A player's own safety controls in one app — client.safety() (ck-api v2.60.0), mirroring
/// CrowdyJS's `client.safety`: who hears and sees the player, contacts, blocks, a parent's
/// consent to voice or video, and reports to the app's moderators. Game plane: call it on the
/// per-game client with the app token for the app (crowdy::startGameSession builds one);
/// an identity session token is refused.
///
/// The realtime servers enforce all of it, not the client: the scopes on sending and receiving,
/// the minor cap, and blocks in both directions. A change reaches the player's live sessions
/// within a second. Show the notice and the indicator the app's safety log needs
/// (admin().apps().recording(appId)).
namespace crowdy::domains {

/// What the player picked for each medium, before policy caps it; unset is the default,
/// EVERYONE.
struct CommChoices {
  std::optional<gen::CommScope> voiceSend;
  std::optional<gen::CommScope> voiceReceive;
  std::optional<gen::CommScope> videoSend;
  std::optional<gen::CommScope> videoReceive;
};

/// A player's voice and video settings in one app. The four scopes are the EFFECTIVE ones the
/// realtime servers enforce: the player's choice, never more than policy allows. A player under
/// 18 (`isMinor`) has each medium OFF until a parent consents to it, then at most CONTACTS.
struct CommSettings {
  std::string appId;
  bool isMinor = false;
  gen::CommScope voiceSend = gen::CommScope::OFF;
  gen::CommScope voiceReceive = gen::CommScope::OFF;
  gen::CommScope videoSend = gen::CommScope::OFF;
  gen::CommScope videoReceive = gen::CommScope::OFF;
  CommChoices choices;
  /// The most each medium may be now: EVERYONE for an adult; for a minor, OFF until a parent
  /// consents, then CONTACTS.
  gen::CommScope voiceAllowed = gen::CommScope::OFF;
  gen::CommScope videoAllowed = gen::CommScope::OFF;
  bool voiceConsent = false;
  bool videoConsent = false;
  /// When the settings were last written; null before the player's first token for the app.
  NullableString updatedAt;

  /// A scope this SDK does not know (from a newer API) reads as OFF.
  static CommSettings fromJson(const graphql::Json& j) {
    CommSettings s;
    s.appId = j["appId"].asBigIntString();
    s.isMinor = j["isMinor"].asBool();
    s.voiceSend = scope(j["voiceSend"]);
    s.voiceReceive = scope(j["voiceReceive"]);
    s.videoSend = scope(j["videoSend"]);
    s.videoReceive = scope(j["videoReceive"]);
    const graphql::Json choices = j["choices"];
    s.choices.voiceSend = choice(choices["voiceSend"]);
    s.choices.voiceReceive = choice(choices["voiceReceive"]);
    s.choices.videoSend = choice(choices["videoSend"]);
    s.choices.videoReceive = choice(choices["videoReceive"]);
    s.voiceAllowed = scope(j["voiceAllowed"]);
    s.videoAllowed = scope(j["videoAllowed"]);
    s.voiceConsent = j["voiceConsent"].asBool();
    s.videoConsent = j["videoConsent"].asBool();
    s.updatedAt = NullableString::fromJson(j["updatedAt"]);
    return s;
  }

 private:
  static gen::CommScope scope(const graphql::Json& v) {
    return gen::commScopeFromString(v.asStringView()).value_or(gen::CommScope::OFF);
  }
  static std::optional<gen::CommScope> choice(const graphql::Json& v) {
    if (!v.ok() || v.isNull()) return std::nullopt;
    return gen::commScopeFromString(v.asStringView());
  }
};

/// SafetyAPI::setCommSettings' choices: a field left unset keeps its current choice. A choice
/// above what policy allows is kept and takes effect only up to the cap, so a player can only
/// ever lower what it sends and receives.
struct CommChoicesInput {
  std::optional<gen::CommScope> voiceSend;
  std::optional<gen::CommScope> voiceReceive;
  std::optional<gen::CommScope> videoSend;
  std::optional<gen::CommScope> videoReceive;
};

/// Another player in the caller's contacts for the app. Contacts are mutual: an accepted
/// contact is in both players' lists, and it is what a CONTACTS scope admits.
struct PlayerContact {
  std::string userId;
  gen::ContactState state = gen::ContactState::PENDING;
  std::string createdAt;
  NullableString acceptedAt;  ///< null while pending

  static PlayerContact fromJson(const graphql::Json& j) {
    PlayerContact c;
    c.userId = j["userId"].asBigIntString();
    c.state = gen::contactStateFromString(j["state"].asStringView())
                  .value_or(gen::ContactState::PENDING);
    c.createdAt = j["createdAt"].asString();
    c.acceptedAt = NullableString::fromJson(j["acceptedAt"]);
    return c;
  }
};

/// A contact request waiting for an answer: INCOMING ones another player sent the caller
/// (acceptContact or removeContact answers it), OUTGOING ones the caller sent (removeContact
/// cancels it).
struct ContactRequest {
  std::string userId;
  gen::ContactRequestDirection direction = gen::ContactRequestDirection::INCOMING;
  std::string createdAt;

  static ContactRequest fromJson(const graphql::Json& j) {
    ContactRequest r;
    r.userId = j["userId"].asBigIntString();
    r.direction = gen::contactRequestDirectionFromString(j["direction"].asStringView())
                      .value_or(gen::ContactRequestDirection::INCOMING);
    r.createdAt = j["createdAt"].asString();
    return r;
  }
};

/// A player the caller blocked in the app: nothing passes between the two, either way, while
/// the block stands, and the blocked player is not told.
struct PlayerBlock {
  std::string userId;
  std::string createdAt;

  static PlayerBlock fromJson(const graphql::Json& j) {
    PlayerBlock b;
    b.userId = j["userId"].asBigIntString();
    b.createdAt = j["createdAt"].asString();
    return b;
  }
};

/// The answer to requestParentalConsent: REQUESTED (the consent service emailed the parent),
/// ALREADY_GRANTED or NOT_REQUIRED (the player is 18 or over); nothing is sent for the last
/// two.
struct ParentalConsentRequest {
  std::string appId;
  gen::ParentalConsentFeature feature = gen::ParentalConsentFeature::VOICE;
  gen::ParentalConsentRequestStatus status = gen::ParentalConsentRequestStatus::REQUESTED;

  static ParentalConsentRequest fromJson(const graphql::Json& j) {
    ParentalConsentRequest r;
    r.appId = j["appId"].asBigIntString();
    r.feature = gen::parentalConsentFeatureFromString(j["feature"].asStringView())
                    .value_or(gen::ParentalConsentFeature::VOICE);
    r.status = gen::parentalConsentRequestStatusFromString(j["status"].asStringView())
                   .value_or(gen::ParentalConsentRequestStatus::REQUESTED);
    return r;
  }
};

/// A report for SafetyAPI::fileReport. It covers both players' recorded sessions from 15 minutes
/// before `occurredAt` to its filing; the reported player is not told.
struct SafetyReportFiling {
  std::string appId;
  std::string reportedUserId;
  /// The reported player's 32-character actor id, when the client knows it (hex, as
  /// core::toString(ActorUuid) writes it).
  std::optional<std::string> reportedActorUuid;
  gen::SafetyReportReason reason = gen::SafetyReportReason::OTHER;
  /// The reporter's own words, at most 1,000 characters.
  std::optional<std::string> details;
  /// When it happened (ISO-8601, within the past year).
  std::string occurredAt;
};

/// A filed report, as its reporter sees it.
struct SafetyReportReceipt {
  std::string reportId;
  std::string filedAt;

  static SafetyReportReceipt fromJson(const graphql::Json& j) {
    SafetyReportReceipt r;
    r.reportId = j["reportId"].asBigIntString();
    r.filedAt = j["filedAt"].asString();
    return r;
  }
};

class SafetyAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  // ----- Who hears and sees the player ------------------------------------------

  /// The player's voice and video settings in the app: the effective scopes, what the player
  /// picked (unset where it kept the default), what policy allows now, whether a parent
  /// consented, and isMinor.
  CommSettings commSettings(std::string_view appId) const {
    return CommSettings::fromJson(run(gen::safety::kMyCommSettingsOperationName, app(appId)));
  }
  void commSettingsAsync(std::string_view appId,
                         std::function<void(graphql::GraphQLOutcome, CommSettings)> cb) const {
    runAsync(gen::safety::kMyCommSettingsOperationName, app(appId), std::move(cb),
             &CommSettings::fromJson);
  }

  /// Change the player's choices; a field left unset keeps its choice. Answers the new
  /// settings.
  CommSettings setCommSettings(std::string_view appId, const CommChoicesInput& input) const {
    return CommSettings::fromJson(
        run(gen::safety::kSetMyCommSettingsOperationName, settingsVars(appId, input)));
  }
  void setCommSettingsAsync(std::string_view appId, const CommChoicesInput& input,
                            std::function<void(graphql::GraphQLOutcome, CommSettings)> cb) const {
    runAsync(gen::safety::kSetMyCommSettingsOperationName, settingsVars(appId, input),
             std::move(cb), &CommSettings::fromJson);
  }

  /// For a player under 18: ask a parent, through the consent service, to allow one medium in
  /// this app. The service emails `parentEmail`, which Crowded Kingdoms does not store; once
  /// the parent consents, the medium opens up to CONTACTS. PARENTAL_CONSENT_UNAVAILABLE
  /// (graphql::parentalConsentUnavailableOf, whose `reason` says whether the deployment has no
  /// consent service or the service refused or did not answer); RATE_LIMITED after three
  /// requests in an hour.
  ParentalConsentRequest requestParentalConsent(std::string_view appId,
                                                gen::ParentalConsentFeature feature,
                                                std::string_view parentEmail) const {
    return ParentalConsentRequest::fromJson(run(gen::safety::kRequestParentalConsentOperationName,
                                                consentVars(appId, feature, parentEmail)));
  }
  void requestParentalConsentAsync(
      std::string_view appId, gen::ParentalConsentFeature feature, std::string_view parentEmail,
      std::function<void(graphql::GraphQLOutcome, ParentalConsentRequest)> cb) const {
    runAsync(gen::safety::kRequestParentalConsentOperationName,
             consentVars(appId, feature, parentEmail), std::move(cb),
             &ParentalConsentRequest::fromJson);
  }

  // ----- Contacts ----------------------------------------------------------------

  /// The player's accepted contacts in the app, most recent first (at most 1,000).
  std::vector<PlayerContact> contacts(std::string_view appId) const {
    return listOf<PlayerContact>(run(gen::safety::kMyContactsOperationName, app(appId)));
  }
  void contactsAsync(std::string_view appId,
                     std::function<void(graphql::GraphQLOutcome, std::vector<PlayerContact>)> cb)
      const {
    runAsync(gen::safety::kMyContactsOperationName, app(appId), std::move(cb),
             &listOf<PlayerContact>);
  }

  /// Requests waiting in the app: INCOMING ones first, then OUTGOING, newest first.
  std::vector<ContactRequest> contactRequests(std::string_view appId) const {
    return listOf<ContactRequest>(run(gen::safety::kContactRequestsOperationName, app(appId)));
  }
  void contactRequestsAsync(
      std::string_view appId,
      std::function<void(graphql::GraphQLOutcome, std::vector<ContactRequest>)> cb) const {
    runAsync(gen::safety::kContactRequestsOperationName, app(appId), std::move(cb),
             &listOf<ContactRequest>);
  }

  /// Ask another player of the app to be a contact. If they already asked the caller, both are
  /// contacts at once (ACCEPTED). Refused while the caller has blocked them;
  /// SOCIAL_LIMIT_REACHED past 100 waiting requests or 1,000 contacts; NOT_FOUND for a player
  /// the app does not know.
  PlayerContact requestContact(std::string_view appId, std::string_view userId) const {
    return PlayerContact::fromJson(
        run(gen::safety::kRequestContactOperationName, pair(appId, userId)));
  }
  void requestContactAsync(std::string_view appId, std::string_view userId,
                           std::function<void(graphql::GraphQLOutcome, PlayerContact)> cb) const {
    runAsync(gen::safety::kRequestContactOperationName, pair(appId, userId), std::move(cb),
             &PlayerContact::fromJson);
  }

  /// Accept another player's request: both become each other's contacts. NOT_FOUND when that
  /// player has not asked.
  PlayerContact acceptContact(std::string_view appId, std::string_view userId) const {
    return PlayerContact::fromJson(
        run(gen::safety::kAcceptContactOperationName, pair(appId, userId)));
  }
  void acceptContactAsync(std::string_view appId, std::string_view userId,
                          std::function<void(graphql::GraphQLOutcome, PlayerContact)> cb) const {
    runAsync(gen::safety::kAcceptContactOperationName, pair(appId, userId), std::move(cb),
             &PlayerContact::fromJson);
  }

  /// Remove a contact, decline the other player's request, or cancel the caller's own. False
  /// when there was nothing.
  bool removeContact(std::string_view appId, std::string_view userId) const {
    return run(gen::safety::kRemoveContactOperationName, pair(appId, userId)).asBool();
  }
  void removeContactAsync(std::string_view appId, std::string_view userId,
                          std::function<void(graphql::GraphQLOutcome, bool)> cb) const {
    runAsync(gen::safety::kRemoveContactOperationName, pair(appId, userId), std::move(cb),
             &asBool);
  }

  // ----- Blocks ------------------------------------------------------------------

  /// The players the player blocked in the app, most recent first (at most 1,000).
  std::vector<PlayerBlock> blocks(std::string_view appId) const {
    return listOf<PlayerBlock>(run(gen::safety::kMyBlocksOperationName, app(appId)));
  }
  void blocksAsync(std::string_view appId,
                   std::function<void(graphql::GraphQLOutcome, std::vector<PlayerBlock>)> cb)
      const {
    runAsync(gen::safety::kMyBlocksOperationName, app(appId), std::move(cb),
             &listOf<PlayerBlock>);
  }

  /// Block another player of the app: nothing of any kind passes between the two, either way,
  /// and any contact or request between them goes. Blocking again changes nothing;
  /// SOCIAL_LIMIT_REACHED past 1,000 blocks.
  PlayerBlock block(std::string_view appId, std::string_view userId) const {
    return PlayerBlock::fromJson(run(gen::safety::kBlockPlayerOperationName, pair(appId, userId)));
  }
  void blockAsync(std::string_view appId, std::string_view userId,
                  std::function<void(graphql::GraphQLOutcome, PlayerBlock)> cb) const {
    runAsync(gen::safety::kBlockPlayerOperationName, pair(appId, userId), std::move(cb),
             &PlayerBlock::fromJson);
  }

  /// Lift a block the caller set; a contact the block removed does not come back. False when
  /// there was no such block.
  bool unblock(std::string_view appId, std::string_view userId) const {
    return run(gen::safety::kUnblockPlayerOperationName, pair(appId, userId)).asBool();
  }
  void unblockAsync(std::string_view appId, std::string_view userId,
                    std::function<void(graphql::GraphQLOutcome, bool)> cb) const {
    runAsync(gen::safety::kUnblockPlayerOperationName, pair(appId, userId), std::move(cb),
             &asBool);
  }

  // ----- Reports ---------------------------------------------------------------

  /// Report another player of the app to its moderators (client.moderation()). BAD_USER_INPUT
  /// for yourself, details over 1,000 characters or an occurredAt outside the past year;
  /// RATE_LIMITED after 20 reports in an hour; NOT_FOUND for a player the app does not know.
  SafetyReportReceipt fileReport(const SafetyReportFiling& report) const {
    return SafetyReportReceipt::fromJson(
        run(gen::safety::kFileSafetyReportOperationName, reportVars(report)));
  }
  void fileReportAsync(const SafetyReportFiling& report,
                       std::function<void(graphql::GraphQLOutcome, SafetyReportReceipt)> cb)
      const {
    runAsync(gen::safety::kFileSafetyReportOperationName, reportVars(report), std::move(cb),
             &SafetyReportReceipt::fromJson);
  }

 private:
  graphql::Json run(std::string_view operation, const graphql::JVal& vars) const {
    return execUnwrap(gen::safety::documentFor(operation), vars, operation);
  }

  template <typename T, typename Parse>
  void runAsync(std::string_view operation, const graphql::JVal& vars,
                std::function<void(graphql::GraphQLOutcome, T)> cb, Parse parse) const {
    execUnwrapAsync(gen::safety::documentFor(operation), vars, operation,
                    [cb = std::move(cb), parse](graphql::GraphQLOutcome out) mutable {
                      T value{};
                      if (out.ok()) value = parse(out.data);
                      cb(std::move(out), std::move(value));
                    });
  }

  template <typename T>
  static std::vector<T> listOf(const graphql::Json& j) {
    std::vector<T> out;
    j.forEach([&](graphql::Json entry) { out.push_back(T::fromJson(entry)); });
    return out;
  }

  static bool asBool(const graphql::Json& j) { return j.asBool(); }

  static graphql::JVal app(std::string_view appId) {
    graphql::JVal vars;
    vars["appId"] = appId;
    return vars;
  }

  static graphql::JVal pair(std::string_view appId, std::string_view userId) {
    graphql::JVal vars = app(appId);
    vars["userId"] = userId;
    return vars;
  }

  static graphql::JVal settingsVars(std::string_view appId, const CommChoicesInput& input) {
    graphql::JVal vars = app(appId);
    graphql::JVal& body = vars["input"];
    body = graphql::JVal(graphql::JObject{});
    if (input.voiceSend) body["voiceSend"] = gen::toString(*input.voiceSend);
    if (input.voiceReceive) body["voiceReceive"] = gen::toString(*input.voiceReceive);
    if (input.videoSend) body["videoSend"] = gen::toString(*input.videoSend);
    if (input.videoReceive) body["videoReceive"] = gen::toString(*input.videoReceive);
    return vars;
  }

  static graphql::JVal consentVars(std::string_view appId, gen::ParentalConsentFeature feature,
                                   std::string_view parentEmail) {
    graphql::JVal vars = app(appId);
    vars["feature"] = gen::toString(feature);
    vars["parentEmail"] = parentEmail;
    return vars;
  }

  static graphql::JVal reportVars(const SafetyReportFiling& report) {
    graphql::JVal vars;
    graphql::JVal& body = vars["input"];
    body["appId"] = report.appId;
    body["reportedUserId"] = report.reportedUserId;
    if (report.reportedActorUuid) body["reportedActorUuid"] = *report.reportedActorUuid;
    body["reason"] = gen::toString(report.reason);
    if (report.details) body["details"] = *report.details;
    body["occurredAt"] = report.occurredAt;
    return vars;
  }
};

}  // namespace crowdy::domains
