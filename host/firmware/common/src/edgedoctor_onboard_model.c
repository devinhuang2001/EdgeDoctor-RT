#include "edgedoctor_onboard_model.h"

#include "edgedoctor_backend.h"
#include "edgedoctor_onboard_model_params.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

const char *edgedoctor_onboard_model_name(void)
{
    return "kaist-vibration-mlp-int8-v1";
}

uint32_t edgedoctor_onboard_model_bytes(void)
{
    return (uint32_t)(sizeof(edgedoctor_model_fc1_weight_q7) +
                      sizeof(edgedoctor_model_fc2_weight_q7) +
                      sizeof(edgedoctor_model_fc2_scale) +
                      sizeof(edgedoctor_model_fc2_bias) +
                      sizeof(edgedoctor_model_hidden_scale) +
                      sizeof(edgedoctor_model_fc1_multiplier_q15) +
                      sizeof(edgedoctor_model_fc1_bias_q));
}

int edgedoctor_onboard_model_infer(
    const int16_t audio[EDGEDOCTOR_MODEL_AUDIO_POINTS],
    const int16_t vibration[EDGEDOCTOR_MODEL_VIBRATION_POINTS],
    edgedoctor_result_t *result)
{
    int32_t hidden[EDGEDOCTOR_ONBOARD_HIDDEN];
    float logits[EDGEDOCTOR_CLASS_COUNT];
    float probabilities[EDGEDOCTOR_CLASS_COUNT];
    float maximum;
    float sum = 0.0f;
    uint32_t index;
    uint32_t neuron;
    uint8_t best_class = 0U;
    (void)audio;
    if ((vibration == NULL) || (result == NULL))
    {
        return -1;
    }
    for (neuron = 0U; neuron < EDGEDOCTOR_ONBOARD_HIDDEN; ++neuron)
    {
        int32_t accumulator = 0;
        for (index = 0U; index < EDGEDOCTOR_ONBOARD_INPUT; ++index)
        {
            int32_t input = vibration[index];
            if (input < 0)
            {
                input = 0;
            }
            if (input > 100)
            {
                input = 100;
            }
            accumulator += input *
                           (int32_t)edgedoctor_model_fc1_weight_q7[neuron][index];
        }
        int64_t scaled = (int64_t)accumulator *
                         edgedoctor_model_fc1_multiplier_q15[neuron];
        int32_t value = (int32_t)((scaled + 16384) >> 15) +
                        edgedoctor_model_fc1_bias_q[neuron];
        if (value < 0)
        {
            value = 0;
        }
        if (value > 127)
        {
            value = 127;
        }
        hidden[neuron] = value;
    }
    for (index = 0U; index < EDGEDOCTOR_CLASS_COUNT; ++index)
    {
        int32_t accumulator = 0;
        for (neuron = 0U; neuron < EDGEDOCTOR_ONBOARD_HIDDEN; ++neuron)
        {
            accumulator += hidden[neuron] *
                           (int32_t)edgedoctor_model_fc2_weight_q7[index][neuron];
        }
        float value = (float)accumulator * edgedoctor_model_hidden_scale[0] *
                      edgedoctor_model_fc2_scale[index] +
                      edgedoctor_model_fc2_bias[index];
        logits[index] = value;
    }
    maximum = logits[0];
    for (index = 1U; index < EDGEDOCTOR_CLASS_COUNT; ++index)
    {
        if (logits[index] > maximum)
        {
            maximum = logits[index];
            best_class = (uint8_t)index;
        }
    }
    for (index = 0U; index < EDGEDOCTOR_CLASS_COUNT; ++index)
    {
        probabilities[index] = expf(logits[index] - maximum);
        sum += probabilities[index];
    }
    for (index = 0U; index < EDGEDOCTOR_CLASS_COUNT; ++index)
    {
        result->class_scores_q15[index] =
            (uint16_t)(probabilities[index] * 32767.0f / sum + 0.5f);
    }
    result->predicted_class = best_class;
    result->confidence_q15 = result->class_scores_q15[best_class];
    result->anomaly_q15 = (uint16_t)(32767U - result->class_scores_q15[0]);
    result->audio_anomaly_q15 = EDGEDOCTOR_SCORE_UNAVAILABLE;
    result->vibration_anomaly_q15 = result->anomaly_q15;
    result->backend = EDGEDOCTOR_BACKEND_REFERENCE;
    result->status = EDGEDOCTOR_RESULT_OK;
    return 0;
}
