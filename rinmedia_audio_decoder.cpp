/* SPDX-License-Identifier: MIT */
#include "rinmedia_audio_decoder.hpp"

/* FFmpeg's public headers intentionally leave the C-linkage boundary to the
 * caller.  Preload RinOS' C++-aware libc headers before opening that boundary;
 * otherwise FFmpeg's common.h includes <limits.h> while extern "C" is active
 * and the C++ numeric_limits templates are rejected by the compiler. */
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
}

namespace RinMedia {

namespace {

constexpr int64_t kMaxProbeBytes = 4 * 1024 * 1024;
constexpr int64_t kMaxAnalyzeDurationUs = 10 * AV_TIME_BASE;
constexpr uint64_t kMaxInputBytes = 1024u * 1024u * 1024u;
constexpr int kMaxStreams = 32;
constexpr int kMaxFrameSamples = 65536;
constexpr size_t kMaxPendingSamples = static_cast<size_t>(kMaxFrameSamples) *
                                      kAudioOutputMaxChannels;
constexpr size_t kMaxMetadataBytes = 4096u;
constexpr int kMaxPacketBytes = 16 * 1024 * 1024;
constexpr int kMaxChannels = 32;
constexpr int kMaxSampleRate = 384000;
constexpr int kMaxAlbumArtBytes = 4 * 1024 * 1024;

}

struct AudioDecoder::Impl {
    int file = -1;
    AVFormatContext* format = nullptr;
    AVIOContext* io = nullptr;
    AVCodecContext* codec = nullptr;
    SwrContext* resampler = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    int streamIndex = -1;
    bool draining = false;
    uint64_t bytesRead = 0;
    Sample* pending = nullptr;
    size_t pendingSamples = 0;
    size_t pendingCapacity = 0;
    size_t pendingOffset = 0;
    AudioOutputFormat output;
    AudioMetadata information;
    std::string error;

    bool ensurePendingCapacity(size_t samples) {
        if (samples == 0 || samples > kMaxPendingSamples) return false;
        if (samples <= pendingCapacity) return true;
        if (samples > SIZE_MAX / sizeof(Sample)) return false;
        void* replacement = av_realloc(pending, samples * sizeof(Sample));
        if (!replacement) return false;
        pending = static_cast<Sample*>(replacement);
        pendingCapacity = samples;
        return true;
    }

    void clearPending() {
        pendingSamples = 0;
        pendingOffset = 0;
    }

    static int readPacket(void* opaque, uint8_t* buffer, int size) {
        Impl* self = static_cast<Impl*>(opaque);
        ssize_t count;
        if (!self || self->file < 0 || !buffer || size <= 0)
            return AVERROR(EINVAL);
        do {
            count = read(self->file, buffer, static_cast<size_t>(size));
        } while (count < 0 && errno == EINTR);
        if (count == 0) return AVERROR_EOF;
        if (count > 0) {
            uint64_t readSize = static_cast<uint64_t>(count);
            if (readSize > kMaxInputBytes ||
                self->bytesRead > kMaxInputBytes - readSize) {
                return AVERROR(EFBIG);
            }
            self->bytesRead += readSize;
        }
        return count < 0 ? AVERROR(EIO) : count;
    }

    static int64_t seekPacket(void* opaque, int64_t offset, int whence) {
        Impl* self = static_cast<Impl*>(opaque);
        if (!self || self->file < 0) return AVERROR(EINVAL);
        if (whence == AVSEEK_SIZE) {
            off_t current = lseek(self->file, 0, SEEK_CUR);
            off_t end = current < 0 ? -1 : lseek(self->file, 0, SEEK_END);
            if (current >= 0) (void)lseek(self->file, current, SEEK_SET);
            return end < 0 ? AVERROR(EIO) : static_cast<int64_t>(end);
        }
        whence &= ~AVSEEK_FORCE;
        if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
            return AVERROR(EINVAL);
        }
        off_t result = lseek(self->file, static_cast<off_t>(offset), whence);
        return result < 0 ? AVERROR(EIO) : static_cast<int64_t>(result);
    }

    void setError(const char* message, int code = 0) {
        error = message ? message : "decode error";
        if (code < 0) {
            char detail[AV_ERROR_MAX_STRING_SIZE];
            if (av_strerror(code, detail, sizeof(detail)) == 0) {
                error += ": ";
                error += detail;
            }
        }
    }

    static bool metadataValue(AVDictionary* dictionary, const char* key,
                              std::string& value) {
        value.clear();
        AVDictionaryEntry* entry = av_dict_get(dictionary, key, nullptr, 0);
        if (!entry || !entry->value) return true;
        size_t length = 0;
        while (length <= kMaxMetadataBytes && entry->value[length] != '\0') ++length;
        if (length > kMaxMetadataBytes) return false;
        value.assign(entry->value, length);
        return true;
    }

    bool initializeResampler(const AudioOutputFormat& requested) {
        AVChannelLayout outputLayout = {};
        SwrContext* replacement = nullptr;
        int result;
        if (!codec || !audioOutputFormatValid(requested)) return false;
        av_channel_layout_default(&outputLayout, static_cast<int>(requested.channels));
        result = swr_alloc_set_opts2(&replacement,
                                     &outputLayout, AV_SAMPLE_FMT_S16,
                                     static_cast<int>(requested.sampleRate),
                                     &codec->ch_layout, codec->sample_fmt,
                                     codec->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&outputLayout);
        if (result < 0 || !replacement || (result = swr_init(replacement)) < 0) {
            if (replacement) swr_free(&replacement);
            setError("音声変換を初期化できません", result);
            return false;
        }
        if (resampler) swr_free(&resampler);
        resampler = replacement;
        output = requested;
        clearPending();
        return true;
    }

    int convertFrame() {
        int inputRate = codec->sample_rate > 0 ? codec->sample_rate : 48000;
        if (frame->nb_samples <= 0 || frame->nb_samples > kMaxFrameSamples) {
            setError("音声フレームのサイズが上限を超えています");
            return -1;
        }
        int64_t delay = swr_get_delay(resampler, inputRate);
        if (delay < 0) {
            setError("音声変換の遅延値が不正です");
            return -1;
        }
        if (delay > INT64_MAX - static_cast<int64_t>(frame->nb_samples)) {
            setError("音声変換の遅延値が大きすぎます");
            return -1;
        }
        int64_t scaledCapacity = av_rescale_rnd(
            delay + frame->nb_samples,
            static_cast<int>(output.sampleRate), inputRate, AV_ROUND_UP);
        if (scaledCapacity <= 0 || scaledCapacity > kMaxFrameSamples) {
            setError("音声フレームの出力サイズが上限を超えています");
            return -1;
        }
        int outputCapacity = static_cast<int>(scaledCapacity);
        if (static_cast<size_t>(outputCapacity) >
            kMaxPendingSamples / static_cast<size_t>(output.channels)) {
            setError("音声フレームの出力サイズが上限を超えています");
            return -1;
        }
        size_t sampleCapacity = static_cast<size_t>(outputCapacity) *
                                static_cast<size_t>(output.channels);
        if (!ensurePendingCapacity(sampleCapacity)) {
            setError("音声バッファー用メモリが不足しています");
            return -1;
        }
        uint8_t* outputPlanes[] = {reinterpret_cast<uint8_t*>(pending)};
        int frames = swr_convert(resampler, outputPlanes, outputCapacity,
                                 frame->extended_data,
                                 frame->nb_samples);
        if (frames < 0) {
            setError("音声の変換に失敗しました", frames);
            clearPending();
            return -1;
        }
        if (frames < 0 || frames > outputCapacity) {
            setError("音声変換の出力サイズが不正です");
            clearPending();
            return -1;
        }
        pendingSamples = static_cast<size_t>(frames) *
                         static_cast<size_t>(output.channels);
        pendingOffset = 0;
        return frames;
    }

    int decodeNextFrame() {
        for (;;) {
            int result = avcodec_receive_frame(codec, frame);
            if (result == 0) return convertFrame();
            if (result == AVERROR_EOF) return 0;
            if (result != AVERROR(EAGAIN)) {
                setError("音声フレームをデコードできません", result);
                return -1;
            }

            bool submitted = false;
            while (!submitted) {
                result = av_read_frame(format, packet);
                if (result < 0) {
                    if (!draining) {
                        draining = true;
                        result = avcodec_send_packet(codec, nullptr);
                        if (result < 0 && result != AVERROR_EOF) {
                            setError("デコーダーを完了できません", result);
                            return -1;
                        }
                    }
                    submitted = true;
                    break;
                }
                if (packet->stream_index == streamIndex) {
                    if (packet->size < 0 || packet->size > kMaxPacketBytes) {
                        av_packet_unref(packet);
                        setError("音声パケットのサイズが上限を超えています");
                        return -1;
                    }
                    result = avcodec_send_packet(codec, packet);
                    av_packet_unref(packet);
                    if (result < 0 && result != AVERROR(EAGAIN)) {
                        setError("音声パケットをデコードできません", result);
                        return -1;
                    }
                    submitted = true;
                } else {
                    av_packet_unref(packet);
                }
            }
        }
    }
};

AudioDecoder::AudioDecoder() : implementation(nullptr) {
    void* memory = rin_malloc(sizeof(Impl));
    if (memory) implementation = new (memory) Impl();
}
AudioDecoder::~AudioDecoder() {
    if (!implementation) return;
    close();
    implementation->~Impl();
    rin_free(implementation);
    implementation = nullptr;
}

bool AudioDecoder::openDescriptor(int descriptor) {
    return openDescriptor(descriptor, AudioOutputFormat());
}

bool AudioDecoder::openDescriptor(int descriptor, const AudioOutputFormat& output) {
    close();
    if (!implementation) return false;
    Impl& state = *implementation;
    state.error.clear();
    if (descriptor < 0 || !audioOutputFormatValid(output)) {
        if (descriptor >= 0) (void)::close(descriptor);
        if (!audioOutputFormatValid(output))
            state.setError("出力音声形式が未対応です");
        else
            state.setError("ファイルを開けません");
        return false;
    }
    state.file = descriptor;
    state.bytesRead = 0;

    state.format = avformat_alloc_context();
    uint8_t* ioBuffer = static_cast<uint8_t*>(av_malloc(32768));
    if (!state.format || !ioBuffer) {
        if (ioBuffer) av_free(ioBuffer);
        state.setError("デコーダー用メモリが不足しています");
        close();
        return false;
    }
    state.io = avio_alloc_context(ioBuffer, 32768, 0, &state,
                                  Impl::readPacket, nullptr, Impl::seekPacket);
    if (!state.io) {
        av_free(ioBuffer);
        state.setError("ファイル読み込みを初期化できません");
        close();
        return false;
    }
    state.format->pb = state.io;
    state.format->flags |= AVFMT_FLAG_CUSTOM_IO;
    /* FFmpeg otherwise lets container headers choose probe/analyse sizes and
     * stream-table growth.  Keep hostile media on a bounded admission path. */
    state.format->probesize = kMaxProbeBytes;
    state.format->format_probesize = static_cast<int>(kMaxProbeBytes);
    state.format->max_analyze_duration = kMaxAnalyzeDurationUs;
    state.format->max_streams = kMaxStreams;
    int result = avformat_open_input(&state.format, nullptr, nullptr, nullptr);
    if (result < 0) {
        state.setError("未対応または破損した音声ファイルです", result);
        close();
        return false;
    }
    result = avformat_find_stream_info(state.format, nullptr);
    if (result < 0) {
        state.setError("音声情報を読み取れません", result);
        close();
        return false;
    }
    const AVCodec* decoder = nullptr;
    state.streamIndex = av_find_best_stream(state.format, AVMEDIA_TYPE_AUDIO,
                                            -1, -1, &decoder, 0);
    if (state.streamIndex < 0 || !decoder) {
        state.setError("音声ストリームがありません", state.streamIndex);
        close();
        return false;
    }
    for (unsigned int index = 0; index < state.format->nb_streams; ++index) {
        AVStream* stream = state.format->streams[index];
        if (!stream) continue;
        if ((stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0 &&
            stream->attached_pic.size > kMaxAlbumArtBytes) {
            state.setError("アルバムアートのサイズが上限を超えています");
            close();
            return false;
        }
    }
    state.codec = avcodec_alloc_context3(decoder);
    if (!state.codec) {
        state.setError("デコーダー用メモリが不足しています");
        close();
        return false;
    }
    result = avcodec_parameters_to_context(
        state.codec, state.format->streams[state.streamIndex]->codecpar);
    if (result < 0 || (result = avcodec_open2(state.codec, decoder, nullptr)) < 0) {
        state.setError("音声デコーダーを開始できません", result);
        close();
        return false;
    }
    if (state.codec->sample_rate <= 0 || state.codec->sample_rate > kMaxSampleRate ||
        state.codec->ch_layout.nb_channels < 0 ||
        state.codec->ch_layout.nb_channels > kMaxChannels) {
        state.setError("音声形式のチャンネルまたはサンプルレートが上限を超えています");
        close();
        return false;
    }
    if (state.codec->ch_layout.nb_channels == 0) {
        av_channel_layout_default(&state.codec->ch_layout, 2);
    }
    if (!state.initializeResampler(output)) {
        close();
        return false;
    }
    state.packet = av_packet_alloc();
    state.frame = av_frame_alloc();
    if (!state.packet || !state.frame) {
        state.setError("デコーダー用メモリが不足しています");
        close();
        return false;
    }
    AVDictionary* streamMetadata = state.format->streams[state.streamIndex]->metadata;
    std::string formatTitle;
    std::string streamTitle;
    std::string formatArtist;
    std::string streamArtist;
    std::string formatAlbum;
    std::string streamAlbum;
    if (!Impl::metadataValue(state.format->metadata, "title", formatTitle) ||
        !Impl::metadataValue(streamMetadata, "title", streamTitle) ||
        !Impl::metadataValue(state.format->metadata, "artist", formatArtist) ||
        !Impl::metadataValue(streamMetadata, "artist", streamArtist) ||
        !Impl::metadataValue(state.format->metadata, "album", formatAlbum) ||
        !Impl::metadataValue(streamMetadata, "album", streamAlbum)) {
        state.setError("音声メタデータのサイズが上限を超えています");
        close();
        return false;
    }
    state.information.title = formatTitle.empty() ? streamTitle : formatTitle;
    state.information.artist = formatArtist.empty() ? streamArtist : formatArtist;
    state.information.album = formatAlbum.empty() ? streamAlbum : formatAlbum;
    if (state.format->duration > 0) {
        state.information.durationMs = av_rescale(state.format->duration, 1000, AV_TIME_BASE);
    } else {
        AVStream* stream = state.format->streams[state.streamIndex];
        if (stream->duration > 0)
            state.information.durationMs = av_rescale_q(
                stream->duration, stream->time_base, AVRational{1, 1000});
    }
    return true;
}

int AudioDecoder::readFrames(Sample* destination, int maximumFrames) {
    if (!implementation || !implementation->codec || !destination ||
        maximumFrames <= 0 || maximumFrames > kMaxFrameSamples) return -1;
    Impl& state = *implementation;
    int produced = 0;
    while (produced < maximumFrames) {
        if (state.pendingOffset < state.pendingSamples) {
            size_t pendingFrames = (state.pendingSamples - state.pendingOffset) /
                                   static_cast<size_t>(state.output.channels);
            int copyFrames = (int)pendingFrames;
            if (copyFrames > maximumFrames - produced) copyFrames = maximumFrames - produced;
            memcpy(destination + static_cast<size_t>(produced) * state.output.channels,
                   state.pending + state.pendingOffset,
                   static_cast<size_t>(copyFrames) * state.output.channels * sizeof(Sample));
            produced += copyFrames;
            state.pendingOffset += static_cast<size_t>(copyFrames) * state.output.channels;
            if (state.pendingOffset == state.pendingSamples) state.clearPending();
            continue;
        }
        int result = state.decodeNextFrame();
        if (result < 0) return produced ? produced : -1;
        if (result == 0) break;
    }
    return produced;
}

bool AudioDecoder::setOutputFormat(const AudioOutputFormat& output) {
    if (!implementation || !implementation->codec || !audioOutputFormatValid(output))
        return false;
    Impl& state = *implementation;
    if (state.output.sampleRate == output.sampleRate &&
        state.output.channels == output.channels)
        return true;
    return state.initializeResampler(output);
}

AudioOutputFormat AudioDecoder::outputFormat() const {
    return implementation ? implementation->output : AudioOutputFormat();
}

bool AudioDecoder::seek(Milliseconds positionMs) {
    if (!implementation || !implementation->format || positionMs < 0) return false;
    Impl& state = *implementation;
    AVStream* stream = state.format->streams[state.streamIndex];
    int64_t timestamp = av_rescale_q(positionMs, AVRational{1, 1000}, stream->time_base);
    int result = av_seek_frame(state.format, state.streamIndex, timestamp, AVSEEK_FLAG_BACKWARD);
    if (result < 0) {
        state.setError("指定位置へ移動できません", result);
        return false;
    }
    avcodec_flush_buffers(state.codec);
    if (!state.initializeResampler(state.output)) {
        state.setError("音声変換を再開できません");
        return false;
    }
    state.draining = false;
    state.clearPending();
    return true;
}

void AudioDecoder::close() {
    if (!implementation) return;
    Impl& state = *implementation;
    if (state.packet) av_packet_free(&state.packet);
    if (state.frame) av_frame_free(&state.frame);
    if (state.resampler) swr_free(&state.resampler);
    if (state.codec) avcodec_free_context(&state.codec);
    if (state.format) avformat_close_input(&state.format);
    if (state.io) avio_context_free(&state.io);
    if (state.pending) av_free(state.pending);
    state.pending = nullptr;
    state.pendingCapacity = 0;
    if (state.file >= 0) (void)::close(state.file);
    state.file = -1;
    state.bytesRead = 0;
    state.streamIndex = -1;
    state.draining = false;
    state.clearPending();
    state.information = AudioMetadata();
}

bool AudioDecoder::isOpen() const {
    return implementation && implementation->codec;
}

const AudioMetadata& AudioDecoder::metadata() const {
    static const AudioMetadata empty;
    return implementation ? implementation->information : empty;
}
const std::string& AudioDecoder::lastError() const {
    static const std::string empty;
    return implementation ? implementation->error : empty;
}

} // namespace RinMedia
