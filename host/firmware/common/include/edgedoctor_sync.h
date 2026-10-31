#ifndef EDGEDOCTOR_SYNC_H
#define EDGEDOCTOR_SYNC_H

#include "edgedoctor_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EDGEDOCTOR_SYNC_CAPACITY 8U

typedef struct
{
    uint32_t frame_id;
    uint64_t timestamp_us;
    uint16_t count;
    int16_t samples[EDGEDOCTOR_AUDIO_CHUNK_SAMPLES];
} edgedoctor_audio_item_t;

typedef struct
{
    uint32_t frame_id;
    uint64_t timestamp_us;
    uint16_t frames;
    int16_t samples[EDGEDOCTOR_VIBRATION_CHUNK_FRAMES]
                   [EDGEDOCTOR_VIBRATION_CHANNELS];
} edgedoctor_vibration_item_t;

typedef struct
{
    edgedoctor_audio_item_t audio[EDGEDOCTOR_SYNC_CAPACITY];
    edgedoctor_vibration_item_t vibration[EDGEDOCTOR_SYNC_CAPACITY];
    uint8_t audio_read;
    uint8_t audio_count;
    uint8_t vibration_read;
    uint8_t vibration_count;
    uint32_t dropped_audio;
    uint32_t dropped_vibration;
    uint32_t mismatched_frames;
} edgedoctor_sync_t;

void edgedoctor_sync_init(edgedoctor_sync_t *sync);
int edgedoctor_sync_push_audio(edgedoctor_sync_t *sync,
                               uint32_t frame_id,
                               uint64_t timestamp_us,
                               const int16_t *samples,
                               uint16_t count);
int edgedoctor_sync_push_vibration(edgedoctor_sync_t *sync,
                                   uint32_t frame_id,
                                   uint64_t timestamp_us,
                                   const int16_t *samples,
                                   uint16_t frames);
int edgedoctor_sync_pop(edgedoctor_sync_t *sync, edgedoctor_chunk_t *chunk);
uint16_t edgedoctor_sync_depth(const edgedoctor_sync_t *sync);

#ifdef __cplusplus
}
#endif

#endif

