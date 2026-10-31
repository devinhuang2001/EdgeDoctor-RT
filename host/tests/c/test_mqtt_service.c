#include "ed_mqtt_client.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static ed_event_t events[20];
static unsigned inbound, reads, sends, receipt_index;
static int fail_read, flood;
static ed_command_result_t command;
static int update_during_publish;

static int receive(void *user)
{
    (void)user;
    ++reads;
    if (fail_read) return -1;
    if (flood) return 1;
    if (!inbound) return 0;
    --inbound;
    assert(ed_mqtt_mark_receipt(events[receipt_index++].event_id, 1) == 0);
    return 1;
}

static int publish(void *user, const char *topic, const char *payload, size_t length)
{
    (void)user; (void)length;
    ++sends;
    if (strstr(topic, "/events"))
    {
        /* A slow PUBACK collects two frames per publish. Eight sequential
         * sends without a drain would overflow the actual eight-slot inbox. */
        assert(inbound < 8);
        ++inbound;
    }
    if (update_during_publish)
    {
        assert(strstr(payload, "PENDING"));
        command.status = ED_COMMAND_OK;
        command.current_revision = 2;
        assert(ed_mqtt_enqueue_command_result(&command) == 0);
        update_during_publish = 0;
    }
    return 1;
}

int main(void)
{
    ed_mqtt_config_t config = {0};
    ed_mqtt_state_t state;
    unsigned i, before;
    strcpy(config.site_id, "site"); strcpy(config.device_id, "pump");
    strcpy(config.broker_host, "localhost"); config.broker_port = 1883; config.keepalive_s = 30;
    assert(ed_mqtt_bind_transport(&config, publish, NULL) == 0);
    for (i = 0; i < 20; ++i)
    {
        snprintf(events[i].event_id, sizeof(events[i].event_id), "pump-1-%u", i);
        strcpy(events[i].key.asset_id, "pump");
        events[i].schema_version = 2; events[i].revision = 1;
        assert(ed_mqtt_enqueue_revision(&events[i]) == 0);
    }
    assert(ed_mqtt_service(receive, NULL, 8) == 8);
    assert(ed_mqtt_service(receive, NULL, 8) == 8);
    assert(ed_mqtt_service(receive, NULL, 8) == 4);
    ed_mqtt_get_state(&state); assert(state.queued == 0 && inbound == 0);
    before = sends; fail_read = 1;
    assert(ed_mqtt_service(receive, NULL, 8) < 0 && sends == before);
    fail_read = 0; flood = 1; reads = 0;
    assert(ed_mqtt_service(receive, NULL, 8) == 0 && reads == 16 && sends == before);
    flood = 0;
    strcpy(command.command_id, "cmd-race"); strcpy(command.event_id, "pump-1-0");
    command.status = ED_COMMAND_PENDING; command.current_revision = 1;
    assert(ed_mqtt_enqueue_command_result(&command) == 0);
    update_during_publish = 1;
    assert(ed_mqtt_flush(1) == 1);
    ed_mqtt_get_state(&state); assert(state.queued == 1);
    command.status = ED_COMMAND_PENDING;
    assert(ed_mqtt_enqueue_command_result(&command) == 0);
    assert(ed_mqtt_flush(1) == 1);
    ed_mqtt_get_state(&state); assert(state.queued == 0);
    puts("mqtt bounded receipt drain and command completion race: passed");
    return 0;
}
