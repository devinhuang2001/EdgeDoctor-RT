#include "edgedoctor_app.h"

#include "edgedoctor_backend.h"
#include "edgedoctor_dsp.h"
#include "edgedoctor_logger.h"
#include "edgedoctor_onboard_model.h"
#include "edgedoctor_replay.h"
#include "edgedoctor_window.h"
#include "ed_mqtt_client.h"
#include "ed_monitor_app.h"

#include <finsh.h>
#include <rtthread.h>
#include <cybsp.h>
#include <string.h>
#include <stdio.h>

#define EDGEDOCTOR_REPLAY_PATH_MAX 256U
#define EDGEDOCTOR_REPLAY_STACK_BYTES 8192U
#define EDGEDOCTOR_REPLAY_PRIORITY 20U

typedef struct
{
    edgedoctor_window_t window;
    edgedoctor_dsp_t dsp;
    edgedoctor_snapshot_t snapshot;
    edgedoctor_profiler_t profiler;
    edgedoctor_ui_spectrum_t spectrum[2];
    volatile uint32_t spectrum_generation;
    volatile uint8_t spectrum_active;
    struct rt_thread replay_thread;
    rt_uint8_t replay_stack[EDGEDOCTOR_REPLAY_STACK_BYTES];
    char replay_path[EDGEDOCTOR_REPLAY_PATH_MAX];
    volatile uint8_t replaying;
    uint8_t real_time;
    uint8_t initialized;
    uint8_t replay_started;
} edgedoctor_app_state_t;

/*
 * The official Edgi-Talk linker map provides the secondary M55 SOCMEM section
 * for large application data.  Keep the one-second multimodal ring, feature
 * tensors and replay workspace there so the 256 KiB tightly-coupled data RAM
 * remains available to the XiaoZhi baseline.  This is still fixed storage: no
 * allocation occurs in the capture or DSP hot paths.
 */
static edgedoctor_app_state_t app_state
    __attribute__((section(".cy_socmem_data"), aligned(8)));

static uint8_t profile_uses_cycle_counter;

extern uint32_t SystemCoreClock;

static void profile_clock_init(void)
{
    if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) == 0U)
    {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CYCCNT = 0U;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
        profile_uses_cycle_counter = 1U;
    }
}

static uint64_t profile_clock_now(void)
{
    if (profile_uses_cycle_counter != 0U)
    {
        return (uint64_t)DWT->CYCCNT;
    }
    return (uint64_t)rt_tick_get_millisecond() * 1000U;
}

static uint32_t profile_clock_elapsed_us(uint64_t started, uint64_t finished)
{
    if ((profile_uses_cycle_counter != 0U) && (SystemCoreClock != 0U))
    {
        return (uint32_t)(((uint64_t)(uint32_t)(finished - started) * 1000000U) /
                          SystemCoreClock);
    }
    return (uint32_t)(finished - started);
}

static int update_spectrum(edgedoctor_app_state_t *state, uint32_t frame_id)
{
    float audio_average[EDGEDOCTOR_UI_AUDIO_POINTS];
    float vibration_average[EDGEDOCTOR_UI_VIBRATION_POINTS];
    uint8_t next = (uint8_t)(state->spectrum_active ^ 1U);
    if (edgedoctor_dsp_stream_summary(&state->dsp, audio_average,
                                      vibration_average) != 1)
    {
        return -1;
    }
    state->spectrum[next].frame_id = frame_id;
    if ((edgedoctor_feature_scale_0_100(audio_average, EDGEDOCTOR_UI_AUDIO_POINTS,
                                        state->spectrum[next].audio,
                                        EDGEDOCTOR_UI_AUDIO_POINTS) != 0) ||
        (edgedoctor_feature_scale_0_100(vibration_average,
                                        EDGEDOCTOR_UI_VIBRATION_POINTS,
                                        state->spectrum[next].vibration,
                                        EDGEDOCTOR_UI_VIBRATION_POINTS) != 0))
    {
        return -1;
    }
    __DMB();
    state->spectrum_active = next;
    __DMB();
    state->spectrum_generation++;
    return 0;
}

static int replay_chunk(const edgedoctor_edgepack_header_t *header,
                        const edgedoctor_chunk_t *chunk,
                        void *context)
{
    edgedoctor_app_state_t *state = (edgedoctor_app_state_t *)context;
    edgedoctor_result_t result;
    uint64_t started;
    uint64_t preprocessed;
    uint64_t inferred;
    int ready;
    if (state->replay_started == 0U)
    {
        edgedoctor_window_init(&state->window);
        edgedoctor_profiler_init(&state->profiler);
        state->replay_started = 1U;
    }
    ready = edgedoctor_window_push(&state->window, chunk);
    if (ready < 0)
    {
        return -1;
    }
    if (ready == 0)
    {
        return edgedoctor_dsp_stream_push(&state->dsp, chunk);
    }
    memset(&result, 0, sizeof(result));
    result.frame_id = chunk->frame_id;
    result.timestamp_us = chunk->timestamp_us;
    result.ground_truth = header->label;
    result.predicted_class = UINT8_MAX;
    result.backend = EDGEDOCTOR_BACKEND_REFERENCE;
    result.status = EDGEDOCTOR_RESULT_MODEL_UNAVAILABLE;
    started = profile_clock_now();
    if ((edgedoctor_dsp_stream_push(&state->dsp, chunk) != 0) ||
        (update_spectrum(state, chunk->frame_id) != 0))
    {
        result.status = EDGEDOCTOR_RESULT_DSP_ERROR;
    }
    preprocessed = profile_clock_now();
    result.preprocess_us = profile_clock_elapsed_us(started, preprocessed);
    if (result.status != EDGEDOCTOR_RESULT_DSP_ERROR)
    {
        uint8_t active = state->spectrum_active;
        if (edgedoctor_onboard_model_infer(state->spectrum[active].audio,
                                           state->spectrum[active].vibration,
                                           &result) != 0)
        {
            result.status = EDGEDOCTOR_RESULT_MODEL_UNAVAILABLE;
        }
    }
    inferred = profile_clock_now();
    result.inference_us = profile_clock_elapsed_us(preprocessed, inferred);
    result.total_us = result.preprocess_us + result.inference_us;
    result.dropped_frames = state->window.discontinuities;
    edgedoctor_profiler_record(&state->profiler,
                               result.preprocess_us,
                               result.inference_us,
                               result.total_us,
                               0U);
    edgedoctor_snapshot_publish(&state->snapshot, &result);
    (void)edgedoctor_logger_submit(&result);
    return 0;
}

static void replay_entry(void *parameter)
{
    edgedoctor_app_state_t *state = (edgedoctor_app_state_t *)parameter;
    int status;
    status = edgedoctor_replay_file(
        state->replay_path, state->real_time, replay_chunk, state);
    rt_kprintf("edgeai replay finished: status=%d path=%s\n", status, state->replay_path);
    state->replaying = 0U;
}

static int start_replay(const char *path, int real_time)
{
    rt_err_t status;
    size_t length;
    if ((path == RT_NULL) || (app_state.initialized == 0U))
    {
        return -1;
    }
    if (app_state.replaying != 0U)
    {
        rt_kprintf("edgeai: replay already active\n");
        return -1;
    }
    length = strlen(path);
    if ((length == 0U) || (length >= sizeof(app_state.replay_path)))
    {
        rt_kprintf("edgeai: invalid path length\n");
        return -1;
    }
    memcpy(app_state.replay_path, path, length + 1U);
    app_state.real_time = (uint8_t)(real_time != 0);
    app_state.replay_started = 0U;
    status = rt_thread_init(&app_state.replay_thread,
                            "edge_replay",
                            replay_entry,
                            &app_state,
                            app_state.replay_stack,
                            sizeof(app_state.replay_stack),
                            EDGEDOCTOR_REPLAY_PRIORITY,
                            10U);
    if (status != RT_EOK)
    {
        rt_kprintf("edgeai: replay thread init failed: %d\n", status);
        return -1;
    }
    app_state.replaying = 1U;
    status = rt_thread_startup(&app_state.replay_thread);
    if (status != RT_EOK)
    {
        app_state.replaying = 0U;
        rt_kprintf("edgeai: replay thread start failed: %d\n", status);
        return -1;
    }
    return 0;
}

int edgedoctor_app_read_result(edgedoctor_result_t *result)
{
    return edgedoctor_snapshot_read(&app_state.snapshot, result);
}

int edgedoctor_app_read_spectrum(edgedoctor_ui_spectrum_t *spectrum)
{
    uint32_t before;
    uint32_t after;
    uint8_t active;
    unsigned int attempt;
    if (spectrum == RT_NULL)
    {
        return -1;
    }
    for (attempt = 0U; attempt < 3U; ++attempt)
    {
        before = app_state.spectrum_generation;
        __DMB();
        active = app_state.spectrum_active;
        *spectrum = app_state.spectrum[active];
        __DMB();
        after = app_state.spectrum_generation;
        if (before == after)
        {
            return (after != 0U) ? 1 : 0;
        }
    }
    return 0;
}

void edgedoctor_app_profile(edgedoctor_profile_snapshot_t *profile)
{
    edgedoctor_profiler_snapshot(&app_state.profiler, profile);
}

int edgedoctor_app_live_condition(const ed_asset_key_t *key, float deviation,
                                  uint8_t active, ed_severity_t severity,
                                  uint64_t now_us)
{
    (void)key;
    (void)deviation;
    (void)active;
    (void)severity;
    (void)now_us;
    /* Live events are owned by ed_monitor_app. This compatibility entry point
     * deliberately does not create a second event source. */
    return 0;
}

int edgedoctor_app_is_replaying(void)
{
    return app_state.replaying != 0U;
}

int edgedoctor_app_ack_event(const char *event_id, uint32_t revision, uint64_t now_us)
{
    ed_ack_request_t request;
    if (!event_id || event_id[0] == '\0')
        return -1;
    memset(&request, 0, sizeof(request));
    rt_snprintf(request.command_id, sizeof(request.command_id), "compat-%llu",
                (unsigned long long)now_us);
    rt_snprintf(request.event_id, sizeof(request.event_id), "%s", event_id);
    request.expected_revision = revision;
    request.origin = ED_ORIGIN_REMOTE;
    request.received_mono_us = now_us;
    return ed_monitor_app_post_ack(&request);
}

static int edgedoctor_app_init(void)
{
    static const char flash_log[] = "/flash/edgedoctor.csv";
    memset(&app_state, 0, sizeof(app_state));
    profile_clock_init();
    edgedoctor_snapshot_init(&app_state.snapshot);
    edgedoctor_profiler_init(&app_state.profiler);
    edgedoctor_window_init(&app_state.window);
    if (edgedoctor_logger_init(flash_log) != 0)
    {
        rt_kprintf("edgeai: asynchronous CSV logger init failed\n");
        return -RT_ERROR;
    }
    if (edgedoctor_dsp_init(&app_state.dsp) != 0)
    {
        rt_kprintf("edgeai: CMSIS-DSP init failed\n");
        return -RT_ERROR;
    }
    app_state.initialized = 1U;
    rt_kprintf("edgeai: initialized; onboard model=%s bytes=%u storage=firmware\n",
               edgedoctor_onboard_model_name(),
               (unsigned int)edgedoctor_onboard_model_bytes());
    return RT_EOK;
}
INIT_APP_EXPORT(edgedoctor_app_init);

static int edgeai(int argc, char **argv)
{
    if ((argc == 2) && (strcmp(argv[1], "info") == 0))
    {
        edgedoctor_logger_stats_t logger_stats;
        edgedoctor_profile_snapshot_t profile;
        edgedoctor_profiler_snapshot(&app_state.profiler, &profile);
        edgedoctor_logger_stats(&logger_stats);
        rt_kprintf("EdgeDoctor-RT initialized=%u replaying=%u model=%s bytes=%u storage=firmware\n",
                   (unsigned int)app_state.initialized,
                   (unsigned int)app_state.replaying,
                   edgedoctor_onboard_model_name(),
                   (unsigned int)edgedoctor_onboard_model_bytes());
        rt_kprintf("clock=%s frames=%u preprocess_p50_us=%u preprocess_p95_us=%u inference_p95_us=%u total_p95_us=%u drops=%u\n",
                   (profile_uses_cycle_counter != 0U) ? "dwt" : "tick",
                   (unsigned int)profile.frames,
                   (unsigned int)profile.preprocess_p50_us,
                   (unsigned int)profile.preprocess_p95_us,
                   (unsigned int)profile.inference_p95_us,
                   (unsigned int)profile.total_p95_us,
                   (unsigned int)app_state.window.discontinuities);
        rt_kprintf("log=%s queued=%u written=%u dropped=%u errors=%u\n",
                   edgedoctor_logger_path(),
                   (unsigned int)logger_stats.queued,
                   (unsigned int)logger_stats.written,
                   (unsigned int)logger_stats.dropped,
                   (unsigned int)logger_stats.write_errors);
        return 0;
    }
    if ((argc >= 3) && (strcmp(argv[1], "replay") == 0))
    {
        int real_time = !((argc >= 4) && (strcmp(argv[3], "fast") == 0));
        return start_replay(argv[2], real_time);
    }
    rt_kprintf("usage: edgeai info | edgeai replay <path.edgp> [fast]\n");
    return -1;
}
MSH_CMD_EXPORT(edgeai, EdgeDoctor info and EdgePack replay);
