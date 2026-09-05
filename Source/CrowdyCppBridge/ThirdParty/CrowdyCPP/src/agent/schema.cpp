#include "crowdy/agent/schema.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <unordered_set>
#include <utility>
#include <vector>

namespace crowdy::agent {

namespace {

constexpr std::size_t kDefaultMaxDepth = 12;
constexpr std::size_t kDefaultMaxNodes = 4'096;
constexpr std::size_t kDefaultMaxBytes = 1'048'576;

std::string normalizedField(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const unsigned char character : value) {
    if ((character >= 'A' && character <= 'Z') ||
        (character >= 'a' && character <= 'z') ||
        (character >= '0' && character <= '9')) {
      result.push_back(static_cast<char>(
          character >= 'A' && character <= 'Z' ? character + 32
                                                : character));
    }
  }
  return result;
}

const std::unordered_set<std::string>& forbiddenFields() {
  static const auto fields = [] {
    std::unordered_set<std::string> values;
    for (const auto field : kForbiddenAgentAuthorityFields) {
      values.insert(normalizedField(field));
    }
    return values;
  }();
  return fields;
}

[[noreturn]] void validationFailure(AgentSchemaDirection direction,
                                    std::string field,
                                    std::string message) {
  std::string code = "AGENT_TOOL_INPUT_INVALID";
  if (direction == AgentSchemaDirection::Output) {
    code = "AGENT_TOOL_OUTPUT_INVALID";
  } else if (direction == AgentSchemaDirection::Schema) {
    code = "AGENT_TOOL_DESCRIPTOR_INVALID";
  }
  throw CrowdyAgentError(std::move(code),
                         field + ": " + std::move(message), false,
                         std::nullopt, std::move(field));
}

void appendCanonical(std::string& output, const graphql::Json& value) {
  if (!value.ok() || value.isNull()) {
    output += "null";
    return;
  }
  if (value.isArray()) {
    output.push_back('[');
    for (std::size_t index = 0; index < value.size(); ++index) {
      if (index != 0) output.push_back(',');
      appendCanonical(output, value.at(index));
    }
    output.push_back(']');
    return;
  }
  if (value.isObject()) {
    std::vector<std::pair<std::string, graphql::Json>> members;
    members.reserve(value.size());
    value.forEachMember([&](std::string_view key, const graphql::Json& child) {
      members.emplace_back(key, child);
    });
    std::sort(members.begin(), members.end(),
              [](const auto& left, const auto& right) {
                return left.first < right.first;
              });
    output.push_back('{');
    for (std::size_t index = 0; index < members.size(); ++index) {
      if (index != 0) output.push_back(',');
      output += graphql::JVal(members[index].first).dump();
      output.push_back(':');
      appendCanonical(output, members[index].second);
    }
    output.push_back('}');
    return;
  }
  if (value.isNumber() && value.asDouble() == 0.0) {
    output.push_back('0');
    return;
  }
  output += value.dump();
}

constexpr std::array<std::uint32_t, 64> kShaConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
static_assert(kShaConstants[50] == 0x2748774cU);

std::uint32_t rotateRight(std::uint32_t value, unsigned bits) {
  return (value >> bits) | (value << (32U - bits));
}

std::array<std::uint8_t, 32> sha256(std::string_view input) {
  const auto inputSize = input.size();
  const auto paddedSize = ((inputSize + 9U + 63U) / 64U) * 64U;
  std::vector<std::uint8_t> bytes(paddedSize);
  std::memcpy(bytes.data(), input.data(), inputSize);
  bytes[inputSize] = 0x80U;
  const auto bitLength = static_cast<std::uint64_t>(inputSize) * 8U;
  for (std::size_t index = 0; index < 8; ++index) {
    bytes[paddedSize - 1U - index] =
        static_cast<std::uint8_t>(bitLength >> (index * 8U));
  }
  std::array<std::uint32_t, 8> hash = {
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::array<std::uint32_t, 64> words{};
  for (std::size_t offset = 0; offset < paddedSize; offset += 64U) {
    for (std::size_t index = 0; index < 16; ++index) {
      const auto base = offset + index * 4U;
      words[index] =
          (static_cast<std::uint32_t>(bytes[base]) << 24U) |
          (static_cast<std::uint32_t>(bytes[base + 1U]) << 16U) |
          (static_cast<std::uint32_t>(bytes[base + 2U]) << 8U) |
          static_cast<std::uint32_t>(bytes[base + 3U]);
    }
    for (std::size_t index = 16; index < 64; ++index) {
      const auto left = words[index - 15U];
      const auto right = words[index - 2U];
      const auto s0 =
          rotateRight(left, 7U) ^ rotateRight(left, 18U) ^ (left >> 3U);
      const auto s1 =
          rotateRight(right, 17U) ^ rotateRight(right, 19U) ^ (right >> 10U);
      words[index] =
          words[index - 16U] + s0 + words[index - 7U] + s1;
    }
    auto a = hash[0];
    auto b = hash[1];
    auto c = hash[2];
    auto d = hash[3];
    auto e = hash[4];
    auto f = hash[5];
    auto g = hash[6];
    auto h = hash[7];
    for (std::size_t index = 0; index < 64; ++index) {
      const auto sum1 =
          rotateRight(e, 6U) ^ rotateRight(e, 11U) ^ rotateRight(e, 25U);
      const auto choice = (e & f) ^ ((~e) & g);
      const auto temp1 =
          h + sum1 + choice + kShaConstants[index] + words[index];
      const auto sum0 =
          rotateRight(a, 2U) ^ rotateRight(a, 13U) ^ rotateRight(a, 22U);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temp2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
    hash[5] += f;
    hash[6] += g;
    hash[7] += h;
  }
  std::array<std::uint8_t, 32> result{};
  for (std::size_t index = 0; index < hash.size(); ++index) {
    result[index * 4U] = static_cast<std::uint8_t>(hash[index] >> 24U);
    result[index * 4U + 1U] =
        static_cast<std::uint8_t>(hash[index] >> 16U);
    result[index * 4U + 2U] =
        static_cast<std::uint8_t>(hash[index] >> 8U);
    result[index * 4U + 3U] = static_cast<std::uint8_t>(hash[index]);
  }
  return result;
}

}  // namespace

std::string canonicalJson(const graphql::Json& value) {
  if (!value.ok()) {
    throw CrowdyAgentError("AGENT_TOOL_INPUT_INVALID",
                           "Cannot canonicalize invalid JSON");
  }
  std::string output;
  output.reserve(value.dump().size());
  appendCanonical(output, value);
  return output;
}

std::string sha256Digest(std::string_view value) {
  static constexpr char hex[] = "0123456789abcdef";
  const auto bytes = sha256(value);
  std::string output = "sha256:";
  output.reserve(71);
  for (const auto byte : bytes) {
    output.push_back(hex[byte >> 4U]);
    output.push_back(hex[byte & 0x0fU]);
  }
  return output;
}

std::string digestCanonicalJson(const graphql::Json& value) {
  return sha256Digest(canonicalJson(value));
}

bool isSha256Digest(std::string_view value) {
  if (value.size() != 71 || value.substr(0, 7) != "sha256:") return false;
  return std::all_of(value.begin() + 7, value.end(),
                     [](unsigned char character) {
                       return (character >= '0' && character <= '9') ||
                              (character >= 'a' && character <= 'f');
                     });
}

bool isDecimalString(std::string_view value, bool allowNegative) {
  if (value.empty()) return false;
  std::size_t offset = 0;
  if (value.front() == '-') {
    if (!allowNegative) return false;
    offset = 1;
  }
  if (offset == value.size()) return false;
  if (value[offset] == '0') return offset + 1 == value.size();
  if (value[offset] < '1' || value[offset] > '9') return false;
  return std::all_of(value.begin() + static_cast<std::ptrdiff_t>(offset + 1),
                     value.end(), [](unsigned char character) {
                       return character >= '0' && character <= '9';
                     });
}

namespace {

bool isSupportedPattern(std::string_view pattern) {
  static constexpr std::array<std::string_view, 5> patterns = {
      "^(0|[1-9][0-9]*)$",
      "^(?!/)(?!.*(?:^|/)\\.\\.?(?:/|$))(?!.*[\\u0000-\\u001f\\u007f])[^\\\\]+$",
      "^(ABSENT|sha256:[0-9a-f]{64})$",
      "^-?(0|[1-9][0-9]*)(\\.[0-9]{1,9})?$",
      "^sha256:[0-9a-f]{64}$",
  };
  return std::find(patterns.begin(), patterns.end(), pattern) !=
         patterns.end();
}

bool matchesPattern(std::string_view pattern, std::string_view value) {
  if (pattern == "^(0|[1-9][0-9]*)$") {
    return isDecimalString(value, false);
  }
  if (pattern == "^sha256:[0-9a-f]{64}$") {
    return isSha256Digest(value);
  }
  if (pattern == "^(ABSENT|sha256:[0-9a-f]{64})$") {
    return value == "ABSENT" || isSha256Digest(value);
  }
  if (pattern ==
      "^(?!/)(?!.*(?:^|/)\\.\\.?(?:/|$))(?!.*[\\u0000-\\u001f\\u007f])[^\\\\]+$") {
    if (value.empty() || value.front() == '/' ||
        value.find('\\') != std::string_view::npos) {
      return false;
    }
    std::size_t start = 0;
    while (start <= value.size()) {
      const auto end = value.find('/', start);
      const auto component =
          value.substr(start, end == std::string_view::npos
                                  ? value.size() - start
                                  : end - start);
      if (component == "." || component == "..") return false;
      if (end == std::string_view::npos) break;
      start = end + 1;
    }
    return std::none_of(value.begin(), value.end(),
                        [](unsigned char character) {
                          return character < 0x20 || character == 0x7f;
                        });
  }
  if (pattern == "^-?(0|[1-9][0-9]*)(\\.[0-9]{1,9})?$") {
    std::size_t offset = !value.empty() && value.front() == '-' ? 1 : 0;
    const auto dot = value.find('.', offset);
    const auto integerPart =
        value.substr(0, dot == std::string_view::npos ? value.size() : dot);
    if (!isDecimalString(integerPart, true)) return false;
    if (dot == std::string_view::npos) return true;
    const auto fraction = value.substr(dot + 1);
    return !fraction.empty() && fraction.size() <= 9 &&
           std::all_of(fraction.begin(), fraction.end(),
                       [](unsigned char character) {
                         return character >= '0' && character <= '9';
                       });
  }
  return false;
}

bool isDateTime(std::string_view value) {
  if (value.size() < 20 || value.size() > 40 || value.back() != 'Z') {
    return false;
  }
  constexpr std::array<std::size_t, 6> separators = {4, 7, 10, 13, 16, 19};
  constexpr std::array<char, 6> expected = {'-', '-', 'T', ':', ':', 'Z'};
  for (std::size_t index = 0; index < separators.size(); ++index) {
    const auto position = separators[index];
    if (position >= value.size()) return false;
    if (index == 5) {
      if (value[position] != 'Z' && value[position] != '.') return false;
    } else if (value[position] != expected[index]) {
      return false;
    }
  }
  for (std::size_t index = 0; index < 19; ++index) {
    if (index == 4 || index == 7 || index == 10 || index == 13 ||
        index == 16) {
      continue;
    }
    if (value[index] < '0' || value[index] > '9') return false;
  }
  if (value[19] == 'Z') return value.size() == 20;
  if (value[19] != '.') return false;
  if (value.size() < 22 || value.size() > 30 || value.back() != 'Z') {
    return false;
  }
  return std::all_of(value.begin() + 20, value.end() - 1,
                     [](unsigned char character) {
                       return character >= '0' && character <= '9';
                     });
}

std::vector<std::string> objectKeys(const graphql::Json& object) {
  std::vector<std::string> keys;
  keys.reserve(object.size());
  object.forEachMember([&](std::string_view key, const graphql::Json&) {
    keys.emplace_back(key);
  });
  return keys;
}

void inspectSchema(const graphql::Json& schema, std::string path,
                   std::size_t depth, std::size_t* nodes,
                   bool rejectAuthorityFields) {
  if (!schema.isObject()) {
    validationFailure(AgentSchemaDirection::Schema, std::move(path),
                      "schema node must be an object");
  }
  if (depth > kDefaultMaxDepth || ++*nodes > kDefaultMaxNodes) {
    validationFailure(AgentSchemaDirection::Schema, std::move(path),
                      "schema is outside depth/node bounds");
  }
  const auto type = schema["type"].asString();
  if (type != "object" && type != "array" && type != "string" &&
      type != "number" && type != "integer" && type != "boolean") {
    validationFailure(AgentSchemaDirection::Schema, std::move(path),
                      "unsupported schema type");
  }
  const auto enumeration = schema["enum"];
  if (enumeration.ok() && !enumeration.isNull()) {
    if (!enumeration.isArray() || enumeration.size() == 0 ||
        enumeration.size() > 128) {
      validationFailure(AgentSchemaDirection::Schema, path,
                        "enum must contain 1 to 128 values");
    }
    std::set<std::string> values;
    enumeration.forEach([&](const graphql::Json& entry) {
      if (!values.insert(canonicalJson(entry)).second) {
        validationFailure(AgentSchemaDirection::Schema, path,
                          "enum values must be unique");
      }
    });
  }
  if (type == "object") {
    const auto properties = schema["properties"];
    const auto required = schema["required"];
    if (!properties.isObject() || !required.isArray() ||
        !schema["additionalProperties"].isBool() ||
        schema["additionalProperties"].asBool(true)) {
      validationFailure(AgentSchemaDirection::Schema, path,
                        "objects require bounded properties and additionalProperties:false");
    }
    const auto keys = objectKeys(properties);
    const auto maxProperties =
        schema["maxProperties"].ok()
            ? schema["maxProperties"].asInt64(-1)
            : static_cast<std::int64_t>(keys.size());
    if (keys.size() > 128 || maxProperties < 0 ||
        static_cast<std::size_t>(maxProperties) > keys.size()) {
      validationFailure(AgentSchemaDirection::Schema, path,
                        "object property bounds are invalid");
    }
    std::set<std::string> requiredFields;
    required.forEach([&](const graphql::Json& entry) {
      if (!entry.isString() ||
          std::find(keys.begin(), keys.end(), entry.asString()) == keys.end() ||
          !requiredFields.insert(entry.asString()).second) {
        validationFailure(AgentSchemaDirection::Schema, path,
                          "required fields are invalid");
      }
    });
    properties.forEachMember(
        [&](std::string_view key, const graphql::Json& child) {
          if (rejectAuthorityFields &&
              forbiddenFields().contains(normalizedField(key))) {
            validationFailure(
                AgentSchemaDirection::Schema,
                path + ".properties." + std::string(key),
                "caller authority fields are forbidden");
          }
          inspectSchema(child, path + ".properties." + std::string(key),
                        depth + 1, nodes, rejectAuthorityFields);
        });
    return;
  }
  if (type == "array") {
    const auto maximum = schema["maxItems"].asInt64(-1);
    const auto minimum = schema["minItems"].ok()
                             ? schema["minItems"].asInt64(-1)
                             : 0;
    if (maximum < 0 || maximum > 10'000 || minimum < 0 ||
        minimum > maximum || !schema["items"].isObject()) {
      validationFailure(AgentSchemaDirection::Schema, path,
                        "array item bounds are required and invalid");
    }
    inspectSchema(schema["items"], path + ".items", depth + 1, nodes,
                  rejectAuthorityFields);
    return;
  }
  if (type == "string") {
    const auto maximum = schema["maxLength"].asInt64(-1);
    const auto minimum = schema["minLength"].ok()
                             ? schema["minLength"].asInt64(-1)
                             : 0;
    if (maximum < 0 || maximum > static_cast<std::int64_t>(kDefaultMaxBytes) ||
        minimum < 0 || minimum > maximum) {
      validationFailure(AgentSchemaDirection::Schema, path,
                        "strings require valid min/max length bounds");
    }
    if (const auto pattern = schema["pattern"];
        pattern.ok() && !pattern.isNull() &&
        (!pattern.isString() || !isSupportedPattern(pattern.asStringView()))) {
      validationFailure(AgentSchemaDirection::Schema, path,
                        "string pattern is unsupported");
    }
    if (const auto format = schema["format"];
        format.ok() && !format.isNull() &&
        (!format.isString() || format.asStringView() != "date-time")) {
      validationFailure(AgentSchemaDirection::Schema, path,
                        "string format is unsupported");
    }
    return;
  }
  if (type == "number" || type == "integer") {
    if (!schema["minimum"].isNumber() || !schema["maximum"].isNumber() ||
        !std::isfinite(schema["minimum"].asDouble()) ||
        !std::isfinite(schema["maximum"].asDouble()) ||
        schema["minimum"].asDouble() > schema["maximum"].asDouble()) {
      validationFailure(AgentSchemaDirection::Schema, path,
                        "numbers require finite minimum and maximum");
    }
  }
}

struct ValidationState {
  std::size_t nodes = 0;
  AgentSchemaValidationOptions options;
};

void validateNode(const graphql::Json& schema, const graphql::Json& value,
                  std::string path, std::size_t depth,
                  ValidationState* state) {
  if (++state->nodes > state->options.maxNodes ||
      depth > state->options.maxDepth) {
    validationFailure(state->options.direction, std::move(path),
                      "value is outside depth/node bounds");
  }
  const auto constant = schema["const"];
  if (constant.ok() && !constant.isNull() &&
      canonicalJson(constant) != canonicalJson(value)) {
    validationFailure(state->options.direction, path,
                      "value does not match const");
  }
  const auto enumeration = schema["enum"];
  if (enumeration.ok() && !enumeration.isNull()) {
    const auto encoded = canonicalJson(value);
    bool found = false;
    enumeration.forEach([&](const graphql::Json& entry) {
      found = found || canonicalJson(entry) == encoded;
    });
    if (!found) {
      validationFailure(state->options.direction, path,
                        "contains an unknown enum value");
    }
  }
  const auto type = schema["type"].asString();
  if (type == "boolean") {
    if (!value.isBool()) {
      validationFailure(state->options.direction, std::move(path),
                        "must be a boolean");
    }
    return;
  }
  if (type == "string") {
    if (!value.isString()) {
      validationFailure(state->options.direction, std::move(path),
                        "must be a string");
    }
    const auto text = value.asStringView();
    const auto minimum = schema["minLength"].ok()
                             ? schema["minLength"].asInt64(0)
                             : 0;
    const auto maximum = schema["maxLength"].asInt64(0);
    if (text.size() < static_cast<std::size_t>(minimum) ||
        text.size() > static_cast<std::size_t>(maximum)) {
      validationFailure(state->options.direction, path,
                        "string length is outside bounds");
    }
    if (schema["pattern"].isString() &&
        !matchesPattern(schema["pattern"].asStringView(), text)) {
      validationFailure(state->options.direction, path,
                        "does not match the required pattern");
    }
    if (schema["format"].isString() &&
        schema["format"].asStringView() == "date-time" &&
        !isDateTime(text)) {
      validationFailure(state->options.direction, path,
                        "must be a UTC date-time");
    }
    return;
  }
  if (type == "number" || type == "integer") {
    if (!value.isNumber() || !std::isfinite(value.asDouble())) {
      validationFailure(state->options.direction, path,
                        "must be a finite number");
    }
    const auto number = value.asDouble();
    if ((type == "integer" &&
         (std::floor(number) != number ||
          std::abs(number) > 9'007'199'254'740'991.0)) ||
        number < schema["minimum"].asDouble() ||
        number > schema["maximum"].asDouble()) {
      validationFailure(state->options.direction, path,
                        "number is outside bounds");
    }
    return;
  }
  if (type == "array") {
    if (!value.isArray()) {
      validationFailure(state->options.direction, path,
                        "must be an array");
    }
    const auto minimum = schema["minItems"].ok()
                             ? schema["minItems"].asInt64(0)
                             : 0;
    const auto maximum = schema["maxItems"].asInt64(0);
    if (value.size() < static_cast<std::size_t>(minimum) ||
        value.size() > static_cast<std::size_t>(maximum)) {
      validationFailure(state->options.direction, path,
                        "array length is outside bounds");
    }
    if (schema["uniqueItems"].asBool(false)) {
      std::set<std::string> values;
      value.forEach([&](const graphql::Json& entry) {
        if (!values.insert(canonicalJson(entry)).second) {
          validationFailure(state->options.direction, path,
                            "array items must be unique");
        }
      });
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
      validateNode(schema["items"], value.at(index),
                   path + "[" + std::to_string(index) + "]", depth + 1,
                   state);
    }
    return;
  }
  if (type != "object" || !value.isObject()) {
    validationFailure(state->options.direction, path,
                      "must be a plain object");
  }
  const auto properties = schema["properties"];
  const auto keys = objectKeys(value);
  const auto allowed = objectKeys(properties);
  for (const auto& key : keys) {
    if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
      validationFailure(state->options.direction, path + "." + key,
                        "unknown fields are forbidden");
    }
  }
  const auto minimum = schema["minProperties"].ok()
                           ? schema["minProperties"].asInt64(0)
                           : 0;
  const auto maximum =
      schema["maxProperties"].ok()
          ? schema["maxProperties"].asInt64(0)
          : static_cast<std::int64_t>(allowed.size());
  if (value.size() < static_cast<std::size_t>(minimum) ||
      value.size() > static_cast<std::size_t>(maximum)) {
    validationFailure(state->options.direction, path,
                      "object property count is outside bounds");
  }
  schema["required"].forEach([&](const graphql::Json& entry) {
    const auto key = entry.asString();
    if (!value[key].ok()) {
      validationFailure(state->options.direction, path + "." + key,
                        "required field is missing");
    }
  });
  for (const auto& key : keys) {
    validateNode(properties[key], value[key], path + "." + key, depth + 1,
                 state);
  }
}

}  // namespace

void assertBoundedJsonSchema(const graphql::Json& schema,
                             bool rejectAuthorityFields) {
  std::size_t nodes = 0;
  inspectSchema(schema, "$", 0, &nodes, rejectAuthorityFields);
}

void validateJsonSchemaValue(const graphql::Json& schema,
                             const graphql::Json& value,
                             AgentSchemaValidationOptions options) {
  if (!value.ok()) {
    validationFailure(options.direction, "$", "value is not JSON");
  }
  const auto encoded = canonicalJson(value);
  if (encoded.size() > options.maxBytes) {
    validationFailure(options.direction, "$",
                      "encoded value exceeds the byte bound");
  }
  ValidationState state{0, options};
  validateNode(schema, value, "$", 0, &state);
}

}  // namespace crowdy::agent
