#ifndef EDGEDOCTOR_WINDOW_H
#define EDGEDOCTOR_WINDOW_H

#include "edgedoctor_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint32_t first_frame_id;
    uint32_t last_frame_id;
    uint64_t timestamp_us;
    int16_t audio[EDGEDOCTOR_AUDIO_WINDOW_SAMPLES];
    int16_t vibration[EDGEDOCTOR_VIBRATION_WINDOW_FRAMES]
                     [EDGEDOCTOR_VIBRATION_CHANNELS];
} edgedoctor_window_buffer_t;

typedef struct
{
    int16_t audio[EDGEDOCTOR_AUDIO_WINDOW_SAMPLES];
    int16_t vibration[EDGEDOCTOR_VIBRATION_WINDOW_FRAMES]
                     [EDGEDOCTOR_VIBRATION_CHANNELS];
    uint32_t last_frame_id;
    uint64_t last_timestamp_us;
    uint32_t accepted_chunks;
    uint32_t discontinuities;
    uint8_t write_chunk;
    uint8_t valid_chunks;
    uint8_t chunks_since_emit;
    uint8_t initialized;
} edgedoctor_window_t;

void edgedoctor_window_init(edgedoctor_window_t *window);
int edgedoctor_window_push(edgedoctor_window_t *window,
                           const edgedoctor_chunk_t *chunk);
int edgedoctor_window_materialize(const edgedoctor_window_t *window,
                                  edgedoctor_window_buffer_t *output);

#ifdef __cplusplus
}
#endif

#endif
