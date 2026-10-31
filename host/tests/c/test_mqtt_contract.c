#include "ed_mqtt_client.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned publishes;

static int capture_publish(void *user, const char *topic,
                           const char *payload, size_t length)
{
    (void)user;
    publishes++;
    assert(strstr(topic, "edgedoctor/site_1/ed84-001/") == topic);
    assert(length > 0U &&
           (strstr(payload, "event_id") != NULL ||
            strstr(payload, "rms_vector_g") != NULL ||
            strstr(payload, "command_id") != NULL));
    /* Simulate an unacknowledged command-result while telemetry is ACKed. */
    return strstr(payload, "command_id") == NULL ? 1 : 0;
}

int main(void)
{
    ed_mqtt_config_t config = {0};
    ed_event_t event = {0};
    ed_mqtt_telemetry_t telemetry = {0};
    ed_command_result_t result = {0};
    char output[512];
    char topic[128];
    size_t written;
    ed_mqtt_state_t state;
    ed_mqtt_ui_state_t ui_state;

    strcpy(config.site_id, "site_1");
    strcpy(config.device_id, "ed84-001");
    strcpy(config.broker_host, "broker");
    config.broker_port = 8883;
    config.keepalive_s = 30;
    assert(ed_mqtt_validate_config(&config) == 0);
    strcpy(config.broker_host, "192.168.1.10");
    assert(ed_mqtt_validate_config(&config) == 0);
    strcpy(config.broker_host, "broker");
    assert(ed_mqtt_build_topic(config.site_id, config.device_id, "events",
                               topic, sizeof(topic)) == 0);
    assert(strcmp(topic, "edgedoctor/site_1/ed84-001/events") == 0);
    config.device_id[0] = '!';
    assert(ed_mqtt_validate_config(&config) < 0);
    strcpy(config.device_id, "ed84-001");

    strcpy(event.event_id, "e1");
    strcpy(event.key.asset_id, "ed84-001");
    event.schema_version = 2;
    event.revision = 1;
    event.source = ED_SOURCE_LIVE_IMU;
    event.severity = ED_SEVERITY_ALARM;
    event.durable = 1U;
    assert(ed_mqtt_encode_event(&event, output, sizeof(output), &written) == 0);
    assert(written > 0U && strstr(output, "\"device_id\":\"ed84-001\"") != NULL &&
           strstr(output, "\"source\":\"LIVE_IMU\"") != NULL &&
           strstr(output, "\"severity\":\"ALARM\"") != NULL &&
           strstr(output, "\"condition\":\"ACTIVE\"") != NULL);
    strcpy(event.event_id, "e\\\"1");
    assert(ed_mqtt_encode_event(&event, output, sizeof(output), &written) == 0 &&
           strstr(output, "e\\\\\\\"1") != NULL);

    assert(ed_mqtt_enqueue_revision(&event) == 0);
    assert(ed_mqtt_enqueue_revision(&event) == 0);
    ed_mqtt_get_state(&state);
    assert(state.queued == 1U);
    assert(ed_mqtt_bind_transport(&config, capture_publish, NULL) == 0);
    ed_mqtt_get_ui_state(&ui_state);
    assert(strcmp(ui_state.broker_host, "broker") == 0 && ui_state.broker_port == 8883);
    assert(ui_state.pending_events == 1 && ui_state.event_receipts == 0);
    assert(ed_mqtt_flush(1U) == 1 && publishes == 1U);
    ed_mqtt_get_state(&state);
    assert(state.queued == 1U && state.sent == 1U);
    assert(ed_mqtt_mark_receipt("e\\\"1", 1U) == 0);
    assert(ed_mqtt_mark_receipt("e\\\"1", 1U) < 0);

    strcpy(telemetry.device_id, "ed84-001");
    telemetry.rms_vector_g = 0.1F;
    assert(ed_mqtt_enqueue_telemetry(&telemetry) == 0);
    strcpy(result.command_id, "cmd-1");
    strcpy(result.event_id, "e\\\"1");
    result.status = ED_COMMAND_OK;
    assert(ed_mqtt_enqueue_command_result(&result) == 0);
    ed_mqtt_get_state(&state);
    assert(state.queued == 2U);
    /* The unacknowledged command result is attempted once; telemetry still flows. */
    assert(ed_mqtt_flush(2U) == 2);
    ed_mqtt_get_state(&state);
    assert(state.queued == 1U);

    /* A telemetry PUBACK must never be displayed as an event database receipt. */
    ed_mqtt_get_ui_state(&ui_state);
    assert(ui_state.event_receipts == 1 && ui_state.pending_events == 0);
    assert(strcmp(ui_state.last_receipt_event, event.event_id) == 0);
    assert(ui_state.last_receipt_revision == 1);
    ed_mqtt_mark_disconnected();
    ed_mqtt_get_ui_state(&ui_state);
    assert(strcmp(ui_state.broker_host, "broker") == 0 && ui_state.event_receipts == 1);

    puts("mqtt contract: all tests passed");
    return 0;
}
