/* SPDX-License-Identifier: MIT */
/* Bounded AVI frame decoder used by the authenticated mediad service. */
#ifndef RIN_MEDIA_VIDEO_DECODER_HPP
#define RIN_MEDIA_VIDEO_DECODER_HPP

#include <rin/media.h>
#include <stddef.h>
#include <stdint.h>

#include "rinvideo.h"

namespace RinMedia {

class VideoDecoder final {
    RVidPlayer player_{};
    uint8_t* source_ = nullptr;
    size_t source_bytes_ = 0u;
    uint32_t* frame_pixels_ = nullptr;
    size_t frame_capacity_pixels_ = 0u;
    RinVideoInfoV1 info_{};
    bool open_ = false;

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

    /* Decodes one bounded ARGB8888 frame into output. A successful EOS call
     * returns metadata with END_OF_STREAM and no pixel payload. */
    bool decodeNextFrame(uint32_t max_width, uint32_t max_height,
                         uint32_t max_frame_bytes,
                         RinVideoFrameInfoV1* info_out, uint8_t* output,
                         size_t output_capacity);
};

} // namespace RinMedia

#endif /* RIN_MEDIA_VIDEO_DECODER_HPP */
