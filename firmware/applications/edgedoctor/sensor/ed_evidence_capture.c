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
static uint16_t ring_pos;
static uint16_t ring_count;
static uint16_t post_count;
static uint8_t pending;
static uint8_t ready;
static size_t evidence_written;
static uint16_t ordered_count;
static uint16_t available_pre_count;
static uint16_t available_post_count;
static char event_id[96];
static char challenge[40];
static int16_t ordered[ED_EVIDENCE_TOTAL_SAMPLES][3] ED_EVIDENCE_EXT;
static uint8_t blob[14000] ED_EVIDENCE_EXT;

void ed_evidence_capture_push(int16_t x, int16_t y, int16_t z)
{
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
            for (i = 0u; i < ordered_count; i++) {
                uint16_t index = (uint16_t)((start + i) % ED_EVIDENCE_TOTAL_SAMPLES);
                ordered[i][0] = ring[index][0]; ordered[i][1] = ring[index][1]; ordered[i][2] = ring[index][2];
            }
            pending = 0u; ready = 1u;
        }
    }
}

int ed_evidence_capture_request(const char *id, const char *token)
{
    if (!id || !token || id[0] == '\0' || token[0] == '\0' || pending || ready) return -1;
    rt_snprintf(event_id, sizeof event_id, "%s", id);
    rt_snprintf(challenge, sizeof challenge, "%s", token);
    post_count = 0u; ordered_count = 0u; available_pre_count = 0u;
    available_post_count = 0u; pending = 1u;
    return 0;
}

uint8_t ed_evidence_capture_pending(void) { return pending; }

int ed_evidence_capture_poll(void)
{
    char metadata[512], path[160];
    size_t written;
    int fd;
    if (!ready) return 0;
    rt_snprintf(metadata, sizeof metadata, "{\"event_id\":\"%s\",\"challenge\":\"%s\",\"sample_rate_hz\":208,\"pre_ms\":%u,\"post_ms\":%u,\"complete\":%s}",
                event_id, challenge, (unsigned)(available_pre_count * 1000U / 208U),
                (unsigned)(available_post_count * 1000U / 208U),
                available_pre_count >= ED_EVIDENCE_PRE_SAMPLES ? "true" : "false");
    if (ed_edv2_pack((const uint8_t *)metadata, (uint32_t)strlen(metadata), &ordered[0][0], ordered_count, blob, sizeof blob, &written) != 0) { ready = 0u; return -1; }
    rt_snprintf(path, sizeof path, "/flash/edgedoctor/evidence-%s.edv2", event_id);
    (void)mkdir("/flash/edgedoctor", 0755);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0 || write(fd, blob, written) != (int)written) { if (fd >= 0) close(fd); ready = 0u; return -1; }
    close(fd); evidence_written = written; ready = 2u;
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
    ready = 0u; evidence_written = 0u; ordered_count = 0u;
    event_id[0] = '\0'; challenge[0] = '\0';
}
