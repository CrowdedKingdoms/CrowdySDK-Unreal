#pragma once

#include <cstddef>
#include <cstdint>

/// Pluggable clock so engine wrappers can drive SDK timing from engine time
/// (and tests can use a fake clock).
namespace crowdy::core {

class IClock {
 public:
  virtual ~IClock() = default;
  /// Wall-clock milliseconds since the Unix epoch.
  virtual std::int64_t epochMillis() const = 0;
  /// Monotonic milliseconds (arbitrary origin) for intervals and timeouts.
  virtual std::int64_t monotonicMillis() const = 0;
};

const IClock& systemClock();

/// Parse an ISO-8601 UTC timestamp ("2026-06-12T18:00:00.000Z") into epoch
/// milliseconds. Returns 0 on parse failure or empty input.
std::int64_t parseIso8601Millis(const char* text, std::size_t len);

}  // namespace crowdy::core
