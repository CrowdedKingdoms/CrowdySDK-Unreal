#include "crowdy/graphql/json.hpp"

#include <yyjson.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "crowdy/core/base64.hpp"

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

// ---- MessagePack --------------------------------------------------------------

namespace {

void packBe(std::string& out, std::uint64_t v, int bytes) {
  for (int i = bytes - 1; i >= 0; --i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

void packUint(std::string& out, std::uint64_t u) {
  if (u <= 0x7f) {
    out.push_back(static_cast<char>(u));
  } else if (u <= 0xff) {
    out.push_back(static_cast<char>(0xcc));
    packBe(out, u, 1);
  } else if (u <= 0xffff) {
    out.push_back(static_cast<char>(0xcd));
    packBe(out, u, 2);
  } else if (u <= 0xffffffffULL) {
    out.push_back(static_cast<char>(0xce));
    packBe(out, u, 4);
  } else {
    out.push_back(static_cast<char>(0xcf));
    packBe(out, u, 8);
  }
}

void packSint(std::string& out, std::int64_t i) {
  if (i >= 0) return packUint(out, static_cast<std::uint64_t>(i));
  const auto bits = static_cast<std::uint64_t>(i);
  if (i >= -32) {
    out.push_back(static_cast<char>(i));
  } else if (i >= -128) {
    out.push_back(static_cast<char>(0xd0));
    packBe(out, bits, 1);
  } else if (i >= -32768) {
    out.push_back(static_cast<char>(0xd1));
    packBe(out, bits, 2);
  } else if (i >= -2147483648LL) {
    out.push_back(static_cast<char>(0xd2));
    packBe(out, bits, 4);
  } else {
    out.push_back(static_cast<char>(0xd3));
    packBe(out, bits, 8);
  }
}

void packHeader(std::string& out, std::size_t n, std::uint8_t fix, std::size_t fixMax,
                std::uint8_t tag16, std::uint8_t tag32) {
  if (n <= fixMax) {
    out.push_back(static_cast<char>(fix | n));
  } else if (n <= 0xffff) {
    out.push_back(static_cast<char>(tag16));
    packBe(out, n, 2);
  } else {
    out.push_back(static_cast<char>(tag32));
    packBe(out, n, 4);
  }
}

void packStr(std::string& out, const char* s, std::size_t len) {
  if (len <= 31) {
    out.push_back(static_cast<char>(0xa0 | len));
  } else if (len <= 0xff) {
    out.push_back(static_cast<char>(0xd9));
    packBe(out, len, 1);
  } else if (len <= 0xffff) {
    out.push_back(static_cast<char>(0xda));
    packBe(out, len, 2);
  } else {
    out.push_back(static_cast<char>(0xdb));
    packBe(out, len, 4);
  }
  out.append(s, len);
}

void packVal(std::string& out, yyjson_val* v) {
  switch (yyjson_get_type(v)) {
    case YYJSON_TYPE_BOOL:
      out.push_back(static_cast<char>(yyjson_get_bool(v) ? 0xc3 : 0xc2));
      return;
    case YYJSON_TYPE_NUM:
      if (yyjson_is_uint(v)) return packUint(out, yyjson_get_uint(v));
      if (yyjson_is_sint(v)) return packSint(out, yyjson_get_sint(v));
      {
        const double d = yyjson_get_real(v);
        std::uint64_t bits = 0;
        std::memcpy(&bits, &d, sizeof bits);
        out.push_back(static_cast<char>(0xcb));
        packBe(out, bits, 8);
      }
      return;
    case YYJSON_TYPE_STR:
      return packStr(out, yyjson_get_str(v), yyjson_get_len(v));
    case YYJSON_TYPE_ARR: {
      packHeader(out, yyjson_arr_size(v), 0x90, 15, 0xdc, 0xdd);
      yyjson_arr_iter it;
      yyjson_arr_iter_init(v, &it);
      while (yyjson_val* e = yyjson_arr_iter_next(&it)) packVal(out, e);
      return;
    }
    case YYJSON_TYPE_OBJ: {
      packHeader(out, yyjson_obj_size(v), 0x80, 15, 0xde, 0xdf);
      yyjson_obj_iter it;
      yyjson_obj_iter_init(v, &it);
      while (yyjson_val* k = yyjson_obj_iter_next(&it)) {
        packStr(out, yyjson_get_str(k), yyjson_get_len(k));
        packVal(out, yyjson_obj_iter_get_val(k));
      }
      return;
    }
    default:
      out.push_back(static_cast<char>(0xc0));
  }
}

constexpr int kMaxMsgpackDepth = 64;

struct Unpacker {
  const std::uint8_t* p;
  const std::uint8_t* end;
  yyjson_mut_doc* doc;
  bool ok = true;

  bool need(std::size_t n) {
    if (static_cast<std::size_t>(end - p) < n) ok = false;
    return ok;
  }
  std::uint64_t be(int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = (v << 8) | *p++;
    return v;
  }
  yyjson_mut_val* str(std::size_t len) {
    if (!need(len)) return nullptr;
    yyjson_mut_val* s = yyjson_mut_strncpy(doc, reinterpret_cast<const char*>(p), len);
    p += len;
    return s;
  }
  yyjson_mut_val* bin(std::size_t len) {
    if (!need(len)) return nullptr;
    const std::string b64 = crowdy::core::base64Encode(crowdy::Bytes(p, len));
    p += len;
    return yyjson_mut_strncpy(doc, b64.data(), b64.size());
  }
  yyjson_mut_val* skipExt(std::size_t len) {
    if (!need(len + 1)) return nullptr;
    p += len + 1;
    return yyjson_mut_null(doc);
  }
  yyjson_mut_val* arr(std::size_t n, int depth) {
    yyjson_mut_val* a = yyjson_mut_arr(doc);
    for (std::size_t i = 0; i < n && ok; ++i) {
      yyjson_mut_val* e = value(depth + 1);
      if (e) yyjson_mut_arr_append(a, e);
    }
    return a;
  }
  yyjson_mut_val* map(std::size_t n, int depth) {
    yyjson_mut_val* o = yyjson_mut_obj(doc);
    for (std::size_t i = 0; i < n && ok; ++i) {
      yyjson_mut_val* k = value(depth + 1);
      yyjson_mut_val* v = value(depth + 1);
      if (!ok || !k || !v) break;
      if (!yyjson_mut_is_str(k)) {
        // A non-string key keeps its value as text, as JSON has only string keys.
        char* text = yyjson_mut_val_write(k, 0, nullptr);
        k = yyjson_mut_strcpy(doc, text ? text : "null");
        free(text);
      }
      yyjson_mut_obj_add(o, k, v);
    }
    return o;
  }
  yyjson_mut_val* value(int depth) {
    if (depth > kMaxMsgpackDepth || !need(1)) {
      ok = false;
      return nullptr;
    }
    const std::uint8_t t = *p++;
    if (t <= 0x7f) return yyjson_mut_uint(doc, t);
    if (t >= 0xe0) return yyjson_mut_sint(doc, static_cast<std::int8_t>(t));
    if ((t & 0xe0) == 0xa0) return str(t & 0x1f);
    if ((t & 0xf0) == 0x90) return arr(t & 0x0f, depth);
    if ((t & 0xf0) == 0x80) return map(t & 0x0f, depth);
    auto sized = [&](int bytes) -> std::uint64_t { return need(bytes) ? be(bytes) : 0; };
    switch (t) {
      case 0xc0: return yyjson_mut_null(doc);
      case 0xc2: return yyjson_mut_bool(doc, false);
      case 0xc3: return yyjson_mut_bool(doc, true);
      case 0xc4: { auto n = sized(1); return ok ? bin(n) : nullptr; }
      case 0xc5: { auto n = sized(2); return ok ? bin(n) : nullptr; }
      case 0xc6: { auto n = sized(4); return ok ? bin(n) : nullptr; }
      case 0xc7: { auto n = sized(1); return ok ? skipExt(n) : nullptr; }
      case 0xc8: { auto n = sized(2); return ok ? skipExt(n) : nullptr; }
      case 0xc9: { auto n = sized(4); return ok ? skipExt(n) : nullptr; }
      case 0xca: {
        const auto bits = static_cast<std::uint32_t>(sized(4));
        float f = 0;
        std::memcpy(&f, &bits, sizeof f);
        return ok ? yyjson_mut_real(doc, f) : nullptr;
      }
      case 0xcb: {
        const std::uint64_t bits = sized(8);
        double d = 0;
        std::memcpy(&d, &bits, sizeof d);
        return ok ? yyjson_mut_real(doc, d) : nullptr;
      }
      case 0xcc: { auto v = sized(1); return ok ? yyjson_mut_uint(doc, v) : nullptr; }
      case 0xcd: { auto v = sized(2); return ok ? yyjson_mut_uint(doc, v) : nullptr; }
      case 0xce: { auto v = sized(4); return ok ? yyjson_mut_uint(doc, v) : nullptr; }
      case 0xcf: { auto v = sized(8); return ok ? yyjson_mut_uint(doc, v) : nullptr; }
      case 0xd0: { auto v = sized(1); return ok ? yyjson_mut_sint(doc, static_cast<std::int8_t>(v)) : nullptr; }
      case 0xd1: { auto v = sized(2); return ok ? yyjson_mut_sint(doc, static_cast<std::int16_t>(v)) : nullptr; }
      case 0xd2: { auto v = sized(4); return ok ? yyjson_mut_sint(doc, static_cast<std::int32_t>(v)) : nullptr; }
      case 0xd3: { auto v = sized(8); return ok ? yyjson_mut_sint(doc, static_cast<std::int64_t>(v)) : nullptr; }
      case 0xd4: return skipExt(1);
      case 0xd5: return skipExt(2);
      case 0xd6: return skipExt(4);
      case 0xd7: return skipExt(8);
      case 0xd8: return skipExt(16);
      case 0xd9: { auto n = sized(1); return ok ? str(n) : nullptr; }
      case 0xda: { auto n = sized(2); return ok ? str(n) : nullptr; }
      case 0xdb: { auto n = sized(4); return ok ? str(n) : nullptr; }
      case 0xdc: { auto n = sized(2); return ok ? arr(n, depth) : nullptr; }
      case 0xdd: { auto n = sized(4); return ok ? arr(n, depth) : nullptr; }
      case 0xde: { auto n = sized(2); return ok ? map(n, depth) : nullptr; }
      case 0xdf: { auto n = sized(4); return ok ? map(n, depth) : nullptr; }
      default:
        ok = false;  // 0xc1 is never used
        return nullptr;
    }
  }
};

}  // namespace

std::string Json::toMsgpack() const {
  std::string out;
  if (!val_) {
    out.push_back(static_cast<char>(0xc0));
    return out;
  }
  packVal(out, val(val_));
  return out;
}

Json Json::fromMsgpack(std::string_view bytes) {
  yyjson_mut_doc* md = yyjson_mut_doc_new(nullptr);
  if (!md) return Json();
  const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes.data());
  Unpacker u{begin, begin + bytes.size(), md};
  yyjson_mut_val* root = u.value(0);
  if (!u.ok || !root || u.p != u.end) {
    yyjson_mut_doc_free(md);
    return Json();
  }
  yyjson_mut_doc_set_root(md, root);
  yyjson_doc* doc = yyjson_mut_doc_imut_copy(md, nullptr);
  yyjson_mut_doc_free(md);
  if (!doc) return Json();
  auto holder = std::shared_ptr<void>(doc, [](void* d) { yyjson_doc_free(static_cast<yyjson_doc*>(d)); });
  return Json(holder, yyjson_doc_get_root(doc));
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
