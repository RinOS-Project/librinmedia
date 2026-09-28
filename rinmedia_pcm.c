/* SPDX-License-Identifier: MIT */

#include "rinmedia_pcm.h"

#include <string.h>

static void pcm_zero(void* output, size_t bytes)
{
    if (output != NULL && bytes != 0u) memset(output, 0, bytes);
}

static size_t pcm_bytes_per_sample(uint32_t format)
{
    switch (format) {
    case RIN_MEDIA_PCM_FORMAT_U8:
    case RIN_MEDIA_PCM_FORMAT_S8:
        return 1u;
    case RIN_MEDIA_PCM_FORMAT_S16LE:
        return 2u;
    case RIN_MEDIA_PCM_FORMAT_S24LE:
        return 3u;
    case RIN_MEDIA_PCM_FORMAT_S32LE:
    case RIN_MEDIA_PCM_FORMAT_F32LE:
        return 4u;
    default:
        return 0u;
    }
}

int rin_media_pcm_config_valid(const RinMediaPcmConfigV1* config)
{
    size_t index;
    if (config == NULL || config->struct_size != sizeof(*config) ||
        config->abi_version != RIN_MEDIA_PCM_ABI_V1 ||
        config->sample_rate == 0u || config->sample_rate > 384000u ||
        config->channels == 0u ||
        config->channels > RIN_MEDIA_PCM_MAX_CHANNELS ||
        pcm_bytes_per_sample(config->format) == 0u || config->flags != 0u)
        return 0;
    for (index = 0u; index < sizeof(config->reserved) / sizeof(uint32_t);
         ++index) {
        if (config->reserved[index] != 0u) return 0;
    }
    return 1;
}

static uint16_t pcm_read_le16(const uint8_t* input)
{
    return (uint16_t)input[0] | (uint16_t)((uint16_t)input[1] << 8u);
}

static uint32_t pcm_read_le32(const uint8_t* input)
{
    return (uint32_t)input[0] | ((uint32_t)input[1] << 8u) |
           ((uint32_t)input[2] << 16u) | ((uint32_t)input[3] << 24u);
}

static int16_t pcm_signed_shift(int32_t value, unsigned shift)
{
    int64_t wide = (int64_t)value;
    const int64_t divisor = (int64_t)UINT32_C(1) << shift;
    if (wide >= 0) return (int16_t)(wide / divisor);
    wide = -wide;
    wide += divisor - 1;
    return (int16_t)(-(wide / divisor));
}

static int16_t pcm_decode_sample(const uint8_t* input, uint32_t format,
                                 int* nonfinite)
{
    if (nonfinite != NULL) *nonfinite = 0;
    switch (format) {
    case RIN_MEDIA_PCM_FORMAT_U8:
        return (int16_t)(((int32_t)input[0] - 128) * 256);
    case RIN_MEDIA_PCM_FORMAT_S8:
        return (int16_t)((int16_t)(int8_t)input[0] * 256);
    case RIN_MEDIA_PCM_FORMAT_S16LE:
        return (int16_t)pcm_read_le16(input);
    case RIN_MEDIA_PCM_FORMAT_S24LE: {
        int32_t value = (int32_t)input[0] |
                        ((int32_t)input[1] << 8u) |
                        ((int32_t)input[2] << 16u);
        if ((value & INT32_C(0x00800000)) != 0)
            value |= (int32_t)~UINT32_C(0x00ffffff);
        return pcm_signed_shift(value, 8u);
    }
    case RIN_MEDIA_PCM_FORMAT_S32LE:
        return pcm_signed_shift((int32_t)pcm_read_le32(input), 16u);
    case RIN_MEDIA_PCM_FORMAT_F32LE: {
        union {
            uint32_t bits;
            float value;
        } decoded;
        float value;
        decoded.bits = pcm_read_le32(input);
        if ((decoded.bits & UINT32_C(0x7f800000)) ==
            UINT32_C(0x7f800000)) {
            if (nonfinite != NULL) *nonfinite = 1;
            return 0;
        }
        value = decoded.value;
        if (value > 1.0f) value = 1.0f;
        if (value < -1.0f) value = -1.0f;
        return (int16_t)(value * 32767.0f);
    }
    default:
        return 0;
    }
}

int rin_media_pcm_decode_s16(
    const RinMediaPcmConfigV1* config, const uint8_t* input,
    size_t input_bytes, int16_t* output, size_t output_capacity_samples,
    size_t* frames_out)
{
    const size_t bytes_per_sample = config == NULL
        ? 0u : pcm_bytes_per_sample(config->format);
    size_t frame_bytes;
    size_t frames;
    size_t samples;
    size_t frame;
    size_t channel;

    if (frames_out != NULL) *frames_out = 0u;
    if (output != NULL && output_capacity_samples <=
        SIZE_MAX / sizeof(*output))
        pcm_zero(output, output_capacity_samples * sizeof(*output));
    if (!rin_media_pcm_config_valid(config) || input == NULL ||
        input_bytes == 0u || input_bytes > RIN_MEDIA_PCM_MAX_INPUT_BYTES ||
        output == NULL || frames_out == NULL || bytes_per_sample == 0u)
        return RIN_MEDIA_PCM_INVALID;
    if ((size_t)config->channels > SIZE_MAX / bytes_per_sample)
        return RIN_MEDIA_PCM_LIMIT;
    frame_bytes = (size_t)config->channels * bytes_per_sample;
    if (frame_bytes == 0u || input_bytes % frame_bytes != 0u)
        return RIN_MEDIA_PCM_INVALID;
    frames = input_bytes / frame_bytes;
    if (frames == 0u || frames > RIN_MEDIA_PCM_MAX_FRAMES)
        return RIN_MEDIA_PCM_LIMIT;
    if ((size_t)config->channels > SIZE_MAX / frames)
        return RIN_MEDIA_PCM_LIMIT;
    samples = frames * (size_t)config->channels;
    if (output_capacity_samples < samples) return RIN_MEDIA_PCM_OUTPUT_TOO_SMALL;

    for (frame = 0u; frame < frames; ++frame) {
        for (channel = 0u; channel < config->channels; ++channel) {
            const size_t sample_index = frame * (size_t)config->channels + channel;
            const size_t input_offset = sample_index * bytes_per_sample;
            int nonfinite = 0;
            output[sample_index] = pcm_decode_sample(
                input + input_offset, config->format, &nonfinite);
            if (nonfinite != 0) {
                pcm_zero(output, samples * sizeof(*output));
                return RIN_MEDIA_PCM_NONFINITE;
            }
        }
    }
    *frames_out = frames;
    return RIN_MEDIA_PCM_OK;
}
