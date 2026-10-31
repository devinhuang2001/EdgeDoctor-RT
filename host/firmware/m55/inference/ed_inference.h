#ifndef ED_INFERENCE_H
#define ED_INFERENCE_H
#include "edgedoctor_backend.h"
#ifdef __cplusplus
extern "C" {
#endif
int ed_inference_init(void);
int ed_inference_run(const int16_t vibration[64], edgedoctor_result_t *result);
int ed_inference_choose(edgedoctor_backend_kind_t kind);
const edgedoctor_backend_t *ed_inference_current(void);
int ed_inference_parity(void);
#ifdef __cplusplus
}
#endif
#endif
