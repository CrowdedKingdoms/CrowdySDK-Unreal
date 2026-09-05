#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

namespace crowdy::domains {

/// Exact GraphQL surface for `crowdy.studio-agent/1`.
///
/// Runtime/session operations, app policy, sanitized usage, and operator
/// controls all go to the one API origin, gated by permission. This class does
/// not expose an arbitrary document executor, provider client, UDP authority,
/// or tool bridge.
class CrowdyStudioAgentAPI {
 public:
  CrowdyStudioAgentAPI(std::shared_ptr<graphql::GraphQLClient> api,
                       std::shared_ptr<graphql::Dispatcher> dispatcher)
      : api_(std::move(api)), dispatcher_(std::move(dispatcher)) {}

  std::shared_ptr<graphql::Dispatcher> dispatcher() const { return dispatcher_; }

  // Reads.
  graphql::Json session(std::string_view sessionId) const {
    return run("CrowdyStudioAgentSession", one("sessionId", sessionId));
  }
  void sessionAsync(std::string_view sessionId, graphql::GraphQLCallback cb) const {
    runAsync("CrowdyStudioAgentSession", one("sessionId", sessionId),
              std::move(cb));
  }
  graphql::Json sessions(std::string_view appId,
                         const graphql::JVal& page = graphql::JVal()) const {
    auto vars = page;
    vars["appId"] = appId;
    return run("CrowdyStudioAgentSessions", vars);
  }
  void sessionsAsync(std::string_view appId, const graphql::JVal& page,
                     graphql::GraphQLCallback cb) const {
    auto vars = page;
    vars["appId"] = appId;
    runAsync("CrowdyStudioAgentSessions", vars, std::move(cb));
  }
  graphql::Json history(std::string_view sessionId, std::string_view afterSeq = "0",
                        int first = 100) const {
    graphql::JVal vars;
    vars["sessionId"] = sessionId;
    vars["afterSeq"] = afterSeq;
    vars["first"] = std::int64_t{first};
    return run("CrowdyStudioAgentHistory", vars);
  }
  void historyAsync(std::string_view sessionId, std::string_view afterSeq,
                    int first, graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["sessionId"] = sessionId;
    vars["afterSeq"] = afterSeq;
    vars["first"] = std::int64_t{first};
    runAsync("CrowdyStudioAgentHistory", vars, std::move(cb));
  }
  graphql::Json toolDescriptors(std::string_view sessionId) const {
    return run("CrowdyStudioAgentToolDescriptors",
                one("sessionId", sessionId));
  }
  void toolDescriptorsAsync(std::string_view sessionId,
                            graphql::GraphQLCallback cb) const {
    runAsync("CrowdyStudioAgentToolDescriptors", one("sessionId", sessionId),
              std::move(cb));
  }
  graphql::Json budget(std::string_view sessionId) const {
    return run("CrowdyStudioAgentBudget", one("sessionId", sessionId));
  }
  void budgetAsync(std::string_view sessionId,
                   graphql::GraphQLCallback cb) const {
    runAsync("CrowdyStudioAgentBudget", one("sessionId", sessionId),
              std::move(cb));
  }

  // Mutations. Inputs deliberately mirror the named schema inputs;
  // authenticated authority fields are injected/validated by the server.
  graphql::Json createSession(const graphql::JVal& input) const {
    return runInput("CrowdyStudioAgentCreateSession", input);
  }
  void createSessionAsync(const graphql::JVal& input,
                          graphql::GraphQLCallback cb) const {
    runInputAsync("CrowdyStudioAgentCreateSession", input, std::move(cb));
  }
  graphql::Json attachClient(const graphql::JVal& input) const {
    return runInput("CrowdyStudioAgentAttachClient", input);
  }
  void attachClientAsync(const graphql::JVal& input,
                         graphql::GraphQLCallback cb) const {
    runInputAsync("CrowdyStudioAgentAttachClient", input, std::move(cb));
  }
  graphql::Json setMode(const graphql::JVal& input) const {
    return runInput("CrowdyStudioAgentSetMode", input);
  }
  void setModeAsync(const graphql::JVal& input,
                    graphql::GraphQLCallback cb) const {
    runInputAsync("CrowdyStudioAgentSetMode", input, std::move(cb));
  }
  graphql::Json acknowledgeEvents(const graphql::JVal& input) const {
    return runInput("CrowdyStudioAgentAcknowledgeEvents", input);
  }
  void acknowledgeEventsAsync(const graphql::JVal& input,
                              graphql::GraphQLCallback cb) const {
    runInputAsync("CrowdyStudioAgentAcknowledgeEvents", input, std::move(cb));
  }
  graphql::Json heartbeat(const graphql::JVal& input) const {
    return runInput("CrowdyStudioAgentHeartbeat", input);
  }
  void heartbeatAsync(const graphql::JVal& input,
                      graphql::GraphQLCallback cb) const {
    runInputAsync("CrowdyStudioAgentHeartbeat", input, std::move(cb));
  }
  graphql::Json sendMessage(const graphql::JVal& input) const {
    return runInput("CrowdyStudioAgentSendMessage", input);
  }
  void sendMessageAsync(const graphql::JVal& input,
                        graphql::GraphQLCallback cb) const {
    runInputAsync("CrowdyStudioAgentSendMessage", input, std::move(cb));
  }
  graphql::Json approveTool(const graphql::JVal& input) const {
    return runInput("CrowdyStudioAgentApproveTool", input);
  }
  void approveToolAsync(const graphql::JVal& input,
                        graphql::GraphQLCallback cb) const {
    runInputAsync("CrowdyStudioAgentApproveTool", input, std::move(cb));
  }
  graphql::Json rejectTool(const graphql::JVal& input) const {
    return runInput("CrowdyStudioAgentRejectTool", input);
  }
  void rejectToolAsync(const graphql::JVal& input,
                       graphql::GraphQLCallback cb) const {
    runInputAsync("CrowdyStudioAgentRejectTool", input, std::move(cb));
  }
  graphql::Json browserToolResult(const graphql::JVal& input) const {
    return runInput("CrowdyStudioAgentToolResult", input);
  }
  void browserToolResultAsync(const graphql::JVal& input,
                              graphql::GraphQLCallback cb) const {
    runInputAsync("CrowdyStudioAgentToolResult", input, std::move(cb));
  }
  graphql::Json grantLease(const graphql::JVal& input) const {
    return runInput("CrowdyStudioAgentGrantLease", input);
  }
  void grantLeaseAsync(const graphql::JVal& input,
                       graphql::GraphQLCallback cb) const {
    runInputAsync("CrowdyStudioAgentGrantLease", input, std::move(cb));
  }
  graphql::Json revokeLease(const graphql::JVal& input) const {
    return runInput("CrowdyStudioAgentRevokeLease", input);
  }
  void revokeLeaseAsync(const graphql::JVal& input,
                        graphql::GraphQLCallback cb) const {
    runInputAsync("CrowdyStudioAgentRevokeLease", input, std::move(cb));
  }
  graphql::Json pause(const graphql::JVal& input) const {
    return runInput("CrowdyStudioAgentPause", input);
  }
  void pauseAsync(const graphql::JVal& input,
                  graphql::GraphQLCallback cb) const {
    runInputAsync("CrowdyStudioAgentPause", input, std::move(cb));
  }
  graphql::Json resume(const graphql::JVal& input) const {
    return runInput("CrowdyStudioAgentResume", input);
  }
  void resumeAsync(const graphql::JVal& input,
                   graphql::GraphQLCallback cb) const {
    runInputAsync("CrowdyStudioAgentResume", input, std::move(cb));
  }
  graphql::Json cancelRun(const graphql::JVal& input) const {
    return runInput("CrowdyStudioAgentCancelRun", input);
  }
  void cancelRunAsync(const graphql::JVal& input,
                      graphql::GraphQLCallback cb) const {
    runInputAsync("CrowdyStudioAgentCancelRun", input, std::move(cb));
  }
  graphql::Json closeSession(const graphql::JVal& input) const {
    return runInput("CrowdyStudioAgentCloseSession", input);
  }
  void closeSessionAsync(const graphql::JVal& input,
                         graphql::GraphQLCallback cb) const {
    runInputAsync("CrowdyStudioAgentCloseSession", input, std::move(cb));
  }

  // App policy and sanitized usage.
  graphql::Json policy(std::string_view appId) const {
    return run("CrowdyStudioAgentPolicy", one("appId", appId));
  }
  void policyAsync(std::string_view appId, graphql::GraphQLCallback cb) const {
    runAsync("CrowdyStudioAgentPolicy", one("appId", appId),
                    std::move(cb));
  }
  graphql::Json effectivePolicy(std::string_view appId) const {
    return run("CrowdyStudioAgentEffectivePolicy", one("appId", appId));
  }
  void effectivePolicyAsync(std::string_view appId,
                            graphql::GraphQLCallback cb) const {
    runAsync("CrowdyStudioAgentEffectivePolicy", one("appId", appId),
                    std::move(cb));
  }
  graphql::Json usage(std::string_view appId,
                      const graphql::JVal& window = graphql::JVal()) const {
    auto vars = window;
    vars["appId"] = appId;
    return run("CrowdyStudioAgentUsage", vars);
  }
  void usageAsync(std::string_view appId, const graphql::JVal& window,
                  graphql::GraphQLCallback cb) const {
    auto vars = window;
    vars["appId"] = appId;
    runAsync("CrowdyStudioAgentUsage", vars, std::move(cb));
  }
  graphql::Json setPolicy(const graphql::JVal& input) const {
    return runInput("CrowdyStudioAgentSetPolicy", input);
  }
  void setPolicyAsync(const graphql::JVal& input,
                      graphql::GraphQLCallback cb) const {
    runInputAsync("CrowdyStudioAgentSetPolicy", input, std::move(cb));
  }

  // Operator roots.
  graphql::Json platformPolicy() const {
    return run("CpCrowdyStudioAgentPlatformPolicy", graphql::JVal());
  }
  void platformPolicyAsync(graphql::GraphQLCallback cb) const {
    runAsync("CpCrowdyStudioAgentPlatformPolicy", graphql::JVal(),
                    std::move(cb));
  }
  graphql::Json setPlatformPolicy(const graphql::JVal& input) const {
    return runInput("CpSetCrowdyStudioAgentPlatformPolicy", input);
  }
  void setPlatformPolicyAsync(const graphql::JVal& input,
                              graphql::GraphQLCallback cb) const {
    runInputAsync("CpSetCrowdyStudioAgentPlatformPolicy", input,
                         std::move(cb));
  }
  graphql::Json setOperatorAppKill(const graphql::JVal& input) const {
    return runInput("CpSetCrowdyStudioAgentAppKill", input);
  }
  void setOperatorAppKillAsync(const graphql::JVal& input,
                               graphql::GraphQLCallback cb) const {
    runInputAsync("CpSetCrowdyStudioAgentAppKill", input, std::move(cb));
  }

 private:
  static graphql::JVal one(std::string_view key, std::string_view value) {
    graphql::JVal vars;
    vars[key] = value;
    return vars;
  }
  static graphql::JVal inputVars(const graphql::JVal& input) {
    graphql::JVal vars;
    vars["input"] = input;
    return vars;
  }
  graphql::Json run(std::string_view operation,
                     const graphql::JVal& vars) const {
    auto data = api_->request(gen::crowdyStudioAgent::documentFor(operation),
                               vars, operation);
    graphql::Json result;
    data.forEachMember([&](std::string_view, graphql::Json value) {
      result = value;
    });
    return result;
  }
  void runAsync(std::string_view operation, const graphql::JVal& vars,
                 graphql::GraphQLCallback cb) const {
    unwrapAsync(api_, gen::crowdyStudioAgent::documentFor(operation),
                operation, vars, std::move(cb));
  }
  graphql::Json runInput(std::string_view operation,
                          const graphql::JVal& input) const {
    return run(operation, inputVars(input));
  }
  void runInputAsync(std::string_view operation, const graphql::JVal& input,
                      graphql::GraphQLCallback cb) const {
    runAsync(operation, inputVars(input), std::move(cb));
  }
  static void unwrapAsync(
      const std::shared_ptr<graphql::GraphQLClient>& client,
      std::string_view document, std::string_view operation,
      const graphql::JVal& vars, graphql::GraphQLCallback cb) {
    client->requestAsync(
        document, vars, operation,
        [cb = std::move(cb)](graphql::GraphQLOutcome outcome) mutable {
          if (outcome.ok() && outcome.data.isObject() &&
              outcome.data.size() == 1) {
            graphql::Json result;
            outcome.data.forEachMember(
                [&](std::string_view, graphql::Json value) { result = value; });
            outcome.data = result;
          }
          cb(std::move(outcome));
        });
  }

  std::shared_ptr<graphql::GraphQLClient> api_;
  std::shared_ptr<graphql::Dispatcher> dispatcher_;
};

}  // namespace crowdy::domains
