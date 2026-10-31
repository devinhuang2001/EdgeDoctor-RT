#include "ed_event_journal.h"

#ifdef EDGEDOCTOR_RT
#include <dfs_posix.h>
#include <fcntl.h>
#include <errno.h>
#include <rtthread.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ED_JOURNAL_QUEUE_DEPTH 128U
#define ED_JOURNAL_COMMIT_DEPTH 128U
#define ED_JOURNAL_STACK_BYTES 4096U
#define ED_JOURNAL_COMMIT_MARKER 0xed84c0deU
#define ED_JOURNAL_SCHEMA 1U
#define ED_BOOT_MAGIC 0xed84b007U
#define ED_BOOT_PATH "/flash/edgedoctor/boot.counter"

typedef struct {
    uint32_t schema;
    uint32_t length;
    uint64_t sequence;
    ed_event_t event;
    uint32_t crc32;
    uint32_t commit_marker;
} ed_event_journal_record_t;

typedef struct {
    struct rt_messagequeue queue;
    rt_uint8_t queue_pool[ED_JOURNAL_QUEUE_DEPTH * sizeof(ed_event_t)]
        __attribute__((aligned(8)));
    struct rt_messagequeue commit_queue;
    rt_uint8_t commit_pool[ED_JOURNAL_COMMIT_DEPTH * sizeof(ed_event_t)]
        __attribute__((aligned(8)));
    struct rt_thread thread;
    rt_uint8_t stack[ED_JOURNAL_STACK_BYTES];
    char path[128];
    uint64_t next_sequence;
    ed_event_journal_stats_t stats;
    uint8_t initialized;
    uint8_t recovery_needed;
} ed_event_journal_state_t;

static ed_event_journal_state_t journal
    __attribute__((section(".cy_socmem_data"), aligned(8)));

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t counter;
    uint32_t crc32;
} ed_boot_slot_t;

static int write_exact(int fd, const void *data, size_t length);

static uint32_t journal_crc32(const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xffffffffU;
    size_t index;
    unsigned bit;
    for (index = 0U; index < length; ++index)
    {
        crc ^= bytes[index];
        for (bit = 0U; bit < 8U; ++bit)
            crc = (crc >> 1U) ^ (0xedb88320U &
                (uint32_t)-(int32_t)(crc & 1U));
    }
    return ~crc;
}

static uint64_t next_boot_counter(void)
{
    ed_boot_slot_t slots[2];
    ed_boot_slot_t next;
    uint32_t best_version = 0U;
    uint64_t best_counter = 0U;
    unsigned best_slot = 0U;
    int fd;
    int count = 0;
    int status;
    unsigned index;
    memset(slots, 0, sizeof(slots));
    fd = open(ED_BOOT_PATH, O_RDONLY, 0);
    if (fd >= 0)
    {
        count = read(fd, slots, sizeof(slots));
        if (close(fd) != 0 || count < 0)
            return 0U;
    }
    else if (errno != ENOENT)
        return 0U;
    for (index = 0U; index < 2U; ++index)
    {
        if (slots[index].magic == ED_BOOT_MAGIC &&
            slots[index].crc32 == journal_crc32(&slots[index],
                                                offsetof(ed_boot_slot_t, crc32)) &&
            slots[index].version >= best_version)
        {
            best_version = slots[index].version;
            best_counter = slots[index].counter;
            best_slot = index;
        }
    }
    /* Nonempty but wholly corrupt state cannot safely be reset to boot 1. */
    if ((count > 0 && best_version == 0U) || best_version == UINT32_MAX ||
        best_counter == UINT64_MAX)
        return 0U;
    memset(&next, 0, sizeof(next));
    next.magic = ED_BOOT_MAGIC;
    next.version = best_version + 1U;
    next.counter = best_counter + 1U;
    next.crc32 = journal_crc32(&next, offsetof(ed_boot_slot_t, crc32));
    fd = open(ED_BOOT_PATH, O_RDWR | O_CREAT, 0);
    if (fd < 0)
        return 0U;
    status = lseek(fd, (off_t)((best_slot ^ 1U) * sizeof(next)), SEEK_SET) ==
                 (off_t)((best_slot ^ 1U) * sizeof(next)) &&
             write_exact(fd, &next, sizeof(next)) == 0 && fsync(fd) == 0;
    if (close(fd) != 0 || !status)
        return 0U;
    return next.counter;
}

static int write_exact(int fd, const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t written = 0U;
    while (written < length)
    {
        int count = write(fd, bytes + written, length - written);
        if (count <= 0)
            return -1;
        written += (size_t)count;
    }
    return 0;
}

static int journal_recover_sequence(void)
{
    ed_event_journal_record_t record;
    off_t valid_bytes = 0;
    int count;
    int status = 0;
    int fd = open(journal.path, O_RDWR | O_CREAT, 0);
    if (fd < 0)
        return -1;
    journal.next_sequence = 1U;
    while ((count = read(fd, &record, sizeof(record))) == (int)sizeof(record))
    {
        uint32_t crc = journal_crc32(&record,
                                     offsetof(ed_event_journal_record_t, crc32));
        if (record.schema != ED_JOURNAL_SCHEMA || record.length != sizeof(record) ||
            record.commit_marker != ED_JOURNAL_COMMIT_MARKER || crc != record.crc32)
            break;
        if (record.sequence >= journal.next_sequence)
            journal.next_sequence = record.sequence + 1U;
        valid_bytes += (off_t)sizeof(record);
    }
    /* A short record or invalid CRC is a torn tail, not usable history. Never
     * append behind it: replay would stop here forever. Read errors, however,
     * must not be mistaken for corruption and destroy valid data. */
    if (count < 0)
        status = -1;
    else if (count > 0 && (ftruncate(fd, valid_bytes) != 0 || fsync(fd) != 0))
        status = -1;
    if (close(fd) != 0)
        status = -1;
    journal.stats.next_sequence = journal.next_sequence;
    return status;
}

static void journal_entry(void *parameter)
{
    ed_event_t event;
    (void)parameter;
    while (1)
    {
        ed_event_journal_record_t record;
        int fd;
        if (rt_mq_recv(&journal.queue, &event, sizeof(event),
                       RT_WAITING_FOREVER) != (rt_ssize_t)sizeof(event))
            continue;
        if (journal.recovery_needed)
        {
            if (journal_recover_sequence() != 0)
            {
                journal.stats.write_errors++;
                continue;
            }
            journal.recovery_needed = 0U;
        }
        memset(&record, 0, sizeof(record));
        record.schema = ED_JOURNAL_SCHEMA;
        record.length = sizeof(record);
        record.sequence = journal.next_sequence++;
        journal.stats.next_sequence = journal.next_sequence;
        record.event = event;
        record.commit_marker = ED_JOURNAL_COMMIT_MARKER;
        record.crc32 = journal_crc32(&record,
                                     offsetof(ed_event_journal_record_t, crc32));
        fd = open(journal.path, O_WRONLY | O_CREAT | O_APPEND, 0);
        if (fd < 0 || write_exact(fd, &record, sizeof(record)) != 0 ||
            (fd >= 0 && fsync(fd) != 0))
        {
            journal.stats.write_errors++;
            journal.recovery_needed = 1U;
            if (fd >= 0)
                close(fd);
            continue;
        }
        close(fd);
        journal.stats.committed++;
        (void)rt_mq_send(&journal.commit_queue, &event, sizeof(event));
    }
}

int ed_event_journal_init(const char *path)
{
    size_t length;
    if (!path)
        return -1;
    length = strlen(path);
    if (length == 0U || length >= sizeof(journal.path))
        return -1;
    memset(&journal, 0, sizeof(journal));
    memcpy(journal.path, path, length + 1U);
    (void)mkdir("/flash/edgedoctor", 0755);
    journal.next_sequence = 1U;
    journal.stats.next_sequence = journal.next_sequence;
    if (journal_recover_sequence() != 0)
        return -1;
    if (rt_mq_init(&journal.queue, "edjrnl", journal.queue_pool,
                   sizeof(ed_event_t), sizeof(journal.queue_pool),
                   RT_IPC_FLAG_FIFO) != RT_EOK)
        return -1;
    if (rt_mq_init(&journal.commit_queue, "edjcm", journal.commit_pool,
                   sizeof(ed_event_t), sizeof(journal.commit_pool),
                   RT_IPC_FLAG_FIFO) != RT_EOK)
        return -1;
    if (rt_thread_init(&journal.thread, "edjrnl", journal_entry, RT_NULL,
                       journal.stack, sizeof(journal.stack), 19U, 10U) != RT_EOK ||
        rt_thread_startup(&journal.thread) != RT_EOK)
        return -1;
    journal.initialized = 1U;
    return 0;
}

int ed_event_journal_submit(const ed_event_t *event)
{
    if (!journal.initialized || !event)
        return -1;
    if (rt_mq_send(&journal.queue, event, sizeof(*event)) != RT_EOK)
    {
        journal.stats.dropped++;
        return -2;
    }
    journal.stats.queued++;
    return 0;
}

int ed_event_journal_take_committed(ed_event_t *event)
{
    if (!journal.initialized || !event)
        return -1;
    return rt_mq_recv(&journal.commit_queue, event, sizeof(*event),
                      RT_WAITING_NO) == (rt_ssize_t)sizeof(*event) ? 0 : -1;
}

void ed_event_journal_stats(ed_event_journal_stats_t *stats)
{
    if (stats)
        *stats = journal.stats;
}

uint64_t ed_event_journal_next_sequence(void)
{
    return journal.next_sequence;
}

uint64_t ed_event_journal_boot_counter(void)
{
    return next_boot_counter();
}

int ed_event_journal_replay(ed_event_journal_replay_fn callback)
{
    ed_event_journal_record_t record;
    int fd;
    int replayed = 0;
    if (!callback)
        return -1;
    fd = open(journal.path, O_RDONLY, 0);
    if (fd < 0)
        return 0;
    while (read(fd, &record, sizeof(record)) == (int)sizeof(record))
    {
        if (record.schema != ED_JOURNAL_SCHEMA || record.length != sizeof(record) ||
            record.commit_marker != ED_JOURNAL_COMMIT_MARKER ||
            journal_crc32(&record, offsetof(ed_event_journal_record_t, crc32)) != record.crc32)
            break;
        if (callback(&record.event) != 0)
            break;
        ++replayed;
    }
    close(fd);
    return replayed;
}

#else

int ed_event_journal_init(const char *path) { (void)path; return -1; }
int ed_event_journal_submit(const ed_event_t *event) { (void)event; return -1; }
int ed_event_journal_take_committed(ed_event_t *event) { (void)event; return -1; }
void ed_event_journal_stats(ed_event_journal_stats_t *stats) { if (stats) *stats = (ed_event_journal_stats_t){0}; }
uint64_t ed_event_journal_next_sequence(void) { return 0U; }
uint64_t ed_event_journal_boot_counter(void) { return 0U; }
int ed_event_journal_replay(ed_event_journal_replay_fn callback) { (void)callback; return -1; }

#endif
