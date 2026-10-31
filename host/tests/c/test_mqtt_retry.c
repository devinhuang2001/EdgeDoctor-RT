#include "ed_mqtt_client.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned calls;
static int publish(void *user, const char *topic, const char *payload, size_t length)
{
    (void)user; (void)topic; (void)payload; (void)length;
    ++calls;
    return 1;
}
int main(void)
{
    ed_mqtt_config_t config = {0};
    ed_event_t event = {0};
    ed_mqtt_state_t state;
    strcpy(config.site_id, "site"); strcpy(config.device_id, "pump");
    strcpy(config.broker_host, "localhost"); config.broker_port = 1883; config.keepalive_s = 30;
    strcpy(event.event_id, "pump-1-1"); strcpy(event.key.asset_id, "pump");
    event.schema_version = 2; event.revision = 1;
    assert(ed_mqtt_bind_transport(&config, publish, NULL) == 0);
    assert(ed_mqtt_enqueue_revision(&event) == 0);
    assert(ed_mqtt_flush(8) == 1 && calls == 1);
    ed_mqtt_mark_disconnected();
    assert(ed_mqtt_bind_transport(&config, publish, NULL) == 0);
    assert(ed_mqtt_flush(8) == 1 && calls == 2);
    assert(ed_mqtt_flush_at(8, 9999) == 0);
    assert(ed_mqtt_flush_at(8, 10000) == 1 && calls == 3);
    assert(ed_mqtt_flush_at(8, 10000) == 0);
    ed_mqtt_get_state(&state);
    assert(state.queued == 1);
    assert(ed_mqtt_mark_receipt(event.event_id, 1) == 0);
    assert(ed_mqtt_flush(8) == 0);
    assert(ed_mqtt_flush_at(8, 20000) == 0 && calls == 3);
    puts("mqtt reconnect retry: passed");
    return 0;
}
