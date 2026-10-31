#ifndef ED_SENSOR_H
#define ED_SENSOR_H
#include "ed_monitor_types.h"
#ifdef __cplusplus
extern "C" {
#endif
int ed_sensor_start(const ed_sensor_config_t *config);
int ed_sensor_stop(void);
int ed_sensor_read_block(ed_imu_block_t *out, uint32_t timeout_ms);
/* Producer hook for an ISR/FIFO worker; uses a fixed ring and never allocates. */
int ed_sensor_submit_block(const ed_imu_block_t *block);
void ed_sensor_get_stats(ed_sensor_stats_t *out);
/* Monotonic microseconds with 32-bit RT-Thread tick wrap extension. */
uint64_t ed_sensor_now_us(void);
#ifdef __cplusplus
}
#endif
#endif
