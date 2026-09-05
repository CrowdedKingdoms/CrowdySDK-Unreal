#include "crowdy/graphql/graphql_client.hpp"

#include <algorithm>
#include <atomic>
#include <string>
#include <utility>

#include "error_extensions.hpp"

namespace crowdy::graphql {

namespace {

/// Turn a completed HTTP response into an outcome, without throwing. This is
/// the single source of truth for the GraphQL response contract; both the
/// blocking request() and the async path run through it, so their behavior
/// can never drift. The decision order matches the historical request().
GraphQLOutcome interpret(int httpStatus, const std::string& body) {
  GraphQLOutcome out;
  out.httpStatus = httpStatus;

  // GraphQL servers return errors with 200 or 4xx; try to parse either way.
  Json parsed = Json::parse(body);
  if (!parsed.ok()) {
    if (httpStatus < 200 || httpStatus >= 300) {
      out.status = Errc::Rejected;
      out.kind = GraphQLErrorKind::Http;
      out.body = body;
    } else {
      out.status = Errc::Malformed;
      out.kind = GraphQLErrorKind::Protocol;
      out.errorMessage = "GraphQL response is not valid JSON";
    }
    return out;
  }

  Json errors = parsed["errors"];
  if (errors.isArray() && errors.size() > 0) {
    errors.forEach([&](Json e) {
      out.errors.push_back(detail::readGraphQLError(e, "GraphQL error"));
    });
    out.status = Errc::Rejected;
    out.kind = GraphQLErrorKind::GraphQL;
    return out;
  }

  if (httpStatus < 200 || httpStatus >= 300) {
    out.status = Errc::Rejected;
    out.kind = GraphQLErrorKind::Http;
    out.body = body;
    return out;
  }

  Json data = parsed["data"];
  if (!data.ok()) {
    out.status = Errc::Malformed;
    out.kind = GraphQLErrorKind::Protocol;
    out.errorMessage = "GraphQL response has no data";
    return out;
  }

  out.data = data;
  out.status = Errc::Ok;
  return out;
}

std::string authorizationOf(const HttpRequest& request) {
  for (const auto& header : request.headers) {
    if (header.first == "Authorization") return header.second;
  }
  return {};
}

/// Replace the request's Authorization with `value` (empty = carry none).
void setAuthorization(HttpRequest& request, const std::string& value) {
  request.headers.erase(
      std::remove_if(request.headers.begin(), request.headers.end(),
                     [](const auto& header) {
                       return header.first == "Authorization";
                     }),
      request.headers.end());
  if (!value.empty()) request.headers.emplace_back("Authorization", value);
}

GraphQLOutcome outcomeFromHttp(HttpOutcome http) {
  if (!http.status.ok()) {
    GraphQLOutcome out;
    out.status = http.status;
    out.kind =
        http.status.code == Errc::Timeout ? GraphQLErrorKind::Timeout : GraphQLErrorKind::Network;
    out.errorMessage = std::move(http.errorMessage);
    return out;
  }
  return interpret(http.response.status, http.response.body);
}

#ifndef CROWDY_NO_EXCEPTIONS
[[noreturn]] void throwOutcome(const GraphQLOutcome& out) {
  switch (out.kind) {
    case GraphQLErrorKind::Http: throw CrowdyHttpError(out.httpStatus, out.body);
    case GraphQLErrorKind::GraphQL:
      // Typed, so a caller can tell "this app is down" from any other rejection
      // and show the server's own message instead of retrying blindly.
      if (isAppUnavailable(out.errors)) {
        throw CrowdyAppUnavailableError(out.errors);
      }
      throw CrowdyGraphQLError(out.errors);
    case GraphQLErrorKind::Protocol: throw CrowdyProtocolError(out.errorMessage);
    case GraphQLErrorKind::Network: throw CrowdyNetworkError(out.errorMessage);
    case GraphQLErrorKind::Timeout: throw CrowdyTimeoutError(out.errorMessage);
    case GraphQLErrorKind::None: break;
  }
  throw CrowdyProtocolError("unknown GraphQL failure");
}
#endif

}  // namespace

bool GraphQLClient::applyDatacenterRedirect(const GraphQLOutcome& outcome,
                                            std::string_view requestUrl) {
  if (outcome.kind != GraphQLErrorKind::GraphQL) return false;
  // Checked FIRST: APP_UNAVAILABLE carries no endpoint on purpose, and must not
  // be read as a redirect whose target happens to be missing.
  if (isAppUnavailable(outcome.errors)) return false;
  const auto move = moveFromErrors(outcome.errors);
  if (!move) return false;

  std::function<bool(const DatacenterMove&)> handler;
  {
    std::lock_guard lock(endpointMutex_);
    handler = wrongDatacenterHandler_;
  }

  bool moved = false;
  if (handler) {
#ifndef CROWDY_NO_EXCEPTIONS
    // A handler that throws must not turn a server error into a client crash;
    // the caller still needs the original rejection. Returned directly rather
    // than falling through: a handler can move this endpoint and THEN throw
    // (the WebSocket or session move failed), and the endpoint comparison
    // below would read that torn state as "already moved, retry quietly" -
    // hiding exactly the split the throw was reporting.
    try {
      moved = handler(*move);
    } catch (...) {
      return false;
    }
#else
    moved = handler(*move);
#endif
  }
  if (moved) return true;

  // "No move happened" is not the same thing as "nowhere to go". Requests to
  // one endpoint can be in flight together, and every one of them is answered
  // with the same redirect; the first to complete moves the client, and the
  // handler then reports "no move" to the rest only because the target is
  // already current. Those requests retry too - the client sits somewhere
  // other than where they were sent, so the retry reaches the new datacenter
  // instead of restating the URL that was just refused. When the endpoint is
  // unchanged the refusal stands, because retrying the same URL is how a
  // redirect loop starts.
  return endpoint() != requestUrl;
}

HttpRequest GraphQLClient::buildHttpRequest(std::string_view document, const JVal& variables,
                                            std::string_view operationName) const {
  JVal body;
  body["query"] = JVal(document);
  if (!variables.isNull()) body["variables"] = variables;
  if (!operationName.empty()) body["operationName"] = JVal(operationName);

  HttpRequest req;
  req.url = endpoint();
  req.body = body.dump();
  req.timeoutMs = config_.timeoutMs;
  req.headers.emplace_back("Content-Type", "application/json");
  req.headers.emplace_back("Accept", "application/json");
  const std::string token = auth_->token();
  if (!token.empty()) req.headers.emplace_back("Authorization", "Bearer " + token);
  return req;
}

HttpOutcome GraphQLClient::sendInline(const HttpRequest& request) {
  if (!transport_) {
    HttpOutcome out;
    out.status = Errc::NotConnected;
    out.errorMessage =
        "No default HTTP transport is available; inject IHttpTransport";
    return out;
  }
  return transport_->sendOutcome(request);
}

#ifndef CROWDY_NO_EXCEPTIONS
Json GraphQLClient::request(std::string_view document, const JVal& variables,
                            std::string_view operationName) {
  if (!transport_) {
    throw CrowdyNetworkError(
        "No default HTTP transport is available; inject IHttpTransport");
  }
  HttpRequest req = buildHttpRequest(document, variables, operationName);
  GraphQLOutcome out = outcomeFromHttp(sendInline(req));
  // Exactly one retry, and only after the endpoint actually moved - whether
  // this redirect moved it or a concurrent request's already had. Retrying a
  // second WRONG_DATACENTER is how two datacenters that disagree about an app
  // turn one query into an infinite ping-pong.
  if (!out.ok() && applyDatacenterRedirect(out, req.url)) {
    // The retry re-issues the SAME operation, so it keeps the attempt's own
    // Authorization; the shared AuthState may hold a different caller's bearer
    // by now.
    const std::string sentAuthorization = authorizationOf(req);
    req = buildHttpRequest(document, variables, operationName);
    setAuthorization(req, sentAuthorization);
    out = outcomeFromHttp(sendInline(req));
  }
  if (!out.ok()) throwOutcome(out);
  return out.data;
}
#else
Json GraphQLClient::request(std::string_view document, const JVal& variables,
                            std::string_view operationName) {
  HttpRequest request = buildHttpRequest(document, variables, operationName);
  GraphQLOutcome outcome = outcomeFromHttp(sendInline(request));
  if (!outcome.ok() && applyDatacenterRedirect(outcome, request.url)) {
    const std::string sentAuthorization = authorizationOf(request);
    request = buildHttpRequest(document, variables, operationName);
    setAuthorization(request, sentAuthorization);
    outcome = outcomeFromHttp(sendInline(request));
  }
  return outcome.ok() ? outcome.data : Json{};
}
#endif

void GraphQLClient::requestAsync(std::string_view document, const JVal& variables,
                                 std::string_view operationName, GraphQLCallback cb) {
  requestAsyncAttempt(std::string(document), variables,
                      std::string(operationName), std::move(cb), false);
}

void GraphQLClient::requestAsyncAttempt(std::string document,
                                        const JVal& variables,
                                        std::string operationName,
                                        GraphQLCallback cb, bool isRetry,
                                        std::optional<std::string> retryAuthorization) {
  HttpRequest req = buildHttpRequest(document, variables, operationName);
  if (retryAuthorization) {
    setAuthorization(req, *retryAuthorization);
  }

  // Wrap the caller's callback so a redirect is applied and the request
  // re-issued ONCE, before the caller ever sees the failure. `isRetry` is what
  // makes it once: without it, two datacenters disagreeing about an app would
  // bounce a request between them forever, asynchronously and invisibly.
  // `sentUrl` and `sentAuthorization` are captured from the request, not read
  // back at delivery time, because the retry needs the URL and the credential
  // this attempt actually used even after a concurrent redirect moved the
  // client or a concurrent caller switched the shared AuthState.
  if (!isRetry) {
    cb = [this, document, variables, operationName, sentUrl = req.url,
          sentAuthorization = authorizationOf(req),
          cb = std::move(cb)](GraphQLOutcome out) mutable {
      if (!out.ok() && applyDatacenterRedirect(out, sentUrl)) {
        requestAsyncAttempt(document, variables, operationName, std::move(cb),
                            true, std::move(sentAuthorization));
        return;
      }
      cb(std::move(out));
    };
  }

  auto dispatcher = dispatcher_;
  auto scope = asyncScope_;
  auto completed = std::make_shared<std::atomic_bool>(false);
  auto deliver = [dispatcher, scope, completed, cb = std::move(cb)](
                     GraphQLOutcome out) mutable {
    if (completed->exchange(true, std::memory_order_acq_rel)) return;
    auto invoke = [scope, cb, out = std::move(out)]() mutable {
      scope->run([&] { cb(std::move(out)); });
    };
    if (dispatcher) {
      dispatcher->post(std::move(invoke));
    } else {
      invoke();
    }
  };

  if (asyncTransport_) {
    auto complete = [deliver](HttpOutcome http) mutable {
      deliver(outcomeFromHttp(std::move(http)));
    };
#ifndef CROWDY_NO_EXCEPTIONS
    try {
      asyncTransport_->sendAsync(req, std::move(complete));
    } catch (const CrowdyTimeoutError& error) {
      deliver(GraphQLOutcome{Errc::Timeout, GraphQLErrorKind::Timeout, {},
                             {}, 0, error.what(), {}});
    } catch (const std::exception& error) {
      deliver(GraphQLOutcome{Errc::SocketError,
                             GraphQLErrorKind::Network, {}, {}, 0,
                             error.what(), {}});
    } catch (...) {
      deliver(GraphQLOutcome{
          Errc::SocketError, GraphQLErrorKind::Network, {}, {}, 0,
          "Async HTTP transport failed with an unknown exception", {}});
    }
#else
    asyncTransport_->sendAsync(req, std::move(complete));
#endif
  } else {
    deliver(outcomeFromHttp(sendInline(req)));
  }
}

}  // namespace crowdy::graphql
