#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

/// Bound on where a client may be MOVED to.
///
/// A move target arrives from the server — a WRONG_DATACENTER redirect, a
/// re-discovery answer, or a directed reconnect — over an authenticated TLS
/// connection, so this is not about whether the server is who it claims. It
/// bounds what a server may ASK for: one compromised instance must not be able
/// to walk an entire fleet's clients onto an origin outside the estate, which
/// would be far worse than the unbalanced fleet a redirect exists to fix.
///
/// Mirrors CrowdyJS's isSameEstate (src/binary-relay.ts) exactly, including its
/// deliberate limits: the comparison is the last two dot-separated labels, not a
/// public-suffix lookup, so it is a bound rather than a proof of ownership.
namespace crowdy::graphql {

/// Lowercased hostname of an absolute URL, or nullopt when it will not parse.
inline std::optional<std::string> estateHostname(std::string_view url) {
  const std::size_t scheme = url.find("://");
  if (scheme == std::string_view::npos) return std::nullopt;
  std::string_view rest = url.substr(scheme + 3);

  // Strip userinfo before the host, so a URL like wss://evil.com@ck.example.com
  // cannot present its userinfo as the host (or hide the real one).
  const std::size_t at = rest.find('@');
  const std::size_t firstSlash = rest.find('/');
  if (at != std::string_view::npos &&
      (firstSlash == std::string_view::npos || at < firstSlash)) {
    rest = rest.substr(at + 1);
  }

  std::size_t end = rest.size();
  for (std::size_t i = 0; i < rest.size(); ++i) {
    const char c = rest[i];
    if (c == '/' || c == '?' || c == '#') {
      end = i;
      break;
    }
  }
  std::string_view host = rest.substr(0, end);

  // An IPv6 literal keeps its brackets; otherwise a colon starts the port.
  if (!host.empty() && host.front() == '[') {
    const std::size_t close = host.find(']');
    if (close == std::string_view::npos) return std::nullopt;
    host = host.substr(0, close + 1);
  } else {
    const std::size_t colon = host.find(':');
    if (colon != std::string_view::npos) host = host.substr(0, colon);
  }
  if (host.empty()) return std::nullopt;

  std::string lowered(host);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char c) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
  });
  return lowered;
}

/// True when `candidate` may be moved to from `current`.
///
/// Accepts an identical host, or two hosts sharing their last two labels
/// (`ck-api-or-4.prod.crowdedkingdoms.com` and `ck.prod.crowdedkingdoms.com`
/// both reduce to `crowdedkingdoms.com`). Refuses anything that will not parse,
/// rather than guessing.
/// A single-label host such as `localhost` matches only itself, which is what
/// keeps `localhost` from pairing with `otherhost` in local development.
inline bool isSameEstate(std::string_view current, std::string_view candidate) {
  const auto a = estateHostname(current);
  const auto b = estateHostname(candidate);
  if (!a || !b) return false;
  if (*a == *b) return true;

  const auto site = [](const std::string& host) -> std::string {
    const std::size_t last = host.rfind('.');
    if (last == std::string::npos) return host;
    const std::size_t prior = host.rfind('.', last - 1);
    return prior == std::string::npos ? host : host.substr(prior + 1);
  };
  const std::string siteA = site(*a);
  // The `.` requirement is what refuses a lookalike: matching on a single label
  // would let ANY two-label pair agree, and matching without it would make a
  // bare host share an estate with every other bare host.
  return siteA == site(*b) && siteA.find('.') != std::string::npos;
}

}  // namespace crowdy::graphql
