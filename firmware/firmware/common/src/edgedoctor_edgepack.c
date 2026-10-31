#include "edgedoctor_edgepack.h"

#include <limits.h>
#include <string.h>

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static int16_t read_i16(const uint8_t *data)
{
    return (int16_t)read_u16(data);
}

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

uint32_t edgedoctor_crc32_init(void)
{
    return UINT32_C(0xFFFFFFFF);
}

uint32_t edgedoctor_crc32_update(uint32_t state, const void *data, size_t bytes)
{
    const uint8_t *cursor = (const uint8_t *)data;
    uint32_t crc = state;
    size_t index;
    unsigned int bit;

    for (index = 0U; index < bytes; ++index)
    {
        crc ^= cursor[index];
        for (bit = 0U; bit < 8U; ++bit)
        {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (UINT32_C(0xEDB88320) & mask);
        }
    }
    return crc;
}

uint32_t edgedoctor_crc32_finish(uint32_t state)
{
    return ~state;
}

uint32_t edgedoctor_crc32(const void *data, size_t bytes)
{
    if ((data == NULL) && (bytes != 0U))
    {
        return 0U;
    }
    return edgedoctor_crc32_finish(
        edgedoctor_crc32_update(edgedoctor_crc32_init(), data, bytes));
}

int edgedoctor_edgepack_parse_header(const void *data,
                                     size_t bytes,
                                     edgedoctor_edgepack_view_t *view)
{
    const uint8_t *raw = (const uint8_t *)data;
    edgedoctor_edgepack_header_t header;
    uint64_t expected_payload;
    uint32_t audio_per_chunk;
    uint32_t vibration_per_chunk;
    uint32_t chunk_count;

    if ((data == NULL) || (view == NULL))
    {
        return EDGEDOCTOR_EDGEPACK_ARGUMENT;
    }
    if (bytes < EDGEDOCTOR_EDGEPACK_HEADER_BYTES)
    {
        return EDGEDOCTOR_EDGEPACK_TRUNCATED;
    }
    if (memcmp(raw, "EDGP", 4U) != 0)
    {
        return EDGEDOCTOR_EDGEPACK_MAGIC;
    }
    if ((read_u16(raw + 4U) != 1U) ||
        (read_u16(raw + 6U) != EDGEDOCTOR_EDGEPACK_HEADER_BYTES))
    {
        return EDGEDOCTOR_EDGEPACK_VERSION;
    }
    if (raw[29U] != 2U)
    {
        return EDGEDOCTOR_EDGEPACK_RANGE;
    }

    header.recording_id = read_u32(raw + 8U);
    header.dataset_id = read_u16(raw + 12U);
    header.label = raw[14U];
    header.severity = raw[15U];
    header.load_x10_nm = read_i16(raw + 16U);
    header.chunk_ms = read_u16(raw + 18U);
    header.audio_rate = read_u32(raw + 20U);
    header.vibration_rate = read_u32(raw + 24U);
    header.vibration_channels = raw[28U];
    header.audio_samples = read_u32(raw + 32U);
    header.vibration_frames = read_u32(raw + 36U);
    header.payload_bytes = read_u32(raw + 40U);
    header.payload_crc32 = read_u32(raw + 44U);

    if ((header.label >= 5U) ||
        (header.chunk_ms == 0U) ||
        ((1000U % header.chunk_ms) != 0U) ||
        (header.audio_rate == 0U) ||
        (header.vibration_rate == 0U) ||
        (header.vibration_channels == 0U) ||
        (header.vibration_channels > 4U))
    {
        return EDGEDOCTOR_EDGEPACK_RANGE;
    }
    if ((((uint64_t)header.audio_rate * header.chunk_ms) % 1000U) != 0U ||
        (((uint64_t)header.vibration_rate * header.chunk_ms) % 1000U) != 0U)
    {
        return EDGEDOCTOR_EDGEPACK_RANGE;
    }
    audio_per_chunk = header.audio_rate * header.chunk_ms / 1000U;
    vibration_per_chunk = header.vibration_rate * header.chunk_ms / 1000U;
    if ((header.audio_samples == 0U) ||
        ((header.audio_samples % audio_per_chunk) != 0U))
    {
        return EDGEDOCTOR_EDGEPACK_RANGE;
    }
    chunk_count = header.audio_samples / audio_per_chunk;
    if (header.vibration_frames != chunk_count * vibration_per_chunk)
    {
        return EDGEDOCTOR_EDGEPACK_RANGE;
    }
    expected_payload = 2ULL *
        ((uint64_t)header.audio_samples +
         (uint64_t)header.vibration_frames * header.vibration_channels);
    if ((expected_payload > UINT32_MAX) ||
        (header.payload_bytes != (uint32_t)expected_payload))
    {
        return EDGEDOCTOR_EDGEPACK_LENGTH;
    }

    view->header = header;
    view->payload = NULL;
    view->file_bytes = EDGEDOCTOR_EDGEPACK_HEADER_BYTES + (size_t)header.payload_bytes;
    view->chunk_count = chunk_count;
    view->audio_samples_per_chunk = audio_per_chunk;
    view->vibration_frames_per_chunk = vibration_per_chunk;
    view->chunk_bytes = 2U *
        (audio_per_chunk + vibration_per_chunk * header.vibration_channels);
    return EDGEDOCTOR_EDGEPACK_OK;
}

int edgedoctor_edgepack_open(const void *data,
                             size_t bytes,
                             edgedoctor_edgepack_view_t *view)
{
    const uint8_t *raw = (const uint8_t *)data;
    int status = edgedoctor_edgepack_parse_header(data, bytes, view);
    if (status != EDGEDOCTOR_EDGEPACK_OK)
    {
        return status;
    }
    if (bytes != view->file_bytes)
    {
        return EDGEDOCTOR_EDGEPACK_LENGTH;
    }
    if (edgedoctor_crc32(raw + EDGEDOCTOR_EDGEPACK_HEADER_BYTES,
                         view->header.payload_bytes) != view->header.payload_crc32)
    {
        return EDGEDOCTOR_EDGEPACK_CRC;
    }
    view->payload = raw + EDGEDOCTOR_EDGEPACK_HEADER_BYTES;
    return EDGEDOCTOR_EDGEPACK_OK;
}

int edgedoctor_edgepack_get_chunk(const edgedoctor_edgepack_view_t *view,
                                  uint32_t index,
                                  edgedoctor_edgepack_chunk_view_t *chunk)
{
    const uint8_t *start;
    uint32_t audio_bytes;

    if ((view == NULL) || (chunk == NULL) || (view->payload == NULL))
    {
        return EDGEDOCTOR_EDGEPACK_ARGUMENT;
    }
    if (index >= view->chunk_count)
    {
        return EDGEDOCTOR_EDGEPACK_RANGE;
    }
    start = view->payload + (size_t)index * view->chunk_bytes;
    audio_bytes = view->audio_samples_per_chunk * 2U;
    chunk->audio = (const int16_t *)(const void *)start;
    chunk->vibration = (const int16_t *)(const void *)(start + audio_bytes);
    chunk->audio_samples = view->audio_samples_per_chunk;
    chunk->vibration_frames = view->vibration_frames_per_chunk;
    chunk->vibration_channels = view->header.vibration_channels;
    return EDGEDOCTOR_EDGEPACK_OK;
}
