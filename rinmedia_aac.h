/* SPDX-License-Identifier: MIT */
#ifndef RIN_MEDIA_AAC_H
#define RIN_MEDIA_AAC_H

#include "rinmedia_capabilities.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RIN_MEDIA_AAC_CONFIG_ABI_V1 1u
#define RIN_MEDIA_AAC_CONFIG_MAX_BYTES 32u

enum {
    RIN_MEDIA_AAC_CONFIG_OK = 0,
    RIN_MEDIA_AAC_CONFIG_INVALID = -1,
    RIN_MEDIA_AAC_CONFIG_LIMIT = -2,
    RIN_MEDIA_AAC_CONFIG_UNSUPPORTED = -3
};

/* Bounded MPEG-4 AudioSpecificConfig metadata.  This is an allocation-free
 * admission record for AAC owners; it does not decode AAC payloads or own
 * the caller's configuration bytes. */
typedef struct RinMediaAacConfigV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t audio_object_type;
    uint32_t extension_audio_object_type;
    uint32_t sample_rate_hz;
    uint32_t extension_sample_rate_hz;
    uint32_t channel_configuration;
    uint32_t channel_count;
    uint32_t sbr_present;
    uint32_t ps_present;
    uint32_t reserved[2];
} RinMediaAacConfigV1;

int rin_media_aac_config_inspect(const uint8_t* data, size_t source_bytes,
                                 RinMediaAacConfigV1* output,
                                 size_t output_size);

#ifdef __cplusplus
}
#endif

#endif /* RIN_MEDIA_AAC_H */
