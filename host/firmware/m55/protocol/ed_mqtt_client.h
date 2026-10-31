#ifndef ED_MQTT_CLIENT_H
#define ED_MQTT_CLIENT_H
#include "ed_monitor_types.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ED_MQTT_OUTBOX_MAX 128u
#define ED_MQTT_USERNAME_MAX 64U
#define ED_MQTT_PASSWORD_MAX 128U
typedef struct {
    char site_id[ED_ID_MAX], device_id[ED_ID_MAX], broker_host[ED_ID_MAX];
    uint16_t broker_port, keepalive_s;
    /* Appended to retain compatibility with previously provisioned files. */
    char username[ED_MQTT_USERNAME_MAX], password[ED_MQTT_PASSWORD_MAX];
} ed_mqtt_config_t;
typedef struct { uint32_t queued, sent, persisted, rejected; uint8_t connected, tls_valid, time_ready; } ed_mqtt_state_t;
/* anomaly_score is only meaningful when deviation_valid is set: an idle window
 * and a window without a baseline have no deviation at all, and reporting 0.0
 * for them would read downstream as "deviation measured, and it was zero". */
typedef struct { char device_id[ED_ID_MAX]; uint64_t mono_ms; float rms_vector_g; float anomaly_score; uint8_t alarm; uint8_t idle; uint8_t deviation_valid; } ed_mqtt_telemetry_t;
typedef int (*ed_mqtt_publish_fn)(void *user, const char *topic, const char *payload, size_t length);
int ed_mqtt_validate_config(const ed_mqtt_config_t *config);
int ed_mqtt_decode_config(const void *data, size_t length, ed_mqtt_config_t *config);
int ed_mqtt_encode_connect(const ed_mqtt_config_t *config, unsigned char *output,
                           size_t capacity, size_t *written);
int ed_mqtt_build_topic(const char *site_id, const char *device_id, const char *kind, char *out, size_t capacity);
int ed_mqtt_encode_event(const ed_event_t *event, char *out, size_t capacity, size_t *written);
int ed_mqtt_enqueue_revision(ed_event_t *event);
int ed_mqtt_enqueue_telemetry(const ed_mqtt_telemetry_t *telemetry);
int ed_mqtt_enqueue_command_result(const ed_command_result_t *result);
int ed_mqtt_mark_receipt(const char *event_id, uint32_t revision);
/* Single source of truth for the device identity. The same value must appear
 * in the MQTT topic and in every payload, otherwise the server rejects the
 * message in _topic_allowed and never sends a receipt. Copies the persisted
 * identity and returns 0, or returns -1 when none is configured. */
int ed_mqtt_configured_device(char *output, size_t capacity);
int ed_mqtt_bind_transport(const ed_mqtt_config_t *config, ed_mqtt_publish_fn publish, void *user);
int ed_mqtt_flush(uint16_t budget);
/* receive handles one incoming message and returns 1, 0 (quiet), or -1.
 * Drain receipts before and after every single publish, with bounded work. */
typedef int (*ed_mqtt_receive_fn)(void *user);
int ed_mqtt_service(ed_mqtt_receive_fn receive, void *user, uint16_t budget);
/* Monotonic milliseconds; permits receipt retries without reconnecting. */
int ed_mqtt_flush_at(uint16_t budget, uint64_t now_ms);
void ed_mqtt_get_state(ed_mqtt_state_t *state);
/* Observation only: event receipts are separate from telemetry queue removal. */
typedef struct {
    char broker_host[ED_ID_MAX], last_receipt_event[ED_EVENT_ID_MAX];
    uint16_t broker_port, pending_events;
    uint32_t last_receipt_revision, event_receipts;
} ed_mqtt_ui_state_t;
void ed_mqtt_get_ui_state(ed_mqtt_ui_state_t *state);
void ed_mqtt_mark_disconnected(void);
void ed_mqtt_set_transport_security(uint8_t tls_active, uint8_t peer_verified);
#ifdef EDGEDOCTOR_RT
int ed_mqtt_socket_connect(const ed_mqtt_config_t *config);
int ed_mqtt_socket_disconnect(void);
int ed_mqtt_socket_publish(void *user, const char *topic, const char *payload, size_t length);
int ed_mqtt_socket_subscribe_commands(const ed_mqtt_config_t *config);
int ed_mqtt_socket_poll(char *topic, size_t topic_capacity, char *payload, size_t payload_capacity);
int ed_mqtt_socket_keepalive(const ed_mqtt_config_t *config);
/* Inbound frames that could not be queued because the receive inbox was full.
 * Non-zero means inbound QoS 1 messages were deliberately left unacknowledged
 * so the broker redelivers them; the worker is not draining fast enough. */
uint32_t ed_mqtt_socket_inbox_dropped(void);
#endif
#ifdef __cplusplus
}
#endif
#endif
