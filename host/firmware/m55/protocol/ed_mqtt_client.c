#include "ed_mqtt_client.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef EDGEDOCTOR_RT
#include "ed_evidence_capture.h"
#include "edgedoctor_app.h"
#include "ed_sensor.h"
#include <dfs_posix.h>
#include <fcntl.h>
#include <finsh.h>
#include <rtthread.h>
#include <unistd.h>
#endif

#define ED_MQTT_TELEMETRY_MAX 32U
#define ED_MQTT_COMMAND_RESULT_MAX 16U
#define ED_MQTT_THREAD_STACK 12288U
#define ED_MQTT_RECEIPT_RETRY_MS 10000U

typedef struct {
    ed_event_t event;
    uint8_t published;
    uint64_t published_ms;
} ed_mqtt_event_item_t;

static ed_mqtt_event_item_t event_queue[ED_MQTT_OUTBOX_MAX]
    __attribute__((section(".cy_socmem_data"), aligned(8)));
static ed_mqtt_telemetry_t telemetry_queue[ED_MQTT_TELEMETRY_MAX]
    __attribute__((section(".cy_socmem_data"), aligned(8)));
static ed_command_result_t command_result_queue[ED_MQTT_COMMAND_RESULT_MAX]
    __attribute__((section(".cy_socmem_data"), aligned(8)));
static uint16_t event_count;
static uint8_t telemetry_count;
static uint16_t command_result_count;
static ed_mqtt_state_t mqtt_state;
static ed_mqtt_config_t transport_config;
static ed_mqtt_ui_state_t ui_transport;
static ed_mqtt_publish_fn publish_fn;
static void *publish_user;

#ifdef EDGEDOCTOR_RT
static ed_mqtt_config_t shell_config;
static const char shell_config_path[] = "/flash/edgedoctor.mqtt";
static rt_thread_t mqtt_thread;
static volatile rt_bool_t mqtt_thread_running;
static uint32_t mqtt_backoff_ms = 1000U;
static uint64_t mqtt_next_retry_ms;

static void mqtt_schedule_retry(void)
{
    uint32_t jitter_ms = (uint32_t)(rt_tick_get() % 251U);
    mqtt_next_retry_ms = ed_sensor_now_us() / 1000U +
                         mqtt_backoff_ms + jitter_ms;
    if (mqtt_backoff_ms < 30000U)
        mqtt_backoff_ms *= 2U;
    if (mqtt_backoff_ms > 30000U)
        mqtt_backoff_ms = 30000U;
}

static int shell_config_save(void)
{
    const char *temp_path = "/flash/edgedoctor.mqtt.tmp";
    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0);
    int ok;
    if (fd < 0)
        return -1;
    ok = write(fd, &shell_config, sizeof(shell_config)) == (int)sizeof(shell_config);
    if (ok) ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = 0;
    return ok && rename(temp_path, shell_config_path) == 0 ? 0 : -1;
}

static int shell_config_load(void)
{
    unsigned char data[sizeof(ed_mqtt_config_t) + 1U];
    int fd = open(shell_config_path, O_RDONLY, 0);
    int length;
    if (fd < 0)
        return -1;
    length = read(fd, data, sizeof(data));
    close(fd);
    return length > 0 ? ed_mqtt_decode_config(data, (size_t)length, &shell_config) : -1;
}
#endif

static int valid_id(const char *value);

int ed_mqtt_configured_device(char *output, size_t capacity)
{
#ifdef EDGEDOCTOR_RT
    if (!output || capacity == 0U)
        return -1;
    if (shell_config.device_id[0] == '\0' && shell_config_load() != 0)
        return -1;
    if (!valid_id(shell_config.device_id))
        return -1;
    snprintf(output, capacity, "%s", shell_config.device_id);
    return 0;
#else
    (void)output;
    (void)capacity;
    return -1;
#endif
}

static int json_escape(const char *source, char *output, size_t capacity)
{
    size_t input = 0U;
    size_t written = 0U;
    unsigned char value;
    static const char hex[] = "0123456789abcdef";
    if (!source || !output || capacity == 0U)
        return -1;
    while (source[input] != '\0')
    {
        value = (unsigned char)source[input++];
        if (value == '"' || value == '\\')
        {
            if (written + 2U >= capacity)
                return -1;
            output[written++] = '\\';
            output[written++] = (char)value;
        }
        else if (value < 0x20U)
        {
            if (written + 6U >= capacity)
                return -1;
            output[written++] = '\\';
            output[written++] = 'u';
            output[written++] = '0';
            output[written++] = '0';
            output[written++] = hex[value >> 4];
            output[written++] = hex[value & 0x0fU];
        }
        else
        {
            if (written + 1U >= capacity)
                return -1;
            output[written++] = (char)value;
        }
    }
    output[written] = '\0';
    return (int)written;
}

/* Identifiers that reach file paths or MQTT topics accept only this alphabet.
 * Without it a broker-supplied event id such as "../../x" escapes the evidence
 * directory and injects extra topic levels. */
static int valid_token(const char *value, size_t limit)
{
    size_t index;
    size_t length;
    if (!value)
        return 0;
    length = strlen(value);
    if (length == 0U || length >= limit)
        return 0;
    for (index = 0U; index < length; ++index)
    {
        if (!((value[index] >= 'a' && value[index] <= 'z') ||
              (value[index] >= 'A' && value[index] <= 'Z') ||
              (value[index] >= '0' && value[index] <= '9') ||
              value[index] == '_' || value[index] == '-'))
            return 0;
    }
    return 1;
}

static int valid_id(const char *value)
{
    return valid_token(value, ED_ID_MAX);
}

static int valid_host(const char *value)
{
    size_t index;
    size_t length;
    if (!value)
        return 0;
    length = strlen(value);
    if (length == 0U || length >= ED_ID_MAX)
        return 0;
    for (index = 0U; index < length; ++index)
    {
        if (!((value[index] >= 'a' && value[index] <= 'z') ||
              (value[index] >= 'A' && value[index] <= 'Z') ||
              (value[index] >= '0' && value[index] <= '9') ||
              value[index] == '.' || value[index] == '-' ||
              value[index] == ':' || value[index] == '_'))
            return 0;
    }
    return 1;
}

int ed_mqtt_validate_config(const ed_mqtt_config_t *config)
{
    if (!config ||
        !memchr(config->site_id, '\0', sizeof(config->site_id)) ||
        !memchr(config->device_id, '\0', sizeof(config->device_id)) ||
        !memchr(config->broker_host, '\0', sizeof(config->broker_host)) ||
        !memchr(config->username, '\0', sizeof(config->username)) ||
        !memchr(config->password, '\0', sizeof(config->password)) ||
        (config->password[0] != '\0' && config->username[0] == '\0') ||
        !valid_id(config->site_id) || !valid_id(config->device_id) ||
        !valid_host(config->broker_host) || config->broker_port == 0U ||
        config->keepalive_s < 10U || config->keepalive_s > 300U)
        return -1;
    return 0;
}

int ed_mqtt_decode_config(const void *data, size_t length, ed_mqtt_config_t *config)
{
    ed_mqtt_config_t candidate = {0};
    if (!data || !config || (length != sizeof(candidate) &&
                            length != offsetof(ed_mqtt_config_t, username)))
        return -1;
    memcpy(&candidate, data, length);
    if (ed_mqtt_validate_config(&candidate) != 0)
        return -1;
    *config = candidate;
    return 0;
}

static size_t connect_string(unsigned char *output, size_t offset, const char *value)
{
    size_t length = strlen(value);
    output[offset++] = (unsigned char)(length >> 8);
    output[offset++] = (unsigned char)length;
    memcpy(output + offset, value, length);
    return offset + length;
}

int ed_mqtt_encode_connect(const ed_mqtt_config_t *config, unsigned char *output,
                           size_t capacity, size_t *written)
{
    char client_id[ED_ID_MAX + 3U];
    size_t remaining, encoded, pos = 0U, header_bytes = 2U;
    if (!output || !written || ed_mqtt_validate_config(config) != 0)
        return -1;
    snprintf(client_id, sizeof(client_id), "ed-%s", config->device_id);
    remaining = 10U + 2U + strlen(client_id);
    if (config->username[0] != '\0')
        remaining += 4U + strlen(config->username) + strlen(config->password);
    for (encoded = remaining; encoded >= 128U; encoded /= 128U)
        ++header_bytes;
    if (remaining + header_bytes > capacity)
        return -1;
    output[pos++] = 0x10U;
    encoded = remaining;
    do {
        unsigned char byte = (unsigned char)(encoded % 128U);
        encoded /= 128U;
        output[pos++] = (unsigned char)(byte | (encoded ? 0x80U : 0U));
    } while (encoded);
    pos = connect_string(output, pos, "MQTT");
    output[pos++] = 4U;
    output[pos++] = config->username[0] ? 0xc2U : 0x02U;
    output[pos++] = (unsigned char)(config->keepalive_s >> 8);
    output[pos++] = (unsigned char)config->keepalive_s;
    pos = connect_string(output, pos, client_id);
    if (config->username[0])
    {
        pos = connect_string(output, pos, config->username);
        pos = connect_string(output, pos, config->password);
    }
    *written = pos;
    return 0;
}

int ed_mqtt_build_topic(const char *site_id, const char *device_id,
                        const char *kind, char *output, size_t capacity)
{
    int length;
    if (!valid_id(site_id) || !valid_id(device_id) || !valid_id(kind) ||
        !output || capacity == 0U)
        return -1;
    length = snprintf(output, capacity, "edgedoctor/%s/%s/%s",
                      site_id, device_id, kind);
    return length < 0 || (size_t)length >= capacity ? -1 : 0;
}

int ed_mqtt_encode_event(const ed_event_t *event, char *output,
                         size_t capacity, size_t *written)
{
    char escaped_id[ED_EVENT_ID_MAX * 6U];
    char escaped_device[ED_ID_MAX * 6U];
    const char *condition;
    const char *ack;
    const char *severity;
    const char *source;
    int length;
    if (!event || !output || !written || capacity == 0U ||
        json_escape(event->event_id, escaped_id, sizeof(escaped_id)) < 0 ||
        json_escape(event->key.asset_id, escaped_device, sizeof(escaped_device)) < 0)
        return -1;
    condition = event->condition == ED_CONDITION_ACTIVE ? "ACTIVE" : "RECOVERED";
    ack = event->ack == ED_ACKNOWLEDGED ? "ACKNOWLEDGED" : "UNACKNOWLEDGED";
    severity = event->severity == ED_SEVERITY_ALARM ? "ALARM" : "WARNING";
    source = event->source == ED_SOURCE_LIVE_IMU ? "LIVE_IMU" : "SENSOR";
    length = snprintf(output, capacity,
                      "{\"schema_version\":%lu,\"device_id\":\"%s\",\"event_id\":\"%s\",\"revision\":%lu,\"source\":\"%s\",\"condition\":\"%s\",\"ack\":\"%s\",\"severity\":\"%s\",\"diagnosis\":\"UNKNOWN\",\"quality\":\"VALID\",\"peak_deviation\":%.6g}",
                      (unsigned long)event->schema_version, escaped_device,
                      escaped_id, (unsigned long)event->revision, source,
                      condition, ack, severity, (double)event->peak_deviation);
    if (length < 0 || (size_t)length >= capacity)
        return -1;
    *written = (size_t)length;
    return 0;
}

int ed_mqtt_enqueue_revision(ed_event_t *event)
{
    uint16_t index;
#ifdef EDGEDOCTOR_RT
    rt_base_t level = rt_hw_interrupt_disable();
#endif
    if (!event || event_count >= ED_MQTT_OUTBOX_MAX)
    {
        mqtt_state.rejected++;
#ifdef EDGEDOCTOR_RT
        rt_hw_interrupt_enable(level);
#endif
        return -1;
    }
    for (index = 0U; index < event_count; ++index)
    {
        if (strcmp(event_queue[index].event.event_id, event->event_id) == 0 &&
            event_queue[index].event.revision == event->revision)
        {
#ifdef EDGEDOCTOR_RT
            rt_hw_interrupt_enable(level);
#endif
            return 0;
        }
    }
    event_queue[event_count].event = *event;
    event_queue[event_count].published = 0U;
    event_count++;
    mqtt_state.queued = event_count + telemetry_count + command_result_count;
#ifdef EDGEDOCTOR_RT
    rt_hw_interrupt_enable(level);
#endif
    return 0;
}

int ed_mqtt_enqueue_telemetry(const ed_mqtt_telemetry_t *telemetry)
{
#ifdef EDGEDOCTOR_RT
    rt_base_t level;
#endif
    if (!telemetry || !valid_id(telemetry->device_id))
    {
        mqtt_state.rejected++;
        return -1;
    }
#ifdef EDGEDOCTOR_RT
    level = rt_hw_interrupt_disable();
#endif
    if (telemetry_count >= ED_MQTT_TELEMETRY_MAX)
    {
        /* Telemetry is periodic state, not an audit trail. When the outbox is
         * full the freshest window is the useful one, so retire the oldest
         * entry instead of refusing the new sample: refusing would pin the
         * queue to stale data and keep rejecting every later window. The loss
         * is still counted so the acceptance gate can see it. */
        if (telemetry_count > 1U)
            memmove(&telemetry_queue[0], &telemetry_queue[1],
                    (telemetry_count - 1U) * sizeof(telemetry_queue[0]));
        telemetry_count--;
        mqtt_state.rejected++;
    }
    telemetry_queue[telemetry_count++] = *telemetry;
    mqtt_state.queued = event_count + telemetry_count + command_result_count;
#ifdef EDGEDOCTOR_RT
    rt_hw_interrupt_enable(level);
#endif
    return 0;
}

int ed_mqtt_enqueue_command_result(const ed_command_result_t *result)
{
    uint16_t index;
#ifdef EDGEDOCTOR_RT
    rt_base_t level = rt_hw_interrupt_disable();
#endif
    if (!result || result->command_id[0] == '\0')
    {
#ifdef EDGEDOCTOR_RT
        rt_hw_interrupt_enable(level);
#endif
        return -1;
    }
    for (index = 0U; index < command_result_count; ++index)
    {
        if (strcmp(command_result_queue[index].command_id, result->command_id) == 0)
        {
            if (command_result_queue[index].status != ED_COMMAND_PENDING &&
                result->status == ED_COMMAND_PENDING)
            {
#ifdef EDGEDOCTOR_RT
                rt_hw_interrupt_enable(level);
#endif
                return 0;
            }
            command_result_queue[index] = *result;
#ifdef EDGEDOCTOR_RT
            rt_hw_interrupt_enable(level);
#endif
            return 0;
        }
    }
    if (command_result_count >= ED_MQTT_COMMAND_RESULT_MAX)
    {
#ifdef EDGEDOCTOR_RT
        rt_hw_interrupt_enable(level);
#endif
        return -1;
    }
    command_result_queue[command_result_count++] = *result;
    mqtt_state.queued = event_count + telemetry_count + command_result_count;
#ifdef EDGEDOCTOR_RT
    rt_hw_interrupt_enable(level);
#endif
    return 0;
}

int ed_mqtt_mark_receipt(const char *event_id, uint32_t revision)
{
    uint16_t index;
#ifdef EDGEDOCTOR_RT
    rt_base_t level = rt_hw_interrupt_disable();
#endif
    if (!event_id)
    {
#ifdef EDGEDOCTOR_RT
        rt_hw_interrupt_enable(level);
#endif
        return -1;
    }
    for (index = 0U; index < event_count; ++index)
    {
        if (strcmp(event_queue[index].event.event_id, event_id) == 0 &&
            event_queue[index].event.revision == revision)
        {
            if (index + 1U < event_count)
                memmove(&event_queue[index], &event_queue[index + 1U],
                        (event_count - index - 1U) * sizeof(event_queue[0]));
            event_count--;
            mqtt_state.queued = event_count + telemetry_count + command_result_count;
              mqtt_state.persisted++;
              ui_transport.event_receipts++;
              snprintf(ui_transport.last_receipt_event, sizeof(ui_transport.last_receipt_event), "%s", event_id);
              ui_transport.last_receipt_revision = revision;
#ifdef EDGEDOCTOR_RT
            rt_hw_interrupt_enable(level);
#endif
            return 0;
        }
    }
#ifdef EDGEDOCTOR_RT
    rt_hw_interrupt_enable(level);
#endif
    return -1;
}

void ed_mqtt_get_state(ed_mqtt_state_t *state)
{
    if (state)
#ifdef EDGEDOCTOR_RT
    {
        rt_base_t level = rt_hw_interrupt_disable();
        *state = mqtt_state;
        rt_hw_interrupt_enable(level);
    }
#else
        *state = mqtt_state;
#endif
}

void ed_mqtt_get_ui_state(ed_mqtt_ui_state_t *state)
{
    if (!state) return;
#ifdef EDGEDOCTOR_RT
    rt_base_t level = rt_hw_interrupt_disable();
#endif
    *state = ui_transport;
    memcpy(state->broker_host, transport_config.broker_host, sizeof(state->broker_host));
    state->broker_host[sizeof(state->broker_host) - 1U] = '\0';
    state->broker_port = transport_config.broker_port;
    state->pending_events = event_count;
#ifdef EDGEDOCTOR_RT
    rt_hw_interrupt_enable(level);
#endif
}

void ed_mqtt_mark_disconnected(void)
{
    uint16_t index;
#ifdef EDGEDOCTOR_RT
    rt_base_t level = rt_hw_interrupt_disable();
#endif
    mqtt_state.connected = 0U;
    mqtt_state.tls_valid = 0U;
    publish_fn = NULL;
    publish_user = NULL;
    for (index = 0U; index < event_count; ++index)
        event_queue[index].published = 0U;
#ifdef EDGEDOCTOR_RT
    rt_hw_interrupt_enable(level);
#endif
}

void ed_mqtt_set_transport_security(uint8_t tls_active, uint8_t peer_verified)
{
#ifdef EDGEDOCTOR_RT
    rt_base_t level = rt_hw_interrupt_disable();
#endif
    mqtt_state.tls_valid = (uint8_t)(tls_active != 0U && peer_verified != 0U);
#ifdef EDGEDOCTOR_RT
    rt_hw_interrupt_enable(level);
#endif
}

int ed_mqtt_bind_transport(const ed_mqtt_config_t *config,
                           ed_mqtt_publish_fn publish, void *user)
{
#ifdef EDGEDOCTOR_RT
    rt_base_t level;
#endif
    if (ed_mqtt_validate_config(config) != 0 || !publish)
        return -1;
#ifdef EDGEDOCTOR_RT
    level = rt_hw_interrupt_disable();
#endif
    transport_config = *config;
    publish_fn = publish;
    publish_user = user;
    mqtt_state.connected = 1U;
#ifdef EDGEDOCTOR_RT
    rt_hw_interrupt_enable(level);
#endif
    return 0;
}

int ed_mqtt_flush(uint16_t budget)
{
#ifdef EDGEDOCTOR_RT
    return ed_mqtt_flush_at(budget, ed_sensor_now_us() / 1000U);
#else
    return ed_mqtt_flush_at(budget, 0U);
#endif
}

static int mqtt_drain(ed_mqtt_receive_fn receive, void *user)
{
    unsigned index;
    for (index = 0U; index < 16U; ++index)
    {
        int result = receive(user);
        if (result <= 0)
            return result;
    }
    /* Defer sending while inbound traffic still fills the drain budget. */
    return 1;
}

int ed_mqtt_service(ed_mqtt_receive_fn receive, void *user, uint16_t budget)
{
    uint16_t index;
    int sent = 0;
    int result;
    if (!receive || budget == 0U)
        return -1;
    result = mqtt_drain(receive, user);
    if (result != 0)
        return result < 0 ? -1 : 0;
    for (index = 0U; index < budget; ++index)
    {
        result = ed_mqtt_flush(1U);
        if (result < 0)
            return result;
        sent += result;
        if (result == 0)
            break;
        result = mqtt_drain(receive, user);
        if (result != 0)
            return result < 0 ? -1 : sent;
    }
    return sent;
}

int ed_mqtt_flush_at(uint16_t budget, uint64_t now_ms)
{
    char topic[ED_ID_MAX * 3U + 24U];
    char payload[512];
    size_t length;
    uint16_t sent = 0U;
    int result;
    if (!publish_fn)
        return -1;
    if (budget == 0U)
        budget = ED_MQTT_OUTBOX_MAX;
    while (command_result_count > 0U && sent < budget)
    {
        /* Worst case: device 47 + command_id 47 + event_id 95 plus the JSON
         * literal, i.e. longer than the old 256 bytes. An over-long value made
         * snprintf report truncation, flush return -1, and the worker drop the
         * link in a loop without ever retiring the offending entry. */
        char result_payload[320];
        ed_command_result_t pending_result;
#ifdef EDGEDOCTOR_RT
        rt_base_t result_level = rt_hw_interrupt_disable();
#endif
        pending_result = command_result_queue[0];
#ifdef EDGEDOCTOR_RT
        rt_hw_interrupt_enable(result_level);
#endif
        size_t result_length = (size_t)snprintf(
            result_payload, sizeof(result_payload),
            "{\"device_id\":\"%s\",\"command_id\":\"%s\",\"event_id\":\"%s\",\"status\":\"%s\",\"current_revision\":%lu}",
            transport_config.device_id, pending_result.command_id,
            pending_result.event_id,
            pending_result.status == ED_COMMAND_OK ? "APPLIED" :
            (pending_result.status == ED_COMMAND_PENDING ? "PENDING" : "REJECTED"),
            (unsigned long)pending_result.current_revision);
        if (result_length >= sizeof(result_payload))
            return -1;
        if (ed_mqtt_build_topic(transport_config.site_id,
                                transport_config.device_id, "command-results",
                                topic, sizeof(topic)) != 0)
            return -1;
        result = publish_fn(publish_user, topic, result_payload, result_length);
        if (result < 0)
        {
            mqtt_state.connected = 0U;
            return -2;
        }
        mqtt_state.sent++;
        sent++;
        if (result > 0)
        {
#ifdef EDGEDOCTOR_RT
            result_level = rt_hw_interrupt_disable();
#endif
            if (command_result_count > 0U &&
                strcmp(command_result_queue[0].command_id, pending_result.command_id) == 0 &&
                command_result_queue[0].status == pending_result.status &&
                command_result_queue[0].current_revision == pending_result.current_revision)
            {
                if (command_result_count > 1U)
                    memmove(&command_result_queue[0], &command_result_queue[1],
                            (command_result_count - 1U) * sizeof(command_result_queue[0]));
                command_result_count--;
                mqtt_state.queued = event_count + telemetry_count + command_result_count;
            }
#ifdef EDGEDOCTOR_RT
            rt_hw_interrupt_enable(result_level);
#endif
        }
        else
        {
            /* A broker that has not acknowledged this result yet must not
             * consume the whole flush budget by retrying the same item. The
             * next worker pass will retry it with the normal backoff. */
            break;
        }
    }
    /* Send each event revision once until its server receipt arrives. This
     * keeps events ahead of telemetry without starving telemetry forever while
     * a broker receipt is delayed. */
    while (event_count > 0U && sent < budget)
    {
        uint16_t event_index;
        uint16_t scan;
        ed_event_t pending_event;
#ifdef EDGEDOCTOR_RT
        rt_base_t level = rt_hw_interrupt_disable();
#endif
        event_index = event_count;
        for (scan = 0U; scan < event_count; ++scan)
        {
            if (!event_queue[scan].published)
            {
                event_index = scan;
                break;
            }
            if (now_ms >= event_queue[scan].published_ms &&
                now_ms - event_queue[scan].published_ms >= ED_MQTT_RECEIPT_RETRY_MS &&
                (event_index == event_count ||
                 event_queue[scan].published_ms < event_queue[event_index].published_ms))
                event_index = scan;
        }
        if (event_index == event_count)
        {
#ifdef EDGEDOCTOR_RT
            rt_hw_interrupt_enable(level);
#endif
            break;
        }
        pending_event = event_queue[event_index].event;
#ifdef EDGEDOCTOR_RT
        rt_hw_interrupt_enable(level);
#endif
        if (ed_mqtt_build_topic(transport_config.site_id,
                                transport_config.device_id, "events",
                                topic, sizeof(topic)) != 0 ||
            ed_mqtt_encode_event(&pending_event, payload, sizeof(payload),
                                 &length) != 0)
            return -1;
        result = publish_fn(publish_user, topic, payload, length);
        if (result < 0)
        {
            mqtt_state.connected = 0U;
            return -2;
        }
        mqtt_state.sent++;
        sent++;
        if (result > 0)
        {
#ifdef EDGEDOCTOR_RT
            level = rt_hw_interrupt_disable();
#endif
            if (event_index < event_count &&
                event_queue[event_index].event.revision == pending_event.revision &&
                strcmp(event_queue[event_index].event.event_id, pending_event.event_id) == 0)
            {
                event_queue[event_index].published = 1U;
                event_queue[event_index].published_ms = now_ms;
            }
#ifdef EDGEDOCTOR_RT
            rt_hw_interrupt_enable(level);
#endif
        }
    }
    while (telemetry_count > 0U && sent < budget)
    {
        ed_mqtt_telemetry_t pending_telemetry;
#ifdef EDGEDOCTOR_RT
        rt_base_t telemetry_level = rt_hw_interrupt_disable();
#endif
        /* Snapshot the head slot before publishing: ed_mqtt_enqueue_telemetry
         * writes the same queue from the monitor thread. */
        pending_telemetry = telemetry_queue[0];
#ifdef EDGEDOCTOR_RT
        rt_hw_interrupt_enable(telemetry_level);
#endif
        if (ed_mqtt_build_topic(transport_config.site_id,
                                transport_config.device_id, "telemetry",
                                topic, sizeof(topic)) != 0)
            return -1;
        int formatted;
        if (pending_telemetry.deviation_valid && isfinite(pending_telemetry.anomaly_score))
        {
            formatted = snprintf(
                payload, sizeof(payload),
                "{\"device_id\":\"%s\",\"mono_ms\":%lu,\"rms_vector_g\":%.6g,\"anomaly_score\":%.6g,\"alarm\":%s,\"idle\":%s}",
                pending_telemetry.device_id,
                (unsigned long)(uint32_t)pending_telemetry.mono_ms,
                (double)pending_telemetry.rms_vector_g,
                (double)pending_telemetry.anomaly_score,
                pending_telemetry.alarm ? "true" : "false",
                pending_telemetry.idle ? "true" : "false");
        }
        else
        {
            /* An idle window (or one without a baseline) has no deviation to
             * report. Omit anomaly_score instead of writing 0: 0 is a real
             * measurement, and a trend or a rule that reads it would turn "no
             * conclusion" into "concluded normal". */
            formatted = snprintf(
                payload, sizeof(payload),
                "{\"device_id\":\"%s\",\"mono_ms\":%lu,\"rms_vector_g\":%.6g,\"alarm\":%s,\"idle\":%s}",
                pending_telemetry.device_id,
                (unsigned long)(uint32_t)pending_telemetry.mono_ms,
                (double)pending_telemetry.rms_vector_g,
                pending_telemetry.alarm ? "true" : "false",
                pending_telemetry.idle ? "true" : "false");
        }
        if (formatted < 0 || (size_t)formatted >= sizeof(payload))
            return -1;
        length = (size_t)formatted;
        result = publish_fn(publish_user, topic, payload, length);
        if (result < 0)
        {
            mqtt_state.connected = 0U;
            return -2;
        }
        mqtt_state.sent++;
        sent++;
        if (result > 0)
        {
#ifdef EDGEDOCTOR_RT
            telemetry_level = rt_hw_interrupt_disable();
#endif
            if (telemetry_count > 1U)
                memmove(&telemetry_queue[0], &telemetry_queue[1],
                        (telemetry_count - 1U) * sizeof(telemetry_queue[0]));
            telemetry_count--;
            mqtt_state.persisted++;
            mqtt_state.queued = event_count + telemetry_count + command_result_count;
#ifdef EDGEDOCTOR_RT
            rt_hw_interrupt_enable(telemetry_level);
#endif
        }
    }
    return (int)sent;
}

#ifdef EDGEDOCTOR_RT
static int command_string(const char *json, const char *key,
                          char *output, size_t capacity)
{
    char needle[48];
    const char *cursor;
    const char *start;
    size_t used = 0U;
    int length;
    if (!json || !key || !output || capacity == 0U)
        return -1;
    length = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (length < 0 || (size_t)length >= sizeof(needle))
        return -1;
    cursor = strstr(json, needle);
    if (!cursor)
        return -1;
    cursor += length;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
        ++cursor;
    if (*cursor++ != ':')
        return -1;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
        ++cursor;
    if (*cursor++ != '"')
        return -1;
    start = cursor;
    while (*cursor != '\0' && *cursor != '"')
    {
        unsigned char value;
        if (*cursor == '\\')
        {
            ++cursor;
            if (*cursor == '\0')
                return -1;
            switch (*cursor++)
            {
            case '"': value = '"'; break;
            case '\\': value = '\\'; break;
            case '/': value = '/'; break;
            case 'b': value = '\b'; break;
            case 'f': value = '\f'; break;
            case 'n': value = '\n'; break;
            case 'r': value = '\r'; break;
            case 't': value = '\t'; break;
            default: return -1;
            }
        }
        else
        {
            value = (unsigned char)*cursor++;
            if (value < 0x20U)
                return -1;
        }
        if (used + 1U >= capacity)
            return -1;
        output[used++] = (char)value;
    }
    if (*cursor != '"' || cursor == start)
        return -1;
    output[used] = '\0';
    return 0;
}

static int command_u32(const char *json, const char *key, uint32_t *value)
{
    char needle[48];
    const char *cursor;
    uint64_t parsed = 0U;
    int length;
    if (!json || !key || !value)
        return -1;
    length = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (length < 0 || (size_t)length >= sizeof(needle))
        return -1;
    cursor = strstr(json, needle);
    if (!cursor)
        return -1;
    cursor += length;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
        ++cursor;
    if (*cursor++ != ':')
        return -1;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
        ++cursor;
    if (*cursor == '-' || *cursor == '+')
        return -1;
    if (*cursor < '0' || *cursor > '9')
        return -1;
    while (*cursor >= '0' && *cursor <= '9')
    {
        parsed = parsed * 10U + (uint64_t)(*cursor - '0');
        if (parsed > UINT32_MAX)
            return -1;
        ++cursor;
    }
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
        ++cursor;
    if (*cursor != ',' && *cursor != '}' && *cursor != '\0')
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static void handle_command(const char *payload)
{
    char action[32] = {0};
    char event_id[ED_EVENT_ID_MAX] = {0};
    char command_id[ED_ID_MAX] = {0};
    char challenge[40] = {0};
    ed_command_result_t result = {0};
    uint32_t revision = 0U;
    int status = -1;
    int ack_action = 0;
    int revision_present;
    if (command_string(payload, "action", action, sizeof(action)) != 0 ||
        command_string(payload, "command_id", command_id, sizeof(command_id)) != 0)
        return;
    /* These two reach the evidence file path and the chunk topic verbatim, so
     * a malformed value is dropped rather than propagated. */
    if (command_string(payload, "event_id", event_id, sizeof(event_id)) == 0 &&
        !valid_token(event_id, sizeof(event_id)))
        event_id[0] = '\0';
    if (command_string(payload, "challenge", challenge, sizeof(challenge)) == 0 &&
        !valid_token(challenge, sizeof(challenge)))
        challenge[0] = '\0';
    revision_present = command_u32(payload, "expected_revision", &revision) == 0;
    if (strcmp(action, "ack_event") == 0 && event_id[0] != '\0' &&
        revision_present)
    {
        ack_action = 1;
        status = edgedoctor_app_ack_event(command_id, event_id, revision,
                                          ed_sensor_now_us());
    }
    else if (strcmp(action, "request_evidence") == 0 && event_id[0] != '\0' &&
             challenge[0] != '\0')
        status = ed_evidence_capture_request(event_id, challenge);
    rt_snprintf(result.command_id, sizeof(result.command_id), "%s", command_id);
    rt_snprintf(result.event_id, sizeof(result.event_id), "%s", event_id);
    result.current_revision = revision;
    result.status = status == 0 ? (ack_action ? ED_COMMAND_PENDING : ED_COMMAND_OK) : ED_COMMAND_INVALID;
    /* Never publish from the receive handler: waiting for PUBACK here would
     * accumulate the remaining command burst instead of draining it. */
    (void)ed_mqtt_enqueue_command_result(&result);
}

static void handle_event_receipt(const char *payload)
{
    char event_id[ED_EVENT_ID_MAX] = {0};
    char status[24] = {0};
    uint32_t revision = 0U;
    if (command_string(payload, "event_id", event_id, sizeof(event_id)) != 0 ||
        command_string(payload, "status", status, sizeof(status)) != 0 ||
        command_u32(payload, "revision", &revision) != 0)
        return;
    if (strcmp(status, "persisted") == 0)
        (void)ed_mqtt_mark_receipt(event_id, revision);
}

static uint16_t mqtt_evidence_index;
static uint64_t mqtt_evidence_retry_ms;

static void handle_evidence_receipt(const char *payload)
{
    char id[ED_EVENT_ID_MAX] = {0};
    char token[40] = {0};
    char status[24] = {0};
    uint32_t bytes = 0U;
    if (command_string(payload, "event_id", id, sizeof(id)) != 0 ||
        command_string(payload, "challenge", token, sizeof(token)) != 0 ||
        command_string(payload, "status", status, sizeof(status)) != 0 ||
        command_u32(payload, "bytes", &bytes) != 0)
        return;
    if (strcmp(status, "stored") == 0 && ed_evidence_capture_accept_receipt(id, token, bytes) == 0)
    {
        mqtt_evidence_index = 0U;
        mqtt_evidence_retry_ms = 0U;
    }
}

static int receive_message(void *user)
{
    char topic[160];
    char payload[512];
    int result;
    (void)user;
    /* Leave room for the result before acknowledging another command. */
    if (command_result_count >= ED_MQTT_COMMAND_RESULT_MAX)
        return 0;
    result = ed_mqtt_socket_poll(topic, sizeof(topic), payload, sizeof(payload));
    if (result > 0)
    {
        if (strstr(topic, "/evidence_receipts") != RT_NULL)
            handle_evidence_receipt(payload);
        else if (strstr(topic, "/event_receipts") != RT_NULL)
            handle_event_receipt(payload);
        else
            handle_command(payload);
    }
    return result;
}

static int upload_evidence(void)
{
    unsigned budget;
    uint16_t total;
    const char *event_id;
    const uint8_t *data;
    size_t length;
    /* "edgedoctor/" + site 47 + "/" + device 47 + "/evidence/" + event 95 +
     * "/chunks" needs 219 bytes, so 192 silently clipped the topic. */
    char topic[256];
    uint8_t packet[512];
    if (ed_evidence_capture_get_chunk(0U, &total, &event_id, &data, &length) != 0)
    {
        mqtt_evidence_index = 0U;
        mqtt_evidence_retry_ms = 0U;
        return 0;
    }
    if (ed_sensor_now_us() / 1000U < mqtt_evidence_retry_ms)
        return 0;
    for (budget = 0U; mqtt_evidence_index < total && budget < 4U; ++budget)
    {
        int drain;
        drain = mqtt_drain(receive_message, RT_NULL);
        if (drain > 0)
            return 0;
        if (drain < 0)
        {
            mqtt_evidence_index = 0U;
            return -1;
        }
        /* A matching stored receipt can retire the capture during drain. */
        if (ed_evidence_capture_get_chunk(mqtt_evidence_index, &total, &event_id,
                                          &data, &length) != 0)
            return 0;
        if (length > 508U)
            return -1;
        packet[0] = (uint8_t)(total >> 8);
        packet[1] = (uint8_t)total;
        packet[2] = (uint8_t)(mqtt_evidence_index >> 8);
        packet[3] = (uint8_t)mqtt_evidence_index;
        memcpy(&packet[4], data, length);
        rt_snprintf(topic, sizeof(topic), "edgedoctor/%s/%s/evidence/%s/chunks",
                    shell_config.site_id, shell_config.device_id, event_id);
        if (ed_mqtt_socket_publish(RT_NULL, topic, (const char *)packet,
                                    length + 4U) < 0)
        {
            mqtt_evidence_index = 0U;
            return -1;
        }
        ++mqtt_evidence_index;
    }
    if (mqtt_evidence_index < total)
        return 0;
    /* Broker PUBACK is not a storage receipt. Retain and retry the complete
     * capture until the service reports matching event/challenge/byte count. */
    mqtt_evidence_index = 0U;
    mqtt_evidence_retry_ms = ed_sensor_now_us() / 1000U + 10000U;
    return 0;
}

static void mqtt_worker(void *parameter)
{
    (void)parameter;
    while (mqtt_thread_running)
    {
        ed_mqtt_state_t state;
        ed_mqtt_get_state(&state);
        if (!state.connected)
        {
            uint64_t now_ms = ed_sensor_now_us() / 1000U;
            if (now_ms >= mqtt_next_retry_ms)
            {
                ed_mqtt_socket_disconnect();
                if (ed_mqtt_socket_connect(&shell_config) == 0)
                {
                    (void)ed_mqtt_bind_transport(&shell_config,
                                                 ed_mqtt_socket_publish, RT_NULL);
                    mqtt_backoff_ms = 1000U;
                    mqtt_next_retry_ms = now_ms;
                }
                else
                    mqtt_schedule_retry();
            }
        }
        else
        {
            if (ed_mqtt_socket_keepalive(&shell_config) != 0)
            {
                ed_mqtt_socket_disconnect();
                mqtt_schedule_retry();
                continue;
            }
            if (ed_mqtt_service(receive_message, RT_NULL, 8U) < 0)
            {
                ed_mqtt_socket_disconnect();
                mqtt_schedule_retry();
            }
            else
            {
                (void)ed_evidence_capture_poll();
                if (upload_evidence() < 0)
                {
                    ed_mqtt_socket_disconnect();
                    mqtt_schedule_retry();
                }
            }
        }
        rt_thread_mdelay(100U);
    }
    ed_mqtt_socket_disconnect();
    mqtt_thread = RT_NULL;
}

static int mqtt_worker_start(void)
{
    if (mqtt_thread_running)
        return 0;
    if (shell_config.broker_host[0] == '\0' && shell_config_load() != 0)
        return -1;
    if (ed_mqtt_validate_config(&shell_config) != 0)
        return -1;
    mqtt_thread_running = RT_TRUE;
    /* The worker nests ed_mqtt_socket_poll (packet[1024]), handle_command
     * and ed_mqtt_flush through ed_mqtt_socket_publish (incoming[1024] plus
     * packet[640]). Cortex-M also services interrupts on the running thread
     * stack, so the previous 4 KiB overflowed as soon as the broker became
     * reachable and corrupted unrelated heap objects. */
    mqtt_thread = rt_thread_create("edmqtt", mqtt_worker, RT_NULL,
                                   ED_MQTT_THREAD_STACK,
                                   RT_THREAD_PRIORITY_MAX / 2, 10);
    if (!mqtt_thread)
    {
        mqtt_thread_running = RT_FALSE;
        return -1;
    }
    rt_thread_startup(mqtt_thread);
    return 0;
}

/* Provisioned boards must report without an operator typing `edmqtt start`
 * after every power cycle. An unprovisioned board simply skips this and stays
 * diagnosable over the shell. */
static int ed_mqtt_boot_start(void)
{
    if (mqtt_worker_start() != 0)
    {
        rt_kprintf("edmqtt: no provisioned broker; run 'edmqtt config' to "
                   "enable automatic reporting\n");
        return -RT_ERROR;
    }
    rt_kprintf("edmqtt: worker started for %s:%u as site=%s device=%s\n",
               shell_config.broker_host, (unsigned)shell_config.broker_port,
               shell_config.site_id, shell_config.device_id);
    return RT_EOK;
}
INIT_APP_EXPORT(ed_mqtt_boot_start);

static int mqtt_worker_stop(void)
{
    uint32_t waited = 0U;
    mqtt_thread_running = RT_FALSE;
    while (mqtt_thread != RT_NULL && waited < 2000U)
    {
        rt_thread_mdelay(10U);
        waited += 10U;
    }
    if (mqtt_thread != RT_NULL)
        return -2;
    return 0;
}

static int edmqtt(int argc, char **argv)
{
    ed_mqtt_state_t state;
    if (shell_config.broker_host[0] == '\0')
        (void)shell_config_load();
    if (argc >= 6 && strcmp(argv[1], "config") == 0)
    {
        if (mqtt_thread_running || mqtt_thread != RT_NULL)
            return -2;
        memset(&shell_config, 0, sizeof(shell_config));
        snprintf(shell_config.site_id, sizeof(shell_config.site_id), "%s", argv[2]);
        snprintf(shell_config.device_id, sizeof(shell_config.device_id), "%s", argv[3]);
        snprintf(shell_config.broker_host, sizeof(shell_config.broker_host), "%s", argv[4]);
        shell_config.broker_port = (uint16_t)strtoul(argv[5], RT_NULL, 10);
        shell_config.keepalive_s = 30U;
        if (ed_mqtt_validate_config(&shell_config) != 0 || shell_config_save() != 0)
            return -1;
        rt_kprintf("edmqtt: saved site=%s device=%s broker=%s:%u\n",
                   shell_config.site_id, shell_config.device_id,
                   shell_config.broker_host, (unsigned)shell_config.broker_port);
        rt_kprintf("edmqtt: the device identity is adopted at boot; "
                   "reboot to apply it\n");
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "auth") == 0)
    {
        if (mqtt_thread_running || mqtt_thread != RT_NULL)
            return -2;
        if (strlen(argv[2]) >= sizeof(shell_config.username) ||
            strlen(argv[3]) >= sizeof(shell_config.password))
            return -1;
        strcpy(shell_config.username, argv[2]);
        strcpy(shell_config.password, argv[3]);
        if (ed_mqtt_validate_config(&shell_config) != 0 || shell_config_save() != 0)
            return -1;
        rt_kprintf("edmqtt: credentials saved\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "start") == 0)
        return mqtt_worker_start();
    if (argc >= 2 && strcmp(argv[1], "stop") == 0)
        return mqtt_worker_stop();
    if (argc >= 2 && strcmp(argv[1], "info") == 0)
    {
        ed_mqtt_get_state(&state);
        rt_kprintf("edmqtt: connected=%u queued=%u sent=%u persisted=%u rejected=%u inbox_dropped=%u\n",
                   state.connected, state.queued, state.sent,
                   state.persisted, state.rejected,
                   (unsigned)ed_mqtt_socket_inbox_dropped());
        return 0;
    }
    rt_kprintf("usage: edmqtt config <site> <device> <broker_host> <port> | edmqtt auth <user> <password> | edmqtt start|stop|info\n");
    return -1;
}
MSH_CMD_EXPORT(edmqtt, EdgeDoctor MQTT configuration and state);
#endif
