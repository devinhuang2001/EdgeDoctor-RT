#ifndef ED_MONITOR_TYPES_H
#define ED_MONITOR_TYPES_H
#include <stdint.h>
#define ED_MONITOR_SCHEMA_VERSION 2u
#define ED_IMU_BLOCK_MAX 32u
#define ED_FEATURE_COUNT 15u
#define ED_BAND_COUNT 8u
#define ED_ID_MAX 48u
#define ED_EVENT_ID_MAX 96u
#define ED_TEXT_MAX 96u
#define ED_ACTIVE_EVENTS_MAX 8u
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { ED_SOURCE_LIVE_IMU=0, ED_SOURCE_KAIST_REPLAY=1, ED_SOURCE_EVIDENCE_REPLAY=2, ED_SOURCE_DEMO=3 } ed_source_t;
typedef enum { ED_QUALITY_WARMUP=0, ED_QUALITY_VALID=1, ED_QUALITY_STALE=2, ED_QUALITY_DISCONNECTED=3, ED_QUALITY_SATURATED=4, ED_QUALITY_GAPPED=5, ED_QUALITY_CONFIG_ERROR=6 } ed_quality_t;
typedef enum { ED_DIAG_NO_BASELINE=0, ED_DIAG_BASELINE_MISMATCH=1, ED_DIAG_WITHIN_BASELINE=2, ED_DIAG_UNKNOWN_ANOMALY=3, ED_DIAG_KNOWN_CLASS=4, ED_DIAG_UNCERTAIN=5, ED_DIAG_UNAVAILABLE=6 } ed_diagnosis_t;
typedef enum { ED_EVENT_MECHANICAL=0, ED_EVENT_SENSOR=1, ED_EVENT_STORAGE=2, ED_EVENT_TREND=3, ED_EVENT_MOUNT=4 } ed_event_kind_t;
typedef enum { ED_SEVERITY_WARNING=1, ED_SEVERITY_ALARM=2 } ed_severity_t;
typedef enum { ED_CONDITION_ACTIVE=0, ED_CONDITION_RECOVERED=1 } ed_condition_t;
typedef enum { ED_UNACKNOWLEDGED=0, ED_ACKNOWLEDGED=1 } ed_ack_state_t;
typedef enum { ED_COMMAND_OK=0, ED_COMMAND_PENDING=1, ED_COMMAND_NOT_FOUND=2, ED_COMMAND_CONFLICT=3, ED_COMMAND_ALREADY_ACKNOWLEDGED=4, ED_COMMAND_INVALID=5, ED_COMMAND_STORAGE_ERROR=6, ED_COMMAND_IDENTITY_ERROR=7, ED_COMMAND_BUSY=8 } ed_command_status_t;
typedef enum { ED_ORIGIN_LOCAL=0, ED_ORIGIN_REMOTE=1 } ed_command_origin_t;
typedef enum { ED_FEEDBACK_INTERFERENCE=0, ED_FEEDBACK_OPERATING_CHANGE=1, ED_FEEDBACK_CONFIRMED_ANOMALY=2, ED_FEEDBACK_UNCERTAIN=3 } ed_feedback_label_t;
enum { ED_BLOCK_GAP=1u, ED_BLOCK_FIFO_OVERRUN=2u, ED_BLOCK_CLOCK_ESTIMATED=4u };
typedef struct {
    uint64_t first_sample_seq, first_sample_mono_us;
    uint32_t sample_period_ns, config_revision;
    uint16_t count, flags;
    int16_t xyz[ED_IMU_BLOCK_MAX][3];
} ed_imu_block_t;
typedef struct {
    char profile_id[ED_ID_MAX]; uint32_t odr_hz, range_mg, bandwidth_millihz;
    uint8_t axis_map[3]; int8_t axis_sign[3]; uint32_t mount_revision, config_revision;
    float sensitivity_g_per_adc;
} ed_sensor_config_t;
typedef struct {
    uint32_t nominal_hz, measured_millihz;
    uint64_t samples_received, samples_dropped_known, fifo_overruns, unknown_gap_count, bus_errors, last_sample_mono_us;
    uint32_t queue_high_water;
} ed_sensor_stats_t;
typedef struct {
    char asset_id[ED_ID_MAX], sensor_profile_hash[ED_ID_MAX], operating_profile_id[ED_ID_MAX];
    uint32_t mount_revision;
} ed_asset_key_t;
typedef struct { float values[ED_FEATURE_COUNT], raw_mean_g[3], band_energy_g2[ED_BAND_COUNT]; uint16_t valid_mask; } ed_features_t;
typedef struct { uint64_t mono_us, utc_us; char boot_id[ED_ID_MAX]; uint8_t utc_valid; } ed_time_t;
typedef struct {
    uint32_t schema_version, policy_version, version; char baseline_id[ED_ID_MAX], operator_name[ED_ID_MAX];
    ed_asset_key_t key; ed_time_t created; uint32_t valid_windows;
    float median[ED_FEATURE_COUNT], mad[ED_FEATURE_COUNT], band_mean_g2[ED_BAND_COUNT], gravity_mean_g[3]; uint32_t crc32;
} ed_baseline_t;
typedef struct {
    uint32_t schema_version, policy_version; uint64_t snapshot_id, mono_us, last_sample_mono_us;
    ed_asset_key_t key; ed_source_t source; ed_quality_t quality; ed_diagnosis_t diagnosis;
    ed_features_t features; uint32_t baseline_version; float baseline_deviation;
    uint16_t warmup_samples; uint8_t idle, audio_contaminated, operating_transition, mount_suspected;
    uint64_t valid_monitor_us, invalid_monitor_us, longest_unavailable_us;
    float signed_rms_ewma, shadow_score; uint32_t shadow_model_version; uint8_t shadow_valid;
} ed_snapshot_t;
typedef struct {
    uint32_t schema_version, revision; char event_id[ED_EVENT_ID_MAX]; ed_asset_key_t key;
    ed_source_t source; ed_event_kind_t kind; ed_severity_t severity; ed_condition_t condition; ed_ack_state_t ack;
    uint64_t created_mono_us, changed_mono_us, recovered_mono_us, acknowledged_mono_us;
    float peak_deviation; uint32_t baseline_version, policy_version, overflow_count;
    char actor[ED_ID_MAX]; uint8_t durable, ack_pending;
} ed_event_t;
typedef struct {
    char command_id[ED_ID_MAX], event_id[ED_EVENT_ID_MAX], actor[ED_ID_MAX], target_boot_id[ED_ID_MAX];
    uint32_t expected_revision; ed_command_origin_t origin; uint64_t received_mono_us;
} ed_ack_request_t;
typedef struct { ed_command_status_t status; char command_id[ED_ID_MAX], event_id[ED_EVENT_ID_MAX]; uint32_t current_revision; } ed_command_result_t;
typedef struct {
    uint32_t version; float warning_deviation, alarm_deviation, recovery_deviation, idle_rms_g;
    uint16_t trigger_outputs, recovery_outputs; uint64_t stale_us, disconnect_us, idle_us, invalid_reset_us, quality_recovery_us;
} ed_monitor_policy_t;
/* Pure validation, no device reads and no ownership transfer. */
int ed_imu_block_validate(const ed_imu_block_t *block);
void ed_monitor_policy_default(ed_monitor_policy_t *policy);
#ifdef __cplusplus
}
#endif
#endif
