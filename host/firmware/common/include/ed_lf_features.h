#ifndef ED_LF_FEATURES_H
#define ED_LF_FEATURES_H
#include "ed_monitor_types.h"
#ifdef __cplusplus
extern "C" {
#endif
int ed_lf_features_compute(const float xyz[][3], uint16_t count, uint32_t sample_hz, ed_features_t *out);
#ifdef __cplusplus
}
#endif
#endif
