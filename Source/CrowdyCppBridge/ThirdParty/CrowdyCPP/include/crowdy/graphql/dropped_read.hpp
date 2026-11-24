#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "crowdy/graphql/errors.hpp"
#include "crowdy/graphql/graphql_client.hpp"

/// Which failed reads the SDK's own waits read again: the Studio runner, the Studio
/// controller's runtime and ExecAPI::waitForModBuild. Not an API for games, which keep
/// their own retry policies. CrowdyJS and CrowdyPy apply the same rule; change all three
/// together.
namespace crowdy::graphql::detail {

/// A refusal the API marks retryable and blames on the platform (`PLATFORM_BUSY`).
/// `retryable` defaults to true, so it is read with `blame`.
inline bool isBusyRefusal(const GraphQLErrorDetail& error) {
  return error.retryable && error.blame == "PLATFORM";
}

/// Whether a failed read is worth reading again: no answer (a network failure or a
/// timeout), an answer that is not the API's (an HTTP 5xx or 429 without GraphQL errors,
/// or a body that does not parse, unless it was over the response limit), or a busy
/// refusal. A GraphQL refusal, any other HTTP status and the caller's own cancel are
/// answers, and reading again would only repeat them.
inline bool isDroppedRead(const GraphQLOutcome& outcome) {
  switch (outcome.kind) {
    case GraphQLErrorKind::Network:
    case GraphQLErrorKind::Timeout:
      return true;
    case GraphQLErrorKind::Protocol:
      return outcome.status.code != Errc::ResponseTooLarge;
    case GraphQLErrorKind::Http:
      return outcome.httpStatus >= 500 || outcome.httpStatus == 429;
    case GraphQLErrorKind::GraphQL:
      return !outcome.errors.empty() && isBusyRefusal(outcome.errors.front());
    case GraphQLErrorKind::None:
    case GraphQLErrorKind::Cancelled:
      return false;
  }
  return false;
}

/// How long a busy refusal asked to wait before the next read (`extensions.retryAfterMs`),
/// or 0.
inline std::int64_t retryAfterMsOf(const GraphQLOutcome& outcome) {
  if (outcome.kind != GraphQLErrorKind::GraphQL || outcome.errors.empty()) return 0;
  const auto& after = outcome.errors.front().retryAfterMs;
  return after && *after > 0 ? *after : 0;
}

/// What a dropped or refused read reports, for a run's or a controller's message.
inline std::string failureOf(const GraphQLOutcome& outcome) {
  if (outcome.kind == GraphQLErrorKind::GraphQL && !outcome.errors.empty()) {
    return outcome.errors.front().message;
  }
  if (outcome.kind == GraphQLErrorKind::Http) return "HTTP " + std::to_string(outcome.httpStatus);
  return outcome.errorMessage.empty() ? std::string("the request failed") : outcome.errorMessage;
}

#ifndef CROWDY_NO_EXCEPTIONS
/// isDroppedRead for the exception being handled, as GraphQLClient::request throws it.
/// Call it only inside a catch block. `retryAfterMs`, when given, receives a busy
/// refusal's wait (0 otherwise).
inline bool isDroppedReadException(std::int64_t* retryAfterMs = nullptr) {
  if (retryAfterMs) *retryAfterMs = 0;
  try {
    throw;
  } catch (const CrowdyNetworkError&) {
    return true;
  } catch (const CrowdyTimeoutError&) {
    return true;
  } catch (const CrowdyHttpError& error) {
    return error.status() >= 500 || error.status() == 429;
  } catch (const CrowdyGraphQLError& error) {
    if (error.errors().empty() || !isBusyRefusal(error.errors().front())) return false;
    const auto& after = error.errors().front().retryAfterMs;
    if (retryAfterMs && after && *after > 0) *retryAfterMs = *after;
    return true;
  } catch (const CrowdyProtocolError& error) {
    // GraphQLClient::request throws this message for Errc::ResponseTooLarge.
    return std::string_view(error.what()).rfind("GraphQL response larger than", 0) != 0;
  } catch (...) {
    return false;
  }
}
#endif

}  // namespace crowdy::graphql::detail
