/* SPDX-License-Identifier: MIT */

#include "rinmedia_flac.h"

#include <limits.h>
#include <string.h>

typedef struct BitReader {
    const uint8_t* data;
    size_t bytes;
    size_t bit;
} BitReader;

static uint8_t crc8(const uint8_t* data, size_t length)
{
    uint8_t result = 0u;
    size_t index;
    for (index = 0u; index < length; ++index) {
        uint8_t bit;
        result ^= data[index];
        for (bit = 0u; bit < 8u; ++bit)
            result = (result & 0x80u) != 0u ?
                (uint8_t)((result << 1u) ^ 0x07u) :
                (uint8_t)(result << 1u);
    }
    return result;
}

static uint16_t crc16(const uint8_t* data, size_t length)
{
    uint16_t result = 0u;
    size_t index;
    for (index = 0u; index < length; ++index) {
        uint8_t bit;
        result ^= (uint16_t)data[index] << 8u;
        for (bit = 0u; bit < 8u; ++bit)
            result = (result & UINT16_C(0x8000)) != 0u ?
                (uint16_t)((result << 1u) ^ UINT16_C(0x8005)) :
                (uint16_t)(result << 1u);
    }
    return result;
}

static int add_i64(int64_t left, int64_t right, int64_t* result)
{
    if (result == NULL) return 0;
    if ((right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right))
        return 0;
    *result = left + right;
    return 1;
}

static int subtract_i64(int64_t left, int64_t right, int64_t* result)
{
    if (result == NULL) return 0;
    if ((right > 0 && left < INT64_MIN + right) ||
        (right < 0 && left > INT64_MAX + right))
        return 0;
    *result = left - right;
    return 1;
}

static int multiply_i64(int64_t left, int64_t right, int64_t* result)
{
    if (result == NULL) return 0;
    if (left == 0 || right == 0) {
        *result = 0;
        return 1;
    }
    if (left == -1) {
        if (right == INT64_MIN) return 0;
        *result = -right;
        return 1;
    }
    if (right == -1) {
        if (left == INT64_MIN) return 0;
        *result = -left;
        return 1;
    }
    if (left > 0) {
        if (right > 0) {
            if (left > INT64_MAX / right) return 0;
        } else if (right < INT64_MIN / left) {
            return 0;
        }
    } else if (right > 0) {
        if (left < INT64_MIN / right) return 0;
    } else if (left < INT64_MAX / right) {
        return 0;
    }
    *result = left * right;
    return 1;
}

static int arithmetic_shift_right(int64_t value, unsigned amount,
                                  int64_t* result)
{
    int64_t divisor;
    int64_t quotient;
    if (result == NULL || amount >= 63u) return 0;
    if (amount == 0u) {
        *result = value;
        return 1;
    }
    divisor = INT64_C(1) << amount;
    quotient = value / divisor;
    if (value < 0 && value % divisor != 0) --quotient;
    *result = quotient;
    return 1;
}

static int read_bits64(BitReader* reader, unsigned count, uint64_t* value)
{
    uint64_t result = 0u;
    size_t total_bits;
    unsigned index;
    if (reader == NULL || reader->data == NULL || value == NULL ||
        count > 64u || reader->bytes > SIZE_MAX / 8u)
        return 0;
    total_bits = reader->bytes * 8u;
    if (reader->bit > total_bits || (size_t)count > total_bits - reader->bit)
        return 0;
    for (index = 0u; index < count; ++index) {
        const size_t bit = reader->bit++;
        result = (result << 1u) |
                 (uint32_t)((reader->data[bit / 8u] >> (7u - bit % 8u)) &
                            1u);
    }
    *value = result;
    return 1;
}

static int read_bits(BitReader* reader, unsigned count, uint32_t* value)
{
    uint64_t result;
    if (value == NULL || count > 32u || !read_bits64(reader, count, &result))
        return 0;
    *value = (uint32_t)result;
    return 1;
}

static int read_signed_bits(BitReader* reader, unsigned count, int64_t* value)
{
    uint64_t raw;
    if (value == NULL || count == 0u || count > 33u ||
        !read_bits64(reader, count, &raw))
        return 0;
    if ((raw & (UINT64_C(1) << (count - 1u))) != 0u)
        *value = (int64_t)raw - (INT64_C(1) << count);
    else
        *value = (int64_t)raw;
    return 1;
}

static int read_unary(BitReader* reader, uint32_t* value)
{
    uint32_t bit;
    uint32_t count = 0u;
    if (reader == NULL || value == NULL) return 0;
    for (;;) {
        if (!read_bits(reader, 1u, &bit)) return 0;
        if (bit != 0u) break;
        if (count == UINT32_MAX) return 0;
        ++count;
    }
    *value = count;
    return 1;
}

static int read_utf8_number(const uint8_t* data, size_t length,
                            uint64_t* value, size_t* consumed)
{
    uint8_t first;
    uint64_t result;
    uint64_t minimum;
    size_t count;
    size_t index;
    if (data == NULL || value == NULL || consumed == NULL || length == 0u)
        return 0;
    first = data[0];
    if ((first & 0x80u) == 0u) {
        count = 1u; result = first; minimum = 0u;
    } else if (first >= 0xc2u && first <= 0xdfu) {
        count = 2u; result = first & 0x1fu; minimum = UINT64_C(0x80);
    } else if (first >= 0xe0u && first <= 0xefu) {
        count = 3u; result = first & 0x0fu; minimum = UINT64_C(0x800);
    } else if (first >= 0xf0u && first <= 0xf7u) {
        count = 4u; result = first & 0x07u; minimum = UINT64_C(0x10000);
    } else if (first >= 0xf8u && first <= 0xfbu) {
        count = 5u; result = first & 0x03u; minimum = UINT64_C(0x200000);
    } else if (first >= 0xfcu && first <= 0xfdu) {
        count = 6u; result = first & 0x01u; minimum = UINT64_C(0x4000000);
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

static int signed_range(uint32_t bits, int64_t* minimum, int64_t* maximum)
{
    if (minimum == NULL || maximum == NULL || bits == 0u || bits > 33u)
        return 0;
    if (bits == 32u) {
        *minimum = INT32_MIN;
        *maximum = INT32_MAX;
    } else {
        *minimum = -(INT64_C(1) << (bits - 1u));
        *maximum = (INT64_C(1) << (bits - 1u)) - 1;
    }
    return 1;
}

static int value_in_range(int64_t value, uint32_t bits)
{
    int64_t minimum;
    int64_t maximum;
    return signed_range(bits, &minimum, &maximum) && value >= minimum &&
           value <= maximum;
}

/* FLAC's mid/side reconstruction uses an arithmetic right shift.  C integer
 * division truncates negative odd values toward zero, so spell out the
 * floor-by-two operation for the bounded side sample range. */
static int64_t arithmetic_shift_right_one(int64_t value)
{
    int64_t result = value / 2;
    if (value < 0 && value % 2 != 0) --result;
    return result;
}

static int read_rice(BitReader* reader, unsigned parameter, int64_t* value)
{
    uint32_t quotient;
    uint32_t remainder = 0u;
    uint64_t unsigned_value;
    if (value == NULL || parameter > 31u || !read_unary(reader, &quotient) ||
        !read_bits(reader, parameter, &remainder))
        return 0;
    unsigned_value = ((uint64_t)quotient << parameter) | remainder;
    if (unsigned_value > UINT64_C(0xffffffff)) return 0;
    *value = (unsigned_value & 1u) != 0u ?
        -(int64_t)((unsigned_value + 1u) / 2u) :
        (int64_t)(unsigned_value / 2u);
    return 1;
}

static int decode_residuals(BitReader* reader, int64_t* samples,
                            uint32_t block_samples, uint32_t order)
{
    uint32_t method;
    uint32_t partition_order;
    uint32_t partition;
    size_t sample = order;
    if (reader == NULL || samples == NULL || order > block_samples ||
        !read_bits(reader, 2u, &method) || method > 1u ||
        !read_bits(reader, 4u, &partition_order) || partition_order > 15u)
        return 0;
    if (partition_order != 0u && block_samples < (UINT32_C(1) << partition_order))
        return 0;
    for (partition = 0u; partition < (UINT32_C(1) << partition_order);
         ++partition) {
        uint32_t parameter;
        uint32_t count = block_samples >> partition_order;
        uint32_t residual_count;
        uint32_t index;
        if (partition == 0u && order > count) return 0;
        residual_count = count - (partition == 0u ? order : 0u);
        if (!read_bits(reader, method == 0u ? 4u : 5u, &parameter))
            return 0;
        if ((method == 0u && parameter == 15u) ||
            (method == 1u && parameter == 31u)) {
            uint32_t raw_bits;
            if (!read_bits(reader, 5u, &raw_bits) || raw_bits > 31u)
                return 0;
            for (index = 0u; index < residual_count; ++index) {
                int64_t residual = 0;
                if (raw_bits != 0u &&
                    !read_signed_bits(reader, raw_bits, &residual)) return 0;
                samples[sample++] = residual;
            }
        } else {
            for (index = 0u; index < residual_count; ++index) {
                int64_t residual;
                if (!read_rice(reader, parameter, &residual)) return 0;
                samples[sample++] = residual;
            }
        }
    }
    return sample == block_samples;
}

static int decode_subframe(BitReader* reader, int64_t* samples,
                           uint32_t block_samples, uint32_t bits_per_sample)
{
    uint32_t zero;
    uint32_t type;
    uint32_t wasted_flag;
    uint32_t wasted = 0u;
    uint32_t effective_bits = bits_per_sample;
    uint32_t index;
    if (reader == NULL || samples == NULL || block_samples == 0u ||
        !read_bits(reader, 1u, &zero) || zero != 0u ||
        !read_bits(reader, 6u, &type) || !read_bits(reader, 1u, &wasted_flag))
        return 0;
    if (wasted_flag != 0u) {
        if (!read_unary(reader, &wasted) || wasted == 0u ||
            wasted >= bits_per_sample)
            return 0;
        effective_bits -= wasted;
    }
    if (type == 0u) {
        int64_t value;
        if (!read_signed_bits(reader, effective_bits, &value)) return 0;
        for (index = 0u; index < block_samples; ++index)
            samples[index] = value;
    } else if (type == 1u) {
        for (index = 0u; index < block_samples; ++index)
            if (!read_signed_bits(reader, effective_bits, &samples[index]))
                return 0;
    } else if (type >= 8u && type <= 12u) {
        const uint32_t order = type - 8u;
        for (index = 0u; index < order; ++index)
            if (!read_signed_bits(reader, effective_bits, &samples[index]))
                return 0;
        if (!decode_residuals(reader, samples, block_samples, order)) return 0;
        for (index = order; index < block_samples; ++index) {
            int64_t prediction;
            int64_t term;
            switch (order) {
            case 0u: prediction = 0; break;
            case 1u: prediction = samples[index - 1u]; break;
            case 2u:
                if (!multiply_i64(2, samples[index - 1u], &term) ||
                    !subtract_i64(term, samples[index - 2u], &prediction))
                    return 0;
                break;
            case 3u:
                if (!multiply_i64(3, samples[index - 1u], &term) ||
                    !multiply_i64(3, samples[index - 2u], &prediction) ||
                    !subtract_i64(term, prediction, &prediction) ||
                    !add_i64(prediction, samples[index - 3u], &prediction))
                    return 0;
                break;
            default:
                if (!multiply_i64(4, samples[index - 1u], &term) ||
                    !multiply_i64(6, samples[index - 2u], &prediction) ||
                    !subtract_i64(term, prediction, &prediction) ||
                    !multiply_i64(4, samples[index - 3u], &term) ||
                    !add_i64(prediction, term, &prediction) ||
                    !subtract_i64(prediction, samples[index - 4u],
                                  &prediction))
                    return 0;
                break;
            }
            if (!add_i64(samples[index], prediction, &samples[index]))
                return 0;
        }
    } else if (type >= 32u) {
        const uint32_t order = (type & 31u) + 1u;
        int32_t coefficients[32];
        int32_t shift;
        if (order > 32u || order > block_samples) return 0;
        for (index = 0u; index < order; ++index)
            if (!read_signed_bits(reader, effective_bits, &samples[index]))
                return 0;
        {
            uint32_t precision;
            if (!read_bits(reader, 4u, &precision) || precision == 15u)
                return 0;
            precision += 1u;
            for (index = 0u; index < order; ++index) {
                int64_t coefficient;
                if (!read_signed_bits(reader, precision, &coefficient)) return 0;
                coefficients[index] = (int32_t)coefficient;
            }
        }
        {
            int64_t shift_value;
            if (!read_signed_bits(reader, 5u, &shift_value)) return 0;
            shift = (int32_t)shift_value;
        }
        if (!decode_residuals(reader, samples, block_samples, order)) return 0;
        for (index = order; index < block_samples; ++index) {
            int64_t sum = 0;
            uint32_t coefficient_index;
            for (coefficient_index = 0u; coefficient_index < order;
                 ++coefficient_index) {
                int64_t product;
                if (!multiply_i64((int64_t)coefficients[coefficient_index],
                                  samples[index - coefficient_index - 1u],
                                  &product) ||
                    !add_i64(sum, product, &sum))
                    return 0;
            }
            if (shift >= 0) {
                const unsigned amount = (unsigned)shift;
                if (!arithmetic_shift_right(sum, amount, &sum)) return 0;
            } else {
                const unsigned amount = (unsigned)(-shift);
                if (amount >= 63u ||
                    !multiply_i64(sum, INT64_C(1) << amount, &sum))
                    return 0;
            }
            if (!add_i64(samples[index], sum, &samples[index])) return 0;
        }
    } else {
        return 0;
    }
    for (index = 0u; index < block_samples; ++index) {
        if (wasted != 0u) {
            if (wasted >= 63u ||
                !multiply_i64(samples[index], INT64_C(1) << wasted,
                              &samples[index]))
                return 0;
        }
        if (!value_in_range(samples[index], bits_per_sample)) return 0;
    }
    return 1;
}

static void clear_output(int32_t* output, size_t output_samples)
{
    if (output != NULL && output_samples <=
        (size_t)RIN_MEDIA_FLAC_MAX_OUTPUT_SAMPLES)
        memset(output, 0, output_samples * sizeof(*output));
}

int rin_media_flac_decode_frame(
    const uint8_t* frame, size_t frame_bytes,
    const RinMediaFlacDecodeRequestV1* request, int64_t* scratch,
    size_t scratch_samples, int32_t* output,
    size_t output_samples, size_t* samples_written,
    uint32_t* block_samples_out)
{
    BitReader reader;
    uint64_t frame_number;
    size_t number_bytes;
    size_t offset;
    uint32_t block_code;
    uint32_t sample_rate_code;
    uint32_t channel_assignment;
    uint32_t sample_size_code;
    uint32_t block_samples;
    uint32_t channels;
    uint32_t channel;
    int result = RIN_MEDIA_FLAC_DECODE_MALFORMED;

    if (samples_written != NULL) *samples_written = 0u;
    if (block_samples_out != NULL) *block_samples_out = 0u;
    clear_output(output, output_samples);
    if (frame == NULL || request == NULL || scratch == NULL || output == NULL ||
        samples_written == NULL || block_samples_out == NULL ||
        frame_bytes < 6u || frame_bytes > RIN_MEDIA_FLAC_MAX_FRAME_BYTES ||
        request->struct_size != sizeof(*request) ||
        request->abi_version != RIN_MEDIA_FLAC_ABI_V1 || request->reserved0 != 0u ||
        request->reserved1 != 0u || request->channels == 0u ||
        request->channels > RIN_MEDIA_FLAC_MAX_CHANNELS ||
        request->bits_per_sample < 4u || request->bits_per_sample > 32u)
        return RIN_MEDIA_FLAC_DECODE_INVALID;
    if (frame[0] != 0xffu || (frame[1] & 0xfeu) != 0xf8u ||
        (frame[3] & 1u) != 0u)
        return RIN_MEDIA_FLAC_DECODE_MALFORMED;
    block_code = frame[2] >> 4u;
    sample_rate_code = frame[2] & 0x0fu;
    channel_assignment = frame[3] >> 4u;
    sample_size_code = (frame[3] >> 1u) & 7u;
    if (block_code == 0u || sample_rate_code == 15u ||
        channel_assignment > 10u || sample_size_code == 3u ||
        sample_size_code == 7u)
        return RIN_MEDIA_FLAC_DECODE_UNSUPPORTED;
    channels = channel_assignment <= 7u ? channel_assignment + 1u : 2u;
    if (channels != request->channels ||
        (sample_size_code != 0u &&
         ((sample_size_code == 1u ? 8u : sample_size_code == 2u ? 12u :
           sample_size_code == 4u ? 16u : sample_size_code == 5u ? 20u : 24u) !=
          request->bits_per_sample)))
        return RIN_MEDIA_FLAC_DECODE_MALFORMED;
    offset = 4u;
    if (!read_utf8_number(frame + offset, frame_bytes - offset, &frame_number,
                          &number_bytes))
        return RIN_MEDIA_FLAC_DECODE_MALFORMED;
    (void)frame_number;
    offset += number_bytes;
    switch (block_code) {
    case 1u: block_samples = 192u; break;
    case 2u: block_samples = 576u; break;
    case 3u: block_samples = 1152u; break;
    case 4u: block_samples = 2304u; break;
    case 5u: block_samples = 4608u; break;
    case 6u:
        if (offset >= frame_bytes) return RIN_MEDIA_FLAC_DECODE_MALFORMED;
        block_samples = (uint32_t)frame[offset++] + 1u;
        break;
    case 7u:
        if (frame_bytes - offset < 2u) return RIN_MEDIA_FLAC_DECODE_MALFORMED;
        block_samples = ((uint32_t)frame[offset] << 8u) |
                        frame[offset + 1u];
        block_samples += 1u;
        offset += 2u;
        break;
    default:
        block_samples = UINT32_C(256) << (block_code - 8u);
        break;
    }
    if (block_samples == 0u || block_samples > RIN_MEDIA_FLAC_MAX_BLOCK_SAMPLES)
        return RIN_MEDIA_FLAC_DECODE_LIMIT;
    if (sample_rate_code == 12u) {
        if (offset >= frame_bytes) return RIN_MEDIA_FLAC_DECODE_MALFORMED;
        ++offset;
    } else if (sample_rate_code == 13u || sample_rate_code == 14u) {
        if (frame_bytes - offset < 2u) return RIN_MEDIA_FLAC_DECODE_MALFORMED;
        offset += 2u;
    }
    if (offset >= frame_bytes || crc8(frame, offset) != frame[offset])
        return RIN_MEDIA_FLAC_DECODE_MALFORMED;
    ++offset;
    if (channels > SIZE_MAX / block_samples ||
        (size_t)channels * block_samples > output_samples ||
        (size_t)channels * block_samples > scratch_samples)
        return RIN_MEDIA_FLAC_DECODE_OUTPUT_TOO_SMALL;
    if (frame_bytes - offset < 2u) return RIN_MEDIA_FLAC_DECODE_MALFORMED;
    reader.data = frame + offset;
    reader.bytes = frame_bytes - offset - 2u;
    reader.bit = 0u;
    for (channel = 0u; channel < channels; ++channel) {
        uint32_t channel_bits = request->bits_per_sample;
        if ((channel_assignment == 8u && channel == 1u) ||
            (channel_assignment == 9u && channel == 1u) ||
            (channel_assignment == 10u && channel == 1u))
            channel_bits += 1u;
        if (channel_bits > 33u || !decode_subframe(
                &reader, scratch + (size_t)channel * block_samples,
                block_samples, channel_bits))
            return RIN_MEDIA_FLAC_DECODE_MALFORMED;
    }
    if (reader.bit % 8u != 0u) {
        uint32_t padding;
        const unsigned count = 8u - (unsigned)(reader.bit % 8u);
        if (!read_bits(&reader, count, &padding) || padding != 0u)
            return RIN_MEDIA_FLAC_DECODE_MALFORMED;
    }
    if (reader.bit / 8u != reader.bytes ||
        crc16(frame, frame_bytes - 2u) !=
            (uint16_t)(((uint16_t)frame[frame_bytes - 2u] << 8u) |
                       frame[frame_bytes - 1u]))
        return RIN_MEDIA_FLAC_DECODE_MALFORMED;
    for (uint32_t sample = 0u; sample < block_samples; ++sample) {
        int64_t values[RIN_MEDIA_FLAC_MAX_CHANNELS] = {0};
        for (channel = 0u; channel < channels; ++channel)
            values[channel] = scratch[(size_t)channel * block_samples + sample];
        if (channel_assignment == 8u) {
            if (!subtract_i64(values[0], values[1], &values[1]))
                return RIN_MEDIA_FLAC_DECODE_MALFORMED;
        } else if (channel_assignment == 9u) {
            if (!add_i64(values[1], values[0], &values[0]))
                return RIN_MEDIA_FLAC_DECODE_MALFORMED;
        }
        else if (channel_assignment == 10u) {
            const int64_t side = values[1];
            const int64_t mid = values[0];
            const int64_t side_shifted = arithmetic_shift_right_one(side);
            const int64_t side_parity = side % 2 != 0 ? 1 : 0;
            if (!add_i64(mid, side_parity, &values[0]) ||
                !add_i64(values[0], side_shifted, &values[0]) ||
                !subtract_i64(mid, side_shifted, &values[1]))
                return RIN_MEDIA_FLAC_DECODE_MALFORMED;
        }
        for (channel = 0u; channel < channels; ++channel)
            if (!value_in_range(values[channel], request->bits_per_sample))
                return RIN_MEDIA_FLAC_DECODE_MALFORMED;
        for (channel = 0u; channel < channels; ++channel)
            output[(size_t)sample * channels + channel] = (int32_t)values[channel];
    }
    *samples_written = (size_t)channels * block_samples;
    *block_samples_out = block_samples;
    result = RIN_MEDIA_FLAC_DECODE_OK;
    return result;
}

int rin_media_flac_decode_packet_table(
    const uint8_t* source, size_t source_bytes,
    const RinMediaDemuxInfoV1* info,
    const RinMediaDemuxPacketTableV1* packets,
    const RinMediaFlacDecodeRequestV1* request, int64_t* scratch,
    size_t scratch_samples, int32_t* output, size_t output_samples,
    size_t* samples_written)
{
    const uint32_t flac_fourcc = UINT32_C(0x664c6143);
    size_t total_samples = 0u;
    uint64_t total_duration = 0u;
    uint64_t expected_timestamp = 0u;
    uint64_t previous_end = 0u;
    uint32_t packet_index;

    if (samples_written != NULL) *samples_written = 0u;
    clear_output(output, output_samples);
    if (source == NULL || source_bytes == 0u ||
        source_bytes > RIN_MEDIA_CONTAINER_PROBE_MAX_BYTES || info == NULL ||
        packets == NULL || request == NULL || scratch == NULL ||
        output == NULL || samples_written == NULL ||
        output_samples > (size_t)RIN_MEDIA_FLAC_MAX_OUTPUT_SAMPLES ||
        info->struct_size != sizeof(*info) ||
        info->abi_version != RIN_MEDIA_DEMUX_ABI_V1 ||
        info->container_id != RIN_MEDIA_CONTAINER_FLAC ||
        info->track_count != 1u || info->tracks[0].track_id == 0u ||
        info->tracks[0].kind != RIN_MEDIA_DEMUX_TRACK_AUDIO ||
        info->tracks[0].codec_id != flac_fourcc ||
        info->tracks[0].time_scale == 0u ||
        packets->struct_size != sizeof(*packets) ||
        packets->abi_version != RIN_MEDIA_DEMUX_ABI_V1 ||
        packets->reserved0 != 0u || packets->packet_count == 0u ||
        packets->packet_count > RIN_MEDIA_DEMUX_MAX_PACKETS ||
        request->struct_size != sizeof(*request) ||
        request->abi_version != RIN_MEDIA_FLAC_ABI_V1)
        return RIN_MEDIA_FLAC_DECODE_INVALID;

    for (packet_index = 0u; packet_index < packets->packet_count;
         ++packet_index) {
        const RinMediaDemuxPacketV1* packet = &packets->packets[packet_index];
        uint64_t packet_end;
        size_t frame_samples = 0u;
        uint32_t block_samples = 0u;
        int result;
        if (packet->track_id != info->tracks[0].track_id ||
            packet->byte_size == 0u ||
            packet->byte_size > RIN_MEDIA_FLAC_MAX_FRAME_BYTES ||
            packet->byte_offset > (uint64_t)source_bytes ||
            packet->byte_size > source_bytes -
                                    (size_t)packet->byte_offset ||
            (packet_index != 0u && packet->byte_offset < previous_end) ||
            packet->timestamp_ticks != expected_timestamp ||
            packet->duration_ticks == 0u ||
            UINT64_MAX - packet->byte_offset < packet->byte_size)
            goto malformed;
        packet_end = packet->byte_offset + packet->byte_size;
        previous_end = packet_end;
        if (UINT64_MAX - total_duration < packet->duration_ticks)
            goto malformed;
        total_duration += packet->duration_ticks;
        expected_timestamp = total_duration;
        if (total_samples > output_samples)
            goto output_too_small;
        result = rin_media_flac_decode_frame(
            source + (size_t)packet->byte_offset, packet->byte_size, request,
            scratch, scratch_samples, output + total_samples,
            output_samples - total_samples, &frame_samples,
            &block_samples);
        if (result != RIN_MEDIA_FLAC_DECODE_OK) {
            clear_output(output, output_samples);
            return result;
        }
        if (frame_samples != (size_t)request->channels * block_samples ||
            packet->duration_ticks != block_samples ||
            frame_samples > output_samples - total_samples ||
            total_samples > (size_t)RIN_MEDIA_FLAC_MAX_OUTPUT_SAMPLES -
                                 frame_samples)
            goto malformed;
        total_samples += frame_samples;
    }
    if (info->duration_ticks != 0u && total_duration != info->duration_ticks)
        goto malformed;
    *samples_written = total_samples;
    return RIN_MEDIA_FLAC_DECODE_OK;

output_too_small:
    clear_output(output, output_samples);
    return RIN_MEDIA_FLAC_DECODE_OUTPUT_TOO_SMALL;
malformed:
    clear_output(output, output_samples);
    return RIN_MEDIA_FLAC_DECODE_MALFORMED;
}
