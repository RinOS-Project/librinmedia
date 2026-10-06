/* SPDX-License-Identifier: MIT */
#ifndef RIN_MEDIA_PCM_H
#define RIN_MEDIA_PCM_H

#if defined(RIN_FREESTANDING)
#include "../libc/stddef.h"
#include "../libc/stdint.h"
#else
#include <stddef.h>
#include <stdint.h>
#endif

#include "rinmedia_demux.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RIN_MEDIA_PCM_ABI_V1 UINT32_C(1)
#define RIN_MEDIA_PCM_MAX_CHANNELS UINT32_C(32)
#define RIN_MEDIA_PCM_MAX_FRAMES UINT32_C(65536)
#define RIN_MEDIA_PCM_MAX_INPUT_BYTES (UINT32_C(1024) * UINT32_C(1024))
#define RIN_MEDIA_PCM_MAX_OUTPUT_SAMPLES \
    (RIN_MEDIA_PCM_MAX_CHANNELS * RIN_MEDIA_PCM_MAX_FRAMES * \
     RIN_MEDIA_DEMUX_MAX_PACKETS)

typedef enum RinMediaPcmFormatV1 {
    RIN_MEDIA_PCM_FORMAT_U8 = 1,
    RIN_MEDIA_PCM_FORMAT_S8 = 2,
    RIN_MEDIA_PCM_FORMAT_S16LE = 3,
    RIN_MEDIA_PCM_FORMAT_S24LE = 4,
    RIN_MEDIA_PCM_FORMAT_S32LE = 5,
    RIN_MEDIA_PCM_FORMAT_F32LE = 6
} RinMediaPcmFormatV1;

typedef enum RinMediaPcmStatusV1 {
    RIN_MEDIA_PCM_OK = 0,
    RIN_MEDIA_PCM_INVALID = -1,
    RIN_MEDIA_PCM_OUTPUT_TOO_SMALL = -2,
    RIN_MEDIA_PCM_UNSUPPORTED = -3,
    RIN_MEDIA_PCM_NONFINITE = -4,
    RIN_MEDIA_PCM_LIMIT = -5
} RinMediaPcmStatusV1;

typedef struct RinMediaPcmConfigV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t format;
    uint32_t flags;
    uint32_t reserved[2];
} RinMediaPcmConfigV1;

/* Validate the bounded, interleaved PCM format descriptor. */
int rin_media_pcm_config_valid(const RinMediaPcmConfigV1* config);

/* Decode one complete interleaved packet to caller-owned signed 16-bit
 * samples.  The output is zeroed before every call and remains zero on every
 * failure.  input_bytes must contain whole frames; partial frames are not
 * silently truncated. */
int rin_media_pcm_decode_s16(
    const RinMediaPcmConfigV1* config, const uint8_t* input,
    size_t input_bytes, int16_t* output, size_t output_capacity_samples,
    size_t* frames_out);

/* Decode every packet in a caller-owned, validated-shaped WAV packet table
 * into one interleaved signed 16-bit span.  This public adapter accepts only
 * WAV PCM or IEEE-float packet tables; ADPCM and other WAV codecs remain
 * explicit unsupported boundaries.  It performs no filesystem, descriptor,
 * service, or backend work.  Output and frames_out are failure-atomic. */
int rin_media_pcm_decode_packet_table_s16(
    const uint8_t* source, size_t source_bytes,
    const RinMediaDemuxInfoV1* info,
    const RinMediaDemuxPacketTableV1* packets,
    const RinMediaPcmConfigV1* config, int16_t* output,
    size_t output_capacity_samples, size_t* frames_out);

#ifdef __cplusplus
}
#endif

#endif /* RIN_MEDIA_PCM_H */
