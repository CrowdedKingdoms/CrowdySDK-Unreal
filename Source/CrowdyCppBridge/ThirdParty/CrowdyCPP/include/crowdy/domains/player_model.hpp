#pragma once

#include <utility>

#include "crowdy/domains/domain_base.hpp"
#include "crowdy/generated/operations.hpp"

/// client.playerModel() — flexible player-owned model data and grid-confined
/// player automations. All identities/scopes are forced by game-api.
namespace crowdy::domains {

class PlayerModelAPI : public DomainBase {
 public:
  using DomainBase::DomainBase;

  graphql::Json containers(std::string_view appId,
                           std::string_view gridId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    return run("PlayerModelContainers", vars);
  }
  void containersAsync(std::string_view appId, std::string_view gridId,
                       graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    runAsync("PlayerModelContainers", vars, std::move(cb));
  }

  graphql::Json container(const graphql::JVal& input) const {
    return byInput("PlayerModelContainer", input);
  }
  void containerAsync(const graphql::JVal& input,
                      graphql::GraphQLCallback cb) const {
    byInputAsync("PlayerModelContainer", input, std::move(cb));
  }

  graphql::Json createContainer(const graphql::JVal& input) const {
    return byInput("PlayerModelCreateContainer", input);
  }
  void createContainerAsync(const graphql::JVal& input,
                            graphql::GraphQLCallback cb) const {
    byInputAsync("PlayerModelCreateContainer", input, std::move(cb));
  }

  graphql::Json setProperty(const graphql::JVal& input) const {
    return byInput("PlayerModelSetProperty", input);
  }
  void setPropertyAsync(const graphql::JVal& input,
                        graphql::GraphQLCallback cb) const {
    byInputAsync("PlayerModelSetProperty", input, std::move(cb));
  }

  graphql::Json deleteContainer(const graphql::JVal& input) const {
    return byInput("PlayerModelDeleteContainer", input);
  }
  void deleteContainerAsync(const graphql::JVal& input,
                            graphql::GraphQLCallback cb) const {
    byInputAsync("PlayerModelDeleteContainer", input, std::move(cb));
  }

  graphql::Json automations(std::string_view appId,
                            std::string_view gridId) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    return run("PlayerAutomations", vars);
  }
  void automationsAsync(std::string_view appId, std::string_view gridId,
                        graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["appId"] = appId;
    vars["gridId"] = gridId;
    runAsync("PlayerAutomations", vars, std::move(cb));
  }

  graphql::Json createAutomation(const graphql::JVal& input) const {
    return byInput("PlayerAutomationCreate", input);
  }
  void createAutomationAsync(const graphql::JVal& input,
                             graphql::GraphQLCallback cb) const {
    byInputAsync("PlayerAutomationCreate", input, std::move(cb));
  }

  graphql::Json setAutomationEnabled(const graphql::JVal& input) const {
    return byInput("PlayerAutomationSetEnabled", input);
  }
  void setAutomationEnabledAsync(const graphql::JVal& input,
                                 graphql::GraphQLCallback cb) const {
    byInputAsync("PlayerAutomationSetEnabled", input, std::move(cb));
  }

  graphql::Json deleteAutomation(const graphql::JVal& input) const {
    return byInput("PlayerAutomationDelete", input);
  }
  void deleteAutomationAsync(const graphql::JVal& input,
                             graphql::GraphQLCallback cb) const {
    byInputAsync("PlayerAutomationDelete", input, std::move(cb));
  }

 private:
  graphql::Json run(std::string_view op, const graphql::JVal& vars) const {
    return execUnwrap(gen::playerModel::documentFor(op), vars, op);
  }
  void runAsync(std::string_view op, const graphql::JVal& vars,
                graphql::GraphQLCallback cb) const {
    execUnwrapAsync(gen::playerModel::documentFor(op), vars, op,
                    std::move(cb));
  }

  graphql::Json byInput(std::string_view op,
                        const graphql::JVal& input) const {
    graphql::JVal vars;
    vars["input"] = input;
    return run(op, vars);
  }
  void byInputAsync(std::string_view op, const graphql::JVal& input,
                    graphql::GraphQLCallback cb) const {
    graphql::JVal vars;
    vars["input"] = input;
    runAsync(op, vars, std::move(cb));
  }
};

}  // namespace crowdy::domains
