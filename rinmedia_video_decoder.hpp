/* SPDX-License-Identifier: MIT */
/* Bounded video frame decoder used by the authenticated mediad service. */
#ifndef RIN_MEDIA_VIDEO_DECODER_HPP
#define RIN_MEDIA_VIDEO_DECODER_HPP

#include <rin/media.h>
#include <stddef.h>
#include <stdint.h>

#include "rinvideo.h"
#include "rinmedia_ffmpeg_video_decoder.hpp"

namespace RinMedia {

class VideoDecoder final {
    RVidPlayer player_{};
    uint8_t* source_ = nullptr;
    size_t source_bytes_ = 0u;
    uint32_t* frame_pixels_ = nullptr;
    size_t frame_capacity_pixels_ = 0u;
    RinVideoInfoV1 info_{};
    FfmpegVideoDecoder ffmpeg_;
    bool open_ = false;
    bool using_ffmpeg_ = false;

public:
    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    /* Takes ownership of descriptor and closes it on every return path. */
    bool openDescriptor(int descriptor);
    void close();
    bool isOpen() const { return open_; }
    const RinVideoInfoV1& info() const { return info_; }

    /* Decodes one bounded ARGB8888 frame and returns a pointer to decoder-
     * owned pixels, valid until the next decode or close. A successful EOS
     * call returns metadata with END_OF_STREAM and a null pixel pointer. */
    bool decodeNextFrame(uint32_t max_width, uint32_t max_height,
                         uint32_t max_frame_bytes,
                         RinVideoFrameInfoV1* info_out,
                         const uint8_t** output_out,
                         RJpegCancellationFunction cancellation,
                         void* cancellation_context);

private:
    bool openFfmpegDescriptor(int descriptor);
};

} // namespace RinMedia

#endif /* RIN_MEDIA_VIDEO_DECODER_HPP */
