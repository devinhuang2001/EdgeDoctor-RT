#include "ed_quality.h"
#include <math.h>
#include <string.h>
void ed_quality_init(ed_quality_state_t *s) { if (s) memset(s,0,sizeof(*s)); }
ed_quality_t ed_quality_step(ed_quality_state_t *s,const ed_monitor_policy_t *p,const ed_quality_input_t *i,uint8_t *idle) {
    if (!s || !p || !i || !idle) return ED_QUALITY_CONFIG_ERROR;
    *idle=0u;
    if (!i->configured || !isfinite(i->dynamic_rms_g) || i->window_samples>208u) return ED_QUALITY_CONFIG_ERROR;
    if (i->probe_failed) return ED_QUALITY_DISCONNECTED;
    if (i->gap) return ED_QUALITY_GAPPED;
    if (i->window_samples < 208u) return ED_QUALITY_WARMUP;
    if (i->saturated_samples >= 2u) return ED_QUALITY_SATURATED;
    if (i->bus_failing) {
        if (i->now_us-i->bus_failure_since_us >= p->disconnect_us) return ED_QUALITY_DISCONNECTED;
        return ED_QUALITY_STALE;
    }
    if (i->now_us-i->last_sample_mono_us >= p->stale_us) return ED_QUALITY_STALE;
    if (i->dynamic_rms_g < p->idle_rms_g) {
        if (!s->idle_tracking) { s->idle_tracking=1u; s->idle_since_us=i->now_us; }
        if (i->now_us-s->idle_since_us >= p->idle_us) *idle=1u;
    } else { s->idle_tracking=0u; s->idle_since_us=0u; }
    return ED_QUALITY_VALID;
}
