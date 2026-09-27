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

typedef struct Mp4StscEntry {
    uint32_t first_chunk;
    uint32_t samples_per_chunk;
    uint32_t sample_description_index;
} Mp4StscEntry;

typedef struct Mp4SampleTables {
    uint32_t sample_count;
    uint32_t sample_size;
    uint32_t sample_sizes[RIN_MEDIA_DEMUX_MAX_PACKETS];
    uint32_t sample_durations[RIN_MEDIA_DEMUX_MAX_PACKETS];
    int64_t composition_offsets[RIN_MEDIA_DEMUX_MAX_PACKETS];
    uint64_t chunk_offsets[RIN_MEDIA_DEMUX_MAX_PACKETS];
    Mp4StscEntry stsc[64u];
    uint32_t chunk_count;
    uint32_t stsc_count;
    int has_stsz;
    int has_stts;
    int has_stco;
    int has_stsc;
    int has_stss;
    int has_ctts;
    uint8_t sync_samples[RIN_MEDIA_DEMUX_MAX_PACKETS];
} Mp4SampleTables;

static int mp4_table_count(const uint8_t* data, size_t length,
                           uint32_t* count)
{
    if (!data || !count || length < 8u) return 0;
    *count = read_be32(data + 4u);
    return 1;
}

static int mp4_parse_sample_table(const uint8_t* data, size_t length,
                                  uint32_t type, Mp4SampleTables* tables)
{
    uint32_t count;
    size_t index;
    if (!data || !tables) return -1;
    if (type == UINT32_C(0x7374737a)) { /* stsz */
        if (length < 12u || tables->has_stsz ||
            (count = read_be32(data + 8u)) > RIN_MEDIA_DEMUX_MAX_PACKETS)
            return -1;
        if (tables->sample_count != 0u && tables->sample_count != count)
            return -1;
        tables->sample_size = read_be32(data + 4u);
        tables->sample_count = count;
        if (tables->sample_size == 0u) {
            if (length - 12u < (size_t)count * 4u) return -1;
            for (index = 0u; index < count; ++index)
                tables->sample_sizes[index] = read_be32(data + 12u + index * 4u);
        }
        tables->has_stsz = 1;
        return 1;
    }
    if (type == UINT32_C(0x73747473)) { /* stts */
        uint32_t entry_count;
        uint32_t sample = 0u;
        if (length < 8u || tables->has_stts ||
            !mp4_table_count(data, length, &entry_count) ||
            (size_t)entry_count > (length - 8u) / 8u)
            return -1;
        for (index = 0u; index < entry_count; ++index) {
            uint32_t run = read_be32(data + 8u + index * 8u);
            uint32_t duration = read_be32(data + 12u + index * 8u);
            uint32_t item;
            if (run == 0u || sample > RIN_MEDIA_DEMUX_MAX_PACKETS - run)
                return -1;
            for (item = 0u; item < run; ++item)
                tables->sample_durations[sample++] = duration;
        }
        tables->has_stts = 1;
        if (tables->sample_count != 0u && sample != tables->sample_count)
            return -1;
        tables->sample_count = sample;
        return 1;
    }
    if (type == UINT32_C(0x63747473)) { /* ctts */
        uint32_t entry_count;
        uint32_t sample = 0u;
        uint8_t version;
        if (length < 8u ||
            !mp4_table_count(data, length, &entry_count) ||
            (size_t)entry_count > (length - 8u) / 8u)
            return -1;
        version = data[0];
        for (index = 0u; index < entry_count; ++index) {
            uint32_t run = read_be32(data + 8u + index * 8u);
            uint32_t raw = read_be32(data + 12u + index * 8u);
            int64_t offset = version == 1u ? (int64_t)(int32_t)raw :
                                            (int64_t)raw;
            uint32_t item;
            if (run == 0u || sample > RIN_MEDIA_DEMUX_MAX_PACKETS - run)
                return -1;
            for (item = 0u; item < run; ++item)
                tables->composition_offsets[sample++] = offset;
        }
        if (tables->sample_count != 0u && sample != tables->sample_count)
            return -1;
        if (tables->sample_count == 0u) tables->sample_count = sample;
        tables->has_ctts = 1;
        return 1;
    }
    if (type == UINT32_C(0x73747363)) { /* stsc */
        if (length < 8u || tables->has_stsc ||
            !mp4_table_count(data, length, &count) || count == 0u ||
            count > sizeof(tables->stsc) / sizeof(tables->stsc[0]) ||
            (size_t)count > (length - 8u) / 12u)
            return -1;
        for (index = 0u; index < count; ++index) {
            Mp4StscEntry* entry = &tables->stsc[index];
            entry->first_chunk = read_be32(data + 8u + index * 12u);
            entry->samples_per_chunk = read_be32(data + 12u + index * 12u);
            entry->sample_description_index =
                read_be32(data + 16u + index * 12u);
            if (entry->first_chunk == 0u || entry->samples_per_chunk == 0u ||
                entry->sample_description_index == 0u ||
                (index != 0u && entry->first_chunk <=
                    tables->stsc[index - 1u].first_chunk))
                return -1;
        }
        tables->stsc_count = count;
        tables->has_stsc = 1;
        return 1;
    }
    if (type == UINT32_C(0x7374636f) || type == UINT32_C(0x636f3634)) {
        size_t width = type == UINT32_C(0x7374636f) ? 4u : 8u;
        if (length < 8u || tables->has_stco ||
            !mp4_table_count(data, length, &count) || count == 0u ||
            count > RIN_MEDIA_DEMUX_MAX_PACKETS ||
            (size_t)count > (length - 8u) / width)
            return -1;
        for (index = 0u; index < count; ++index) {
            tables->chunk_offsets[index] = width == 4u ?
                (uint64_t)read_be32(data + 8u + index * width) :
                read_be64(data + 8u + index * width);
        }
        tables->chunk_count = count;
        tables->has_stco = 1;
        return 1;
    }
    if (type == UINT32_C(0x73747373)) { /* stss */
        if (length < 8u || tables->has_stss ||
            !mp4_table_count(data, length, &count) || count == 0u ||
            count > RIN_MEDIA_DEMUX_MAX_PACKETS ||
            (size_t)count > (length - 8u) / 4u)
            return -1;
        memset(tables->sync_samples, 0, sizeof(tables->sync_samples));
        for (index = 0u; index < count; ++index) {
            uint32_t sample = read_be32(data + 8u + index * 4u);
            if (sample == 0u || sample > RIN_MEDIA_DEMUX_MAX_PACKETS)
                return -1;
            tables->sync_samples[sample - 1u] = 1u;
        }
        tables->has_stss = 1;
        return 1;
    }
    return 0;
}

static int mp4_collect_sample_tables(const uint8_t* data, size_t begin,
                                     size_t end, Mp4SampleTables* tables,
                                     int depth)
{
    size_t offset = begin;
    if (!data || !tables || depth > 8) return 0;
    while (offset < end) {
        BoxView box;
        int parsed;
        if (!iso_box(data, end, offset, &box)) return 0;
        parsed = mp4_parse_sample_table(data + box.payload,
                                        box.end - box.payload, box.type,
                                        tables);
        if (parsed < 0) return 0;
        if (parsed == 0 && (box.type == UINT32_C(0x6d6f6f76) ||
                            box.type == UINT32_C(0x7472616b) ||
                            box.type == UINT32_C(0x6d646961) ||
                            box.type == UINT32_C(0x6d696e66) ||
                            box.type == UINT32_C(0x7374626c))) {
            if (!mp4_collect_sample_tables(data, box.payload, box.end,
                                           tables, depth + 1))
                return 0;
        }
        offset = box.end;
    }
    return offset == end;
}

static int mp4_stsc_for_chunk(const Mp4SampleTables* tables,
                              uint32_t chunk_number, uint32_t* samples)
{
    uint32_t index;
    if (!tables || !samples || chunk_number == 0u) return 0;
    for (index = tables->stsc_count; index > 0u; --index) {
        const Mp4StscEntry* entry = &tables->stsc[index - 1u];
        if (chunk_number >= entry->first_chunk) {
            *samples = entry->samples_per_chunk;
            return 1;
        }
    }
    return 0;
}

static int mp4_index_track(const uint8_t* data, size_t source_bytes,
                           const RinMediaDemuxTrackV1* track,
                           const Mp4SampleTables* tables,
                           RinMediaDemuxPacketTableV1* output)
{
    uint32_t chunk;
    uint32_t sample = 0u;
    uint64_t dts = 0u;
    if (!data || !track || !tables || !output || !tables->has_stsz ||
        !tables->has_stts || !tables->has_stco || !tables->has_stsc ||
        tables->sample_count == 0u || tables->sample_count >
            RIN_MEDIA_DEMUX_MAX_PACKETS)
        return 0;
    /* ctts is optional; the zero-filled offsets mean PTS equals DTS. */
    for (chunk = 1u; chunk <= tables->chunk_count; ++chunk) {
        uint32_t samples_per_chunk;
        uint32_t item;
        uint64_t byte_offset = tables->chunk_offsets[chunk - 1u];
        if (!mp4_stsc_for_chunk(tables, chunk, &samples_per_chunk)) return 0;
        if (sample > tables->sample_count ||
            samples_per_chunk > tables->sample_count - sample)
            return 0;
        for (item = 0u; item < samples_per_chunk &&
                       sample < tables->sample_count; ++item, ++sample) {
            uint32_t size = tables->sample_size != 0u ? tables->sample_size :
                             tables->sample_sizes[sample];
            uint32_t duration = tables->sample_durations[sample];
            int64_t composition = tables->composition_offsets[sample];
            RinMediaDemuxPacketV1 packet;
            if (size == 0u || size > 1024u * 1024u ||
                byte_offset > (uint64_t)source_bytes ||
                size > source_bytes - (size_t)byte_offset ||
                duration == 0u || (composition < 0 &&
                    (uint64_t)(-composition) > dts) ||
                (composition >= 0 && UINT64_MAX - dts <
                    (uint64_t)composition))
                return 0;
            if (output->packet_count >= RIN_MEDIA_DEMUX_MAX_PACKETS)
                return 0;
            memset(&packet, 0, sizeof(packet));
            packet.byte_offset = byte_offset;
            packet.byte_size = size;
            packet.track_id = track->track_id;
            packet.timestamp_ticks = composition < 0 ?
                dts - (uint64_t)(-composition) : dts + (uint64_t)composition;
            packet.duration_ticks = duration;
            packet.flags = tables->has_stss ? tables->sync_samples[sample] ?
                RIN_MEDIA_DEMUX_PACKET_KEYFRAME : 0u :
                RIN_MEDIA_DEMUX_PACKET_KEYFRAME;
            output->packets[output->packet_count++] = packet;
            if (UINT64_MAX - byte_offset < size) return 0;
            byte_offset += size;
            if (UINT64_MAX - dts < duration) return 0;
            dts += duration;
        }
        if (item != samples_per_chunk && sample != tables->sample_count)
            return 0;
    }
    return sample == tables->sample_count;
}

static int media_track_index(const RinMediaDemuxInfoV1* info, uint32_t track_id)
{
    uint32_t index;
    if (!info) return -1;
    for (index = 0u; index < info->track_count; ++index)
        if (info->tracks[index].track_id == track_id) return (int)index;
    return -1;
}

static int mp4_index_packets(const uint8_t* data, size_t source_bytes,
                             const RinMediaDemuxInfoV1* info,
                             RinMediaDemuxPacketTableV1* output)
{
    size_t offset = 0u;
    int found = 0;
    while (offset < source_bytes) {
        BoxView box;
        if (!iso_box(data, source_bytes, offset, &box)) return 0;
        if (box.type == UINT32_C(0x6d6f6f76)) {
            size_t child = box.payload;
            while (child < box.end) {
                BoxView track_box;
                RinMediaDemuxTrackV1 track;
                Mp4SampleTables tables;
                if (!iso_box(data, box.end, child, &track_box)) return 0;
                if (track_box.type == UINT32_C(0x7472616b)) {
                    memset(&track, 0, sizeof(track));
                    if (!mp4_find_track_fields(data, track_box.payload,
                                               track_box.end, &track, 0))
                        return 0;
                    if (media_track_index(info, track.track_id) < 0)
                        return 0;
                    memset(&tables, 0, sizeof(tables));
                    if (!mp4_collect_sample_tables(data, track_box.payload,
                                                   track_box.end, &tables, 0) ||
                        !mp4_index_track(data, source_bytes, &track, &tables,
                                         output))
                        return 0;
                    found = 1;
                }
                child = track_box.end;
            }
        }
        offset = box.end;
    }
    return found && output->packet_count != 0u;
}

static int webm_track_number(const uint8_t* data, size_t length,
                             uint32_t* track, size_t* consumed)
{
    uint64_t value;
    size_t bytes;
    int unknown;
    if (!ebml_vint(data, length, &value, &bytes, &unknown) || unknown ||
        value == 0u || value > UINT32_MAX || !track || !consumed)
        return 0;
    *track = (uint32_t)value;
    *consumed = bytes;
    return 1;
}

static int webm_index_cluster(const uint8_t* data, size_t begin, size_t end,
                              const RinMediaDemuxInfoV1* info,
                              RinMediaDemuxPacketTableV1* output)
{
    size_t offset = begin;
    uint64_t cluster_timecode = 0u;
    int have_timecode = 0;
    while (offset < end) {
        uint64_t id;
        size_t payload;
        size_t element_end;
        if (!ebml_element(data, end, offset, &id, &payload, &element_end))
            return 0;
        if (id == UINT64_C(0xe7)) {
            size_t length = element_end - payload;
            if (length == 0u || length > 8u) return 0;
            cluster_timecode = ebml_uint(data + payload, length);
            have_timecode = 1;
        } else if (id == UINT64_C(0xa3)) { /* SimpleBlock */
            uint32_t track_id;
            size_t track_bytes;
            int16_t relative;
            uint8_t flags;
            RinMediaDemuxPacketV1 packet;
            if (!have_timecode || element_end - payload < 4u ||
                !webm_track_number(data + payload, element_end - payload,
                                    &track_id, &track_bytes) ||
                track_bytes > element_end - payload - 3u)
                return 0;
            relative = (int16_t)(((uint16_t)data[payload + track_bytes] << 8u) |
                                 data[payload + track_bytes + 1u]);
            flags = data[payload + track_bytes + 2u];
            if ((flags & 0x06u) != 0u) return 0; /* lacing is not indexed */
            if (media_track_index(info, track_id) < 0 ||
                output->packet_count >= RIN_MEDIA_DEMUX_MAX_PACKETS)
                return 0;
            if (relative < 0 && (uint64_t)(-(int64_t)relative) > cluster_timecode)
                return 0;
            memset(&packet, 0, sizeof(packet));
            packet.byte_offset = payload + track_bytes + 3u;
            packet.byte_size = (uint32_t)(element_end - packet.byte_offset);
            packet.track_id = track_id;
            if (relative >= 0 &&
                UINT64_MAX - cluster_timecode < (uint64_t)relative)
                return 0;
            packet.timestamp_ticks = relative < 0 ?
                cluster_timecode - (uint64_t)(-(int64_t)relative) :
                cluster_timecode + (uint64_t)relative;
            packet.flags = (flags & 0x80u) != 0u ?
                RIN_MEDIA_DEMUX_PACKET_KEYFRAME : 0u;
            if (packet.byte_size == 0u || packet.byte_size > 1024u * 1024u)
                return 0;
            output->packets[output->packet_count++] = packet;
        }
        offset = element_end;
    }
    return offset == end;
}

static int webm_index_packets(const uint8_t* data, size_t source_bytes,
                              const RinMediaDemuxInfoV1* info,
                              RinMediaDemuxPacketTableV1* output)
{
    uint64_t id;
    size_t payload;
    size_t end;
    size_t offset;
    if (!ebml_element(data, source_bytes, 0u, &id, &payload, &end) ||
        id != UINT64_C(0x1a45dfa3)) return 0;
    offset = end;
    while (offset < source_bytes) {
        if (!ebml_element(data, source_bytes, offset, &id, &payload, &end))
            return 0;
        if (id == UINT64_C(0x18538067)) {
            size_t segment = payload;
            while (segment < end) {
                size_t child_payload;
                size_t child_end;
                if (!ebml_element(data, end, segment, &id, &child_payload,
                                  &child_end))
                    return 0;
                if (id == UINT64_C(0x1f43b675) &&
                    !webm_index_cluster(data, child_payload, child_end,
                                        info, output))
                    return 0;
                segment = child_end;
            }
            return output->packet_count != 0u;
        }
        offset = end;
    }
    return 0;
}

static void sort_packets(RinMediaDemuxPacketTableV1* output)
{
    uint32_t index;
    if (!output) return;
    for (index = 1u; index < output->packet_count; ++index) {
        RinMediaDemuxPacketV1 value = output->packets[index];
        uint32_t cursor = index;
        while (cursor != 0u &&
               output->packets[cursor - 1u].byte_offset > value.byte_offset) {
            output->packets[cursor] = output->packets[cursor - 1u];
            --cursor;
        }
        output->packets[cursor] = value;
    }
}

int rin_media_container_index_packets(
    const uint8_t* data, size_t source_bytes,
    const RinMediaDemuxInfoV1* info, size_t info_size,
    RinMediaDemuxPacketTableV1* output, size_t output_size)
{
    int result;
    if (output != NULL) memset(output, 0, sizeof(*output));
    if (!data || source_bytes == 0u ||
        source_bytes > RIN_MEDIA_CONTAINER_PROBE_MAX_BYTES || !info ||
        info_size < sizeof(*info) || info->struct_size != sizeof(*info) ||
        info->abi_version != RIN_MEDIA_DEMUX_ABI_V1 ||
        info->track_count == 0u || info->track_count > RIN_MEDIA_DEMUX_MAX_TRACKS ||
        !output || output_size < sizeof(*output))
        return RIN_MEDIA_DEMUX_INVALID;
    if (info->container_id != RIN_MEDIA_CONTAINER_MP4 &&
        info->container_id != RIN_MEDIA_CONTAINER_WEBM &&
        info->container_id != RIN_MEDIA_CONTAINER_MATROSKA)
        return RIN_MEDIA_DEMUX_UNSUPPORTED;
    output->struct_size = sizeof(*output);
    output->abi_version = RIN_MEDIA_DEMUX_ABI_V1;
    result = info->container_id == RIN_MEDIA_CONTAINER_MP4 ?
        mp4_index_packets(data, source_bytes, info, output) :
        webm_index_packets(data, source_bytes, info, output);
    if (!result || output->packet_count == 0u) {
        memset(output, 0, sizeof(*output));
        return RIN_MEDIA_DEMUX_UNSUPPORTED;
    }
    sort_packets(output);
    return RIN_MEDIA_DEMUX_OK;
}
