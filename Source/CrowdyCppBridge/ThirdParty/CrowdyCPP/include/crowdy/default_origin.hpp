// GENERATED — do not edit. The operator tooling's
// `sync-client-origins.mjs --write --tier dev` writes this file, and
// `check-sdk-default-origin.mjs` refuses it when it names the wrong tier or a
// host that declaration does not carry. Regenerate; never hand-edit.
//
// THE DEFAULT IS LOAD-BEARING DURING A ROLLOUT: while the branches are
// mid-migration this is what an unconfigured consumer gets, so a WRONG default on
// one branch is worse than no default at all. The literal that used to live in
// tests/prodsmoke/main.cpp named a host retired on 2026-08-19 and nothing said so.
//
// Source: the operator's per-tier public CK API origin declaration, tier 'dev'
#ifndef CROWDY_DEFAULT_ORIGIN_HPP
#define CROWDY_DEFAULT_ORIGIN_HPP

namespace crowdy {

/// The tier this build of the SDK is released for.
inline constexpr const char* kDefaultTier = "dev";

/// The public CK API origin for that tier.
inline constexpr const char* kDefaultHttpOrigin = "https://ck.dev.crowdedkingdoms.com";

/// The same host over WebSocket. A scheme is composed; a hostname is looked up.
inline constexpr const char* kDefaultWsOrigin = "wss://ck.dev.crowdedkingdoms.com";

/// The bare hostname, for callers that need to compare rather than dial.
inline constexpr const char* kDefaultHost = "ck.dev.crowdedkingdoms.com";

}  // namespace crowdy

#endif  // CROWDY_DEFAULT_ORIGIN_HPP
