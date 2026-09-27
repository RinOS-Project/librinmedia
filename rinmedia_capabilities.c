/* SPDX-License-Identifier: MIT */
#include "rinmedia_capabilities.h"

#include <string.h>

static uint32_t read_be32(const uint8_t* value)
{
    return ((uint32_t)value[0] << 24u) |
           ((uint32_t)value[1] << 16u) |
           ((uint32_t)value[2] << 8u) |
           (uint32_t)value[3];
}

static uint32_t read_le32(const uint8_t* value)
{
    return (uint32_t)value[0] |
           ((uint32_t)value[1] << 8u) |
           ((uint32_t)value[2] << 16u) |
           ((uint32_t)value[3] << 24u);
}

static int ogg_page_size(const uint8_t* data, size_t source_bytes,
                         size_t* page_bytes)
{
    size_t segment_table;
    size_t body_bytes = 0u;
    size_t index;
    if (!data || !page_bytes || source_bytes < 27u ||
        memcmp(data, "OggS", 4u) != 0 || data[4] != 0u)
        return 0;
    segment_table = 27u + (size_t)data[26];
    if (segment_table > source_bytes) return 0;
    for (index = 0u; index < (size_t)data[26]; ++index) {
        if ((size_t)data[27u + index] > source_bytes - segment_table - body_bytes)
            return 0;
        body_bytes += data[27u + index];
    }
    *page_bytes = segment_table + body_bytes;
    return 1;
}

static int fourcc(const uint8_t* value, char a, char b, char c, char d)
{
    return value[0] == (uint8_t)a && value[1] == (uint8_t)b &&
           value[2] == (uint8_t)c && value[3] == (uint8_t)d;
}

static int ebml_vint_length(uint8_t first)
{
    uint8_t mask = 0x80u;
    int length = 1;
    while (length <= 8 && (first & mask) == 0u) {
        mask = (uint8_t)(mask >> 1u);
        ++length;
    }
    return length > 8 ? 0 : length;
}

static uint64_t ebml_vint_value(const uint8_t* data, int length)
{
    uint64_t value = (uint64_t)(data[0] & (uint8_t)(0xffu >> length));
    int index;
    for (index = 1; index < length; ++index)
        value = (value << 8u) | data[index];
    return value;
}

static int ebml_doctype(const uint8_t* data, size_t source_bytes,
                        uint32_t* container_id)
{
    size_t offset;
    size_t header_end;
    int size_length;
    uint64_t header_size;
    if (source_bytes < 5u || read_be32(data) != UINT32_C(0x1a45dfa3))
        return 0;
    size_length = ebml_vint_length(data[4]);
    if (size_length == 0 || source_bytes < 4u + (size_t)size_length)
        return 0;
    header_size = ebml_vint_value(data + 4, size_length);
    offset = 4u + (size_t)size_length;
    if (header_size > (uint64_t)(source_bytes - offset)) return 0;
    header_end = offset + (size_t)header_size;
    while (offset < header_end) {
        uint64_t element_id = 0u;
        uint64_t element_size;
        size_t element_end;
        int id_length = ebml_vint_length(data[offset]);
        int element_size_length;
        int index;
        if (id_length == 0 || (size_t)id_length > header_end - offset)
            return 0;
        for (index = 0; index < id_length; ++index)
            element_id = (element_id << 8u) | data[offset + (size_t)index];
        offset += (size_t)id_length;
        if (offset >= header_end) return 0;
        element_size_length = ebml_vint_length(data[offset]);
        if (element_size_length == 0 ||
            (size_t)element_size_length > header_end - offset)
            return 0;
        element_size = ebml_vint_value(data + offset, element_size_length);
        offset += (size_t)element_size_length;
        if (element_size > (uint64_t)(header_end - offset)) return 0;
        element_end = offset + (size_t)element_size;
        if (element_id == UINT32_C(0x4282)) {
            if (element_size == 4u &&
                memcmp(data + offset, "webm", 4u) == 0) {
                *container_id = RIN_MEDIA_CONTAINER_WEBM;
                return 1;
            }
            if (element_size == 8u &&
                memcmp(data + offset, "matroska", 8u) == 0) {
                *container_id = RIN_MEDIA_CONTAINER_MATROSKA;
                return 1;
            }
            return 0;
        }
        offset = element_end;
    }
    return 0;
}

static int iso_bmff(const uint8_t* data, size_t source_bytes,
                    uint32_t* container_id)
{
    size_t offset = 0u;
    while (offset + 8u <= source_bytes) {
        uint64_t box_size = read_be32(data + offset);
        size_t header_size = 8u;
        size_t box_end;
        if (box_size == 1u) {
            if (offset + 16u > source_bytes) return 0;
            box_size = ((uint64_t)read_be32(data + offset + 8u) << 32u) |
                       read_be32(data + offset + 12u);
            header_size = 16u;
        } else if (box_size == 0u) {
            return 0;
        }
        if (box_size < (uint64_t)header_size ||
            box_size > (uint64_t)(source_bytes - offset)) return 0;
        box_end = offset + (size_t)box_size;
        if (fourcc(data + offset + 4u, 'f', 't', 'y', 'p')) {
            if (box_size < 16u) return 0;
            *container_id = RIN_MEDIA_CONTAINER_MP4;
            return 1;
        }
        offset = box_end;
    }
    return 0;
}

typedef struct RinMediaCapabilityEntry {
    uint32_t codec_id;
    const char* name;
} RinMediaCapabilityEntry;

static const RinMediaCapabilityEntry kCapabilities[] = {
    {RIN_MEDIA_CODEC_WAV, "wav"},
    {RIN_MEDIA_CODEC_MP3, "mp3"},
    {RIN_MEDIA_CODEC_FLAC, "flac"},
    {RIN_MEDIA_CODEC_OGG, "ogg"},
    {RIN_MEDIA_CODEC_OPUS, "opus"},
    {RIN_MEDIA_CODEC_AAC, "aac"},
    {RIN_MEDIA_CODEC_M4A, "m4a"},
};

static size_t bounded_length(const char* value, size_t capacity)
{
    size_t length = 0u;
    if (value == NULL) return 0u;
    while (length < capacity && value[length] != '\0') ++length;
    return length;
}

uint32_t rin_media_capability_count(void)
{
    return (uint32_t)(sizeof(kCapabilities) / sizeof(kCapabilities[0]));
}

int rin_media_capability_get(uint32_t index, RinMediaCapabilityV1* output,
                             size_t output_size)
{
    const RinMediaCapabilityEntry* entry;
    size_t name_bytes;
    if (output == NULL || output_size < sizeof(*output) ||
        index >= rin_media_capability_count()) return -1;
    memset(output, 0, sizeof(*output));
    entry = &kCapabilities[index];
    name_bytes = bounded_length(entry->name, RIN_MEDIA_CAPABILITY_NAME_MAX - 1u);
    if (name_bytes == 0u || name_bytes >= RIN_MEDIA_CAPABILITY_NAME_MAX) {
        memset(output, 0, sizeof(*output));
        return -1;
    }
    output->struct_size = (uint32_t)sizeof(*output);
    output->abi_version = RIN_MEDIA_CAPABILITY_ABI_V1;
    output->codec_id = entry->codec_id;
    memcpy(output->name, entry->name, name_bytes);
    output->name[name_bytes] = '\0';
    return 0;
}

int rin_media_capability_find(const char* name, size_t name_bytes,
                              uint32_t* codec_id)
{
    size_t index;
    if (codec_id != NULL) *codec_id = 0u;
    if (name == NULL || codec_id == NULL || name_bytes == 0u ||
        name_bytes >= RIN_MEDIA_CAPABILITY_NAME_MAX) return -1;
    for (index = 0u; index < sizeof(kCapabilities) / sizeof(kCapabilities[0]); ++index) {
        size_t entry_bytes = bounded_length(kCapabilities[index].name,
                                            RIN_MEDIA_CAPABILITY_NAME_MAX - 1u);
        if (entry_bytes == name_bytes &&
            memcmp(name, kCapabilities[index].name, name_bytes) == 0) {
            *codec_id = kCapabilities[index].codec_id;
            return 0;
        }
    }
    return -1;
}

int rin_media_container_probe(const uint8_t* data, size_t source_bytes,
                              uint32_t* container_id)
{
    if (container_id != NULL) *container_id = 0u;
    if (data == NULL || container_id == NULL || source_bytes == 0u ||
        source_bytes > RIN_MEDIA_CONTAINER_PROBE_MAX_BYTES)
        return -1;
    if (source_bytes >= 4u && memcmp(data, "fLaC", 4u) == 0) {
        *container_id = RIN_MEDIA_CONTAINER_FLAC;
        return 0;
    }
    if (source_bytes >= 4u && memcmp(data, "OggS", 4u) == 0) {
        size_t page_bytes;
        if (!ogg_page_size(data, source_bytes, &page_bytes)) return -1;
        *container_id = RIN_MEDIA_CONTAINER_OGG;
        return 0;
    }
    if (source_bytes >= 12u && fourcc(data, 'R', 'I', 'F', 'F')) {
        /* RIFF size includes the form type and all bytes following it. */
        uint32_t riff_size = read_le32(data + 4u);
        if (riff_size < 4u ||
            (uint64_t)riff_size > (uint64_t)(source_bytes - 8u))
            return -1;
        if (fourcc(data + 8u, 'A', 'V', 'I', ' ')) {
            *container_id = RIN_MEDIA_CONTAINER_AVI;
            return 0;
        }
        if (fourcc(data + 8u, 'W', 'A', 'V', 'E')) {
            *container_id = RIN_MEDIA_CONTAINER_WAV;
            return 0;
        }
        return -1;
    }
    if (ebml_doctype(data, source_bytes, container_id) != 0) return 0;
    if (iso_bmff(data, source_bytes, container_id) != 0) return 0;
    *container_id = 0u;
    return -1;
}
