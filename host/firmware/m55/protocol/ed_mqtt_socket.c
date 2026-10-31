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

/* Inbound PUBLISH frames waiting to be handed to ed_mqtt_socket_poll. The old
 * single slot was only "enough" because a lost command was invisible: while
 * ed_mqtt_socket_publish waited for its own PUBACK, a second inbound PUBLISH
 * overwrote the first, and the publish path never acknowledged either. The
 * broker then redelivered, the worker kept overwriting, and no operator could
 * tell the difference. The queue is bounded; when it is full the PUBACK is
 * withheld rather than the frame dropped, so MQTT treats the message as
 * undelivered and resends it instead of losing it. */
#define ED_MQTT_INBOX_MAX 8u
/* Bounds for the wait on our own PUBACK. The receive timeout is 100 ms, so a
 * slow broker used to look like a broken link; the loop now waits until the
 * deadline instead of failing on the first quiet interval. */
#define ED_MQTT_PUBACK_WAIT_MS 5000U
#define ED_MQTT_PUBACK_MAX_PACKETS 32U

typedef struct {
    char topic[160];
    char payload[512];
} mqtt_inbox_item_t;

static mqtt_inbox_item_t inbox_queue[ED_MQTT_INBOX_MAX];
static uint8_t inbox_head;
static uint8_t inbox_count;
static uint32_t inbox_dropped;
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
                          size_t payload_capacity, uint16_t *packet_id)
{
    size_t pos = 0U;
    size_t topic_len;
    size_t payload_len;
    if (packet_id)
        *packet_id = 0U;
    if ((header & 0xf0U) != 0x30U || remaining < 2U || !topic || !payload)
        return -1;
    topic_len = ((size_t)packet[0] << 8) | packet[1];
    pos = 2U;
    if (topic_len == 0U || topic_len >= topic_capacity || pos + topic_len > remaining)
        return -1;
    memcpy(topic, packet + pos, topic_len);
    topic[topic_len] = '\0';
    pos += topic_len;
    /* QoS > 0 carries a packet id that must survive into the PUBACK. Dropping
     * it here is what left inbound QoS 1 messages unacknowledged. */
    if ((header & 0x06U) != 0U)
    {
        if (pos + 2U > remaining)
            return -1;
        if (packet_id)
            *packet_id = (uint16_t)(((uint16_t)packet[pos] << 8) | packet[pos + 1U]);
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

static int send_puback(uint16_t packet_id)
{
    unsigned char ack[4];
    ack[0] = 0x40U;
    ack[1] = 2U;
    ack[2] = (unsigned char)(packet_id >> 8);
    ack[3] = (unsigned char)packet_id;
    return send_all(ack, sizeof(ack));
}

/* Queue one inbound PUBLISH and acknowledge it only once it is safely stored.
 * Returns 0 when the frame is queued and (for QoS > 0) acknowledged, -1 when
 * the queue is full or the frame is malformed. A -1 leaves the frame
 * unacknowledged on purpose: the broker will redeliver, which is the only
 * loss-free option available with a bounded buffer. */
static int inbox_push(const unsigned char *packet, size_t remaining,
                      unsigned char header)
{
    uint16_t packet_id = 0U;
    mqtt_inbox_item_t *slot;
    if (inbox_count >= ED_MQTT_INBOX_MAX)
    {
        inbox_dropped++;
        return -1;
    }
    slot = &inbox_queue[(uint8_t)((inbox_head + inbox_count) % ED_MQTT_INBOX_MAX)];
    if (decode_publish(packet, remaining, header, slot->topic, sizeof(slot->topic),
                       slot->payload, sizeof(slot->payload), &packet_id) != 0)
        return -1;
    inbox_count++;
    if (packet_id != 0U && send_puback(packet_id) != 0)
        return -1;
    return 0;
}

/* Returns 1 with the oldest queued frame copied out, 0 when the queue is empty,
 * -1 when the caller's buffers cannot hold it. */
static int inbox_pop(char *topic, size_t topic_capacity,
                     char *payload, size_t payload_capacity)
{
    mqtt_inbox_item_t *slot;
    size_t topic_len;
    size_t payload_len;
    if (inbox_count == 0U)
        return 0;
    slot = &inbox_queue[inbox_head];
    topic_len = strlen(slot->topic);
    payload_len = strlen(slot->payload);
    if (topic_len >= topic_capacity || payload_len >= payload_capacity)
        return -1;
    memcpy(topic, slot->topic, topic_len + 1U);
    memcpy(payload, slot->payload, payload_len + 1U);
    inbox_head = (uint8_t)((inbox_head + 1U) % ED_MQTT_INBOX_MAX);
    inbox_count--;
    return 1;
}

/* Observation hook for `edmqtt info`: how many inbound frames were not queued
 * because the inbox was full. Non-zero means the broker is being asked to
 * redeliver and the worker is not draining fast enough. */
uint32_t ed_mqtt_socket_inbox_dropped(void)
{
    return inbox_dropped;
}

static int mqtt_connect_packet(const ed_mqtt_config_t *c)
{
    unsigned char packet[384];
    size_t length;
    if (ed_mqtt_encode_connect(c, packet, sizeof(packet), &length) != 0)
        return -1;
    if (send_all(packet, length) != 0) return -1;
    {
        unsigned char ack[4];
        if (recv_all(ack, sizeof ack) != 0 || ack[0] != 0x20u || ack[1] != 2u || ack[3] != 0u) return -1;
    }
    return 0;
}

static int mqtt_subscribe_packet(const ed_mqtt_config_t *c)
{
    unsigned char packet[512], body[480], ack[7];
    char topic[ED_ID_MAX * 2u + 32u];
    char receipt_topic[ED_ID_MAX * 2u + 40u];
    char evidence_topic[ED_ID_MAX * 2u + 40u];
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
    body[body_pos++] = 1u; /* QoS 1 commands: acknowledge only after inbox storage. */
    body[body_pos++] = (unsigned char)(receipt_len >> 8);
    body[body_pos++] = (unsigned char)receipt_len;
    memcpy(&body[body_pos], receipt_topic, receipt_len); body_pos += receipt_len;
    body[body_pos++] = 1u; /* QoS 1 durable event receipts. */
    rt_snprintf(evidence_topic, sizeof evidence_topic,
                "edgedoctor/%s/%s/evidence_receipts", c->site_id, c->device_id);
    receipt_len = strlen(evidence_topic);
    body[body_pos++] = (unsigned char)(receipt_len >> 8);
    body[body_pos++] = (unsigned char)receipt_len;
    memcpy(&body[body_pos], evidence_topic, receipt_len); body_pos += receipt_len;
    body[body_pos++] = 1u;
    packet[pos++] = 0x82u;
    remaining_length = put_remaining(&packet[pos], body_pos);
    pos += remaining_length;
    memcpy(&packet[pos], body, body_pos);
    pos += body_pos;
    if (send_all(packet, pos) != 0) return -1;
    if (recv_all(ack, sizeof ack) != 0 || ack[0] != 0x90u || ack[1] != 5u ||
        ack[2] != (unsigned char)(subscribe_id >> 8) ||
        ack[3] != (unsigned char)subscribe_id || ack[4] == 0x80u ||
        ack[5] == 0x80u || ack[6] == 0x80u) return -1;
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
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0)
        {
            mqtt_fd = fd;
            rc = mqtt_connect_packet(config);
            if (rc == 0)
                break;
            /* The CONNECT exchange failed, so this descriptor still belongs to
             * the loop. Leaving mqtt_fd set makes the failure path close it a
             * second time, and on a later iteration close a descriptor that
             * socket() has already handed out again. */
            mqtt_fd = -1;
        }
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
    /* The inbox survives a reconnect on purpose. Everything still queued was
     * already acknowledged to the broker, so clearing it here would drop a
     * command the broker believes is delivered, and nothing would ever resend
     * it. Stale commands are rejected on their own merit: ack_event carries an
     * expected_revision and fails with revision_mismatch once the board has
     * moved on. */
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
    int queued;
    if (!topic || !payload || topic_capacity == 0u || payload_capacity == 0u) return -1;
    /* Drain what a publish turn already received before touching the socket:
     * those frames are acknowledged and must not be skipped. */
    queued = inbox_pop(topic, topic_capacity, payload, payload_capacity);
    if (queued != 0)
        return queued;
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
    /* Every inbound PUBLISH goes through the inbox, which is also where the
     * QoS 1 PUBACK is emitted, so the two paths cannot disagree about whether
     * a command was acknowledged. */
    if (inbox_push(packet, remaining, header) != 0)
        return -1;
    return inbox_pop(topic, topic_capacity, payload, payload_capacity);
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
        uint64_t deadline_ms = ed_sensor_now_us() / 1000U + ED_MQTT_PUBACK_WAIT_MS;
        for (;;) {
            uint16_t id;
            int packet_status;
            if (attempts >= ED_MQTT_PUBACK_MAX_PACKETS ||
                ed_sensor_now_us() / 1000U >= deadline_ms)
                return -1;
            packet_status = read_packet(&header, incoming, sizeof(incoming), &remaining);
            if (packet_status < 0)
                return -1;
            /* A quiet interval is not a dead link: the receive timeout is only
             * 100 ms, so treating it as failure tore the connection down and
             * re-queued the message whenever the broker was merely slow. */
            if (packet_status == 0)
                continue;
            ++attempts;
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
            if ((header & 0xf0U) == 0x30U)
            {
                /* A command must not be lost because this publish is waiting for
                 * its own PUBACK. Queue it and acknowledge it here; the worker
                 * collects it on its next poll. */
                if (inbox_push(incoming, remaining, header) != 0)
                    return -1;
                continue;
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
