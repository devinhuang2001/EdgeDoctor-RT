#ifndef ED_ALARM_H
#define ED_ALARM_H
#include "ed_monitor_types.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Submit must copy record into a bounded queue and return immediately. 0 accepted;
 * nonzero rejected. Completion MUST be serialized later, never inside submit.
 * All APIs on a context belong to one owner (runtime command queue in W2/W4).
 * Eight slots retain unresolved/recovered-unacknowledged events. Fully resolved
 * slots may be reused only after durable completion; archive lookup is W4. */
typedef int (*ed_alarm_storage_submit_t)(void *user, const ed_event_t *record, const ed_ack_request_t *ack);
typedef struct {
    ed_event_t event; ed_ack_request_t pending_ack;
    uint64_t last_valid_us, invalid_since_us, quality_valid_since_us;
    uint16_t warning_count, alarm_count, recovery_count;
    uint8_t used, has_event, invalid_tracking, quality_valid_tracking;
} ed_alarm_slot_t;
typedef struct {
    ed_alarm_slot_t slots[ED_ACTIVE_EVENTS_MAX]; ed_monitor_policy_t policy;
    char device_id[ED_ID_MAX], boot_id[ED_ID_MAX]; uint64_t boot_counter, next_event_seq, last_now_us;
    uint32_t overflow_count, lost_event_revisions; uint8_t identity_valid;
    ed_asset_key_t candidate_key; uint16_t candidate_count; uint8_t candidate_valid;
    char last_command_id[ED_ID_MAX]; char last_command_event_id[ED_EVENT_ID_MAX];
    uint32_t last_command_expected_revision; ed_command_result_t last_command_result;
    ed_alarm_storage_submit_t submit; void *storage_user;
} ed_alarm_t;
int ed_alarm_init(ed_alarm_t *core, const char *device_id, const char *boot_id, uint64_t durable_boot_counter, int identity_valid, ed_alarm_storage_submit_t submit, void *user);
int ed_alarm_step(ed_alarm_t *core, const ed_snapshot_t *snapshot, uint64_t now_us);
/* Explicit quality/storage/trend/mount input; replay sources cannot create formal events. */
int ed_alarm_condition(ed_alarm_t *core, const ed_asset_key_t *key, ed_source_t source, ed_event_kind_t kind, int active, ed_severity_t severity, uint64_t now_us);
int ed_alarm_ack(ed_alarm_t *core, const ed_ack_request_t *request, ed_command_result_t *out);
int ed_alarm_complete(ed_alarm_t *core, const char *event_id, uint32_t revision, int durable_success, ed_command_result_t *out);
/* Complete an ACK after the pending revision has reached the local journal.
 * The resulting event remains non-durable until its own journal commit. */
int ed_alarm_complete_pending(ed_alarm_t *core, const char *event_id,
                              uint32_t revision, ed_command_result_t *out);
int ed_alarm_mark_durable(ed_alarm_t *core, const char *event_id,
                          uint32_t revision);
int ed_alarm_get_event(const ed_alarm_t *core, const char *event_id, ed_event_t *out);
#ifdef __cplusplus
}
#endif
#endif
