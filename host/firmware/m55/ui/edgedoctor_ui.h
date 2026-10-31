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
int edgedoctor_ui_stress_switch(uint32_t count, uint32_t *elapsed_ms);

#ifdef __cplusplus
}
#endif

#endif
