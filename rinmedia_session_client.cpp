/* SPDX-License-Identifier: MIT */
#include "rinmedia_session_client.hpp"

#include "../libc/errno.h"
#include "../libc/poll.h"
#include "../libc/string.h"
#include "../libc/sys/socket.h"
#include "../libc/sys/un.h"
#include "../libc/unistd.h"
#include "../../RinOS-SDK/include/rin/socket_abi.h"

namespace RinMedia {
namespace {

constexpr uint32_t kIoPollMs = 1000u;
constexpr uint32_t kIoIdleLimit = 5u;
constexpr uint32_t kAutostartRetryMs = 25u;
constexpr uint32_t kAutostartBudgetMs = 500u;
constexpr int kSystemServiceScope = 1;

extern "C" int rin_service_start(int scope, const char* id);
extern "C" void rin_sleep(unsigned int milliseconds);

bool transferExact(int fd, void* bytes, uint32_t size, bool receive) {
    uint8_t* cursor = static_cast<uint8_t*>(bytes);
    uint32_t offset = 0u;
    uint32_t idle = 0u;
    while (offset < size) {
        pollfd pfd = {};
        pfd.fd = fd;
        pfd.events = static_cast<short>(receive ? POLLIN : POLLOUT);
        const int ready = poll(&pfd, 1u, static_cast<int>(kIoPollMs));
        if (ready == 0) {
            if (++idle >= kIoIdleLimit) return false;
            continue;
        }
        if (ready < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
            (pfd.revents & pfd.events) == 0) return false;
        const ssize_t count = receive
            ? recv(fd, cursor + offset, static_cast<size_t>(size - offset), 0)
            : send(fd, cursor + offset, static_cast<size_t>(size - offset), MSG_NOSIGNAL);
        if (count <= 0 || static_cast<uint32_t>(count) > size - offset) return false;
        offset += static_cast<uint32_t>(count);
        idle = 0u;
    }
    return true;
}

bool serviceIdentityValid(int fd) {
    rin_unix_service_identity_v1 identity = {};
    socklen_t size = sizeof(identity);
    return getsockopt(fd, SOL_SOCKET, SO_RIN_UNIX_SERVICE_IDENTITY,
                      &identity, &size) == 0 && size == sizeof(identity) &&
           identity.slot_id != 0u && identity.owner_uid == 0u &&
           identity.scope == 1u &&
           identity.flags == RIN_UNIX_SERVICE_IDENTITY_FLAG_PUBLISHED;
}

int connectOnce() {
    sockaddr_un address = {};
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, RIN_MEDIA_SESSION_SOCKET_PATH,
            sizeof(address.sun_path) - 1u);
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        !serviceIdentityValid(fd)) {
        (void)::close(fd);
        return -1;
    }
    return fd;
}

int connectWithAutostart() {
    int fd = connectOnce();
    if (fd >= 0) return fd;
    const int start = rin_service_start(kSystemServiceScope, "mediasessiond");
    if (start == -EPERM || start == -ENOENT || start == -ENOSYS) return -1;
    for (uint32_t waited = 0u; waited < kAutostartBudgetMs;
         waited += kAutostartRetryMs) {
        rin_sleep(kAutostartRetryMs);
        fd = connectOnce();
        if (fd >= 0) return fd;
    }
    return -1;
}

bool validReply(const RinMediaSessionHeaderV1& reply, uint16_t operation,
                uint32_t request_id, uint64_t session_id) {
    return reply.magic == RIN_MEDIA_SESSION_MAGIC &&
           reply.version == RIN_MEDIA_SESSION_VERSION &&
           reply.operation == operation && reply.reserved0 == 0u &&
           reply.request_id == request_id && reply.session_id == session_id;
}

bool callOnSocket(int fd, uint32_t* next_request_id, uint16_t operation,
                  uint64_t session_id, const void* payload,
                  uint32_t payload_bytes, RinMediaSessionHeaderV1* reply) {
    RinMediaSessionHeaderV1 request = {};
    RinMediaSessionHeaderV1 response = {};
    if (fd < 0 || next_request_id == nullptr || reply == nullptr ||
        (payload_bytes != 0u && payload == nullptr)) return false;
    if (*next_request_id == 0u) *next_request_id = 1u;
    request.magic = RIN_MEDIA_SESSION_MAGIC;
    request.version = RIN_MEDIA_SESSION_VERSION;
    request.operation = operation;
    request.request_id = (*next_request_id)++;
    request.payload_bytes = payload_bytes;
    request.session_id = session_id;
    if (!transferExact(fd, &request, sizeof(request), false) ||
        (payload_bytes != 0u &&
         !transferExact(fd, const_cast<void*>(payload), payload_bytes, false)) ||
        !transferExact(fd, &response, sizeof(response), true) ||
        !validReply(response, operation, request.request_id, session_id)) return false;
    *reply = response;
    return true;
}

bool validPublisherInfo(const RinMediaSessionInfoV1& info) {
    return info.struct_size == sizeof(info) &&
           info.version == RIN_MEDIA_SESSION_VERSION && info.revision == 0u;
}

} // namespace

MediaSessionClient::~MediaSessionClient() { close(); }

void MediaSessionClient::reset() {
    if (socket_fd_ >= 0) (void)::close(socket_fd_);
    socket_fd_ = -1;
    session_id_ = 0u;
    next_request_id_ = 1u;
}

bool MediaSessionClient::call(uint16_t operation, const void* payload,
                              uint32_t payload_bytes,
                              RinMediaSessionHeaderV1* reply) {
    if (!isOpen() || !callOnSocket(socket_fd_, &next_request_id_, operation,
                                   session_id_, payload, payload_bytes, reply)) {
        error_ = "Media session service connection failed";
        reset();
        return false;
    }
    return true;
}

bool MediaSessionClient::publish(const RinMediaSessionInfoV1& info) {
    RinMediaSessionHeaderV1 reply = {};
    RinMediaSessionPublishReplyV1 published = {};
    reset();
    error_.clear();
    if (!validPublisherInfo(info)) {
        error_ = "Invalid media session information";
        return false;
    }
    const int fd = connectWithAutostart();
    uint32_t request_id = 1u;
    if (fd < 0 || !callOnSocket(fd, &request_id, RIN_MEDIA_SESSION_OP_PUBLISH,
                                0u, &info, sizeof(info), &reply) ||
        reply.status != 0 || reply.payload_bytes != sizeof(published) ||
        !transferExact(fd, &published, sizeof(published), true) ||
        published.session_id == 0u || published.revision == 0u) {
        if (fd >= 0) (void)::close(fd);
        error_ = "Media session service rejected publication";
        return false;
    }
    socket_fd_ = fd;
    session_id_ = published.session_id;
    next_request_id_ = request_id;
    return true;
}

bool MediaSessionClient::update(const RinMediaSessionInfoV1& info) {
    RinMediaSessionHeaderV1 reply = {};
    if (!validPublisherInfo(info)) {
        error_ = "Invalid media session information";
        return false;
    }
    if (!call(RIN_MEDIA_SESSION_OP_UPDATE, &info, sizeof(info), &reply) ||
        reply.status != 0 || reply.payload_bytes != 0u) {
        if (error_.empty()) error_ = "Media session update failed";
        return false;
    }
    return true;
}

bool MediaSessionClient::pollCommand(RinMediaSessionCommandMessageV1* command_out) {
    RinMediaSessionHeaderV1 reply = {};
    RinMediaSessionCommandMessageV1 command = {};
    if (command_out != nullptr) memset(command_out, 0, sizeof(*command_out));
    if (command_out == nullptr ||
        !call(RIN_MEDIA_SESSION_OP_POLL_COMMAND, nullptr, 0u, &reply) ||
        reply.status != 0 || reply.payload_bytes != sizeof(command) ||
        !transferExact(socket_fd_, &command, sizeof(command), true) ||
        command.reserved0 != 0u ||
        command.command > RIN_MEDIA_SESSION_COMMAND_STOP) {
        if (error_.empty()) error_ = "Media session command polling failed";
        reset();
        return false;
    }
    *command_out = command;
    return true;
}

void MediaSessionClient::close() {
    RinMediaSessionHeaderV1 reply = {};
    if (isOpen() &&
        (!call(RIN_MEDIA_SESSION_OP_UNPUBLISH, nullptr, 0u, &reply) ||
         reply.status != 0 || reply.payload_bytes != 0u) && error_.empty())
        error_ = "Media session close failed";
    reset();
}

bool MediaSessionDesktopClient::queryActive(
    RinMediaSessionPublishReplyV1* session_out, RinMediaSessionInfoV1* info_out) {
    RinMediaSessionHeaderV1 reply = {};
    RinMediaSessionPublishReplyV1 session = {};
    RinMediaSessionInfoV1 info = {};
    uint32_t request_id = 1u;
    const int fd = connectWithAutostart();
    error_.clear();
    if (session_out != nullptr) memset(session_out, 0, sizeof(*session_out));
    if (info_out != nullptr) memset(info_out, 0, sizeof(*info_out));
    if (fd < 0 || session_out == nullptr || info_out == nullptr ||
        !callOnSocket(fd, &request_id, RIN_MEDIA_SESSION_OP_QUERY_ACTIVE, 0u,
                      nullptr, 0u, &reply) || reply.status != 0 ||
        reply.payload_bytes != sizeof(session) + sizeof(info) ||
        !transferExact(fd, &session, sizeof(session), true) ||
        !transferExact(fd, &info, sizeof(info), true) || session.session_id == 0u ||
        session.revision == 0u) {
        if (fd >= 0) (void)::close(fd);
        error_ = "No active media session is available";
        return false;
    }
    (void)::close(fd);
    *session_out = session;
    *info_out = info;
    return true;
}

bool MediaSessionDesktopClient::dispatch(
    uint64_t session_id, RinMediaSessionCommandMessageV1 command) {
    RinMediaSessionHeaderV1 reply = {};
    uint32_t request_id = 1u;
    error_.clear();
    if (session_id == 0u || command.reserved0 != 0u ||
        command.revision != 0u ||
        command.command < RIN_MEDIA_SESSION_COMMAND_PLAY_PAUSE ||
        command.command > RIN_MEDIA_SESSION_COMMAND_STOP) {
        error_ = "Invalid media session command";
        return false;
    }
    const int fd = connectWithAutostart();
    if (fd < 0 ||
        !callOnSocket(fd, &request_id, RIN_MEDIA_SESSION_OP_DISPATCH_COMMAND,
                      session_id, &command, sizeof(command), &reply) ||
        reply.status != 0 || reply.payload_bytes != 0u) {
        if (fd >= 0) (void)::close(fd);
        error_ = "Media session command dispatch failed";
        return false;
    }
    (void)::close(fd);
    return true;
}

} // namespace RinMedia
