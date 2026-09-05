#pragma once

#include <functional>
#include <utility>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

/// Operator surface — client.operator_(): the platform-wide compute ceilings and
/// org wallet credits. As of the unified API (v13) this is otherwise reduced:
/// dedicated customer environments were retired, and the infrastructure
/// control plane (environments, change orders, secrets, release management,
/// audit) moved to the separate infra-control-plane service with its own auth
/// and operator console. The surviving operations still require is_operator;
/// the server enforces the flag on every call.
namespace crowdy::domains {

class OperatorAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  /// Platform-wide compute ceilings (the maxima computeSetPolicy clamps to).
  /// Nullable knobs: null = no operator override (game-api bootstrap default).
  graphql::Json computePlatformCeilings() const {
    return run("CpComputePlatformCeilings", graphql::JVal());
  }
  void computePlatformCeilingsAsync(graphql::GraphQLCallback cb) const {
    runAsync("CpComputePlatformCeilings", graphql::JVal(), std::move(cb));
  }
  /// Patch the compute ceilings. Per knob: omit = unchanged, explicit null =
  /// clear the override, positive value = set. Replica-syncs to game-api
  /// (no restart, <=30s cache bound) and writes an audit entry.
  graphql::Json setComputePlatformCeilings(const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return run("CpSetComputePlatformCeilings", vars);
  }
  void setComputePlatformCeilingsAsync(const graphql::JVal& input,
                                       graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    runAsync("CpSetComputePlatformCeilings", vars, std::move(cb));
  }

  /// Credit an org wallet. The ONLY sanctioned way to put funds in one.
  ///
  /// A hand-written INSERT is silently wrong rather than an error: nothing
  /// assigns the wallet_id surrogate and the unique index tolerates repeated
  /// NULLs, so the row inserts and every later lookup by wallet_id misses. This
  /// mutation also re-evaluates the runtime out-of-funds decision, which is a
  /// STORED verdict and not a live read of the balance — funding without
  /// re-evaluating leaves a funded app still refusing every client.
  ///
  /// `reason` becomes the ledger description, so write it for whoever reads the
  /// ledger later. `referenceId` makes the call idempotent: replaying one returns
  /// the original transaction rather than crediting twice.
  graphql::Json creditOrgWallet(std::string_view orgId,
                                std::string_view amountCents,
                                std::string_view reason,
                                std::string_view referenceId = {}) const {
    return run("CpCreditOrgWallet",
               creditVars(orgId, amountCents, reason, referenceId));
  }
  void creditOrgWalletAsync(std::string_view orgId, std::string_view amountCents,
                            std::string_view reason,
                            std::string_view referenceId,
                            graphql::GraphQLCallback cb) const {
    runAsync("CpCreditOrgWallet",
             creditVars(orgId, amountCents, reason, referenceId), std::move(cb));
  }

 private:
  static graphql::JVal creditVars(std::string_view orgId,
                                  std::string_view amountCents,
                                  std::string_view reason,
                                  std::string_view referenceId) {
    graphql::JVal vars;
    vars["orgId"] = orgId;
    // BigInt stays a decimal STRING; narrowing money to a double is how a
    // credit becomes almost the right amount.
    vars["amountCents"] = amountCents;
    vars["reason"] = reason;
    if (!referenceId.empty()) vars["referenceId"] = referenceId;
    return vars;
  }

  graphql::Json run(std::string_view op, const graphql::JVal& vars) const {
    return execUnwrap(gen::controlPlane::documentFor(op), vars, op);
  }
  void runAsync(std::string_view op, const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::controlPlane::documentFor(op), vars, op, std::move(cb));
  }
};

}  // namespace crowdy::domains
