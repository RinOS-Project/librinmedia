/* SPDX-License-Identifier: MIT */
#ifndef RIN_MEDIA_DEMUX_H
#define RIN_MEDIA_DEMUX_H

#include "rinmedia_capabilities.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RIN_MEDIA_DEMUX_ABI_V1 1u
#define RIN_MEDIA_DEMUX_MAX_TRACKS 32u
#define RIN_MEDIA_DEMUX_CODEC_NAME_MAX 32u

enum {
    RIN_MEDIA_DEMUX_OK = 0,
    RIN_MEDIA_DEMUX_INVALID = -1,
    RIN_MEDIA_DEMUX_UNSUPPORTED = -2,
    RIN_MEDIA_DEMUX_OUTPUT_TOO_SMALL = -3
};

enum {
    RIN_MEDIA_DEMUX_TRACK_UNKNOWN = 0u,
    RIN_MEDIA_DEMUX_TRACK_AUDIO = 1u,
    RIN_MEDIA_DEMUX_TRACK_VIDEO = 2u,
    RIN_MEDIA_DEMUX_TRACK_SUBTITLE = 3u
};

/* ISO-BMFF codec_id is the big-endian fourcc.  EBML codec_id is the stable
 * FNV-1a hash of the UTF-8 CodecID string; codec_name always carries the
 * bounded spelling for callers that do not use a numeric dispatch table. */
typedef struct RinMediaDemuxTrackV1 {
    uint32_t track_id;
    uint32_t kind;
    uint32_t codec_id;
    uint32_t time_scale;
    uint64_t duration_ticks;
    char codec_name[RIN_MEDIA_DEMUX_CODEC_NAME_MAX];
} RinMediaDemuxTrackV1;

typedef struct RinMediaDemuxInfoV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t container_id;
    uint32_t track_count;
    uint32_t time_scale;
    uint32_t reserved0;
    uint64_t duration_ticks;
    RinMediaDemuxTrackV1 tracks[RIN_MEDIA_DEMUX_MAX_TRACKS];
} RinMediaDemuxInfoV1;

/* Inspect bounded container metadata only.  Packet bytes, filesystem paths,
 * codec state, and backend-specific ownership are not part of this API. */
int rin_media_container_inspect(const uint8_t* data, size_t source_bytes,
                                RinMediaDemuxInfoV1* output,
                                size_t output_size);

#ifdef __cplusplus
}
#endif

#endif /* RIN_MEDIA_DEMUX_H */
