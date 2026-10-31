#ifndef ED_MONITOR_APP_H
#define ED_MONITOR_APP_H

#include "ed_monitor_types.h"

#ifdef __cplusplus
extern "C" {
#endif

int ed_monitor_app_read_snapshot(ed_snapshot_t *out);
int ed_monitor_app_is_running(void);
int ed_monitor_app_baseline_start(uint8_t allow_idle);
int ed_monitor_app_baseline_status(uint32_t *windows, uint8_t *active,
                                   uint8_t *valid, uint8_t *timed_out);
int ed_monitor_app_baseline_commit(void);
int ed_monitor_app_baseline_load(void);
int ed_monitor_app_calibration_start(void);
int ed_monitor_app_calibration_status(uint32_t *samples, uint8_t *active,
                                      uint8_t *complete, float *gravity_g,
                                      float *dynamic_rms_g);
int ed_monitor_app_post_ack(const ed_ack_request_t *request);
int ed_monitor_app_read_active_event(ed_event_t *event);

#ifdef __cplusplus
}
#endif

#endif
