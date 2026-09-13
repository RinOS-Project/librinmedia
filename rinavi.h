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

static inline uint32_t ravi_read32(RAviContext* ctx) {
    if (ctx->pos + 4 > ctx->size) return 0;
    uint32_t val = ctx->data[ctx->pos] |
                   (ctx->data[ctx->pos + 1] << 8) |
                   (ctx->data[ctx->pos + 2] << 16) |
                   (ctx->data[ctx->pos + 3] << 24);
    ctx->pos += 4;
    return val;
}

static inline uint16_t ravi_read16(RAviContext* ctx) {
    if (ctx->pos + 2 > ctx->size) return 0;
    uint16_t val = ctx->data[ctx->pos] | (ctx->data[ctx->pos + 1] << 8);
    ctx->pos += 2;
    return val;
}

static inline void ravi_skip(RAviContext* ctx, size_t n) {
    ctx->pos += n;
    if (ctx->pos > ctx->size) ctx->pos = ctx->size;
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

/* ═══════════════════════════════════════════════════════════════
 * 内部パース関数
 * ═══════════════════════════════════════════════════════════════*/

static inline int ravi_parse_stream_header(RAviContext* ctx, size_t end_pos) {
    if (ctx->stream_count >= RAVI_MAX_STREAMS) return RAVI_ERROR;

    RAviStream* stream = &ctx->streams[ctx->stream_count];
    stream->stream_index = ctx->stream_count;

    while (ctx->pos < end_pos) {
        uint32_t fourcc = ravi_read32(ctx);
        uint32_t chunk_size = ravi_read32(ctx);
        size_t chunk_end = ctx->pos + chunk_size;

        if (fourcc == RAVI_STRH) {
            /* ストリームヘッダー */
            RAviStreamHeader hdr;
            if (chunk_size >= sizeof(RAviStreamHeader)) {
                const uint8_t* p = ctx->data + ctx->pos;
                hdr.type = *(uint32_t*)(p + 0);
                hdr.handler = *(uint32_t*)(p + 4);
                hdr.scale = *(uint32_t*)(p + 20);
                hdr.rate = *(uint32_t*)(p + 24);
                hdr.length = *(uint32_t*)(p + 32);

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
                    RAviVideoFormat* fmt = (RAviVideoFormat*)(ctx->data + ctx->pos);
                    stream->video.width = fmt->width;
                    stream->video.height = fmt->height > 0 ? fmt->height : -fmt->height;
                    stream->video.bit_depth = fmt->bit_count;
                    if (stream->video.codec == 0) {
                        stream->video.codec = fmt->compression;
                    }
                }
            } else if (stream->type == 1) {
                /* オーディオフォーマット */
                if (chunk_size >= sizeof(RAviAudioFormat)) {
                    RAviAudioFormat* fmt = (RAviAudioFormat*)(ctx->data + ctx->pos);
                    stream->audio.format = fmt->format_tag;
                    stream->audio.channels = fmt->channels;
                    stream->audio.sample_rate = fmt->samples_per_sec;
                    stream->audio.bits_per_sample = fmt->bits_per_sample;
                }
            }
        }

        ctx->pos = chunk_end;
        if (chunk_size & 1) ctx->pos++;  /* パディング */
    }

    ctx->stream_count++;
    return RAVI_OK;
}

static inline int ravi_parse_header_list(RAviContext* ctx, size_t end_pos) {
    while (ctx->pos < end_pos) {
        uint32_t fourcc = ravi_read32(ctx);
        uint32_t chunk_size = ravi_read32(ctx);
        size_t chunk_end = ctx->pos + chunk_size;

        if (fourcc == RAVI_AVIH) {
            /* メインヘッダー */
            if (chunk_size >= sizeof(RAviMainHeader)) {
                RAviMainHeader* hdr = (RAviMainHeader*)(ctx->data + ctx->pos);
                ctx->width = hdr->width;
                ctx->height = hdr->height;
                ctx->total_frames = hdr->total_frames;
                if (hdr->micro_sec_per_frame > 0) {
                    ctx->fps = 1000000.0f / (float)hdr->micro_sec_per_frame;
                    ctx->duration_ms = (uint32_t)((uint64_t)hdr->total_frames *
                                       hdr->micro_sec_per_frame / 1000);
                }
            }
        } else if (fourcc == RAVI_LIST) {
            uint32_t list_type = ravi_read32(ctx);
            if (list_type == RAVI_STRL) {
                ravi_parse_stream_header(ctx, chunk_end);
            }
        }

        ctx->pos = chunk_end;
        if (chunk_size & 1) ctx->pos++;
    }
    return RAVI_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * 公開API
 * ═══════════════════════════════════════════════════════════════*/

/*
 * AVIファイルを開く
 */
static inline int ravi_open(RAviContext* ctx, const uint8_t* data, size_t size) {
    if (!ctx || !data || size < 12) return RAVI_ERROR;

    /* 初期化 */
    for (size_t i = 0; i < sizeof(RAviContext); i++) {
        ((uint8_t*)ctx)[i] = 0;
    }
    ctx->data = data;
    ctx->size = size;
    ctx->video_stream = -1;
    ctx->audio_stream = -1;

    /* RIFFヘッダー確認 */
    uint32_t riff = ravi_read32(ctx);
    uint32_t file_size = ravi_read32(ctx);
    uint32_t avi = ravi_read32(ctx);
    (void)file_size;

    if (riff != RAVI_RIFF || avi != RAVI_AVI) {
        return RAVI_DATA_ERROR;
    }

    /* チャンク解析 */
    while (ctx->pos < ctx->size) {
        uint32_t fourcc = ravi_read32(ctx);
        uint32_t chunk_size = ravi_read32(ctx);
        size_t chunk_end = ctx->pos + chunk_size;

        if (fourcc == RAVI_LIST) {
            uint32_t list_type = ravi_read32(ctx);

            if (list_type == RAVI_HDRL) {
                ravi_parse_header_list(ctx, chunk_end);
            } else if (list_type == RAVI_MOVI) {
                ctx->movi_offset = ctx->pos;
                ctx->movi_size = chunk_size - 4;
            }
        } else if (fourcc == RAVI_IDX1) {
            ctx->idx1_offset = ctx->pos;
            ctx->idx1_count = chunk_size / sizeof(RAviIndexEntry);
        }

        ctx->pos = chunk_end;
        if (chunk_size & 1) ctx->pos++;
    }

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
    if (!ctx || ctx->audio_stream < 0) return RAVI_ERROR;
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
    if (!ctx || ctx->video_stream < 0) return 0;
    return ctx->streams[ctx->video_stream].video.codec;
}

/*
 * インデックスを使ってフレームを取得
 */
static inline int ravi_get_frame_by_index(RAviContext* ctx, uint32_t frame_num,
                                           RAviFrame* frame) {
    if (!ctx || !frame || ctx->idx1_offset == 0) return RAVI_ERROR;

    const RAviIndexEntry* idx = (const RAviIndexEntry*)(ctx->data + ctx->idx1_offset);
    uint32_t video_frame = 0;

    for (size_t i = 0; i < ctx->idx1_count; i++) {
        if (ravi_is_video_chunk(idx[i].chunk_id)) {
            if (video_frame == frame_num) {
                frame->data = ctx->data + ctx->movi_offset + idx[i].offset + 8;
                frame->size = idx[i].size;
                frame->stream_index = ravi_get_stream_index(idx[i].chunk_id);
                frame->is_keyframe = (idx[i].flags & 0x10) != 0;
                frame->frame_number = frame_num;
                return RAVI_OK;
            }
            video_frame++;
        }
    }

    return RAVI_END_OF_FILE;
}

/*
 * 次のビデオフレームを取得（シーケンシャル読み取り）
 */
static inline int ravi_read_next_video_frame(RAviContext* ctx, RAviFrame* frame) {
    int ret = ravi_get_frame_by_index(ctx, ctx->current_frame, frame);
    if (ret == RAVI_OK) {
        ctx->current_frame++;
    }
    return ret;
}

/*
 * オーディオチャンクを取得
 */
static inline int ravi_get_audio_chunk(RAviContext* ctx, uint32_t chunk_num,
                                        RAviFrame* frame) {
    if (!ctx || !frame || ctx->idx1_offset == 0) return RAVI_ERROR;

    const RAviIndexEntry* idx = (const RAviIndexEntry*)(ctx->data + ctx->idx1_offset);
    uint32_t audio_chunk = 0;

    for (size_t i = 0; i < ctx->idx1_count; i++) {
        if (ravi_is_audio_chunk(idx[i].chunk_id)) {
            if (audio_chunk == chunk_num) {
                frame->data = ctx->data + ctx->movi_offset + idx[i].offset + 8;
                frame->size = idx[i].size;
                frame->stream_index = ravi_get_stream_index(idx[i].chunk_id);
                frame->is_keyframe = 1;
                frame->frame_number = chunk_num;
                return RAVI_OK;
            }
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
    uint32_t frame = (uint32_t)(time_ms * ctx->fps / 1000.0f);
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
    return (uint32_t)(ctx->current_frame * 1000.0f / ctx->fps);
}

#endif /* RINAVI_H */
