/* SPDX-License-Identifier: MIT */
/* Bounded AVI frame decoding for authenticated mediad sessions. */
#include "rinmedia_video_decoder.hpp"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace RinMedia {
namespace {

constexpr uint32_t kVideoMaximumDimension = 4096u;
constexpr uint64_t kVideoMaximumPixels = UINT64_C(16777216);
constexpr uint64_t kVideoMaximumFrameBytes = UINT64_C(67108864);
constexpr size_t kSourceReadChunkBytes = 64u * 1024u;

bool closeOwnedDescriptor(int descriptor) {
    return descriptor < 0 || ::close(descriptor) == 0;
}

bool frameTimestamp(uint64_t frame_index, uint32_t scale, uint32_t rate,
                    uint64_t* timestamp_out) {
    uint64_t ticks;
    uint64_t whole_seconds;
    uint64_t remainder;
    uint64_t fractional_ns;
    if (!timestamp_out || scale == 0u || rate == 0u ||
        frame_index > UINT64_MAX / scale)
        return false;
    ticks = frame_index * scale;
    whole_seconds = ticks / rate;
    remainder = ticks % rate;
    if (whole_seconds > UINT64_MAX / UINT64_C(1000000000))
        return false;
    fractional_ns = remainder * UINT64_C(1000000000) / rate;
    *timestamp_out = whole_seconds * UINT64_C(1000000000);
    if (fractional_ns > UINT64_MAX - *timestamp_out) return false;
    *timestamp_out += fractional_ns;
    return true;
}

} // namespace

VideoDecoder::~VideoDecoder() { close(); }

void VideoDecoder::close() {
    rvid_close(&player_);
    free(frame_pixels_);
    free(source_);
    frame_pixels_ = nullptr;
    source_ = nullptr;
    source_bytes_ = 0u;
    frame_capacity_pixels_ = 0u;
    memset(&player_, 0, sizeof(player_));
    memset(&info_, 0, sizeof(info_));
    open_ = false;
}

bool VideoDecoder::openDescriptor(int descriptor) {
    struct stat status = {};
    size_t offset = 0u;
    uint32_t interrupted = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t fps_numerator = 0u;
    uint32_t fps_denominator = 0u;
    uint64_t pixel_count;
    uint64_t frame_bytes;
    RAviStream* stream;

    close();
    if (descriptor < 0) return false;
    if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size <= 0 ||
        (uint64_t)status.st_size > RIN_MEDIA_MAX_VIDEO_SOURCE_BYTES_V1 ||
        (uint64_t)status.st_size > (uint64_t)SIZE_MAX) {
        (void)closeOwnedDescriptor(descriptor);
        return false;
    }
    source_bytes_ = (size_t)status.st_size;
    source_ = (uint8_t*)malloc(source_bytes_);
    if (source_ == nullptr) {
        source_bytes_ = 0u;
        (void)closeOwnedDescriptor(descriptor);
        return false;
    }
    while (offset < source_bytes_) {
        const size_t remaining = source_bytes_ - offset;
        const size_t chunk = remaining < kSourceReadChunkBytes
            ? remaining : kSourceReadChunkBytes;
        const ssize_t count = pread(descriptor, source_ + offset, chunk,
                                    (off_t)offset);
        if (count < 0 && errno == EINTR) {
            if (++interrupted < 32u) continue;
            (void)closeOwnedDescriptor(descriptor);
            close();
            return false;
        }
        if (count <= 0 || (size_t)count > chunk) {
            (void)closeOwnedDescriptor(descriptor);
            close();
            return false;
        }
        offset += (size_t)count;
        interrupted = 0u;
    }
    if (!closeOwnedDescriptor(descriptor)) {
        close();
        return false;
    }

    if (rvid_open(&player_, source_, source_bytes_) != RVID_OK ||
        player_.avi.video_stream < 0 ||
        player_.avi.video_stream >= RAVI_MAX_STREAMS ||
        player_.video_info.width <= 0 || player_.video_info.height <= 0 ||
        player_.video_info.total_frames == 0u) {
        close();
        return false;
    }
    stream = &player_.avi.streams[player_.avi.video_stream];
    width = (uint32_t)player_.video_info.width;
    height = (uint32_t)player_.video_info.height;
    fps_numerator = stream->rate;
    fps_denominator = stream->scale;
    if (width > kVideoMaximumDimension || height > kVideoMaximumDimension ||
        fps_numerator == 0u || fps_denominator == 0u ||
        (uint64_t)fps_numerator > (uint64_t)fps_denominator * 1000u) {
        close();
        return false;
    }
    pixel_count = (uint64_t)width * height;
    if (pixel_count == 0u || pixel_count > kVideoMaximumPixels ||
        pixel_count > SIZE_MAX / sizeof(uint32_t)) {
        close();
        return false;
    }
    frame_bytes = pixel_count * sizeof(uint32_t);
    if (frame_bytes > kVideoMaximumFrameBytes || frame_bytes > SIZE_MAX) {
        close();
        return false;
    }
    frame_capacity_pixels_ = (size_t)pixel_count;
    frame_pixels_ = (uint32_t*)malloc((size_t)frame_bytes);
    if (frame_pixels_ == nullptr) {
        close();
        return false;
    }
    memset(&info_, 0, sizeof(info_));
    info_.struct_size = sizeof(info_);
    info_.version = RIN_SDK_STRUCT_VERSION_1;
    info_.width = width;
    info_.height = height;
    info_.format = RIN_IMAGE_DECODE_FORMAT_ARGB8888;
    info_.row_stride_bytes = width * 4u;
    info_.frame_count = player_.video_info.total_frames;
    info_.flags = RIN_VIDEO_INFO_FRAME_COUNT_KNOWN;
    info_.duration_ns = (uint64_t)player_.video_info.duration_ms *
                        UINT64_C(1000000);
    info_.fps_numerator = fps_numerator;
    info_.fps_denominator = fps_denominator;
    open_ = true;
    return true;
}

bool VideoDecoder::decodeNextFrame(uint32_t max_width, uint32_t max_height,
                                   uint32_t max_frame_bytes,
                                   RinVideoFrameInfoV1* info_out,
                                   const uint8_t** output_out,
                                   RJpegCancellationFunction cancellation,
                                   void* cancellation_context) {
    RinVideoFrameInfoV1 info = {};
    const uint32_t frame_index = player_.current_frame;
    const uint64_t expected_bytes = (uint64_t)info_.row_stride_bytes *
                                    info_.height;
    if (output_out != nullptr) *output_out = nullptr;
    if (!open_ || !info_out || !output_out || max_width < info_.width ||
        max_height < info_.height || expected_bytes > max_frame_bytes ||
        expected_bytes > SIZE_MAX)
        return false;

    info.struct_size = sizeof(info);
    info.version = RIN_SDK_STRUCT_VERSION_1;
    info.width = info_.width;
    info.height = info_.height;
    info.format = info_.format;
    info.row_stride_bytes = info_.row_stride_bytes;
    info.frame_index = frame_index;

    const int decoded = rvid_next_frame_cancellable(
        &player_, frame_pixels_, frame_capacity_pixels_,
        (int)info_.width, (int)info_.height, cancellation,
        cancellation_context);
    if (decoded == RVID_END_OF_FILE) {
        info.flags = RIN_VIDEO_FRAME_FLAG_END_OF_STREAM;
        info.timestamp_ns = info_.duration_ns;
        info.payload_size_bytes = 0u;
        *info_out = info;
        return true;
    }
    if (decoded != RVID_OK ||
        !frameTimestamp(frame_index, info_.fps_denominator,
                        info_.fps_numerator, &info.timestamp_ns))
        return false;
    *output_out = (const uint8_t*)frame_pixels_;
    info.payload_size_bytes = expected_bytes;
    *info_out = info;
    return true;
}

} // namespace RinMedia
