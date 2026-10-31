#include "ed_imu_profile.h"

#include <stddef.h>

int ed_imu_profile_from_ctrl1(uint8_t ctrl1, ed_sensor_config_t *out)
{
    if ((out == NULL) || ((ctrl1 & 0xfcU) != ED_IMU_CTRL1_208HZ_4G))
    {
        return -1;
    }

    out->odr_hz = 208U;
    out->range_mg = 4000U;
    out->sensitivity_g_per_adc = 0.000122f;
    return 0;
}
