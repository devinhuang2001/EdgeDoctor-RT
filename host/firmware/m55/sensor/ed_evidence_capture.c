#include "ed_evidence_capture.h"
#include "ed_evidence_format.h"
#include <string.h>
#include <stdio.h>
#ifdef EDGEDOCTOR_RT
#include <rtthread.h>
#include <dfs_posix.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

#define ED_EVIDENCE_PRE_SAMPLES 1040u
#define ED_EVIDENCE_POST_SAMPLES 1040u
#define ED_EVIDENCE_TOTAL_SAMPLES (ED_EVIDENCE_PRE_SAMPLES + ED_EVIDENCE_POST_SAMPLES)
#define ED_EVIDENCE_EXT __attribute__((section(".cy_socmem_data"), aligned(8)))
static int16_t ring[ED_EVIDENCE_TOTAL_SAMPLES][3] ED_EVIDENCE_EXT;
static uint8_t ring_gap[ED_EVIDENCE_TOTAL_SAMPLES] ED_EVIDENCE_EXT;
static uint8_t next_gap;
static uint8_t capture_gap;
static uint16_t ring_pos;
static uint16_t ring_count;
static uint16_t post_count;
static uint8_t pending;
static uint8_t ready;
static size_t evidence_written;
static uint32_t storage_retry_tick;
static uint8_t storage_retry_pending;
static uint16_t ordered_count;
static uint16_t available_pre_count;
static uint16_t available_post_count;
static char event_id[96];
static char challenge[40];
static int16_t ordered[ED_EVIDENCE_TOTAL_SAMPLES][3] ED_EVIDENCE_EXT;
static uint8_t blob[14000] ED_EVIDENCE_EXT;

void ed_evidence_capture_push(int16_t x, int16_t y, int16_t z)
{
    ring_gap[ring_pos] = next_gap;
    next_gap = 0u;
    ring[ring_pos][0] = x; ring[ring_pos][1] = y; ring[ring_pos][2] = z;
    ring_pos = (uint16_t)((ring_pos + 1u) % ED_EVIDENCE_TOTAL_SAMPLES);
    if (ring_count < ED_EVIDENCE_TOTAL_SAMPLES) ring_count++;
    if (pending && post_count < ED_EVIDENCE_POST_SAMPLES) {
        post_count++;
        if (post_count >= ED_EVIDENCE_POST_SAMPLES) {
            uint16_t pre_count = ring_count > post_count
                ? (uint16_t)(ring_count - post_count) : 0U;
            if (pre_count > ED_EVIDENCE_PRE_SAMPLES)
                pre_count = ED_EVIDENCE_PRE_SAMPLES;
            ordered_count = (uint16_t)(pre_count + post_count);
            available_pre_count = pre_count;
            available_post_count = post_count;
            uint16_t start = (uint16_t)((ring_pos + ED_EVIDENCE_TOTAL_SAMPLES - ordered_count) % ED_EVIDENCE_TOTAL_SAMPLES);
            uint16_t i;
            capture_gap = 0u;
            for (i = 0u; i < ordered_count; i++) {
                uint16_t index = (uint16_t)((start + i) % ED_EVIDENCE_TOTAL_SAMPLES);
                ordered[i][0] = ring[index][0]; ordered[i][1] = ring[index][1]; ordered[i][2] = ring[index][2];
                if (i > 0u && ring_gap[index]) capture_gap = 1u;
            }
            /* Publish the completed capture as a single transition. The
             * consumer thread treats "neither pending nor ready" as idle and
             * would otherwise re-arm a capture between these two stores,
             * pairing the new event id with this capture's samples. */
#ifdef EDGEDOCTOR_RT
            rt_base_t level = rt_hw_interrupt_disable();
#endif
            ready = 1u;
            pending = 0u;
#ifdef EDGEDOCTOR_RT
            rt_hw_interrupt_enable(level);
#endif
        }
    }
}

void ed_evidence_capture_mark_gap(void) { next_gap = 1u; }

int ed_evidence_capture_request(const char *id, const char *token)
{
#ifdef EDGEDOCTOR_RT
    rt_base_t level;
#endif
    if (!id || !token || id[0] == '\0' || token[0] == '\0') return -1;
#ifdef EDGEDOCTOR_RT
    level = rt_hw_interrupt_disable();
#endif
    if (strcmp(id, event_id) == 0 && strcmp(token, challenge) == 0)
    {
#ifdef EDGEDOCTOR_RT
        rt_hw_interrupt_enable(level);
#endif
        return 0; /* Same command retried: do not re-arm or reject it. */
    }
    if (pending || ready)
    {
#ifdef EDGEDOCTOR_RT
        rt_hw_interrupt_enable(level);
#endif
        return -1;
    }
    /* Arm the capture before releasing the critical section so the producer
     * never observes a half-initialised request. */
    rt_snprintf(event_id, sizeof event_id, "%s", id);
    rt_snprintf(challenge, sizeof challenge, "%s", token);
    post_count = 0u; ordered_count = 0u; available_pre_count = 0u;
    available_post_count = 0u; pending = 1u;
#ifdef EDGEDOCTOR_RT
    rt_hw_interrupt_enable(level);
#endif
    return 0;
}

uint8_t ed_evidence_capture_pending(void) { return pending; }

/* The state flags are shared with the sample producer, so every transition
 * has to be a single store the producer cannot observe half-applied. */
static void evidence_set_ready(uint8_t value)
{
#ifdef EDGEDOCTOR_RT
    rt_base_t level = rt_hw_interrupt_disable();
#endif
    ready = value;
#ifdef EDGEDOCTOR_RT
    rt_hw_interrupt_enable(level);
#endif
}

int ed_evidence_capture_poll(void)
{
    char metadata[512], path[160];
    size_t written;
    int fd;
    if (ready != 1u) return 0;
    if (storage_retry_pending && (int32_t)((uint32_t)rt_tick_get() - storage_retry_tick) < 0) return 0;
    rt_snprintf(metadata, sizeof metadata, "{\"event_id\":\"%s\",\"challenge\":\"%s\",\"sample_rate_hz\":208,\"sample_rate_kind\":\"nominal\",\"profile\":\"imu_lf_208_v1\",\"encoding\":\"xyz_int16_le\",\"sensitivity_g_per_adc\":0.000122,\"pre_ms\":%u,\"post_ms\":%u,\"complete\":%s}",
                event_id, challenge, (unsigned)(available_pre_count * 1000U / 208U),
                (unsigned)(available_post_count * 1000U / 208U),
                available_pre_count >= ED_EVIDENCE_PRE_SAMPLES && !capture_gap ? "true" : "false");
    if (ed_edv2_pack((const uint8_t *)metadata, (uint32_t)strlen(metadata), &ordered[0][0], ordered_count, blob, sizeof blob, &written) != 0) { evidence_set_ready(0u); return -1; }
    /* One bounded local capture slot; acknowledged captures must not leave an
     * unbounded file per event on the small LittleFS partition. */
    rt_snprintf(path, sizeof path, "/flash/edgedoctor/evidence-current.edv2.tmp");
    (void)mkdir("/flash/edgedoctor", 0755);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0 || write(fd, blob, written) != (int)written || fsync(fd) != 0) {
        if (fd >= 0) close(fd);
        storage_retry_tick = (uint32_t)rt_tick_get() + 30U * RT_TICK_PER_SECOND;
        storage_retry_pending = 1u;
        return -1;
    }
    if (close(fd) != 0 || rename(path, "/flash/edgedoctor/evidence-current.edv2") != 0) {
        storage_retry_tick = (uint32_t)rt_tick_get() + 30U * RT_TICK_PER_SECOND;
        storage_retry_pending = 1u;
        return -1;
    }
    storage_retry_pending = 0u;
    evidence_written = written; evidence_set_ready(2u);
    return 1;
}

int ed_evidence_capture_get_chunk(uint16_t index, uint16_t *total, const char **id, const uint8_t **data, size_t *length)
{
    size_t offset, remaining;
    uint16_t chunks;
    if (ready != 2u || !total || !id || !data || !length) return -1;
    chunks = (uint16_t)((evidence_written + 507u) / 508u);
    if (index >= chunks) return -1;
    offset = (size_t)index * 508u; remaining = evidence_written - offset;
    *total = chunks; *id = event_id; *data = &blob[offset]; *length = remaining > 508u ? 508u : remaining;
    return 0;
}

void ed_evidence_capture_ack_upload(void)
{
    evidence_set_ready(0u); evidence_written = 0u; ordered_count = 0u;
    /* Keep the last command identity so an in-flight duplicate after the
     * stored receipt cannot arm a second, different capture for that token. */
}

int ed_evidence_capture_accept_receipt(const char *id, const char *token, uint32_t bytes)
{
    if (ready != 2u || !id || !token || strcmp(id, event_id) != 0 ||
        strcmp(token, challenge) != 0 || bytes != evidence_written)
        return -1;
    ed_evidence_capture_ack_upload();
    return 0;
}
