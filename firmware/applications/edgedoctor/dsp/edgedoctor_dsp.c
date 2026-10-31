#include "edgedoctor_dsp.h"

#include <math.h>
#include <string.h>

#define EDGEDOCTOR_LOG_FLOOR 0.000001f
#define EDGEDOCTOR_PI 3.14159265358979323846f

static const uint16_t audio_mel_edges[EDGEDOCTOR_AUDIO_FEATURE_BINS + 2U] = {
    0U, 1U, 2U, 4U, 6U, 8U, 10U, 12U, 14U, 16U, 19U, 21U, 24U, 27U,
    30U, 33U, 37U, 41U, 45U, 49U, 54U, 59U, 64U, 69U, 75U, 81U, 88U,
    95U, 103U, 110U, 119U, 128U, 137U, 148U, 158U, 170U, 182U, 195U,
    209U, 224U, 239U, 256U
};

static const uint8_t vibration_band_left[EDGEDOCTOR_VIBRATION_FEATURE_BANDS] = {
    1U,1U,1U,1U,1U,1U,1U,1U,1U,1U,2U,2U,2U,2U,2U,3U,
    3U,3U,3U,4U,4U,4U,5U,5U,6U,6U,7U,7U,8U,9U,9U,10U,
    11U,12U,13U,14U,15U,16U,17U,19U,20U,22U,24U,26U,28U,30U,32U,35U,
    38U,41U,44U,48U,51U,55U,60U,65U,70U,75U,81U,88U,95U,102U,110U,119U
};

static const uint8_t vibration_band_right[EDGEDOCTOR_VIBRATION_FEATURE_BANDS] = {
    2U,2U,2U,2U,2U,2U,2U,2U,2U,3U,3U,3U,3U,3U,4U,4U,
    4U,4U,5U,5U,5U,6U,6U,7U,7U,8U,8U,9U,10U,10U,11U,12U,
    13U,14U,15U,16U,17U,18U,20U,21U,23U,25U,27U,29U,31U,33U,36U,39U,
    42U,45U,49U,52U,56U,61U,66U,71U,76U,82U,89U,96U,103U,111U,120U,129U
};

static void calculate_power(const float *packed, float *power, uint32_t fft_size)
{
    uint32_t bin;
    power[0] = packed[0] * packed[0];
    power[fft_size / 2U] = packed[1] * packed[1];
    for (bin = 1U; bin < fft_size / 2U; ++bin)
    {
        float real = packed[2U * bin];
        float imaginary = packed[2U * bin + 1U];
        power[bin] = real * real + imaginary * imaginary;
    }
}

static int16_t stream_audio_sample(const edgedoctor_dsp_t *dsp,
                                   const edgedoctor_chunk_t *chunk,
                                   uint32_t index)
{
    return (index < EDGEDOCTOR_AUDIO_CHUNK_SAMPLES) ? dsp->previous_audio[index] :
        chunk->audio[index - EDGEDOCTOR_AUDIO_CHUNK_SAMPLES];
}

static int16_t stream_vibration_sample(const edgedoctor_dsp_t *dsp,
                                       const edgedoctor_chunk_t *chunk,
                                       uint32_t index,
                                       uint32_t channel)
{
    return (index < EDGEDOCTOR_VIBRATION_CHUNK_FRAMES) ?
        dsp->previous_vibration[index][channel] :
        chunk->vibration[index - EDGEDOCTOR_VIBRATION_CHUNK_FRAMES][channel];
}

static void stream_audio_frame(edgedoctor_dsp_t *dsp,
                               const edgedoctor_chunk_t *chunk,
                               uint32_t offset)
{
    uint32_t sample;
    uint32_t mel;
    uint16_t slot = dsp->audio_stream_write;
    memset(dsp->fft_input, 0, sizeof(dsp->fft_input));
    for (sample = 0U; sample < 400U; ++sample)
    {
        dsp->fft_input[sample] =
            ((float)stream_audio_sample(dsp, chunk, offset + sample) / 32768.0f) *
            dsp->audio_window[sample];
    }
    arm_rfft_fast_f32(&dsp->fft512, dsp->fft_input, dsp->fft_output, 0U);
    calculate_power(dsp->fft_output, dsp->power, 512U);
    for (mel = 0U; mel < EDGEDOCTOR_AUDIO_FEATURE_BINS; ++mel)
    {
        uint16_t left = audio_mel_edges[mel];
        uint16_t center = audio_mel_edges[mel + 1U];
        uint16_t right = audio_mel_edges[mel + 2U];
        uint16_t bin;
        float energy = 0.0f;
        for (bin = left; bin < center; ++bin)
        {
            energy += dsp->power[bin] * ((float)(bin - left) / (float)(center - left));
        }
        for (bin = center; bin < right; ++bin)
        {
            energy += dsp->power[bin] * ((float)(right - bin) / (float)(right - center));
        }
        dsp->streamed_audio[mel * EDGEDOCTOR_AUDIO_FEATURE_FRAMES + slot] =
            logf((energy > EDGEDOCTOR_LOG_FLOOR) ? energy : EDGEDOCTOR_LOG_FLOOR);
    }
    dsp->audio_stream_write = (uint16_t)((slot + 1U) % EDGEDOCTOR_AUDIO_FEATURE_FRAMES);
    if (dsp->audio_stream_count < EDGEDOCTOR_AUDIO_FEATURE_FRAMES)
    {
        dsp->audio_stream_count++;
    }
}

static void stream_vibration_frame(edgedoctor_dsp_t *dsp,
                                   const edgedoctor_chunk_t *chunk,
                                   uint32_t channel)
{
    uint32_t sample;
    uint32_t band;
    uint16_t slot = dsp->vibration_stream_write;
    for (sample = 0U; sample < 256U; ++sample)
    {
        dsp->fft_input[sample] =
            ((float)stream_vibration_sample(dsp, chunk, sample, channel) / 32768.0f) *
            dsp->vibration_window[sample];
    }
    arm_rfft_fast_f32(&dsp->fft256, dsp->fft_input, dsp->fft_output, 0U);
    calculate_power(dsp->fft_output, dsp->power, 256U);
    for (band = 0U; band < EDGEDOCTOR_VIBRATION_FEATURE_BANDS; ++band)
    {
        uint32_t bin;
        float energy = 0.0f;
        for (bin = vibration_band_left[band]; bin < vibration_band_right[band]; ++bin)
        {
            energy += dsp->power[bin];
        }
        energy /= (float)(vibration_band_right[band] - vibration_band_left[band]);
        dsp->streamed_vibration[
            (band * EDGEDOCTOR_VIBRATION_FEATURE_FRAMES + slot) *
            EDGEDOCTOR_VIBRATION_CHANNELS + channel] =
            logf((energy > EDGEDOCTOR_LOG_FLOOR) ? energy : EDGEDOCTOR_LOG_FLOOR);
    }
}

int edgedoctor_dsp_init(edgedoctor_dsp_t *dsp)
{
    uint32_t index;
    if (dsp == NULL)
    {
        return -1;
    }
    memset(dsp, 0, sizeof(*dsp));
    if ((arm_rfft_fast_init_f32(&dsp->fft512, 512U) != ARM_MATH_SUCCESS) ||
        (arm_rfft_fast_init_f32(&dsp->fft256, 256U) != ARM_MATH_SUCCESS))
    {
        return -1;
    }
    for (index = 0U; index < 400U; ++index)
    {
        dsp->audio_window[index] =
            0.5f - 0.5f * cosf(2.0f * EDGEDOCTOR_PI * (float)index / 400.0f);
    }
    for (index = 0U; index < 256U; ++index)
    {
        dsp->vibration_window[index] =
            0.5f - 0.5f * cosf(2.0f * EDGEDOCTOR_PI * (float)index / 256.0f);
    }
    dsp->initialized = 1U;
    return 0;
}

int edgedoctor_dsp_audio(edgedoctor_dsp_t *dsp,
                         const int16_t *audio,
                         float *features,
                         size_t feature_count)
{
    uint32_t frame;
    if ((dsp == NULL) || (audio == NULL) || (features == NULL) ||
        (dsp->initialized == 0U) ||
        (feature_count < EDGEDOCTOR_AUDIO_FEATURE_VALUES))
    {
        return -1;
    }
    for (frame = 0U; frame < EDGEDOCTOR_AUDIO_FEATURE_FRAMES; ++frame)
    {
        uint32_t sample;
        uint32_t mel;
        memset(dsp->fft_input, 0, sizeof(dsp->fft_input));
        for (sample = 0U; sample < 400U; ++sample)
        {
            dsp->fft_input[sample] =
                ((float)audio[frame * 160U + sample] / 32768.0f) *
                dsp->audio_window[sample];
        }
        arm_rfft_fast_f32(&dsp->fft512, dsp->fft_input, dsp->fft_output, 0U);
        calculate_power(dsp->fft_output, dsp->power, 512U);
        for (mel = 0U; mel < EDGEDOCTOR_AUDIO_FEATURE_BINS; ++mel)
        {
            uint16_t left = audio_mel_edges[mel];
            uint16_t center = audio_mel_edges[mel + 1U];
            uint16_t right = audio_mel_edges[mel + 2U];
            uint16_t bin;
            float energy = 0.0f;
            for (bin = left; bin < center; ++bin)
            {
                energy += dsp->power[bin] *
                    ((float)(bin - left) / (float)(center - left));
            }
            for (bin = center; bin < right; ++bin)
            {
                energy += dsp->power[bin] *
                    ((float)(right - bin) / (float)(right - center));
            }
            features[mel * EDGEDOCTOR_AUDIO_FEATURE_FRAMES + frame] =
                logf((energy > EDGEDOCTOR_LOG_FLOOR) ? energy : EDGEDOCTOR_LOG_FLOOR);
        }
    }
    return 0;
}

int edgedoctor_dsp_vibration(edgedoctor_dsp_t *dsp,
                             const int16_t vibration[][EDGEDOCTOR_VIBRATION_CHANNELS],
                             float *features,
                             size_t feature_count)
{
    uint32_t channel;
    if ((dsp == NULL) || (vibration == NULL) || (features == NULL) ||
        (dsp->initialized == 0U) ||
        (feature_count < EDGEDOCTOR_VIBRATION_FEATURE_VALUES))
    {
        return -1;
    }
    for (channel = 0U; channel < EDGEDOCTOR_VIBRATION_CHANNELS; ++channel)
    {
        uint32_t frame;
        for (frame = 0U; frame < EDGEDOCTOR_VIBRATION_FEATURE_FRAMES; ++frame)
        {
            uint32_t sample;
            uint32_t band;
            for (sample = 0U; sample < 256U; ++sample)
            {
                dsp->fft_input[sample] =
                    ((float)vibration[frame * 128U + sample][channel] / 32768.0f) *
                    dsp->vibration_window[sample];
            }
            arm_rfft_fast_f32(&dsp->fft256, dsp->fft_input, dsp->fft_output, 0U);
            calculate_power(dsp->fft_output, dsp->power, 256U);
            for (band = 0U; band < EDGEDOCTOR_VIBRATION_FEATURE_BANDS; ++band)
            {
                uint32_t bin;
                float energy = 0.0f;
                for (bin = vibration_band_left[band];
                     bin < vibration_band_right[band];
                     ++bin)
                {
                    energy += dsp->power[bin];
                }
                energy /= (float)(vibration_band_right[band] -
                                  vibration_band_left[band]);
                features[(band * EDGEDOCTOR_VIBRATION_FEATURE_FRAMES + frame) *
                         EDGEDOCTOR_VIBRATION_CHANNELS + channel] =
                    logf((energy > EDGEDOCTOR_LOG_FLOOR) ? energy : EDGEDOCTOR_LOG_FLOOR);
            }
        }
    }
    return 0;
}

int edgedoctor_dsp_stream_push(edgedoctor_dsp_t *dsp,
                               const edgedoctor_chunk_t *chunk)
{
    uint32_t channel;
    if ((dsp == NULL) || (chunk == NULL) || (dsp->initialized == 0U) ||
        (chunk->audio_count != EDGEDOCTOR_AUDIO_CHUNK_SAMPLES) ||
        (chunk->vibration_frames != EDGEDOCTOR_VIBRATION_CHUNK_FRAMES))
    {
        return -1;
    }
    if ((dsp->stream_has_previous != 0U) &&
        (chunk->frame_id != (dsp->last_chunk_frame_id + 1U)))
    {
        dsp->stream_has_previous = 0U;
        dsp->audio_stream_count = 0U;
        dsp->vibration_stream_count = 0U;
        dsp->audio_stream_write = 0U;
        dsp->vibration_stream_write = 0U;
    }
    if (dsp->stream_has_previous != 0U)
    {
        stream_audio_frame(dsp, chunk, 0U);
        stream_audio_frame(dsp, chunk, 160U);
        for (channel = 0U; channel < EDGEDOCTOR_VIBRATION_CHANNELS; ++channel)
        {
            stream_vibration_frame(dsp, chunk, channel);
        }
        dsp->vibration_stream_write =
            (uint16_t)((dsp->vibration_stream_write + 1U) %
                       EDGEDOCTOR_VIBRATION_FEATURE_FRAMES);
        if (dsp->vibration_stream_count < EDGEDOCTOR_VIBRATION_FEATURE_FRAMES)
        {
            dsp->vibration_stream_count++;
        }
    }
    memcpy(dsp->previous_audio, chunk->audio, sizeof(dsp->previous_audio));
    memcpy(dsp->previous_vibration, chunk->vibration, sizeof(dsp->previous_vibration));
    dsp->last_chunk_frame_id = chunk->frame_id;
    dsp->stream_has_previous = 1U;
    return 0;
}

int edgedoctor_dsp_stream_summary(const edgedoctor_dsp_t *dsp,
                                  float audio[EDGEDOCTOR_AUDIO_FEATURE_BINS],
                                  float vibration[EDGEDOCTOR_VIBRATION_FEATURE_BANDS])
{
    if ((dsp == NULL) || (audio == NULL) || (vibration == NULL) ||
        (dsp->audio_stream_count != EDGEDOCTOR_AUDIO_FEATURE_FRAMES) ||
        (dsp->vibration_stream_count != EDGEDOCTOR_VIBRATION_FEATURE_FRAMES))
    {
        return 0;
    }
    if ((edgedoctor_feature_band_average(dsp->streamed_audio,
                                         EDGEDOCTOR_AUDIO_FEATURE_BINS,
                                         EDGEDOCTOR_AUDIO_FEATURE_FRAMES,
                                         1U,
                                         audio,
                                         EDGEDOCTOR_AUDIO_FEATURE_BINS) != 0) ||
        (edgedoctor_feature_band_average(dsp->streamed_vibration,
                                         EDGEDOCTOR_VIBRATION_FEATURE_BANDS,
                                         EDGEDOCTOR_VIBRATION_FEATURE_FRAMES,
                                         EDGEDOCTOR_VIBRATION_CHANNELS,
                                         vibration,
                                         EDGEDOCTOR_VIBRATION_FEATURE_BANDS) != 0))
    {
        return -1;
    }
    return 1;
}
