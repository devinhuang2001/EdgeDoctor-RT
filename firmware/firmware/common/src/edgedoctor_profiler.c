#include "edgedoctor_profiler.h"

#include <stddef.h>
#include <string.h>

static void history_add(edgedoctor_metric_history_t *history, uint32_t value)
{
    history->values[history->write_index] = value;
    history->write_index = (uint16_t)((history->write_index + 1U) % EDGEDOCTOR_PROFILE_CAPACITY);
    if (history->count < EDGEDOCTOR_PROFILE_CAPACITY)
    {
        history->count++;
    }
}

static uint32_t percentile(const edgedoctor_metric_history_t *history,
                           uint32_t numerator,
                           uint32_t denominator)
{
    uint32_t sorted[EDGEDOCTOR_PROFILE_CAPACITY];
    uint16_t index;
    uint16_t inner;
    uint32_t value;
    uint32_t rank;
    if (history->count == 0U)
    {
        return 0U;
    }
    memcpy(sorted, history->values, (size_t)history->count * sizeof(sorted[0]));
    for (index = 1U; index < history->count; ++index)
    {
        value = sorted[index];
        inner = index;
        while ((inner > 0U) && (sorted[inner - 1U] > value))
        {
            sorted[inner] = sorted[inner - 1U];
            inner--;
        }
        sorted[inner] = value;
    }
    rank = ((uint32_t)(history->count - 1U) * numerator + denominator - 1U) /
           denominator;
    return sorted[rank];
}

void edgedoctor_profiler_init(edgedoctor_profiler_t *profiler)
{
    if (profiler != NULL)
    {
        memset(profiler, 0, sizeof(*profiler));
    }
}

void edgedoctor_profiler_record(edgedoctor_profiler_t *profiler,
                                uint32_t preprocess_us,
                                uint32_t inference_us,
                                uint32_t total_us,
                                uint16_t queue_depth)
{
    if (profiler == NULL)
    {
        return;
    }
    history_add(&profiler->preprocess_us, preprocess_us);
    history_add(&profiler->inference_us, inference_us);
    history_add(&profiler->total_us, total_us);
    profiler->frames++;
    profiler->current_queue_depth = queue_depth;
    if (queue_depth > profiler->max_queue_depth)
    {
        profiler->max_queue_depth = queue_depth;
    }
}

void edgedoctor_profiler_snapshot(const edgedoctor_profiler_t *profiler,
                                  edgedoctor_profile_snapshot_t *snapshot)
{
    if ((profiler == NULL) || (snapshot == NULL))
    {
        return;
    }
    snapshot->preprocess_p50_us = percentile(&profiler->preprocess_us, 50U, 100U);
    snapshot->preprocess_p95_us = percentile(&profiler->preprocess_us, 95U, 100U);
    snapshot->inference_p50_us = percentile(&profiler->inference_us, 50U, 100U);
    snapshot->inference_p95_us = percentile(&profiler->inference_us, 95U, 100U);
    snapshot->total_p50_us = percentile(&profiler->total_us, 50U, 100U);
    snapshot->total_p95_us = percentile(&profiler->total_us, 95U, 100U);
    snapshot->frames = profiler->frames;
    snapshot->dropped_frames = profiler->dropped_frames;
    snapshot->dropped_logs = profiler->dropped_logs;
    snapshot->current_queue_depth = profiler->current_queue_depth;
    snapshot->max_queue_depth = profiler->max_queue_depth;
}

