#pragma once

#include <string>
#include <utility>
#include <variant>

/// Exception-free result type used on hot paths (the replication layer never
/// throws). The GraphQL layer uses structured exceptions instead — see
/// crowdy/graphql/errors.hpp.
namespace crowdy {

enum class Errc {
  Ok = 0,
  InvalidArgument,
  BufferTooSmall,
  Malformed,
  HmacMismatch,
  NotConnected,
  SocketError,
  Timeout,
  TokenExpired,
  Rejected,
  Closed,
  CryptoUnavailable,
  /// Transient: the operation could not proceed right now (a full kernel send
  /// buffer is the usual cause) and nothing was transferred. The socket is
  /// healthy — retry shortly. Distinct from SocketError, which is a fault.
  WouldBlock,
};

inline const char* errcName(Errc e) {
  switch (e) {
    case Errc::Ok: return "Ok";
    case Errc::InvalidArgument: return "InvalidArgument";
    case Errc::BufferTooSmall: return "BufferTooSmall";
    case Errc::Malformed: return "Malformed";
    case Errc::HmacMismatch: return "HmacMismatch";
    case Errc::NotConnected: return "NotConnected";
    case Errc::SocketError: return "SocketError";
    case Errc::Timeout: return "Timeout";
    case Errc::TokenExpired: return "TokenExpired";
    case Errc::Rejected: return "Rejected";
    case Errc::Closed: return "Closed";
    case Errc::CryptoUnavailable: return "CryptoUnavailable";
    case Errc::WouldBlock: return "WouldBlock";
  }
  return "Unknown";
}

/// Result<void> analog: just an error code, Errc::Ok means success.
struct Status {
  Errc code = Errc::Ok;
  constexpr Status() = default;
  constexpr Status(Errc c) : code(c) {}
  constexpr bool ok() const { return code == Errc::Ok; }
  constexpr explicit operator bool() const { return ok(); }
};

/// Minimal expected-like carrier for hot paths.
template <typename T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}
  Result(Errc code) : value_(code) {}

  bool ok() const { return std::holds_alternative<T>(value_); }
  explicit operator bool() const { return ok(); }

  T& value() { return std::get<T>(value_); }
  const T& value() const { return std::get<T>(value_); }
  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }

  Errc error() const { return ok() ? Errc::Ok : std::get<Errc>(value_); }

 private:
  std::variant<T, Errc> value_;
};

}  // namespace crowdy
