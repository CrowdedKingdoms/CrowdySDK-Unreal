#include "crowdy/domains/crowdy_studio_agent.hpp"

#include <utility>

namespace crowdy::domains {

namespace {

std::int64_t integerOf(const graphql::Json& value) {
  return value.isNumber() ? value.asInt64() : 0;
}

/// A refusal that is not the endpoint's envelope (a gateway's page, a guard's
/// default body): at most this much of it is kept as the message.
constexpr std::size_t kRefusalExcerptBytes = 512;

CrowdyStudioModelError refusalOf(const graphql::Json& body, const std::string& raw,
                                 int httpStatus) {
  CrowdyStudioModelError error;
  error.retryable = httpStatus >= 500;
  const graphql::Json detail = body["error"];
  if (detail.isObject()) {
    error.code = detail["code"].asString();
    error.message = detail["message"].asString();
    error.type = detail["type"].asString();
    if (detail["retryable"].isBool()) error.retryable = detail["retryable"].asBool();
    return error;
  }
  error.message = body["message"].isString() ? body["message"].asString()
                                             : raw.substr(0, kRefusalExcerptBytes);
  return error;
}

CrowdyStudioModel modelOf(const graphql::Json& entry, std::string id) {
  CrowdyStudioModel model;
  model.name = entry["name"].isString() ? entry["name"].asString() : id;
  model.id = std::move(id);
  if (entry["context_window"].isNumber()) {
    model.contextWindow = entry["context_window"].asInt64();
  }
  const graphql::Json modalities = entry["input_modalities"];
  if (modalities.isArray()) {
    modalities.forEach([&](const graphql::Json& modality) {
      if (modality.isString()) model.inputModalities.push_back(modality.asString());
    });
  }
  const graphql::Json pricing = entry["pricing_microusd_per_million"];
  model.pricingMicrousdPerMillion.input = integerOf(pricing["input"]);
  model.pricingMicrousdPerMillion.output = integerOf(pricing["output"]);
  model.pricingMicrousdPerMillion.reasoning = integerOf(pricing["reasoning"]);
  model.pricingMicrousdPerMillion.cachedInput = integerOf(pricing["cached_input"]);
  return model;
}

}  // namespace

CrowdyStudioModelList crowdyStudioModelListFrom(const graphql::HttpOutcome& http) {
  CrowdyStudioModelList list;
  list.httpStatus = http.response.status;
  if (!http.status.ok()) {
    list.status = http.status;
    list.errorMessage = http.errorMessage;
    return list;
  }
  const graphql::Json body = graphql::Json::parse(http.response.body);
  if (http.response.status < 200 || http.response.status >= 300) {
    list.status = Errc::Rejected;
    list.error = refusalOf(body, http.response.body, http.response.status);
    list.errorMessage = list.error->message;
    return list;
  }
  const graphql::Json data = body["data"];
  if (!data.isArray()) {
    list.status = Errc::Malformed;
    list.errorMessage = "the model list is not an OpenAI list: it has no data array";
    return list;
  }
  data.forEach([&](const graphql::Json& entry) {
    if (list.models.size() >= kCrowdyStudioModelListMax) return;
    std::string id = entry["id"].asString();
    if (!id.empty()) list.models.push_back(modelOf(entry, std::move(id)));
  });
  return list;
}

}  // namespace crowdy::domains
