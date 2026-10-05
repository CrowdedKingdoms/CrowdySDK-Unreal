#pragma once

#include <functional>
#include <memory>
#include <string_view>
#include <utility>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/domains/types.hpp"
#include "crowdy/generated/operations.hpp"

/// client.marketplace() — the D4 grid claim flows (policy, requests, chunk
/// and ownership claims, invites) and studio moderation of player code
/// (admission queue, listing administration, ownership transfer,
/// claim-policy config). The player-facing listings (publish, acquire,
/// install) and grid-attached client mods went with legacy player compute;
/// ck-exec mods publish and install through client.exec().modPublish /
/// modInstall.
namespace crowdy::domains {

class MarketplaceAPI {
  /// Executor over the shared marketplace document.
  class Executor : public DomainBase {
   public:
    using DomainBase::DomainBase;
    graphql::Json run(std::string_view op, const graphql::JVal& vars) const {
      return execUnwrap(gen::marketplace::documentFor(op), vars, op);
    }
    void runAsync(std::string_view op, const graphql::JVal& vars,
                  graphql::GraphQLCallback cb) const {
      execUnwrapAsync(gen::marketplace::documentFor(op), vars, op,
                      std::move(cb));
    }
  };

 public:
  explicit MarketplaceAPI(std::shared_ptr<graphql::GraphQLClient> api)
      : api_(std::move(api)) {}

  // -- D4 grid claim flows -------------------------------------------------------

  /// The app's claim policy (self_claim / approval / invite / marketplace_only).
  graphql::Json gridClaimPolicy(const graphql::JVal& vars) const {
    return api_.run("MarketplaceGridClaimPolicy", vars);
  }
  void gridClaimPolicyAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    api_.runAsync("MarketplaceGridClaimPolicy", vars, std::move(cb));
  }

  /// Pending claim requests (approvers see the app queue; players their own).
  graphql::Json gridClaimRequests(const graphql::JVal& vars) const {
    return api_.run("MarketplaceGridClaimRequests", vars);
  }
  void gridClaimRequestsAsync(const graphql::JVal& vars,
                              graphql::GraphQLCallback cb) const {
    api_.runAsync("MarketplaceGridClaimRequests", vars, std::move(cb));
  }

  /// Claim grid ownership under the app policy (server-authorized, D4).
  graphql::Json claimGridOwnership(const graphql::JVal& vars) const {
    return api_.run("MarketplaceClaimGridOwnership", vars);
  }
  void claimGridOwnershipAsync(const graphql::JVal& vars,
                               graphql::GraphQLCallback cb) const {
    api_.runAsync("MarketplaceClaimGridOwnership", vars, std::move(cb));
  }

  /// Atomically create and claim one chunk under SELF_CLAIM. This is a
  /// player/app-token operation; it never requires
  /// manage_apps. BigInt variables must be decimal strings.
  graphql::Json claimGridChunk(const graphql::JVal& vars) const {
    return api_.run("MarketplaceClaimGridChunk", vars);
  }
  void claimGridChunkAsync(const graphql::JVal& vars,
                           graphql::GraphQLCallback cb) const {
    api_.runAsync("MarketplaceClaimGridChunk", vars, std::move(cb));
  }
  graphql::Json claimGridChunk(std::string_view appId,
                               const ChunkRef& chunk) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["chunk"] = chunk.toInput();
    return claimGridChunk(vars);
  }
  void claimGridChunkAsync(std::string_view appId, const ChunkRef& chunk,
                           graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["chunk"] = chunk.toInput();
    claimGridChunkAsync(vars, std::move(cb));
  }

  /// Release an eligible one-chunk grid previously created by
  /// claimGridChunk. The authenticated app-token user must still own it.
  graphql::Json releaseClaimedGrid(const graphql::JVal& vars) const {
    return api_.run("MarketplaceReleaseClaimedGrid", vars);
  }
  void releaseClaimedGridAsync(const graphql::JVal& vars,
                               graphql::GraphQLCallback cb) const {
    api_.runAsync("MarketplaceReleaseClaimedGrid", vars, std::move(cb));
  }
  graphql::Json releaseClaimedGrid(std::string_view appId,
                                   std::string_view gridId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    return releaseClaimedGrid(vars);
  }
  void releaseClaimedGridAsync(std::string_view appId,
                               std::string_view gridId,
                               graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    releaseClaimedGridAsync(vars, std::move(cb));
  }

  /// Approve or deny a pending claim request (approvers/staff).
  graphql::Json decideGridClaim(const graphql::JVal& vars) const {
    return api_.run("MarketplaceDecideGridClaim", vars);
  }
  void decideGridClaimAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    api_.runAsync("MarketplaceDecideGridClaim", vars, std::move(cb));
  }

  /// Issue a standing claim invite (approvers/staff; INVITE mode).
  graphql::Json issueGridClaimInvite(const graphql::JVal& vars) const {
    return api_.run("MarketplaceIssueGridClaimInvite", vars);
  }
  void issueGridClaimInviteAsync(const graphql::JVal& vars,
                                 graphql::GraphQLCallback cb) const {
    api_.runAsync("MarketplaceIssueGridClaimInvite", vars, std::move(cb));
  }

  // -- Studio moderation (requires studio permissions) ---------------------------

  /// The admission queue: listings joined with allow-list standing.
  graphql::Json admissionQueue(const graphql::JVal& vars) const {
    return api_.run("MarketplaceAdmissionQueue", vars);
  }
  void admissionQueueAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    api_.runAsync("MarketplaceAdmissionQueue", vars, std::move(cb));
  }

  /// Studio catalog administration view (includes delisted/killed on request).
  graphql::Json appListings(const graphql::JVal& vars) const {
    return api_.run("MarketplaceAppListings", vars);
  }
  void appListingsAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    api_.runAsync("MarketplaceAppListings", vars, std::move(cb));
  }

  /// Immutable versions of one listing in the studio administration view.
  /// Requires view_compute_diagnostics.
  graphql::Json appListingVersions(const graphql::JVal& vars) const {
    return api_.run("MarketplaceAppListingVersions", vars);
  }
  void appListingVersionsAsync(const graphql::JVal& vars,
                               graphql::GraphQLCallback cb) const {
    api_.runAsync("MarketplaceAppListingVersions", vars,
                         std::move(cb));
  }

  /// All acquisitions in the app (studio audit view).
  graphql::Json appAcquisitions(const graphql::JVal& vars) const {
    return api_.run("MarketplaceAppAcquisitions", vars);
  }
  void appAcquisitionsAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    api_.runAsync("MarketplaceAppAcquisitions", vars, std::move(cb));
  }

  /// Audited personal<->org listing transfer (DN-9).
  graphql::Json transferListing(const graphql::JVal& vars) const {
    return api_.run("MarketplaceTransferListing", vars);
  }
  void transferListingAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    api_.runAsync("MarketplaceTransferListing", vars, std::move(cb));
  }

  /// Catalog status (owner delist/relist; studio KILLED).
  graphql::Json setListingStatus(const graphql::JVal& vars) const {
    return api_.run("MarketplaceSetListingStatus", vars);
  }
  void setListingStatusAsync(const graphql::JVal& vars, graphql::GraphQLCallback cb) const {
    api_.runAsync("MarketplaceSetListingStatus", vars, std::move(cb));
  }

  /// Configure the app's D4 grid claim policy (manage_apps).
  graphql::Json setGridClaimPolicy(const graphql::JVal& vars) const {
    return api_.run("MarketplaceSetGridClaimPolicy", vars);
  }
  void setGridClaimPolicyAsync(const graphql::JVal& vars,
                               graphql::GraphQLCallback cb) const {
    api_.runAsync("MarketplaceSetGridClaimPolicy", vars, std::move(cb));
  }

 private:
  Executor api_;
};

}  // namespace crowdy::domains
