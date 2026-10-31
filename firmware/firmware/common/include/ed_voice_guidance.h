#ifndef ED_VOICE_GUIDANCE_H
#define ED_VOICE_GUIDANCE_H
#include "ed_monitor_types.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Builds a bounded, read-only answer for XiaoZhi/MCP without touching UI or audio. */
int ed_voice_build_response(const ed_snapshot_t *snapshot, const ed_event_t *event,
                            char *out, size_t capacity, size_t *written);
#ifdef __cplusplus
}
#endif
#endif
