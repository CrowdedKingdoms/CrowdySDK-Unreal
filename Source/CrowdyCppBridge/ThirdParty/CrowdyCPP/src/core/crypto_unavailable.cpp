#include "crowdy/core/crypto.hpp"

namespace crowdy::core {

namespace {

class UnavailableCrypto final : public ICrypto {
 public:
  bool hmacSha256(Bytes, Bytes, std::uint8_t*) const override {
    return false;
  }
  bool sha256(Bytes, std::uint8_t*) const override { return false; }
  bool constantTimeEquals(const std::uint8_t*, const std::uint8_t*,
                          std::size_t) const override {
    return false;
  }
  bool randomBytes(std::uint8_t*, std::size_t) const override {
    return false;
  }
  Status availability() const override {
    return Errc::CryptoUnavailable;
  }
};

}  // namespace

const ICrypto& unavailableCrypto() {
  static UnavailableCrypto crypto;
  return crypto;
}

}  // namespace crowdy::core
