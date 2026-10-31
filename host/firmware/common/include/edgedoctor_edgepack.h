#ifndef EDGEDOCTOR_EDGEPACK_H
#define EDGEDOCTOR_EDGEPACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDGEDOCTOR_EDGEPACK_HEADER_BYTES 64U

typedef enum
{
    EDGEDOCTOR_EDGEPACK_OK = 0,
    EDGEDOCTOR_EDGEPACK_ARGUMENT = -1,
    EDGEDOCTOR_EDGEPACK_TRUNCATED = -2,
    EDGEDOCTOR_EDGEPACK_MAGIC = -3,
    EDGEDOCTOR_EDGEPACK_VERSION = -4,
    EDGEDOCTOR_EDGEPACK_RANGE = -5,
    EDGEDOCTOR_EDGEPACK_LENGTH = -6,
    EDGEDOCTOR_EDGEPACK_CRC = -7
} edgedoctor_edgepack_status_t;

typedef struct
{
    uint32_t recording_id;
    uint16_t dataset_id;
    uint8_t label;
    uint8_t severity;
    int16_t load_x10_nm;
    uint16_t chunk_ms;
    uint32_t audio_rate;
    uint32_t vibration_rate;
    uint8_t vibration_channels;
    uint32_t audio_samples;
    uint32_t vibration_frames;
    uint32_t payload_bytes;
    uint32_t payload_crc32;
} edgedoctor_edgepack_header_t;

typedef struct
{
    edgedoctor_edgepack_header_t header;
    const uint8_t *payload;
    size_t file_bytes;
    uint32_t chunk_count;
    uint32_t audio_samples_per_chunk;
    uint32_t vibration_frames_per_chunk;
    uint32_t chunk_bytes;
} edgedoctor_edgepack_view_t;

typedef struct
{
    const int16_t *audio;
    const int16_t *vibration;
    uint32_t audio_samples;
    uint32_t vibration_frames;
    uint8_t vibration_channels;
} edgedoctor_edgepack_chunk_view_t;

int edgedoctor_edgepack_open(const void *data,
                             size_t bytes,
                             edgedoctor_edgepack_view_t *view);

int edgedoctor_edgepack_parse_header(const void *data,
                                     size_t bytes,
                                     edgedoctor_edgepack_view_t *view);

int edgedoctor_edgepack_get_chunk(const edgedoctor_edgepack_view_t *view,
                                  uint32_t index,
                                  edgedoctor_edgepack_chunk_view_t *chunk);

uint32_t edgedoctor_crc32(const void *data, size_t bytes);
uint32_t edgedoctor_crc32_init(void);
uint32_t edgedoctor_crc32_update(uint32_t state, const void *data, size_t bytes);
uint32_t edgedoctor_crc32_finish(uint32_t state);

#ifdef __cplusplus
}
#endif

#endif
