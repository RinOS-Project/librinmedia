/* SPDX-License-Identifier: MIT */

#include "rinmedia_pcm.h"

#include <string.h>

static const uint32_t pcm_wav_format = UINT32_C(1);
static const uint32_t pcm_wav_float_format = UINT32_C(3);

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

int rin_media_pcm_decode_packet_table_s16(
    const uint8_t* source, size_t source_bytes,
    const RinMediaDemuxInfoV1* info,
    const RinMediaDemuxPacketTableV1* packets,
    const RinMediaPcmConfigV1* config, int16_t* output,
    size_t output_capacity_samples, size_t* frames_out)
{
    size_t total_frames = 0u;
    uint64_t previous_end = 0u;
    uint32_t packet_index;
    int table_valid;

    if (frames_out != NULL) *frames_out = 0u;
    if (output != NULL && output_capacity_samples <=
        SIZE_MAX / sizeof(*output))
        pcm_zero(output, output_capacity_samples * sizeof(*output));
    table_valid = source != NULL && source_bytes != 0u &&
                  source_bytes <= RIN_MEDIA_CONTAINER_PROBE_MAX_BYTES &&
                  info != NULL && packets != NULL && config != NULL &&
                  output != NULL && frames_out != NULL &&
                  output_capacity_samples <=
                      (size_t)RIN_MEDIA_PCM_MAX_OUTPUT_SAMPLES &&
                  info->struct_size == sizeof(*info) &&
                  info->abi_version == RIN_MEDIA_DEMUX_ABI_V1 &&
                  info->container_id == RIN_MEDIA_CONTAINER_WAV &&
                  info->track_count == 1u &&
                  info->tracks[0].track_id == 1u &&
                  info->tracks[0].kind == RIN_MEDIA_DEMUX_TRACK_AUDIO &&
                  info->tracks[0].time_scale != 0u &&
                  packets->struct_size == sizeof(*packets) &&
                  packets->abi_version == RIN_MEDIA_DEMUX_ABI_V1 &&
                  packets->reserved0 == 0u && packets->packet_count != 0u &&
                  packets->packet_count <= RIN_MEDIA_DEMUX_MAX_PACKETS &&
                  rin_media_pcm_config_valid(config) &&
                  config->sample_rate == info->tracks[0].time_scale &&
                  ((config->format == RIN_MEDIA_PCM_FORMAT_F32LE &&
                    info->tracks[0].codec_id == pcm_wav_float_format) ||
                   (config->format != RIN_MEDIA_PCM_FORMAT_F32LE &&
                    info->tracks[0].codec_id == pcm_wav_format));
    if (!table_valid) return RIN_MEDIA_PCM_INVALID;

    for (packet_index = 0u; packet_index < packets->packet_count;
         ++packet_index) {
        const RinMediaDemuxPacketV1* packet = &packets->packets[packet_index];
        size_t decoded_frames = 0u;
        uint64_t packet_end;
        int result;
        if (packet->track_id != info->tracks[0].track_id ||
            packet->byte_size == 0u ||
            packet->byte_size > RIN_MEDIA_PCM_MAX_INPUT_BYTES ||
            packet->byte_offset > (uint64_t)source_bytes ||
            packet->byte_size > source_bytes -
                                    (size_t)packet->byte_offset ||
            (packet_index != 0u && packet->byte_offset < previous_end) ||
            packet->timestamp_ticks != total_frames ||
            packet->duration_ticks == 0u ||
            UINT64_MAX - packet->byte_offset < packet->byte_size)
            goto malformed;
        packet_end = packet->byte_offset + packet->byte_size;
        previous_end = packet_end;
        if (total_frames > output_capacity_samples)
            goto output_too_small;
        result = rin_media_pcm_decode_s16(
            config, source + (size_t)packet->byte_offset, packet->byte_size,
            output + total_frames * (size_t)config->channels,
            output_capacity_samples -
                total_frames * (size_t)config->channels,
            &decoded_frames);
        if (result != RIN_MEDIA_PCM_OK) {
            pcm_zero(output, output_capacity_samples * sizeof(*output));
            return result;
        }
        if (decoded_frames != packet->duration_ticks ||
            total_frames > SIZE_MAX - decoded_frames ||
            total_frames + decoded_frames > RIN_MEDIA_PCM_MAX_FRAMES *
                                             RIN_MEDIA_DEMUX_MAX_PACKETS)
            goto malformed;
        total_frames += decoded_frames;
    }
    if (info->duration_ticks != 0u &&
        info->duration_ticks != total_frames)
        goto malformed;
    *frames_out = total_frames;
    return RIN_MEDIA_PCM_OK;

output_too_small:
    pcm_zero(output, output_capacity_samples * sizeof(*output));
    return RIN_MEDIA_PCM_OUTPUT_TOO_SMALL;
malformed:
    pcm_zero(output, output_capacity_samples * sizeof(*output));
    return RIN_MEDIA_PCM_INVALID;
}
