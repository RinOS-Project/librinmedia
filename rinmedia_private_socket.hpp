/* SPDX-License-Identifier: MIT */
#ifndef RINMEDIA_PRIVATE_SOCKET_HPP
#define RINMEDIA_PRIVATE_SOCKET_HPP

#include "../libc/errno.h"
#include "../libc/fcntl.h"
#include "../libc/sys/socket.h"
#include "../libc/unistd.h"

namespace RinMedia {
namespace detail {

inline int createPrivateUnixStreamSocket() noexcept {
    int fd;
#if defined(SOCK_CLOEXEC)
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) return fd;
    if (errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP) return -1;
#endif
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    const int descriptorFlags = fcntl(fd, F_GETFD, 0);
    if (descriptorFlags < 0 ||
        fcntl(fd, F_SETFD, descriptorFlags | FD_CLOEXEC) != 0) {
        (void)::close(fd);
        return -1;
    }
    return fd;
}

} // namespace detail
} // namespace RinMedia

#endif
