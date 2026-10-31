#include "ed_monitor_types.h"
#include <math.h>
#include <string.h>
int ed_imu_block_validate(const ed_imu_block_t *b) {
    if (!b || b->count == 0u || b->count > ED_IMU_BLOCK_MAX || b->sample_period_ns == 0u) return -1;
    return 0;
}
void ed_monitor_policy_default(ed_monitor_policy_t *p) {
    if (!p) return;
    memset(p, 0, sizeof(*p)); p->version=1u; p->warning_deviation=6.0f;
    p->alarm_deviation=10.0f; p->recovery_deviation=3.0f; p->idle_rms_g=0.005f;
    p->trigger_outputs=3u; p->recovery_outputs=20u; p->stale_us=200000u;
    p->disconnect_us=1000000u; p->idle_us=10000000u; p->invalid_reset_us=2000000u;
    p->quality_recovery_us=2000000u;
}
