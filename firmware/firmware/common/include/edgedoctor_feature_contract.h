#ifndef EDGEDOCTOR_FEATURE_CONTRACT_H
#define EDGEDOCTOR_FEATURE_CONTRACT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Formal model-feature contract.  DSP stores values as
 * [band][frame][channel]; display scaling is deliberately kept here so the
 * inference path and LVGL rendering share a deterministic, testable mapping.
 */
int edgedoctor_feature_band_average(const float *source,
                                    size_t bands,
                                    size_t frames,
                                    size_t channels,
                                    float *destination,
                                    size_t destination_count);

int edgedoctor_feature_scale_0_100(const float *source,
                                    size_t count,
                                    int16_t *destination,
                                    size_t destination_count);

#ifdef __cplusplus
}
#endif

#endif
