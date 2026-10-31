#ifndef EDGEDOCTOR_TYPES_H
#define EDGEDOCTOR_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDGEDOCTOR_AUDIO_RATE_HZ          16000U
#define EDGEDOCTOR_VIBRATION_RATE_HZ       6400U
#define EDGEDOCTOR_CHUNK_MS                  20U
#define EDGEDOCTOR_AUDIO_CHUNK_SAMPLES      320U
#define EDGEDOCTOR_VIBRATION_CHUNK_FRAMES   128U
#define EDGEDOCTOR_VIBRATION_CHANNELS         2U
#define EDGEDOCTOR_CLASS_COUNT                 5U
#define EDGEDOCTOR_WINDOW_CHUNKS               50U
#define EDGEDOCTOR_HOP_CHUNKS                  25U
#define EDGEDOCTOR_AUDIO_WINDOW_SAMPLES     16000U
#define EDGEDOCTOR_VIBRATION_WINDOW_FRAMES   6400U
/* A missing modality must not be interpreted as a measured normal/abnormal score. */
#define EDGEDOCTOR_SCORE_UNAVAILABLE UINT16_MAX

typedef enum
{
    EDGEDOCTOR_CLASS_NORMAL = 0,
    EDGEDOCTOR_CLASS_BEARING_INNER = 1,
    EDGEDOCTOR_CLASS_BEARING_OUTER = 2,
    EDGEDOCTOR_CLASS_MISALIGNMENT = 3,
    EDGEDOCTOR_CLASS_UNBALANCE = 4
} edgedoctor_class_t;

typedef enum
{
    EDGEDOCTOR_RESULT_WARMUP = 0,
    EDGEDOCTOR_RESULT_OK = 1,
    EDGEDOCTOR_RESULT_MODEL_UNAVAILABLE = 2,
    EDGEDOCTOR_RESULT_DSP_ERROR = 3
} edgedoctor_result_status_t;

typedef struct
{
    uint32_t frame_id;
    uint64_t timestamp_us;
    uint16_t audio_count;
    uint16_t vibration_frames;
    int16_t audio[EDGEDOCTOR_AUDIO_CHUNK_SAMPLES];
    int16_t vibration[EDGEDOCTOR_VIBRATION_CHUNK_FRAMES]
                     [EDGEDOCTOR_VIBRATION_CHANNELS];
} edgedoctor_chunk_t;

typedef struct
{
    uint32_t frame_id;
    uint64_t timestamp_us;
    uint8_t ground_truth;
    uint8_t predicted_class;
    uint16_t confidence_q15;
    uint16_t anomaly_q15;
    uint16_t audio_anomaly_q15;
    uint16_t vibration_anomaly_q15;
    uint16_t class_scores_q15[EDGEDOCTOR_CLASS_COUNT];
    uint32_t preprocess_us;
    uint32_t inference_us;
    uint32_t total_us;
    uint32_t dropped_frames;
    uint16_t queue_depth;
    uint8_t backend;
    uint8_t status;
} edgedoctor_result_t;

#ifdef __cplusplus
}
#endif

#endif
