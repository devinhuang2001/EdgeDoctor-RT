#include "edgedoctor_csv.h"

#include <inttypes.h>
#include <stdio.h>

const char *edgedoctor_csv_header(void)
{
    return "frame_id,timestamp_us,ground_truth,predicted_class,confidence_q15,"
           "anomaly_q15,audio_anomaly_q15,vibration_anomaly_q15,"
           "score_normal_q15,score_inner_q15,score_outer_q15,"
           "score_misalignment_q15,score_unbalance_q15,preprocess_us,"
           "inference_us,total_us,dropped_frames,queue_depth,backend,status\n";
}

int edgedoctor_csv_format(char *destination,
                          size_t capacity,
                          const edgedoctor_result_t *result)
{
    int written;
    if ((destination == NULL) || (capacity == 0U) || (result == NULL))
    {
        return -1;
    }
    written = snprintf(
        destination,
        capacity,
        "%" PRIu32 ",%" PRIu64 ",%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%" PRIu32 ",%" PRIu32
        ",%" PRIu32 ",%" PRIu32 ",%u,%u,%u\n",
        result->frame_id,
        result->timestamp_us,
        (unsigned int)result->ground_truth,
        (unsigned int)result->predicted_class,
        (unsigned int)result->confidence_q15,
        (unsigned int)result->anomaly_q15,
        (unsigned int)result->audio_anomaly_q15,
        (unsigned int)result->vibration_anomaly_q15,
        (unsigned int)result->class_scores_q15[0],
        (unsigned int)result->class_scores_q15[1],
        (unsigned int)result->class_scores_q15[2],
        (unsigned int)result->class_scores_q15[3],
        (unsigned int)result->class_scores_q15[4],
        result->preprocess_us,
        result->inference_us,
        result->total_us,
        result->dropped_frames,
        (unsigned int)result->queue_depth,
        (unsigned int)result->backend,
        (unsigned int)result->status);
    if ((written < 0) || ((size_t)written >= capacity))
    {
        destination[0] = '\0';
        return -1;
    }
    return written;
}
