#ifndef ED_IMU_PROFILE_H
#define ED_IMU_PROFILE_H

#include "ed_monitor_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ED_IMU_CTRL1_208HZ_4G 0x58U

int ed_imu_profile_from_ctrl1(uint8_t ctrl1, ed_sensor_config_t *out);

#ifdef __cplusplus
}
#endif

#endif
