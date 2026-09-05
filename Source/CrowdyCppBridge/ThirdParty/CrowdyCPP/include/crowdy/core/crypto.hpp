#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "crowdy/core/bytes.hpp"
#include "crowdy/core/result.hpp"

/// Pluggable crypto provider. The SDK needs exactly four primitives:
/// HMAC-SHA256, constant-time comparison, and random bytes. The default
/// implementation (crowdy::core::opensslCrypto()) is backed by OpenSSL's
/// libcrypto; engine wrappers may substitute their own (e.g. an engine's
/// bundled crypto module) by implementing this interface.
namespace crowdy::core {

/// A MAC bound to one key, reusable for many messages.
///
/// The one-shot `ICrypto::hmacSha256` re-imports the key on every call. On the
/// replication path one token signs every datagram for the life of a session,
/// so that setup, not the hashing, is the dominant per-datagram cost: measured
/// at 1241 ns for the one-shot call against 311 ns for a pre-keyed context over
/// the same 220 bytes. Providers that can hold a keyed context should offer one.
class IMac {
 public:
  virtual ~IMac() = default;

  /// MAC over the concatenation of `parts`, which is never materialised.
  /// `out` must hold ICrypto::kHmacTagSize bytes.
  ///
  /// Must be safe to call concurrently: the SDK signs from whichever thread
  /// the game calls a send on, and verifies on the network thread.
  virtual bool compute(const Bytes* parts, std::size_t count, std::uint8_t* out) const = 0;
};

class ICrypto {
 public:
  virtual ~ICrypto() = default;

  static constexpr std::size_t kHmacTagSize = 32;

  /// out must hold 32 bytes. Returns false on provider failure.
  virtual bool hmacSha256(Bytes key, Bytes message, std::uint8_t* out) const = 0;

  /// A reusable HMAC-SHA256 bound to `key`, for a key that will sign or verify
  /// many messages.
  ///
  /// Returning nullptr is not a failure and is the default: callers fall back
  /// to hmacSha256, so a provider written before this existed keeps working
  /// unchanged and merely does not get the speedup.
  virtual std::shared_ptr<IMac> makeHmacSha256(Bytes key) const {
    (void)key;
    return nullptr;
  }

  /// Plain SHA-256 (used by the PKCE portal flow). out must hold 32 bytes.
  virtual bool sha256(Bytes message, std::uint8_t* out) const = 0;

  /// Constant-time equality of two 32-byte tags.
  virtual bool constantTimeEquals(const std::uint8_t* a, const std::uint8_t* b,
                                  std::size_t len) const = 0;

  /// Cryptographically secure random bytes (used for actor UUIDs and PKCE).
  virtual bool randomBytes(std::uint8_t* out, std::size_t len) const = 0;

  /// Provider availability. Injected providers remain source-compatible and
  /// are assumed available unless they override this. The built-in unavailable
  /// provider returns Errc::CryptoUnavailable.
  virtual Status availability() const { return Errc::Ok; }
};

/// Explicit provider used when no platform crypto implementation is linked.
/// Every primitive fails and availability() reports CryptoUnavailable.
const ICrypto& unavailableCrypto();

/// OpenSSL provider. When CROWDY_WITH_OPENSSL is disabled this resolves to the
/// explicit unavailable provider instead of leaving an unresolved symbol;
/// check availability() when provider selection is dynamic.
const ICrypto& opensslCrypto();

/// Build-selected convenience provider: OpenSSL when enabled, otherwise the
/// explicit unavailable provider. Security-sensitive APIs should prefer
/// injected ICrypto ownership so unavailability remains visible to callers.
const ICrypto& defaultCrypto();

}  // namespace crowdy::core
