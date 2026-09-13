/* SPDX-License-Identifier: MIT */
#include "rinmedia_capabilities.h"

#include <string.h>

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
