/* SPDX-License-Identifier: MIT */
/* Bounded FFmpeg video decoder for authenticated mediad sessions. */
#ifndef RIN_MEDIA_FFMPEG_VIDEO_DECODER_HPP
#define RIN_MEDIA_FFMPEG_VIDEO_DECODER_HPP

#include <rin/media.h>
#include <stdint.h>

#include "rinvideo.h"

namespace RinMedia {

class FfmpegVideoDecoder final {
    struct Impl;
    Impl* implementation_ = nullptr;

public:
    FfmpegVideoDecoder();
    ~FfmpegVideoDecoder();
    FfmpegVideoDecoder(const FfmpegVideoDecoder&) = delete;
    FfmpegVideoDecoder& operator=(const FfmpegVideoDecoder&) = delete;

    /* Takes ownership of descriptor and closes it on every return path. */
    bool openDescriptor(int descriptor);
    void close();
    const RinVideoInfoV1& info() const;

    /* Output pixels stay owned by the decoder until its next decode/close. */
    bool decodeNextFrame(uint32_t max_width, uint32_t max_height,
                         uint32_t max_frame_bytes,
                         RinVideoFrameInfoV1* info_out,
                         const uint8_t** output_out,
                         RJpegCancellationFunction cancellation,
                         void* cancellation_context);
};

} // namespace RinMedia

#endif /* RIN_MEDIA_FFMPEG_VIDEO_DECODER_HPP */
