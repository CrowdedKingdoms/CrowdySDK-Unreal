#pragma once

#include <cstdint>
#include <string>

#include "crowdy/core/bytes.hpp"
#include "crowdy/core/result.hpp"

/// Minimal portable UDP socket (POSIX + Winsock). The socket is connect()ed
/// to the replication server, so recv only returns that server's datagrams
/// and send needs no per-call address.
namespace crowdy::replication {

class UdpSocket {
 public:
  UdpSocket() = default;
  ~UdpSocket() { close(); }
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  /// Open and connect to host:port. `host` is a literal IPv4 or IPv6 address.
  /// Buffer sizes are hints passed to SO_RCVBUF / SO_SNDBUF; a value <= 0
  /// leaves the OS default in place. Kernels may round up (Linux doubles),
  /// and a request the kernel declines is not an error.
  Status open(const std::string& host, int port, int recvBufferBytes, int sendBufferBytes);
  void close();
  bool isOpen() const { return fd_ >= 0; }

  /// Send one datagram. Never blocks. Returns Errc::Ok when the datagram was
  /// handed to the kernel, Errc::WouldBlock when the send buffer is full — a
  /// transient condition, nothing was sent, retry shortly — and
  /// Errc::SocketError only for a genuine fault.
  Status send(Bytes datagram);

  /// Receive one datagram into `buffer`, waiting up to timeoutMs (0 = no
  /// wait). Returns the datagram length, 0 on timeout, or an error.
  Result<std::size_t> recv(MutableBytes buffer, int timeoutMs);

  /// Batched receive: fill up to `count` datagrams into equal-sized slots of
  /// `slab` (slotSize bytes each), writing each datagram's length into
  /// `lengths`. Waits up to timeoutMs for the first datagram, then drains
  /// without waiting. Uses recvmmsg(2) on Linux (one syscall per batch);
  /// falls back to a recv loop elsewhere. Returns the number received.
  Result<std::size_t> recvBatch(std::uint8_t* slab, std::size_t slotSize, std::size_t count,
                                std::size_t* lengths, int timeoutMs);

  /// Underlying descriptor (a SOCKET on Windows), or -1 when closed. For
  /// diagnostics and tests — reading socket options back, for instance. The
  /// socket remains owned by this object; do not close or reconfigure it.
  long long nativeHandle() const { return fd_; }

 private:
  long long fd_ = -1;
};

}  // namespace crowdy::replication
