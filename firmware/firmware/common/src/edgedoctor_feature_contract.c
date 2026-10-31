#include "edgedoctor_feature_contract.h"

int edgedoctor_feature_band_average(const float *source,
                                    size_t bands,
                                    size_t frames,
                                    size_t channels,
                                    float *destination,
                                    size_t destination_count)
{
    size_t band;
    size_t samples;
    if ((source == NULL) || (destination == NULL) || (bands == 0U) ||
        (frames == 0U) || (channels == 0U) || (destination_count < bands))
    {
        return -1;
    }
    samples = frames * channels;
    for (band = 0U; band < bands; ++band)
    {
        size_t sample;
        float sum = 0.0f;
        for (sample = 0U; sample < samples; ++sample)
        {
            sum += source[band * samples + sample];
        }
        destination[band] = sum / (float)samples;
    }
    return 0;
}

int edgedoctor_feature_scale_0_100(const float *source,
                                    size_t count,
                                    int16_t *destination,
                                    size_t destination_count)
{
    float minimum;
    float maximum;
    size_t index;
    if ((source == NULL) || (destination == NULL) || (count == 0U) ||
        (destination_count < count))
    {
        return -1;
    }
    minimum = source[0];
    maximum = minimum;
    for (index = 1U; index < count; ++index)
    {
        if (source[index] < minimum)
        {
            minimum = source[index];
        }
        if (source[index] > maximum)
        {
            maximum = source[index];
        }
    }
    if (maximum <= minimum)
    {
        for (index = 0U; index < count; ++index)
        {
            destination[index] = 0;
        }
        return 0;
    }
    for (index = 0U; index < count; ++index)
    {
        float value = (source[index] - minimum) * 100.0f / (maximum - minimum);
        if (value < 0.0f)
        {
            value = 0.0f;
        }
        if (value > 100.0f)
        {
            value = 100.0f;
        }
        destination[index] = (int16_t)(value + 0.5f);
    }
    return 0;
}
