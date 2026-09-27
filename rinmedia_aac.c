/* SPDX-License-Identifier: MIT */
#include "rinmedia_aac.h"

#include <string.h>

typedef struct RinMediaAacBitReader {
    const uint8_t* data;
    size_t bits;
    size_t offset;
} RinMediaAacBitReader;

static void clear_output(RinMediaAacConfigV1* output, size_t output_size)
{
    if (output != NULL && output_size != 0u)
        memset(output, 0, output_size < sizeof(*output) ? output_size :
               sizeof(*output));
}

static int read_bits(RinMediaAacBitReader* reader, unsigned count,
                     uint32_t* value)
{
    uint32_t result = 0u;
    unsigned index;
    if (reader == NULL || value == NULL || count == 0u || count > 24u ||
        reader->offset > reader->bits || (size_t)count >
            reader->bits - reader->offset)
        return 0;
    for (index = 0u; index < count; ++index) {
        const size_t bit = reader->offset++;
        result = (result << 1u) |
                 ((uint32_t)(reader->data[bit / 8u] >>
                  (7u - (unsigned)(bit % 8u))) & 1u);
    }
    *value = result;
    return 1;
}

static int read_audio_object_type(RinMediaAacBitReader* reader,
                                  uint32_t* object_type)
{
    uint32_t value = 0u;
    uint32_t extension = 0u;
    if (!read_bits(reader, 5u, &value)) return 0;
    if (value == 31u) {
        if (!read_bits(reader, 6u, &extension) || extension == 0u)
            return 0;
        value = 32u + extension;
    }
    if (value == 0u || value > 95u) return 0;
    *object_type = value;
    return 1;
}

static int read_sample_rate(RinMediaAacBitReader* reader,
                            uint32_t* sample_rate)
{
    static const uint32_t rates[] = {
        96000u, 88200u, 64000u, 48000u, 44100u, 32000u, 24000u,
        22050u, 16000u, 12000u, 11025u, 8000u, 7350u
    };
    uint32_t index = 0u;
    uint32_t explicit_rate = 0u;
    if (!read_bits(reader, 4u, &index)) return 0;
    if (index < sizeof(rates) / sizeof(rates[0])) {
        *sample_rate = rates[index];
        return 1;
    }
    if (index != 15u || !read_bits(reader, 24u, &explicit_rate) ||
        explicit_rate == 0u || explicit_rate > 384000u)
        return 0;
    *sample_rate = explicit_rate;
    return 1;
}

static int channel_count(uint32_t configuration, uint32_t* count)
{
    static const uint32_t channels[] = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 8u};
    if (count == NULL || configuration == 0u || configuration > 7u)
        return 0;
    *count = channels[configuration];
    return 1;
}

int rin_media_aac_config_inspect(const uint8_t* data, size_t source_bytes,
                                RinMediaAacConfigV1* output,
                                size_t output_size)
{
    RinMediaAacBitReader reader;
    RinMediaAacConfigV1 candidate = {0};
    uint32_t object_type = 0u;
    uint32_t sample_rate = 0u;
    uint32_t channel_configuration = 0u;
    uint32_t channels = 0u;
    int extended = 0;

    clear_output(output, output_size);
    if (output == NULL || output_size < sizeof(*output) || data == NULL ||
        source_bytes == 0u)
        return RIN_MEDIA_AAC_CONFIG_INVALID;
    if (source_bytes > RIN_MEDIA_AAC_CONFIG_MAX_BYTES)
        return RIN_MEDIA_AAC_CONFIG_LIMIT;

    reader.data = data;
    reader.bits = source_bytes * 8u;
    reader.offset = 0u;
    if (!read_audio_object_type(&reader, &object_type))
        return RIN_MEDIA_AAC_CONFIG_UNSUPPORTED;
    if (object_type == 5u || object_type == 29u) {
        candidate.extension_audio_object_type = object_type;
        candidate.sbr_present = 1u;
        candidate.ps_present = object_type == 29u ? 1u : 0u;
        extended = 1;
        if (!read_sample_rate(&reader, &candidate.extension_sample_rate_hz) ||
            !read_audio_object_type(&reader, &object_type))
            return RIN_MEDIA_AAC_CONFIG_UNSUPPORTED;
    }
    if (!read_sample_rate(&reader, &sample_rate) ||
        !read_bits(&reader, 4u, &channel_configuration) ||
        !channel_count(channel_configuration, &channels))
        return RIN_MEDIA_AAC_CONFIG_UNSUPPORTED;

    candidate.struct_size = sizeof(candidate);
    candidate.abi_version = RIN_MEDIA_AAC_CONFIG_ABI_V1;
    candidate.audio_object_type = object_type;
    candidate.sample_rate_hz = sample_rate;
    candidate.channel_configuration = channel_configuration;
    candidate.channel_count = channels;
    if (!extended) {
        candidate.extension_audio_object_type = 0u;
        candidate.extension_sample_rate_hz = 0u;
        candidate.sbr_present = 0u;
        candidate.ps_present = 0u;
    }
    *output = candidate;
    return RIN_MEDIA_AAC_CONFIG_OK;
}
