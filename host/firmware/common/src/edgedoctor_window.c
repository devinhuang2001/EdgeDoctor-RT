#include "edgedoctor_window.h"

#include <string.h>

void edgedoctor_window_init(edgedoctor_window_t *window)
{
    if (window != NULL)
    {
        memset(window, 0, sizeof(*window));
    }
}

static void copy_chunk(edgedoctor_window_t *window,
                       const edgedoctor_chunk_t *chunk)
{
    size_t audio_offset = (size_t)window->write_chunk * EDGEDOCTOR_AUDIO_CHUNK_SAMPLES;
    size_t vibration_offset = (size_t)window->write_chunk * EDGEDOCTOR_VIBRATION_CHUNK_FRAMES;
    memcpy(&window->audio[audio_offset], chunk->audio, sizeof(chunk->audio));
    memcpy(&window->vibration[vibration_offset], chunk->vibration, sizeof(chunk->vibration));
    window->write_chunk = (uint8_t)((window->write_chunk + 1U) % EDGEDOCTOR_WINDOW_CHUNKS);
}

int edgedoctor_window_push(edgedoctor_window_t *window,
                           const edgedoctor_chunk_t *chunk)
{
    uint8_t discontinuity = 0U;
    if ((window == NULL) || (chunk == NULL) ||
        (chunk->audio_count != EDGEDOCTOR_AUDIO_CHUNK_SAMPLES) ||
        (chunk->vibration_frames != EDGEDOCTOR_VIBRATION_CHUNK_FRAMES))
    {
        return -1;
    }
    if ((window->initialized != 0U) &&
        ((chunk->frame_id != window->last_frame_id + 1U) ||
         (chunk->timestamp_us <= window->last_timestamp_us)))
    {
        window->write_chunk = 0U;
        window->valid_chunks = 0U;
        window->chunks_since_emit = 0U;
        window->discontinuities++;
        discontinuity = 1U;
    }
    copy_chunk(window, chunk);
    window->last_frame_id = chunk->frame_id;
    window->last_timestamp_us = chunk->timestamp_us;
    window->accepted_chunks++;
    window->initialized = 1U;
    if (window->valid_chunks < EDGEDOCTOR_WINDOW_CHUNKS)
    {
        window->valid_chunks++;
    }
    window->chunks_since_emit++;
    if ((window->valid_chunks == EDGEDOCTOR_WINDOW_CHUNKS) &&
        ((window->accepted_chunks == EDGEDOCTOR_WINDOW_CHUNKS) ||
         (window->chunks_since_emit >= EDGEDOCTOR_HOP_CHUNKS)))
    {
        window->chunks_since_emit = 0U;
        return (discontinuity != 0U) ? 2 : 1;
    }
    return 0;
}

int edgedoctor_window_materialize(const edgedoctor_window_t *window,
                                  edgedoctor_window_buffer_t *output)
{
    uint32_t destination_chunk;
    if ((window == NULL) || (output == NULL) ||
        (window->valid_chunks != EDGEDOCTOR_WINDOW_CHUNKS))
    {
        return 0;
    }
    for (destination_chunk = 0U;
         destination_chunk < EDGEDOCTOR_WINDOW_CHUNKS;
         ++destination_chunk)
    {
        uint32_t source_chunk =
            ((uint32_t)window->write_chunk + destination_chunk) % EDGEDOCTOR_WINDOW_CHUNKS;
        memcpy(&output->audio[destination_chunk * EDGEDOCTOR_AUDIO_CHUNK_SAMPLES],
               &window->audio[source_chunk * EDGEDOCTOR_AUDIO_CHUNK_SAMPLES],
               EDGEDOCTOR_AUDIO_CHUNK_SAMPLES * sizeof(int16_t));
        memcpy(&output->vibration[destination_chunk * EDGEDOCTOR_VIBRATION_CHUNK_FRAMES],
               &window->vibration[source_chunk * EDGEDOCTOR_VIBRATION_CHUNK_FRAMES],
               EDGEDOCTOR_VIBRATION_CHUNK_FRAMES * EDGEDOCTOR_VIBRATION_CHANNELS *
                   sizeof(int16_t));
    }
    output->last_frame_id = window->last_frame_id;
    output->first_frame_id = window->last_frame_id - EDGEDOCTOR_WINDOW_CHUNKS + 1U;
    output->timestamp_us = window->last_timestamp_us;
    return 1;
}
