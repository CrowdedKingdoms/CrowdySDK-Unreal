#pragma once

#include <functional>
#include <utility>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

/// client.playerWallet() — the player wallet and player-billing surface
/// (player compute P2, DN-5).
///
/// Viewer-scoped calls operate on the CALLER's own wallet: balance, ledger,
/// hourly usage charges (platform vs studio-markup split), self-set spend
/// caps, auto-recharge config, and per-app gate states. Fund the wallet with
/// a PLAYER_WALLET_TOPUP checkout (payments domain). Studio-scoped calls
/// (policies, markup, per-player usage, accrued markup) require the matching
/// org permissions.
namespace crowdy::domains {

class PlayerWalletAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  /// The caller's wallet, created empty on first access (never null).
  graphql::Json balance() const { return run("PlayerWalletBalance", {}); }
  void balanceAsync(graphql::GraphQLCallback cb) const {
    runAsync("PlayerWalletBalance", {}, std::move(cb));
  }

  /// The caller's wallet ledger, newest first.
  graphql::Json transactions(const graphql::JVal& options = graphql::JVal()) const {
    return run("PlayerWalletTransactions", options);
  }
  void transactionsAsync(const graphql::JVal& options,
                         graphql::GraphQLCallback cb) const {
    runAsync("PlayerWalletTransactions", options, std::move(cb));
  }

  /// The caller's posted hourly usage charges (platform vs markup split).
  graphql::Json charges(const graphql::JVal& options = graphql::JVal()) const {
    return run("PlayerUsageCharges", options);
  }
  void chargesAsync(const graphql::JVal& options,
                    graphql::GraphQLCallback cb) const {
    runAsync("PlayerUsageCharges", options, std::move(cb));
  }

  /// The caller's self-set spend caps with running counters.
  graphql::Json spendCaps() const { return run("PlayerSpendCaps", {}); }
  void spendCapsAsync(graphql::GraphQLCallback cb) const {
    runAsync("PlayerSpendCaps", {}, std::move(cb));
  }

  /// Set (or clear with null limits) a global or per-app self spend cap.
  graphql::Json setSpendCap(std::string_view scope,
                            const graphql::JVal& options = graphql::JVal()) const {
    graphql::JVal vars = options;
    vars["scope"] = scope;
    return run("SetPlayerSpendCap", vars);
  }
  void setSpendCapAsync(std::string_view scope, const graphql::JVal& options,
                        graphql::GraphQLCallback cb) const {
    graphql::JVal vars = options;
    vars["scope"] = scope;
    runAsync("SetPlayerSpendCap", vars, std::move(cb));
  }

  /// The caller's auto-recharge settings.
  graphql::Json autoBilling() const { return run("PlayerAutoBilling", {}); }
  void autoBillingAsync(graphql::GraphQLCallback cb) const {
    runAsync("PlayerAutoBilling", {}, std::move(cb));
  }

  /// Configure auto-recharge (a vaulted payment method is required to enable).
  graphql::Json setAutoBilling(bool enabled,
                               const graphql::JVal& options = graphql::JVal()) const {
    graphql::JVal vars = options;
    vars["enabled"] = enabled;
    return run("SetPlayerAutoBilling", vars);
  }
  void setAutoBillingAsync(bool enabled, const graphql::JVal& options,
                           graphql::GraphQLCallback cb) const {
    graphql::JVal vars = options;
    vars["enabled"] = enabled;
    runAsync("SetPlayerAutoBilling", vars, std::move(cb));
  }

  /// Begin vaulting a card (P4b): returns the Stripe SetupIntent client
  /// secret + publishable key. Confirming the card is a BROWSER flow —
  /// native callers hand the secret to a webview/external browser.
  graphql::Json beginCardSetup() const {
    return run("BeginPlayerCardSetup", {});
  }
  void beginCardSetupAsync(graphql::GraphQLCallback cb) const {
    runAsync("BeginPlayerCardSetup", {}, std::move(cb));
  }

  /// The caller's per-app gate states (absence means active).
  graphql::Json runtimeStates() const { return run("PlayerRuntimeStates", {}); }
  void runtimeStatesAsync(graphql::GraphQLCallback cb) const {
    runAsync("PlayerRuntimeStates", {}, std::move(cb));
  }

  /// Studio: list an app's player policy rows (view_compute_diagnostics).
  graphql::Json policies(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return run("PlayerWasmPolicies", vars);
  }
  void policiesAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    runAsync("PlayerWasmPolicies", vars, std::move(cb));
  }

  /// Studio: upsert a player policy row (manage_compute).
  graphql::Json setPolicy(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return run("SetPlayerWasmPolicy", vars);
  }
  void setPolicyAsync(const graphql::JVal& input,
                      graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    runAsync("SetPlayerWasmPolicy", vars, std::move(cb));
  }

  /// Studio: delete a player policy row (manage_compute).
  graphql::Json deletePolicy(std::string_view appId, std::string_view scope,
                             const graphql::JVal& options = graphql::JVal()) const {
    graphql::JVal vars = options;
    vars["appId"] = appId;
    vars["scope"] = scope;
    return run("DeletePlayerWasmPolicy", vars);
  }
  void deletePolicyAsync(std::string_view appId, std::string_view scope,
                         const graphql::JVal& options,
                         graphql::GraphQLCallback cb) const {
    graphql::JVal vars = options;
    vars["appId"] = appId;
    vars["scope"] = scope;
    runAsync("DeletePlayerWasmPolicy", vars, std::move(cb));
  }

  /// Studio: read the app's player rate-card markup in bps (view_billing).
  graphql::Json rateMarkup(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return run("PlayerRateMarkup", vars);
  }
  void rateMarkupAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    runAsync("PlayerRateMarkup", vars, std::move(cb));
  }

  /// Studio: set the app's player rate-card markup in bps (manage_billing).
  graphql::Json setRateMarkup(std::string_view appId, int markupBps) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["markupBps"] = markupBps;
    return run("SetPlayerRateMarkup", vars);
  }
  void setRateMarkupAsync(std::string_view appId, int markupBps,
                          graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["markupBps"] = markupBps;
    runAsync("SetPlayerRateMarkup", vars, std::move(cb));
  }

  /// Studio: per-player usage aggregate (view_compute_diagnostics).
  graphql::Json appPlayerUsage(std::string_view appId,
                               const graphql::JVal& options = graphql::JVal()) const {
    graphql::JVal vars = options;
    vars["appId"] = appId;
    return run("AppPlayerUsage", vars);
  }
  void appPlayerUsageAsync(std::string_view appId, const graphql::JVal& options,
                           graphql::GraphQLCallback cb) const {
    graphql::JVal vars = options;
    vars["appId"] = appId;
    runAsync("AppPlayerUsage", vars, std::move(cb));
  }

  /// Studio: total accrued markup income in cents (view_billing).
  graphql::Json appMarkupAccrued(std::string_view appId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    return run("AppPlayerMarkupAccrued", vars);
  }
  void appMarkupAccruedAsync(std::string_view appId,
                             graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    runAsync("AppPlayerMarkupAccrued", vars, std::move(cb));
  }

 private:
  graphql::Json run(std::string_view op, const graphql::JVal& vars) const {
    return execUnwrap(gen::playerWallet::documentFor(op), vars, op);
  }
  void runAsync(std::string_view op, const graphql::JVal& vars,
                graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::playerWallet::documentFor(op), vars, op,
                    std::move(cb));
  }
};

}  // namespace crowdy::domains
