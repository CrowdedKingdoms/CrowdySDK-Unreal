#include "crowdy/replication/udp_socket.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SockLen = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using SockLen = socklen_t;
#endif

#include <cstring>

#include "socket_errors.hpp"

namespace crowdy::replication {

namespace {

#ifdef _WIN32
void ensureWinsock() {
  static bool initialized = [] {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  (void)initialized;
}
#endif

}  // namespace

Status UdpSocket::open(const std::string& host, int port, int recvBufferBytes,
                       int sendBufferBytes) {
  close();
#ifdef _WIN32
  ensureWinsock();
#endif

  sockaddr_storage addr{};
  SockLen addrLen = 0;
  in6_addr v6{};
  in_addr v4{};
  if (inet_pton(AF_INET, host.c_str(), &v4) == 1) {
    auto* a = reinterpret_cast<sockaddr_in*>(&addr);
    a->sin_family = AF_INET;
    a->sin_port = htons(static_cast<std::uint16_t>(port));
    a->sin_addr = v4;
    addrLen = sizeof(sockaddr_in);
  } else if (inet_pton(AF_INET6, host.c_str(), &v6) == 1) {
    auto* a = reinterpret_cast<sockaddr_in6*>(&addr);
    a->sin6_family = AF_INET6;
    a->sin6_port = htons(static_cast<std::uint16_t>(port));
    a->sin6_addr = v6;
    addrLen = sizeof(sockaddr_in6);
  } else {
    return Errc::InvalidArgument;
  }

  const int family = reinterpret_cast<sockaddr*>(&addr)->sa_family;
#ifdef _WIN32
  const auto fd = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
  if (fd == INVALID_SOCKET) return Errc::SocketError;
#else
  const int fd = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) return Errc::SocketError;
#endif

  if (recvBufferBytes > 0) {
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&recvBufferBytes),
                 sizeof(recvBufferBytes));
  }
  if (sendBufferBytes > 0) {
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBufferBytes),
                 sizeof(sendBufferBytes));
  }

  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), addrLen) != 0) {
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd);
#endif
    return Errc::SocketError;
  }

#ifdef _WIN32
  // recv(..., timeoutMs=0) is the manual-pump fast path and must never block.
  // POSIX uses MSG_DONTWAIT below; Winsock requires the socket itself to be
  // non-blocking so a drained socket returns WSAEWOULDBLOCK.
  u_long nonBlocking = 1;
  if (::ioctlsocket(fd, FIONBIO, &nonBlocking) != 0) {
    ::closesocket(fd);
    return Errc::SocketError;
  }
#endif

  fd_ = static_cast<long long>(fd);
  return Errc::Ok;
}

void UdpSocket::close() {
  if (fd_ < 0) return;
#ifdef _WIN32
  ::closesocket(static_cast<SOCKET>(fd_));
#else
  ::close(static_cast<int>(fd_));
#endif
  fd_ = -1;
}

Status UdpSocket::send(Bytes datagram) {
  if (fd_ < 0) return Errc::NotConnected;
  // A datagram socket takes the whole datagram or none of it, so a short write
  // that is not an error cannot happen; only n < 0 carries a meaningful error
  // code, and consulting one after a non-negative return would read a stale
  // value from some earlier call.
#ifdef _WIN32
  // The socket is non-blocking (see open), so a full send buffer reports
  // WSAEWOULDBLOCK here rather than waiting for room.
  const int n = ::send(static_cast<SOCKET>(fd_), reinterpret_cast<const char*>(datagram.data()),
                       static_cast<int>(datagram.size()), 0);
  if (n < 0) return detail::sendErrcFromNativeError(WSAGetLastError());
  if (n != static_cast<int>(datagram.size())) return Errc::SocketError;
#else
  // MSG_DONTWAIT for the same reason recv uses it: the caller may be a game
  // loop, which must not stall behind a full buffer. Without it a saturated
  // socket blocks here instead of reporting WouldBlock.
  const ssize_t n =
      ::send(static_cast<int>(fd_), datagram.data(), datagram.size(), MSG_DONTWAIT);
  if (n < 0) return detail::sendErrcFromNativeError(errno);
  if (n != static_cast<ssize_t>(datagram.size())) return Errc::SocketError;
#endif
  return Errc::Ok;
}

Result<std::size_t> UdpSocket::recvBatch(std::uint8_t* slab, std::size_t slotSize,
                                         std::size_t count, std::size_t* lengths,
                                         int timeoutMs) {
  if (fd_ < 0) return Errc::NotConnected;
#ifdef __linux__
  constexpr std::size_t kMaxBatch = 32;
  if (count > kMaxBatch) count = kMaxBatch;

  pollfd pfd{static_cast<int>(fd_), POLLIN, 0};
  const int r = ::poll(&pfd, 1, timeoutMs);
  if (r == 0) return std::size_t{0};
  if (r < 0) return Errc::SocketError;

  mmsghdr messages[kMaxBatch]{};
  iovec vectors[kMaxBatch];
  for (std::size_t i = 0; i < count; ++i) {
    vectors[i].iov_base = slab + i * slotSize;
    vectors[i].iov_len = slotSize;
    messages[i].msg_hdr.msg_iov = &vectors[i];
    messages[i].msg_hdr.msg_iovlen = 1;
  }
  const int n = ::recvmmsg(static_cast<int>(fd_), messages, static_cast<unsigned>(count),
                           MSG_DONTWAIT, nullptr);
  if (n < 0) {
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? Result<std::size_t>(std::size_t{0})
                                                     : Result<std::size_t>(Errc::SocketError);
  }
  for (int i = 0; i < n; ++i) lengths[i] = messages[i].msg_len;
  return static_cast<std::size_t>(n);
#else
  // Portable fallback: one recv per datagram, draining after the first.
  std::size_t received = 0;
  int wait = timeoutMs;
  while (received < count) {
    auto n = recv(MutableBytes(slab + received * slotSize, slotSize), wait);
    if (!n.ok()) return received > 0 ? Result<std::size_t>(received) : n;
    if (n.value() == 0) break;
    lengths[received++] = n.value();
    wait = 0;
  }
  return received;
#endif
}

Result<std::size_t> UdpSocket::recv(MutableBytes buffer, int timeoutMs) {
  if (fd_ < 0) return Errc::NotConnected;

#ifdef _WIN32
  if (timeoutMs > 0) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(static_cast<SOCKET>(fd_), &readSet);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    const int r = ::select(0, &readSet, nullptr, nullptr, &tv);
    if (r == 0) return std::size_t{0};
    if (r < 0) return Errc::SocketError;
  }
  const int n = ::recv(static_cast<SOCKET>(fd_), reinterpret_cast<char*>(buffer.data()),
                       static_cast<int>(buffer.size()), 0);
  if (n < 0) {
    return WSAGetLastError() == WSAEWOULDBLOCK ? Result<std::size_t>(std::size_t{0})
                                               : Result<std::size_t>(Errc::SocketError);
  }
  return static_cast<std::size_t>(n);
#else
  pollfd pfd{static_cast<int>(fd_), POLLIN, 0};
  const int r = ::poll(&pfd, 1, timeoutMs);
  if (r == 0) return std::size_t{0};
  if (r < 0) return Errc::SocketError;
  const ssize_t n =
      ::recv(static_cast<int>(fd_), buffer.data(), buffer.size(), MSG_DONTWAIT);
  if (n < 0) {
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? Result<std::size_t>(std::size_t{0})
                                                     : Result<std::size_t>(Errc::SocketError);
  }
  return static_cast<std::size_t>(n);
#endif
}

}  // namespace crowdy::replication
