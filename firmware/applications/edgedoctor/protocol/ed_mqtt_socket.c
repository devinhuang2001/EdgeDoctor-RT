#include "ed_mqtt_client.h"

#ifdef EDGEDOCTOR_RT
#include "ed_sensor.h"
#include <rtthread.h>
#include <finsh.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <string.h>
#include <stdlib.h>
#include "tls_client.h"

static int mqtt_fd = -1;
static MbedTLSSession *tls_session;
static uint16_t mqtt_packet_id = 1u;

typedef struct {
    uint8_t ready;
    char topic[160];
    char payload[512];
} mqtt_pending_publish_t;

static mqtt_pending_publish_t pending_publish;
static uint64_t last_activity_ms;
static uint64_t ping_sent_ms;

static int send_all(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    while (len != 0u) {
        int n = tls_session ? mbedtls_client_write(tls_session, p, len) : send(mqtt_fd, p, len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
        last_activity_ms = ed_sensor_now_us() / 1000U;
    }
    return 0;
}

static int recv_all(void *data, size_t len)
{
    unsigned char *p = (unsigned char *)data;
    while (len != 0u) {
        int n = tls_session ? mbedtls_client_read(tls_session, p, len) : recv(mqtt_fd, p, len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Read one complete MQTT frame. The caller owns packet and decides how to
 * dispatch its fixed-size payload. */
static int read_packet(unsigned char *header, unsigned char *packet,
                       size_t capacity, size_t *remaining)
{
    unsigned char byte;
    size_t multiplier = 1U;
    size_t count = 0U;
    int n;
    if (!header || !packet || !remaining)
        return -1;
    n = tls_session ? mbedtls_client_read(tls_session, header, 1U)
                    : recv(mqtt_fd, header, 1U, 0);
    if (n == 0)
        return -1;
    if (n < 0)
        return 0;
    last_activity_ms = ed_sensor_now_us() / 1000U;
    *remaining = 0U;
    do {
        n = tls_session ? mbedtls_client_read(tls_session, &byte, 1U)
                        : recv(mqtt_fd, &byte, 1U, 0);
        if (n <= 0 || count >= 4U)
            return -1;
        *remaining += (size_t)(byte & 0x7fU) * multiplier;
        multiplier *= 128U;
        ++count;
    } while ((byte & 0x80U) != 0U);
    if (*remaining > capacity)
        return -1;
    return *remaining == 0U || recv_all(packet, *remaining) == 0 ? 1 : -1;
}

static int decode_publish(const unsigned char *packet, size_t remaining,
                          unsigned char header, char *topic,
                          size_t topic_capacity, char *payload,
                          size_t payload_capacity)
{
    size_t pos = 0U;
    size_t topic_len;
    size_t payload_len;
    if ((header & 0xf0U) != 0x30U || remaining < 2U || !topic || !payload)
        return -1;
    topic_len = ((size_t)packet[0] << 8) | packet[1];
    pos = 2U;
    if (topic_len == 0U || topic_len >= topic_capacity || pos + topic_len > remaining)
        return -1;
    memcpy(topic, packet + pos, topic_len);
    topic[topic_len] = '\0';
    pos += topic_len;
    if ((header & 0x06U) != 0U)
    {
        if (pos + 2U > remaining)
            return -1;
        pos += 2U;
    }
    payload_len = remaining - pos;
    if (payload_len >= payload_capacity)
        return -1;
    memcpy(payload, packet + pos, payload_len);
    payload[payload_len] = '\0';
    return 0;
}

static size_t put_remaining(unsigned char *out, size_t value)
{
    size_t n = 0u;
    do {
        unsigned char byte = (unsigned char)(value % 128u);
        value /= 128u;
        if (value != 0u) byte |= 0x80u;
        out[n++] = byte;
    } while (value != 0u && n < 4u);
    return n;
}

static int mqtt_connect_packet(const ed_mqtt_config_t *c)
{
    unsigned char packet[160];
    size_t pos = 0u, rem_pos, rem;
    size_t client_len = strlen(c->device_id) + 3u;
    if (client_len >= ED_ID_MAX + 3u) return -1;
    packet[pos++] = 0x10u;
    rem_pos = pos++;
    packet[pos++] = 0u; packet[pos++] = 4u;
    memcpy(&packet[pos], "MQTT", 4u); pos += 4u;
    packet[pos++] = 4u; packet[pos++] = 2u;
    packet[pos++] = (unsigned char)(c->keepalive_s >> 8);
    packet[pos++] = (unsigned char)c->keepalive_s;
    packet[pos++] = (unsigned char)(client_len >> 8);
    packet[pos++] = (unsigned char)client_len;
    memcpy(&packet[pos], "ed-", 3u); pos += 3u;
    memcpy(&packet[pos], c->device_id, client_len - 3u); pos += client_len - 3u;
    rem = pos - 2u;
    /* CONNECT's remaining-length field is at packet[1]; this configuration
       always fits one byte, but retain the helper for protocol correctness. */
    if (rem >= 128u) return -1;
    packet[rem_pos] = (unsigned char)rem;
    if (send_all(packet, pos) != 0) return -1;
    {
        unsigned char ack[4];
        if (recv_all(ack, sizeof ack) != 0 || ack[0] != 0x20u || ack[1] != 2u || ack[3] != 0u) return -1;
    }
    return 0;
}

static int mqtt_subscribe_packet(const ed_mqtt_config_t *c)
{
    unsigned char packet[384], body[320], ack[6];
    char topic[ED_ID_MAX * 2u + 32u];
    char receipt_topic[ED_ID_MAX * 2u + 40u];
    size_t pos = 0u, body_pos = 0u, topic_len, receipt_len, remaining_length;
    static uint16_t subscribe_id = 0x4000u;
    if (!c) return -1;
    rt_snprintf(topic, sizeof topic, "edgedoctor/%s/%s/commands", c->site_id, c->device_id);
    rt_snprintf(receipt_topic, sizeof receipt_topic,
                "edgedoctor/%s/%s/event_receipts", c->site_id, c->device_id);
    topic_len = strlen(topic);
    receipt_len = strlen(receipt_topic);
    if (topic_len > 150u || receipt_len > 150u) return -1;
    body[body_pos++] = (unsigned char)(subscribe_id >> 8);
    body[body_pos++] = (unsigned char)subscribe_id;
    body[body_pos++] = (unsigned char)(topic_len >> 8);
    body[body_pos++] = (unsigned char)topic_len;
    memcpy(&body[body_pos], topic, topic_len); body_pos += topic_len;
    body[body_pos++] = 0u;
    body[body_pos++] = (unsigned char)(receipt_len >> 8);
    body[body_pos++] = (unsigned char)receipt_len;
    memcpy(&body[body_pos], receipt_topic, receipt_len); body_pos += receipt_len;
    body[body_pos++] = 0u;
    packet[pos++] = 0x82u;
    remaining_length = put_remaining(&packet[pos], body_pos);
    pos += remaining_length;
    memcpy(&packet[pos], body, body_pos);
    pos += body_pos;
    if (send_all(packet, pos) != 0) return -1;
    if (recv_all(ack, sizeof ack) != 0 || ack[0] != 0x90u || ack[1] != 4u ||
        ack[2] != (unsigned char)(subscribe_id >> 8) ||
        ack[3] != (unsigned char)subscribe_id || ack[4] == 0x80u ||
        ack[5] == 0x80u) return -1;
    subscribe_id = (uint16_t)(subscribe_id == 0xffffu ? 0x4000u : subscribe_id + 1u);
    return 0;
}

int ed_mqtt_socket_connect(const ed_mqtt_config_t *config)
{
    struct addrinfo hints, *result = RT_NULL, *it;
    char port[8];
    int fd = -1, rc = -1;
    struct timeval timeout;
    timeout.tv_sec = 5; timeout.tv_usec = 0;
    if (!config || ed_mqtt_validate_config(config) != 0) return -1;
    if (config->broker_port == 8883u) {
        tls_session = (MbedTLSSession *)rt_calloc(1, sizeof *tls_session);
        if (!tls_session) return -2;
        tls_session->host = (char *)rt_malloc(ED_ID_MAX);
        tls_session->port = (char *)rt_malloc(8u);
        tls_session->buffer = (unsigned char *)rt_malloc(1024u);
        if (!tls_session->host || !tls_session->port || !tls_session->buffer) { mbedtls_client_close(tls_session); tls_session = RT_NULL; return -2; }
        rt_snprintf(tls_session->host, ED_ID_MAX, "%s", config->broker_host);
        rt_snprintf(tls_session->port, 8u, "%u", (unsigned)config->broker_port);
        tls_session->buffer_len = 1024u;
        if (mbedtls_client_init(tls_session, (void *)"edgedoctor", 10u) != 0 ||
            mbedtls_client_context(tls_session) != 0 || mbedtls_client_connect(tls_session) != 0) {
            mbedtls_client_close(tls_session); tls_session = RT_NULL; return -3;
        }
        if (mqtt_connect_packet(config) != 0 || mqtt_subscribe_packet(config) != 0) { mbedtls_client_close(tls_session); tls_session = RT_NULL; return -3; }
        ed_mqtt_set_transport_security(1U, 1U);
        last_activity_ms = ed_sensor_now_us() / 1000U;
        return 0;
    }
    rt_snprintf(port, sizeof port, "%u", (unsigned)config->broker_port);
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(config->broker_host, port, &hints, &result) != 0) return -1;
    for (it = result; it != RT_NULL; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) { mqtt_fd = fd; rc = mqtt_connect_packet(config); if (rc == 0) break; }
        closesocket(fd); fd = -1;
    }
    freeaddrinfo(result);
    if (rc != 0 || mqtt_subscribe_packet(config) != 0) { if (mqtt_fd >= 0) { closesocket(mqtt_fd); mqtt_fd = -1; } return -1; }
    ed_mqtt_set_transport_security(0U, 0U);
    {
        struct timeval idle_timeout = {0, 100000};
        (void)setsockopt(mqtt_fd, SOL_SOCKET, SO_RCVTIMEO,
                         &idle_timeout, sizeof(idle_timeout));
    }
    last_activity_ms = ed_sensor_now_us() / 1000U;
    return 0;
}

int ed_mqtt_socket_disconnect(void)
{
    if (tls_session) { mbedtls_client_close(tls_session); tls_session = RT_NULL; }
    if (mqtt_fd >= 0) { closesocket(mqtt_fd); mqtt_fd = -1; }
    memset(&pending_publish, 0, sizeof(pending_publish));
    last_activity_ms = 0U;
    ping_sent_ms = 0U;
    ed_mqtt_mark_disconnected();
    return 0;
}

int ed_mqtt_socket_subscribe_commands(const ed_mqtt_config_t *config)
{
    return mqtt_subscribe_packet(config);
}

int ed_mqtt_socket_poll(char *topic, size_t topic_capacity, char *payload, size_t payload_capacity)
{
    unsigned char header, packet[1024];
    size_t remaining;
    if (!topic || !payload || topic_capacity == 0u || payload_capacity == 0u) return -1;
    if (pending_publish.ready)
    {
        if (strlen(pending_publish.topic) >= topic_capacity ||
            strlen(pending_publish.payload) >= payload_capacity)
            return -1;
        strcpy(topic, pending_publish.topic);
        strcpy(payload, pending_publish.payload);
        pending_publish.ready = 0U;
        return 1;
    }
    {
        int packet_status = read_packet(&header, packet, sizeof(packet), &remaining);
        if (packet_status == 0)
            return 0;
        if (packet_status < 0)
            return -1;
    }
    if (header == 0xd0U && remaining == 0U)
    {
        ping_sent_ms = 0U;
        return 0;
    }
    if ((header & 0xf0U) != 0x30U)
        return 0;
    if (decode_publish(packet, remaining, header, topic, topic_capacity,
                       payload, payload_capacity) != 0)
        return -1;
    if ((header & 0x06U) != 0U)
    {
        size_t topic_len = ((size_t)packet[0] << 8) | packet[1];
        size_t ack_pos = 2U + topic_len;
        unsigned char ack[4];
        if (ack_pos + 2U > remaining)
            return -1;
        ack[0] = 0x40U;
        ack[1] = 2U;
        ack[2] = packet[ack_pos];
        ack[3] = packet[ack_pos + 1U];
        if (send_all(ack, sizeof(ack)) != 0)
            return -1;
    }
    return 1;
}

int ed_mqtt_socket_publish(void *user, const char *topic, const char *payload, size_t length)
{
    unsigned char packet[640];
    size_t pos = 0u, rem, n;
    (void)user;
    if ((mqtt_fd < 0 && tls_session == RT_NULL) || !topic || !payload || length > 512u || strlen(topic) > 120u) return -1;
    packet[pos++] = 0x32u; /* QoS 1: only clear the outbox after PUBACK. */
    rem = 4u + strlen(topic) + length;
    if (rem + 5u > sizeof(packet)) return -1;
    n = put_remaining(&packet[pos], rem); pos += n;
    n = strlen(topic); packet[pos++] = (unsigned char)(n >> 8); packet[pos++] = (unsigned char)n;
    memcpy(&packet[pos], topic, n); pos += n;
    packet[pos++] = (unsigned char)(mqtt_packet_id >> 8); packet[pos++] = (unsigned char)mqtt_packet_id;
    memcpy(&packet[pos], payload, length); pos += length;
    if (send_all(packet, pos) != 0) return -1;
    {
        unsigned char header, incoming[1024];
        size_t remaining;
        unsigned attempts = 0U;
        for (;;) {
            uint16_t id;
            int packet_status = read_packet(&header, incoming, sizeof(incoming), &remaining);
            if (packet_status <= 0 || attempts++ >= 8U)
                return -1;
            if (header == 0x40U && remaining == 2U)
            {
                id = (uint16_t)(((uint16_t)incoming[0] << 8) | incoming[1]);
                if (id == mqtt_packet_id)
                    break;
                continue;
            }
            if (header == 0xd0U && remaining == 0U)
            {
                ping_sent_ms = 0U;
                continue;
            }
            if ((header & 0xf0U) == 0x30U && !pending_publish.ready &&
                decode_publish(incoming, remaining, header,
                               pending_publish.topic, sizeof(pending_publish.topic),
                               pending_publish.payload, sizeof(pending_publish.payload)) == 0)
            {
                pending_publish.ready = 1U;
            }
        }
    }
    mqtt_packet_id = (uint16_t)(mqtt_packet_id == 0xffffu ? 1u : mqtt_packet_id + 1u);
    return 1;
}

int ed_mqtt_socket_keepalive(const ed_mqtt_config_t *config)
{
    uint64_t now;
    unsigned char ping[2] = {0xc0U, 0x00U};
    if (!config || (mqtt_fd < 0 && tls_session == RT_NULL))
        return -1;
    now = ed_sensor_now_us() / 1000U;
    if (ping_sent_ms != 0U && now - ping_sent_ms >
        (uint64_t)config->keepalive_s * 2000U)
        return -1;
    if (now - last_activity_ms >= (uint64_t)config->keepalive_s * 1000U)
    {
        if (send_all(ping, sizeof(ping)) != 0)
            return -1;
        ping_sent_ms = now;
    }
    return 0;
}

#endif
