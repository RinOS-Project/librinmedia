/* SPDX-License-Identifier: MIT */

#include "rinmedia_demux.h"
#include "rinavi.h"
#include "rinwav.h"

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

static uint32_t read_le32(const uint8_t* value)
{
    return (uint32_t)value[0] |
           ((uint32_t)value[1] << 8u) |
           ((uint32_t)value[2] << 16u) |
           ((uint32_t)value[3] << 24u);
}

static uint64_t read_le64(const uint8_t* value)
{
    return (uint64_t)read_le32(value) |
           ((uint64_t)read_le32(value + 4u) << 32u);
}

static int bounded_text(const uint8_t* value, size_t length, size_t capacity)
{
    size_t index;
    if (!value || length == 0u || length >= capacity) return 0;
    for (index = 0u; index < length; ++index)
        if (value[index] == 0u) return 0;
    return 1;
}

static uint32_t avi_fourcc_be(uint32_t value)
{
    return ((value & UINT32_C(0x000000ff)) << 24u) |
           ((value & UINT32_C(0x0000ff00)) << 8u) |
           ((value & UINT32_C(0x00ff0000)) >> 8u) |
           ((value & UINT32_C(0xff000000)) >> 24u);
}

static int avi_video_codec(uint32_t codec, uint32_t* codec_id,
                           char* codec_name)
{
    uint32_t index;
    if (!codec_id || !codec_name || codec == 0u) return 0;
    for (index = 0u; index < 4u; ++index) {
        uint8_t byte = (uint8_t)(codec >> (index * 8u));
        if (byte == 0u || byte < 0x20u || byte > 0x7eu) return 0;
        codec_name[index] = (char)byte;
    }
    codec_name[4] = '\0';
    *codec_id = avi_fourcc_be(codec);
    return 1;
}

static int avi_audio_codec(uint16_t format, uint32_t* codec_id,
                           char* codec_name)
{
    const char* name;
    if (!codec_id || !codec_name || format == 0u) return 0;
    switch (format) {
    case RAVI_WAVE_PCM: name = "PCM "; break;
    case RAVI_WAVE_ADPCM: name = "ADPC"; break;
    case RAVI_WAVE_FLOAT: name = "FL32"; break;
    case RAVI_WAVE_ALAW: name = "ALAW"; break;
    case RAVI_WAVE_MULAW: name = "ULAW"; break;
    case RAVI_WAVE_MP3: name = "MP3 "; break;
    default: name = "WAVE"; break;
    }
    memcpy(codec_name, name, 4u);
    codec_name[4] = '\0';
    *codec_id = (uint32_t)format;
    return 1;
}

static int avi_fill_track(const RAviStream* stream,
                          RinMediaDemuxTrackV1* track)
{
    uint32_t scale;
    if (!stream || !track || stream->stream_index < 0 ||
        stream->stream_index >= (int)RIN_MEDIA_DEMUX_MAX_TRACKS)
        return 0;
    memset(track, 0, sizeof(*track));
    track->track_id = (uint32_t)stream->stream_index + 1u;
    track->kind = stream->type == 0 ? RIN_MEDIA_DEMUX_TRACK_VIDEO :
                 stream->type == 1 ? RIN_MEDIA_DEMUX_TRACK_AUDIO : 0u;
    if (track->kind == 0u) return 0;
    scale = stream->scale == 0u ? 1u : stream->scale;
    track->time_scale = scale;
    if ((uint64_t)stream->total_frames > UINT64_MAX / scale)
        return 0;
    track->duration_ticks = (uint64_t)stream->total_frames * scale;
    return track->kind == RIN_MEDIA_DEMUX_TRACK_VIDEO ?
        avi_video_codec(stream->video.codec, &track->codec_id,
                        track->codec_name) :
        avi_audio_codec(stream->audio.format, &track->codec_id,
                        track->codec_name);
}

static int wav_fill_codec(uint16_t format, uint32_t* codec_id,
                          char* codec_name)
{
    const char* name;
    if (!codec_id || !codec_name || format == 0u) return 0;
    switch (format) {
    case RWAV_FORMAT_PCM: name = "PCM "; break;
    case RWAV_FORMAT_IEEE_FLOAT: name = "FL32"; break;
    case RWAV_FORMAT_ALAW: name = "ALAW"; break;
    case RWAV_FORMAT_MULAW: name = "ULAW"; break;
    case RWAV_FORMAT_IMA_ADPCM: name = "IMAD"; break;
    default: name = "WAVE"; break;
    }
    memcpy(codec_name, name, 4u);
    codec_name[4] = '\0';
    *codec_id = (uint32_t)format;
    return 1;
}

static int wav_fill_track(const RWavContext* context, uint32_t total_samples,
                          RinMediaDemuxTrackV1* track)
{
    if (!context || !track || context->sample_rate == 0u ||
        context->channels == 0u || context->channels > 2u)
        return 0;
    memset(track, 0, sizeof(*track));
    track->track_id = 1u;
    track->kind = RIN_MEDIA_DEMUX_TRACK_AUDIO;
    track->time_scale = context->sample_rate;
    track->duration_ticks = total_samples;
    return wav_fill_codec(context->format, &track->codec_id,
                          track->codec_name);
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

static int mp4_track_id_exists(const RinMediaDemuxInfoV1* output,
                               uint32_t track_id)
{
    uint32_t index;
    if (!output || track_id == 0u) return 0;
    for (index = 0u; index < output->track_count; ++index)
        if (output->tracks[index].track_id == track_id) return 1;
    return 0;
}

static int mp4_box_is_container(uint32_t type)
{
    switch (type) {
    case UINT32_C(0x65647473): /* edts */
    case UINT32_C(0x6d646961): /* mdia */
    case UINT32_C(0x6d696e66): /* minf */
    case UINT32_C(0x64696e66): /* dinf */
    case UINT32_C(0x7374626c): /* stbl */
        return 1;
    default:
        return 0;
    }
}

static int mp4_find_track_fields(const uint8_t* data, size_t begin, size_t end,
                                 RinMediaDemuxTrackV1* track, int depth,
                                 uint32_t* seen_fields)
{
    size_t offset = begin;
    if (!data || !track || !seen_fields || depth > 8) return 0;
    while (offset < end) {
        BoxView box;
        if (!iso_box(data, end, offset, &box)) return 0;
        if (box.type == UINT32_C(0x746b6864)) { /* tkhd */
            if ((*seen_fields & UINT32_C(0x01)) != 0u) return 0;
            *seen_fields |= UINT32_C(0x01);
            track->track_id = mp4_track_id(data + box.payload,
                                           box.end - box.payload);
        } else if (box.type == UINT32_C(0x6d646864)) { /* mdhd */
            if ((*seen_fields & UINT32_C(0x02)) != 0u) return 0;
            *seen_fields |= UINT32_C(0x02);
            if (!mp4_media_header(data + box.payload, box.end - box.payload,
                                  &track->time_scale,
                                  &track->duration_ticks))
                return 0;
        } else if (box.type == UINT32_C(0x68646c72)) { /* hdlr */
            if ((*seen_fields & UINT32_C(0x04)) != 0u) return 0;
            *seen_fields |= UINT32_C(0x04);
            if (!mp4_handler_kind(data + box.payload, box.end - box.payload,
                                  &track->kind))
                return 0;
        } else if (box.type == UINT32_C(0x73747364)) { /* stsd */
            const uint8_t* payload = data + box.payload;
            size_t length = box.end - box.payload;
            if ((*seen_fields & UINT32_C(0x08)) != 0u) return 0;
            *seen_fields |= UINT32_C(0x08);
            if (length < 16u || read_be32(payload + 4u) != 1u ||
                !mp4_sample_entry(payload + 8u, length - 8u,
                                  &track->codec_id, track->codec_name))
                return 0;
        } else if (mp4_box_is_container(box.type) &&
                   !mp4_find_track_fields(data, box.payload, box.end, track,
                                          depth + 1, seen_fields)) {
            return 0;
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
            if (found_ftyp) return RIN_MEDIA_DEMUX_UNSUPPORTED;
            if (box.end - box.payload < 8u) return RIN_MEDIA_DEMUX_INVALID;
            found_ftyp = 1;
        } else if (box.type == UINT32_C(0x6d6f6f76)) { /* moov */
            size_t child = box.payload;
            if (found_moov) return RIN_MEDIA_DEMUX_UNSUPPORTED;
            found_moov = 1;
            while (child < box.end) {
                BoxView trak_box;
                RinMediaDemuxTrackV1 track;
                uint32_t seen_fields = 0u;
                if (!iso_box(data, box.end, child, &trak_box))
                    return RIN_MEDIA_DEMUX_INVALID;
                if (trak_box.type == UINT32_C(0x7472616b)) { /* trak */
                    if (output->track_count >= RIN_MEDIA_DEMUX_MAX_TRACKS)
                        return RIN_MEDIA_DEMUX_UNSUPPORTED;
                    memset(&track, 0, sizeof(track));
                    if (!mp4_find_track_fields(data, trak_box.payload,
                                               trak_box.end, &track, 0,
                                               &seen_fields) ||
                        seen_fields != UINT32_C(0x0f) ||
                        track.track_id == 0u || track.kind == 0u ||
                        track.codec_id == 0u || track.time_scale == 0u ||
                        mp4_track_id_exists(output, track.track_id))
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

static int avi_inspect(const uint8_t* data, size_t source_bytes,
                       RinMediaDemuxInfoV1* output)
{
    RAviContext context;
    uint32_t container = 0u;
    int result;
    int index;
    if (rin_media_container_probe(data, source_bytes, &container) != 0 ||
        container != RIN_MEDIA_CONTAINER_AVI)
        return RIN_MEDIA_DEMUX_INVALID;
    result = ravi_open(&context, data, source_bytes);
    if (result != RAVI_OK || context.stream_count <= 0 ||
        context.stream_count > (int)RIN_MEDIA_DEMUX_MAX_TRACKS)
        return result == RAVI_DATA_ERROR ? RIN_MEDIA_DEMUX_INVALID :
               RIN_MEDIA_DEMUX_UNSUPPORTED;
    for (index = 0; index < context.stream_count; ++index) {
        RinMediaDemuxTrackV1 track;
        if (output->track_count >= RIN_MEDIA_DEMUX_MAX_TRACKS ||
            !avi_fill_track(&context.streams[index], &track))
            return RIN_MEDIA_DEMUX_UNSUPPORTED;
        if (output->track_count == 0u) {
            output->time_scale = track.time_scale;
            output->duration_ticks = track.duration_ticks;
        }
        output->tracks[output->track_count++] = track;
    }
    output->container_id = RIN_MEDIA_CONTAINER_AVI;
    return RIN_MEDIA_DEMUX_OK;
}

static int wav_inspect(const uint8_t* data, size_t source_bytes,
                       RinMediaDemuxInfoV1* output)
{
    RWavContext context;
    uint32_t container = 0u;
    uint32_t total_samples = 0u;
    int result;
    if (rin_media_container_probe(data, source_bytes, &container) != 0 ||
        container != RIN_MEDIA_CONTAINER_WAV)
        return RIN_MEDIA_DEMUX_INVALID;
    result = rwav_open(&context, data, source_bytes);
    if (result != RWAV_OK ||
        rwav_get_info(&context, 0, 0, 0, &total_samples) != RWAV_OK)
        return result == RWAV_DATA_ERROR ? RIN_MEDIA_DEMUX_INVALID :
               RIN_MEDIA_DEMUX_UNSUPPORTED;
    if (context.block_align == 0u ||
        context.data_size % context.block_align != 0u)
        return RIN_MEDIA_DEMUX_INVALID;
    if (!wav_fill_track(&context, total_samples, &output->tracks[0]))
        return RIN_MEDIA_DEMUX_UNSUPPORTED;
    output->track_count = 1u;
    output->time_scale = output->tracks[0].time_scale;
    output->duration_ticks = output->tracks[0].duration_ticks;
    output->container_id = RIN_MEDIA_CONTAINER_WAV;
    return RIN_MEDIA_DEMUX_OK;
}

typedef struct OggPageView {
    size_t segment_table;
    size_t payload;
    size_t end;
    uint64_t granule_position;
    uint32_t serial;
    uint32_t sequence;
    uint8_t header_type;
    uint8_t segment_count;
} OggPageView;

static uint32_t ogg_crc32(const uint8_t* data, size_t length)
{
    uint32_t crc = 0u;
    size_t index;
    for (index = 0u; index < length; ++index) {
        uint8_t value = (index >= 22u && index < 26u) ? 0u : data[index];
        uint8_t bit;
        crc ^= (uint32_t)value << 24u;
        for (bit = 0u; bit < 8u; ++bit)
            crc = (crc & UINT32_C(0x80000000)) != 0u ?
                (crc << 1u) ^ UINT32_C(0x04c11db7) : crc << 1u;
    }
    return crc;
}

static int ogg_page(const uint8_t* data, size_t source_bytes, size_t offset,
                    OggPageView* output)
{
    size_t segment_table;
    size_t body_bytes = 0u;
    size_t index;
    if (!data || !output || offset > source_bytes ||
        source_bytes - offset < 27u ||
        memcmp(data + offset, "OggS", 4u) != 0 || data[offset + 4u] != 0u)
        return 0;
    segment_table = offset + 27u + (size_t)data[offset + 26u];
    if (segment_table < offset || segment_table > source_bytes) return 0;
    for (index = 0u; index < (size_t)data[offset + 26u]; ++index) {
        size_t segment = data[offset + 27u + index];
        if (segment > source_bytes - segment_table - body_bytes)
            return 0;
        body_bytes += segment;
    }
    if (body_bytes > source_bytes - segment_table) return 0;
    memset(output, 0, sizeof(*output));
    output->segment_table = offset + 27u;
    output->payload = segment_table;
    output->end = segment_table + body_bytes;
    output->granule_position = read_le64(data + offset + 6u);
    output->serial = read_le32(data + offset + 14u);
    output->sequence = read_le32(data + offset + 18u);
    output->header_type = data[offset + 5u];
    output->segment_count = data[offset + 26u];
    if (ogg_crc32(data + offset, output->end - offset) !=
        read_le32(data + offset + 22u))
        return 0;
    return 1;
}

static int ogg_fill_track(const uint8_t* packet, size_t packet_bytes,
                          uint64_t duration_ticks,
                          RinMediaDemuxTrackV1* track)
{
    uint32_t channels;
    uint32_t sample_rate;
    if (!packet || !track) return 0;
    memset(track, 0, sizeof(*track));
    track->track_id = 1u;
    track->kind = RIN_MEDIA_DEMUX_TRACK_AUDIO;
    track->duration_ticks = duration_ticks;
    if (packet_bytes >= 19u && memcmp(packet, "OpusHead", 8u) == 0) {
        if (packet[8] != 1u || packet[9] == 0u || packet[9] > 32u)
            return 0;
        channels = packet[9];
        sample_rate = read_le32(packet + 12u);
        if (sample_rate == 0u || sample_rate > 384000u ||
            packet[18] > 1u)
            return 0;
        if (packet[18] == 0u && channels > 2u) return 0;
        if (packet[18] == 1u) {
            if (packet_bytes < 21u + (size_t)channels ||
                packet[19] == 0u || packet[19] > channels ||
                packet[20] > packet[19])
                return 0;
        }
        track->codec_id = RIN_MEDIA_CODEC_OPUS;
        track->time_scale = 48000u;
        memcpy(track->codec_name, "OPUS", 5u);
        return 1;
    }
    if (packet_bytes >= 30u && packet[0] == 1u &&
        memcmp(packet + 1u, "vorbis", 6u) == 0) {
        if (read_le32(packet + 7u) != 0u || packet[12u] == 0u ||
            packet[12u] > 32u)
            return 0;
        channels = packet[12u];
        sample_rate = read_le32(packet + 13u);
        if (sample_rate == 0u || sample_rate > 384000u ||
            (packet[28u] >> 4u) < (packet[28u] & 0x0fu) ||
            (packet[28u] & 0x0fu) < 4u || (packet[28u] >> 4u) > 13u ||
            (packet[29u] & 1u) == 0u)
            return 0;
        track->codec_id = RIN_MEDIA_CODEC_VORBIS;
        track->time_scale = sample_rate;
        memcpy(track->codec_name, "VORBIS", 7u);
        return 1;
    }
    return 0;
}

static int ogg_inspect(const uint8_t* data, size_t source_bytes,
                       RinMediaDemuxInfoV1* output)
{
    uint8_t packet[64u] = {0};
    RinMediaDemuxTrackV1 track;
    OggPageView page;
    size_t offset = 0u;
    size_t packet_bytes = 0u;
    uint64_t last_granule = UINT64_MAX;
    uint32_t serial = 0u;
    uint32_t expected_sequence = 0u;
    uint32_t page_count = 0u;
    int first_page = 1;
    int first_packet_done = 0;
    if (rin_media_container_probe(data, source_bytes, &serial) != 0 ||
        serial != RIN_MEDIA_CONTAINER_OGG)
        return RIN_MEDIA_DEMUX_INVALID;
    while (offset < source_bytes) {
        size_t body_offset;
        size_t index;
        if (page_count++ >= 4096u ||
            !ogg_page(data, source_bytes, offset, &page))
            return RIN_MEDIA_DEMUX_INVALID;
        if (first_page) {
            if ((page.header_type & 0x02u) == 0u ||
                (page.header_type & 0x01u) != 0u || page.sequence != 0u)
                return RIN_MEDIA_DEMUX_INVALID;
            serial = page.serial;
            expected_sequence = page.sequence;
            first_page = 0;
        } else if (page.serial != serial || page.sequence != expected_sequence) {
            return RIN_MEDIA_DEMUX_UNSUPPORTED;
        }
        expected_sequence = page.sequence + 1u;
        if (page.granule_position != UINT64_MAX)
            last_granule = page.granule_position;
        body_offset = page.payload;
        for (index = 0u; index < page.segment_count; ++index) {
            size_t segment_bytes = data[page.segment_table + index];
            if (!first_packet_done) {
                if (segment_bytes > sizeof(packet) - packet_bytes)
                    return RIN_MEDIA_DEMUX_UNSUPPORTED;
                memcpy(packet + packet_bytes, data + body_offset,
                       segment_bytes);
                packet_bytes += segment_bytes;
            }
            body_offset += segment_bytes;
            if (segment_bytes < 255u && !first_packet_done) {
                if (!ogg_fill_track(packet, packet_bytes,
                                    last_granule == UINT64_MAX ? 0u :
                                    last_granule, &track))
                    return RIN_MEDIA_DEMUX_UNSUPPORTED;
                first_packet_done = 1;
            }
        }
        offset = page.end;
    }
    if (first_page || !first_packet_done) return RIN_MEDIA_DEMUX_UNSUPPORTED;
    if (last_granule != UINT64_MAX) track.duration_ticks = last_granule;
    output->tracks[0] = track;
    output->track_count = 1u;
    output->time_scale = track.time_scale;
    output->duration_ticks = track.duration_ticks;
    output->container_id = RIN_MEDIA_CONTAINER_OGG;
    return RIN_MEDIA_DEMUX_OK;
}

/* This is an extent-only Opus subset.  It admits bounded C=0/1/2/3 forms,
 * deriving their known RFC 6716 frame duration without decoding packet
 * bytes.  Variable frame lengths, padding, and the 1275-byte per-frame
 * bound are checked; page-spanning packets and Vorbis packets remain
 * Unsupported. */
static int ogg_opus_frame_duration(uint32_t config, uint32_t* frame_ticks)
{
    if (!frame_ticks || config > 31u) return 0;
    if (config < 12u) {
        static const uint32_t silk_ticks[4] = {480u, 960u, 1920u, 2880u};
        *frame_ticks = silk_ticks[config & 3u];
    } else if (config < 16u) {
        *frame_ticks = (config & 1u) != 0u ? 960u : 480u;
    } else {
        static const uint32_t celt_ticks[4] = {120u, 240u, 480u, 960u};
        *frame_ticks = celt_ticks[config & 3u];
    }
    return 1;
}

static int ogg_opus_read_frame_length(const uint8_t* packet,
                                      size_t packet_bytes, size_t* cursor,
                                      size_t* frame_bytes)
{
    size_t first;
    if (!packet || !cursor || !frame_bytes || *cursor >= packet_bytes)
        return 0;
    first = (size_t)packet[*cursor];
    ++*cursor;
    if (first >= 252u) {
        if (*cursor >= packet_bytes) return 0;
        *frame_bytes = first + (size_t)packet[*cursor] * 4u;
        ++*cursor;
    } else {
        *frame_bytes = first;
    }
    return 1;
}

static int ogg_opus_packet_duration(const uint8_t* packet, size_t packet_bytes,
                                    uint32_t* duration_ticks)
{
    const size_t max_frame_bytes = 1275u;
    uint32_t frame_ticks;
    uint32_t frame_code;
    uint32_t frame_count;
    if (!packet || !duration_ticks || packet_bytes < 2u ||
        !ogg_opus_frame_duration((uint32_t)(packet[0] >> 3u),
                                 &frame_ticks))
        return 0;
    frame_code = (uint32_t)(packet[0] & 0x03u);
    if (frame_code == 0u) {
        if (packet_bytes - 1u > max_frame_bytes)
            return 0;
        frame_count = 1u;
    } else if (frame_code == 1u) {
        /* C=1 stores two equal-sized frames after the TOC byte. */
        if (packet_bytes < 3u || ((packet_bytes - 1u) & 1u) != 0u ||
            (packet_bytes - 1u) / 2u > max_frame_bytes)
            return 0;
        frame_count = 2u;
    } else if (frame_code == 2u) {
        size_t first_frame_size;
        size_t cursor = 1u;
        size_t frame_payload_size;
        if (packet_bytes < 3u) return 0;
        if (!ogg_opus_read_frame_length(packet, packet_bytes, &cursor,
                                         &first_frame_size) ||
            cursor >= packet_bytes)
            return 0;
        frame_payload_size = packet_bytes - cursor;
        if (first_frame_size == 0u || first_frame_size > max_frame_bytes ||
            first_frame_size >= frame_payload_size ||
            frame_payload_size - first_frame_size > max_frame_bytes)
            return 0;
        frame_count = 2u;
    } else if (frame_code == 3u) {
        size_t cursor = 2u;
        size_t padding_bytes = 0u;
        size_t frame_payload_end = packet_bytes;
        size_t payload_bytes;
        size_t frame_bytes;
        size_t signaled_bytes = 0u;
        uint32_t index;
        int variable = (packet[1] & 0x01u) != 0u;
        if (packet_bytes < 2u || (packet[1] >> 2u) == 0u)
            return 0;
        frame_count = (uint32_t)(packet[1] >> 2u);
        if ((packet[1] & 0x02u) != 0u) {
            size_t padding_header_bytes = 0u;
            for (;;) {
                size_t value;
                if (cursor >= packet_bytes) return 0;
                value = (size_t)packet[cursor++];
                ++padding_header_bytes;
                if (value == 255u) {
                    if (padding_bytes > (size_t)-1 - 254u) return 0;
                    padding_bytes += 254u;
                } else {
                    if (padding_bytes > (size_t)-1 - value) return 0;
                    padding_bytes += value;
                    break;
                }
            }
            if (padding_header_bytes > packet_bytes - 2u ||
                padding_bytes > packet_bytes - 2u - padding_header_bytes)
                return 0;
            frame_payload_end = packet_bytes - padding_bytes;
        }
        if (!variable) {
            if (cursor > frame_payload_end) return 0;
            payload_bytes = frame_payload_end - cursor;
            if (payload_bytes == 0u || payload_bytes % frame_count != 0u)
                return 0;
            frame_bytes = payload_bytes / frame_count;
            if (frame_bytes == 0u || frame_bytes > max_frame_bytes)
                return 0;
        } else {
            for (index = 0u; index + 1u < frame_count; ++index) {
                if (!ogg_opus_read_frame_length(packet, frame_payload_end,
                                                 &cursor,
                                                 &frame_bytes) ||
                    frame_bytes == 0u || frame_bytes > max_frame_bytes ||
                    signaled_bytes > (size_t)-1 - frame_bytes)
                    return 0;
                signaled_bytes += frame_bytes;
            }
            if (cursor > frame_payload_end ||
                signaled_bytes > frame_payload_end - cursor)
                return 0;
            frame_bytes = frame_payload_end - cursor - signaled_bytes;
            if (frame_bytes == 0u || frame_bytes > max_frame_bytes)
                return 0;
        }
    } else {
        frame_count = 0u;
    }
    if (frame_count == 0u || packet_bytes < 1u + frame_count ||
        (uint64_t)frame_count * (uint64_t)frame_ticks > 5760u)
        return 0;
    *duration_ticks = frame_count * frame_ticks;
    return 1;
}

static int ogg_opus_index_packets(const uint8_t* data, size_t source_bytes,
                                  const RinMediaDemuxInfoV1* info,
                                  RinMediaDemuxPacketTableV1* output)
{
    RinMediaDemuxInfoV1 verified;
    OggPageView page;
    size_t offset = 0u;
    size_t packet_start = 0u;
    size_t packet_bytes = 0u;
    uint64_t timestamp = 0u;
    uint32_t serial = 0u;
    uint32_t expected_sequence = 0u;
    uint32_t page_count = 0u;
    uint32_t header_packets = 0u;
    int first_page = 1;
    if (!data || !info || !output || info->container_id !=
        RIN_MEDIA_CONTAINER_OGG || info->track_count != 1u ||
        rin_media_container_inspect(data, source_bytes, &verified,
                                    sizeof(verified)) != RIN_MEDIA_DEMUX_OK ||
            verified.tracks[0].codec_id !=
            RIN_MEDIA_CODEC_OPUS)
        return 0;
    while (offset < source_bytes) {
        size_t body_offset;
        size_t index;
        if (page_count++ >= 4096u ||
            !ogg_page(data, source_bytes, offset, &page))
            return 0;
        if (first_page) {
            if ((page.header_type & 0x02u) == 0u ||
                (page.header_type & 0x01u) != 0u || page.sequence != 0u)
                return 0;
            serial = page.serial;
            expected_sequence = page.sequence;
            first_page = 0;
        } else if (page.serial != serial || page.sequence != expected_sequence ||
                   (page.header_type & 0x01u) != 0u) {
            /* A continued packet is not a single caller-owned byte extent. */
            return 0;
        }
        expected_sequence = page.sequence + 1u;
        body_offset = page.payload;
        packet_bytes = 0u;
        for (index = 0u; index < page.segment_count; ++index) {
            size_t segment_bytes = data[page.segment_table + index];
            if (packet_bytes == 0u) packet_start = body_offset;
            if (segment_bytes > 1024u * 1024u - packet_bytes)
                return 0;
            packet_bytes += segment_bytes;
            body_offset += segment_bytes;
            if (segment_bytes < 255u) {
                if (packet_bytes == 0u) return 0;
                if (header_packets == 0u) {
                    if (packet_bytes < 19u ||
                        memcmp(data + packet_start, "OpusHead", 8u) != 0)
                        return 0;
                } else if (header_packets == 1u) {
                    if (packet_bytes < 8u ||
                        memcmp(data + packet_start, "OpusTags", 8u) != 0)
                        return 0;
                } else {
                    RinMediaDemuxPacketV1 packet;
                    uint32_t duration;
                    if (output->packet_count >= RIN_MEDIA_DEMUX_MAX_PACKETS ||
                        !ogg_opus_packet_duration(data + packet_start,
                                                  packet_bytes, &duration) ||
                        UINT64_MAX - timestamp < duration)
                        return 0;
                    memset(&packet, 0, sizeof(packet));
                    packet.byte_offset = (uint64_t)packet_start;
                    packet.byte_size = (uint32_t)packet_bytes;
                    packet.track_id = verified.tracks[0].track_id;
                    packet.timestamp_ticks = timestamp;
                    packet.duration_ticks = duration;
                    output->packets[output->packet_count++] = packet;
                    timestamp += duration;
                }
                ++header_packets;
                packet_bytes = 0u;
            }
        }
        if (packet_bytes != 0u) return 0;
        offset = page.end;
    }
    return !first_page && header_packets >= 3u && output->packet_count != 0u;
}

/* This is an extent-only Vorbis subset.  It accepts the three Vorbis header
 * packets followed by at most one complete audio packet on each Ogg page.
 * The page granule position supplies the end timestamp; packets that span a
 * page, share a page, or have no bounded granule position remain Unsupported.
 * Packet bytes are never decoded here. */
static int ogg_vorbis_index_packets(const uint8_t* data, size_t source_bytes,
                                    const RinMediaDemuxInfoV1* info,
                                    RinMediaDemuxPacketTableV1* output)
{
    RinMediaDemuxInfoV1 verified;
    OggPageView page;
    size_t offset = 0u;
    size_t packet_start = 0u;
    size_t packet_bytes = 0u;
    uint64_t timestamp = 0u;
    uint32_t serial = 0u;
    uint32_t expected_sequence = 0u;
    uint32_t page_count = 0u;
    uint32_t header_packets = 0u;
    int first_page = 1;
    if (!data || !info || !output || info->container_id !=
        RIN_MEDIA_CONTAINER_OGG || info->track_count != 1u ||
        rin_media_container_inspect(data, source_bytes, &verified,
                                    sizeof(verified)) != RIN_MEDIA_DEMUX_OK ||
        verified.tracks[0].codec_id != RIN_MEDIA_CODEC_VORBIS)
        return 0;
    while (offset < source_bytes) {
        size_t body_offset;
        size_t index;
        uint32_t audio_packets = 0u;
        if (page_count++ >= 4096u ||
            !ogg_page(data, source_bytes, offset, &page))
            return 0;
        if (first_page) {
            if ((page.header_type & 0x02u) == 0u ||
                (page.header_type & 0x01u) != 0u || page.sequence != 0u)
                return 0;
            serial = page.serial;
            expected_sequence = page.sequence;
            first_page = 0;
        } else if (page.serial != serial || page.sequence != expected_sequence ||
                   (page.header_type & 0x01u) != 0u) {
            /* A continued packet is not a single caller-owned byte extent. */
            return 0;
        }
        expected_sequence = page.sequence + 1u;
        body_offset = page.payload;
        for (index = 0u; index < page.segment_count; ++index) {
            size_t segment_bytes = data[page.segment_table + index];
            if (packet_bytes == 0u) packet_start = body_offset;
            if (segment_bytes > 1024u * 1024u - packet_bytes)
                return 0;
            packet_bytes += segment_bytes;
            body_offset += segment_bytes;
            if (segment_bytes < 255u) {
                if (packet_bytes == 0u) return 0;
                if (header_packets < 3u) {
                    uint8_t packet_type = data[packet_start];
                    if ((header_packets == 0u &&
                         (packet_type != 1u || packet_bytes < 7u)) ||
                        (header_packets == 1u &&
                         (packet_type != 3u || packet_bytes < 7u)) ||
                        (header_packets == 2u &&
                         (packet_type != 5u || packet_bytes < 7u)) ||
                        memcmp(data + packet_start + 1u, "vorbis", 6u) != 0)
                        return 0;
                    ++header_packets;
                } else {
                    RinMediaDemuxPacketV1 packet;
                    uint64_t end_timestamp = page.granule_position;
                    uint64_t duration;
                    if ((data[packet_start] & 1u) != 0u ||
                        end_timestamp == UINT64_MAX || audio_packets != 0u ||
                        output->packet_count >= RIN_MEDIA_DEMUX_MAX_PACKETS ||
                        end_timestamp <= timestamp)
                        return 0;
                    duration = end_timestamp - timestamp;
                    if (duration > UINT32_MAX) return 0;
                    memset(&packet, 0, sizeof(packet));
                    packet.byte_offset = (uint64_t)packet_start;
                    packet.byte_size = (uint32_t)packet_bytes;
                    packet.track_id = verified.tracks[0].track_id;
                    packet.timestamp_ticks = timestamp;
                    packet.duration_ticks = (uint32_t)duration;
                    output->packets[output->packet_count++] = packet;
                    timestamp = end_timestamp;
                    ++audio_packets;
                }
                packet_bytes = 0u;
            }
        }
        if (packet_bytes != 0u) return 0;
        offset = page.end;
    }
    return !first_page && header_packets == 3u && output->packet_count != 0u &&
           timestamp == verified.duration_ticks;
}

typedef struct AdtsFrameHeader {
    size_t frame_bytes;
    uint32_t profile;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t samples;
} AdtsFrameHeader;

static const uint32_t kAdtsSampleRates[] = {
    96000u, 88200u, 64000u, 48000u, 44100u, 32000u, 24000u,
    22050u, 16000u, 12000u, 11025u, 8000u, 7350u
};

static int adts_frame_header(const uint8_t* data, size_t source_bytes,
                             size_t offset, AdtsFrameHeader* output)
{
    const uint8_t* header;
    uint32_t sample_rate_index;
    uint32_t channel_configuration;
    uint32_t raw_data_blocks;
    size_t header_bytes;
    size_t frame_bytes;
    if (!data || !output || offset > source_bytes ||
        source_bytes - offset < 7u)
        return 0;
    header = data + offset;
    if (header[0] != 0xffu || (header[1] & 0xf6u) != 0xf0u)
        return 0;
    sample_rate_index = ((uint32_t)header[2] >> 2u) & 0x0fu;
    channel_configuration = ((uint32_t)(header[2] & 0x01u) << 2u) |
                            ((uint32_t)header[3] >> 6u);
    if (sample_rate_index >= (uint32_t)(sizeof(kAdtsSampleRates) /
                                        sizeof(kAdtsSampleRates[0])) ||
        channel_configuration == 0u || channel_configuration > 6u)
        return 0;
    header_bytes = (header[1] & 0x01u) != 0u ? 7u : 9u;
    frame_bytes = ((size_t)(header[3] & 0x03u) << 11u) |
                  ((size_t)header[4] << 3u) | ((size_t)header[5] >> 5u);
    if (frame_bytes <= header_bytes || frame_bytes > source_bytes - offset)
        return 0;
    raw_data_blocks = (uint32_t)(header[6] & 0x03u) + 1u;
    memset(output, 0, sizeof(*output));
    output->frame_bytes = frame_bytes;
    output->profile = ((uint32_t)header[2] >> 6u) + 1u;
    output->sample_rate = kAdtsSampleRates[sample_rate_index];
    output->channels = channel_configuration;
    output->samples = raw_data_blocks * 1024u;
    return 1;
}

static int adts_inspect(const uint8_t* data, size_t source_bytes,
                        RinMediaDemuxInfoV1* output)
{
    AdtsFrameHeader frame;
    RinMediaDemuxTrackV1 track;
    size_t offset = 0u;
    uint64_t duration = 0u;
    uint32_t frame_count = 0u;
    uint32_t profile = 0u;
    uint32_t sample_rate = 0u;
    uint32_t channels = 0u;
    uint32_t container = 0u;
    if (rin_media_container_probe(data, source_bytes, &container) != 0 ||
        container != RIN_MEDIA_CONTAINER_ADTS)
        return RIN_MEDIA_DEMUX_INVALID;
    while (offset < source_bytes) {
        if (frame_count++ >= 65536u ||
            !adts_frame_header(data, source_bytes, offset, &frame))
            return RIN_MEDIA_DEMUX_INVALID;
        if (profile == 0u) {
            profile = frame.profile;
            sample_rate = frame.sample_rate;
            channels = frame.channels;
        } else if (profile != frame.profile || sample_rate != frame.sample_rate ||
                   channels != frame.channels) {
            return RIN_MEDIA_DEMUX_UNSUPPORTED;
        }
        if (UINT64_MAX - duration < frame.samples)
            return RIN_MEDIA_DEMUX_INVALID;
        duration += frame.samples;
        offset += frame.frame_bytes;
    }
    if (frame_count == 0u || offset != source_bytes)
        return RIN_MEDIA_DEMUX_INVALID;
    memset(&track, 0, sizeof(track));
    track.track_id = 1u;
    track.kind = RIN_MEDIA_DEMUX_TRACK_AUDIO;
    track.codec_id = RIN_MEDIA_CODEC_AAC;
    track.time_scale = sample_rate;
    track.duration_ticks = duration;
    memcpy(track.codec_name, "AAC", 4u);
    output->tracks[0] = track;
    output->track_count = 1u;
    output->time_scale = sample_rate;
    output->duration_ticks = duration;
    output->container_id = RIN_MEDIA_CONTAINER_ADTS;
    return RIN_MEDIA_DEMUX_OK;
}

static int adts_index_packets(const uint8_t* data, size_t source_bytes,
                              const RinMediaDemuxInfoV1* info,
                              RinMediaDemuxPacketTableV1* output)
{
    RinMediaDemuxInfoV1 verified;
    size_t offset = 0u;
    uint64_t timestamp = 0u;
    uint32_t profile = 0u;
    uint32_t sample_rate = 0u;
    uint32_t channels = 0u;
    if (!data || !info || !output || info->container_id !=
        RIN_MEDIA_CONTAINER_ADTS || info->track_count != 1u ||
        rin_media_container_inspect(data, source_bytes, &verified,
                                    sizeof(verified)) != RIN_MEDIA_DEMUX_OK)
        return 0;
    while (offset < source_bytes) {
        AdtsFrameHeader frame;
        RinMediaDemuxPacketV1 packet;
        if (output->packet_count >= RIN_MEDIA_DEMUX_MAX_PACKETS ||
            !adts_frame_header(data, source_bytes, offset, &frame))
            return 0;
        if (profile == 0u) {
            profile = frame.profile;
            sample_rate = frame.sample_rate;
            channels = frame.channels;
        } else if (profile != frame.profile || sample_rate != frame.sample_rate ||
                   channels != frame.channels) {
            return 0;
        }
        if (UINT64_MAX - timestamp < frame.samples)
            return 0;
        memset(&packet, 0, sizeof(packet));
        packet.byte_offset = offset;
        packet.byte_size = (uint32_t)frame.frame_bytes;
        packet.track_id = 1u;
        packet.timestamp_ticks = timestamp;
        packet.duration_ticks = frame.samples;
        output->packets[output->packet_count++] = packet;
        timestamp += frame.samples;
        offset += frame.frame_bytes;
    }
    return output->packet_count != 0u && offset == source_bytes &&
           timestamp == verified.duration_ticks;
}

static int flac_inspect(const uint8_t* data, size_t source_bytes,
                        RinMediaDemuxInfoV1* output)
{
    size_t offset = 4u;
    int found_streaminfo = 0;
    int found_last = 0;
    uint16_t min_block_size = 0u;
    uint16_t max_block_size = 0u;
    uint32_t sample_rate = 0u;
    uint32_t channels = 0u;
    uint32_t bits_per_sample = 0u;
    uint64_t total_samples = 0u;
    if (!data || !output || source_bytes < 4u ||
        memcmp(data, "fLaC", 4u) != 0)
        return RIN_MEDIA_DEMUX_INVALID;
    while (offset < source_bytes) {
        uint8_t block_header;
        uint32_t block_size;
        size_t block_end;
        if (source_bytes - offset < 4u) return RIN_MEDIA_DEMUX_INVALID;
        block_header = data[offset];
        block_size = ((uint32_t)data[offset + 1u] << 16u) |
                     ((uint32_t)data[offset + 2u] << 8u) |
                     (uint32_t)data[offset + 3u];
        offset += 4u;
        if (block_size > source_bytes - offset)
            return RIN_MEDIA_DEMUX_INVALID;
        block_end = offset + (size_t)block_size;
        if ((block_header & 0x7fu) > 6u)
            return RIN_MEDIA_DEMUX_UNSUPPORTED;
        if (!found_streaminfo && offset == 8u &&
            (block_header & 0x7fu) != 0u)
            return RIN_MEDIA_DEMUX_INVALID;
        if ((block_header & 0x7fu) == 0u) {
            const uint8_t* streaminfo = data + offset;
            uint64_t packed;
            if (found_streaminfo || block_size != 34u)
                return RIN_MEDIA_DEMUX_INVALID;
            min_block_size = (uint16_t)(((uint16_t)streaminfo[0] << 8u) |
                                        streaminfo[1]);
            max_block_size = (uint16_t)(((uint16_t)streaminfo[2] << 8u) |
                                        streaminfo[3]);
            if (min_block_size == 0u || max_block_size == 0u ||
                min_block_size > max_block_size)
                return RIN_MEDIA_DEMUX_INVALID;
            sample_rate = ((uint32_t)streaminfo[10] << 12u) |
                          ((uint32_t)streaminfo[11] << 4u) |
                          ((uint32_t)streaminfo[12] >> 4u);
            channels = (((uint32_t)streaminfo[12] & 0x0eu) >> 1u) + 1u;
            bits_per_sample = (((uint32_t)streaminfo[12] & 0x01u) << 4u) |
                               ((uint32_t)streaminfo[13] >> 4u);
            bits_per_sample += 1u;
            packed = read_be64(streaminfo + 10u);
            total_samples = packed & UINT64_C(0xfffffffff);
            if (sample_rate == 0u || sample_rate > 384000u ||
                channels == 0u || channels > 8u || bits_per_sample < 4u ||
                bits_per_sample > 32u)
                return RIN_MEDIA_DEMUX_INVALID;
            found_streaminfo = 1;
        }
        offset = block_end;
        if ((block_header & 0x80u) != 0u) {
            found_last = 1;
            break;
        }
    }
    if (!found_streaminfo || !found_last)
        return RIN_MEDIA_DEMUX_INVALID;
    memset(&output->tracks[0], 0, sizeof(output->tracks[0]));
    output->tracks[0].track_id = 1u;
    output->tracks[0].kind = RIN_MEDIA_DEMUX_TRACK_AUDIO;
    output->tracks[0].codec_id = UINT32_C(0x664c6143); /* fLaC */
    output->tracks[0].time_scale = sample_rate;
    output->tracks[0].duration_ticks = total_samples;
    memcpy(output->tracks[0].codec_name, "FLAC", 5u);
    output->track_count = 1u;
    output->time_scale = sample_rate;
    output->duration_ticks = total_samples;
    output->container_id = RIN_MEDIA_CONTAINER_FLAC;
    return RIN_MEDIA_DEMUX_OK;
}

typedef struct FlacFrameHeader {
    size_t header_end;
    uint32_t block_samples;
    uint64_t number;
    int variable_blocking;
} FlacFrameHeader;

static uint8_t flac_crc8(const uint8_t* data, size_t length)
{
    uint8_t crc = 0u;
    size_t index;
    for (index = 0u; index < length; ++index) {
        uint8_t bit;
        crc ^= data[index];
        for (bit = 0u; bit < 8u; ++bit)
            crc = (crc & 0x80u) != 0u ?
                (uint8_t)((crc << 1u) ^ 0x07u) :
                (uint8_t)(crc << 1u);
    }
    return crc;
}

static uint16_t flac_crc16(const uint8_t* data, size_t length)
{
    uint16_t crc = 0u;
    size_t index;
    for (index = 0u; index < length; ++index) {
        uint8_t bit;
        crc ^= (uint16_t)data[index] << 8u;
        for (bit = 0u; bit < 8u; ++bit)
            crc = (crc & UINT16_C(0x8000)) != 0u ?
                (uint16_t)((crc << 1u) ^ UINT16_C(0x8005)) :
                (uint16_t)(crc << 1u);
    }
    return crc;
}

static int flac_utf8_number(const uint8_t* data, size_t length,
                            uint64_t* value, size_t* consumed)
{
    uint8_t first;
    uint64_t result;
    uint64_t minimum;
    size_t count;
    size_t index;
    if (!data || !value || !consumed || length == 0u) return 0;
    first = data[0];
    if ((first & 0x80u) == 0u) {
        count = 1u;
        result = first;
        minimum = 0u;
    } else if (first >= 0xc2u && first <= 0xdfu) {
        count = 2u;
        result = first & 0x1fu;
        minimum = UINT64_C(0x80);
    } else if (first >= 0xe0u && first <= 0xefu) {
        count = 3u;
        result = first & 0x0fu;
        minimum = UINT64_C(0x800);
    } else if (first >= 0xf0u && first <= 0xf7u) {
        count = 4u;
        result = first & 0x07u;
        minimum = UINT64_C(0x10000);
    } else if (first >= 0xf8u && first <= 0xfbu) {
        count = 5u;
        result = first & 0x03u;
        minimum = UINT64_C(0x200000);
    } else if (first >= 0xfcu && first <= 0xfdu) {
        count = 6u;
        result = first & 0x01u;
        minimum = UINT64_C(0x4000000);
    } else {
        return 0;
    }
    if (length < count) return 0;
    for (index = 1u; index < count; ++index) {
        if ((data[index] & 0xc0u) != 0x80u) return 0;
        result = (result << 6u) | (uint64_t)(data[index] & 0x3fu);
    }
    if (result < minimum) return 0;
    *value = result;
    *consumed = count;
    return 1;
}

static int flac_frame_header(const uint8_t* data, size_t source_bytes,
                             size_t start, FlacFrameHeader* output)
{
    uint8_t block_code;
    uint8_t sample_rate_code;
    uint8_t channel_assignment;
    uint8_t sample_size_code;
    size_t offset;
    size_t number_bytes;
    uint64_t number;
    uint32_t block_samples;
    if (!data || !output || start > source_bytes ||
        source_bytes - start < 6u || data[start] != 0xffu ||
        (data[start + 1u] & 0xfeu) != 0xf8u ||
        (data[start + 3u] & 0x01u) != 0u)
        return 0;
    block_code = (uint8_t)(data[start + 2u] >> 4u);
    sample_rate_code = (uint8_t)(data[start + 2u] & 0x0fu);
    channel_assignment = (uint8_t)(data[start + 3u] >> 4u);
    sample_size_code = (uint8_t)((data[start + 3u] >> 1u) & 0x07u);
    if (block_code == 0u || sample_rate_code == 15u ||
        channel_assignment > 8u || sample_size_code == 3u)
        return 0;
    offset = start + 4u;
    if (!flac_utf8_number(data + offset, source_bytes - offset, &number,
                          &number_bytes))
        return 0;
    offset += number_bytes;
    switch (block_code) {
    case 1u: block_samples = 192u; break;
    case 2u: block_samples = 576u; break;
    case 3u: block_samples = 1152u; break;
    case 4u: block_samples = 2304u; break;
    case 5u: block_samples = 4608u; break;
    case 6u:
        if (source_bytes - offset < 1u) return 0;
        block_samples = (uint32_t)data[offset++] + 1u;
        break;
    case 7u:
        if (source_bytes - offset < 2u) return 0;
        block_samples = ((uint32_t)data[offset] << 8u) |
                        (uint32_t)data[offset + 1u];
        block_samples += 1u;
        offset += 2u;
        break;
    default:
        block_samples = UINT32_C(256) << (block_code - 8u);
        break;
    }
    switch (sample_rate_code) {
    case 12u:
        if (source_bytes - offset < 1u) return 0;
        ++offset;
        break;
    case 13u:
    case 14u:
        if (source_bytes - offset < 2u) return 0;
        offset += 2u;
        break;
    default:
        break;
    }
    if (source_bytes - offset < 1u ||
        flac_crc8(data + start, offset - start) != data[offset])
        return 0;
    output->header_end = offset + 1u;
    output->block_samples = block_samples;
    output->number = number;
    output->variable_blocking = (int)(data[start + 1u] & 0x01u);
    return 1;
}

static int flac_frame_crc_ok(const uint8_t* data, size_t source_bytes,
                             size_t start, size_t end)
{
    uint16_t stored;
    if (!data || start > end || end > source_bytes || end - start < 3u ||
        end - start > 1024u * 1024u)
        return 0;
    stored = (uint16_t)(((uint16_t)data[end - 2u] << 8u) |
                        data[end - 1u]);
    return flac_crc16(data + start, end - start - 2u) == stored;
}

static int flac_frame_end(const uint8_t* data, size_t source_bytes,
                          size_t start, size_t header_end, size_t* end)
{
    size_t candidate;
    size_t limit;
    if (!data || !end || start > source_bytes || header_end < start ||
        header_end > source_bytes || source_bytes - start < 3u)
        return 0;
    limit = source_bytes - start > 1024u * 1024u ?
        start + 1024u * 1024u : source_bytes;
    if (header_end > limit || limit - header_end < 2u) return 0;
    for (candidate = header_end + 2u; candidate + 3u <= limit; ++candidate) {
        FlacFrameHeader next;
        if (!flac_frame_header(data, source_bytes, candidate, &next)) continue;
        if (flac_frame_crc_ok(data, source_bytes, start, candidate)) {
            *end = candidate;
            return 1;
        }
    }
    if (limit == source_bytes &&
        flac_frame_crc_ok(data, source_bytes, start, source_bytes)) {
        *end = source_bytes;
        return 1;
    }
    return 0;
}

static int flac_index_packets(const uint8_t* data, size_t source_bytes,
                              const RinMediaDemuxInfoV1* info,
                              RinMediaDemuxPacketTableV1* output)
{
    RinMediaDemuxInfoV1 verified;
    size_t offset = 4u;
    uint64_t timestamp = 0u;
    if (!data || !info || !output || info->container_id !=
        RIN_MEDIA_CONTAINER_FLAC || info->track_count != 1u ||
        rin_media_container_inspect(data, source_bytes, &verified,
                                    sizeof(verified)) != RIN_MEDIA_DEMUX_OK)
        return 0;
    while (offset < source_bytes) {
        uint8_t header;
        uint32_t block_size;
        if (source_bytes - offset < 4u) return 0;
        header = data[offset];
        block_size = ((uint32_t)data[offset + 1u] << 16u) |
                     ((uint32_t)data[offset + 2u] << 8u) |
                     data[offset + 3u];
        offset += 4u;
        if (block_size > source_bytes - offset) return 0;
        offset += block_size;
        if ((header & 0x80u) != 0u) break;
    }
    if (offset >= source_bytes) return 0;
    while (offset < source_bytes) {
        FlacFrameHeader frame;
        size_t end;
        RinMediaDemuxPacketV1 packet;
        if (output->packet_count >= RIN_MEDIA_DEMUX_MAX_PACKETS ||
            !flac_frame_header(data, source_bytes, offset, &frame) ||
            !flac_frame_end(data, source_bytes, offset, frame.header_end,
                            &end) || end <= offset || end - offset >
                1024u * 1024u || frame.block_samples == 0u)
            return 0;
        if (frame.variable_blocking) {
            if (output->packet_count != 0u && frame.number < timestamp)
                return 0;
            timestamp = frame.number;
        } else if (UINT64_MAX - timestamp < frame.block_samples) {
            return 0;
        }
        memset(&packet, 0, sizeof(packet));
        packet.byte_offset = offset;
        packet.byte_size = (uint32_t)(end - offset);
        packet.track_id = 1u;
        packet.timestamp_ticks = frame.variable_blocking ? frame.number :
                                 timestamp;
        packet.duration_ticks = frame.block_samples;
        packet.flags = RIN_MEDIA_DEMUX_PACKET_KEYFRAME;
        output->packets[output->packet_count++] = packet;
        if (frame.variable_blocking) {
            if (UINT64_MAX - frame.number < frame.block_samples) return 0;
            timestamp = frame.number + frame.block_samples;
        } else {
            timestamp += frame.block_samples;
        }
        offset = end;
    }
    return output->packet_count != 0u &&
           (verified.duration_ticks == 0u ||
            timestamp == verified.duration_ticks);
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
                        uint64_t* id, size_t* payload, size_t* end,
                        int* unknown_size_out)
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
    if (unknown_size_out != NULL) *unknown_size_out = unknown_size;
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

static int ebml_nonnegative_finite(const uint8_t* data, size_t length)
{
    uint64_t raw;
    uint64_t exponent;
    if (!data || (length != 4u && length != 8u)) return 0;
    raw = length == 4u ? (uint64_t)read_be32(data) : read_be64(data);
    if ((raw >> (length == 4u ? 31u : 63u)) != 0u) return 0;
    exponent = length == 4u ? (raw >> 23u) & UINT64_C(0xff) :
                              (raw >> 52u) & UINT64_C(0x7ff);
    return exponent != (length == 4u ? UINT64_C(0xff) : UINT64_C(0x7ff));
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

static int webm_track_id_exists(const RinMediaDemuxInfoV1* output,
                                uint32_t track_id)
{
    uint32_t index;
    if (!output || track_id == 0u) return 0;
    for (index = 0u; index < output->track_count; ++index)
        if (output->tracks[index].track_id == track_id) return 1;
    return 0;
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
        int unknown_size = 0;
        if (!ebml_element(data, end, offset, &id, &payload, &element_end,
                          &unknown_size) || unknown_size)
            return 0;
        if (id == UINT64_C(0xd7)) {
            uint64_t track_number;
            if (got_number) return 0;
            if (element_end - payload == 0u || element_end - payload > 8u)
                return 0;
            track_number = ebml_uint(data + payload, element_end - payload);
            if (track_number == 0u || track_number > UINT32_MAX) return 0;
            track->track_id = (uint32_t)track_number;
            got_number = track->track_id != 0u;
        } else if (id == UINT64_C(0x83)) {
            uint64_t track_type;
            if (got_type) return 0;
            if (element_end - payload == 0u || element_end - payload > 8u)
                return 0;
            track_type = ebml_uint(data + payload, element_end - payload);
            track->kind = track_type == 1u ? RIN_MEDIA_DEMUX_TRACK_VIDEO :
                          track_type == 2u ? RIN_MEDIA_DEMUX_TRACK_AUDIO :
                          track_type == 17u ? RIN_MEDIA_DEMUX_TRACK_SUBTITLE :
                          RIN_MEDIA_DEMUX_TRACK_UNKNOWN;
            got_type = track->kind != RIN_MEDIA_DEMUX_TRACK_UNKNOWN;
        } else if (id == UINT64_C(0x86)) {
            if (got_codec) return 0;
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
        int unknown_size = 0;
        if (!ebml_element(data, end, offset, &id, &payload, &element_end,
                          &unknown_size) ||
            (unknown_size && id != UINT64_C(0x18538067) &&
             id != UINT64_C(0x1f43b675)))
            return 0;
        if (id == UINT64_C(0x1654ae6b)) { /* Tracks */
            size_t track_offset = payload;
            while (track_offset < element_end) {
                uint64_t track_id;
                size_t track_payload;
                size_t track_end;
                RinMediaDemuxTrackV1 track;
                int track_unknown_size = 0;
                if (!ebml_element(data, element_end, track_offset, &track_id,
                                  &track_payload, &track_end,
                                  &track_unknown_size) ||
                    track_unknown_size)
                    return 0;
                if (track_id == UINT64_C(0xae)) { /* TrackEntry */
                    if (output->track_count >= RIN_MEDIA_DEMUX_MAX_TRACKS)
                        return 0;
                    memset(&track, 0, sizeof(track));
                    if (!ebml_track(data, track_payload, track_end, &track) ||
                        webm_track_id_exists(output, track.track_id))
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
                int info_unknown_size = 0;
                if (!ebml_element(data, element_end, info_offset, &info_id,
                                  &info_payload, &info_end,
                                  &info_unknown_size) || info_unknown_size)
                    return 0;
                if (info_id == UINT64_C(0x2ad7b1)) {
                    if (info_end - info_payload == 0u ||
                        info_end - info_payload > 4u ||
                        output->time_scale != 0u)
                        return 0;
                    uint64_t scale = ebml_uint(data + info_payload,
                                               info_end - info_payload);
                    if (scale == 0u || scale > UINT32_MAX) return 0;
                    output->time_scale = (uint32_t)scale;
                } else if (info_id == UINT64_C(0x4489) &&
                           !ebml_nonnegative_finite(
                               data + info_payload,
                               info_end - info_payload)) {
                    return 0;
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
    uint32_t track_index;
    uint64_t id;
    size_t header_end;
    size_t offset;
    int found_segment = 0;
    int unknown_size = 0;
    if (rin_media_container_probe(data, source_bytes, &container) != 0 ||
        (container != RIN_MEDIA_CONTAINER_WEBM &&
         container != RIN_MEDIA_CONTAINER_MATROSKA))
        return RIN_MEDIA_DEMUX_INVALID;
    if (!ebml_element(data, source_bytes, 0u, &id, &offset, &header_end,
                      &unknown_size) || unknown_size ||
        id != UINT64_C(0x1a45dfa3))
        return RIN_MEDIA_DEMUX_INVALID;
    offset = header_end;
    while (offset < source_bytes) {
        size_t payload;
        size_t end;
        unknown_size = 0;
        if (!ebml_element(data, source_bytes, offset, &id, &payload, &end,
                          &unknown_size) ||
            (unknown_size && id != UINT64_C(0x18538067)))
            return RIN_MEDIA_DEMUX_INVALID;
        if (id == UINT64_C(0x18538067)) {
            if (found_segment) return RIN_MEDIA_DEMUX_INVALID;
            found_segment = 1;
            if (!webm_find_tracks(data, payload, end, output, 0))
                return RIN_MEDIA_DEMUX_INVALID;
        }
        offset = end;
    }
    if (!found_segment || output->track_count == 0u)
        return RIN_MEDIA_DEMUX_UNSUPPORTED;
    if (output->time_scale == 0u) output->time_scale = 1000000u;
    for (track_index = 0u; track_index < output->track_count; ++track_index)
        output->tracks[track_index].time_scale = output->time_scale;
    output->container_id = container;
    return RIN_MEDIA_DEMUX_OK;
}

int rin_media_container_inspect(const uint8_t* data, size_t source_bytes,
                                RinMediaDemuxInfoV1* output,
                                size_t output_size)
{
    int result;
    uint32_t container = 0u;
    if (output != NULL) memset(output, 0, sizeof(*output));
    if (data == NULL || source_bytes == 0u ||
        source_bytes > RIN_MEDIA_CONTAINER_PROBE_MAX_BYTES || output == NULL)
        return RIN_MEDIA_DEMUX_INVALID;
    if (output_size < sizeof(*output)) return RIN_MEDIA_DEMUX_OUTPUT_TOO_SMALL;
    output->struct_size = sizeof(*output);
    output->abi_version = RIN_MEDIA_DEMUX_ABI_V1;
    if (rin_media_container_probe(data, source_bytes, &container) == 0 &&
        container == RIN_MEDIA_CONTAINER_AVI)
        result = avi_inspect(data, source_bytes, output);
    else if (container == RIN_MEDIA_CONTAINER_WAV)
        result = wav_inspect(data, source_bytes, output);
    else if (container == RIN_MEDIA_CONTAINER_FLAC)
        result = flac_inspect(data, source_bytes, output);
    else if (container == RIN_MEDIA_CONTAINER_OGG)
        result = ogg_inspect(data, source_bytes, output);
    else if (container == RIN_MEDIA_CONTAINER_ADTS)
        result = adts_inspect(data, source_bytes, output);
    else if (source_bytes >= 8u &&
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
            if (run == 0u || run > RIN_MEDIA_DEMUX_MAX_PACKETS ||
                sample > RIN_MEDIA_DEMUX_MAX_PACKETS - run)
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
        if (length < 8u || tables->has_ctts ||
            !mp4_table_count(data, length, &entry_count) ||
            (size_t)entry_count > (length - 8u) / 8u ||
            data[0] > 1u)
            return -1;
        version = data[0];
        for (index = 0u; index < entry_count; ++index) {
            uint32_t run = read_be32(data + 8u + index * 8u);
            uint32_t raw = read_be32(data + 12u + index * 8u);
            int64_t offset = version == 1u ? (int64_t)(int32_t)raw :
                                            (int64_t)raw;
            uint32_t item;
            if (run == 0u || run > RIN_MEDIA_DEMUX_MAX_PACKETS ||
                sample > RIN_MEDIA_DEMUX_MAX_PACKETS - run)
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
                entry->sample_description_index != 1u ||
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

static int avi_index_packets(const uint8_t* data, size_t source_bytes,
                             const RinMediaDemuxInfoV1* info,
                             RinMediaDemuxPacketTableV1* output)
{
    RAviContext context;
    uint64_t timestamps[RAVI_MAX_STREAMS] = {0};
    uint32_t container = 0u;
    size_t index;
    if (rin_media_container_probe(data, source_bytes, &container) != 0 ||
        container != RIN_MEDIA_CONTAINER_AVI ||
        ravi_open(&context, data, source_bytes) != RAVI_OK ||
        context.idx1_offset == 0u || context.idx1_count == 0u)
        return 0;
    if (context.idx1_offset > source_bytes ||
        context.idx1_count > (source_bytes - context.idx1_offset) /
            sizeof(RAviIndexEntry))
        return 0;
    for (index = 0u; index < context.idx1_count; ++index) {
        const uint8_t* entry = data + context.idx1_offset +
                               index * sizeof(RAviIndexEntry);
        uint32_t chunk_id = read_le32(entry + 0u);
        uint32_t flags = read_le32(entry + 4u);
        uint32_t offset = read_le32(entry + 8u);
        uint32_t size = read_le32(entry + 12u);
        int stream_index;
        const uint8_t* payload = 0;
        uint32_t duration;
        RinMediaDemuxPacketV1 packet;
        if (!ravi_is_video_chunk(chunk_id) && !ravi_is_audio_chunk(chunk_id))
            continue;
        stream_index = ravi_get_stream_index(chunk_id);
        if (stream_index < 0 || stream_index >= context.stream_count ||
            media_track_index(info, (uint32_t)stream_index + 1u) < 0 ||
            size == 0u || size > 1024u * 1024u ||
            !ravi_frame_payload(&context, offset, size, &payload) ||
            payload < data || (size_t)(payload - data) > source_bytes ||
            output->packet_count >= RIN_MEDIA_DEMUX_MAX_PACKETS)
            return 0;
        duration = context.streams[stream_index].scale == 0u ?
            1u : context.streams[stream_index].scale;
        if (UINT64_MAX - timestamps[stream_index] < duration)
            return 0;
        memset(&packet, 0, sizeof(packet));
        packet.byte_offset = (uint64_t)(payload - data);
        packet.byte_size = size;
        packet.track_id = (uint32_t)stream_index + 1u;
        packet.timestamp_ticks = timestamps[stream_index];
        packet.duration_ticks = duration;
        packet.flags = (!ravi_is_audio_chunk(chunk_id) &&
                        (flags & UINT32_C(0x10)) != 0u) ?
            RIN_MEDIA_DEMUX_PACKET_KEYFRAME : 0u;
        output->packets[output->packet_count++] = packet;
        timestamps[stream_index] += duration;
    }
    return output->packet_count != 0u;
}

static int wav_index_packets(const uint8_t* data, size_t source_bytes,
                             const RinMediaDemuxInfoV1* info,
                             RinMediaDemuxPacketTableV1* output)
{
    RWavContext context;
    uint32_t container = 0u;
    size_t block_count;
    size_t index;
    uint32_t samples_per_block = 1u;
    uint64_t timestamp = 0u;
    if (rin_media_container_probe(data, source_bytes, &container) != 0 ||
        container != RIN_MEDIA_CONTAINER_WAV ||
        rwav_open(&context, data, source_bytes) != RWAV_OK ||
        media_track_index(info, 1u) < 0 || context.data_offset > source_bytes ||
        context.data_size > source_bytes - context.data_offset ||
        context.block_align == 0u ||
        context.data_size % context.block_align != 0u)
        return 0;
    if (context.format == RWAV_FORMAT_IMA_ADPCM) {
        size_t channels = context.channels;
        size_t payload_bytes;
        if (channels == 0u || context.block_align < channels * 4u)
            return 0;
        payload_bytes = (size_t)context.block_align - channels * 4u;
        samples_per_block = (uint32_t)(payload_bytes * 2u / channels + 1u);
        if (samples_per_block == 0u)
            return 0;
    }
    block_count = context.data_size / context.block_align;
    if (block_count == 0u || block_count > RIN_MEDIA_DEMUX_MAX_PACKETS)
        return 0;
    for (index = 0u; index < block_count; ++index) {
        RinMediaDemuxPacketV1 packet;
        if (UINT64_MAX - timestamp < samples_per_block)
            return 0;
        memset(&packet, 0, sizeof(packet));
        packet.byte_offset = (uint64_t)context.data_offset +
                             (uint64_t)index * context.block_align;
        packet.byte_size = context.block_align;
        packet.track_id = 1u;
        packet.timestamp_ticks = timestamp;
        packet.duration_ticks = samples_per_block;
        output->packets[output->packet_count++] = packet;
        timestamp += samples_per_block;
    }
    return output->packet_count != 0u;
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
                uint32_t seen_fields = 0u;
                Mp4SampleTables tables;
                if (!iso_box(data, box.end, child, &track_box)) return 0;
                if (track_box.type == UINT32_C(0x7472616b)) {
                    memset(&track, 0, sizeof(track));
                    if (!mp4_find_track_fields(data, track_box.payload,
                                               track_box.end, &track, 0,
                                               &seen_fields) ||
                        seen_fields != UINT32_C(0x0f))
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

static int webm_lace_sizes(const uint8_t* data, size_t begin, size_t end,
                           uint8_t flags, uint32_t* sizes,
                           uint32_t* count, size_t* payload_begin)
{
    uint8_t lace_type = flags & 0x06u;
    uint32_t index;
    size_t cursor = begin;
    size_t remaining;
    if (!data || !sizes || !count || !payload_begin || begin > end)
        return 0;
    if (lace_type == 0u) {
        if (end == begin || end - begin > UINT32_MAX) return 0;
        *count = 1u;
        sizes[0] = (uint32_t)(end - begin);
        *payload_begin = begin;
        return sizes[0] != 0u;
    }
    if (end - cursor < 1u) return 0;
    *count = (uint32_t)data[cursor++] + 1u;
    if (*count == 0u || *count > RIN_MEDIA_DEMUX_MAX_PACKETS)
        return 0;
    remaining = end - cursor;
    if (lace_type == 0x04u) { /* fixed-size lacing */
        if (remaining == 0u || remaining % *count != 0u ||
            remaining / *count > UINT32_MAX)
            return 0;
        for (index = 0u; index < *count; ++index)
            sizes[index] = (uint32_t)(remaining / *count);
        *payload_begin = cursor;
        return sizes[0] != 0u;
    }
    if (lace_type == 0x02u) { /* Xiph lacing */
        for (index = 0u; index + 1u < *count; ++index) {
            uint32_t size = 0u;
            uint8_t part;
            do {
                if (cursor >= end) return 0;
                part = data[cursor++];
                if (size > UINT32_MAX - (uint32_t)part) return 0;
                size += part;
            } while (part == 0xffu);
            if ((uint64_t)size > (uint64_t)(end - cursor)) return 0;
            sizes[index] = size;
            remaining = end - cursor - size;
        }
        if (remaining == 0u || remaining > UINT32_MAX) return 0;
        sizes[*count - 1u] = (uint32_t)remaining;
        *payload_begin = cursor;
        for (index = 0u; index < *count; ++index)
            if (sizes[index] == 0u) return 0;
        return 1;
    }
    /* EBML lacing stores the first size as an unsigned vint and subsequent
     * sizes as signed deltas with a vint-width-dependent bias. */
    {
        uint64_t previous;
        uint64_t total = 0u;
        uint64_t value;
        size_t vint_bytes;
        int unknown;
        if (!ebml_vint(data + cursor, end - cursor, &value, &vint_bytes,
                       &unknown) || unknown || value == 0u ||
            value > UINT32_MAX)
            return 0;
        sizes[0] = (uint32_t)value;
        previous = value;
        total = value;
        cursor += vint_bytes;
        for (index = 1u; index + 1u < *count; ++index) {
            uint64_t bias;
            int64_t delta;
            if (!ebml_vint(data + cursor, end - cursor, &value, &vint_bytes,
                           &unknown) || unknown || vint_bytes > 8u)
                return 0;
            bias = (UINT64_C(1) << (7u * vint_bytes - 1u)) - 1u;
            delta = value >= bias ? (int64_t)(value - bias) :
                                    -(int64_t)(bias - value);
            if (delta < 0) {
                if ((uint64_t)(-delta) > previous) return 0;
                previous -= (uint64_t)(-delta);
            } else {
                if (UINT64_MAX - previous < (uint64_t)delta) return 0;
                previous += (uint64_t)delta;
            }
            cursor += vint_bytes;
            if (previous == 0u || previous > UINT32_MAX ||
                UINT64_MAX - total < previous)
                return 0;
            total += previous;
            sizes[index] = (uint32_t)previous;
        }
        if (cursor >= end || total >= (uint64_t)(end - cursor) ||
            end - cursor - (size_t)total > UINT32_MAX)
            return 0;
        sizes[*count - 1u] = (uint32_t)((end - cursor) - (size_t)total);
        *payload_begin = cursor;
        for (index = 0u; index < *count; ++index)
            if (sizes[index] == 0u) return 0;
        return 1;
    }
}

static int webm_append_block(const uint8_t* data, size_t payload,
                             size_t element_end, uint64_t cluster_timecode,
                             const RinMediaDemuxInfoV1* info,
                             RinMediaDemuxPacketTableV1* output,
                             int keyframe_override,
                             int has_duration,
                             uint32_t duration_ticks)
{
    uint32_t track_id;
    size_t track_bytes;
    int16_t relative;
    uint8_t flags;
    uint32_t lace_sizes[RIN_MEDIA_DEMUX_MAX_PACKETS];
    uint32_t lace_count;
    size_t packet_data;
    uint32_t lace_index;
    if (!data || !info || !output || payload > element_end ||
        element_end - payload < 4u ||
        !webm_track_number(data + payload, element_end - payload,
                           &track_id, &track_bytes) ||
        track_bytes > element_end - payload - 3u)
        return 0;
    relative = (int16_t)(((uint16_t)data[payload + track_bytes] << 8u) |
                         data[payload + track_bytes + 1u]);
    flags = data[payload + track_bytes + 2u];
    if (!webm_lace_sizes(data, payload + track_bytes + 3u, element_end,
                         flags, lace_sizes, &lace_count, &packet_data))
        return 0;
    if (media_track_index(info, track_id) < 0 || lace_count >
        RIN_MEDIA_DEMUX_MAX_PACKETS - output->packet_count)
        return 0;
    if (has_duration && lace_count != 1u)
        return 0;
    if (relative < 0 && (uint64_t)(-(int64_t)relative) > cluster_timecode)
        return 0;
    if (relative >= 0 &&
        UINT64_MAX - cluster_timecode < (uint64_t)relative)
        return 0;
    for (lace_index = 0u; lace_index < lace_count; ++lace_index) {
        RinMediaDemuxPacketV1 packet;
        memset(&packet, 0, sizeof(packet));
        packet.byte_offset = packet_data;
        packet.byte_size = lace_sizes[lace_index];
        packet.track_id = track_id;
        packet.timestamp_ticks = relative < 0 ?
            cluster_timecode - (uint64_t)(-(int64_t)relative) :
            cluster_timecode + (uint64_t)relative;
        packet.duration_ticks = has_duration ? duration_ticks : 0u;
        packet.flags = (keyframe_override < 0 ? (flags & 0x80u) != 0u :
                        keyframe_override != 0) ?
            RIN_MEDIA_DEMUX_PACKET_KEYFRAME : 0u;
        if (packet.byte_size == 0u || packet.byte_size > 1024u * 1024u)
            return 0;
        output->packets[output->packet_count++] = packet;
        packet_data += packet.byte_size;
    }
    return 1;
}

static int webm_index_block_group(const uint8_t* data, size_t begin,
                                  size_t end, uint64_t cluster_timecode,
                                  const RinMediaDemuxInfoV1* info,
                                  RinMediaDemuxPacketTableV1* output)
{
    size_t offset = begin;
    size_t block_payload = 0u;
    size_t block_end = 0u;
    uint32_t block_duration = 0u;
    int have_block = 0;
    int have_duration = 0;
    int has_reference = 0;
    while (offset < end) {
        uint64_t id;
        size_t payload;
        size_t element_end;
        int unknown_size = 0;
        if (!ebml_element(data, end, offset, &id, &payload, &element_end,
                          &unknown_size) || unknown_size)
            return 0;
        if (id == UINT64_C(0xa1)) { /* Block */
            if (have_block) return 0;
            block_payload = payload;
            block_end = element_end;
            have_block = 1;
        } else if (id == UINT64_C(0xfb)) { /* ReferenceBlock */
            const size_t length = element_end - payload;
            if (length == 0u || length > 8u) return 0;
            has_reference = 1;
        } else if (id == UINT64_C(0x9b)) { /* BlockDuration */
            const size_t length = element_end - payload;
            const uint64_t value = length == 0u || length > 8u ?
                UINT64_MAX : ebml_uint(data + payload, length);
            if (have_duration || value > UINT32_MAX) return 0;
            block_duration = (uint32_t)value;
            have_duration = 1;
        }
        offset = element_end;
    }
    if (!have_block || offset != end) return 0;
    return webm_append_block(data, block_payload, block_end,
                             cluster_timecode, info, output,
                             has_reference ? 0 : 1, have_duration,
                             block_duration);
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
        int unknown_size = 0;
        if (!ebml_element(data, end, offset, &id, &payload, &element_end,
                          &unknown_size) || unknown_size)
            return 0;
        if (id == UINT64_C(0xe7)) {
            size_t length = element_end - payload;
            if (length == 0u || length > 8u) return 0;
            cluster_timecode = ebml_uint(data + payload, length);
            have_timecode = 1;
        } else if (id == UINT64_C(0xa3)) { /* SimpleBlock */
            if (!have_timecode || !webm_append_block(
                    data, payload, element_end, cluster_timecode, info,
                    output, -1, 0, 0u))
                return 0;
        } else if (id == UINT64_C(0xa0)) { /* BlockGroup */
            if (!have_timecode || !webm_index_block_group(
                    data, payload, element_end, cluster_timecode, info,
                    output))
                return 0;
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
    int unknown_size = 0;
    if (!ebml_element(data, source_bytes, 0u, &id, &payload, &end,
                      &unknown_size) || unknown_size ||
        id != UINT64_C(0x1a45dfa3)) return 0;
    offset = end;
    while (offset < source_bytes) {
        unknown_size = 0;
        if (!ebml_element(data, source_bytes, offset, &id, &payload, &end,
                          &unknown_size) ||
            (unknown_size && id != UINT64_C(0x18538067)))
            return 0;
        if (id == UINT64_C(0x18538067)) {
            size_t segment = payload;
            while (segment < end) {
                size_t child_payload;
                size_t child_end;
                int child_unknown_size = 0;
                if (!ebml_element(data, end, segment, &id, &child_payload,
                                  &child_end, &child_unknown_size) ||
                    (child_unknown_size && id != UINT64_C(0x1f43b675)))
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
        info->container_id != RIN_MEDIA_CONTAINER_MATROSKA &&
        info->container_id != RIN_MEDIA_CONTAINER_OGG &&
        info->container_id != RIN_MEDIA_CONTAINER_AVI &&
        info->container_id != RIN_MEDIA_CONTAINER_WAV &&
        info->container_id != RIN_MEDIA_CONTAINER_FLAC &&
        info->container_id != RIN_MEDIA_CONTAINER_ADTS)
        return RIN_MEDIA_DEMUX_UNSUPPORTED;
    output->struct_size = sizeof(*output);
    output->abi_version = RIN_MEDIA_DEMUX_ABI_V1;
    result = info->container_id == RIN_MEDIA_CONTAINER_MP4 ?
        mp4_index_packets(data, source_bytes, info, output) :
        info->container_id == RIN_MEDIA_CONTAINER_OGG ?
        (info->tracks[0].codec_id == RIN_MEDIA_CODEC_OPUS ?
         ogg_opus_index_packets(data, source_bytes, info, output) :
         ogg_vorbis_index_packets(data, source_bytes, info, output)) :
        info->container_id == RIN_MEDIA_CONTAINER_AVI ?
        avi_index_packets(data, source_bytes, info, output) :
        info->container_id == RIN_MEDIA_CONTAINER_WAV ?
        wav_index_packets(data, source_bytes, info, output) :
        info->container_id == RIN_MEDIA_CONTAINER_FLAC ?
        flac_index_packets(data, source_bytes, info, output) :
        info->container_id == RIN_MEDIA_CONTAINER_ADTS ?
        adts_index_packets(data, source_bytes, info, output) :
        webm_index_packets(data, source_bytes, info, output);
    if (!result || output->packet_count == 0u) {
        memset(output, 0, sizeof(*output));
        return RIN_MEDIA_DEMUX_UNSUPPORTED;
    }
    sort_packets(output);
    return RIN_MEDIA_DEMUX_OK;
}

int rin_media_container_copy_packet(
    const uint8_t* data, size_t source_bytes,
    const RinMediaDemuxPacketV1* packet, size_t packet_size,
    uint8_t* output, size_t output_capacity, size_t* bytes_copied)
{
    size_t clear_bytes;
    if (bytes_copied) *bytes_copied = 0u;
    if (!data || source_bytes == 0u ||
        source_bytes > RIN_MEDIA_CONTAINER_PROBE_MAX_BYTES || !packet ||
        packet_size < sizeof(*packet) || packet->byte_size == 0u ||
        packet->byte_size > RIN_MEDIA_DEMUX_MAX_PACKET_BYTES ||
        packet->byte_offset > (uint64_t)source_bytes ||
        packet->byte_size > source_bytes - (size_t)packet->byte_offset) {
        clear_bytes = output_capacity < RIN_MEDIA_DEMUX_MAX_PACKET_BYTES ?
            output_capacity : RIN_MEDIA_DEMUX_MAX_PACKET_BYTES;
        if (output && clear_bytes != 0u) memset(output, 0, clear_bytes);
        return RIN_MEDIA_DEMUX_INVALID;
    }
    if (!output || output_capacity < (size_t)packet->byte_size ||
        !bytes_copied) {
        clear_bytes = output_capacity < RIN_MEDIA_DEMUX_MAX_PACKET_BYTES ?
            output_capacity : RIN_MEDIA_DEMUX_MAX_PACKET_BYTES;
        if (output && clear_bytes != 0u) memset(output, 0, clear_bytes);
        return RIN_MEDIA_DEMUX_OUTPUT_TOO_SMALL;
    }
    memmove(output, data + (size_t)packet->byte_offset,
            (size_t)packet->byte_size);
    *bytes_copied = (size_t)packet->byte_size;
    return RIN_MEDIA_DEMUX_OK;
}

int rin_media_container_read_packet_with_cancellation(
    const RinMediaDemuxPacketV1* packet, size_t packet_size,
    RinMediaDemuxPacketReadV1 read_source, void* context,
    RinMediaDemuxPacketCancelledV1 cancelled, void* cancellation_context,
    uint8_t* output, size_t output_capacity, size_t* bytes_read)
{
    size_t offset = 0u;
    size_t clear_bytes;
    int cancellation_status;
    if (!bytes_read) {
        clear_bytes = output_capacity < RIN_MEDIA_DEMUX_MAX_PACKET_BYTES ?
            output_capacity : RIN_MEDIA_DEMUX_MAX_PACKET_BYTES;
        if (output && clear_bytes != 0u) memset(output, 0, clear_bytes);
        return RIN_MEDIA_DEMUX_INVALID;
    }
    *bytes_read = 0u;
    if (!packet || packet_size < sizeof(*packet) || !read_source ||
        packet->byte_size == 0u ||
        packet->byte_size > RIN_MEDIA_DEMUX_MAX_PACKET_BYTES ||
        packet->byte_offset > UINT64_MAX - (uint64_t)packet->byte_size) {
        clear_bytes = output_capacity < RIN_MEDIA_DEMUX_MAX_PACKET_BYTES ?
            output_capacity : RIN_MEDIA_DEMUX_MAX_PACKET_BYTES;
        if (output && clear_bytes != 0u) memset(output, 0, clear_bytes);
        return RIN_MEDIA_DEMUX_INVALID;
    }
    if (!output || output_capacity < (size_t)packet->byte_size) {
        clear_bytes = output_capacity < RIN_MEDIA_DEMUX_MAX_PACKET_BYTES ?
            output_capacity : RIN_MEDIA_DEMUX_MAX_PACKET_BYTES;
        if (output && clear_bytes != 0u) memset(output, 0, clear_bytes);
        return RIN_MEDIA_DEMUX_OUTPUT_TOO_SMALL;
    }
    while (offset < (size_t)packet->byte_size) {
        size_t request = (size_t)packet->byte_size - offset;
        size_t received = 0u;
        int result;
        if (request > RIN_MEDIA_DEMUX_PACKET_READ_CHUNK_BYTES)
            request = RIN_MEDIA_DEMUX_PACKET_READ_CHUNK_BYTES;
        if (cancelled != NULL) {
            cancellation_status = cancelled(cancellation_context);
            if (cancellation_status == 1) {
                memset(output, 0, (size_t)packet->byte_size);
                return RIN_MEDIA_DEMUX_CANCELLED;
            }
            if (cancellation_status != 0) {
                memset(output, 0, (size_t)packet->byte_size);
                return RIN_MEDIA_DEMUX_SOURCE_FAILED;
            }
        }
        result = read_source(
            context, packet->byte_offset + (uint64_t)offset,
            output + offset, request, &received);
        if (result != 0) {
            memset(output, 0, (size_t)packet->byte_size);
            return RIN_MEDIA_DEMUX_SOURCE_FAILED;
        }
        if (received > request) {
            memset(output, 0, (size_t)packet->byte_size);
            return RIN_MEDIA_DEMUX_INVALID;
        }
        if (received == 0u) {
            memset(output, 0, (size_t)packet->byte_size);
            return RIN_MEDIA_DEMUX_SOURCE_STALLED;
        }
        /* The source owns only the reported prefix of this request.  Clear
         * the rest even if a buggy source wrote into the remainder, so a
         * caller-reused packet buffer cannot expose unreported bytes. */
        if (received < request)
            memset(output + offset + received, 0, request - received);
        if (cancelled != NULL) {
            cancellation_status = cancelled(cancellation_context);
            if (cancellation_status == 1) {
                memset(output, 0, (size_t)packet->byte_size);
                return RIN_MEDIA_DEMUX_CANCELLED;
            }
            if (cancellation_status != 0) {
                memset(output, 0, (size_t)packet->byte_size);
                return RIN_MEDIA_DEMUX_SOURCE_FAILED;
            }
        }
        offset += received;
    }
    *bytes_read = offset;
    return RIN_MEDIA_DEMUX_OK;
}

int rin_media_container_read_packet(
    const RinMediaDemuxPacketV1* packet, size_t packet_size,
    RinMediaDemuxPacketReadV1 read_source, void* context,
    uint8_t* output, size_t output_capacity, size_t* bytes_read)
{
    return rin_media_container_read_packet_with_cancellation(
        packet, packet_size, read_source, context, NULL, NULL, output,
        output_capacity, bytes_read);
}
