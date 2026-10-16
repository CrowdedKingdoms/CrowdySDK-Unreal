#include "crowdy/kit/protocol.hpp"

#include <cctype>
#include <charconv>
#include <random>

namespace crowdy::kit {

namespace {

bool isReserved(std::string_view key) { return key == "v" || key == "epoch" || key == "seq" || key == "kind"; }

std::optional<KitMessageKind> kindOf(std::string_view name) {
  if (name == "snapshot") return KitMessageKind::Snapshot;
  if (name == "delta") return KitMessageKind::Delta;
  if (name == "changed") return KitMessageKind::Changed;
  if (name == "event") return KitMessageKind::Event;
  return std::nullopt;
}

/// A JSON string literal for `text`, escaped.
std::string quoted(std::string_view text) { return graphql::JVal(std::string(text)).dump(); }

bool codeChar(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }

std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
  std::int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
  return q;
}

}  // namespace

std::string_view kitMessageKindName(KitMessageKind kind) noexcept {
  switch (kind) {
    case KitMessageKind::Snapshot: return "snapshot";
    case KitMessageKind::Delta: return "delta";
    case KitMessageKind::Changed: return "changed";
    case KitMessageKind::Event: return "event";
  }
  return "snapshot";
}

std::optional<KitHeader> kitHeader(const graphql::Json& value) {
  if (!value.isObject()) return std::nullopt;
  const graphql::Json v = value["v"], epoch = value["epoch"], seq = value["seq"], kind = value["kind"];
  if (!v.isNumber() || !epoch.isNumber() || !seq.isNumber() || !kind.isString()) return std::nullopt;
  const auto k = kindOf(kind.asStringView());
  if (!k) return std::nullopt;
  return KitHeader{static_cast<int>(v.asInt64()), epoch.asInt64(), seq.asInt64(), *k};
}

std::optional<KitMessage> splitKitMessage(const graphql::Json& value) {
  auto header = kitHeader(value);
  if (!header) return std::nullopt;
  std::string text = "{";
  bool first = true;
  value.forEachMember([&](std::string_view key, const graphql::Json& member) {
    if (isReserved(key)) return;
    if (!first) text.push_back(',');
    first = false;
    text += quoted(key);
    text.push_back(':');
    text += member.dump();
  });
  text.push_back('}');
  return KitMessage{*header, graphql::Json::parse(text)};
}

std::optional<KitChanged> kitChanged(const graphql::Json& body) {
  if (!body.isObject() || !body["what"].isString()) return std::nullopt;
  KitChanged out;
  out.what = body["what"].asString();
  body["who"].forEach([&](const graphql::Json& who) { out.who.push_back(who.asBigIntString()); });
  return out;
}

std::optional<KitRefusal> parseKitRefusal(std::string_view message) {
  constexpr std::string_view prefix = "kit:";
  if (message.substr(0, prefix.size()) != prefix) return std::nullopt;
  const std::string_view rest = message.substr(prefix.size());
  const std::size_t colon = rest.find(": ");
  if (colon == std::string_view::npos || colon == 0 || colon > 64) return std::nullopt;
  const std::string_view code = rest.substr(0, colon);
  for (char c : code) {
    if (!codeChar(c)) return std::nullopt;
  }
  return KitRefusal{std::string(code), std::string(rest.substr(colon + 2))};
}

KitReply KitReply::from(domains::ExecReply reply) {
  KitReply out{reply.status, std::move(reply.payload), std::nullopt};
  if (out.status == domains::ExecStatus::AppError) out.refusal = parseKitRefusal(out.payload);
  return out;
}

KitReply KitReply::refused(std::string_view code, std::string_view text) {
  std::string message = "kit:" + std::string(code) + ": " + std::string(text);
  return from(domains::ExecReply{domains::ExecStatus::AppError, std::move(message)});
}

std::string newActionId() {
  thread_local std::mt19937_64 rng{std::random_device{}() ^ (static_cast<std::uint64_t>(std::random_device{}()) << 32)};
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  for (int half = 0; half < 2; ++half) {
    std::uint64_t bits = rng();
    for (int i = 0; i < 16; ++i) {
      out.push_back(digits[bits & 0xf]);
      bits >>= 4;
    }
  }
  return out;
}

graphql::JVal withActionId(graphql::JVal args, std::string_view actionId) {
  if (args.isNull()) return graphql::JVal::object({{"action_id", graphql::JVal(std::string(actionId))}});
  if (args.isObject()) args["action_id"] = graphql::JVal(std::string(actionId));
  return args;
}

std::pair<std::int64_t, std::int64_t> shardCentre(std::int64_t x, std::int64_t z, std::int64_t size) {
  const std::int64_t s = size < 1 ? 1 : size;
  const std::int64_t half = s / 2;
  return {floorDiv(x, s) * s + half, floorDiv(z, s) * s + half};
}

std::string shardKey(std::int64_t x, std::int64_t z, std::int64_t size) {
  const auto [cx, cz] = shardCentre(x, z, size);
  return std::to_string(cx) + "," + std::to_string(cz);
}

std::optional<std::pair<std::int64_t, std::int64_t>> parseShardKey(std::string_view key) {
  auto trim = [](std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
  };
  const std::size_t comma = key.find(',');
  if (comma == std::string_view::npos) return std::nullopt;
  auto number = [](std::string_view s) -> std::optional<std::int64_t> {
    if (s.empty()) return std::nullopt;
    std::int64_t v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || end != s.data() + s.size()) return std::nullopt;
    return v;
  };
  const auto x = number(trim(key.substr(0, comma)));
  const auto z = number(trim(key.substr(comma + 1)));
  if (!x || !z) return std::nullopt;
  return std::make_pair(*x, *z);
}

std::vector<std::string> shardKeysAround(std::int64_t x, std::int64_t z, std::int64_t size, int radius) {
  const std::int64_t s = size < 1 ? 1 : size;
  const auto [cx, cz] = shardCentre(x, z, s);
  std::vector<std::string> keys{std::to_string(cx) + "," + std::to_string(cz)};
  for (int dz = -radius; dz <= radius; ++dz) {
    for (int dx = -radius; dx <= radius; ++dx) {
      if (dx != 0 || dz != 0) keys.push_back(std::to_string(cx + dx * s) + "," + std::to_string(cz + dz * s));
    }
  }
  return keys;
}

}  // namespace crowdy::kit
