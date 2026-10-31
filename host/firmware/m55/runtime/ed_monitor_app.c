#include "ed_monitor_app.h"

#include "ed_lf_features.h"
#include "ed_lf_window.h"
#include "ed_alarm.h"
#include "ed_baseline.h"
#include "ed_quality.h"
#include "ed_evidence_capture.h"
#include "ed_event_journal.h"
#include "ed_mqtt_client.h"
#include "ed_sensor.h"

#include <dfs_posix.h>
#include <fcntl.h>
#include <math.h>
#include <rtthread.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#define ED_MONITOR_STACK_BYTES 4096U
#define ED_MONITOR_PRIORITY 24U
#define ED_MONITOR_BASELINE_PATH "/flash/edgedoctor.baseline"
#define ED_MONITOR_BASELINE_TEMP "/flash/edgedoctor.baseline.tmp"
#define ED_MONITOR_COMMAND_DEPTH 8U
#define ED_BASELINE_FILE_MAGIC 0xed84b153U
#define ED_BASELINE_FILE_FORMAT 1U

typedef struct {
    uint32_t magic;
    uint16_t format;
    uint16_t length;
    ed_baseline_t value;
    uint32_t crc32;
} ed_baseline_file_t;

typedef enum {
    ED_MONITOR_COMMAND_ACK = 1,
    ED_MONITOR_COMMAND_BASELINE_START,
    ED_MONITOR_COMMAND_BASELINE_COMMIT,
    ED_MONITOR_COMMAND_BASELINE_LOAD,
    ED_MONITOR_COMMAND_SENSOR_START,
    ED_MONITOR_COMMAND_SENSOR_STOP
} ed_monitor_command_kind_t;

typedef struct {
    ed_monitor_command_kind_t kind;
    ed_ack_request_t ack;
    uint8_t allow_idle;
} ed_monitor_command_t;

typedef struct {
    ed_lf_window_t window;
    ed_features_t features;
    ed_baseline_t baseline;
    ed_baseline_builder_t baseline_builder;
    ed_quality_state_t quality;
    ed_monitor_policy_t policy;
    ed_sensor_config_t sensor_config;
    ed_alarm_t alarm;
    ed_snapshot_t snapshot;
    ed_asset_key_t key;
    struct rt_mutex lock;
    struct rt_thread thread;
    rt_uint8_t stack[ED_MONITOR_STACK_BYTES];
    uint8_t baseline_valid;
    uint8_t running;
    uint8_t initialized;
    uint8_t lock_ready;
    uint8_t baseline_active;
    uint8_t baseline_allow_idle;
    uint8_t baseline_mismatch;
    uint32_t last_config_revision;
    uint32_t calibration_samples;
    double calibration_sum[3];
    double calibration_sum_sq;
    float calibration_gravity_g;
    float calibration_dynamic_rms_g;
    uint8_t calibration_active;
    uint8_t calibration_complete;
    struct rt_messagequeue command_queue;
    rt_uint8_t command_pool[ED_MONITOR_COMMAND_DEPTH * sizeof(ed_monitor_command_t)]
        __attribute__((aligned(8)));
    uint8_t command_queue_ready;
    uint8_t pending_ack_stage;
    uint8_t pending_ack_final_submitted;
    char pending_ack_command_id[ED_ID_MAX];
    char pending_ack_event_id[ED_EVENT_ID_MAX];
    uint32_t pending_ack_initial_revision;
    uint32_t pending_ack_final_revision;
    uint16_t saturated_samples;
    uint64_t no_block_since_us;
} ed_monitor_state_t;

static ed_monitor_state_t monitor __attribute__((section(".cy_socmem_data"), aligned(8)));

static int ed_monitor_app_baseline_commit_now(void);
static int ed_monitor_app_baseline_load_now(void);

static void monitor_update_no_block(uint64_t now_us)
{
    uint64_t elapsed;
    uint8_t disconnected = 0U;
    if (monitor.no_block_since_us == 0U)
        monitor.no_block_since_us = now_us;
    elapsed = now_us - monitor.no_block_since_us;
    if (monitor.lock_ready && rt_mutex_take(&monitor.lock, RT_WAITING_NO) == RT_EOK)
    {
        if (monitor.snapshot.snapshot_id != 0U &&
            elapsed >= monitor.policy.stale_us)
        {
            monitor.snapshot.quality = elapsed >= monitor.policy.disconnect_us
                ? ED_QUALITY_DISCONNECTED : ED_QUALITY_STALE;
            monitor.snapshot.diagnosis = ED_DIAG_UNAVAILABLE;
            monitor.snapshot.baseline_deviation = NAN;
            (void)ed_alarm_step(&monitor.alarm, &monitor.snapshot, now_us);
            ed_lf_window_init(&monitor.window);
            monitor.saturated_samples = 0U;
            monitor.snapshot.invalid_monitor_us = elapsed;
            if (elapsed > monitor.snapshot.longest_unavailable_us)
                monitor.snapshot.longest_unavailable_us = elapsed;
            disconnected = (uint8_t)(elapsed >= monitor.policy.disconnect_us);
        }
        rt_mutex_release(&monitor.lock);
    }
    if (disconnected)
        (void)ed_alarm_condition(&monitor.alarm, &monitor.key,
                                 ED_SOURCE_LIVE_IMU, ED_EVENT_SENSOR, 1,
                                 ED_SEVERITY_ALARM, now_us);
}

static void monitor_mark_block_received(uint64_t now_us)
{
    if (monitor.no_block_since_us != 0U)
    {
        (void)ed_alarm_condition(&monitor.alarm, &monitor.key,
                                 ED_SOURCE_LIVE_IMU, ED_EVENT_SENSOR, 0,
                                 ED_SEVERITY_ALARM, now_us);
        monitor.no_block_since_us = 0U;
    }
}

static uint32_t monitor_crc32(const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xffffffffU;
    size_t index;
    unsigned bit;
    for (index = 0U; index < length; ++index)
    {
        crc ^= bytes[index];
        for (bit = 0U; bit < 8U; ++bit)
            crc = (crc >> 1U) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
    }
    return ~crc;
}

static int monitor_baseline_finite(const ed_baseline_t *baseline)
{
    unsigned index;
    if (!baseline)
        return 0;
    for (index = 0U; index < ED_FEATURE_COUNT; ++index)
    {
        if (!isfinite(baseline->median[index]) || !isfinite(baseline->mad[index]))
            return 0;
    }
    for (index = 0U; index < ED_BAND_COUNT; ++index)
        if (!isfinite(baseline->band_mean_g2[index]))
            return 0;
    for (index = 0U; index < 3U; ++index)
        if (!isfinite(baseline->gravity_mean_g[index]))
            return 0;
    return 1;
}

static int monitor_alarm_submit(void *user, const ed_event_t *event,
                                const ed_ack_request_t *ack)
{
    (void)user;
    (void)ack;
    if (event == RT_NULL)
        return -1;
    /* The journal worker owns the fsync boundary. MQTT publication is started
     * only after monitor_process_journal_commits observes that boundary. */
    return ed_event_journal_submit(event) == 0 ? 0 : -2;
}

static int monitor_replay_journal_event(const ed_event_t *event)
{
    ed_event_t copy;
    if (!event)
        return -1;
    copy = *event;
    return ed_mqtt_enqueue_revision(&copy);
}

static int monitor_load_baseline(void)
{
    ed_baseline_file_t file;
    ed_baseline_t *candidate = &file.value;
    int fd;
    int length;
    fd = open(ED_MONITOR_BASELINE_PATH, O_RDONLY, 0);
    if (fd < 0)
        return -1;
    length = read(fd, &file, sizeof(file));
    close(fd);
    if (length != (int)sizeof(file) || file.magic != ED_BASELINE_FILE_MAGIC ||
        file.format != ED_BASELINE_FILE_FORMAT || file.length != sizeof(file.value) ||
        file.crc32 == 0U || monitor_crc32(&file, offsetof(ed_baseline_file_t, crc32)) != file.crc32 ||
        candidate->valid_windows < 60U || candidate->schema_version != ED_MONITOR_SCHEMA_VERSION ||
        candidate->crc32 == 0U ||
        monitor_crc32(candidate, offsetof(ed_baseline_t, crc32)) != candidate->crc32 ||
        !monitor_baseline_finite(candidate) || !ed_baseline_key_matches(candidate, &monitor.key))
    {
        monitor.baseline_mismatch = 1U;
        return -1;
    }
    monitor.baseline = *candidate;
    monitor.baseline_valid = 1U;
    monitor.baseline_mismatch = 0U;
    return 0;
}

static int monitor_save_baseline(void)
{
    ed_baseline_file_t file;
    int fd;
    int written;
    monitor.baseline.crc32 = monitor_crc32(&monitor.baseline,
                                           offsetof(ed_baseline_t, crc32));
    memset(&file, 0, sizeof(file));
    file.magic = ED_BASELINE_FILE_MAGIC;
    file.format = ED_BASELINE_FILE_FORMAT;
    file.length = sizeof(file.value);
    file.value = monitor.baseline;
    file.crc32 = monitor_crc32(&file, offsetof(ed_baseline_file_t, crc32));
    fd = open(ED_MONITOR_BASELINE_TEMP, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0)
        return -1;
    written = write(fd, &file, sizeof(file));
    if (written == (int)sizeof(file) && fsync(fd) != 0)
        written = -1;
    close(fd);
    if (written != (int)sizeof(file) || rename(ED_MONITOR_BASELINE_TEMP,
                                               ED_MONITOR_BASELINE_PATH) != 0)
        return -1;
    return 0;
}

static void monitor_publish_snapshot(const ed_features_t *features,
                                     ed_quality_t quality, uint8_t idle,
                                     float deviation, uint64_t now_us)
{
    ed_snapshot_t next;
    memset(&next, 0, sizeof(next));
    next.schema_version = ED_MONITOR_SCHEMA_VERSION;
    next.policy_version = monitor.policy.version;
    next.snapshot_id = now_us;
    next.mono_us = now_us;
    next.last_sample_mono_us = now_us;
    next.key = monitor.key;
    next.source = ED_SOURCE_LIVE_IMU;
    next.quality = quality;
    next.idle = idle;
    next.diagnosis = quality != ED_QUALITY_VALID || idle ? ED_DIAG_UNAVAILABLE
        : monitor.baseline_mismatch
        ? ED_DIAG_BASELINE_MISMATCH
        : monitor.baseline_valid
        ? (isfinite(deviation) && deviation >= monitor.policy.alarm_deviation
           ? ED_DIAG_BASELINE_MISMATCH : ED_DIAG_WITHIN_BASELINE)
        : ED_DIAG_NO_BASELINE;
    next.features = *features;
    next.baseline_version = monitor.baseline_valid ? monitor.baseline.version : 0U;
    next.baseline_deviation = deviation;
    if (monitor.lock_ready && rt_mutex_take(&monitor.lock, RT_WAITING_FOREVER) == RT_EOK)
    {
        monitor.snapshot = next;
        rt_mutex_release(&monitor.lock);
    }
}

static void monitor_process_window(uint64_t now_us)
{
    ed_quality_input_t input;
    ed_quality_t quality;
    uint8_t idle;
    uint8_t lock_held = 0U;
    float deviation = NAN;
    if (ed_lf_features_compute((const float (*)[3])monitor.window.xyz,
                               ED_LF_WINDOW_SAMPLES, 208U,
                               &monitor.features) != 0)
        return;
    memset(&input, 0, sizeof(input));
    input.now_us = now_us;
    input.last_sample_mono_us = now_us;
    input.window_samples = ED_LF_WINDOW_SAMPLES;
    input.dynamic_rms_g = monitor.features.values[3];
    input.saturated_samples = monitor.saturated_samples;
    input.configured = 1U;
    quality = ed_quality_step(&monitor.quality, &monitor.policy, &input, &idle);
    if (monitor.lock_ready && rt_mutex_take(&monitor.lock, RT_WAITING_FOREVER) == RT_EOK)
    {
        lock_held = 1U;
        if (monitor.baseline_active &&
            ed_baseline_feed_ex(&monitor.baseline_builder, &monitor.features,
                                quality, (uint8_t)(monitor.baseline_allow_idle ? 0U : idle),
                                0U, now_us) == 0 && monitor.baseline_builder.count >= 60U)
        {
            if (ed_baseline_commit(&monitor.baseline_builder, &monitor.baseline) == 0 &&
                monitor_save_baseline() == 0)
            {
                monitor.baseline_active = 0U;
                monitor.baseline_valid = 1U;
            }
        }
    }
    if (lock_held)
        rt_mutex_release(&monitor.lock);
    if (monitor.baseline_valid && quality == ED_QUALITY_VALID && !idle)
        deviation = ed_baseline_deviation(&monitor.baseline, &monitor.features);
    monitor_publish_snapshot(&monitor.features, quality, idle, deviation, now_us);
    /* Invalid/idle observations reset trigger and recovery runs in the alarm
     * core. Filtering them here would join runs across an unavailable period. */
    if (monitor.lock_ready && rt_mutex_take(&monitor.lock, RT_WAITING_FOREVER) == RT_EOK)
    {
        (void)ed_alarm_step(&monitor.alarm, &monitor.snapshot, now_us);
        rt_mutex_release(&monitor.lock);
    }
    /* A valid window is reported even when the machine is idle. The board is
     * still sampling at this point, and gating telemetry on a finite deviation
     * made a sampling device look like a stale one: the server decides
     * ONLINE/STALE from telemetry receipts, so a resting machine silently
     * dropped off the console. The idle window carries no anomaly score at all
     * rather than a zero one, which would read as "measured, and it was zero". */
    if (quality == ED_QUALITY_VALID)
    {
        ed_mqtt_telemetry_t telemetry;
        memset(&telemetry, 0, sizeof(telemetry));
        rt_snprintf(telemetry.device_id, sizeof(telemetry.device_id), "%s",
                    monitor.key.asset_id);
        telemetry.mono_ms = now_us / 1000U;
        telemetry.rms_vector_g = monitor.features.values[3];
        telemetry.idle = idle;
        if (isfinite(deviation))
        {
            telemetry.deviation_valid = 1U;
            telemetry.anomaly_score = deviation;
            telemetry.alarm = (uint8_t)(deviation >= monitor.policy.alarm_deviation);
        }
        (void)ed_mqtt_enqueue_telemetry(&telemetry);
    }
    ed_lf_window_advance(&monitor.window);
    monitor.saturated_samples = 0U;
}

static void monitor_process_commands(void)
{
    ed_monitor_command_t command;
    while (monitor.command_queue_ready &&
           rt_mq_recv(&monitor.command_queue, &command, sizeof(command),
                      RT_WAITING_NO) == (rt_ssize_t)sizeof(command))
    {
        if (command.kind == ED_MONITOR_COMMAND_ACK)
        {
            ed_command_result_t result;
            if (rt_mutex_take(&monitor.lock, RT_WAITING_FOREVER) == RT_EOK)
            {
                (void)ed_alarm_ack(&monitor.alarm, &command.ack, &result);
                if (result.status == ED_COMMAND_PENDING)
                {
                    ed_event_t pending_event;
                    if (monitor.pending_ack_stage == 0U &&
                        ed_alarm_get_event(&monitor.alarm, command.ack.event_id,
                                           &pending_event) == 0)
                    {
                        monitor.pending_ack_stage = 1U;
                        monitor.pending_ack_final_submitted = 0U;
                        monitor.pending_ack_initial_revision = pending_event.revision;
                        rt_snprintf(monitor.pending_ack_command_id,
                                    sizeof(monitor.pending_ack_command_id), "%s",
                                    command.ack.command_id);
                        rt_snprintf(monitor.pending_ack_event_id,
                                    sizeof(monitor.pending_ack_event_id), "%s",
                                    command.ack.event_id);
                    }
                }
                else
                {
                    /* The alarm refused the ack and no two-phase commit will
                     * follow, so report the outcome now. A replayed journal
                     * event lands here as ED_COMMAND_NOT_FOUND because replay
                     * only requeues it for retransmission and never restores it
                     * to the alarm table. Without a result the server keeps the
                     * command pending and republishes it every two seconds. */
                    (void)ed_mqtt_enqueue_command_result(&result);
                }
                rt_mutex_release(&monitor.lock);
            }
        }
        else if (command.kind == ED_MONITOR_COMMAND_BASELINE_START)
        {
            if (rt_mutex_take(&monitor.lock, RT_WAITING_FOREVER) == RT_EOK)
            {
                ed_baseline_begin_at(&monitor.baseline_builder, &monitor.key,
                                     ed_sensor_now_us());
                monitor.baseline_active = 1U;
                monitor.baseline_allow_idle = command.allow_idle != 0U;
    monitor.baseline_valid = 0U;
    monitor.baseline_mismatch = 0U;
                rt_mutex_release(&monitor.lock);
            }
        }
        else if (command.kind == ED_MONITOR_COMMAND_BASELINE_COMMIT)
        {
            (void)ed_monitor_app_baseline_commit_now();
        }
        else if (command.kind == ED_MONITOR_COMMAND_BASELINE_LOAD)
        {
            (void)ed_monitor_app_baseline_load_now();
        }
        else if (command.kind == ED_MONITOR_COMMAND_SENSOR_START ||
                 command.kind == ED_MONITOR_COMMAND_SENSOR_STOP)
        {
            int start = command.kind == ED_MONITOR_COMMAND_SENSOR_START;
            int status = start ? ed_sensor_start(&monitor.sensor_config) : ed_sensor_stop();
            rt_kprintf("edmon: sensor %s status=%d\n", start ? "start" : "stop", status);
            ed_lf_window_init(&monitor.window);
            ed_quality_init(&monitor.quality);
            monitor.saturated_samples = 0U;
            monitor.no_block_since_us = ed_sensor_now_us();
            if (rt_mutex_take(&monitor.lock, RT_WAITING_FOREVER) == RT_EOK)
            {
                monitor.snapshot.quality = start && status == 0
                    ? ED_QUALITY_WARMUP : ED_QUALITY_DISCONNECTED;
                monitor.snapshot.diagnosis = ED_DIAG_UNAVAILABLE;
                monitor.snapshot.baseline_deviation = NAN;
                (void)ed_alarm_step(&monitor.alarm, &monitor.snapshot,
                                    monitor.no_block_since_us);
                rt_mutex_release(&monitor.lock);
            }
        }
    }
}

static void monitor_process_journal_commits(void)
{
    ed_event_t committed;
    while (ed_event_journal_take_committed(&committed) == 0)
    {
        ed_event_t current;
        if (rt_mutex_take(&monitor.lock, RT_WAITING_FOREVER) != RT_EOK)
            continue;
        (void)ed_alarm_mark_durable(&monitor.alarm, committed.event_id,
                                    committed.revision);
        if (monitor.pending_ack_stage == 1U &&
            strcmp(monitor.pending_ack_event_id, committed.event_id) == 0 &&
            committed.revision == monitor.pending_ack_initial_revision)
        {
            ed_command_result_t pending_result;
            if (ed_alarm_complete_pending(&monitor.alarm,
                                          monitor.pending_ack_event_id,
                                          committed.revision + 1U,
                                          &pending_result) == 0)
            {
                monitor.pending_ack_final_revision = committed.revision + 1U;
                monitor.pending_ack_stage = 2U;
                monitor.pending_ack_final_submitted = 0U;
            }
        }
        if (monitor.pending_ack_stage == 3U &&
            strcmp(monitor.pending_ack_event_id, committed.event_id) == 0 &&
            committed.revision == monitor.pending_ack_final_revision &&
            ed_alarm_get_event(&monitor.alarm, committed.event_id, &current) == 0)
        {
            ed_command_result_t result;
            memset(&result, 0, sizeof(result));
            result.status = ED_COMMAND_OK;
            rt_snprintf(result.command_id, sizeof(result.command_id), "%s",
                        monitor.pending_ack_command_id);
            rt_snprintf(result.event_id, sizeof(result.event_id), "%s",
                        committed.event_id);
            result.current_revision = committed.revision;
            (void)ed_mqtt_enqueue_revision(&current);
            (void)ed_mqtt_enqueue_command_result(&result);
            /* Preserve the idempotency answer for a duplicate command after
             * the two journal commits have completed. */
            rt_snprintf(monitor.alarm.last_command_id,
                        sizeof(monitor.alarm.last_command_id), "%s",
                        monitor.pending_ack_command_id);
            rt_snprintf(monitor.alarm.last_command_event_id,
                        sizeof(monitor.alarm.last_command_event_id), "%s",
                        monitor.pending_ack_event_id);
            monitor.alarm.last_command_expected_revision =
                monitor.pending_ack_initial_revision;
            monitor.alarm.last_command_result = result;
            monitor.pending_ack_stage = 0U;
            monitor.pending_ack_final_submitted = 0U;
            monitor.pending_ack_command_id[0] = '\0';
            monitor.pending_ack_event_id[0] = '\0';
        }
        else if (!(monitor.pending_ack_stage == 1U &&
                   strcmp(monitor.pending_ack_event_id, committed.event_id) == 0 &&
                   committed.revision == monitor.pending_ack_initial_revision))
        {
            if (ed_alarm_get_event(&monitor.alarm, committed.event_id, &current) == 0 &&
                current.durable != 0U)
                (void)ed_mqtt_enqueue_revision(&current);
        }
        rt_mutex_release(&monitor.lock);
    }
    if (monitor.pending_ack_stage == 2U && !monitor.pending_ack_final_submitted &&
        rt_mutex_take(&monitor.lock, RT_WAITING_FOREVER) == RT_EOK)
    {
        ed_event_t completed;
        if (ed_alarm_get_event(&monitor.alarm, monitor.pending_ack_event_id,
                               &completed) == 0 &&
            ed_event_journal_submit(&completed) == 0)
        {
            monitor.pending_ack_final_submitted = 1U;
            monitor.pending_ack_stage = 3U;
        }
        rt_mutex_release(&monitor.lock);
    }
}

static void monitor_entry(void *parameter)
{
    (void)parameter;
    /* Let console, LVGL and network initialization finish before any sensor
     * probe.  Some BSP I2C drivers can spend a bounded but noticeable time
     * waiting for a bus that is physically absent. */
    rt_thread_mdelay(3000U);
    if (ed_sensor_start(&monitor.sensor_config) != 0)
        rt_kprintf("edmon: sensor start deferred/failed; use edgeimu start to retry\n");
    while (monitor.running)
    {
        ed_imu_block_t block;
        uint16_t index;
        monitor_process_journal_commits();
        monitor_process_commands();
        if (ed_sensor_read_block(&block, 100U) != 0)
        {
            uint64_t now_us = ed_sensor_now_us();
            monitor_update_no_block(now_us);
            /* A stopped/unprobed sensor returns immediately, unlike a queue
             * timeout. Yield so UI, shell and capture recovery can run. */
            rt_thread_mdelay(10U);
            continue;
        }
        monitor_mark_block_received(block.first_sample_mono_us);
        if ((block.flags & (ED_BLOCK_GAP | ED_BLOCK_FIFO_OVERRUN)) != 0U ||
            (monitor.last_config_revision != 0U &&
             block.config_revision != monitor.last_config_revision))
        {
            ed_evidence_capture_mark_gap();
            ed_lf_window_init(&monitor.window);
            monitor.last_config_revision = block.config_revision;
            monitor.saturated_samples = 0U;
            memset(&monitor.features, 0, sizeof(monitor.features));
            {
                ed_quality_input_t gap_input;
                uint8_t idle = 0U;
                memset(&gap_input, 0, sizeof(gap_input));
                gap_input.now_us = block.first_sample_mono_us;
                gap_input.last_sample_mono_us = block.first_sample_mono_us;
                gap_input.window_samples = ED_LF_WINDOW_SAMPLES;
                gap_input.dynamic_rms_g = 0.0f;
                gap_input.configured = 1U;
                gap_input.gap = 1U;
                (void)ed_quality_step(&monitor.quality, &monitor.policy,
                                      &gap_input, &idle);
            }
            if (monitor.lock_ready && rt_mutex_take(&monitor.lock, RT_WAITING_FOREVER) == RT_EOK)
            {
                monitor.snapshot.quality = ED_QUALITY_GAPPED;
                monitor.snapshot.diagnosis = ED_DIAG_UNAVAILABLE;
                monitor.snapshot.baseline_deviation = NAN;
                (void)ed_alarm_step(&monitor.alarm, &monitor.snapshot,
                                    block.first_sample_mono_us);
                rt_mutex_release(&monitor.lock);
            }
            continue;
        }
        monitor.last_config_revision = block.config_revision;
        for (index = 0U; index < block.count; ++index)
        {
            float sample[3];
            sample[0] = (float)block.xyz[index][0] * 0.000122f;
            sample[1] = (float)block.xyz[index][1] * 0.000122f;
            sample[2] = (float)block.xyz[index][2] * 0.000122f;
            if (block.xyz[index][0] >= 32760 || block.xyz[index][0] <= -32760 ||
                block.xyz[index][1] >= 32760 || block.xyz[index][1] <= -32760 ||
                block.xyz[index][2] >= 32760 || block.xyz[index][2] <= -32760)
                ++monitor.saturated_samples;
            if (monitor.calibration_active && monitor.lock_ready &&
                rt_mutex_take(&monitor.lock, RT_WAITING_FOREVER) == RT_EOK)
            {
                monitor.calibration_sum[0] += sample[0];
                monitor.calibration_sum[1] += sample[1];
                monitor.calibration_sum[2] += sample[2];
                monitor.calibration_sum_sq += (double)sample[0] * sample[0] +
                    (double)sample[1] * sample[1] + (double)sample[2] * sample[2];
                monitor.calibration_samples++;
                if (monitor.calibration_samples >= 416U)
                {
                    double mean_sq = 0.0;
                    float mean[3];
                    mean[0] = (float)(monitor.calibration_sum[0] / 416.0);
                    mean[1] = (float)(monitor.calibration_sum[1] / 416.0);
                    mean[2] = (float)(monitor.calibration_sum[2] / 416.0);
                    monitor.calibration_gravity_g = sqrtf(mean[0] * mean[0] +
                                                          mean[1] * mean[1] +
                                                          mean[2] * mean[2]);
                    mean_sq = monitor.calibration_sum_sq / 416.0 -
                        (double)monitor.calibration_gravity_g * monitor.calibration_gravity_g;
                    monitor.calibration_dynamic_rms_g = mean_sq > 0.0
                        ? sqrtf((float)mean_sq) : 0.0f;
                    monitor.calibration_active = 0U;
                    monitor.calibration_complete = 1U;
                }
                rt_mutex_release(&monitor.lock);
            }
            ed_evidence_capture_push(block.xyz[index][0], block.xyz[index][1], block.xyz[index][2]);
            if (ed_lf_window_push(&monitor.window, sample) == 1)
            {
                /* Every 104-sample hop produces a fresh diagnostic snapshot;
                 * the fixed DFT tables keep this work bounded for the 208 Hz
                 * producer without thinning the required 0.5 s cadence. */
                monitor_process_window(block.first_sample_mono_us +
                                       (uint64_t)index * block.sample_period_ns / 1000U);
            }
        }
    }
}

int ed_monitor_app_read_snapshot(ed_snapshot_t *out)
{
    ed_snapshot_t copy;
    if (!monitor.initialized || out == RT_NULL || !monitor.lock_ready)
        return -1;
    if (rt_mutex_take(&monitor.lock, RT_WAITING_NO) != RT_EOK)
        return 0;
    copy = monitor.snapshot;
    rt_mutex_release(&monitor.lock);
    *out = copy;
    return copy.snapshot_id != 0U ? 1 : 0;
}

int ed_monitor_app_is_running(void)
{
    return monitor.running != 0U;
}

int ed_monitor_app_sensor_request(uint8_t start)
{
    ed_monitor_command_t command;
    if (!monitor.running || !monitor.command_queue_ready)
        return -1;
    memset(&command, 0, sizeof(command));
    command.kind = start ? ED_MONITOR_COMMAND_SENSOR_START : ED_MONITOR_COMMAND_SENSOR_STOP;
    return rt_mq_send(&monitor.command_queue, &command, sizeof(command)) == RT_EOK ? 0 : -2;
}

int ed_monitor_app_read_ui_state(ed_monitor_ui_state_t *out)
{
    unsigned index;
    if (!monitor.initialized || !out || !monitor.lock_ready)
        return -1;
    if (rt_mutex_take(&monitor.lock, RT_WAITING_NO) != RT_EOK)
        return 0;
    memset(out, 0, sizeof(*out));
    out->baseline = monitor.baseline;
    out->baseline_windows = monitor.baseline_builder.count;
    out->baseline_active = monitor.baseline_active;
    out->baseline_valid = monitor.baseline_valid;
    out->baseline_timed_out = monitor.baseline_builder.timed_out;
    out->baseline_mismatch = monitor.baseline_mismatch;
    for (index = 0U; index < ED_ACTIVE_EVENTS_MAX; ++index)
    {
        if (monitor.alarm.slots[index].used)
            out->events[out->event_count++] = monitor.alarm.slots[index].event;
    }
    rt_mutex_release(&monitor.lock);
    return 1;
}

int ed_monitor_app_baseline_start(uint8_t allow_idle)
{
    ed_monitor_command_t command;
    if (!monitor.initialized || !monitor.command_queue_ready)
        return -1;
    memset(&command, 0, sizeof(command));
    command.kind = ED_MONITOR_COMMAND_BASELINE_START;
    command.allow_idle = allow_idle != 0U;
    return rt_mq_send(&monitor.command_queue, &command, sizeof(command)) == RT_EOK
        ? 0 : -2;
}

int ed_monitor_app_baseline_status(uint32_t *windows, uint8_t *active,
                                   uint8_t *valid, uint8_t *timed_out)
{
    if (!monitor.initialized || !windows || !active || !valid || !timed_out)
        return -1;
    if (!monitor.lock_ready || rt_mutex_take(&monitor.lock, RT_WAITING_FOREVER) != RT_EOK)
        return -1;
    *windows = monitor.baseline_builder.count;
    *active = monitor.baseline_active;
    *valid = monitor.baseline_valid;
    *timed_out = monitor.baseline_builder.timed_out;
    rt_mutex_release(&monitor.lock);
    return 0;
}

static int ed_monitor_app_baseline_commit_now(void)
{
    if (!monitor.initialized || !monitor.lock_ready ||
        rt_mutex_take(&monitor.lock, RT_WAITING_FOREVER) != RT_EOK)
        return -1;
    if (ed_baseline_commit(&monitor.baseline_builder, &monitor.baseline) != 0 ||
        monitor_save_baseline() != 0)
    {
        rt_mutex_release(&monitor.lock);
        return -1;
    }
    monitor.baseline_active = 0U;
    monitor.baseline_valid = 1U;
    rt_mutex_release(&monitor.lock);
    return 0;
}

int ed_monitor_app_baseline_commit(void)
{
    ed_monitor_command_t command;
    if (!monitor.initialized || !monitor.command_queue_ready)
        return -1;
    memset(&command, 0, sizeof(command));
    command.kind = ED_MONITOR_COMMAND_BASELINE_COMMIT;
    return rt_mq_send(&monitor.command_queue, &command, sizeof(command)) == RT_EOK
        ? 0 : -2;
}

static int ed_monitor_app_baseline_load_now(void)
{
    int result;
    if (!monitor.lock_ready || rt_mutex_take(&monitor.lock, RT_WAITING_FOREVER) != RT_EOK)
        return -1;
    result = monitor_load_baseline();
    rt_mutex_release(&monitor.lock);
    return result;
}

int ed_monitor_app_baseline_load(void)
{
    ed_monitor_command_t command;
    if (!monitor.initialized || !monitor.command_queue_ready)
        return -1;
    memset(&command, 0, sizeof(command));
    command.kind = ED_MONITOR_COMMAND_BASELINE_LOAD;
    return rt_mq_send(&monitor.command_queue, &command, sizeof(command)) == RT_EOK
        ? 0 : -2;
}

int ed_monitor_app_calibration_start(void)
{
    if (!monitor.initialized || !monitor.lock_ready ||
        rt_mutex_take(&monitor.lock, RT_WAITING_FOREVER) != RT_EOK)
        return -1;
    memset(monitor.calibration_sum, 0, sizeof(monitor.calibration_sum));
    monitor.calibration_sum_sq = 0.0;
    monitor.calibration_samples = 0U;
    monitor.calibration_gravity_g = 0.0f;
    monitor.calibration_dynamic_rms_g = 0.0f;
    monitor.calibration_complete = 0U;
    monitor.calibration_active = 1U;
    rt_mutex_release(&monitor.lock);
    return 0;
}

int ed_monitor_app_calibration_status(uint32_t *samples, uint8_t *active,
                                      uint8_t *complete, float *gravity_g,
                                      float *dynamic_rms_g)
{
    if (!monitor.initialized || !samples || !active || !complete ||
        !gravity_g || !dynamic_rms_g || !monitor.lock_ready ||
        rt_mutex_take(&monitor.lock, RT_WAITING_FOREVER) != RT_EOK)
        return -1;
    *samples = monitor.calibration_samples;
    *active = monitor.calibration_active;
    *complete = monitor.calibration_complete;
    *gravity_g = monitor.calibration_gravity_g;
    *dynamic_rms_g = monitor.calibration_dynamic_rms_g;
    rt_mutex_release(&monitor.lock);
    return 0;
}

int ed_monitor_app_post_ack(const ed_ack_request_t *request)
{
    ed_monitor_command_t command;
    if (!monitor.initialized || !monitor.command_queue_ready || !request)
        return -1;
    memset(&command, 0, sizeof(command));
    command.kind = ED_MONITOR_COMMAND_ACK;
    command.ack = *request;
    return rt_mq_send(&monitor.command_queue, &command, sizeof(command)) == RT_EOK
        ? 0 : -2;
}

int ed_monitor_app_read_active_event(ed_event_t *event)
{
    unsigned index;
    if (!monitor.initialized || !event || !monitor.lock_ready ||
        rt_mutex_take(&monitor.lock, RT_WAITING_NO) != RT_EOK)
        return 0;
    for (index = 0U; index < ED_ACTIVE_EVENTS_MAX; ++index)
    {
        if (monitor.alarm.slots[index].used &&
            monitor.alarm.slots[index].event.source == ED_SOURCE_LIVE_IMU &&
            monitor.alarm.slots[index].event.kind == ED_EVENT_MECHANICAL &&
            monitor.alarm.slots[index].event.condition == ED_CONDITION_ACTIVE)
        {
            *event = monitor.alarm.slots[index].event;
            rt_mutex_release(&monitor.lock);
            return 1;
        }
    }
    rt_mutex_release(&monitor.lock);
    return 0;
}

static int ed_monitor_app_init(void)
{
    uint64_t boot_counter;
    memset(&monitor, 0, sizeof(monitor));
    /* The provisioned device identity is the single source of truth for MQTT
     * topics and payloads. The server rejects a message whose payload
     * device_id disagrees with its topic, so the two must never be derived
     * from separate constants. Fall back to the factory asset id when the
     * board has not been provisioned yet. */
    if (ed_mqtt_configured_device(monitor.key.asset_id,
                                  sizeof(monitor.key.asset_id)) != 0)
        strcpy(monitor.key.asset_id, "ed84-001");
    strcpy(monitor.key.sensor_profile_hash, "lsm6ds3trc-208hz-4g-v1");
    strcpy(monitor.key.operating_profile_id, "default");
    ed_lf_window_init(&monitor.window);
    ed_monitor_policy_default(&monitor.policy);
    ed_quality_init(&monitor.quality);
    if (rt_mutex_init(&monitor.lock, "edmon", RT_IPC_FLAG_PRIO) != RT_EOK)
        return -RT_ERROR;
    monitor.lock_ready = 1U;
    if (rt_mq_init(&monitor.command_queue, "edmc", monitor.command_pool,
                   sizeof(ed_monitor_command_t), sizeof(monitor.command_pool),
                   RT_IPC_FLAG_PRIO) != RT_EOK)
        return -RT_ERROR;
    monitor.command_queue_ready = 1U;
    memset(&monitor.sensor_config, 0, sizeof(monitor.sensor_config));
    strcpy(monitor.sensor_config.profile_id, "imu_lf_208_v1");
    monitor.sensor_config.sensitivity_g_per_adc = 0.000122f;
    if (ed_event_journal_init("/flash/edgedoctor/events.journal") != 0)
    {
        rt_kprintf("edmon: event journal unavailable; alarms disabled\n");
    }
    else
    {
        int replayed = ed_event_journal_replay(monitor_replay_journal_event);
        if (replayed > 0)
            rt_kprintf("edmon: queued %d journal events for retransmission\n", replayed);
    }
    (void)monitor_load_baseline();
    boot_counter = ed_event_journal_boot_counter();
    if (boot_counter == 0U)
        rt_kprintf("edmon: boot identity not durable; new alarms disabled\n");
    if (ed_alarm_init(&monitor.alarm, monitor.key.asset_id, "boot",
                      boot_counter, boot_counter != 0U,
                      monitor_alarm_submit, RT_NULL) != 0)
        return -RT_ERROR;
    monitor.running = 1U;
    monitor.initialized = 1U;
    if (rt_thread_init(&monitor.thread, "edmon", monitor_entry, RT_NULL,
                       monitor.stack, sizeof(monitor.stack),
                       ED_MONITOR_PRIORITY, 10U) != RT_EOK)
        return -RT_ERROR;
    return rt_thread_startup(&monitor.thread) == RT_EOK ? RT_EOK : -RT_ERROR;
}
INIT_APP_EXPORT(ed_monitor_app_init);
