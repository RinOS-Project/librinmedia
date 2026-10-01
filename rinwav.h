/*
 * RinOS WAV Decoder ✿
 * 軽量WAVオーディオデコーダー
 * PCM / IMA-ADPCM 対応
 */

#ifndef RINWAV_H
#define RINWAV_H

#if defined(RIN_FREESTANDING)
#include "../libc/stdint.h"
#include "../libc/stddef.h"
#else
#include <stdint.h>
#include <stddef.h>
#endif

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
    size_t  adpcm_block_start;
    uint32_t adpcm_block_frames;
    uint32_t adpcm_frame;
    uint8_t adpcm_block_active;
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

static inline uint16_t rwav_read_le16(const uint8_t* bytes)
{
    return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8u);
}

static inline uint32_t rwav_read_le32(const uint8_t* bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
           ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static inline int rwav_extensible_subformat_valid(const uint8_t* guid,
                                                   uint16_t format)
{
    static const uint8_t suffix[12] = {
        0u, 0u, 0x10u, 0u, 0x80u, 0u, 0u, 0xaau,
        0u, 0x38u, 0x9bu, 0x71u
    };
    size_t index;
    if (guid == NULL ||
        (format != RWAV_FORMAT_PCM && format != RWAV_FORMAT_IEEE_FLOAT &&
         format != RWAV_FORMAT_ALAW && format != RWAV_FORMAT_MULAW &&
         format != RWAV_FORMAT_IMA_ADPCM))
        return 0;
    if (guid[0] != (uint8_t)format || guid[1] != 0u || guid[2] != 0u ||
        guid[3] != 0u)
        return 0;
    for (index = 0u; index < sizeof(suffix); ++index)
        if (guid[index + 4u] != suffix[index]) return 0;
    return 1;
}

static inline float rwav_read_le_float(const uint8_t* bytes)
{
    union {
        uint32_t bits;
        float value;
    } decoded;
    decoded.bits = rwav_read_le32(bytes);
    return decoded.value;
}

static inline int rwav_float_bits_finite(uint32_t bits)
{
    return (bits & UINT32_C(0x7f800000)) != UINT32_C(0x7f800000);
}

static inline int16_t rwav_pcm24_to_s16(const uint8_t* bytes)
{
    uint32_t raw = (uint32_t)bytes[0] |
                   ((uint32_t)bytes[1] << 8u) |
                   ((uint32_t)bytes[2] << 16u);
    int32_t sample;
    int32_t magnitude;
    if ((raw & UINT32_C(0x00800000)) != 0u)
        sample = (int32_t)(raw | UINT32_C(0xff000000));
    else
        sample = (int32_t)raw;
    /* Convert signed 24-bit PCM to signed 16-bit using the arithmetic
     * right-shift result, including the floor direction for negative values.
     * The 24-bit range makes the magnitude operation safe. */
    if (sample >= 0) return (int16_t)(sample / 256);
    magnitude = -sample;
    return (int16_t)-((magnitude + 255) / 256);
}

static inline void rwav_clear_samples(int16_t* output, size_t count)
{
    size_t index;
    if (output == NULL) return;
    for (index = 0u; index < count; ++index) output[index] = 0;
}

static inline int rwav_checked_sample_count(size_t samples,
                                             uint16_t channels,
                                             size_t* count_out)
{
    if (count_out == NULL || channels == 0u ||
        samples > SIZE_MAX / (size_t)channels)
        return 0;
    *count_out = samples * (size_t)channels;
    return 1;
}

static inline int rwav_checked_sample_bytes(size_t samples,
                                             uint16_t channels,
                                             size_t bytes_per_sample,
                                             size_t* bytes_out)
{
    size_t count;
    if (bytes_per_sample == 0u ||
        !rwav_checked_sample_count(samples, channels, &count) ||
        count > SIZE_MAX / bytes_per_sample)
        return 0;
    *bytes_out = count * bytes_per_sample;
    return 1;
}

static inline void rwav_clear_context(RWavContext* ctx)
{
    if (ctx == NULL) return;
    for (size_t index = 0u; index < sizeof(*ctx); ++index)
        ((uint8_t*)ctx)[index] = 0u;
}

static inline int rwav_open_fail(RWavContext* ctx, int result)
{
    rwav_clear_context(ctx);
    return result;
}

/*
 * WAVファイルを開く
 */
static inline int rwav_open(RWavContext* ctx, const uint8_t* data, size_t size) {
    size_t container_end;
    size_t pos;
    uint32_t file_size;
    int format_seen = 0;
    int data_seen = 0;

    if (ctx == NULL) return RWAV_ERROR;
    rwav_clear_context(ctx);
    if (data == NULL || size < 12u)
        return rwav_open_fail(ctx, RWAV_ERROR);

    if (rwav_read_le32(data) != RWAV_RIFF ||
        rwav_read_le32(data + 8u) != RWAV_WAVE)
        return rwav_open_fail(ctx, RWAV_DATA_ERROR);
    file_size = rwav_read_le32(data + 4u);
    if (file_size < 4u ||
        (uint64_t)file_size + 8u > (uint64_t)size)
        return rwav_open_fail(ctx, RWAV_DATA_ERROR);
    container_end = (size_t)((uint64_t)file_size + 8u);
    ctx->data = data;
    ctx->size = container_end;
    pos = 12u;

    while (pos < container_end) {
        uint32_t chunk_id;
        uint32_t chunk_size;
        size_t payload;
        size_t chunk_end;
        size_t padded_end;
        if (container_end - pos < 8u)
            return rwav_open_fail(ctx, RWAV_DATA_ERROR);
        chunk_id = rwav_read_le32(data + pos);
        chunk_size = rwav_read_le32(data + pos + 4u);
        payload = pos + 8u;
        if ((size_t)chunk_size > container_end - payload)
            return rwav_open_fail(ctx, RWAV_DATA_ERROR);
        chunk_end = payload + (size_t)chunk_size;
        if ((size_t)(chunk_size & 1u) > container_end - chunk_end)
            return rwav_open_fail(ctx, RWAV_DATA_ERROR);
        padded_end = chunk_end + (size_t)(chunk_size & 1u);

        if (chunk_id == RWAV_FMT) {
            if (format_seen || chunk_size < 16u)
                return rwav_open_fail(ctx, RWAV_DATA_ERROR);
            ctx->format = rwav_read_le16(data + payload);
            ctx->channels = rwav_read_le16(data + payload + 2u);
            ctx->sample_rate = rwav_read_le32(data + payload + 4u);
            ctx->block_align = rwav_read_le16(data + payload + 12u);
            ctx->bits_per_sample = rwav_read_le16(data + payload + 14u);
            if (ctx->format == RWAV_FORMAT_EXTENSIBLE) {
                uint16_t sub_format;
                if (chunk_size < sizeof(RWavFormatEx) ||
                    rwav_read_le16(data + payload + 16u) < 22u)
                    return rwav_open_fail(ctx, RWAV_DATA_ERROR);
                sub_format = rwav_read_le16(data + payload + 24u);
                if (!rwav_extensible_subformat_valid(data + payload + 24u,
                                                     sub_format))
                    return rwav_open_fail(ctx, RWAV_UNSUPPORTED);
                ctx->format = sub_format;
            }
            format_seen = 1;
        } else if (chunk_id == RWAV_DATA) {
            if (data_seen)
                return rwav_open_fail(ctx, RWAV_DATA_ERROR);
            ctx->data_offset = payload;
            ctx->data_size = (size_t)chunk_size;
            data_seen = 1;
        }
        pos = padded_end;
    }

    if (!format_seen || !data_seen || ctx->channels == 0u ||
        ctx->channels > 2u || ctx->sample_rate == 0u ||
        ctx->block_align == 0u)
        return rwav_open_fail(ctx, RWAV_DATA_ERROR);

    /* A data chunk must end on a complete interleaved block.  Accepting a
     * trailing byte here would make get_info() silently floor the sample
     * count while rwav_read_s16() exposed a partial frame.  Keep the legacy
     * reader failure-closed; rinmedia_demux applies the same boundary. */
    if (ctx->data_size % (size_t)ctx->block_align != 0u)
        return rwav_open_fail(ctx, RWAV_DATA_ERROR);

    if (ctx->format == RWAV_FORMAT_PCM) {
        size_t bytes_per_sample;
        if (ctx->bits_per_sample != 8u && ctx->bits_per_sample != 16u &&
            ctx->bits_per_sample != 24u && ctx->bits_per_sample != 32u)
            return rwav_open_fail(ctx, RWAV_UNSUPPORTED);
        bytes_per_sample = (size_t)ctx->bits_per_sample / 8u;
        if ((size_t)ctx->block_align !=
                (size_t)ctx->channels * bytes_per_sample)
            return rwav_open_fail(ctx, RWAV_DATA_ERROR);
    } else if (ctx->format == RWAV_FORMAT_IEEE_FLOAT) {
        if (ctx->bits_per_sample != 32u ||
            (size_t)ctx->block_align != (size_t)ctx->channels * 4u)
            return rwav_open_fail(ctx, RWAV_UNSUPPORTED);
    } else if (ctx->format == RWAV_FORMAT_ALAW ||
               ctx->format == RWAV_FORMAT_MULAW) {
        if (ctx->bits_per_sample != 8u ||
            (size_t)ctx->block_align != (size_t)ctx->channels)
            return rwav_open_fail(ctx, RWAV_UNSUPPORTED);
    } else if (ctx->format == RWAV_FORMAT_IMA_ADPCM) {
        if (ctx->bits_per_sample != 4u ||
            (size_t)ctx->block_align < (size_t)ctx->channels * 4u)
            return rwav_open_fail(ctx, RWAV_UNSUPPORTED);
    } else {
        return rwav_open_fail(ctx, RWAV_UNSUPPORTED);
    }

    ctx->read_pos = 0u;
    return RWAV_OK;
}

/*
 * フォーマット情報取得
 */
static inline int rwav_get_info(RWavContext* ctx, int* channels, int* sample_rate,
                                 int* bits_per_sample, uint32_t* total_samples) {
    if (channels) *channels = 0;
    if (sample_rate) *sample_rate = 0;
    if (bits_per_sample) *bits_per_sample = 0;
    if (total_samples) *total_samples = 0u;
    if (!ctx) return RWAV_ERROR;
    if (ctx->data == NULL || ctx->data_offset > ctx->size ||
        ctx->data_size > ctx->size - ctx->data_offset ||
        ctx->channels == 0u || ctx->channels > 2u ||
        ctx->sample_rate == 0u || ctx->block_align == 0u)
        return RWAV_DATA_ERROR;

    if (channels) *channels = ctx->channels;
    if (sample_rate) *sample_rate = ctx->sample_rate;
    if (bits_per_sample) *bits_per_sample = ctx->bits_per_sample;

    if (total_samples) {
        if (ctx->format == RWAV_FORMAT_IMA_ADPCM) {
            /* ADPCM: ブロック数 × ブロックあたりサンプル数 */
            size_t samples_per_block;
            size_t num_blocks;
            uint64_t total;
            if (ctx->bits_per_sample != 4u ||
                (size_t)ctx->block_align < (size_t)ctx->channels * 4u)
                return RWAV_DATA_ERROR;
            samples_per_block =
                ((size_t)ctx->block_align - (size_t)ctx->channels * 4u) *
                    2u / (size_t)ctx->channels + 1u;
            num_blocks = ctx->data_size / (size_t)ctx->block_align;
            total = (uint64_t)num_blocks * (uint64_t)samples_per_block;
            *total_samples = total > UINT32_MAX ? UINT32_MAX :
                              (uint32_t)total;
        } else {
            size_t bytes_per_sample = (size_t)ctx->bits_per_sample / 8u;
            size_t frame_bytes;
            if (bytes_per_sample == 0u ||
                (size_t)ctx->channels > SIZE_MAX / bytes_per_sample)
                return RWAV_DATA_ERROR;
            frame_bytes = bytes_per_sample * (size_t)ctx->channels;
            *total_samples = (uint32_t)(ctx->data_size / frame_bytes >
                                            UINT32_MAX
                                        ? UINT32_MAX
                                        : ctx->data_size / frame_bytes);
        }
    }

    return RWAV_OK;
}

/*
 * 再生時間取得 (ミリ秒)
 */
static inline uint32_t rwav_get_duration_ms(RWavContext* ctx) {
    if (!ctx || ctx->sample_rate == 0) return 0;
    uint32_t total_samples = 0u;
    uint64_t duration_ms;
    if (rwav_get_info(ctx, NULL, NULL, NULL, &total_samples) != RWAV_OK)
        return 0u;
    duration_ms = (uint64_t)total_samples * 1000u / ctx->sample_rate;
    return duration_ms >= (uint64_t)UINT32_MAX ? UINT32_MAX :
           (uint32_t)duration_ms;
}

/*
 * PCMデータ読み取り (16bit signed に変換)
 */
static inline int rwav_read_s16(RWavContext* ctx, int16_t* output, size_t num_samples) {
    if (!ctx || !output) return RWAV_ERROR;
    if (num_samples == 0u) return RWAV_END_OF_FILE;
    if (num_samples > (size_t)INT32_MAX) return RWAV_ERROR;
    if (ctx->data == NULL || ctx->data_offset > ctx->size ||
        ctx->data_size > ctx->size - ctx->data_offset ||
        ctx->read_pos > ctx->data_size || ctx->channels == 0u ||
        ctx->channels > 2u || ctx->block_align == 0u)
        return RWAV_DATA_ERROR;
    if (ctx->format == RWAV_FORMAT_IMA_ADPCM &&
        (ctx->bits_per_sample != 4u ||
         (size_t)ctx->block_align < (size_t)ctx->channels * 4u))
        return RWAV_DATA_ERROR;

    const uint8_t* src = ctx->data + ctx->data_offset + ctx->read_pos;
    size_t remaining = ctx->data_size - ctx->read_pos;
    size_t samples_read = 0;

    if (ctx->format == RWAV_FORMAT_PCM) {
        if (ctx->bits_per_sample == 16) {
            /* 16bit PCM: そのままコピー */
            size_t bytes;
            if (!rwav_checked_sample_bytes(num_samples, ctx->channels, 2u,
                                           &bytes))
                return RWAV_ERROR;
            if (bytes > remaining) bytes = remaining;
            size_t count = bytes / 2;

            for (size_t i = 0; i < count; i++) {
                output[i] = (int16_t)rwav_read_le16(src + i * 2u);
            }
            ctx->read_pos += bytes;
            samples_read = count / ctx->channels;

        } else if (ctx->bits_per_sample == 8) {
            /* 8bit PCM: unsigned -> signed 変換 */
            size_t bytes;
            if (!rwav_checked_sample_bytes(num_samples, ctx->channels, 1u,
                                           &bytes))
                return RWAV_ERROR;
            if (bytes > remaining) bytes = remaining;

            for (size_t i = 0; i < bytes; i++) {
                output[i] = ((int16_t)src[i] - 128) << 8;
            }
            ctx->read_pos += bytes;
            samples_read = bytes / ctx->channels;

        } else if (ctx->bits_per_sample == 24) {
            /* 24bit PCM: signed sampleをarithmetic right shiftで16bit化 */
            size_t bytes;
            if (!rwav_checked_sample_bytes(num_samples, ctx->channels, 3u,
                                           &bytes))
                return RWAV_ERROR;
            if (bytes > remaining) bytes = remaining;
            size_t count = bytes / 3;

            for (size_t i = 0; i < count; i++) {
                output[i] = rwav_pcm24_to_s16(src + i * 3u);
            }
            ctx->read_pos += bytes;
            samples_read = count / ctx->channels;

        } else if (ctx->bits_per_sample == 32) {
            /* 32bit PCM: 上位16bitを取得 */
            size_t bytes;
            if (!rwav_checked_sample_bytes(num_samples, ctx->channels, 4u,
                                           &bytes))
                return RWAV_ERROR;
            if (bytes > remaining) bytes = remaining;
            size_t count = bytes / 4;

            for (size_t i = 0; i < count; i++) {
                int32_t val = (int32_t)rwav_read_le32(src + i * 4u);
                output[i] = (int16_t)(val >> 16);
            }
            ctx->read_pos += bytes;
            samples_read = count / ctx->channels;
        }

    } else if (ctx->format == RWAV_FORMAT_IEEE_FLOAT) {
        /* 32bit float */
        size_t bytes;
        if (!rwav_checked_sample_bytes(num_samples, ctx->channels, 4u,
                                       &bytes))
            return RWAV_ERROR;
        if (bytes > remaining) bytes = remaining;
        size_t count = bytes / 4;

        for (size_t i = 0; i < count; i++) {
            if (!rwav_float_bits_finite(rwav_read_le32(src + i * 4u))) {
                rwav_clear_samples(output, num_samples * (size_t)ctx->channels);
                return RWAV_DATA_ERROR;
            }
        }
        for (size_t i = 0; i < count; i++) {
            float f = rwav_read_le_float(src + i * 4u);
            if (f > 1.0f) f = 1.0f;
            if (f < -1.0f) f = -1.0f;
            output[i] = (int16_t)(f * 32767.0f);
        }
        ctx->read_pos += bytes;
        samples_read = count / ctx->channels;

    } else if (ctx->format == RWAV_FORMAT_ALAW) {
        /* A-law */
        size_t bytes;
        if (!rwav_checked_sample_bytes(num_samples, ctx->channels, 1u,
                                       &bytes))
            return RWAV_ERROR;
        if (bytes > remaining) bytes = remaining;

        for (size_t i = 0; i < bytes; i++) {
            output[i] = rwav_alaw_to_linear(src[i]);
        }
        ctx->read_pos += bytes;
        samples_read = bytes / ctx->channels;

    } else if (ctx->format == RWAV_FORMAT_MULAW) {
        /* μ-law */
        size_t bytes;
        if (!rwav_checked_sample_bytes(num_samples, ctx->channels, 1u,
                                       &bytes))
            return RWAV_ERROR;
        if (bytes > remaining) bytes = remaining;

        for (size_t i = 0; i < bytes; i++) {
            output[i] = rwav_ulaw_to_linear(src[i]);
        }
        ctx->read_pos += bytes;
        samples_read = bytes / ctx->channels;

    } else if (ctx->format == RWAV_FORMAT_IMA_ADPCM) {
        /* IMA ADPCM */
        size_t out_idx = 0;
        size_t max_out;
        if (!rwav_checked_sample_count(num_samples, ctx->channels, &max_out))
            return RWAV_ERROR;

        while (out_idx < max_out) {
            if (!ctx->adpcm_block_active) {
                size_t header_bytes = (size_t)ctx->channels * 4u;
                size_t data_bytes;
                if (ctx->read_pos > ctx->data_size ||
                    ctx->data_size - ctx->read_pos < ctx->block_align)
                    break;
                if (ctx->block_align < header_bytes)
                    return RWAV_DATA_ERROR;
                data_bytes = (size_t)ctx->block_align - header_bytes;
                if (data_bytes == 0u || data_bytes > SIZE_MAX / 2u)
                    return RWAV_DATA_ERROR;
                ctx->adpcm_block_start = ctx->read_pos;
                ctx->adpcm_block_frames =
                    (uint32_t)(data_bytes * 2u / (size_t)ctx->channels + 1u);
                ctx->adpcm_frame = 0u;
                for (int ch = 0; ch < ctx->channels; ch++) {
                    const uint8_t* block = ctx->data + ctx->data_offset +
                                           ctx->adpcm_block_start;
                    ctx->adpcm_predictor[ch] =
                        (int16_t)rwav_read_le16(block + (size_t)ch * 4u);
                    ctx->adpcm_step_index[ch] = block[(size_t)ch * 4u + 2u];
                    if (ctx->adpcm_step_index[ch] > 88)
                        return RWAV_DATA_ERROR;
                }
                ctx->adpcm_block_active = 1u;
            }

            {
                const uint8_t* block = ctx->data + ctx->data_offset +
                                       ctx->adpcm_block_start;
                uint32_t frame = ctx->adpcm_frame;
                for (int ch = 0; ch < ctx->channels; ++ch) {
                    if (frame == 0u) {
                        output[out_idx++] = ctx->adpcm_predictor[ch];
                    } else {
                        size_t sample_index = (size_t)frame - 1u;
                        size_t group = sample_index / 8u;
                        size_t within = sample_index % 8u;
                        size_t channel_stride = (size_t)ctx->channels * 4u;
                        size_t byte_offset = channel_stride +
                            group * channel_stride + (size_t)ch * 4u +
                            within / 2u;
                        uint8_t packed;
                        int nibble;
                        if (byte_offset >= (size_t)ctx->block_align)
                            return RWAV_DATA_ERROR;
                        packed = block[byte_offset];
                        nibble = (within & 1u) != 0u
                            ? (int)(packed >> 4u) : (int)(packed & 0x0fu);
                        output[out_idx++] = rwav_ima_decode_sample(
                            nibble, &ctx->adpcm_predictor[ch],
                            &ctx->adpcm_step_index[ch]);
                    }
                }
                ++ctx->adpcm_frame;
                if (ctx->adpcm_frame == ctx->adpcm_block_frames) {
                    ctx->read_pos = ctx->adpcm_block_start +
                                    (size_t)ctx->block_align;
                    ctx->adpcm_block_active = 0u;
                }
            }
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
    if (ctx->data == NULL || ctx->data_offset > ctx->size ||
        ctx->data_size > ctx->size - ctx->data_offset ||
        ctx->channels == 0u || ctx->channels > 2u ||
        ctx->block_align == 0u)
        return RWAV_DATA_ERROR;

    if (ctx->format == RWAV_FORMAT_IMA_ADPCM) {
        /* ADPCM: ブロック単位でシーク */
        size_t samples_per_block;
        size_t block_num;
        if (ctx->bits_per_sample != 4u ||
            (size_t)ctx->block_align < (size_t)ctx->channels * 4u)
            return RWAV_DATA_ERROR;
        samples_per_block =
            ((size_t)ctx->block_align - (size_t)ctx->channels * 4u) * 2u /
                (size_t)ctx->channels + 1u;
        block_num = (size_t)sample_pos / samples_per_block;
        if (block_num > SIZE_MAX / (size_t)ctx->block_align)
            ctx->read_pos = ctx->data_size;
        else
            ctx->read_pos = block_num * (size_t)ctx->block_align;
    } else {
        size_t bytes_per_sample = (size_t)ctx->bits_per_sample / 8u;
        size_t frame_bytes;
        if (bytes_per_sample == 0u ||
            (size_t)ctx->channels > SIZE_MAX / bytes_per_sample)
            return RWAV_DATA_ERROR;
        frame_bytes = bytes_per_sample * (size_t)ctx->channels;
        if ((size_t)sample_pos > SIZE_MAX / frame_bytes)
            ctx->read_pos = ctx->data_size;
        else
            ctx->read_pos = (size_t)sample_pos * frame_bytes;
    }

    if (ctx->read_pos > ctx->data_size) {
        ctx->read_pos = ctx->data_size;
    }
    ctx->adpcm_block_active = 0u;
    ctx->adpcm_block_frames = 0u;
    ctx->adpcm_frame = 0u;

    return RWAV_OK;
}

/*
 * 時間でシーク (ミリ秒)
 */
static inline int rwav_seek_ms(RWavContext* ctx, uint32_t time_ms) {
    if (!ctx) return RWAV_ERROR;
    if (ctx->sample_rate == 0u) return RWAV_DATA_ERROR;
    uint64_t sample_pos = (uint64_t)time_ms * ctx->sample_rate / 1000u;
    if (sample_pos > UINT32_MAX) sample_pos = UINT32_MAX;
    return rwav_seek(ctx, (uint32_t)sample_pos);
}

/*
 * リセット (最初に戻る)
 */
static inline int rwav_reset(RWavContext* ctx) {
    if (!ctx) return RWAV_ERROR;
    ctx->read_pos = 0;
    ctx->adpcm_predictor[0] = ctx->adpcm_predictor[1] = 0;
    ctx->adpcm_step_index[0] = ctx->adpcm_step_index[1] = 0;
    ctx->adpcm_block_start = 0u;
    ctx->adpcm_block_frames = 0u;
    ctx->adpcm_frame = 0u;
    ctx->adpcm_block_active = 0u;
    return RWAV_OK;
}

/*
 * 生データポインタ取得 (直接アクセス用)
 */
static inline const uint8_t* rwav_get_raw_data(RWavContext* ctx, size_t* size) {
    if (size) *size = 0u;
    if (!ctx || ctx->data == NULL || ctx->data_offset > ctx->size ||
        ctx->data_size > ctx->size - ctx->data_offset)
        return NULL;
    if (size) *size = ctx->data_size;
    return ctx->data + ctx->data_offset;
}

#endif /* RINWAV_H */
