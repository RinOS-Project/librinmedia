/* SPDX-License-Identifier: MIT */

#include "rinmedia_decode_service.hpp"
#include "rinmedia_event_loop.hpp"
#include "rinmedia_private_socket.hpp"

#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <rin/service.h>
#include <rin/socket_abi.h>

namespace RinMedia {
namespace {

constexpr uint32_t kIoIdleLimit = 5u;
constexpr uint32_t kIoInterruptionLimit = 32u;
constexpr uint32_t kIoReadinessRaceLimit = 32u;
constexpr uint32_t kAutostartRetryMs = 25u;
constexpr uint32_t kAutostartBudgetMs = 500u;
constexpr size_t kSourceSnapshotChunkBytes = 64u * 1024u;

bool mediaRequestRangeValid(uint64_t address, uint64_t size,
                            uint64_t maximumSize) {
    if (address == 0u || size == 0u || size > maximumSize ||
        address > static_cast<uint64_t>(UINTPTR_MAX))
        return false;
    return size <= static_cast<uint64_t>(UINTPTR_MAX) - address;
}

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
    uint32_t readiness_races = 0u;
    while (offset < size) {
        if (pollCancelled(cancellation, cancellationContext, cancelled)) return false;
        const auto wait_status = detail::waitForMediaFd(
            fd, receive ? RinRuntime::EventLoop::WAIT_READABLE
                         : RinRuntime::EventLoop::WAIT_WRITABLE);
        if (wait_status == detail::MediaWaitStatus::Timeout) {
            if (++idle >= kIoIdleLimit) return false;
            continue;
        }
        if (wait_status != detail::MediaWaitStatus::Ready) return false;
        if (pollCancelled(cancellation, cancellationContext, cancelled)) return false;
        const ssize_t count = receive
            ? recv(fd, cursor + offset, static_cast<size_t>(size - offset), 0)
            : send(fd, cursor + offset, static_cast<size_t>(size - offset), MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) {
            if (++interrupted >= kIoInterruptionLimit) return false;
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (++readiness_races >= kIoReadinessRaceLimit) return false;
            continue;
        }
        if (count <= 0 || static_cast<uint32_t>(count) > size - offset) return false;
        offset += static_cast<uint32_t>(count);
        idle = 0u;
        interrupted = 0u;
        readiness_races = 0u;
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
    int fd = detail::createPrivateUnixStreamSocket();
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

void clearReadOutput(Sample* destination, int maximumFrames,
                     uint32_t channels) {
    if (destination == nullptr || maximumFrames <= 0 || channels == 0u ||
        channels > kAudioOutputMaxChannels)
        return;
    const std::size_t samples = static_cast<std::size_t>(maximumFrames) *
                                static_cast<std::size_t>(channels);
    if (samples > SIZE_MAX / sizeof(Sample)) return;
    memset(destination, 0, samples * sizeof(Sample));
}

bool sendOpenWithDescriptor(int fd, const MediaDecodeHeaderV1& request,
                            const MediaDecodeOutputFormatV2& output,
                            int descriptor,
                            RinRuntimeCancellationFunction cancellation,
                            void* cancellationContext, bool* cancelled) {
    uint8_t frame[sizeof(request) + sizeof(output)] = {};
    iovec iov = {};
    alignas(struct cmsghdr) uint8_t control[CMSG_SPACE(sizeof(int))] = {};
    msghdr message = {};
    cmsghdr* cmsg;
    memcpy(frame, &request, sizeof(request));
    memcpy(frame + sizeof(request), &output, sizeof(output));
    iov.iov_base = frame;
    iov.iov_len = sizeof(frame);
    message.msg_iov = &iov;
    message.msg_iovlen = 1u;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    cmsg = CMSG_FIRSTHDR(&message);
    if (!cmsg) return false;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(cmsg), &descriptor, sizeof(descriptor));
    uint32_t offset = 0u;
    uint32_t idle = 0u;
    uint32_t interrupted = 0u;
    uint32_t readiness_races = 0u;
    while (offset < sizeof(frame)) {
        if (pollCancelled(cancellation, cancellationContext, cancelled)) return false;
        const auto wait = detail::waitForMediaFd(
            fd, RinRuntime::EventLoop::WAIT_WRITABLE);
        if (wait == detail::MediaWaitStatus::Timeout) {
            if (++idle >= kIoIdleLimit) return false;
            continue;
        }
        if (wait != detail::MediaWaitStatus::Ready ||
            pollCancelled(cancellation, cancellationContext, cancelled))
            return false;

        iov.iov_base = frame + offset;
        iov.iov_len = sizeof(frame) - offset;
        const ssize_t count = offset == 0u
            ? sendmsg(fd, &message, MSG_NOSIGNAL)
            : send(fd, frame + offset, sizeof(frame) - offset, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) {
            if (++interrupted >= kIoInterruptionLimit) return false;
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (++readiness_races >= kIoReadinessRaceLimit) return false;
            continue;
        }
        if (count <= 0 || static_cast<size_t>(count) > sizeof(frame) - offset)
            return false;
        offset += static_cast<uint32_t>(count);
        idle = 0u;
        interrupted = 0u;
        readiness_races = 0u;
        /* SCM_RIGHTS is attached only to the first stream fragment. */
        message.msg_control = nullptr;
        message.msg_controllen = 0u;
    }
    return true;
}

bool writeSourceSnapshot(FILE* stream, const void* source,
                         uint64_t sourceBytes,
                         RinRuntimeCancellationFunction cancellation,
                         void* cancellationContext, bool* cancelled) {
    const uint8_t* cursor = static_cast<const uint8_t*>(source);
    uint64_t remaining = sourceBytes;
    while (remaining != 0u) {
        if (pollCancelled(cancellation, cancellationContext, cancelled))
            return false;
        const size_t chunk = remaining < kSourceSnapshotChunkBytes
            ? static_cast<size_t>(remaining)
            : kSourceSnapshotChunkBytes;
        if (fwrite(cursor, 1u, chunk, stream) != chunk) return false;
        cursor += chunk;
        remaining -= chunk;
    }
    return ferror(stream) == 0;
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
    if (socket_fd_ < 0 || session_id_ == 0u || !reply ||
        (requestBytes != 0u && request == nullptr))
        return false;
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
    AudioMetadata candidateMetadata;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {
#endif
        candidateMetadata.durationMs = metadata.duration_ms;
        candidateMetadata.title.assign(metadata.title, metadata.title_bytes);
        candidateMetadata.artist.assign(metadata.artist, metadata.artist_bytes);
        candidateMetadata.album.assign(metadata.album, metadata.album_bytes);
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    } catch (...) {
        (void)::close(fd);
        error_ = "メディアデコードのmetadataを確保できません";
        return false;
    }
#endif
    socket_fd_ = fd;
    session_id_ = reply.session_id;
    next_request_id_ = 2u;
    output_format_ = output;
    metadata_.durationMs = candidateMetadata.durationMs;
    metadata_.title.swap(candidateMetadata.title);
    metadata_.artist.swap(candidateMetadata.artist);
    metadata_.album.swap(candidateMetadata.album);
    return true;
}

bool AudioDecoderClient::openBytes(const void* source, uint64_t sourceBytes) {
    return openBytes(source, sourceBytes, AudioOutputFormat());
}

bool AudioDecoderClient::openBytes(const void* source, uint64_t sourceBytes,
                                   const AudioOutputFormat& output) {
    FILE* snapshot = nullptr;
    int ownedDescriptor = -1;
    bool snapshotReady = false;

    reset();
    cancelled_ = false;
    error_.clear();
    if (source == nullptr || sourceBytes == 0u ||
        sourceBytes > kMediaDecodeMaxSourceBytes ||
        !audioOutputFormatValid(output)) {
        error_ = "Invalid media source snapshot";
        return false;
    }
    const uintptr_t sourceAddress = reinterpret_cast<uintptr_t>(source);
    if (sourceBytes > static_cast<uint64_t>(UINTPTR_MAX - sourceAddress)) {
        error_ = "Invalid media source snapshot range";
        return false;
    }
    if (pollCancelled(cancellation_, cancellation_context_, &cancelled_)) {
        error_ = "メディアデコードがキャンセルされました";
        return false;
    }

    snapshot = tmpfile();
    if (snapshot == nullptr) {
        error_ = "Could not create private media source snapshot";
        return false;
    }
    const int snapshotDescriptor = fileno(snapshot);
    if (snapshotDescriptor < 0 ||
        fcntl(snapshotDescriptor, F_SETFD, FD_CLOEXEC) < 0) {
        (void)fclose(snapshot);
        error_ = "Could not protect private media source snapshot";
        return false;
    }

    snapshotReady = writeSourceSnapshot(snapshot, source, sourceBytes,
                                        cancellation_, cancellation_context_,
                                        &cancelled_) &&
                    fflush(snapshot) == 0 && ferror(snapshot) == 0 &&
                    fseek(snapshot, 0L, SEEK_SET) == 0;
    if (!snapshotReady) {
        (void)fclose(snapshot);
        error_ = cancelled_ ? "メディアデコードがキャンセルされました"
                            : "Could not copy media source into private snapshot";
        return false;
    }

    const int sourceDescriptor = fileno(snapshot);
    if (sourceDescriptor >= 0)
        ownedDescriptor = fcntl(sourceDescriptor, F_DUPFD_CLOEXEC, 0);
    const int closeResult = fclose(snapshot);
    if (ownedDescriptor < 0 || closeResult != 0) {
        if (ownedDescriptor >= 0) (void)::close(ownedDescriptor);
        error_ = "Could not transfer private media source snapshot";
        return false;
    }

    /* openDescriptor consumes and closes ownedDescriptor on every path. The
     * backing tmpfile has already been unlinked, so no path authority or
     * caller-owned source memory crosses the service boundary. */
    return openDescriptor(ownedDescriptor, output);
}

bool AudioDecoderClient::openMediaRequestV1(
    const RinMediaOpenV1* request) {
    return openMediaRequestV1(request, AudioOutputFormat());
}

bool AudioDecoderClient::openMediaRequestV1(
    const RinMediaOpenV1* request, const AudioOutputFormat& output) {
    RinMediaOpenV1 snapshot = {};

    reset();
    cancelled_ = false;
    error_.clear();
    if (!request) {
        error_ = "Invalid RinMediaOpenV1 request";
        return false;
    }
    memcpy(&snapshot, request, sizeof(snapshot));
    if (snapshot.struct_size < sizeof(snapshot) ||
        snapshot.version != RIN_SDK_STRUCT_VERSION_1) {
        error_ = "RinMediaOpenV1 ABI mismatch";
        return false;
    }
    if (snapshot.media_type != RIN_MEDIA_TYPE_AUDIO) {
        error_ = (snapshot.media_type == RIN_MEDIA_TYPE_IMAGE ||
                  snapshot.media_type == RIN_MEDIA_TYPE_VIDEO)
                     ? "This decoder client accepts audio requests only"
                     : "Invalid RinMediaOpenV1 media type";
        return false;
    }
    if (snapshot.flags != 0u ||
        !mediaRequestRangeValid(snapshot.mime_type.address,
                                snapshot.mime_type.size, UINT64_MAX) ||
        !mediaRequestRangeValid(snapshot.source.address,
                                snapshot.source.size,
                                kMediaDecodeMaxSourceBytes) ||
        snapshot.reserved[0] != 0u || snapshot.reserved[1] != 0u ||
        snapshot.reserved[2] != 0u || snapshot.reserved[3] != 0u) {
        error_ = "Invalid RinMediaOpenV1 request fields";
        return false;
    }

    /* MIME bytes remain advisory at this service boundary: MDC1 currently
     * authenticates the source descriptor and the decoder probes its content.
     * Do not reinterpret the MIME span as source authority. */
    return openBytes(reinterpret_cast<const void*>(
                         static_cast<uintptr_t>(snapshot.source.address)),
                     snapshot.source.size, output);
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
    if (!audioOutputFormatValid(output_format_)) return -1;
    const std::size_t maximumBytesSize =
        static_cast<std::size_t>(maximumFrames) *
        static_cast<std::size_t>(output_format_.channels) * sizeof(Sample);
    if (maximumBytesSize > UINT32_MAX) return -1;
    clearReadOutput(destination, maximumFrames, output_format_.channels);
    const auto failRead = [&]() -> int {
        clearReadOutput(destination, maximumFrames, output_format_.channels);
        reset();
        return -1;
    };
    const uint32_t maximumBytes = static_cast<uint32_t>(maximumBytesSize);
    request.maximum_frames = static_cast<uint32_t>(maximumFrames);
    if (pollCancelled(cancellation_, cancellation_context_, &cancelled_)) {
        error_ = "メディアデコードがキャンセルされました";
        return failRead();
    }
    if (!call(MediaDecodeOperation::Read, &request, sizeof(request), &reply)) {
        if (error_.empty())
            error_ = cancelled_ ? "メディアデコードがキャンセルされました"
                                : "Media decode service returned invalid audio";
        return failRead();
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
        return failRead();
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
    if (pollCancelled(cancellation_, cancellation_context_, &cancelled_)) {
        error_ = "メディアデコードがキャンセルされました";
        reset();
        return false;
    }
    if (reply.status != 0 || reply.payload_bytes != 0u) {
        if (error_.empty())
            error_ = "Media decode seek failed";
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
