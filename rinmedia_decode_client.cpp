/* SPDX-License-Identifier: MIT */

#include "rinmedia_decode_service.hpp"

#include "../libc/errno.h"
#include "../libc/poll.h"
#include "../libc/string.h"
#include "../libc/sys/socket.h"
#include "../libc/sys/un.h"
#include "../libc/unistd.h"
#include "../../RinOS-SDK/include/rin/service.h"
#include "../../RinOS-SDK/include/rin/socket_abi.h"

namespace RinMedia {
namespace {

constexpr uint32_t kIoPollMs = 1000u;
constexpr uint32_t kIoIdleLimit = 5u;
constexpr uint32_t kAutostartRetryMs = 25u;
constexpr uint32_t kAutostartBudgetMs = 500u;
constexpr uint32_t kMaxPollEintr = 32u;

extern "C" void rin_sleep(unsigned int milliseconds);

bool pollCancelled(RinRuntimeCancellationFunction cancellation,
                   void* cancellationContext, bool* cancelled) {
    if (!cancellation || cancellation(cancellationContext) == 0) return false;
    if (cancelled) *cancelled = true;
    return true;
}

bool transferExact(int fd, void* bytes, uint32_t size, bool receive,
                   RinRuntimeCancellationFunction cancellation,
                   void* cancellationContext, bool* cancelled) {
    uint8_t* cursor = static_cast<uint8_t*>(bytes);
    uint32_t offset = 0u;
    uint32_t idle = 0u;
    uint32_t interrupted = 0u;
    while (offset < size) {
        if (pollCancelled(cancellation, cancellationContext, cancelled)) return false;
        pollfd pfd = {};
        pfd.fd = fd;
        pfd.events = static_cast<short>(receive ? POLLIN : POLLOUT);
        const int ready = poll(&pfd, 1u, static_cast<int>(kIoPollMs));
        if (ready < 0 && errno == EINTR) {
            if (++interrupted > kMaxPollEintr) return false;
            continue;
        }
        if (ready == 0) {
            if (++idle >= kIoIdleLimit) return false;
            continue;
        }
        if (ready < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
            (pfd.revents & pfd.events) == 0) return false;
        if (pollCancelled(cancellation, cancellationContext, cancelled)) return false;
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

int connectOnce(RinRuntimeCancellationFunction cancellation,
                void* cancellationContext, bool* cancelled) {
    if (pollCancelled(cancellation, cancellationContext, cancelled)) return -1;
    sockaddr_un address = {};
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, kMediaDecodeSocketPath,
            sizeof(address.sun_path) - 1u);
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0 || !serviceIdentityValid(fd)) {
        (void)::close(fd);
        return -1;
    }
    return fd;
}

int connectWithAutostart(RinRuntimeCancellationFunction cancellation,
                         void* cancellationContext, bool* cancelled) {
    int fd = connectOnce(cancellation, cancellationContext, cancelled);
    if (fd >= 0) return fd;
    if (pollCancelled(cancellation, cancellationContext, cancelled)) return -1;
    const RinResult start = rin_service_start(RIN_SERVICE_SCOPE_SYSTEM,
                                              "mediad");
    if (start == -EPERM || start == -ENOENT || start == -ENOSYS) return -1;
    for (uint32_t waited = 0u; waited < kAutostartBudgetMs;
         waited += kAutostartRetryMs) {
        rin_sleep(kAutostartRetryMs);
        fd = connectOnce(cancellation, cancellationContext, cancelled);
        if (fd >= 0) return fd;
        if (pollCancelled(cancellation, cancellationContext, cancelled)) return -1;
    }
    return -1;
}

bool validReply(const MediaDecodeHeaderV1& reply,
                MediaDecodeOperation operation, uint32_t requestId,
                uint64_t sessionId) {
    return reply.magic == kMediaDecodeMagic && reply.version == kMediaDecodeVersion &&
           reply.operation == static_cast<uint16_t>(operation) &&
           reply.reserved0 == 0u && reply.request_id == requestId &&
           reply.session_id == sessionId;
}

bool sendOpenWithDescriptor(int fd, const MediaDecodeHeaderV1& request,
                            const MediaDecodeOutputFormatV2& output,
                            int descriptor,
                            RinRuntimeCancellationFunction cancellation,
                            void* cancellationContext, bool* cancelled) {
    iovec iov[2] = {};
    alignas(struct cmsghdr) uint8_t control[CMSG_SPACE(sizeof(int))] = {};
    msghdr message = {};
    cmsghdr* cmsg;
    iov[0].iov_base = const_cast<MediaDecodeHeaderV1*>(&request);
    iov[0].iov_len = sizeof(request);
    iov[1].iov_base = const_cast<MediaDecodeOutputFormatV2*>(&output);
    iov[1].iov_len = sizeof(output);
    message.msg_iov = iov;
    message.msg_iovlen = 2u;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    cmsg = CMSG_FIRSTHDR(&message);
    if (!cmsg) return false;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(cmsg), &descriptor, sizeof(descriptor));
    if (pollCancelled(cancellation, cancellationContext, cancelled)) return false;
    pollfd ready = {};
    ready.fd = fd;
    ready.events = POLLOUT;
    uint32_t interrupted = 0u;
    int pollResult;
    do {
        pollResult = poll(&ready, 1u, static_cast<int>(kIoPollMs));
    } while (pollResult < 0 && errno == EINTR && ++interrupted <= kMaxPollEintr);
    if (pollResult <= 0 || (ready.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
        (ready.revents & POLLOUT) == 0 ||
        pollCancelled(cancellation, cancellationContext, cancelled))
        return false;
    return sendmsg(fd, &message, MSG_NOSIGNAL) ==
           static_cast<ssize_t>(sizeof(request) + sizeof(output));
}

MediaDecodeOutputFormatV2 wireOutput(const AudioOutputFormat& output) {
    MediaDecodeOutputFormatV2 wire = {};
    wire.sample_rate = output.sampleRate;
    wire.channels = output.channels;
    wire.sample_format = kMediaDecodeSampleFormatS16LE;
    return wire;
}

} // namespace

AudioDecoderClient::~AudioDecoderClient() { close(); }

void AudioDecoderClient::reset() {
    if (socket_fd_ >= 0) (void)::close(socket_fd_);
    socket_fd_ = -1;
    session_id_ = 0u;
    next_request_id_ = 1u;
    output_format_ = AudioOutputFormat();
    metadata_ = AudioMetadata();
}

bool AudioDecoderClient::call(MediaDecodeOperation operation, const void* request,
                              uint32_t requestBytes, MediaDecodeHeaderV1* reply,
                              bool honorCancellation) {
    MediaDecodeHeaderV1 header = {};
    MediaDecodeHeaderV1 response = {};
    if (socket_fd_ < 0 || session_id_ == 0u || !reply) return false;
    RinRuntimeCancellationFunction cancellation =
        honorCancellation ? cancellation_ : nullptr;
    void* cancellationContext = honorCancellation ? cancellation_context_ : nullptr;
    if (pollCancelled(cancellation, cancellationContext, &cancelled_)) {
        error_ = "メディアデコードがキャンセルされました";
        reset();
        return false;
    }
    if (next_request_id_ == 0u) next_request_id_ = 1u;
    header.magic = kMediaDecodeMagic;
    header.version = kMediaDecodeVersion;
    header.operation = static_cast<uint16_t>(operation);
    header.request_id = next_request_id_++;
    header.payload_bytes = requestBytes;
    header.session_id = session_id_;
    if (!transferExact(socket_fd_, &header, sizeof(header), false, cancellation,
                       cancellationContext, &cancelled_) ||
        (requestBytes != 0u && !transferExact(socket_fd_, const_cast<void*>(request),
                                               requestBytes, false, cancellation,
                                               cancellationContext, &cancelled_)) ||
        !transferExact(socket_fd_, &response, sizeof(response), true, cancellation,
                       cancellationContext, &cancelled_) ||
        !validReply(response, operation, header.request_id, session_id_)) {
        error_ = cancelled_ ? "メディアデコードがキャンセルされました"
                            : "Media decode service connection failed";
        reset();
        return false;
    }
    *reply = response;
    return true;
}

bool AudioDecoderClient::openDescriptor(int descriptor) {
    return openDescriptor(descriptor, AudioOutputFormat());
}

bool AudioDecoderClient::openDescriptor(int descriptor,
                                        const AudioOutputFormat& output) {
    MediaDecodeHeaderV1 request = {};
    MediaDecodeHeaderV1 reply = {};
    MediaDecodeOpenReplyV2 metadata = {};
    reset();
    cancelled_ = false;
    error_.clear();
    if (descriptor < 0 || !audioOutputFormatValid(output)) {
        if (descriptor >= 0) (void)::close(descriptor);
        error_ = "Invalid media descriptor";
        return false;
    }
    if (pollCancelled(cancellation_, cancellation_context_, &cancelled_)) {
        (void)::close(descriptor);
        error_ = "メディアデコードがキャンセルされました";
        return false;
    }
    const int fd = connectWithAutostart(cancellation_, cancellation_context_,
                                        &cancelled_);
    if (fd < 0) {
        (void)::close(descriptor);
        error_ = cancelled_ ? "メディアデコードがキャンセルされました"
                            : "Media decode service is unavailable";
        return false;
    }
    request.magic = kMediaDecodeMagic;
    request.version = kMediaDecodeVersion;
    request.operation = static_cast<uint16_t>(MediaDecodeOperation::Open);
    request.request_id = 1u;
    request.payload_bytes = sizeof(MediaDecodeOutputFormatV2);
    const MediaDecodeOutputFormatV2 requested = wireOutput(output);
    if (!sendOpenWithDescriptor(fd, request, requested, descriptor, cancellation_,
                                cancellation_context_, &cancelled_)) {
        (void)::close(descriptor);
        (void)::close(fd);
        error_ = cancelled_ ? "メディアデコードがキャンセルされました"
                            : "Could not transfer the media descriptor";
        return false;
    }
    (void)::close(descriptor);
    if (!transferExact(fd, &reply, sizeof(reply), true, cancellation_,
                       cancellation_context_, &cancelled_) ||
        !validReply(reply, MediaDecodeOperation::Open, request.request_id, 0u) ||
        reply.status != 0 || reply.session_id == 0u ||
        reply.payload_bytes != sizeof(metadata) ||
        !transferExact(fd, &metadata, sizeof(metadata), true, cancellation_,
                       cancellation_context_, &cancelled_) ||
        metadata.session_id != reply.session_id || metadata.reserved0 != 0u ||
        metadata.reserved1 != 0u ||
        metadata.output_sample_rate != output.sampleRate ||
        metadata.output_channels != output.channels ||
        metadata.sample_format != kMediaDecodeSampleFormatS16LE ||
        metadata.title_bytes > kMediaDecodeMaxTextBytes ||
        metadata.artist_bytes > kMediaDecodeMaxTextBytes ||
        metadata.album_bytes > kMediaDecodeMaxTextBytes || metadata.duration_ms < 0) {
        (void)::close(fd);
        error_ = cancelled_ ? "メディアデコードがキャンセルされました"
                            : "Media decode service rejected the descriptor";
        return false;
    }
    if (pollCancelled(cancellation_, cancellation_context_, &cancelled_)) {
        (void)::close(fd);
        error_ = "メディアデコードがキャンセルされました";
        return false;
    }
    socket_fd_ = fd;
    session_id_ = reply.session_id;
    next_request_id_ = 2u;
    output_format_ = output;
    metadata_.durationMs = metadata.duration_ms;
    metadata_.title.assign(metadata.title, metadata.title_bytes);
    metadata_.artist.assign(metadata.artist, metadata.artist_bytes);
    metadata_.album.assign(metadata.album, metadata.album_bytes);
    return true;
}

bool AudioDecoderClient::setOutputFormat(const AudioOutputFormat& output) {
    MediaDecodeHeaderV1 reply = {};
    if (!isOpen() || !audioOutputFormatValid(output)) return false;
    if (pollCancelled(cancellation_, cancellation_context_, &cancelled_)) {
        error_ = "メディアデコードがキャンセルされました";
        reset();
        return false;
    }
    if (output.sampleRate == output_format_.sampleRate &&
        output.channels == output_format_.channels)
        return true;
    const MediaDecodeOutputFormatV2 request = wireOutput(output);
    if (!call(MediaDecodeOperation::ConfigureOutput, &request, sizeof(request), &reply)) {
        if (error_.empty()) error_ = "Media decode output configuration failed";
        reset();
        return false;
    }
    if (pollCancelled(cancellation_, cancellation_context_, &cancelled_) ||
        reply.status != 0 || reply.payload_bytes != 0u) {
        if (error_.empty())
            error_ = cancelled_ ? "メディアデコードがキャンセルされました"
                                : "Media decode output configuration failed";
        reset();
        return false;
    }
    output_format_ = output;
    return true;
}

void AudioDecoderClient::setCancellation(
    RinRuntimeCancellationFunction cancellation, void* cancellation_context) {
    cancellation_ = cancellation;
    cancellation_context_ = cancellation_context;
    cancelled_ = false;
}

int AudioDecoderClient::readFrames(Sample* destination, int maximumFrames) {
    MediaDecodeReadRequestV1 request = {};
    MediaDecodeHeaderV1 reply = {};
    MediaDecodeReadReplyV1 result = {};
    if (!destination || maximumFrames <= 0 ||
        maximumFrames > static_cast<int>(kMediaDecodeMaxFrames)) return -1;
    const uint32_t maximumBytes = static_cast<uint32_t>(maximumFrames) *
                                  output_format_.channels * sizeof(Sample);
    request.maximum_frames = static_cast<uint32_t>(maximumFrames);
    if (pollCancelled(cancellation_, cancellation_context_, &cancelled_)) {
        error_ = "メディアデコードがキャンセルされました";
        reset();
        return -1;
    }
    if (!call(MediaDecodeOperation::Read, &request, sizeof(request), &reply)) {
        if (error_.empty())
            error_ = cancelled_ ? "メディアデコードがキャンセルされました"
                                : "Media decode service returned invalid audio";
        reset();
        return -1;
    }
    if (pollCancelled(cancellation_, cancellation_context_, &cancelled_) ||
        reply.status != 0 || reply.payload_bytes < sizeof(result) ||
        reply.payload_bytes > sizeof(result) + maximumBytes ||
        !transferExact(socket_fd_, &result, sizeof(result), true, cancellation_,
                       cancellation_context_, &cancelled_) ||
        result.reserved0 != 0u || result.frames > static_cast<uint32_t>(maximumFrames) ||
        reply.payload_bytes != sizeof(result) + result.frames * output_format_.channels * sizeof(Sample) ||
        (result.frames != 0u && !transferExact(socket_fd_, destination,
                                                result.frames * output_format_.channels * sizeof(Sample), true,
                                                cancellation_, cancellation_context_, &cancelled_))) {
        if (error_.empty())
            error_ = cancelled_ ? "メディアデコードがキャンセルされました"
                                : "Media decode service returned invalid audio";
        reset();
        return -1;
    }
    return static_cast<int>(result.frames);
}

bool AudioDecoderClient::seek(Milliseconds positionMs) {
    MediaDecodeSeekRequestV1 request = {};
    MediaDecodeHeaderV1 reply = {};
    if (positionMs < 0) return false;
    if (pollCancelled(cancellation_, cancellation_context_, &cancelled_)) {
        error_ = "メディアデコードがキャンセルされました";
        reset();
        return false;
    }
    request.position_ms = positionMs;
    if (!call(MediaDecodeOperation::Seek, &request, sizeof(request), &reply)) {
        if (error_.empty()) error_ = "Media decode seek failed";
        return false;
    }
    if (pollCancelled(cancellation_, cancellation_context_, &cancelled_) ||
        reply.status != 0 || reply.payload_bytes != 0u) {
        if (error_.empty())
            error_ = cancelled_ ? "メディアデコードがキャンセルされました"
                                : "Media decode seek failed";
        return false;
    }
    return true;
}

void AudioDecoderClient::close() {
    MediaDecodeHeaderV1 reply = {};
    if (socket_fd_ >= 0 && session_id_ != 0u &&
        call(MediaDecodeOperation::Close, nullptr, 0u, &reply, false) &&
        (reply.status != 0 || reply.payload_bytes != 0u)) {
        error_ = "Media decode service close failed";
    }
    reset();
}

} // namespace RinMedia
