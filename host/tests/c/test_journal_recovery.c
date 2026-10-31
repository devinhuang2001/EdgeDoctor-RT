/* Real journal code and real files; only the RT scheduler and faulting I/O
 * boundary are substituted. Run from an isolated scratch directory. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <rtthread.h>

jmp_buf ed_test_idle;
static int fail_open, fail_write, fail_sync;
static unsigned sync_calls;
static int test_open(const char *path, int flags, int mode)
{
    (void)mode;
    if (fail_open) { errno = EIO; return -1; }
    if (strcmp(path, "/flash/edgedoctor/boot.counter") == 0) path="boot.counter";
    return open(path, flags | O_BINARY, S_IREAD | S_IWRITE);
}
static int test_write(int fd, const void *data, size_t length)
{
    if (fail_write==2) { fail_write=1; return write(fd,data,13); }
    if (fail_write) return -1;
    return write(fd,data,(unsigned)length);
}
static int test_sync(int fd)
{ ++sync_calls; return fail_sync ? -1 : _commit(fd); }
static int test_truncate(int fd, off_t length)
{ return _chsize_s(fd, length) == 0 ? 0 : -1; }
static int test_mkdir(const char *path, int mode) { (void)path; (void)mode; return 0; }
#define open test_open
#define write test_write
#define fsync test_sync
#define ftruncate test_truncate
#define mkdir test_mkdir
#define EDGEDOCTOR_RT
#include "../../firmware/m55/logger/ed_event_journal.c"
#undef open
#undef write
#undef fsync
#undef ftruncate
#undef mkdir

static unsigned replayed, second_revision=2;
static int replay(const ed_event_t *event)
{ assert(event->revision == (replayed ? second_revision : 1)); ++replayed; return 0; }
static void drain(void)
{ if (setjmp(ed_test_idle) == 0) journal_entry(NULL); }
static void recovery(int corrupt_full_record)
{
    ed_event_t event = {0};
    int fd;
    unsigned char corrupt[sizeof(ed_event_journal_record_t)] = {0};
    unlink("events.journal");
    assert(ed_event_journal_init("events.journal") == 0);
    strcpy(event.event_id,"pump-1-1"); event.revision=1;
    assert(ed_event_journal_submit(&event) == 0); drain();
    fd=open("events.journal",O_WRONLY|O_APPEND|O_BINARY); assert(fd>=0);
    assert(write(fd,corrupt,corrupt_full_record ? sizeof(corrupt) : 13) > 0); close(fd);
    assert(ed_event_journal_init("events.journal") == 0);
    event.revision=2;
    assert(ed_event_journal_submit(&event) == 0); drain();
    replayed=0;
    assert(ed_event_journal_replay(replay) == 2);
    assert(replayed == 2);
    unlink("events.journal");
}
static void recovery_after_write_failure(void)
{
    ed_event_t event={0};
    unlink("events.journal");
    assert(ed_event_journal_init("events.journal")==0);
    event.revision=1;
    assert(ed_event_journal_submit(&event)==0); drain();
    event.revision=2; fail_write=2;
    assert(ed_event_journal_submit(&event)==0); drain();
    event.revision=3; fail_write=0;
    assert(ed_event_journal_submit(&event)==0); drain();
    replayed=0; second_revision=3;
    assert(ed_event_journal_replay(replay)==2);
    unlink("events.journal");
}
int main(int argc,char **argv)
{
    if (argc > 1 && strcmp(argv[1],"boot") == 0)
    {
        unlink("boot.counter");
        fail_open=1;
        assert(ed_event_journal_boot_counter() == 0);
        fail_open=0; fail_write=1;
        assert(ed_event_journal_boot_counter() == 0);
        fail_write=0; fail_sync=1;
        assert(ed_event_journal_boot_counter() == 0);
        fail_sync=0; sync_calls=0;
        assert(ed_event_journal_boot_counter() > 0 && sync_calls > 0);
        { uint64_t previous=ed_event_journal_boot_counter();
          assert(ed_event_journal_boot_counter() == previous+1); }
        unlink("boot.counter");
        puts("durable boot counter: passed");
    }
    else { recovery(0); recovery(1); recovery_after_write_failure(); puts("journal tail recovery: passed"); }
    return 0;
}
