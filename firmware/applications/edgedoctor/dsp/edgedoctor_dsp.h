#ifndef EDGEDOCTOR_DSP_H
#define EDGEDOCTOR_DSP_H

#include "arm_math.h"
#include "edgedoctor_feature_contract.h"
#include "edgedoctor_window.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EDGEDOCTOR_AUDIO_FEATURE_BINS 40U
#define EDGEDOCTOR_AUDIO_FEATURE_FRAMES 98U
#define EDGEDOCTOR_AUDIO_FEATURE_VALUES \
    (EDGEDOCTOR_AUDIO_FEATURE_BINS * EDGEDOCTOR_AUDIO_FEATURE_FRAMES)
#define EDGEDOCTOR_VIBRATION_FEATURE_BANDS 64U
#define EDGEDOCTOR_VIBRATION_FEATURE_FRAMES 49U
#define EDGEDOCTOR_VIBRATION_FEATURE_VALUES \
    (EDGEDOCTOR_VIBRATION_FEATURE_BANDS * EDGEDOCTOR_VIBRATION_FEATURE_FRAMES * \
     EDGEDOCTOR_VIBRATION_CHANNELS)

typedef struct
{
    arm_rfft_fast_instance_f32 fft512;
    arm_rfft_fast_instance_f32 fft256;
    float fft_input[512];
    float fft_output[512];
    float power[257];
    float audio_window[400];
    float vibration_window[256];
    int16_t previous_audio[EDGEDOCTOR_AUDIO_CHUNK_SAMPLES];
    int16_t previous_vibration[EDGEDOCTOR_VIBRATION_CHUNK_FRAMES]
                              [EDGEDOCTOR_VIBRATION_CHANNELS];
    float streamed_audio[EDGEDOCTOR_AUDIO_FEATURE_VALUES];
    float streamed_vibration[EDGEDOCTOR_VIBRATION_FEATURE_VALUES];
    uint16_t audio_stream_write;
    uint16_t vibration_stream_write;
    uint16_t audio_stream_count;
    uint16_t vibration_stream_count;
    uint32_t last_chunk_frame_id;
    uint8_t stream_has_previous;
    uint8_t initialized;
} edgedoctor_dsp_t;

int edgedoctor_dsp_init(edgedoctor_dsp_t *dsp);
int edgedoctor_dsp_audio(edgedoctor_dsp_t *dsp,
                         const int16_t *audio,
                         float *features,
                         size_t feature_count);
int edgedoctor_dsp_vibration(edgedoctor_dsp_t *dsp,
                             const int16_t vibration[][EDGEDOCTOR_VIBRATION_CHANNELS],
                             float *features,
                             size_t feature_count);
int edgedoctor_dsp_stream_push(edgedoctor_dsp_t *dsp,
                               const edgedoctor_chunk_t *chunk);
int edgedoctor_dsp_stream_summary(const edgedoctor_dsp_t *dsp,
                                  float audio[EDGEDOCTOR_AUDIO_FEATURE_BINS],
                                  float vibration[EDGEDOCTOR_VIBRATION_FEATURE_BANDS]);

#ifdef __cplusplus
}
#endif

#endif
