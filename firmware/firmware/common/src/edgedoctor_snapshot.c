#include "edgedoctor_snapshot.h"

#include <string.h>

#if defined(__arm__) || defined(__thumb__)
#include "cmsis_compiler.h"
#define EDGEDOCTOR_SNAPSHOT_BARRIER() __DMB()
#else
#include <stdatomic.h>
#define EDGEDOCTOR_SNAPSHOT_BARRIER() atomic_thread_fence(memory_order_seq_cst)
#endif

void edgedoctor_snapshot_init(edgedoctor_snapshot_t *snapshot)
{
    if (snapshot != NULL)
    {
        memset(snapshot, 0, sizeof(*snapshot));
    }
}

void edgedoctor_snapshot_publish(edgedoctor_snapshot_t *snapshot,
                                 const edgedoctor_result_t *result)
{
    uint8_t next;
    if ((snapshot == NULL) || (result == NULL))
    {
        return;
    }
    next = (uint8_t)(snapshot->active ^ 1U);
    snapshot->result[next] = *result;
    EDGEDOCTOR_SNAPSHOT_BARRIER();
    snapshot->active = next;
    EDGEDOCTOR_SNAPSHOT_BARRIER();
    snapshot->generation++;
}

int edgedoctor_snapshot_read(const edgedoctor_snapshot_t *snapshot,
                             edgedoctor_result_t *result)
{
    uint32_t before;
    uint32_t after;
    uint8_t active;
    unsigned int attempt;
    if ((snapshot == NULL) || (result == NULL))
    {
        return -1;
    }
    for (attempt = 0U; attempt < 3U; ++attempt)
    {
        before = snapshot->generation;
        EDGEDOCTOR_SNAPSHOT_BARRIER();
        active = snapshot->active;
        *result = snapshot->result[active];
        EDGEDOCTOR_SNAPSHOT_BARRIER();
        after = snapshot->generation;
        if (before == after)
        {
            return (after != 0U) ? 1 : 0;
        }
    }
    return 0;
}
