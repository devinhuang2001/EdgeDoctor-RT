#include "ed_lf_features.h"
#include "ed_lf_window.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned feed_chunks(unsigned chunk, ed_features_t output[3])
{
    ed_lf_window_t window;
    float sequence[416][3];
    unsigned input = 0U;
    unsigned result = 0U;
    unsigned index;

    for (index = 0U; index < 416U; ++index)
    {
        sequence[index][0] = 0.1f * sinf(2.0f * 3.14159265f * 10.0f *
                                         (float)index / 208.0f);
        sequence[index][1] = 0.02f * cosf(2.0f * 3.14159265f * 5.0f *
                                          (float)index / 208.0f);
        sequence[index][2] = 1.0f;
    }
    ed_lf_window_init(&window);
    while (input < 416U)
    {
        unsigned end = input + chunk;
        if (end > 416U)
            end = 416U;
        while (input < end)
        {
            if (ed_lf_window_push(&window, sequence[input]) == 1)
            {
                assert(result < 3U);
                assert(ed_lf_features_compute(
                           (const float (*)[3])window.xyz,
                           ED_LF_WINDOW_SAMPLES, 208U, &output[result]) == 0);
                ++result;
                ed_lf_window_advance(&window);
            }
            ++input;
        }
    }
    return result;
}

int main(void)
{
    ed_features_t one[3];
    ed_features_t eight[3];
    ed_features_t thirty_two[3];
    ed_lf_window_t window;
    float xyz[3] = {0.0f, 1.0f, 2.0f};
    unsigned index;

    assert(feed_chunks(1U, one) == 3U);
    assert(feed_chunks(8U, eight) == 3U);
    assert(feed_chunks(32U, thirty_two) == 3U);
    for (index = 0U; index < 3U; ++index)
    {
        assert(memcmp(&one[index], &eight[index], sizeof(one[index])) == 0);
        assert(memcmp(&one[index], &thirty_two[index], sizeof(one[index])) == 0);
    }

    ed_lf_window_init(&window);
    for (index = 0U; index < 207U; ++index)
        assert(ed_lf_window_push(&window, xyz) == 0);
    assert(ed_lf_window_push(&window, xyz) == 1);
    ed_lf_window_advance(&window);
    assert(window.count == ED_LF_WINDOW_HOP);
    ed_lf_window_init(&window);
    for (index = 0U; index < 50U; ++index)
        assert(ed_lf_window_push(&window, xyz) == 0);
    /* A discontinuity drops the partial window and restarts accumulation. */
    ed_lf_window_init(&window);
    for (index = 0U; index < 207U; ++index)
        assert(ed_lf_window_push(&window, xyz) == 0);
    assert(ed_lf_window_push(&window, xyz) == 1);
    puts("low-frequency window: all tests passed");
    return 0;
}
