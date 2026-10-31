#ifndef EDGEDOCTOR_ONBOARD_MODEL_H
#define EDGEDOCTOR_ONBOARD_MODEL_H

#include "edgedoctor_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EDGEDOCTOR_MODEL_AUDIO_POINTS 40U
#define EDGEDOCTOR_MODEL_VIBRATION_POINTS 64U

const char *edgedoctor_onboard_model_name(void);
uint32_t edgedoctor_onboard_model_bytes(void);
int edgedoctor_onboard_model_infer(
    const int16_t audio[EDGEDOCTOR_MODEL_AUDIO_POINTS],
    const int16_t vibration[EDGEDOCTOR_MODEL_VIBRATION_POINTS],
    edgedoctor_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
