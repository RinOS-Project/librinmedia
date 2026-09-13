/* SPDX-License-Identifier: MIT */
/* Versioned, bounded wire contract for the daemon-owned audio decoder. */
#ifndef RIN_MEDIA_DECODE_SERVICE_HPP
#define RIN_MEDIA_DECODE_SERVICE_HPP

#include "rinmedia_audio_decoder.hpp"

#include <stdint.h>

namespace RinMedia {

constexpr uint32_t kMediaDecodeMagic = UINT32_C(0x3143444d); /* "MDC1" */
constexpr uint32_t kMediaDecodeVersion = 2u;
constexpr uint32_t kMediaDecodeMaxTextBytes = 4096u;
constexpr uint32_t kMediaDecodeMaxFrames = 1024u;
constexpr uint32_t kMediaDecodeSampleFormatS16LE = 1u;
constexpr const char kMediaDecodeSocketPath[] = "/run/rin/mediad.sock";

enum class MediaDecodeOperation : uint16_t {
    Open = 1u,
    Read = 2u,
    Seek = 3u,
    Close = 4u,
    ConfigureOutput = 5u,
};

/* The request uses status == 0.  Responses echo operation/request_id and use
 * a negative status on rejection; no caller-supplied pointer or pathname is
 * ever serialised. */
struct MediaDecodeHeaderV1 {
    uint32_t magic;
    uint32_t version;
    uint16_t operation;
    uint16_t reserved0;
    uint32_t request_id;
    uint32_t payload_bytes;
    int32_t status;
    uint64_t session_id;
};

/* Version 2 puts an explicit, bounded PCM output request on both Open and
 * ConfigureOutput.  The fixed header layout is unchanged, so a v1 peer is
 * rejected by the header-version check before descriptor ownership transfers. */
struct MediaDecodeOutputFormatV2 {
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t sample_format;
    uint32_t reserved0;
};

struct MediaDecodeOpenReplyV2 {
    uint64_t session_id;
    int64_t duration_ms;
    uint32_t output_sample_rate;
    uint32_t output_channels;
    uint32_t sample_format;
    uint32_t reserved0;
    uint32_t title_bytes;
    uint32_t artist_bytes;
    uint32_t album_bytes;
    uint32_t reserved1;
    char title[kMediaDecodeMaxTextBytes];
    char artist[kMediaDecodeMaxTextBytes];
    char album[kMediaDecodeMaxTextBytes];
};

struct MediaDecodeReadRequestV1 {
    uint32_t maximum_frames;
    uint32_t reserved0;
};

struct MediaDecodeReadReplyV1 {
    uint32_t frames;
    uint32_t reserved0;
};

struct MediaDecodeSeekRequestV1 {
    int64_t position_ms;
};

static_assert(sizeof(MediaDecodeHeaderV1) == 32u,
              "media decode header ABI drift");
static_assert(sizeof(MediaDecodeOutputFormatV2) == 16u,
              "media decode output format ABI drift");
static_assert(sizeof(MediaDecodeOpenReplyV2) == 12336u,
              "media decode open reply ABI drift");
static_assert(sizeof(MediaDecodeReadRequestV1) == 8u,
              "media decode read request ABI drift");
static_assert(sizeof(MediaDecodeReadReplyV1) == 8u,
              "media decode read reply ABI drift");
static_assert(sizeof(MediaDecodeSeekRequestV1) == 8u,
              "media decode seek request ABI drift");

class AudioDecoderClient final {
    int socket_fd_ = -1;
    uint64_t session_id_ = 0u;
    uint32_t next_request_id_ = 1u;
    AudioOutputFormat output_format_;
    AudioMetadata metadata_;
    std::string error_;

    void reset();
    bool call(MediaDecodeOperation operation, const void* request,
              uint32_t request_bytes, MediaDecodeHeaderV1* reply);

public:
    AudioDecoderClient() = default;
    ~AudioDecoderClient();
    AudioDecoderClient(const AudioDecoderClient&) = delete;
    AudioDecoderClient& operator=(const AudioDecoderClient&) = delete;

    /* Takes ownership of descriptor.  It sends exactly one SCM_RIGHTS FD to
     * the authenticated daemon; the descriptor is closed locally on every
     * result and cannot be retained by the application after this call. */
    bool openDescriptor(int descriptor);
    bool openDescriptor(int descriptor, const AudioOutputFormat& output);
    bool setOutputFormat(const AudioOutputFormat& output);
    int readFrames(Sample* destination, int maximumFrames);
    bool seek(Milliseconds positionMs);
    void close();
    bool isOpen() const { return socket_fd_ >= 0 && session_id_ != 0u; }
    AudioOutputFormat outputFormat() const { return output_format_; }
    const AudioMetadata& metadata() const { return metadata_; }
    const std::string& lastError() const { return error_; }
};

} // namespace RinMedia

#endif /* RIN_MEDIA_DECODE_SERVICE_HPP */
