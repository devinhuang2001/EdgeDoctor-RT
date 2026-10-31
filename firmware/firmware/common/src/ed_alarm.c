#include "ed_alarm.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void copy_text(char *destination, size_t capacity, const char *source)
{
    size_t length;
    if (!destination || capacity == 0U)
        return;
    length = strlen(source ? source : "");
    if (length >= capacity)
        length = capacity - 1U;
    memcpy(destination, source ? source : "", length);
    destination[length] = '\0';
}

static int same_key(const ed_asset_key_t *left, const ed_asset_key_t *right)
{
    return left && right && left->mount_revision == right->mount_revision &&
           strcmp(left->asset_id, right->asset_id) == 0 &&
           strcmp(left->sensor_profile_hash, right->sensor_profile_hash) == 0 &&
           strcmp(left->operating_profile_id, right->operating_profile_id) == 0;
}

static ed_alarm_slot_t *find_event(ed_alarm_t *core, const char *event_id)
{
    unsigned index;
    for (index = 0U; index < ED_ACTIVE_EVENTS_MAX; ++index)
        if (core->slots[index].used &&
            strcmp(core->slots[index].event.event_id, event_id) == 0)
            return &core->slots[index];
    return NULL;
}

static ed_alarm_slot_t *find_active(ed_alarm_t *core, const ed_asset_key_t *key,
                                    ed_source_t source, ed_event_kind_t kind)
{
    unsigned index;
    for (index = 0U; index < ED_ACTIVE_EVENTS_MAX; ++index)
    {
        ed_alarm_slot_t *slot = &core->slots[index];
        if (slot->used && slot->event.condition == ED_CONDITION_ACTIVE &&
            slot->event.source == source && slot->event.kind == kind &&
            same_key(&slot->event.key, key))
            return slot;
    }
    return NULL;
}

static int submit_event(ed_alarm_t *core, ed_alarm_slot_t *slot,
                        const ed_event_t *previous)
{
    if (!core->submit || core->submit(core->storage_user, &slot->event, NULL) == 0)
        return 0;
    if (previous)
        slot->event = *previous;
    core->lost_event_revisions++;
    return -3;
}

int ed_alarm_init(ed_alarm_t *core, const char *device_id, const char *boot_id,
                  uint64_t durable_boot_counter, int identity_valid,
                  ed_alarm_storage_submit_t submit, void *user)
{
    if (!core || !device_id || !boot_id)
        return -1;
    memset(core, 0, sizeof(*core));
    ed_monitor_policy_default(&core->policy);
    copy_text(core->device_id, sizeof(core->device_id), device_id);
    copy_text(core->boot_id, sizeof(core->boot_id), boot_id);
    core->boot_counter = durable_boot_counter;
    core->identity_valid = (uint8_t)(identity_valid != 0);
    core->submit = submit;
    core->storage_user = user;
    return 0;
}

int ed_alarm_condition(ed_alarm_t *core, const ed_asset_key_t *key,
                       ed_source_t source, ed_event_kind_t kind, int active,
                       ed_severity_t severity, uint64_t now_us)
{
    ed_alarm_slot_t *slot;
    unsigned index;
    int submit_status;
    if (!core || !key || source != ED_SOURCE_LIVE_IMU)
        return -1;
    slot = find_active(core, key, source, kind);
    if (!active)
    {
        if (!slot)
            return 0;
        {
            ed_event_t previous = slot->event;
            slot->event.condition = ED_CONDITION_RECOVERED;
            slot->event.revision++;
            slot->event.changed_mono_us = now_us;
            slot->event.recovered_mono_us = now_us;
            submit_status = submit_event(core, slot, &previous);
            return submit_status;
        }
    }
    if (slot)
    {
        if (severity <= slot->event.severity)
            return 0;
        {
            ed_event_t previous = slot->event;
            slot->event.severity = severity;
            slot->event.revision++;
            slot->event.changed_mono_us = now_us;
            return submit_event(core, slot, &previous);
        }
    }
    for (index = 0U; index < ED_ACTIVE_EVENTS_MAX; ++index)
    {
        if (!core->slots[index].used ||
            (core->slots[index].event.condition == ED_CONDITION_RECOVERED &&
             core->slots[index].event.ack == ED_ACKNOWLEDGED &&
             core->slots[index].event.durable != 0U))
        {
            slot = &core->slots[index];
            memset(slot, 0, sizeof(*slot));
            slot->used = 1U;
            slot->event.schema_version = ED_MONITOR_SCHEMA_VERSION;
            slot->event.revision = 1U;
            slot->event.key = *key;
            slot->event.source = source;
            slot->event.kind = kind;
            slot->event.severity = severity;
            slot->event.condition = ED_CONDITION_ACTIVE;
            slot->event.ack = ED_UNACKNOWLEDGED;
            slot->event.created_mono_us = now_us;
            slot->event.changed_mono_us = now_us;
            (void)snprintf(slot->event.event_id, sizeof(slot->event.event_id),
                            "%s-%llu-%llu", core->device_id,
                            (unsigned long long)core->boot_counter,
                            (unsigned long long)++core->next_event_seq);
            submit_status = submit_event(core, slot, NULL);
            if (submit_status != 0)
            {
                /* A rejected first revision never became durable. Reclaim the
                 * slot so the next valid window can retry the event. */
                memset(slot, 0, sizeof(*slot));
            }
            return submit_status;
        }
    }
    core->overflow_count++;
    return -2;
}

int ed_alarm_step(ed_alarm_t *core, const ed_snapshot_t *snapshot, uint64_t now_us)
{
    ed_alarm_slot_t *slot;
    float deviation;
    ed_severity_t severity;
    unsigned index;
    int submit_status;
    if (!core || !snapshot)
        return -1;
    if (snapshot->source != ED_SOURCE_LIVE_IMU ||
        snapshot->quality != ED_QUALITY_VALID || snapshot->idle ||
        !isfinite(snapshot->baseline_deviation))
    {
        core->candidate_valid = 0U;
        core->candidate_count = 0U;
        for (index = 0U; index < ED_ACTIVE_EVENTS_MAX; ++index)
            if (core->slots[index].used)
                core->slots[index].recovery_count = 0U;
        return 0;
    }
    deviation = snapshot->baseline_deviation;
    slot = find_active(core, &snapshot->key, snapshot->source, ED_EVENT_MECHANICAL);
    if (deviation >= core->policy.warning_deviation)
    {
        severity = deviation >= core->policy.alarm_deviation
            ? ED_SEVERITY_ALARM : ED_SEVERITY_WARNING;
        if (slot)
        {
            slot->recovery_count = 0U;
            if (severity > slot->event.severity)
            {
                ed_event_t previous = slot->event;
                slot->event.severity = severity;
                slot->event.revision++;
                slot->event.changed_mono_us = now_us;
                submit_status = submit_event(core, slot, &previous);
                return submit_status;
            }
            return 0;
        }
        if (!core->candidate_valid || !same_key(&core->candidate_key, &snapshot->key))
        {
            core->candidate_key = snapshot->key;
            core->candidate_count = 0U;
            core->candidate_valid = 1U;
        }
        if (core->candidate_count < core->policy.trigger_outputs)
            core->candidate_count++;
        if (core->candidate_count >= core->policy.trigger_outputs)
        {
            core->candidate_valid = 0U;
            core->candidate_count = 0U;
            submit_status = ed_alarm_condition(core, &snapshot->key,
                                               snapshot->source,
                                               ED_EVENT_MECHANICAL, 1,
                                               severity, now_us);
            if (submit_status == -2)
            {
                core->overflow_count++;
                return 0;
            }
            return submit_status;
        }
        return 0;
    }
    core->candidate_valid = 0U;
    core->candidate_count = 0U;
    if (slot)
    {
        if (deviation <= core->policy.recovery_deviation)
        {
            if (slot->recovery_count < core->policy.recovery_outputs)
                slot->recovery_count++;
            if (slot->recovery_count >= core->policy.recovery_outputs)
                return ed_alarm_condition(core, &snapshot->key, snapshot->source,
                                          ED_EVENT_MECHANICAL, 0,
                                          slot->event.severity, now_us);
        }
        else
            slot->recovery_count = 0U;
    }
    return 0;
}

int ed_alarm_ack(ed_alarm_t *core, const ed_ack_request_t *request,
                 ed_command_result_t *out)
{
    ed_alarm_slot_t *slot;
    int submit_status;
    if (!core || !request || !out)
        return -1;
    if (request->command_id[0] != '\0' &&
        strcmp(core->last_command_id, request->command_id) == 0 &&
        strcmp(core->last_command_event_id, request->event_id) == 0 &&
        core->last_command_expected_revision == request->expected_revision)
    {
        *out = core->last_command_result;
        return 0;
    }
    memset(out, 0, sizeof(*out));
    copy_text(out->command_id, sizeof(out->command_id), request->command_id);
    copy_text(out->event_id, sizeof(out->event_id), request->event_id);
    slot = find_event(core, request->event_id);
    if (!slot)
        out->status = ED_COMMAND_NOT_FOUND;
    else if (slot->event.ack == ED_ACKNOWLEDGED)
        out->status = ED_COMMAND_ALREADY_ACKNOWLEDGED;
    else if (request->expected_revision != slot->event.revision)
    {
        out->status = ED_COMMAND_CONFLICT;
        out->current_revision = slot->event.revision;
    }
    else if (!core->submit)
        out->status = ED_COMMAND_STORAGE_ERROR;
    else
    {
        slot->pending_ack = *request;
        slot->event.ack_pending = 1U;
        slot->event.changed_mono_us = request->received_mono_us;
        out->status = ED_COMMAND_PENDING;
        submit_status = core->submit(core->storage_user, &slot->event, request);
        if (submit_status != 0)
        {
            slot->event.ack_pending = 0U;
            out->status = ED_COMMAND_STORAGE_ERROR;
        }
    }
    if (slot)
        out->current_revision = slot->event.revision;
    copy_text(core->last_command_id, sizeof(core->last_command_id), request->command_id);
    copy_text(core->last_command_event_id, sizeof(core->last_command_event_id), request->event_id);
    core->last_command_expected_revision = request->expected_revision;
    core->last_command_result = *out;
    if (out->status == ED_COMMAND_STORAGE_ERROR)
        core->last_command_id[0] = '\0';
    return 0;
}

int ed_alarm_complete(ed_alarm_t *core, const char *event_id, uint32_t revision,
                      int durable_success, ed_command_result_t *out)
{
    ed_alarm_slot_t *slot;
    if (!core || !event_id || !out)
        return -1;
    slot = find_event(core, event_id);
    if (!slot || !slot->event.ack_pending || revision != slot->event.revision + 1U)
        return -1;
    slot->event.ack_pending = 0U;
    if (durable_success)
    {
        slot->event.ack = ED_ACKNOWLEDGED;
        slot->event.revision = revision;
        slot->event.acknowledged_mono_us = slot->event.changed_mono_us;
        slot->event.durable = 1U;
        out->status = ED_COMMAND_OK;
    }
    else
        out->status = ED_COMMAND_STORAGE_ERROR;
    copy_text(out->event_id, sizeof(out->event_id), event_id);
    out->current_revision = slot->event.revision;
    core->last_command_id[0] = '\0';
    core->last_command_result = *out;
    return 0;
}

int ed_alarm_complete_pending(ed_alarm_t *core, const char *event_id,
                              uint32_t revision, ed_command_result_t *out)
{
    ed_alarm_slot_t *slot;
    if (!core || !event_id || !out)
        return -1;
    slot = find_event(core, event_id);
    if (!slot || !slot->event.ack_pending ||
        revision != slot->event.revision + 1U)
        return -1;
    slot->event.ack_pending = 0U;
    slot->event.ack = ED_ACKNOWLEDGED;
    slot->event.revision = revision;
    slot->event.acknowledged_mono_us = slot->event.changed_mono_us;
    slot->event.durable = 0U;
    memset(out, 0, sizeof(*out));
    out->status = ED_COMMAND_PENDING;
    copy_text(out->event_id, sizeof(out->event_id), event_id);
    out->current_revision = revision;
    return 0;
}

int ed_alarm_mark_durable(ed_alarm_t *core, const char *event_id,
                          uint32_t revision)
{
    ed_alarm_slot_t *slot;
    if (!core || !event_id)
        return -1;
    slot = find_event(core, event_id);
    if (!slot || slot->event.revision != revision)
        return -1;
    slot->event.durable = 1U;
    return 0;
}

int ed_alarm_get_event(const ed_alarm_t *core, const char *event_id,
                       ed_event_t *out)
{
    unsigned index;
    if (!core || !event_id || !out)
        return -1;
    for (index = 0U; index < ED_ACTIVE_EVENTS_MAX; ++index)
        if (core->slots[index].used &&
            strcmp(core->slots[index].event.event_id, event_id) == 0)
        {
            *out = core->slots[index].event;
            return 0;
        }
    return -1;
}
