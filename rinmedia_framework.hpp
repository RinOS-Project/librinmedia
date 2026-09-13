/* SPDX-License-Identifier: MIT */
/* Backend-independent media framework contracts. */

#ifndef RIN_MEDIA_FRAMEWORK_HPP
#define RIN_MEDIA_FRAMEWORK_HPP

#include "../libc/stddef.h"
#include "../libc/stdint.h"

namespace RinMedia {

constexpr size_t kMediaPacketMaxBytes = 1024u * 1024u;
constexpr uint32_t kMediaMaxTracks = 32u;
constexpr uint32_t kMediaMaxChannels = 32u;

enum class MediaContainer : uint32_t {
    Unknown = 0u,
    Mp4 = 1u,
    Webm = 2u,
    Matroska = 3u,
    Avi = 4u,
    Wav = 5u,
};

enum class MediaTrackKind : uint32_t {
    Unknown = 0u,
    Audio = 1u,
    Video = 2u,
    Subtitle = 3u,
};

/* The source is already an authenticated File Portal descriptor.  Paths,
 * arbitrary shared-memory names, and backend-specific handles stay outside
 * this contract. */
struct MediaSource {
    int descriptor = -1;
    uint64_t generation = 0u;
    uint64_t byteLength = 0u;

    bool valid() const {
        return descriptor >= 0 && generation != 0u;
    }
};

struct MediaTrack {
    uint32_t id = 0u;
    MediaTrackKind kind = MediaTrackKind::Unknown;
    uint32_t codecId = 0u;
    uint32_t timeScale = 0u;
    uint64_t durationTicks = 0u;

    bool valid() const {
        return id != 0u && kind != MediaTrackKind::Unknown &&
               codecId != 0u && timeScale != 0u;
    }
};

struct MediaPacket {
    const uint8_t* data = nullptr;
    size_t byteLength = 0u;
    uint32_t trackId = 0u;
    int64_t pts = 0;
    int64_t dts = 0;
    uint32_t durationTicks = 0u;
    uint32_t flags = 0u;

    bool valid() const {
        return data != nullptr && byteLength != 0u &&
               byteLength <= kMediaPacketMaxBytes && trackId != 0u &&
               pts >= 0 && dts >= 0;
    }
};

struct AudioFrame {
    int16_t* samples = nullptr;
    size_t sampleCount = 0u;
    uint32_t channels = 0u;
    uint32_t sampleRate = 0u;
    int64_t timestampMs = 0;

    bool valid() const {
        return samples != nullptr && sampleCount != 0u &&
               channels != 0u && channels <= kMediaMaxChannels &&
               sampleRate != 0u && timestampMs >= 0;
    }
};

struct VideoFrame {
    uint32_t* pixels = nullptr;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t strideBytes = 0u;
    int64_t timestampMs = 0;

    bool valid() const {
        return pixels != nullptr && width != 0u && height != 0u &&
               width <= UINT32_MAX / 4u &&
               strideBytes >= width * 4u && timestampMs >= 0;
    }
};

/* The clock has no platform dependency; a session owner advances it from its
 * monotonic source and a renderer/audio sink consumes the published value. */
class PlaybackClock final {
    uint64_t positionMs_ = 0u;
    uint32_t ratePermille_ = 1000u;
    bool paused_ = true;

public:
    bool setPosition(uint64_t positionMs) {
        positionMs_ = positionMs;
        return true;
    }

    bool setRatePermille(uint32_t ratePermille) {
        if (ratePermille == 0u || ratePermille > 4000u) return false;
        ratePermille_ = ratePermille;
        return true;
    }

    void setPaused(bool paused) { paused_ = paused; }
    uint64_t positionMs() const { return positionMs_; }
    uint32_t ratePermille() const { return ratePermille_; }
    bool paused() const { return paused_; }
};

class Demuxer {
public:
    virtual ~Demuxer() = default;
    virtual bool open(const MediaSource& source) = 0;
    virtual MediaContainer container() const = 0;
    virtual uint32_t trackCount() const = 0;
    virtual bool track(uint32_t index, MediaTrack* output) const = 0;
    virtual bool readPacket(MediaPacket* output) = 0;
    virtual bool seek(uint64_t timestampMs) = 0;
};

class AudioDecoderBackend {
public:
    virtual ~AudioDecoderBackend() = default;
    virtual bool open(const MediaTrack& track) = 0;
    virtual bool decode(const MediaPacket& packet, AudioFrame* output) = 0;
};

class VideoDecoderBackend {
public:
    virtual ~VideoDecoderBackend() = default;
    virtual bool open(const MediaTrack& track) = 0;
    virtual bool decode(const MediaPacket& packet, VideoFrame* output) = 0;
};

} // namespace RinMedia

#endif /* RIN_MEDIA_FRAMEWORK_HPP */
