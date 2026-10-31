#ifndef EDGEDOCTOR_UI_H
#define EDGEDOCTOR_UI_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

void edgedoctor_ui_init(const lv_font_t *font_small,
                        const lv_font_t *font_medium,
                        const lv_font_t *font_large);
void edgedoctor_ui_process(void);

#ifdef __cplusplus
}
#endif

#endif
