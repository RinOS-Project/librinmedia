/*
 * RinOS AVI Parser ✿
 * 軽量AVIコンテナパーサー
 * AVI 1.0 / OpenDML 対応
 */

#ifndef RINAVI_H
#define RINAVI_H

#include "../libc/stdint.h"
#include "../libc/stddef.h"

/* ═══════════════════════════════════════════════════════════════
 * 定数
 * ═══════════════════════════════════════════════════════════════*/

#define RAVI_OK              0
#define RAVI_ERROR          -1
#define RAVI_DATA_ERROR     -2
#define RAVI_UNSUPPORTED    -3
#define RAVI_END_OF_FILE    -4

#define RAVI_MAX_STREAMS    16

/* FourCC マクロ */
#define RAVI_FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* 主要なFourCC */
#define RAVI_RIFF   RAVI_FOURCC('R', 'I', 'F', 'F')
#define RAVI_AVI    RAVI_FOURCC('A', 'V', 'I', ' ')
#define RAVI_LIST   RAVI_FOURCC('L', 'I', 'S', 'T')
#define RAVI_HDRL   RAVI_FOURCC('h', 'd', 'r', 'l')
#define RAVI_AVIH   RAVI_FOURCC('a', 'v', 'i', 'h')
#define RAVI_STRL   RAVI_FOURCC('s', 't', 'r', 'l')
#define RAVI_STRH   RAVI_FOURCC('s', 't', 'r', 'h')
#define RAVI_STRF   RAVI_FOURCC('s', 't', 'r', 'f')
#define RAVI_MOVI   RAVI_FOURCC('m', 'o', 'v', 'i')
#define RAVI_IDX1   RAVI_FOURCC('i', 'd', 'x', '1')

/* ストリームタイプ */
#define RAVI_VIDS   RAVI_FOURCC('v', 'i', 'd', 's')
#define RAVI_AUDS   RAVI_FOURCC('a', 'u', 'd', 's')

/* ビデオコーデック */
#define RAVI_MJPG   RAVI_FOURCC('M', 'J', 'P', 'G')
#define RAVI_MJPEG  RAVI_FOURCC('m', 'j', 'p', 'g')
#define RAVI_JPEG   RAVI_FOURCC('J', 'P', 'E', 'G')
#define RAVI_DIB    RAVI_FOURCC('D', 'I', 'B', ' ')  /* 無圧縮RGB */
#define RAVI_RAW    0x00000000                        /* 無圧縮 */

/* オーディオフォーマット */
#define RAVI_WAVE_PCM       0x0001
#define RAVI_WAVE_ADPCM     0x0002
#define RAVI_WAVE_FLOAT     0x0003
#define RAVI_WAVE_ALAW      0x0006
#define RAVI_WAVE_MULAW     0x0007
#define RAVI_WAVE_MP3       0x0055

/* ═══════════════════════════════════════════════════════════════
 * 構造体
 * ═══════════════════════════════════════════════════════════════*/

#pragma pack(push, 1)

/* RIFFチャンクヘッダー */
typedef struct {
    uint32_t fourcc;
    uint32_t size;
} RAviChunk;

/* AVIメインヘッダー (avih) */
typedef struct {
    uint32_t micro_sec_per_frame;   /* フレーム間隔 (マイクロ秒) */
    uint32_t max_bytes_per_sec;     /* 最大データレート */
    uint32_t padding_granularity;
    uint32_t flags;
    uint32_t total_frames;          /* 総フレーム数 */
    uint32_t initial_frames;
    uint32_t streams;               /* ストリーム数 */
    uint32_t suggested_buffer_size;
    uint32_t width;
    uint32_t height;
    uint32_t reserved[4];
} RAviMainHeader;

/* ストリームヘッダー (strh) */
typedef struct {
    uint32_t type;                  /* 'vids' or 'auds' */
    uint32_t handler;               /* コーデックFourCC */
    uint32_t flags;
    uint16_t priority;
    uint16_t language;
    uint32_t initial_frames;
    uint32_t scale;                 /* サンプルレート計算用 */
    uint32_t rate;                  /* rate/scale = samples/sec */
    uint32_t start;
    uint32_t length;                /* ストリーム長 */
    uint32_t suggested_buffer_size;
    uint32_t quality;
    uint32_t sample_size;
    int16_t  frame_left;
    int16_t  frame_top;
    int16_t  frame_right;
    int16_t  frame_bottom;
} RAviStreamHeader;

/* ビデオフォーマット (strf for video) - BITMAPINFOHEADER */
typedef struct {
    uint32_t size;
    int32_t  width;
    int32_t  height;
    uint16_t planes;
    uint16_t bit_count;
    uint32_t compression;           /* コーデックFourCC */
    uint32_t size_image;
    int32_t  x_pels_per_meter;
    int32_t  y_pels_per_meter;
    uint32_t clr_used;
    uint32_t clr_important;
} RAviVideoFormat;

/* オーディオフォーマット (strf for audio) - WAVEFORMATEX */
typedef struct {
    uint16_t format_tag;            /* フォーマットタイプ */
    uint16_t channels;              /* チャンネル数 */
    uint32_t samples_per_sec;       /* サンプルレート */
    uint32_t avg_bytes_per_sec;     /* 平均バイトレート */
    uint16_t block_align;           /* ブロックアライン */
    uint16_t bits_per_sample;       /* ビット深度 */
} RAviAudioFormat;

/* インデックスエントリ (idx1) */
typedef struct {
    uint32_t chunk_id;              /* 'xxdc', 'xxwb' など */
    uint32_t flags;
    uint32_t offset;                /* moviからのオフセット */
    uint32_t size;
} RAviIndexEntry;

#pragma pack(pop)

/* ストリーム情報 */
typedef struct {
    int type;                       /* 0=video, 1=audio */
    int stream_index;

    union {
        struct {
            int width;
            int height;
            int bit_depth;
            uint32_t codec;
            float fps;
        } video;

        struct {
            int channels;
            int sample_rate;
            int bits_per_sample;
            uint16_t format;
        } audio;
    };

    uint32_t total_frames;
    uint32_t scale;
    uint32_t rate;
} RAviStream;

/* AVIコンテキスト */
typedef struct {
    const uint8_t* data;
    size_t size;
    size_t pos;

    /* ファイル情報 */
    int width;
    int height;
    float fps;
    uint32_t total_frames;
    uint32_t duration_ms;

    /* ストリーム情報 */
    int stream_count;
    int video_stream;               /* メインビデオストリームのインデックス */
    int audio_stream;               /* メインオーディオストリームのインデックス */
    RAviStream streams[RAVI_MAX_STREAMS];

    /* データ位置 */
    size_t movi_offset;             /* moviリストの開始位置 */
    size_t movi_size;
    size_t idx1_offset;             /* インデックスの開始位置 */
    size_t idx1_count;

    /* 再生状態 */
    uint32_t current_frame;
} RAviContext;

/* フレームデータ */
typedef struct {
    const uint8_t* data;
    size_t size;
    int stream_index;
    int is_keyframe;
    uint32_t frame_number;
} RAviFrame;

/* ═══════════════════════════════════════════════════════════════
 * ユーティリティ
 * ═══════════════════════════════════════════════════════════════*/

static inline int ravi_range_valid(const RAviContext* ctx, size_t offset,
                                   size_t length) {
    return ctx != 0 && ctx->data != 0 && offset <= ctx->size &&
           length <= ctx->size - offset;
}

static inline uint16_t ravi_load16(const uint8_t* data) {
    return (uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8);
}

static inline uint32_t ravi_load32(const uint8_t* data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static inline int ravi_read_chunk_header(RAviContext* ctx,
                                         uint32_t* fourcc_out,
                                         uint32_t* size_out,
                                         size_t* end_out) {
    uint32_t chunk_size;
    if (ctx == 0 || fourcc_out == 0 || size_out == 0 || end_out == 0 ||
        !ravi_range_valid(ctx, ctx->pos, sizeof(RAviChunk))) return 0;
    *fourcc_out = ravi_load32(ctx->data + ctx->pos);
    chunk_size = ravi_load32(ctx->data + ctx->pos + 4u);
    ctx->pos += sizeof(RAviChunk);
    if (!ravi_range_valid(ctx, ctx->pos, (size_t)chunk_size)) return 0;
    *size_out = chunk_size;
    *end_out = ctx->pos + (size_t)chunk_size;
    return 1;
}

static inline int ravi_finish_chunk(RAviContext* ctx, size_t chunk_end,
                                    size_t parent_end, uint32_t chunk_size) {
    if (ctx == 0 || chunk_end > parent_end ||
        !ravi_range_valid(ctx, chunk_end, 0u)) return 0;
    if ((chunk_size & 1u) != 0u) {
        if (chunk_end >= parent_end || !ravi_range_valid(ctx, chunk_end, 1u))
            return 0;
        ++chunk_end;
    }
    ctx->pos = chunk_end;
    return 1;
}

static inline uint32_t ravi_read32(RAviContext* ctx) {
    uint32_t val;
    if (!ravi_range_valid(ctx, ctx->pos, 4u)) return 0;
    val = ravi_load32(ctx->data + ctx->pos);
    ctx->pos += 4;
    return val;
}

static inline uint16_t ravi_read16(RAviContext* ctx) {
    uint16_t val;
    if (!ravi_range_valid(ctx, ctx->pos, 2u)) return 0;
    val = ravi_load16(ctx->data + ctx->pos);
    ctx->pos += 2;
    return val;
}

static inline void ravi_skip(RAviContext* ctx, size_t n) {
    if (ctx == 0 || !ravi_range_valid(ctx, ctx->pos, n)) {
        if (ctx != 0) ctx->pos = ctx->size;
        return;
    }
    ctx->pos += n;
}

static inline int ravi_is_video_chunk(uint32_t fourcc) {
    /* xxdc (compressed) or xxdb (uncompressed) */
    uint8_t c2 = (fourcc >> 16) & 0xFF;
    uint8_t c3 = (fourcc >> 24) & 0xFF;
    return (c2 == 'd' && (c3 == 'c' || c3 == 'b'));
}

static inline int ravi_is_audio_chunk(uint32_t fourcc) {
    /* xxwb (audio) */
    uint8_t c2 = (fourcc >> 16) & 0xFF;
    uint8_t c3 = (fourcc >> 24) & 0xFF;
    return (c2 == 'w' && c3 == 'b');
}

static inline int ravi_get_stream_index(uint32_t fourcc) {
    uint8_t c0 = fourcc & 0xFF;
    uint8_t c1 = (fourcc >> 8) & 0xFF;
    if (c0 >= '0' && c0 <= '9' && c1 >= '0' && c1 <= '9') {
        return (c0 - '0') * 10 + (c1 - '0');
    }
    return -1;
}

static inline uint32_t ravi_frame_time_ms(uint32_t frame, float fps)
{
    double value;

    if (!(fps > 0.0f)) return 0u;
    value = (double)frame * 1000.0 / (double)fps;
    if (!(value > 0.0)) return 0u;
    if (value >= (double)UINT32_MAX) return UINT32_MAX;
    return (uint32_t)value;
}

static inline uint32_t ravi_frame_for_time(uint32_t time_ms, float fps)
{
    double value;

    if (!(fps > 0.0f)) return 0u;
    value = (double)time_ms * (double)fps / 1000.0;
    if (!(value > 0.0)) return 0u;
    if (value >= (double)UINT32_MAX) return UINT32_MAX;
    return (uint32_t)value;
}

/* ═══════════════════════════════════════════════════════════════
 * 内部パース関数
 * ═══════════════════════════════════════════════════════════════*/

static inline int ravi_parse_stream_header(RAviContext* ctx, size_t end_pos) {
    if (ctx == 0 || end_pos > ctx->size || ctx->pos > end_pos)
        return RAVI_DATA_ERROR;
    if (ctx->stream_count >= RAVI_MAX_STREAMS) return RAVI_ERROR;

    RAviStream* stream = &ctx->streams[ctx->stream_count];
    stream->stream_index = ctx->stream_count;

    while (ctx->pos < end_pos) {
        uint32_t fourcc;
        uint32_t chunk_size;
        size_t chunk_end;
        if (!ravi_read_chunk_header(ctx, &fourcc, &chunk_size, &chunk_end) ||
            chunk_end > end_pos) return RAVI_DATA_ERROR;

        if (fourcc == RAVI_STRH) {
            /* ストリームヘッダー */
            RAviStreamHeader hdr;
            if (chunk_size >= sizeof(RAviStreamHeader)) {
                const uint8_t* p = ctx->data + ctx->pos;
                hdr.type = ravi_load32(p + 0u);
                hdr.handler = ravi_load32(p + 4u);
                hdr.scale = ravi_load32(p + 20u);
                hdr.rate = ravi_load32(p + 24u);
                hdr.length = ravi_load32(p + 32u);

                stream->scale = hdr.scale;
                stream->rate = hdr.rate;
                stream->total_frames = hdr.length;

                if (hdr.type == RAVI_VIDS) {
                    stream->type = 0;
                    stream->video.codec = hdr.handler;
                    if (hdr.scale > 0) {
                        stream->video.fps = (float)hdr.rate / (float)hdr.scale;
                    }
                    if (ctx->video_stream < 0) {
                        ctx->video_stream = ctx->stream_count;
                    }
                } else if (hdr.type == RAVI_AUDS) {
                    stream->type = 1;
                    if (ctx->audio_stream < 0) {
                        ctx->audio_stream = ctx->stream_count;
                    }
                }
            }
        } else if (fourcc == RAVI_STRF) {
            /* ストリームフォーマット */
            if (stream->type == 0) {
                /* ビデオフォーマット */
                if (chunk_size >= sizeof(RAviVideoFormat)) {
                    const uint8_t* p = ctx->data + ctx->pos;
                    int32_t width = (int32_t)ravi_load32(p + 4u);
                    int32_t height = (int32_t)ravi_load32(p + 8u);
                    if (height == (int32_t)0x80000000) return RAVI_DATA_ERROR;
                    stream->video.width = width;
                    stream->video.height = height > 0 ? height : -height;
                    stream->video.bit_depth = (int)ravi_load16(p + 14u);
                    if (stream->video.codec == 0) {
                        stream->video.codec = ravi_load32(p + 16u);
                    }
                }
            } else if (stream->type == 1) {
                /* オーディオフォーマット */
                if (chunk_size >= sizeof(RAviAudioFormat)) {
                    const uint8_t* p = ctx->data + ctx->pos;
                    stream->audio.format = ravi_load16(p + 0u);
                    stream->audio.channels = (int)ravi_load16(p + 2u);
                    stream->audio.sample_rate = (int)ravi_load32(p + 4u);
                    stream->audio.bits_per_sample = (int)ravi_load16(p + 14u);
                }
            }
        }

        if (!ravi_finish_chunk(ctx, chunk_end, end_pos, chunk_size))
            return RAVI_DATA_ERROR;
    }

    if (ctx->pos != end_pos) return RAVI_DATA_ERROR;
    ctx->stream_count++;
    return RAVI_OK;
}

static inline int ravi_parse_header_list(RAviContext* ctx, size_t end_pos) {
    if (ctx == 0 || end_pos > ctx->size || ctx->pos > end_pos)
        return RAVI_DATA_ERROR;
    while (ctx->pos < end_pos) {
        uint32_t fourcc;
        uint32_t chunk_size;
        size_t chunk_end;
        if (!ravi_read_chunk_header(ctx, &fourcc, &chunk_size, &chunk_end) ||
            chunk_end > end_pos) return RAVI_DATA_ERROR;

        if (fourcc == RAVI_AVIH) {
            /* メインヘッダー */
            if (chunk_size >= sizeof(RAviMainHeader)) {
                const uint8_t* p = ctx->data + ctx->pos;
                uint32_t microseconds = ravi_load32(p + 0u);
                uint32_t total_frames = ravi_load32(p + 16u);
                ctx->width = (int)ravi_load32(p + 32u);
                ctx->height = (int)ravi_load32(p + 36u);
                ctx->total_frames = total_frames;
                if (microseconds > 0u) {
                    ctx->fps = 1000000.0f / (float)microseconds;
                    {
                        uint64_t duration_ms = (uint64_t)total_frames *
                                               microseconds / 1000u;
                        ctx->duration_ms = duration_ms >= UINT32_MAX ?
                                           UINT32_MAX :
                                           (uint32_t)duration_ms;
                    }
                }
            }
        } else if (fourcc == RAVI_LIST) {
            uint32_t list_type;
            if (chunk_size < 4u) return RAVI_DATA_ERROR;
            list_type = ravi_read32(ctx);
            if (list_type == RAVI_STRL) {
                int result = ravi_parse_stream_header(ctx, chunk_end);
                if (result != RAVI_OK) return result;
            }
        }

        if (!ravi_finish_chunk(ctx, chunk_end, end_pos, chunk_size))
            return RAVI_DATA_ERROR;
    }
    return ctx->pos == end_pos ? RAVI_OK : RAVI_DATA_ERROR;
}

/* ═══════════════════════════════════════════════════════════════
 * 公開API
 * ═══════════════════════════════════════════════════════════════*/

/*
 * AVIファイルを開く
 */
static inline int ravi_open(RAviContext* ctx, const uint8_t* data, size_t size) {
    uint32_t riff;
    uint32_t file_size;
    uint32_t avi;
    size_t riff_end;
    if (!ctx) return RAVI_ERROR;

    /* 初期化 */
    for (size_t i = 0; i < sizeof(RAviContext); i++) {
        ((uint8_t*)ctx)[i] = 0;
    }
    if (!data || size < 12u) return RAVI_ERROR;
    ctx->data = data;
    ctx->size = size;
    ctx->video_stream = -1;
    ctx->audio_stream = -1;

    /* RIFFヘッダー確認 */
    riff = ravi_read32(ctx);
    file_size = ravi_read32(ctx);
    avi = ravi_read32(ctx);

    if (riff != RAVI_RIFF || avi != RAVI_AVI || file_size < 4u ||
        (size_t)file_size > size - 8u) {
        return RAVI_DATA_ERROR;
    }
    riff_end = 8u + (size_t)file_size;
    ctx->size = riff_end;

    /* チャンク解析 */
    while (ctx->pos < ctx->size) {
        uint32_t fourcc;
        uint32_t chunk_size;
        size_t chunk_end;
        if (!ravi_read_chunk_header(ctx, &fourcc, &chunk_size, &chunk_end))
            return RAVI_DATA_ERROR;

        if (fourcc == RAVI_LIST) {
            uint32_t list_type;
            if (chunk_size < 4u) return RAVI_DATA_ERROR;
            list_type = ravi_read32(ctx);

            if (list_type == RAVI_HDRL) {
                int result = ravi_parse_header_list(ctx, chunk_end);
                if (result != RAVI_OK) return result;
            } else if (list_type == RAVI_MOVI) {
                if (ctx->movi_offset != 0u) return RAVI_DATA_ERROR;
                ctx->movi_offset = ctx->pos;
                ctx->movi_size = (size_t)chunk_size - 4u;
            }
        } else if (fourcc == RAVI_IDX1) {
            if (ctx->idx1_offset != 0u ||
                chunk_size % sizeof(RAviIndexEntry) != 0u)
                return RAVI_DATA_ERROR;
            ctx->idx1_offset = ctx->pos;
            ctx->idx1_count = (size_t)chunk_size / sizeof(RAviIndexEntry);
        }

        if (!ravi_finish_chunk(ctx, chunk_end, ctx->size, chunk_size))
            return RAVI_DATA_ERROR;
    }
    if (ctx->pos != ctx->size) return RAVI_DATA_ERROR;

    /* ビデオストリームから情報を更新 */
    if (ctx->video_stream >= 0) {
        RAviStream* vs = &ctx->streams[ctx->video_stream];
        if (vs->video.width > 0) ctx->width = vs->video.width;
        if (vs->video.height > 0) ctx->height = vs->video.height;
        if (vs->video.fps > 0) ctx->fps = vs->video.fps;
        if (vs->total_frames > 0) ctx->total_frames = vs->total_frames;
    }

    ctx->current_frame = 0;
    return RAVI_OK;
}

/*
 * ファイル情報取得
 */
static inline int ravi_get_info(RAviContext* ctx, int* width, int* height,
                                 float* fps, uint32_t* total_frames) {
    if (!ctx) return RAVI_ERROR;
    if (width) *width = ctx->width;
    if (height) *height = ctx->height;
    if (fps) *fps = ctx->fps;
    if (total_frames) *total_frames = ctx->total_frames;
    return RAVI_OK;
}

/*
 * オーディオ情報取得
 */
static inline int ravi_get_audio_info(RAviContext* ctx, int* channels,
                                       int* sample_rate, int* bits_per_sample) {
    if (!ctx || ctx->audio_stream < 0 ||
        ctx->audio_stream >= RAVI_MAX_STREAMS) return RAVI_ERROR;
    RAviStream* as = &ctx->streams[ctx->audio_stream];
    if (channels) *channels = as->audio.channels;
    if (sample_rate) *sample_rate = as->audio.sample_rate;
    if (bits_per_sample) *bits_per_sample = as->audio.bits_per_sample;
    return RAVI_OK;
}

/*
 * ビデオコーデック取得
 */
static inline uint32_t ravi_get_video_codec(RAviContext* ctx) {
    if (!ctx || ctx->video_stream < 0 ||
        ctx->video_stream >= RAVI_MAX_STREAMS) return 0;
    return ctx->streams[ctx->video_stream].video.codec;
}

static inline void ravi_clear_frame(RAviFrame* frame) {
    if (frame == 0) return;
    for (size_t i = 0u; i < sizeof(*frame); ++i)
        ((uint8_t*)frame)[i] = 0u;
}

static inline int ravi_index_range(const RAviContext* ctx,
                                   const uint8_t** entry_out) {
    if (ctx == 0 || entry_out == 0 || ctx->data == 0 ||
        ctx->idx1_offset > ctx->size ||
        ctx->idx1_count >
            (ctx->size - ctx->idx1_offset) / sizeof(RAviIndexEntry)) return 0;
    *entry_out = ctx->data + ctx->idx1_offset;
    return 1;
}

static inline int ravi_frame_payload(const RAviContext* ctx,
                                     uint32_t offset, uint32_t size,
                                     const uint8_t** data_out) {
    size_t payload_offset;
    if (ctx == 0 || data_out == 0 || ctx->movi_offset > ctx->size ||
        (size_t)offset > ctx->movi_size ||
        ctx->movi_size - (size_t)offset < 8u ||
        (size_t)size > ctx->movi_size - (size_t)offset - 8u)
        return 0;
    payload_offset = ctx->movi_offset + (size_t)offset + 8u;
    if (!ravi_range_valid(ctx, payload_offset, size)) return 0;
    *data_out = ctx->data + payload_offset;
    return 1;
}

/*
 * インデックスを使ってフレームを取得
 */
static inline int ravi_get_frame_by_index(RAviContext* ctx, uint32_t frame_num,
                                           RAviFrame* frame) {
    const uint8_t* index_data = 0;
    ravi_clear_frame(frame);
    if (!ctx || !frame || ctx->idx1_offset == 0 ||
        !ravi_index_range(ctx, &index_data)) return RAVI_ERROR;
    uint32_t video_frame = 0;

    for (size_t i = 0; i < ctx->idx1_count; i++) {
        const uint8_t* entry = index_data + i * sizeof(RAviIndexEntry);
        uint32_t chunk_id = ravi_load32(entry + 0u);
        uint32_t flags = ravi_load32(entry + 4u);
        uint32_t offset = ravi_load32(entry + 8u);
        uint32_t size = ravi_load32(entry + 12u);
        const uint8_t* payload = 0;
        if (ravi_is_video_chunk(chunk_id)) {
            if (video_frame == frame_num) {
                if (!ravi_frame_payload(ctx, offset, size, &payload))
                    return RAVI_DATA_ERROR;
                frame->data = payload;
                frame->size = size;
                frame->stream_index = ravi_get_stream_index(chunk_id);
                frame->is_keyframe = (flags & 0x10u) != 0u;
                frame->frame_number = frame_num;
                return RAVI_OK;
            }
            if (video_frame == UINT32_MAX) return RAVI_DATA_ERROR;
            video_frame++;
        }
    }

    return RAVI_END_OF_FILE;
}

/*
 * 次のビデオフレームを取得（シーケンシャル読み取り）
 */
static inline int ravi_read_next_video_frame(RAviContext* ctx, RAviFrame* frame) {
    if (!ctx) {
        ravi_clear_frame(frame);
        return RAVI_ERROR;
    }
    int ret = ravi_get_frame_by_index(ctx, ctx->current_frame, frame);
    if (ret == RAVI_OK) {
        if (ctx->current_frame != UINT32_MAX)
            ctx->current_frame++;
    }
    return ret;
}

/*
 * オーディオチャンクを取得
 */
static inline int ravi_get_audio_chunk(RAviContext* ctx, uint32_t chunk_num,
                                        RAviFrame* frame) {
    const uint8_t* index_data = 0;
    ravi_clear_frame(frame);
    if (!ctx || !frame || ctx->idx1_offset == 0 ||
        !ravi_index_range(ctx, &index_data)) return RAVI_ERROR;
    uint32_t audio_chunk = 0;

    for (size_t i = 0; i < ctx->idx1_count; i++) {
        const uint8_t* entry = index_data + i * sizeof(RAviIndexEntry);
        uint32_t chunk_id = ravi_load32(entry + 0u);
        uint32_t offset = ravi_load32(entry + 8u);
        uint32_t size = ravi_load32(entry + 12u);
        const uint8_t* payload = 0;
        if (ravi_is_audio_chunk(chunk_id)) {
            if (audio_chunk == chunk_num) {
                if (!ravi_frame_payload(ctx, offset, size, &payload))
                    return RAVI_DATA_ERROR;
                frame->data = payload;
                frame->size = size;
                frame->stream_index = ravi_get_stream_index(chunk_id);
                frame->is_keyframe = 1;
                frame->frame_number = chunk_num;
                return RAVI_OK;
            }
            if (audio_chunk == UINT32_MAX) return RAVI_DATA_ERROR;
            audio_chunk++;
        }
    }

    return RAVI_END_OF_FILE;
}

/*
 * フレーム位置をシーク
 */
static inline int ravi_seek(RAviContext* ctx, uint32_t frame_num) {
    if (!ctx) return RAVI_ERROR;
    if (frame_num >= ctx->total_frames) {
        frame_num = ctx->total_frames > 0 ? ctx->total_frames - 1 : 0;
    }
    ctx->current_frame = frame_num;
    return RAVI_OK;
}

/*
 * 時間でシーク (ミリ秒)
 */
static inline int ravi_seek_ms(RAviContext* ctx, uint32_t time_ms) {
    if (!ctx || ctx->fps <= 0) return RAVI_ERROR;
    uint32_t frame = ravi_frame_for_time(time_ms, ctx->fps);
    return ravi_seek(ctx, frame);
}

/*
 * 現在のフレーム番号取得
 */
static inline uint32_t ravi_get_current_frame(RAviContext* ctx) {
    return ctx ? ctx->current_frame : 0;
}

/*
 * 現在の再生時間取得 (ミリ秒)
 */
static inline uint32_t ravi_get_current_time_ms(RAviContext* ctx) {
    if (!ctx || ctx->fps <= 0) return 0;
    return ravi_frame_time_ms(ctx->current_frame, ctx->fps);
}

#endif /* RINAVI_H */
