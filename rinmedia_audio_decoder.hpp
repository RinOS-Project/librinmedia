/* SPDX-License-Identifier: MIT */
#ifndef RIN_MEDIA_AUDIO_DECODER_HPP
#define RIN_MEDIA_AUDIO_DECODER_HPP

#include "../libc/stdint.h"
#include "../libc/stddef.h"
#include "../libcxx/string.h"

namespace RinMedia {

using Sample = int16_t;
using Milliseconds = int64_t;

/* The media framework produces interleaved signed 16-bit PCM because that is
 * the currently published Rin audio sample format.  The device chooses the
 * rate and channel count; keep both values bounded before they reach FFmpeg's
 * resampler or a fixed-size service buffer. */
constexpr uint32_t kAudioOutputDefaultSampleRate = 48000u;
constexpr uint32_t kAudioOutputDefaultChannels = 2u;
constexpr uint32_t kAudioOutputMaxSampleRate = 384000u;
constexpr uint32_t kAudioOutputMaxChannels = 32u;

struct AudioOutputFormat {
    uint32_t sampleRate = kAudioOutputDefaultSampleRate;
    uint32_t channels = kAudioOutputDefaultChannels;
};

constexpr bool audioOutputFormatValid(const AudioOutputFormat& value) {
    return value.sampleRate != 0u &&
           value.sampleRate <= kAudioOutputMaxSampleRate &&
           value.channels != 0u && value.channels <= kAudioOutputMaxChannels;
}

struct AudioMetadata {
    std::string title;
    std::string artist;
    std::string album;
    Milliseconds durationMs = 0;
};

/* Shared media-framework audio decoder. Implementations own the supplied
 * descriptor and expose bounded, interleaved S16 PCM in the requested output
 * format.  A caller changing output format discards only undecoded/resampled
 * pending PCM; the container/codec position remains daemon-owned. */
class AudioDecoder {
    struct Impl;
    Impl* implementation = nullptr;

public:
    AudioDecoder();
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    /* Takes ownership of a File Portal descriptor. */
    bool openDescriptor(int descriptor);
    bool openDescriptor(int descriptor, const AudioOutputFormat& output);
    bool setOutputFormat(const AudioOutputFormat& output);
    int readFrames(Sample* destination, int maximumFrames);
    bool seek(Milliseconds positionMs);
    void close();
    bool isOpen() const;
    AudioOutputFormat outputFormat() const;
    const AudioMetadata& metadata() const;
    const std::string& lastError() const;
};

} // namespace RinMedia

#endif
