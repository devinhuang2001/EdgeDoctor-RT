#ifndef ED_QUALITY_H
#define ED_QUALITY_H
#include "ed_monitor_types.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint64_t idle_since_us; uint8_t idle_tracking; } ed_quality_state_t;
typedef struct {
    uint64_t now_us, last_sample_mono_us, bus_failure_since_us;
    uint32_t window_samples, saturated_samples; float dynamic_rms_g;
    uint8_t configured, probe_failed, bus_failing, gap;
} ed_quality_input_t;
void ed_quality_init(ed_quality_state_t *state);
ed_quality_t ed_quality_step(ed_quality_state_t *state, const ed_monitor_policy_t *policy, const ed_quality_input_t *input, uint8_t *idle);
#ifdef __cplusplus
}
#endif
#endif
