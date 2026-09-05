#include "crowdy/core/base64.hpp"

namespace crowdy::core {

namespace {
constexpr char kStd[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char kUrl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string encodeWith(Bytes data, const char* alphabet, bool pad) {
  std::string out;
  out.reserve(((data.size() + 2) / 3) * 4);
  std::size_t i = 0;
  while (i + 3 <= data.size()) {
    const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                            (static_cast<std::uint32_t>(data[i + 1]) << 8) | data[i + 2];
    out.push_back(alphabet[(n >> 18) & 63]);
    out.push_back(alphabet[(n >> 12) & 63]);
    out.push_back(alphabet[(n >> 6) & 63]);
    out.push_back(alphabet[n & 63]);
    i += 3;
  }
  const std::size_t rem = data.size() - i;
  if (rem == 1) {
    const std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
    out.push_back(alphabet[(n >> 18) & 63]);
    out.push_back(alphabet[(n >> 12) & 63]);
    if (pad) out += "==";
  } else if (rem == 2) {
    const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                            (static_cast<std::uint32_t>(data[i + 1]) << 8);
    out.push_back(alphabet[(n >> 18) & 63]);
    out.push_back(alphabet[(n >> 12) & 63]);
    out.push_back(alphabet[(n >> 6) & 63]);
    if (pad) out.push_back('=');
  }
  return out;
}

int decodeChar(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

}  // namespace

std::string base64Encode(Bytes data) { return encodeWith(data, kStd, true); }
std::string base64UrlEncode(Bytes data) { return encodeWith(data, kUrl, false); }

std::optional<std::vector<std::uint8_t>> base64Decode(std::string_view text) {
  std::vector<std::uint8_t> out;
  out.reserve((text.size() / 4) * 3);
  std::uint32_t acc = 0;
  int bits = 0;
  for (char c : text) {
    if (c == '=' || c == '\n' || c == '\r') continue;
    const int v = decodeChar(c);
    if (v < 0) return std::nullopt;
    acc = (acc << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xff));
    }
  }
  return out;
}

}  // namespace crowdy::core
