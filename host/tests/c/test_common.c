#include "edgedoctor_edgepack.h"
#include "edgedoctor_backend.h"
#include "edgedoctor_csv.h"
#include "edgedoctor_ipc.h"
#include "edgedoctor_profiler.h"
#include "edgedoctor_sync.h"
#include "edgedoctor_window.h"
#include "edgedoctor_snapshot.h"
#include "edgedoctor_onboard_model.h"
#include "edgedoctor_feature_contract.h"
#include "edgedoctor_onboard_model_golden.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_AUDIO_SAMPLES 320U
#define TEST_VIBRATION_FRAMES 128U
#define TEST_PAYLOAD_BYTES (2U * (TEST_AUDIO_SAMPLES + TEST_VIBRATION_FRAMES * 2U))

static void write_u16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

static void test_edgepack(void)
{
    uint8_t file[EDGEDOCTOR_EDGEPACK_HEADER_BYTES + TEST_PAYLOAD_BYTES];
    edgedoctor_edgepack_view_t view;
    edgedoctor_edgepack_chunk_view_t chunk;
    uint32_t crc;
    uint32_t stream_crc;
    size_t index;

    memset(file, 0, sizeof(file));
    memcpy(file, "EDGP", 4U);
    write_u16(file + 4U, 1U);
    write_u16(file + 6U, EDGEDOCTOR_EDGEPACK_HEADER_BYTES);
    write_u32(file + 8U, 42U);
    write_u16(file + 12U, 1U);
    file[14U] = 3U;
    file[15U] = 2U;
    write_u16(file + 16U, 40U);
    write_u16(file + 18U, 20U);
    write_u32(file + 20U, 16000U);
    write_u32(file + 24U, 6400U);
    file[28U] = 2U;
    file[29U] = 2U;
    write_u32(file + 32U, TEST_AUDIO_SAMPLES);
    write_u32(file + 36U, TEST_VIBRATION_FRAMES);
    write_u32(file + 40U, TEST_PAYLOAD_BYTES);
    for (index = EDGEDOCTOR_EDGEPACK_HEADER_BYTES; index < sizeof(file); ++index)
    {
        file[index] = (uint8_t)(index & 0xFFU);
    }
    crc = edgedoctor_crc32(file + EDGEDOCTOR_EDGEPACK_HEADER_BYTES, TEST_PAYLOAD_BYTES);
    stream_crc = edgedoctor_crc32_init();
    stream_crc = edgedoctor_crc32_update(
        stream_crc, file + EDGEDOCTOR_EDGEPACK_HEADER_BYTES, 100U);
    stream_crc = edgedoctor_crc32_update(
        stream_crc, file + EDGEDOCTOR_EDGEPACK_HEADER_BYTES + 100U,
        TEST_PAYLOAD_BYTES - 100U);
    assert(edgedoctor_crc32_finish(stream_crc) == crc);
    write_u32(file + 44U, crc);

    assert(edgedoctor_edgepack_parse_header(
               file, EDGEDOCTOR_EDGEPACK_HEADER_BYTES, &view) == EDGEDOCTOR_EDGEPACK_OK);
    assert(view.payload == NULL);
    assert(view.chunk_count == 1U);
    assert(edgedoctor_edgepack_open(file, sizeof(file), &view) == EDGEDOCTOR_EDGEPACK_OK);
    assert(view.chunk_count == 1U);
    assert(view.header.recording_id == 42U);
    assert(edgedoctor_edgepack_get_chunk(&view, 0U, &chunk) == EDGEDOCTOR_EDGEPACK_OK);
    assert(chunk.audio_samples == TEST_AUDIO_SAMPLES);
    assert(chunk.vibration_frames == TEST_VIBRATION_FRAMES);
    assert(chunk.vibration_channels == 2U);
    assert(edgedoctor_edgepack_get_chunk(&view, 1U, &chunk) == EDGEDOCTOR_EDGEPACK_RANGE);

    file[sizeof(file) - 1U] ^= 1U;
    assert(edgedoctor_edgepack_open(file, sizeof(file), &view) == EDGEDOCTOR_EDGEPACK_CRC);
    assert(edgedoctor_edgepack_open(file, sizeof(file) - 1U, &view) == EDGEDOCTOR_EDGEPACK_LENGTH);
}

static void fill_samples(int16_t *samples, size_t count, int16_t base)
{
    size_t index;
    for (index = 0U; index < count; ++index)
    {
        samples[index] = (int16_t)(base + (int16_t)index);
    }
}

static void test_sync(void)
{
    edgedoctor_sync_t sync;
    edgedoctor_chunk_t chunk;
    int16_t audio[EDGEDOCTOR_AUDIO_CHUNK_SAMPLES];
    int16_t vibration[EDGEDOCTOR_VIBRATION_CHUNK_FRAMES * EDGEDOCTOR_VIBRATION_CHANNELS];
    unsigned int index;

    fill_samples(audio, EDGEDOCTOR_AUDIO_CHUNK_SAMPLES, 100);
    fill_samples(vibration,
                 EDGEDOCTOR_VIBRATION_CHUNK_FRAMES * EDGEDOCTOR_VIBRATION_CHANNELS,
                 200);
    edgedoctor_sync_init(&sync);
    assert(edgedoctor_sync_push_audio(&sync, 1U, 1000U, audio,
                                      EDGEDOCTOR_AUDIO_CHUNK_SAMPLES) == 0);
    assert(edgedoctor_sync_pop(&sync, &chunk) == 0);
    assert(edgedoctor_sync_push_vibration(&sync, 1U, 1100U, vibration,
                                          EDGEDOCTOR_VIBRATION_CHUNK_FRAMES) == 0);
    assert(edgedoctor_sync_pop(&sync, &chunk) == 1);
    assert(chunk.frame_id == 1U);
    assert(chunk.timestamp_us == 1100U);
    assert(chunk.audio[0] == 100);
    assert(chunk.vibration[0][0] == 200);

    assert(edgedoctor_sync_push_audio(&sync, 2U, 2000U, audio,
                                      EDGEDOCTOR_AUDIO_CHUNK_SAMPLES) == 0);
    assert(edgedoctor_sync_push_vibration(&sync, 3U, 3000U, vibration,
                                          EDGEDOCTOR_VIBRATION_CHUNK_FRAMES) == 0);
    assert(edgedoctor_sync_pop(&sync, &chunk) == 0);
    assert(sync.dropped_audio == 1U);
    assert(sync.mismatched_frames == 1U);

    edgedoctor_sync_init(&sync);
    for (index = 0U; index < EDGEDOCTOR_SYNC_CAPACITY + 1U; ++index)
    {
        assert(edgedoctor_sync_push_audio(&sync, index, index, audio,
                                          EDGEDOCTOR_AUDIO_CHUNK_SAMPLES) == 0);
    }
    assert(sync.dropped_audio == 1U);
    assert(sync.audio_count == EDGEDOCTOR_SYNC_CAPACITY);
}

static void test_profiler(void)
{
    edgedoctor_profiler_t profiler;
    edgedoctor_profile_snapshot_t snapshot;
    uint32_t index;

    edgedoctor_profiler_init(&profiler);
    for (index = 1U; index <= 200U; ++index)
    {
        edgedoctor_profiler_record(&profiler, index, index * 2U, index * 3U,
                                   (uint16_t)(index % 9U));
    }
    edgedoctor_profiler_snapshot(&profiler, &snapshot);
    assert(snapshot.frames == 200U);
    assert(snapshot.preprocess_p50_us == 101U);
    assert(snapshot.preprocess_p95_us == 191U);
    assert(snapshot.inference_p95_us == 382U);
    assert(snapshot.total_p95_us == 573U);
    assert(snapshot.max_queue_depth == 8U);
}

static void test_ipc(void)
{
    edgedoctor_ipc_slot_t slot;
    edgedoctor_sentinel_message_t message = {77U, 30000U, 2U, 0U};
    edgedoctor_sentinel_message_t received;

    edgedoctor_ipc_init(&slot);
    assert(slot.state == EDGEDOCTOR_SLOT_EMPTY);
    assert(edgedoctor_ipc_acquire(&slot, &received) == 0);
    assert(edgedoctor_ipc_publish(&slot, &message) == 1);
    assert(slot.state == EDGEDOCTOR_SLOT_READY);
    assert(edgedoctor_ipc_publish(&slot, &message) == 0);
    assert(edgedoctor_ipc_acquire(&slot, &received) == 1);
    assert(received.frame_id == message.frame_id);
    assert(received.score_q15 == message.score_q15);
    assert(edgedoctor_ipc_release(&slot) == 1);
    assert(slot.state == EDGEDOCTOR_SLOT_EMPTY);
}

static void test_ipc_stress(void)
{
    edgedoctor_ipc_slot_t slot;
    edgedoctor_sentinel_message_t sent, received;
    uint32_t index;
    edgedoctor_ipc_init(&slot);
    for (index = 0U; index < 10000U; ++index)
    {
        sent.frame_id = index;
        sent.score_q15 = (uint16_t)(index & 32767U);
        sent.trigger_reason = (uint8_t)(index & 3U);
        sent.reserved = 0U;
        assert(edgedoctor_ipc_publish(&slot, &sent) == 1);
        assert(edgedoctor_ipc_acquire(&slot, &received) == 1);
        assert(received.frame_id == sent.frame_id);
        assert(received.score_q15 == sent.score_q15);
        assert(received.trigger_reason == sent.trigger_reason);
        assert(edgedoctor_ipc_release(&slot) == 1);
    }
    puts("IPC ordered stress 10000: passed");
}

static void test_window(void)
{
    edgedoctor_window_t window;
    edgedoctor_window_buffer_t output;
    edgedoctor_chunk_t chunk;
    uint32_t frame;

    edgedoctor_window_init(&window);
    memset(&chunk, 0, sizeof(chunk));
    chunk.audio_count = EDGEDOCTOR_AUDIO_CHUNK_SAMPLES;
    chunk.vibration_frames = EDGEDOCTOR_VIBRATION_CHUNK_FRAMES;
    for (frame = 0U; frame < 75U; ++frame)
    {
        chunk.frame_id = frame;
        chunk.timestamp_us = ((uint64_t)frame + 1U) * 20000U;
        chunk.audio[0] = (int16_t)frame;
        chunk.vibration[0][0] = (int16_t)(100U + frame);
        assert(edgedoctor_window_push(&window, &chunk) ==
               ((frame == 49U || frame == 74U) ? 1 : 0));
        if (frame == 49U || frame == 74U)
        {
            assert(edgedoctor_window_materialize(&window, &output) == 1);
            assert(output.first_frame_id == frame - 49U);
            assert(output.last_frame_id == frame);
            assert(output.audio[0] == (int16_t)(frame - 49U));
            assert(output.audio[49U * EDGEDOCTOR_AUDIO_CHUNK_SAMPLES] == (int16_t)frame);
            assert(output.vibration[0][0] == (int16_t)(100U + frame - 49U));
        }
    }

    chunk.frame_id = 80U;
    chunk.timestamp_us += 20000U;
    assert(edgedoctor_window_push(&window, &chunk) == 0);
    assert(window.discontinuities == 1U);
    assert(window.valid_chunks == 1U);
    assert(edgedoctor_window_materialize(&window, &output) == 0);
}

static int backend_echo(const int8_t *input,
                        size_t input_bytes,
                        int8_t *output,
                        size_t output_bytes,
                        void *context)
{
    size_t index;
    (void)context;
    for (index = 0U; index < output_bytes; ++index)
    {
        output[index] = input[index % input_bytes];
    }
    return 0;
}

static void test_backend(void)
{
    edgedoctor_backend_t backends[3] = {
        {EDGEDOCTOR_BACKEND_REFERENCE, "reference", 200U, 1000U, 0U, 1U, 0U,
         backend_echo, NULL},
        {EDGEDOCTOR_BACKEND_CMSIS_NN, "cmsis-nn", 200U, 1000U, 0U, 1U, 0U,
         backend_echo, NULL},
        {EDGEDOCTOR_BACKEND_U55, "u55", 200U, 1000U, 949U, 1U, 0U,
         backend_echo, NULL}
    };
    const edgedoctor_backend_t *selected;
    int8_t input[5] = {1, 2, 3, 4, 5};
    int8_t output[5] = {0};

    assert(edgedoctor_backend_validate(&backends[2]) ==
           EDGEDOCTOR_BACKEND_COVERAGE_LIMIT);
    selected = edgedoctor_backend_select(EDGEDOCTOR_BACKEND_U55, backends, 3U, 0);
    assert(selected == NULL);
    selected = edgedoctor_backend_select(EDGEDOCTOR_BACKEND_U55, backends, 3U, 1);
    assert(selected == &backends[1]);
    backends[2].npu_coverage_permille = 950U;
    selected = edgedoctor_backend_select(EDGEDOCTOR_BACKEND_U55, backends, 3U, 0);
    assert(selected == &backends[2]);
    assert(edgedoctor_backend_invoke(selected, input, sizeof(input), output,
                                     sizeof(output)) == EDGEDOCTOR_BACKEND_OK);
    assert(memcmp(input, output, sizeof(input)) == 0);
    backends[2].model_bytes = EDGEDOCTOR_MODEL_MAX_BYTES + 1U;
    assert(edgedoctor_backend_validate(&backends[2]) == EDGEDOCTOR_BACKEND_MODEL_LIMIT);
}

static void test_snapshot_and_csv(void)
{
    edgedoctor_snapshot_t snapshot;
    edgedoctor_result_t expected;
    edgedoctor_result_t actual;
    char row[EDGEDOCTOR_CSV_ROW_MAX];

    memset(&expected, 0, sizeof(expected));
    expected.frame_id = 123U;
    expected.timestamp_us = UINT64_C(456789);
    expected.ground_truth = 2U;
    expected.predicted_class = 3U;
    expected.confidence_q15 = 30000U;
    expected.total_us = 987U;
    expected.backend = EDGEDOCTOR_BACKEND_U55;
    edgedoctor_snapshot_init(&snapshot);
    assert(edgedoctor_snapshot_read(&snapshot, &actual) == 0);
    edgedoctor_snapshot_publish(&snapshot, &expected);
    assert(edgedoctor_snapshot_read(&snapshot, &actual) == 1);
    assert(memcmp(&expected, &actual, sizeof(expected)) == 0);
    assert(strstr(edgedoctor_csv_header(), "frame_id,timestamp_us") != NULL);
    assert(edgedoctor_csv_format(row, sizeof(row), &actual) > 0);
    assert(strstr(row, "123,456789,2,3,30000") == row);
    assert(edgedoctor_csv_format(row, 4U, &actual) == -1);
}

static void test_onboard_model(void)
{
    int16_t audio[EDGEDOCTOR_MODEL_AUDIO_POINTS];
    int16_t vibration[EDGEDOCTOR_MODEL_VIBRATION_POINTS];
    edgedoctor_result_t first;
    edgedoctor_result_t second;
    uint32_t score_sum = 0U;
    uint32_t index;
    for (index = 0U; index < EDGEDOCTOR_MODEL_AUDIO_POINTS; ++index)
    {
        audio[index] = (int16_t)(20U + (index % 7U));
    }
    for (index = 0U; index < EDGEDOCTOR_MODEL_VIBRATION_POINTS; ++index)
    {
        vibration[index] = (int16_t)(20U + (index % 9U));
    }
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    assert(edgedoctor_onboard_model_infer(audio, vibration, &first) == 0);
    assert(edgedoctor_onboard_model_infer(audio, vibration, &second) == 0);
    assert(memcmp(&first, &second, sizeof(first)) == 0);
    assert(first.status == EDGEDOCTOR_RESULT_OK);
    assert(first.audio_anomaly_q15 == EDGEDOCTOR_SCORE_UNAVAILABLE);
    assert(first.vibration_anomaly_q15 == first.anomaly_q15);
    assert(first.predicted_class < EDGEDOCTOR_CLASS_COUNT);
    assert(first.confidence_q15 == first.class_scores_q15[first.predicted_class]);
    for (index = 0U; index < EDGEDOCTOR_CLASS_COUNT; ++index)
    {
        score_sum += first.class_scores_q15[index];
    }
    assert(score_sum >= 32763U && score_sum <= 32767U);
    assert(edgedoctor_onboard_model_bytes() > 0U);
    assert(edgedoctor_onboard_model_bytes() < 4000U);
    assert(strcmp(edgedoctor_onboard_model_name(),
                  "kaist-vibration-mlp-int8-v1") == 0);
}

static void test_onboard_model_golden(void)
{
    uint32_t vector;
    for (vector = 0U; vector < EDGEDOCTOR_MODEL_GOLDEN_VECTORS; ++vector)
    {
        edgedoctor_result_t result;
        uint32_t class_index;
        memset(&result, 0, sizeof(result));
        assert(edgedoctor_onboard_model_infer(NULL,
                                              edgedoctor_model_golden_input[vector],
                                              &result) == 0);
        assert(result.predicted_class == edgedoctor_model_golden_class[vector]);
        for (class_index = 0U; class_index < EDGEDOCTOR_CLASS_COUNT; ++class_index)
        {
            int difference = (int)result.class_scores_q15[class_index] -
                             (int)edgedoctor_model_golden_score_q15[vector][class_index];
            assert(difference >= -2 && difference <= 2);
        }
    }
}

static void test_feature_contract(void)
{
    const float values[] = {2.0f, 4.0f, 6.0f, 8.0f, 10.0f, 12.0f};
    const float constant[] = {5.0f, 5.0f, 5.0f};
    float averages[2];
    int16_t scaled[3];

    assert(edgedoctor_feature_band_average(values, 2U, 3U, 1U,
                                           averages, 2U) == 0);
    assert(averages[0] == 4.0f);
    assert(averages[1] == 10.0f);
    assert(edgedoctor_feature_scale_0_100(averages, 2U, scaled, 2U) == 0);
    assert(scaled[0] == 0);
    assert(scaled[1] == 100);
    assert(edgedoctor_feature_scale_0_100(constant, 3U, scaled, 3U) == 0);
    assert(scaled[0] == 0 && scaled[1] == 0 && scaled[2] == 0);
    assert(edgedoctor_feature_band_average(values, 0U, 3U, 1U,
                                           averages, 2U) == -1);
}

int main(void)
{
    test_edgepack();
    test_sync();
    test_profiler();
    test_ipc();
    test_ipc_stress();
    test_window();
    test_backend();
    test_snapshot_and_csv();
    test_onboard_model();
    test_onboard_model_golden();
    test_feature_contract();
    puts("EdgeDoctor common C tests: PASS");
    return 0;
}
