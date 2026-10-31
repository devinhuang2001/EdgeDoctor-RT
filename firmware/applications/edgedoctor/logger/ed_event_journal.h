#ifndef ED_EVENT_JOURNAL_H
#define ED_EVENT_JOURNAL_H

#include "ed_monitor_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t queued;
    uint32_t committed;
    uint32_t dropped;
    uint32_t write_errors;
    uint64_t next_sequence;
} ed_event_journal_stats_t;
typedef int (*ed_event_journal_replay_fn)(const ed_event_t *event);

int ed_event_journal_init(const char *path);
int ed_event_journal_submit(const ed_event_t *event);
/* Non-blocking notification for records that have completed fsync. */
int ed_event_journal_take_committed(ed_event_t *event);
void ed_event_journal_stats(ed_event_journal_stats_t *stats);
uint64_t ed_event_journal_next_sequence(void);
uint64_t ed_event_journal_boot_counter(void);
int ed_event_journal_replay(ed_event_journal_replay_fn callback);

#ifdef __cplusplus
}
#endif

#endif
