#pragma once

#include "crowdy/core/result.hpp"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <cerrno>
#endif

/// Internal to the replication library; not an installed header.
namespace crowdy::replication::detail {

/// Classify the error from a failed datagram send.
///
/// A full kernel send buffer is not a fault: the socket is healthy, nothing was
/// transferred, and the same datagram will go out on a later attempt. Reporting
/// it as SocketError is what made burst load look like a broken socket.
///
/// ENOBUFS sits with EAGAIN because Linux reports a momentarily full device
/// queue that way, and it is just as transient. Which of the two you get
/// depends on the datagram size rather than on anything the caller did wrong.
inline Errc sendErrcFromNativeError(int nativeError) {
#ifdef _WIN32
  return (nativeError == WSAEWOULDBLOCK || nativeError == WSAENOBUFS) ? Errc::WouldBlock
                                                                      : Errc::SocketError;
#else
  return (nativeError == EAGAIN || nativeError == EWOULDBLOCK || nativeError == ENOBUFS)
             ? Errc::WouldBlock
             : Errc::SocketError;
#endif
}

}  // namespace crowdy::replication::detail
