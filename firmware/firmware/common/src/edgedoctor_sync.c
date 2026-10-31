#include "edgedoctor_sync.h"

#include <string.h>

void edgedoctor_sync_init(edgedoctor_sync_t *sync)
{
    if (sync != NULL)
    {
        memset(sync, 0, sizeof(*sync));
    }
}

int edgedoctor_sync_push_audio(edgedoctor_sync_t *sync,
                               uint32_t frame_id,
                               uint64_t timestamp_us,
                               const int16_t *samples,
                               uint16_t count)
{
    uint8_t write_index;
    edgedoctor_audio_item_t *item;
    if ((sync == NULL) || (samples == NULL) ||
        (count != EDGEDOCTOR_AUDIO_CHUNK_SAMPLES))
    {
        return -1;
    }
    if (sync->audio_count == EDGEDOCTOR_SYNC_CAPACITY)
    {
        sync->audio_read = (uint8_t)((sync->audio_read + 1U) % EDGEDOCTOR_SYNC_CAPACITY);
        sync->audio_count--;
        sync->dropped_audio++;
    }
    write_index = (uint8_t)((sync->audio_read + sync->audio_count) % EDGEDOCTOR_SYNC_CAPACITY);
    item = &sync->audio[write_index];
    item->frame_id = frame_id;
    item->timestamp_us = timestamp_us;
    item->count = count;
    memcpy(item->samples, samples, (size_t)count * sizeof(samples[0]));
    sync->audio_count++;
    return 0;
}

int edgedoctor_sync_push_vibration(edgedoctor_sync_t *sync,
                                   uint32_t frame_id,
                                   uint64_t timestamp_us,
                                   const int16_t *samples,
                                   uint16_t frames)
{
    uint8_t write_index;
    edgedoctor_vibration_item_t *item;
    if ((sync == NULL) || (samples == NULL) ||
        (frames != EDGEDOCTOR_VIBRATION_CHUNK_FRAMES))
    {
        return -1;
    }
    if (sync->vibration_count == EDGEDOCTOR_SYNC_CAPACITY)
    {
        sync->vibration_read = (uint8_t)((sync->vibration_read + 1U) % EDGEDOCTOR_SYNC_CAPACITY);
        sync->vibration_count--;
        sync->dropped_vibration++;
    }
    write_index = (uint8_t)((sync->vibration_read + sync->vibration_count) % EDGEDOCTOR_SYNC_CAPACITY);
    item = &sync->vibration[write_index];
    item->frame_id = frame_id;
    item->timestamp_us = timestamp_us;
    item->frames = frames;
    memcpy(item->samples,
           samples,
           (size_t)frames * EDGEDOCTOR_VIBRATION_CHANNELS * sizeof(samples[0]));
    sync->vibration_count++;
    return 0;
}

int edgedoctor_sync_pop(edgedoctor_sync_t *sync, edgedoctor_chunk_t *chunk)
{
    edgedoctor_audio_item_t *audio;
    edgedoctor_vibration_item_t *vibration;
    if ((sync == NULL) || (chunk == NULL))
    {
        return -1;
    }
    while ((sync->audio_count > 0U) && (sync->vibration_count > 0U))
    {
        audio = &sync->audio[sync->audio_read];
        vibration = &sync->vibration[sync->vibration_read];
        if (audio->frame_id < vibration->frame_id)
        {
            sync->audio_read = (uint8_t)((sync->audio_read + 1U) % EDGEDOCTOR_SYNC_CAPACITY);
            sync->audio_count--;
            sync->dropped_audio++;
            sync->mismatched_frames++;
            continue;
        }
        if (vibration->frame_id < audio->frame_id)
        {
            sync->vibration_read = (uint8_t)((sync->vibration_read + 1U) % EDGEDOCTOR_SYNC_CAPACITY);
            sync->vibration_count--;
            sync->dropped_vibration++;
            sync->mismatched_frames++;
            continue;
        }
        chunk->frame_id = audio->frame_id;
        chunk->timestamp_us = (audio->timestamp_us > vibration->timestamp_us)
            ? audio->timestamp_us : vibration->timestamp_us;
        chunk->audio_count = audio->count;
        chunk->vibration_frames = vibration->frames;
        memcpy(chunk->audio, audio->samples, sizeof(chunk->audio));
        memcpy(chunk->vibration, vibration->samples, sizeof(chunk->vibration));
        sync->audio_read = (uint8_t)((sync->audio_read + 1U) % EDGEDOCTOR_SYNC_CAPACITY);
        sync->vibration_read = (uint8_t)((sync->vibration_read + 1U) % EDGEDOCTOR_SYNC_CAPACITY);
        sync->audio_count--;
        sync->vibration_count--;
        return 1;
    }
    return 0;
}

uint16_t edgedoctor_sync_depth(const edgedoctor_sync_t *sync)
{
    if (sync == NULL)
    {
        return 0U;
    }
    return (sync->audio_count < sync->vibration_count)
        ? sync->audio_count : sync->vibration_count;
}

