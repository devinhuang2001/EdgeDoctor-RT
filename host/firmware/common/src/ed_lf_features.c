#include "ed_lf_features.h"
#include "ed_lf_window.h"

#include <math.h>
#include <string.h>

#define ED_FFT_N 256U
#define ED_PI 3.14159265358979323846f
#define ED_BIN_FIRST 7U
#define ED_BIN_LAST 49U
#define ED_BIN_COUNT (ED_BIN_LAST - ED_BIN_FIRST + 1U)

#ifdef EDGEDOCTOR_RT
#define ED_FEATURE_STORAGE __attribute__((section(".cy_socmem_data"), aligned(8)))
#else
#define ED_FEATURE_STORAGE
#endif

static const float band_edges[ED_BAND_COUNT + 1U] = {
    5.0f, 8.0f, 11.0f, 14.0f, 18.0f, 23.0f, 29.0f, 35.0f, 40.0f
};

static float hann_table[ED_LF_WINDOW_SAMPLES] ED_FEATURE_STORAGE;
static float cos_table[ED_BIN_COUNT][ED_LF_WINDOW_SAMPLES] ED_FEATURE_STORAGE;
static float sin_table[ED_BIN_COUNT][ED_LF_WINDOW_SAMPLES] ED_FEATURE_STORAGE;
static float centered_work[3][ED_FFT_N] ED_FEATURE_STORAGE;
static uint8_t tables_ready;

static void ensure_tables(void)
{
    uint16_t bin;
    uint16_t sample;

    if (tables_ready != 0U)
        return;
    for (sample = 0U; sample < ED_LF_WINDOW_SAMPLES; ++sample)
    {
        float phase = (float)sample / (float)(ED_LF_WINDOW_SAMPLES - 1U);
        hann_table[sample] = 0.5f - 0.5f * cosf(2.0f * ED_PI * phase);
        for (bin = 0U; bin < ED_BIN_COUNT; ++bin)
        {
            float angle = 2.0f * ED_PI *
                (float)(bin + ED_BIN_FIRST) * (float)sample / (float)ED_FFT_N;
            cos_table[bin][sample] = cosf(angle);
            sin_table[bin][sample] = sinf(angle);
        }
    }
    tables_ready = 1U;
}

static int band_for_frequency(float frequency)
{
    uint8_t band;
    for (band = 0U; band < ED_BAND_COUNT; ++band)
    {
        if (frequency >= band_edges[band] && frequency < band_edges[band + 1U])
            return (int)band;
    }
    return -1;
}

int ed_lf_features_compute(const float xyz[][3], uint16_t count,
                           uint32_t sample_hz, ed_features_t *out)
{
    float mean[3] = {0.0f};
    float sum_sq[3] = {0.0f};
    float peak = 0.0f;
    float vector_rms;
    float max_kurtosis = 0.0f;
    uint16_t sample;
    uint8_t axis;

    if (xyz == NULL || out == NULL || count < 33U || count > ED_FFT_N ||
        sample_hz == 0U)
        return -1;

    ensure_tables();
    memset(out, 0, sizeof(*out));
    for (sample = 0U; sample < count; ++sample)
    {
        for (axis = 0U; axis < 3U; ++axis)
        {
            if (!isfinite(xyz[sample][axis]))
                return -1;
            mean[axis] += xyz[sample][axis];
        }
    }
    for (axis = 0U; axis < 3U; ++axis)
        mean[axis] /= (float)count;

    for (sample = 0U; sample < count; ++sample)
    {
        float magnitude_sq = 0.0f;
        for (axis = 0U; axis < 3U; ++axis)
        {
            float delta = xyz[sample][axis] - mean[axis];
            centered_work[axis][sample] = delta;
            sum_sq[axis] += delta * delta;
            magnitude_sq += xyz[sample][axis] * xyz[sample][axis];
        }
        peak = fmaxf(peak, sqrtf(fmaxf(0.0f, magnitude_sq)));
    }

    for (axis = 0U; axis < 3U; ++axis)
    {
        float second = sum_sq[axis] / (float)count;
        float fourth = 0.0f;

        out->raw_mean_g[axis] = mean[axis];
        out->values[axis] = sqrtf(fmaxf(0.0f, second));
        for (sample = 0U; sample < count; ++sample)
        {
            float delta = centered_work[axis][sample];
            fourth += delta * delta * delta * delta;
        }
        if (second > 1.0e-8f)
            max_kurtosis = fmaxf(max_kurtosis,
                                 (fourth / (float)count) / (second * second));
    }

    vector_rms = sqrtf(fmaxf(0.0f,
        (sum_sq[0] + sum_sq[1] + sum_sq[2]) / (float)count));
    out->values[3] = vector_rms;
    out->values[4] = peak;
    out->values[5] = peak / fmaxf(vector_rms, 0.001f);
    out->values[6] = max_kurtosis;

    for (axis = 0U; axis < 3U; ++axis)
    {
        uint16_t bin;
        for (bin = 0U; bin < ED_BIN_COUNT; ++bin)
        {
            float frequency = (float)(bin + ED_BIN_FIRST) *
                (float)sample_hz / (float)ED_FFT_N;
            float real = 0.0f;
            float imaginary = 0.0f;
            uint16_t index;
            int band = band_for_frequency(frequency);

            if (band < 0)
                continue;
            for (index = 0U; index < count; ++index)
            {
                float weighted = centered_work[axis][index] * hann_table[index];
                real += weighted * cos_table[bin][index];
                imaginary -= weighted * sin_table[bin][index];
            }
            out->band_energy_g2[band] +=
                (real * real + imaginary * imaginary) /
                ((float)count * (float)count);
        }
    }
    for (sample = 0U; sample < ED_BAND_COUNT; ++sample)
        out->values[7U + sample] = out->band_energy_g2[sample];
    out->valid_mask = 0xffffU;
    return 0;
}
