#include "ed_imu_profile.h"

#include <assert.h>
#include <math.h>

int main(void)
{
    ed_sensor_config_t config;

    assert(ed_imu_profile_from_ctrl1(0x50U, &config) < 0);
    assert(ed_imu_profile_from_ctrl1(0x54U, &config) < 0);
    assert(ed_imu_profile_from_ctrl1(ED_IMU_CTRL1_208HZ_4G, &config) == 0);
    assert(config.odr_hz == 208U);
    assert(config.range_mg == 4000U);
    assert(fabsf(8197.0f * config.sensitivity_g_per_adc - 1.0f) < 0.001f);
    return 0;
}
