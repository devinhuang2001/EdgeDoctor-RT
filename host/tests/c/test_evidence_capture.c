#include <rtthread.h>
#include <dfs_posix.h>
#include <assert.h>
#include <stdarg.h>
#include "ed_evidence_format.h"
static unsigned writes;
static int fail_write;
static uint32_t capture_tick;
#define rt_tick_get() capture_tick
static int capture_open(const char *path, int flags, ...)
{ (void)flags; assert(strcmp(path, "/flash/edgedoctor/evidence-current.edv2.tmp") == 0); return 3; }
static int capture_write(int fd, const void *data, size_t size)
{ (void)fd; assert(ed_edv2_validate(data, size) == 0); ++writes; return fail_write ? -1 : (int)size; }
static int capture_close(int fd) { (void)fd; return 0; }
static int capture_sync(int fd) { (void)fd; return 0; }
static int capture_rename(const char *from, const char *to) {
    assert(strcmp(from, "/flash/edgedoctor/evidence-current.edv2.tmp") == 0);
    assert(strcmp(to, "/flash/edgedoctor/evidence-current.edv2") == 0);
    return 0;
}
static int capture_mkdir(const char *path, unsigned mode) { (void)path; (void)mode; return 0; }
#define EDGEDOCTOR_RT
#define open capture_open
#define write capture_write
#define close capture_close
#define fsync capture_sync
#define mkdir capture_mkdir
#define rename capture_rename
#include "../../firmware/m55/sensor/ed_evidence_capture.c"

int main(void)
{
    unsigned i;
    uint16_t total;
    const char *id;
    const uint8_t *data;
    size_t length, bytes = 0;
    for (i = 0; i < 1040; ++i) ed_evidence_capture_push(1, 2, 3);
    assert(ed_evidence_capture_request("event-1", "challenge-1") == 0);
    assert(ed_evidence_capture_request("event-1", "challenge-1") == 0);
    for (i = 0; i < 1040; ++i) ed_evidence_capture_push(4, 5, 6);
    fail_write = 1;
    assert(ed_evidence_capture_poll() < 0);
    assert(ed_evidence_capture_request("other", "token") < 0);
    assert(ed_evidence_capture_poll() == 0 && writes == 1);
    capture_tick = 29999;
    assert(ed_evidence_capture_poll() == 0 && writes == 1);
    capture_tick = 30000;
    fail_write = 0;
    assert(ed_evidence_capture_poll() == 1 && writes == 2);
    assert(ed_evidence_capture_poll() == 0 && writes == 2);
    assert(ed_evidence_capture_get_chunk(0, &total, &id, &data, &length) == 0);
    for (i = 0; i < total; ++i) {
        assert(ed_evidence_capture_get_chunk((uint16_t)i, &total, &id, &data, &length) == 0);
        bytes += length;
    }
    assert(ed_evidence_capture_request("other", "token") < 0); /* PUBACK alone cannot free it. */
    assert(ed_evidence_capture_accept_receipt("event-1", "wrong", (uint32_t)bytes) < 0);
    assert(ed_evidence_capture_accept_receipt("event-1", "challenge-1", (uint32_t)bytes + 1) < 0);
    assert(ed_evidence_capture_accept_receipt("event-1", "challenge-1", (uint32_t)bytes) == 0);
    assert(ed_evidence_capture_accept_receipt("event-1", "challenge-1", (uint32_t)bytes) < 0);
    assert(ed_evidence_capture_request("event-1", "challenge-1") == 0);
    assert(ed_evidence_capture_pending() == 0);
    assert(ed_evidence_capture_request("next", "token") == 0);
    ed_evidence_capture_mark_gap();
    for (i = 0; i < 1040; ++i) ed_evidence_capture_push(7, 8, 9);
    assert(ed_evidence_capture_poll() == 1);
    assert(capture_gap == 1u); /* Full sample count must not hide a capture gap. */
    puts("evidence retained until matching storage receipt, single write, disk retry: passed");
    return 0;
}
