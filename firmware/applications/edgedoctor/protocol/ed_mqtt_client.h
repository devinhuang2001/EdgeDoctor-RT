#ifndef ED_MQTT_CLIENT_H
#define ED_MQTT_CLIENT_H
#include "ed_monitor_types.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ED_MQTT_OUTBOX_MAX 128u
typedef struct { char site_id[ED_ID_MAX]; char device_id[ED_ID_MAX]; char broker_host[ED_ID_MAX]; uint16_t broker_port; uint16_t keepalive_s; } ed_mqtt_config_t;
typedef struct { uint32_t queued, sent, persisted, rejected; uint8_t connected, tls_valid, time_ready; } ed_mqtt_state_t;
typedef struct { char device_id[ED_ID_MAX]; uint64_t mono_ms; float rms_vector_g; float anomaly_score; uint8_t alarm; } ed_mqtt_telemetry_t;
typedef int (*ed_mqtt_publish_fn)(void *user, const char *topic, const char *payload, size_t length);
int ed_mqtt_validate_config(const ed_mqtt_config_t *config);
int ed_mqtt_build_topic(const char *site_id, const char *device_id, const char *kind, char *out, size_t capacity);
int ed_mqtt_encode_event(const ed_event_t *event, char *out, size_t capacity, size_t *written);
int ed_mqtt_enqueue_revision(ed_event_t *event);
int ed_mqtt_enqueue_telemetry(const ed_mqtt_telemetry_t *telemetry);
int ed_mqtt_enqueue_command_result(const ed_command_result_t *result);
int ed_mqtt_mark_receipt(const char *event_id, uint32_t revision);
int ed_mqtt_bind_transport(const ed_mqtt_config_t *config, ed_mqtt_publish_fn publish, void *user);
int ed_mqtt_flush(uint16_t budget);
void ed_mqtt_get_state(ed_mqtt_state_t *state);
void ed_mqtt_mark_disconnected(void);
void ed_mqtt_set_transport_security(uint8_t tls_active, uint8_t peer_verified);
#ifdef EDGEDOCTOR_RT
int ed_mqtt_socket_connect(const ed_mqtt_config_t *config);
int ed_mqtt_socket_disconnect(void);
int ed_mqtt_socket_publish(void *user, const char *topic, const char *payload, size_t length);
int ed_mqtt_socket_subscribe_commands(const ed_mqtt_config_t *config);
int ed_mqtt_socket_poll(char *topic, size_t topic_capacity, char *payload, size_t payload_capacity);
int ed_mqtt_socket_keepalive(const ed_mqtt_config_t *config);
#endif
#ifdef __cplusplus
}
#endif
#endif
