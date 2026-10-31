#ifndef EDGEDOCTOR_APP_H
#define EDGEDOCTOR_APP_H

#include "edgedoctor_profiler.h"
#include "edgedoctor_snapshot.h"
#include "ed_monitor_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EDGEDOCTOR_UI_AUDIO_POINTS 40U
#define EDGEDOCTOR_UI_VIBRATION_POINTS 64U

typedef struct
{
    uint32_t frame_id;
    int16_t audio[EDGEDOCTOR_UI_AUDIO_POINTS];
    int16_t vibration[EDGEDOCTOR_UI_VIBRATION_POINTS];
} edgedoctor_ui_spectrum_t;

int edgedoctor_app_read_result(edgedoctor_result_t *result);
int edgedoctor_app_read_spectrum(edgedoctor_ui_spectrum_t *spectrum);
void edgedoctor_app_profile(edgedoctor_profile_snapshot_t *profile);
int edgedoctor_app_is_replaying(void);
int edgedoctor_app_live_condition(const ed_asset_key_t *key, float deviation,
                                  uint8_t active, ed_severity_t severity,
                                  uint64_t now_us);
int edgedoctor_app_ack_event(const char *command_id, const char *event_id,
                             uint32_t revision, uint64_t now_us);

#ifdef __cplusplus
}
#endif

#endif
