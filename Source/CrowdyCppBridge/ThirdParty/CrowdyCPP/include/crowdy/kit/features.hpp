#pragma once

#include "crowdy/kit/core.hpp"

/// Features — runtime monetization helpers: the app feature/tier surface in
/// shop terms. Features are string keys ("vip", "land_owner", "battle_pass")
/// defined per app and granted to ACCESS TIERS (the management-side
/// appAccess tiers players buy/hold); any model function whose policy
/// carries a tier_feature leaf then admits only players on a granted tier.
/// Gate blueprint functions by composing gate() (= featureGate) into a
/// builder's policy-extra composition point. Mirrors CrowdyJS's
/// kit.features. There is no blueprint half — this kit is runtime-only.
namespace crowdy::kit {

/// All methods require the app-admin manage_apps permission.
class FeaturesKit {
 public:
  FeaturesKit(std::string appId, domains::GameModelAPI& gameModel)
      : appId_(std::move(appId)), gameModel_(gameModel) {}

  /// Define (or re-describe) a feature key for the app.
  Json define(std::string_view featureKey, std::string_view description = {}) {
    JVal input;
    input["appId"] = appId_;
    input["featureKey"] = featureKey;
    if (!description.empty()) input["description"] = description;
    return gameModel_.defineFeature(input);
  }

  /// List the app's defined feature keys.
  Json list() { return gameModel_.features(appId_); }

  /// Grant a feature to an access tier (its holders pass tier_feature
  /// checks).
  Json grantToTier(std::string_view tierId, std::string_view featureKey) {
    JVal input;
    input["appId"] = appId_;
    input["tierId"] = tierId;
    input["featureKey"] = featureKey;
    return gameModel_.grantTierFeature(input);
  }

  /// Revoke a feature from an access tier.
  bool revokeFromTier(std::string_view tierId, std::string_view featureKey) {
    JVal input;
    input["appId"] = appId_;
    input["tierId"] = tierId;
    input["featureKey"] = featureKey;
    return gameModel_.revokeTierFeature(input).asBool();
  }

  /// List tier->feature grants (optionally for one tier).
  Json tierFeatures(std::string_view tierId = {}) {
    return gameModel_.tierFeatures(appId_, tierId);
  }

  /// A tier_feature policy leaf for `feature` — AND it into any blueprint
  /// function's policy (via andPolicies) to monetization-gate that function.
  /// (Also available standalone as featureGate.)
  JVal gate(std::string_view feature) const { return featureGate(feature); }

 private:
  std::string appId_;
  domains::GameModelAPI& gameModel_;
};

}  // namespace crowdy::kit
