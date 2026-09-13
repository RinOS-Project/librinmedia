/*
 * RinOS WAV Decoder ✿
 * 軽量WAVオーディオデコーダー
 * PCM / IMA-ADPCM 対応
 */

#ifndef RINWAV_H
#define RINWAV_H

#include "../libc/stdint.h"
#include "../libc/stddef.h"

/* ═══════════════════════════════════════════════════════════════
 * 定数
 * ═══════════════════════════════════════════════════════════════*/

#define RWAV_OK              0
#define RWAV_ERROR          -1
#define RWAV_DATA_ERROR     -2
#define RWAV_UNSUPPORTED    -3
#define RWAV_END_OF_FILE    -4

/* フォーマットタグ */
#define RWAV_FORMAT_PCM         0x0001
#define RWAV_FORMAT_ADPCM       0x0002
#define RWAV_FORMAT_IEEE_FLOAT  0x0003
#define RWAV_FORMAT_ALAW        0x0006
#define RWAV_FORMAT_MULAW       0x0007
#define RWAV_FORMAT_IMA_ADPCM   0x0011
#define RWAV_FORMAT_EXTENSIBLE  0xFFFE

/* FourCC */
#define RWAV_FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define RWAV_RIFF   RWAV_FOURCC('R', 'I', 'F', 'F')
#define RWAV_WAVE   RWAV_FOURCC('W', 'A', 'V', 'E')
#define RWAV_FMT    RWAV_FOURCC('f', 'm', 't', ' ')
#define RWAV_DATA   RWAV_FOURCC('d', 'a', 't', 'a')
#define RWAV_FACT   RWAV_FOURCC('f', 'a', 'c', 't')
#define RWAV_LIST   RWAV_FOURCC('L', 'I', 'S', 'T')

/* ═══════════════════════════════════════════════════════════════
 * 構造体
 * ═══════════════════════════════════════════════════════════════*/

#pragma pack(push, 1)

/* WAVフォーマットヘッダー */
typedef struct {
    uint16_t format_tag;
    uint16_t channels;
    uint32_t samples_per_sec;
    uint32_t avg_bytes_per_sec;
    uint16_t block_align;
    uint16_t bits_per_sample;
} RWavFormat;

/* 拡張フォーマット */
typedef struct {
    RWavFormat base;
    uint16_t ext_size;
    uint16_t valid_bits_per_sample;
    uint32_t channel_mask;
    uint8_t  sub_format[16];
} RWavFormatEx;

#pragma pack(pop)

/* WAVコンテキスト */
typedef struct {
    const uint8_t* data;
    size_t size;

    /* フォーマット情報 */
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint16_t block_align;

    /* データ位置 */
    size_t data_offset;
    size_t data_size;

    /* 再生状態 */
    size_t read_pos;

    /* ADPCM状態 */
    int16_t adpcm_predictor[2];
    int     adpcm_step_index[2];
} RWavContext;

/* ═══════════════════════════════════════════════════════════════
 * IMA-ADPCMデコード
 * ═══════════════════════════════════════════════════════════════*/

static const int rwav_ima_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static const int rwav_ima_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static inline int16_t rwav_ima_decode_sample(int nibble, int16_t* predictor, int* step_index) {
    int step = rwav_ima_step_table[*step_index];
    int diff = step >> 3;

    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    if (nibble & 8) diff = -diff;

    int pred = *predictor + diff;
    if (pred > 32767) pred = 32767;
    if (pred < -32768) pred = -32768;
    *predictor = (int16_t)pred;

    *step_index += rwav_ima_index_table[nibble];
    if (*step_index < 0) *step_index = 0;
    if (*step_index > 88) *step_index = 88;

    return *predictor;
}

/* ═══════════════════════════════════════════════════════════════
 * A-law / μ-law デコード
 * ═══════════════════════════════════════════════════════════════*/

static inline int16_t rwav_alaw_to_linear(uint8_t a) {
    a ^= 0x55;
    int sign = (a & 0x80) ? -1 : 1;
    int exponent = (a >> 4) & 0x07;
    int mantissa = a & 0x0F;

    int sample;
    if (exponent == 0) {
        sample = (mantissa << 4) + 8;
    } else {
        sample = ((mantissa << 4) + 0x108) << (exponent - 1);
    }
    return (int16_t)(sign * sample);
}

static inline int16_t rwav_ulaw_to_linear(uint8_t u) {
    u = ~u;
    int sign = (u & 0x80) ? -1 : 1;
    int exponent = (u >> 4) & 0x07;
    int mantissa = u & 0x0F;

    int sample = ((mantissa << 3) + 0x84) << exponent;
    sample -= 0x84;
    return (int16_t)(sign * sample);
}

/* ═══════════════════════════════════════════════════════════════
 * 公開API
 * ═══════════════════════════════════════════════════════════════*/

/*
 * WAVファイルを開く
 */
static inline int rwav_open(RWavContext* ctx, const uint8_t* data, size_t size) {
    if (!ctx || !data || size < 44) return RWAV_ERROR;

    /* 初期化 */
    for (size_t i = 0; i < sizeof(RWavContext); i++) {
        ((uint8_t*)ctx)[i] = 0;
    }
    ctx->data = data;
    ctx->size = size;

    size_t pos = 0;

    /* RIFFヘッダー確認 */
    uint32_t riff = *(uint32_t*)(data + pos); pos += 4;
    uint32_t file_size = *(uint32_t*)(data + pos); pos += 4;
    uint32_t wave = *(uint32_t*)(data + pos); pos += 4;
    (void)file_size;

    if (riff != RWAV_RIFF || wave != RWAV_WAVE) {
        return RWAV_DATA_ERROR;
    }

    /* チャンク解析 */
    while (pos < size - 8) {
        uint32_t chunk_id = *(uint32_t*)(data + pos); pos += 4;
        uint32_t chunk_size = *(uint32_t*)(data + pos); pos += 4;

        if (chunk_id == RWAV_FMT) {
            if (chunk_size >= sizeof(RWavFormat)) {
                RWavFormat* fmt = (RWavFormat*)(data + pos);
                ctx->format = fmt->format_tag;
                ctx->channels = fmt->channels;
                ctx->sample_rate = fmt->samples_per_sec;
                ctx->bits_per_sample = fmt->bits_per_sample;
                ctx->block_align = fmt->block_align;

                /* 拡張フォーマット対応 */
                if (ctx->format == RWAV_FORMAT_EXTENSIBLE && chunk_size >= sizeof(RWavFormatEx)) {
                    RWavFormatEx* fmtex = (RWavFormatEx*)(data + pos);
                    ctx->format = *(uint16_t*)fmtex->sub_format;
                }
            }
        } else if (chunk_id == RWAV_DATA) {
            ctx->data_offset = pos;
            ctx->data_size = chunk_size;
            break;  /* データチャンク発見 */
        }

        pos += chunk_size;
        if (chunk_size & 1) pos++;  /* パディング */
    }

    if (ctx->data_offset == 0) {
        return RWAV_DATA_ERROR;
    }

    /* サポート確認 */
    if (ctx->format != RWAV_FORMAT_PCM &&
        ctx->format != RWAV_FORMAT_IEEE_FLOAT &&
        ctx->format != RWAV_FORMAT_IMA_ADPCM &&
        ctx->format != RWAV_FORMAT_ALAW &&
        ctx->format != RWAV_FORMAT_MULAW) {
        return RWAV_UNSUPPORTED;
    }

    ctx->read_pos = 0;
    return RWAV_OK;
}

/*
 * フォーマット情報取得
 */
static inline int rwav_get_info(RWavContext* ctx, int* channels, int* sample_rate,
                                 int* bits_per_sample, uint32_t* total_samples) {
    if (!ctx) return RWAV_ERROR;

    if (channels) *channels = ctx->channels;
    if (sample_rate) *sample_rate = ctx->sample_rate;
    if (bits_per_sample) *bits_per_sample = ctx->bits_per_sample;

    if (total_samples) {
        if (ctx->format == RWAV_FORMAT_IMA_ADPCM) {
            /* ADPCM: ブロック数 × ブロックあたりサンプル数 */
            int samples_per_block = (ctx->block_align - 4 * ctx->channels) * 2 / ctx->channels + 1;
            int num_blocks = ctx->data_size / ctx->block_align;
            *total_samples = num_blocks * samples_per_block;
        } else {
            int bytes_per_sample = ctx->bits_per_sample / 8;
            if (bytes_per_sample > 0 && ctx->channels > 0) {
                *total_samples = ctx->data_size / (bytes_per_sample * ctx->channels);
            }
        }
    }

    return RWAV_OK;
}

/*
 * 再生時間取得 (ミリ秒)
 */
static inline uint32_t rwav_get_duration_ms(RWavContext* ctx) {
    if (!ctx || ctx->sample_rate == 0) return 0;
    uint32_t total_samples;
    rwav_get_info(ctx, NULL, NULL, NULL, &total_samples);
    return (uint32_t)((uint64_t)total_samples * 1000 / ctx->sample_rate);
}

/*
 * PCMデータ読み取り (16bit signed に変換)
 */
static inline int rwav_read_s16(RWavContext* ctx, int16_t* output, size_t num_samples) {
    if (!ctx || !output) return RWAV_ERROR;

    const uint8_t* src = ctx->data + ctx->data_offset + ctx->read_pos;
    size_t remaining = ctx->data_size - ctx->read_pos;
    size_t samples_read = 0;

    if (ctx->format == RWAV_FORMAT_PCM) {
        if (ctx->bits_per_sample == 16) {
            /* 16bit PCM: そのままコピー */
            size_t bytes = num_samples * ctx->channels * 2;
            if (bytes > remaining) bytes = remaining;
            size_t count = bytes / 2;

            for (size_t i = 0; i < count; i++) {
                output[i] = *(int16_t*)(src + i * 2);
            }
            ctx->read_pos += bytes;
            samples_read = count / ctx->channels;

        } else if (ctx->bits_per_sample == 8) {
            /* 8bit PCM: unsigned -> signed 変換 */
            size_t bytes = num_samples * ctx->channels;
            if (bytes > remaining) bytes = remaining;

            for (size_t i = 0; i < bytes; i++) {
                output[i] = ((int16_t)src[i] - 128) << 8;
            }
            ctx->read_pos += bytes;
            samples_read = bytes / ctx->channels;

        } else if (ctx->bits_per_sample == 24) {
            /* 24bit PCM: 上位16bitを取得 */
            size_t bytes = num_samples * ctx->channels * 3;
            if (bytes > remaining) bytes = remaining;
            size_t count = bytes / 3;

            for (size_t i = 0; i < count; i++) {
                output[i] = (int16_t)((src[i * 3 + 1]) | (src[i * 3 + 2] << 8));
            }
            ctx->read_pos += bytes;
            samples_read = count / ctx->channels;

        } else if (ctx->bits_per_sample == 32) {
            /* 32bit PCM: 上位16bitを取得 */
            size_t bytes = num_samples * ctx->channels * 4;
            if (bytes > remaining) bytes = remaining;
            size_t count = bytes / 4;

            for (size_t i = 0; i < count; i++) {
                int32_t val = *(int32_t*)(src + i * 4);
                output[i] = (int16_t)(val >> 16);
            }
            ctx->read_pos += bytes;
            samples_read = count / ctx->channels;
        }

    } else if (ctx->format == RWAV_FORMAT_IEEE_FLOAT) {
        /* 32bit float */
        size_t bytes = num_samples * ctx->channels * 4;
        if (bytes > remaining) bytes = remaining;
        size_t count = bytes / 4;

        for (size_t i = 0; i < count; i++) {
            float f = *(float*)(src + i * 4);
            if (f > 1.0f) f = 1.0f;
            if (f < -1.0f) f = -1.0f;
            output[i] = (int16_t)(f * 32767.0f);
        }
        ctx->read_pos += bytes;
        samples_read = count / ctx->channels;

    } else if (ctx->format == RWAV_FORMAT_ALAW) {
        /* A-law */
        size_t bytes = num_samples * ctx->channels;
        if (bytes > remaining) bytes = remaining;

        for (size_t i = 0; i < bytes; i++) {
            output[i] = rwav_alaw_to_linear(src[i]);
        }
        ctx->read_pos += bytes;
        samples_read = bytes / ctx->channels;

    } else if (ctx->format == RWAV_FORMAT_MULAW) {
        /* μ-law */
        size_t bytes = num_samples * ctx->channels;
        if (bytes > remaining) bytes = remaining;

        for (size_t i = 0; i < bytes; i++) {
            output[i] = rwav_ulaw_to_linear(src[i]);
        }
        ctx->read_pos += bytes;
        samples_read = bytes / ctx->channels;

    } else if (ctx->format == RWAV_FORMAT_IMA_ADPCM) {
        /* IMA ADPCM */
        size_t out_idx = 0;
        size_t max_out = num_samples * ctx->channels;

        while (ctx->read_pos + ctx->block_align <= ctx->data_size && out_idx < max_out) {
            const uint8_t* block = ctx->data + ctx->data_offset + ctx->read_pos;

            /* ブロックヘッダー読み取り */
            for (int ch = 0; ch < ctx->channels; ch++) {
                ctx->adpcm_predictor[ch] = *(int16_t*)(block + ch * 4);
                ctx->adpcm_step_index[ch] = block[ch * 4 + 2];
                if (ctx->adpcm_step_index[ch] > 88) ctx->adpcm_step_index[ch] = 88;
            }

            /* 最初のサンプル出力 */
            for (int ch = 0; ch < ctx->channels && out_idx < max_out; ch++) {
                output[out_idx++] = ctx->adpcm_predictor[ch];
            }

            /* データデコード */
            const uint8_t* data_ptr = block + 4 * ctx->channels;
            int data_bytes = ctx->block_align - 4 * ctx->channels;

            for (int i = 0; i < data_bytes && out_idx < max_out; i++) {
                int ch = (ctx->channels == 2) ? ((i / 4) & 1) : 0;
                uint8_t byte = data_ptr[i];

                output[out_idx++] = rwav_ima_decode_sample(byte & 0x0F,
                    &ctx->adpcm_predictor[ch], &ctx->adpcm_step_index[ch]);
                if (out_idx < max_out) {
                    output[out_idx++] = rwav_ima_decode_sample((byte >> 4) & 0x0F,
                        &ctx->adpcm_predictor[ch], &ctx->adpcm_step_index[ch]);
                }
            }

            ctx->read_pos += ctx->block_align;
        }
        samples_read = out_idx / ctx->channels;

    } else {
        return RWAV_UNSUPPORTED;
    }

    if (samples_read == 0) {
        return RWAV_END_OF_FILE;
    }

    return (int)samples_read;
}

/*
 * シーク (サンプル単位)
 */
static inline int rwav_seek(RWavContext* ctx, uint32_t sample_pos) {
    if (!ctx) return RWAV_ERROR;

    if (ctx->format == RWAV_FORMAT_IMA_ADPCM) {
        /* ADPCM: ブロック単位でシーク */
        int samples_per_block = (ctx->block_align - 4 * ctx->channels) * 2 / ctx->channels + 1;
        int block_num = sample_pos / samples_per_block;
        ctx->read_pos = block_num * ctx->block_align;
    } else {
        int bytes_per_sample = ctx->bits_per_sample / 8 * ctx->channels;
        ctx->read_pos = sample_pos * bytes_per_sample;
    }

    if (ctx->read_pos > ctx->data_size) {
        ctx->read_pos = ctx->data_size;
    }

    return RWAV_OK;
}

/*
 * 時間でシーク (ミリ秒)
 */
static inline int rwav_seek_ms(RWavContext* ctx, uint32_t time_ms) {
    if (!ctx) return RWAV_ERROR;
    uint32_t sample_pos = (uint32_t)((uint64_t)time_ms * ctx->sample_rate / 1000);
    return rwav_seek(ctx, sample_pos);
}

/*
 * リセット (最初に戻る)
 */
static inline int rwav_reset(RWavContext* ctx) {
    if (!ctx) return RWAV_ERROR;
    ctx->read_pos = 0;
    ctx->adpcm_predictor[0] = ctx->adpcm_predictor[1] = 0;
    ctx->adpcm_step_index[0] = ctx->adpcm_step_index[1] = 0;
    return RWAV_OK;
}

/*
 * 生データポインタ取得 (直接アクセス用)
 */
static inline const uint8_t* rwav_get_raw_data(RWavContext* ctx, size_t* size) {
    if (!ctx) return NULL;
    if (size) *size = ctx->data_size;
    return ctx->data + ctx->data_offset;
}

#endif /* RINWAV_H */
