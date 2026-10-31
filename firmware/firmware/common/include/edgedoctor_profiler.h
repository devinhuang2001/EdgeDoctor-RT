#ifndef EDGEDOCTOR_PROFILER_H
#define EDGEDOCTOR_PROFILER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDGEDOCTOR_PROFILE_CAPACITY 256U

typedef struct
{
    uint32_t values[EDGEDOCTOR_PROFILE_CAPACITY];
    uint16_t count;
    uint16_t write_index;
} edgedoctor_metric_history_t;

typedef struct
{
    edgedoctor_metric_history_t preprocess_us;
    edgedoctor_metric_history_t inference_us;
    edgedoctor_metric_history_t total_us;
    uint32_t frames;
    uint32_t dropped_frames;
    uint32_t dropped_logs;
    uint16_t current_queue_depth;
    uint16_t max_queue_depth;
} edgedoctor_profiler_t;

typedef struct
{
    uint32_t preprocess_p50_us;
    uint32_t preprocess_p95_us;
    uint32_t inference_p50_us;
    uint32_t inference_p95_us;
    uint32_t total_p50_us;
    uint32_t total_p95_us;
    uint32_t frames;
    uint32_t dropped_frames;
    uint32_t dropped_logs;
    uint16_t current_queue_depth;
    uint16_t max_queue_depth;
} edgedoctor_profile_snapshot_t;

void edgedoctor_profiler_init(edgedoctor_profiler_t *profiler);
void edgedoctor_profiler_record(edgedoctor_profiler_t *profiler,
                                uint32_t preprocess_us,
                                uint32_t inference_us,
                                uint32_t total_us,
                                uint16_t queue_depth);
void edgedoctor_profiler_snapshot(const edgedoctor_profiler_t *profiler,
                                  edgedoctor_profile_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif

#endif

