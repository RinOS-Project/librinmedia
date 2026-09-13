/* SPDX-License-Identifier: MIT */
#ifndef RIN_MEDIA_CAPABILITIES_H
#define RIN_MEDIA_CAPABILITIES_H

#if defined(RIN_FREESTANDING)
#include "../libc/stddef.h"
#include "../libc/stdint.h"
#else
#include <stddef.h>
#include <stdint.h>
#endif

#define RIN_MEDIA_CAPABILITY_ABI_V1 1u
#define RIN_MEDIA_CAPABILITY_NAME_MAX 16u

enum {
    RIN_MEDIA_CODEC_WAV = 1u,
    RIN_MEDIA_CODEC_MP3 = 2u,
    RIN_MEDIA_CODEC_FLAC = 3u,
    RIN_MEDIA_CODEC_OGG = 4u,
    RIN_MEDIA_CODEC_OPUS = 5u,
    RIN_MEDIA_CODEC_AAC = 6u,
    RIN_MEDIA_CODEC_M4A = 7u,
};

typedef struct RinMediaCapabilityV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t codec_id;
    uint32_t flags;
    char name[RIN_MEDIA_CAPABILITY_NAME_MAX];
} RinMediaCapabilityV1;

/* Stable, allocation-free capability enumeration for media-framework clients. */
uint32_t rin_media_capability_count(void);
int rin_media_capability_get(uint32_t index, RinMediaCapabilityV1* output,
                             size_t output_size);
int rin_media_capability_find(const char* name, size_t name_bytes,
                              uint32_t* codec_id);

#endif
