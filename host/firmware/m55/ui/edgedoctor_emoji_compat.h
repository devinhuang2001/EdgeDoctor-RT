#ifndef EDGEDOCTOR_EMOJI_COMPAT_H
#define EDGEDOCTOR_EMOJI_COMPAT_H

/*
 * The EdgeDoctor HMI owns the only LVGL screen.  XiaoZhi still references
 * these legacy notification hooks from its audio/state code, so keep the
 * ABI as small no-op inline shims while the vendor 3D demo is excluded from
 * the build.
 */
typedef struct lv_obj_t lv_obj_t;

#ifdef __cplusplus
extern "C" {
#endif

static inline lv_obj_t *lv_example_virtual3d_animated_emoji(lv_obj_t *parent)
{
    (void)parent;
    return (lv_obj_t *)0;
}

static inline void lv_example_virtual3d_animated_emoji_popup(void)
{
}

void qday_show_emoji_by_rtt_info(int index);

static inline void qday_emoji_handle_touch_event(void *event)
{
    (void)event;
}

#ifdef __cplusplus
}
#endif

#endif /* EDGEDOCTOR_EMOJI_COMPAT_H */
