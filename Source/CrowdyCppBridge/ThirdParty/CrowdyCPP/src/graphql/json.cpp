#include "crowdy/graphql/json.hpp"

#include <yyjson.h>

#include <cmath>
#include <cstdio>
#include <limits>

namespace crowdy::graphql {

namespace {

bool isDecimal(std::string_view value) {
  if (value.empty()) return false;
  std::size_t index = value.front() == '-' ? 1 : 0;
  if (index == value.size()) return false;
  for (; index < value.size(); ++index) {
    if (value[index] < '0' || value[index] > '9') return false;
  }
  return true;
}

}  // namespace

std::optional<std::int64_t> parseBigInt(std::string_view decimal) {
  if (!isDecimal(decimal)) return std::nullopt;

  const bool negative = decimal.front() == '-';
  std::size_t index = negative ? 1 : 0;
  constexpr std::uint64_t positiveLimit =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  constexpr std::uint64_t negativeLimit = positiveLimit + 1;
  const std::uint64_t limit = negative ? negativeLimit : positiveLimit;
  std::uint64_t magnitude = 0;
  for (; index < decimal.size(); ++index) {
    const std::uint64_t digit =
        static_cast<std::uint64_t>(decimal[index] - '0');
    if (magnitude > (limit - digit) / 10) return std::nullopt;
    magnitude = magnitude * 10 + digit;
  }

  if (!negative) return static_cast<std::int64_t>(magnitude);
  if (magnitude == negativeLimit)
    return std::numeric_limits<std::int64_t>::min();
  return -static_cast<std::int64_t>(magnitude);
}

// ---- JVal -------------------------------------------------------------------

JVal& JVal::operator[](std::string_view key) {
  if (isNull()) v_ = JObject{};
  auto& o = std::get<JObject>(v_);
  auto it = o.find(key);
  if (it == o.end()) it = o.emplace(std::string(key), JVal()).first;
  return it->second;
}

namespace {

void appendEscaped(std::string& out, const std::string& s) {
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
}

}  // namespace

void JVal::dumpTo(std::string& out) const {
  struct Visitor {
    std::string& out;
    void operator()(std::nullptr_t) { out += "null"; }
    void operator()(bool b) { out += b ? "true" : "false"; }
    void operator()(std::int64_t i) { out += std::to_string(i); }
    void operator()(double d) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.17g", d);
      out += buf;
    }
    void operator()(const std::string& s) { appendEscaped(out, s); }
    void operator()(const JArray& a) {
      out.push_back('[');
      bool first = true;
      for (const auto& v : a) {
        if (!first) out.push_back(',');
        first = false;
        v.dumpTo(out);
      }
      out.push_back(']');
    }
    void operator()(const JObject& o) {
      out.push_back('{');
      bool first = true;
      for (const auto& [k, v] : o) {
        if (!first) out.push_back(',');
        first = false;
        appendEscaped(out, k);
        out.push_back(':');
        v.dumpTo(out);
      }
      out.push_back('}');
    }
  };
  std::visit(Visitor{out}, v_);
}

std::string JVal::dump() const {
  std::string out;
  dumpTo(out);
  return out;
}

// ---- Json -------------------------------------------------------------------

namespace {
inline yyjson_val* val(void* p) { return static_cast<yyjson_val*>(p); }
}  // namespace

Json Json::parse(std::string_view text) {
  yyjson_doc* doc = yyjson_read(text.data(), text.size(), 0);
  if (!doc) return Json();
  auto holder = std::shared_ptr<void>(doc, [](void* d) { yyjson_doc_free(static_cast<yyjson_doc*>(d)); });
  return Json(holder, yyjson_doc_get_root(doc));
}

bool Json::isNull() const { return !val_ || yyjson_is_null(val(val_)); }
bool Json::isObject() const { return val_ && yyjson_is_obj(val(val_)); }
bool Json::isArray() const { return val_ && yyjson_is_arr(val(val_)); }
bool Json::isString() const { return val_ && yyjson_is_str(val(val_)); }
bool Json::isNumber() const { return val_ && yyjson_is_num(val(val_)); }
bool Json::isBool() const { return val_ && yyjson_is_bool(val(val_)); }

Json Json::operator[](std::string_view key) const {
  if (!isObject()) return Json();
  yyjson_val* v = yyjson_obj_getn(val(val_), key.data(), key.size());
  if (!v) return Json();
  return Json(doc_, v);
}

Json Json::at(std::size_t index) const {
  if (!isArray()) return Json();
  yyjson_val* v = yyjson_arr_get(val(val_), index);
  if (!v) return Json();
  return Json(doc_, v);
}

std::size_t Json::size() const {
  if (isArray()) return yyjson_arr_size(val(val_));
  if (isObject()) return yyjson_obj_size(val(val_));
  return 0;
}

std::string_view Json::asStringView(std::string_view fallback) const {
  if (!isString()) return fallback;
  return std::string_view(yyjson_get_str(val(val_)), yyjson_get_len(val(val_)));
}

std::int64_t Json::asInt64(std::int64_t fallback) const {
  if (!val_) return fallback;
  yyjson_val* v = val(val_);
  if (yyjson_is_int(v)) return yyjson_get_sint(v);
  if (yyjson_is_real(v)) return static_cast<std::int64_t>(yyjson_get_real(v));
  return fallback;
}

double Json::asDouble(double fallback) const {
  if (!val_ || !yyjson_is_num(val(val_))) return fallback;
  return yyjson_get_num(val(val_));
}

bool Json::asBool(bool fallback) const {
  if (!isBool()) return fallback;
  return yyjson_get_bool(val(val_));
}

std::optional<std::int64_t> Json::tryAsBigInt() const {
  if (isString()) {
    return parseBigInt(asStringView());
  }
  if (!val_) return std::nullopt;
  yyjson_val* value = val(val_);
  if (yyjson_is_sint(value)) return yyjson_get_sint(value);
  if (yyjson_is_uint(value)) {
    const std::uint64_t unsignedValue = yyjson_get_uint(value);
    if (unsignedValue >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
      return std::nullopt;
    }
    return static_cast<std::int64_t>(unsignedValue);
  }
  if (yyjson_is_real(value)) {
    const double realValue = yyjson_get_real(value);
    constexpr double int64UpperExclusive = 9223372036854775808.0;
    constexpr double int64LowerInclusive = -9223372036854775808.0;
    if (!std::isfinite(realValue) || std::trunc(realValue) != realValue ||
        realValue >= int64UpperExclusive ||
        realValue < int64LowerInclusive) {
      return std::nullopt;
    }
    return static_cast<std::int64_t>(realValue);
  }
  return std::nullopt;
}

std::int64_t Json::asBigInt(std::int64_t fallback) const {
  const auto parsed = tryAsBigInt();
  return parsed ? *parsed : fallback;
}

std::string Json::asBigIntString(std::string_view fallback) const {
  if (isString()) {
    const std::string_view decimal = asStringView();
    return isDecimal(decimal) ? std::string(decimal) : std::string(fallback);
  }
  if (!val_) return std::string(fallback);
  yyjson_val* value = val(val_);
  if (yyjson_is_sint(value)) return std::to_string(yyjson_get_sint(value));
  if (yyjson_is_uint(value)) return std::to_string(yyjson_get_uint(value));
  const auto parsed = tryAsBigInt();
  return parsed ? std::to_string(*parsed) : std::string(fallback);
}

std::string Json::dump() const {
  if (!val_) return "null";
  char* s = yyjson_val_write(val(val_), 0, nullptr);
  if (!s) return "null";
  std::string out(s);
  free(s);
  return out;
}

std::size_t Json::memberCount() const { return isObject() ? yyjson_obj_size(val(val_)) : 0; }

std::pair<std::string_view, Json> Json::memberAt(std::size_t i) const {
  yyjson_val* key = nullptr;
  yyjson_val* v = nullptr;
  std::size_t idx = 0;
  yyjson_obj_iter iter;
  yyjson_obj_iter_init(val(val_), &iter);
  while ((key = yyjson_obj_iter_next(&iter)) != nullptr) {
    v = yyjson_obj_iter_get_val(key);
    if (idx == i) {
      return {std::string_view(yyjson_get_str(key), yyjson_get_len(key)), Json(doc_, v)};
    }
    ++idx;
  }
  return {std::string_view(), Json()};
}

}  // namespace crowdy::graphql
