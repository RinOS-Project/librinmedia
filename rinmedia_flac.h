/* SPDX-License-Identifier: MIT */
#ifndef RIN_MEDIA_FLAC_H
#define RIN_MEDIA_FLAC_H

#include <stddef.h>
#include <stdint.h>

#include "rinmedia_demux.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RIN_MEDIA_FLAC_ABI_V1 1u
#define RIN_MEDIA_FLAC_MAX_CHANNELS 8u
#define RIN_MEDIA_FLAC_MAX_BLOCK_SAMPLES 65536u
#define RIN_MEDIA_FLAC_MAX_FRAME_BYTES (1024u * 1024u)
#define RIN_MEDIA_FLAC_MAX_OUTPUT_SAMPLES \
    (RIN_MEDIA_FLAC_MAX_CHANNELS * RIN_MEDIA_FLAC_MAX_BLOCK_SAMPLES * \
     RIN_MEDIA_DEMUX_MAX_PACKETS)

enum {
    RIN_MEDIA_FLAC_DECODE_OK = 0,
    RIN_MEDIA_FLAC_DECODE_INVALID = -1,
    RIN_MEDIA_FLAC_DECODE_MALFORMED = -2,
    RIN_MEDIA_FLAC_DECODE_UNSUPPORTED = -3,
    RIN_MEDIA_FLAC_DECODE_OUTPUT_TOO_SMALL = -4,
    RIN_MEDIA_FLAC_DECODE_LIMIT = -5
};

/* Decoded samples are signed, interleaved, native-endian int32 values.  The
 * frame header and this caller-owned stream description must agree on the
 * channel count and bits per sample.  scratch must hold at least
 * channels * block_samples int64_t values; output is cleared on failure when
 * output_samples is within the published maximum.  No allocation, path,
 * descriptor, or service ownership is part of this API. */
typedef struct RinMediaFlacDecodeRequestV1 {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved0;
    uint32_t channels;
    uint32_t bits_per_sample;
    uint32_t reserved1;
} RinMediaFlacDecodeRequestV1;

int rin_media_flac_decode_frame(
    const uint8_t* frame, size_t frame_bytes,
    const RinMediaFlacDecodeRequestV1* request, int64_t* scratch,
    size_t scratch_samples, int32_t* output,
    size_t output_samples, size_t* samples_written,
    uint32_t* block_samples_out);

/* Decode every packet in a validated, caller-owned FLAC packet table into one
 * interleaved signed-sample span.  The packet table is the public demux
 * source view; this helper performs no filesystem, descriptor, service, or
 * codec-backend work.  Output and samples_written are failure-atomic, and a
 * packet duration must agree with the decoded block size before any samples
 * are published. */
int rin_media_flac_decode_packet_table(
    const uint8_t* source, size_t source_bytes,
    const RinMediaDemuxInfoV1* info,
    const RinMediaDemuxPacketTableV1* packets,
    const RinMediaFlacDecodeRequestV1* request, int64_t* scratch,
    size_t scratch_samples, int32_t* output, size_t output_samples,
    size_t* samples_written);

#ifdef __cplusplus
}
#endif

#endif /* RIN_MEDIA_FLAC_H */
