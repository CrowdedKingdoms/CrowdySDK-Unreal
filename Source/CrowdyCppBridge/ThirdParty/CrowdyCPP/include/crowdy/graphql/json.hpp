#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/// JSON support for the GraphQL layer (never used on the UDP path).
///
/// Two shapes:
///  - JVal: a small mutable value tree for building GraphQL variables.
///  - Json: an immutable parsed document (yyjson-backed) for responses.
namespace crowdy::graphql {

/// Parse a signed decimal string without narrowing or errno-dependent
/// saturation. Returns nullopt for malformed input and int64 overflow.
std::optional<std::int64_t> parseBigInt(std::string_view decimal);

// ---------------------------------------------------------------------------
// JVal — build JSON values (GraphQL variables)
// ---------------------------------------------------------------------------

class JVal;
using JArray = std::vector<JVal>;
using JObject = std::map<std::string, JVal, std::less<>>;

class JVal {
 public:
  JVal() : v_(nullptr) {}
  JVal(std::nullptr_t) : v_(nullptr) {}
  JVal(bool b) : v_(b) {}
  JVal(int i) : v_(static_cast<std::int64_t>(i)) {}
  JVal(std::int64_t i) : v_(i) {}
  JVal(double d) : v_(d) {}
  JVal(const char* s) : v_(std::string(s)) {}
  JVal(std::string s) : v_(std::move(s)) {}
  JVal(std::string_view s) : v_(std::string(s)) {}
  JVal(JArray a) : v_(std::move(a)) {}
  JVal(JObject o) : v_(std::move(o)) {}

  static JVal object(std::initializer_list<std::pair<const std::string, JVal>> init) {
    return JVal(JObject(init));
  }
  static JVal array(std::initializer_list<JVal> init) { return JVal(JArray(init)); }

  bool isNull() const { return std::holds_alternative<std::nullptr_t>(v_); }
  bool isObject() const { return std::holds_alternative<JObject>(v_); }
  bool isArray() const { return std::holds_alternative<JArray>(v_); }
  /// Pointer to the held string, or nullptr when this is not a string.
  const std::string* asStringPtr() const { return std::get_if<std::string>(&v_); }

  JObject& obj() { return std::get<JObject>(v_); }
  const JObject& obj() const { return std::get<JObject>(v_); }
  JArray& arr() { return std::get<JArray>(v_); }
  const JArray& arr() const { return std::get<JArray>(v_); }

  /// Object field access; creates the object variant if currently null.
  JVal& operator[](std::string_view key);
  /// Rejects `value[0]`, which would otherwise bind the literal to
  /// string_view as a null pointer and read through it. Build arrays with
  /// JVal::array(...).
  JVal& operator[](std::nullptr_t) = delete;

  /// Serialize to a compact JSON string (RFC 8259 escaping).
  std::string dump() const;

 private:
  std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, JArray, JObject> v_;
  void dumpTo(std::string& out) const;
};

// ---------------------------------------------------------------------------
// Json — parsed immutable document view
// ---------------------------------------------------------------------------

/// A parsed JSON value. Instances share ownership of the underlying document,
/// so subvalues stay valid as long as any Json referencing the doc lives.
class Json {
 public:
  Json() = default;

  /// Parse. Returns a null Json (ok() == false) on invalid input.
  static Json parse(std::string_view text);

  bool ok() const { return val_ != nullptr; }
  bool isNull() const;
  bool isObject() const;
  bool isArray() const;
  bool isString() const;
  bool isNumber() const;
  bool isBool() const;

  /// Object member (null Json when absent or not an object).
  Json operator[](std::string_view key) const;
  /// Rejects `json[0]`, which would otherwise bind the literal to string_view
  /// as a null pointer and segfault measuring its length. Index with at().
  Json operator[](std::nullptr_t) const = delete;
  /// Array element (null Json when out of range).
  Json at(std::size_t index) const;
  std::size_t size() const;  // array length or object member count

  std::string_view asStringView(std::string_view fallback = {}) const;
  std::string asString(std::string_view fallback = {}) const {
    return std::string(asStringView(fallback));
  }
  std::int64_t asInt64(std::int64_t fallback = 0) const;
  double asDouble(double fallback = 0) const;
  bool asBool(bool fallback = false) const;

  /// GraphQL BigInt scalars cross the wire as decimal strings (and IDs may be
  /// numbers); this accepts either representation. Malformed and overflowing
  /// values return `fallback`.
  std::int64_t asBigInt(std::int64_t fallback = 0) const;
  /// Checked twin of asBigInt().
  std::optional<std::int64_t> tryAsBigInt() const;
  /// Preserve a GraphQL BigInt as decimal text, including values outside
  /// int64. Integer JSON numbers are converted losslessly.
  std::string asBigIntString(std::string_view fallback = {}) const;

  /// Serialize this value back to a JSON string.
  std::string dump() const;

  /// Iterate object members / array elements.
  template <typename Fn>
  void forEachMember(Fn&& fn) const {
    for (std::size_t i = 0, n = memberCount(); i < n; ++i) {
      auto [k, v] = memberAt(i);
      fn(k, v);
    }
  }
  template <typename Fn>
  void forEach(Fn&& fn) const {
    for (std::size_t i = 0, n = size(); i < n; ++i) fn(at(i));
  }

 private:
  Json(std::shared_ptr<void> doc, void* val) : doc_(std::move(doc)), val_(val) {}
  std::size_t memberCount() const;
  std::pair<std::string_view, Json> memberAt(std::size_t i) const;

  std::shared_ptr<void> doc_;  // yyjson_doc, type-erased to keep yyjson private
  void* val_ = nullptr;        // yyjson_val
};

}  // namespace crowdy::graphql
