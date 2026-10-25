/* SPDX-License-Identifier: MIT */
/* Bounded FFmpeg video decode for authenticated mediad sessions. */
#include "rinmedia_ffmpeg_video_decoder.hpp"

/* Preload libc headers before FFmpeg opens its C-linkage include boundary. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <new>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace RinMedia {
namespace {

constexpr int64_t kMaxProbeBytes = 4 * 1024 * 1024;
constexpr int64_t kMaxAnalyzeDurationUs = 10 * AV_TIME_BASE;
constexpr uint64_t kMaxSourceBytes = RIN_MEDIA_MAX_VIDEO_SOURCE_BYTES_V1;
constexpr uint64_t kMaxCumulativeReadBytes = UINT64_C(4) * kMaxSourceBytes;
constexpr uint32_t kMaxDimension = 4096u;
constexpr uint64_t kMaxPixels = UINT64_C(16777216);
constexpr uint64_t kMaxFrameBytes = UINT64_C(67108864);
constexpr int kMaxStreams = 32;
constexpr int kMaxProbePackets = 2048;
constexpr unsigned int kMaxIndexBytes = 4u * 1024u * 1024u;
constexpr int kMaxPacketBytes = 16 * 1024 * 1024;
constexpr int kMaxExtradataBytes = 1 * 1024 * 1024;
constexpr int kIoBufferBytes = 32768;

bool closeDescriptor(int descriptor) {
    return descriptor < 0 || ::close(descriptor) == 0;
}

bool validFrameGeometry(uint32_t width, uint32_t height,
                        uint32_t* stride_out, uint64_t* bytes_out) {
    uint64_t pixels;
    uint64_t bytes;
    if (stride_out) *stride_out = 0u;
    if (bytes_out) *bytes_out = 0u;
    if (!stride_out || !bytes_out || width == 0u || height == 0u ||
        width > kMaxDimension || height > kMaxDimension ||
        width > UINT32_MAX / 4u)
        return false;
    pixels = static_cast<uint64_t>(width) * height;
    if (pixels == 0u || pixels > kMaxPixels ||
        pixels > SIZE_MAX / sizeof(uint32_t))
        return false;
    bytes = pixels * sizeof(uint32_t);
    if (bytes > kMaxFrameBytes || bytes > SIZE_MAX) return false;
    *stride_out = width * 4u;
    *bytes_out = bytes;
    return true;
}

bool rationalToU32(AVRational value, uint32_t* numerator_out,
                   uint32_t* denominator_out) {
    if (numerator_out) *numerator_out = 0u;
    if (denominator_out) *denominator_out = 0u;
    if (!numerator_out || !denominator_out || value.num <= 0 ||
        value.den <= 0)
        return false;
    if (static_cast<uint64_t>(value.num) >
        static_cast<uint64_t>(value.den) * UINT64_C(1000))
        return false;
    *numerator_out = static_cast<uint32_t>(value.num);
    *denominator_out = static_cast<uint32_t>(value.den);
    return true;
}

bool timestampToNanoseconds(int64_t timestamp, AVRational time_base,
                            uint64_t* timestamp_out) {
    if (timestamp_out) *timestamp_out = 0u;
    if (!timestamp_out || timestamp == AV_NOPTS_VALUE || timestamp < 0 ||
        time_base.num <= 0 || time_base.den <= 0)
        return false;
    const int64_t nanoseconds = av_rescale_q_rnd(
        timestamp, time_base, AVRational{1, 1000000000},
        static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
    if (nanoseconds < 0 || nanoseconds == INT64_MAX) return false;
    *timestamp_out = static_cast<uint64_t>(nanoseconds);
    return true;
}

bool durationToNanoseconds(int64_t duration, AVRational time_base,
                           uint64_t* duration_out) {
    return timestampToNanoseconds(duration, time_base, duration_out);
}

} // namespace

struct FfmpegVideoDecoder::Impl {
    int descriptor = -1;
    uint64_t source_bytes = 0u;
    uint64_t cumulative_read_bytes = 0u;
    AVFormatContext* format = nullptr;
    AVIOContext* io = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* scaler = nullptr;
    uint8_t* pixels = nullptr;
    size_t pixel_capacity = 0u;
    int stream_index = -1;
    AVRational time_base{0, 1};
    RinVideoInfoV1 information{};
    uint64_t frame_index = 0u;
    bool packet_pending = false;
    bool drain_requested = false;
    bool flush_sent = false;
    bool end_of_stream = false;
    RJpegCancellationFunction cancellation = nullptr;
    void* cancellation_context = nullptr;
    bool cancelled = false;

    bool pollCancellation() {
        if (!cancellation || cancellation(cancellation_context) == 0)
            return false;
        cancelled = true;
        return true;
    }

    static int interrupt(void* opaque) {
        Impl* self = static_cast<Impl*>(opaque);
        return !self || self->pollCancellation() ? 1 : 0;
    }

    static int readPacket(void* opaque, uint8_t* buffer, int size) {
        Impl* self = static_cast<Impl*>(opaque);
        ssize_t count;
        if (!self || self->descriptor < 0 || !buffer || size <= 0)
            return AVERROR(EINVAL);
        if (self->pollCancellation()) return AVERROR_EXIT;
        do {
            count = ::read(self->descriptor, buffer, static_cast<size_t>(size));
        } while (count < 0 && errno == EINTR && !self->pollCancellation());
        if (self->pollCancellation()) return AVERROR_EXIT;
        if (count == 0) return AVERROR_EOF;
        if (count < 0) return AVERROR(EIO);
        const uint64_t bytes = static_cast<uint64_t>(count);
        if (bytes > kMaxCumulativeReadBytes ||
            self->cumulative_read_bytes > kMaxCumulativeReadBytes - bytes)
            return AVERROR(EFBIG);
        self->cumulative_read_bytes += bytes;
        return count > INT_MAX ? AVERROR(EOVERFLOW) : static_cast<int>(count);
    }

    static int64_t seekPacket(void* opaque, int64_t offset, int whence) {
        Impl* self = static_cast<Impl*>(opaque);
        int64_t base;
        int64_t target;
        if (!self || self->descriptor < 0) return AVERROR(EINVAL);
        if (self->pollCancellation()) return AVERROR_EXIT;
        if (whence == AVSEEK_SIZE) {
            return self->source_bytes > static_cast<uint64_t>(INT64_MAX)
                ? AVERROR(EFBIG) : static_cast<int64_t>(self->source_bytes);
        }
        whence &= ~AVSEEK_FORCE;
        if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
            return AVERROR(EINVAL);
        if (whence == SEEK_SET) {
            base = 0;
        } else if (whence == SEEK_END) {
            base = static_cast<int64_t>(self->source_bytes);
        } else {
            const off_t current = ::lseek(self->descriptor, 0, SEEK_CUR);
            if (current < 0 ||
                static_cast<uint64_t>(current) > self->source_bytes)
                return AVERROR(EIO);
            base = static_cast<int64_t>(current);
        }
        const int64_t end = static_cast<int64_t>(self->source_bytes);
        if (offset < -base || offset > end - base)
            return AVERROR(EFBIG);
        target = base + offset;
        if (::lseek(self->descriptor, static_cast<off_t>(target), SEEK_SET) < 0)
            return AVERROR(EIO);
        return target;
    }

    void clear() {
        if (frame) av_frame_free(&frame);
        if (packet) av_packet_free(&packet);
        if (scaler) {
            sws_freeContext(scaler);
            scaler = nullptr;
        }
        if (codec) avcodec_free_context(&codec);
        if (format) avformat_close_input(&format);
        if (io) avio_context_free(&io);
        if (descriptor >= 0) {
            (void)closeDescriptor(descriptor);
            descriptor = -1;
        }
        av_free(pixels);
        pixels = nullptr;
        pixel_capacity = 0u;
        source_bytes = 0u;
        cumulative_read_bytes = 0u;
        stream_index = -1;
        time_base = AVRational{0, 1};
        memset(&information, 0, sizeof(information));
        frame_index = 0u;
        packet_pending = false;
        drain_requested = false;
        flush_sent = false;
        end_of_stream = false;
        cancellation = nullptr;
        cancellation_context = nullptr;
        cancelled = false;
    }

    bool openDescriptor(int owned_descriptor) {
        struct stat status = {};
        AVCodecParameters* parameters;
        const AVCodec* decoder = nullptr;
        uint8_t* io_buffer = nullptr;
        int result;
        clear();
        if (owned_descriptor < 0) return false;
        descriptor = owned_descriptor;
        if (fstat(descriptor, &status) != 0 ||
            !S_ISREG(status.st_mode) || status.st_size <= 0 ||
            static_cast<uint64_t>(status.st_size) > kMaxSourceBytes ||
            static_cast<uint64_t>(status.st_size) >
                static_cast<uint64_t>(INT64_MAX)) {
            clear();
            return false;
        }
        source_bytes = static_cast<uint64_t>(status.st_size);
        if (::lseek(descriptor, 0, SEEK_SET) < 0) {
            clear();
            return false;
        }

        format = avformat_alloc_context();
        io_buffer = static_cast<uint8_t*>(av_malloc(kIoBufferBytes));
        if (!format || !io_buffer) {
            if (io_buffer) av_free(io_buffer);
            clear();
            return false;
        }
        io = avio_alloc_context(io_buffer, kIoBufferBytes, 0, this,
                                readPacket, nullptr, seekPacket);
        if (!io) {
            av_free(io_buffer);
            clear();
            return false;
        }
        format->pb = io;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
        format->interrupt_callback.callback = interrupt;
        format->interrupt_callback.opaque = this;
        format->probesize = kMaxProbeBytes;
        format->format_probesize = static_cast<int>(kMaxProbeBytes);
        format->max_analyze_duration = kMaxAnalyzeDurationUs;
        format->max_streams = kMaxStreams;
        format->max_probe_packets = kMaxProbePackets;
        format->max_index_size = kMaxIndexBytes;
        result = avformat_open_input(&format, nullptr, nullptr, nullptr);
        if (result < 0 || cancelled || !format) {
            clear();
            return false;
        }
        result = avformat_find_stream_info(format, nullptr);
        if (result < 0 || cancelled || format->nb_streams == 0u ||
            format->nb_streams > static_cast<unsigned int>(kMaxStreams)) {
            clear();
            return false;
        }
        stream_index = -1;
        for (unsigned int index = 0u; index < format->nb_streams; ++index) {
            AVStream* candidate = format->streams[index];
            if (!candidate || !candidate->codecpar ||
                candidate->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
                (candidate->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0)
                continue;
            const AVCodec* candidate_decoder = avcodec_find_decoder(
                candidate->codecpar->codec_id);
            if (!candidate_decoder) continue;
            if (stream_index < 0 ||
                (candidate->disposition & AV_DISPOSITION_DEFAULT) != 0) {
                stream_index = static_cast<int>(index);
                decoder = candidate_decoder;
            }
            if ((candidate->disposition & AV_DISPOSITION_DEFAULT) != 0)
                break;
        }
        if (stream_index < 0 || !decoder ||
            static_cast<unsigned int>(stream_index) >= format->nb_streams) {
            clear();
            return false;
        }
        AVStream* stream = format->streams[stream_index];
        parameters = stream ? stream->codecpar : nullptr;
        uint32_t width = 0u;
        uint32_t height = 0u;
        uint32_t stride = 0u;
        uint64_t frame_bytes = 0u;
        if (!stream || stream->time_base.num <= 0 ||
            stream->time_base.den <= 0 || !parameters ||
            parameters->width <= 0 || parameters->height <= 0 ||
            parameters->extradata_size < 0 ||
            parameters->extradata_size > kMaxExtradataBytes ||
            (parameters->extradata_size != 0 && !parameters->extradata) ||
            !validFrameGeometry(static_cast<uint32_t>(parameters->width),
                                static_cast<uint32_t>(parameters->height),
                                &stride, &frame_bytes)) {
            clear();
            return false;
        }
        width = static_cast<uint32_t>(parameters->width);
        height = static_cast<uint32_t>(parameters->height);
        codec = avcodec_alloc_context3(decoder);
        if (!codec) {
            clear();
            return false;
        }
        result = avcodec_parameters_to_context(codec, parameters);
        if (result < 0) {
            clear();
            return false;
        }
        codec->thread_count = 1;
        codec->thread_type = 0;
        codec->max_pixels = static_cast<int64_t>(kMaxPixels);
        codec->pkt_timebase = stream->time_base;
        if (avcodec_open2(codec, decoder, nullptr) < 0 || cancelled ||
            codec->width != static_cast<int>(width) ||
            codec->height != static_cast<int>(height)) {
            clear();
            return false;
        }
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        pixels = static_cast<uint8_t*>(av_malloc(static_cast<size_t>(frame_bytes)));
        if (!packet || !frame || !pixels) {
            clear();
            return false;
        }
        pixel_capacity = static_cast<size_t>(frame_bytes);
        time_base = stream->time_base;
        memset(&information, 0, sizeof(information));
        information.struct_size = sizeof(information);
        information.version = RIN_SDK_STRUCT_VERSION_1;
        information.width = width;
        information.height = height;
        information.format = RIN_IMAGE_DECODE_FORMAT_ARGB8888;
        information.row_stride_bytes = stride;
        if (stream->nb_frames > 0 &&
            static_cast<uint64_t>(stream->nb_frames) <= UINT32_MAX) {
            information.frame_count = static_cast<uint32_t>(stream->nb_frames);
            information.flags |= RIN_VIDEO_INFO_FRAME_COUNT_KNOWN;
        }
        if (rationalToU32(stream->avg_frame_rate,
                          &information.fps_numerator,
                          &information.fps_denominator)) {
            information.flags |= RIN_VIDEO_INFO_FPS_KNOWN;
        }
        uint64_t duration = 0u;
        if (stream->duration > 0 &&
            durationToNanoseconds(stream->duration, stream->time_base,
                                  &duration)) {
            information.duration_ns = duration;
            information.flags |= RIN_VIDEO_INFO_DURATION_KNOWN;
        } else if (format->duration > 0 &&
                   durationToNanoseconds(format->duration, AV_TIME_BASE_Q,
                                         &duration)) {
            information.duration_ns = duration;
            information.flags |= RIN_VIDEO_INFO_DURATION_KNOWN;
        }
        return true;
    }

    bool emitEndOfStream(RinVideoFrameInfoV1* output) {
        RinVideoFrameInfoV1 result = {};
        result.struct_size = sizeof(result);
        result.version = RIN_SDK_STRUCT_VERSION_1;
        result.width = information.width;
        result.height = information.height;
        result.format = information.format;
        result.row_stride_bytes = information.row_stride_bytes;
        result.flags = RIN_VIDEO_FRAME_FLAG_END_OF_STREAM;
        result.frame_index = frame_index;
        if ((information.flags & RIN_VIDEO_INFO_DURATION_KNOWN) != 0u) {
            result.timestamp_ns = information.duration_ns;
            result.flags |= RIN_VIDEO_FRAME_FLAG_TIMESTAMP_KNOWN;
        }
        *output = result;
        end_of_stream = true;
        return true;
    }

    bool decodeNextFrame(uint32_t max_width, uint32_t max_height,
                         uint32_t max_frame_bytes,
                         RinVideoFrameInfoV1* info_out,
                         const uint8_t** output_out,
                         RJpegCancellationFunction cancel,
                         void* cancel_context) {
        uint32_t stride = 0u;
        uint64_t frame_bytes = 0u;
        if (output_out) *output_out = nullptr;
        if (!info_out || !output_out || !codec || !format || !packet ||
            !frame || !pixels || max_width < information.width ||
            max_height < information.height ||
            !validFrameGeometry(information.width, information.height,
                                &stride, &frame_bytes) ||
            frame_bytes > max_frame_bytes || frame_bytes > pixel_capacity)
            return false;
        cancellation = cancel;
        cancellation_context = cancel_context;
        if (pollCancellation()) return false;
        if (end_of_stream) return emitEndOfStream(info_out);

        for (;;) {
            if (pollCancellation()) return false;
            int result = avcodec_receive_frame(codec, frame);
            if (result == 0) {
                if (frame->width != static_cast<int>(information.width) ||
                    frame->height != static_cast<int>(information.height) ||
                    !validFrameGeometry(static_cast<uint32_t>(frame->width),
                                        static_cast<uint32_t>(frame->height),
                                        &stride, &frame_bytes) ||
                    frame_bytes > max_frame_bytes || frame_bytes > pixel_capacity) {
                    av_frame_unref(frame);
                    return false;
                }
                scaler = sws_getCachedContext(
                    scaler, frame->width, frame->height,
                    static_cast<AVPixelFormat>(frame->format), frame->width,
                    frame->height, AV_PIX_FMT_ARGB, SWS_BILINEAR, nullptr,
                    nullptr, nullptr);
                if (!scaler) {
                    av_frame_unref(frame);
                    return false;
                }
                uint8_t* destination_data[4] = {pixels, nullptr, nullptr, nullptr};
                int destination_stride[4] = {
                    static_cast<int>(information.row_stride_bytes), 0, 0, 0};
                const int rows = sws_scale(
                    scaler, frame->data, frame->linesize, 0, frame->height,
                    destination_data, destination_stride);
                if (rows != frame->height || pollCancellation()) {
                    av_frame_unref(frame);
                    return false;
                }
                RinVideoFrameInfoV1 decoded = {};
                decoded.struct_size = sizeof(decoded);
                decoded.version = RIN_SDK_STRUCT_VERSION_1;
                decoded.width = information.width;
                decoded.height = information.height;
                decoded.format = information.format;
                decoded.row_stride_bytes = information.row_stride_bytes;
                decoded.frame_index = frame_index;
                if (timestampToNanoseconds(frame->best_effort_timestamp,
                                           time_base,
                                           &decoded.timestamp_ns))
                    decoded.flags |= RIN_VIDEO_FRAME_FLAG_TIMESTAMP_KNOWN;
                decoded.payload_size_bytes = frame_bytes;
                av_frame_unref(frame);
                if (frame_index == UINT64_MAX) return false;
                ++frame_index;
                *output_out = pixels;
                *info_out = decoded;
                return true;
            }
            if (result == AVERROR_EOF) return emitEndOfStream(info_out);
            if (result != AVERROR(EAGAIN)) return false;

            if (packet_pending) {
                result = avcodec_send_packet(codec, packet);
                if (result == AVERROR(EAGAIN)) return false;
                av_packet_unref(packet);
                packet_pending = false;
                if (result < 0) return false;
                continue;
            }
            if (drain_requested) {
                if (flush_sent) return emitEndOfStream(info_out);
                result = avcodec_send_packet(codec, nullptr);
                if (result == AVERROR(EAGAIN)) return false;
                if (result < 0 && result != AVERROR_EOF) return false;
                flush_sent = true;
                continue;
            }
            for (;;) {
                if (pollCancellation()) return false;
                result = av_read_frame(format, packet);
                if (cancelled) return false;
                if (result == AVERROR_EOF) {
                    drain_requested = true;
                    break;
                }
                if (result < 0) return false;
                if (packet->size < 0 || packet->size > kMaxPacketBytes) {
                    av_packet_unref(packet);
                    return false;
                }
                if (packet->stream_index == stream_index) {
                    packet_pending = true;
                    break;
                }
                av_packet_unref(packet);
            }
        }
    }
};

FfmpegVideoDecoder::FfmpegVideoDecoder() : implementation_(nullptr) {
    void* memory = malloc(sizeof(Impl));
    if (memory) implementation_ = new (memory) Impl();
}

FfmpegVideoDecoder::~FfmpegVideoDecoder() {
    if (!implementation_) return;
    close();
    implementation_->~Impl();
    free(implementation_);
    implementation_ = nullptr;
}

bool FfmpegVideoDecoder::openDescriptor(int descriptor) {
    if (!implementation_) {
        if (descriptor >= 0) (void)closeDescriptor(descriptor);
        return false;
    }
    return implementation_->openDescriptor(descriptor);
}

void FfmpegVideoDecoder::close() {
    if (implementation_) implementation_->clear();
}

const RinVideoInfoV1& FfmpegVideoDecoder::info() const {
    static const RinVideoInfoV1 empty = {};
    return implementation_ ? implementation_->information : empty;
}

bool FfmpegVideoDecoder::decodeNextFrame(
    uint32_t max_width, uint32_t max_height, uint32_t max_frame_bytes,
    RinVideoFrameInfoV1* info_out, const uint8_t** output_out,
    RJpegCancellationFunction cancellation, void* cancellation_context) {
    if (!implementation_) {
        if (info_out) memset(info_out, 0, sizeof(*info_out));
        if (output_out) *output_out = nullptr;
        return false;
    }
    if (info_out) memset(info_out, 0, sizeof(*info_out));
    return implementation_->decodeNextFrame(
        max_width, max_height, max_frame_bytes, info_out, output_out,
        cancellation, cancellation_context);
}

} // namespace RinMedia
