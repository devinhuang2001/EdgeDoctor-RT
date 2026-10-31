#include "ed_lf_window.h"

#include <string.h>

void ed_lf_window_init(ed_lf_window_t *window)
{
    if (window != NULL)
    {
        memset(window, 0, sizeof(*window));
    }
}

int ed_lf_window_push(ed_lf_window_t *window, const float xyz[3])
{
    if ((window == NULL) || (xyz == NULL) ||
        (window->count >= ED_LF_WINDOW_SAMPLES))
    {
        return -1;
    }

    window->xyz[window->count][0] = xyz[0];
    window->xyz[window->count][1] = xyz[1];
    window->xyz[window->count][2] = xyz[2];
    window->count++;
    return (window->count == ED_LF_WINDOW_SAMPLES) ? 1 : 0;
}

void ed_lf_window_advance(ed_lf_window_t *window)
{
    if (window == NULL)
    {
        return;
    }
    if (window->count < ED_LF_WINDOW_SAMPLES)
    {
        window->count = 0U;
        return;
    }
    memmove(window->xyz,
            window->xyz + ED_LF_WINDOW_HOP,
            ED_LF_WINDOW_HOP * sizeof(window->xyz[0]));
    window->count = ED_LF_WINDOW_HOP;
}
