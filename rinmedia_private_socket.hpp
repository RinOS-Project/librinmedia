/* SPDX-License-Identifier: MIT */
#ifndef RINMEDIA_PRIVATE_SOCKET_HPP
#define RINMEDIA_PRIVATE_SOCKET_HPP

#include "../libc/sys/socket.h"
#include "../libc/unistd.h"

namespace RinMedia {
namespace detail {

inline int createPrivateUnixStreamSocket() noexcept {
#if defined(SOCK_CLOEXEC)
    return socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    return -1;
#endif
}

} // namespace detail
} // namespace RinMedia

#endif
