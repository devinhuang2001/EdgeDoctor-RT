#ifndef ED_LF_WINDOW_H
#define ED_LF_WINDOW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ED_LF_WINDOW_SAMPLES 208U
#define ED_LF_WINDOW_HOP 104U

typedef struct {
    float xyz[ED_LF_WINDOW_SAMPLES][3];
    uint16_t count;
} ed_lf_window_t;

void ed_lf_window_init(ed_lf_window_t *window);
int ed_lf_window_push(ed_lf_window_t *window, const float xyz[3]);
void ed_lf_window_advance(ed_lf_window_t *window);

#ifdef __cplusplus
}
#endif

#endif
