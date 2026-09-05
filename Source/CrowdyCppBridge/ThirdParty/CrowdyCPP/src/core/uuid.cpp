#include "crowdy/core/uuid.hpp"

#include <cstring>

namespace crowdy::core {

Result<ActorUuid> tryGenerateActorUuid(const ICrypto& crypto) {
  const Status availability = crypto.availability();
  if (!availability.ok()) return availability.code;

  static constexpr char kHex[] = "0123456789abcdef";
  std::uint8_t raw[16]{};
  if (!crypto.randomBytes(raw, sizeof(raw))) return Errc::CryptoUnavailable;
  ActorUuid out{};
  for (std::size_t i = 0; i < 16; ++i) {
    out[i * 2] = kHex[raw[i] >> 4];
    out[i * 2 + 1] = kHex[raw[i] & 0x0f];
  }
  return out;
}

ActorUuid generateActorUuid(const ICrypto& crypto) {
  auto generated = tryGenerateActorUuid(crypto);
  return generated.ok() ? generated.value() : ActorUuid{};
}

bool actorUuidFromString(std::string_view s, ActorUuid& out) {
  if (s.size() != out.size()) return false;
  std::memcpy(out.data(), s.data(), out.size());
  return true;
}

}  // namespace crowdy::core
