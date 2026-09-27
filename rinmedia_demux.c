/* SPDX-License-Identifier: MIT */

#include "rinmedia_demux.h"

#include <string.h>

typedef struct BoxView {
    size_t payload;
    size_t end;
    uint32_t type;
} BoxView;

static uint32_t read_be32(const uint8_t* value)
{
    return ((uint32_t)value[0] << 24u) |
           ((uint32_t)value[1] << 16u) |
           ((uint32_t)value[2] << 8u) |
           (uint32_t)value[3];
}

static uint64_t read_be64(const uint8_t* value)
{
    return ((uint64_t)read_be32(value) << 32u) | read_be32(value + 4u);
}

static int bounded_text(const uint8_t* value, size_t length, size_t capacity)
{
    size_t index;
    if (!value || length == 0u || length >= capacity) return 0;
    for (index = 0u; index < length; ++index)
        if (value[index] == 0u) return 0;
    return 1;
}

static uint32_t fourcc_value(const uint8_t* value)
{
    return read_be32(value);
}

static uint32_t text_hash(const uint8_t* value, size_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    size_t index;
    for (index = 0u; index < length; ++index) {
        hash ^= value[index];
        hash *= UINT32_C(16777619);
    }
    return hash == 0u ? 1u : hash;
}

static int iso_box(const uint8_t* data, size_t limit, size_t offset,
                   BoxView* output)
{
    uint64_t size;
    size_t header_size = 8u;
    if (!data || !output || offset > limit || limit - offset < 8u)
        return 0;
    size = read_be32(data + offset);
    output->type = read_be32(data + offset + 4u);
    if (size == 1u) {
        if (limit - offset < 16u) return 0;
        size = read_be64(data + offset + 8u);
        header_size = 16u;
    } else if (size == 0u) {
        return 0;
    }
    if (size < (uint64_t)header_size || size > (uint64_t)(limit - offset))
        return 0;
    output->payload = offset + header_size;
    output->end = offset + (size_t)size;
    return 1;
}

static uint32_t mp4_track_id(const uint8_t* data, size_t length)
{
    if (!data || length < 16u) return 0u;
    if (data[0] == 0u) return read_be32(data + 12u);
    if (data[0] == 1u && length >= 24u) return read_be32(data + 20u);
    return 0u;
}

static int mp4_media_header(const uint8_t* data, size_t length,
                            uint32_t* time_scale, uint64_t* duration)
{
    if (!data || !time_scale || !duration || length < 20u) return 0;
    if (data[0] == 0u) {
        *time_scale = read_be32(data + 12u);
        *duration = read_be32(data + 16u);
    } else if (data[0] == 1u && length >= 32u) {
        *time_scale = read_be32(data + 20u);
        *duration = read_be64(data + 24u);
    } else {
        return 0;
    }
    return *time_scale != 0u;
}

static int mp4_handler_kind(const uint8_t* data, size_t length,
                            uint32_t* kind)
{
    uint32_t handler;
    if (!data || !kind || length < 12u) return 0;
    handler = read_be32(data + 8u);
    if (handler == UINT32_C(0x736f756e)) { /* soun */
        *kind = RIN_MEDIA_DEMUX_TRACK_AUDIO;
    } else if (handler == UINT32_C(0x76696465)) { /* vide */
        *kind = RIN_MEDIA_DEMUX_TRACK_VIDEO;
    } else if (handler == UINT32_C(0x73756274) || /* subt */
               handler == UINT32_C(0x74657874) || /* text */
               handler == UINT32_C(0x636c6370)) { /* clcp */
        *kind = RIN_MEDIA_DEMUX_TRACK_SUBTITLE;
    } else {
        *kind = RIN_MEDIA_DEMUX_TRACK_UNKNOWN;
    }
    return 1;
}

static int mp4_sample_entry(const uint8_t* data, size_t length,
                            uint32_t* codec_id, char* codec_name)
{
    uint32_t entry_size;
    if (!data || !codec_id || !codec_name || length < 8u) return 0;
    entry_size = read_be32(data);
    if (entry_size < 8u || entry_size > length ||
        !bounded_text(data + 4u, 4u, RIN_MEDIA_DEMUX_CODEC_NAME_MAX))
        return 0;
    *codec_id = fourcc_value(data + 4u);
    memcpy(codec_name, data + 4u, 4u);
    codec_name[4] = '\0';
    return 1;
}

static int mp4_find_track_fields(const uint8_t* data, size_t begin, size_t end,
                                 RinMediaDemuxTrackV1* track, int depth)
{
    size_t offset = begin;
    if (!data || !track || depth > 8) return 0;
    while (offset < end) {
        BoxView box;
        if (!iso_box(data, end, offset, &box)) return 0;
        if (box.type == UINT32_C(0x746b6864)) { /* tkhd */
            track->track_id = mp4_track_id(data + box.payload,
                                           box.end - box.payload);
        } else if (box.type == UINT32_C(0x6d646864)) { /* mdhd */
            if (!mp4_media_header(data + box.payload, box.end - box.payload,
                                  &track->time_scale,
                                  &track->duration_ticks))
                return 0;
        } else if (box.type == UINT32_C(0x68646c72)) { /* hdlr */
            if (!mp4_handler_kind(data + box.payload, box.end - box.payload,
                                  &track->kind))
                return 0;
        } else if (box.type == UINT32_C(0x73747364)) { /* stsd */
            const uint8_t* payload = data + box.payload;
            size_t length = box.end - box.payload;
            if (length < 16u || read_be32(payload + 4u) == 0u ||
                !mp4_sample_entry(payload + 8u, length - 8u,
                                  &track->codec_id, track->codec_name))
                return 0;
        } else if (!mp4_find_track_fields(data, box.payload, box.end, track,
                                          depth + 1)) {
            /* Unknown leaf payloads are not containers.  Only reject a
             * malformed box when it advertises a non-empty nested payload. */
            if (box.payload != box.end && depth < 3) return 0;
        }
        offset = box.end;
    }
    return offset == end;
}

static int mp4_inspect(const uint8_t* data, size_t source_bytes,
                       RinMediaDemuxInfoV1* output)
{
    size_t offset = 0u;
    int found_ftyp = 0;
    int found_moov = 0;
    while (offset < source_bytes) {
        BoxView box;
        if (!iso_box(data, source_bytes, offset, &box))
            return RIN_MEDIA_DEMUX_INVALID;
        if (box.type == UINT32_C(0x66747970)) { /* ftyp */
            if (box.end - box.payload < 8u) return RIN_MEDIA_DEMUX_INVALID;
            found_ftyp = 1;
        } else if (box.type == UINT32_C(0x6d6f6f76)) { /* moov */
            size_t child = box.payload;
            found_moov = 1;
            while (child < box.end) {
                BoxView trak_box;
                RinMediaDemuxTrackV1 track;
                if (!iso_box(data, box.end, child, &trak_box))
                    return RIN_MEDIA_DEMUX_INVALID;
                if (trak_box.type == UINT32_C(0x7472616b)) { /* trak */
                    if (output->track_count >= RIN_MEDIA_DEMUX_MAX_TRACKS)
                        return RIN_MEDIA_DEMUX_UNSUPPORTED;
                    memset(&track, 0, sizeof(track));
                    if (!mp4_find_track_fields(data, trak_box.payload,
                                               trak_box.end, &track, 0) ||
                        track.track_id == 0u || track.kind == 0u ||
                        track.codec_id == 0u || track.time_scale == 0u)
                        return RIN_MEDIA_DEMUX_UNSUPPORTED;
                    if (output->track_count == 0u) {
                        output->time_scale = track.time_scale;
                        output->duration_ticks = track.duration_ticks;
                    }
                    output->tracks[output->track_count++] = track;
                }
                child = trak_box.end;
            }
        }
        offset = box.end;
    }
    if (!found_ftyp || !found_moov || output->track_count == 0u)
        return RIN_MEDIA_DEMUX_UNSUPPORTED;
    output->container_id = RIN_MEDIA_CONTAINER_MP4;
    return RIN_MEDIA_DEMUX_OK;
}

static int ebml_vint(const uint8_t* data, size_t length, uint64_t* value,
                     size_t* bytes, int* unknown)
{
    uint8_t mask = 0x80u;
    size_t count = 1u;
    uint64_t result;
    size_t index;
    if (!data || !value || !bytes || !unknown || length == 0u) return 0;
    while (count <= 8u && (data[0] & mask) == 0u) {
        mask = (uint8_t)(mask >> 1u);
        ++count;
    }
    if (count > 8u || length < count) return 0;
    result = (uint64_t)(data[0] & (uint8_t)(0xffu >> count));
    for (index = 1u; index < count; ++index)
        result = (result << 8u) | data[index];
    *value = result;
    *bytes = count;
    *unknown = result == ((UINT64_C(1) << (7u * count)) - 1u);
    return 1;
}

static int ebml_element(const uint8_t* data, size_t limit, size_t offset,
                        uint64_t* id, size_t* payload, size_t* end)
{
    uint64_t raw_id;
    uint64_t size;
    size_t id_bytes;
    size_t size_bytes;
    int ignored_unknown;
    int unknown_size;
    if (!data || !id || !payload || !end || offset >= limit ||
        !ebml_vint(data + offset, limit - offset, &raw_id, &id_bytes,
                   &ignored_unknown) ||
        !ebml_vint(data + offset + id_bytes, limit - offset - id_bytes,
                   &size, &size_bytes, &unknown_size))
        return 0;
    *payload = offset + id_bytes + size_bytes;
    if (unknown_size) {
        *end = limit;
    } else {
        if (size > (uint64_t)(limit - *payload)) return 0;
        *end = *payload + (size_t)size;
    }
    *id = 0u;
    for (size_t index = 0u; index < id_bytes; ++index)
        *id = (*id << 8u) | data[offset + index];
    return 1;
}

static uint64_t ebml_uint(const uint8_t* data, size_t length)
{
    uint64_t value = 0u;
    size_t index;
    for (index = 0u; index < length; ++index)
        value = (value << 8u) | data[index];
    return value;
}

static int ebml_copy_codec(const uint8_t* data, size_t length,
                           RinMediaDemuxTrackV1* track)
{
    if (!bounded_text(data, length, RIN_MEDIA_DEMUX_CODEC_NAME_MAX))
        return 0;
    memcpy(track->codec_name, data, length);
    track->codec_name[length] = '\0';
    track->codec_id = text_hash(data, length);
    return 1;
}

static int ebml_track(const uint8_t* data, size_t begin, size_t end,
                      RinMediaDemuxTrackV1* track)
{
    size_t offset = begin;
    int got_number = 0;
    int got_type = 0;
    int got_codec = 0;
    while (offset < end) {
        uint64_t id;
        size_t payload;
        size_t element_end;
        if (!ebml_element(data, end, offset, &id, &payload, &element_end))
            return 0;
        if (id == UINT64_C(0xd7)) {
            if (element_end - payload == 0u || element_end - payload > 8u)
                return 0;
            track->track_id = (uint32_t)ebml_uint(data + payload,
                                                  element_end - payload);
            got_number = track->track_id != 0u;
        } else if (id == UINT64_C(0x83)) {
            uint64_t track_type;
            if (element_end - payload == 0u || element_end - payload > 8u)
                return 0;
            track_type = ebml_uint(data + payload, element_end - payload);
            track->kind = track_type == 1u ? RIN_MEDIA_DEMUX_TRACK_VIDEO :
                          track_type == 2u ? RIN_MEDIA_DEMUX_TRACK_AUDIO :
                          track_type == 17u ? RIN_MEDIA_DEMUX_TRACK_SUBTITLE :
                          RIN_MEDIA_DEMUX_TRACK_UNKNOWN;
            got_type = track->kind != RIN_MEDIA_DEMUX_TRACK_UNKNOWN;
        } else if (id == UINT64_C(0x86)) {
            got_codec = ebml_copy_codec(data + payload,
                                        element_end - payload, track);
        }
        offset = element_end;
    }
    return offset == end && got_number && got_type && got_codec;
}

static int webm_find_tracks(const uint8_t* data, size_t begin, size_t end,
                            RinMediaDemuxInfoV1* output, int depth)
{
    size_t offset = begin;
    if (depth > 8) return 0;
    while (offset < end) {
        uint64_t id;
        size_t payload;
        size_t element_end;
        if (!ebml_element(data, end, offset, &id, &payload, &element_end))
            return 0;
        if (id == UINT64_C(0x1654ae6b)) { /* Tracks */
            size_t track_offset = payload;
            while (track_offset < element_end) {
                uint64_t track_id;
                size_t track_payload;
                size_t track_end;
                RinMediaDemuxTrackV1 track;
                if (!ebml_element(data, element_end, track_offset, &track_id,
                                  &track_payload, &track_end))
                    return 0;
                if (track_id == UINT64_C(0xae)) { /* TrackEntry */
                    if (output->track_count >= RIN_MEDIA_DEMUX_MAX_TRACKS)
                        return 0;
                    memset(&track, 0, sizeof(track));
                    if (!ebml_track(data, track_payload, track_end, &track))
                        return 0;
                    output->tracks[output->track_count++] = track;
                }
                track_offset = track_end;
            }
        } else if (id == UINT64_C(0x1549a966)) { /* Info */
            size_t info_offset = payload;
            while (info_offset < element_end) {
                uint64_t info_id;
                size_t info_payload;
                size_t info_end;
                if (!ebml_element(data, element_end, info_offset, &info_id,
                                  &info_payload, &info_end))
                    return 0;
                if (info_id == UINT64_C(0x2ad7b1) &&
                    info_end - info_payload > 0u &&
                    info_end - info_payload <= 4u) {
                    uint64_t scale = ebml_uint(data + info_payload,
                                               info_end - info_payload);
                    if (scale == 0u || scale > UINT32_MAX) return 0;
                    output->time_scale = (uint32_t)scale;
                }
                info_offset = info_end;
            }
        } else if (id == UINT64_C(0x18538067)) { /* Segment */
            if (!webm_find_tracks(data, payload, element_end, output,
                                  depth + 1))
                return 0;
        }
        offset = element_end;
    }
    return offset == end;
}

static int webm_inspect(const uint8_t* data, size_t source_bytes,
                        RinMediaDemuxInfoV1* output)
{
    uint32_t container = 0u;
    uint64_t id;
    size_t header_end;
    size_t offset;
    int found_segment = 0;
    if (rin_media_container_probe(data, source_bytes, &container) != 0 ||
        (container != RIN_MEDIA_CONTAINER_WEBM &&
         container != RIN_MEDIA_CONTAINER_MATROSKA))
        return RIN_MEDIA_DEMUX_INVALID;
    if (!ebml_element(data, source_bytes, 0u, &id, &offset, &header_end) ||
        id != UINT64_C(0x1a45dfa3))
        return RIN_MEDIA_DEMUX_INVALID;
    offset = header_end;
    while (offset < source_bytes) {
        size_t payload;
        size_t end;
        if (!ebml_element(data, source_bytes, offset, &id, &payload, &end))
            return RIN_MEDIA_DEMUX_INVALID;
        if (id == UINT64_C(0x18538067)) {
            found_segment = 1;
            if (!webm_find_tracks(data, payload, end, output, 0))
                return RIN_MEDIA_DEMUX_INVALID;
            break;
        }
        offset = end;
    }
    if (!found_segment || output->track_count == 0u)
        return RIN_MEDIA_DEMUX_UNSUPPORTED;
    if (output->time_scale == 0u) output->time_scale = 1000000u;
    output->container_id = container;
    return RIN_MEDIA_DEMUX_OK;
}

int rin_media_container_inspect(const uint8_t* data, size_t source_bytes,
                                RinMediaDemuxInfoV1* output,
                                size_t output_size)
{
    int result;
    if (output != NULL) memset(output, 0, sizeof(*output));
    if (data == NULL || source_bytes == 0u ||
        source_bytes > RIN_MEDIA_CONTAINER_PROBE_MAX_BYTES || output == NULL)
        return RIN_MEDIA_DEMUX_INVALID;
    if (output_size < sizeof(*output)) return RIN_MEDIA_DEMUX_OUTPUT_TOO_SMALL;
    output->struct_size = sizeof(*output);
    output->abi_version = RIN_MEDIA_DEMUX_ABI_V1;
    if (source_bytes >= 8u &&
        read_be32(data + 4u) == UINT32_C(0x66747970))
        result = mp4_inspect(data, source_bytes, output);
    else
        result = webm_inspect(data, source_bytes, output);
    if (result != RIN_MEDIA_DEMUX_OK) memset(output, 0, sizeof(*output));
    return result;
}
