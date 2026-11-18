#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "crowdy/default_origin.hpp"

namespace crowdy {

/// The Studio page a dock sends a player to when the model needs funds
/// (`AGENT_FUNDS_NEEDED`): the wallet, under the Studio origin.
inline constexpr std::string_view kStudioWalletPath = "/account/wallet";

/// This tier's Crowdy Studio as the platform names it (`crowdyStudioLinks`, Game API v2.73.0;
/// `crowdyStudio().links()`): `origin` is the only origin a Studio dock opens a popup to,
/// `walletUrl` where it sends a player when the model needs funds. Prefer it to
/// `studioOriginFor`, which derives the same from a host-name convention without asking.
struct CrowdyStudioLinks {
  std::string origin;
  std::string walletUrl;
};

/// The Crowdy Studio of the tier an API origin (or endpoint) belongs to. The API answers
/// on `ck.<root>`, and per datacenter or instance on `ck-<label>.<root>` (the game API URL
/// a token is minted with), and Studio on `studio.<root>`: the convention CrowdyJS's
/// `defaultHostedSignInUrl` leans on for `ck.` hosts. So `https://ck.dev.crowdedkingdoms.com`
/// and `https://ck-va.dev.crowdedkingdoms.com` give `https://studio.dev.crowdedkingdoms.com`;
/// a local API (`localhost`, `127.0.0.1`) gives Studio on port 3001. Any other host follows
/// no convention: nullopt. The answer is only as trustworthy as `apiOrigin`: pass the
/// endpoint the game was configured with (CrowdyClient::studioOrigin() does), never a URL a
/// player or a page supplied.
inline std::optional<std::string> studioOriginFor(std::string_view apiOrigin) {
  const auto lower = [](std::string_view text) {
    std::string out(text);
    for (char& c : out) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
  };
  std::string scheme = "https";
  std::string_view rest = apiOrigin;
  const std::size_t separator = rest.find("://");
  if (separator != std::string_view::npos) {
    scheme = lower(rest.substr(0, separator));
    rest.remove_prefix(separator + 3);
  }
  rest = rest.substr(0, rest.find_first_of("/?#"));
  const std::size_t at = rest.rfind('@');
  if (at != std::string_view::npos) rest.remove_prefix(at + 1);
  const std::size_t port = rest.rfind(':');
  const std::string host = lower(port == std::string_view::npos ? rest : rest.substr(0, port));
  if (host == "localhost" || host == "127.0.0.1") return scheme + "://" + host + ":3001";
  const std::size_t dot = host.find('.');
  if (dot == std::string::npos || dot + 1 >= host.size()) return std::nullopt;
  const std::string_view label = std::string_view(host).substr(0, dot);
  if (label != "ck" && label.substr(0, 3) != "ck-") return std::nullopt;
  return "https://studio." + host.substr(dot + 1);
}

/// The Studio of the tier this build of the SDK is released for (kDefaultHttpOrigin's).
inline std::string defaultStudioOrigin() {
  return studioOriginFor(kDefaultHttpOrigin).value_or(std::string());
}

/// The wallet page of the Studio at `studioOrigin`.
inline std::string studioWalletUrl(std::string_view studioOrigin) {
  std::string url(studioOrigin);
  while (!url.empty() && url.back() == '/') url.pop_back();
  url += kStudioWalletPath;
  return url;
}

}  // namespace crowdy
