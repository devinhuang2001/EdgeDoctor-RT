#include "edgedoctor_ipc.h"

#include <stddef.h>

#if defined(__arm__) || defined(__thumb__)
#include "cmsis_compiler.h"
#define EDGEDOCTOR_BARRIER() __DMB()
#else
#include <stdatomic.h>
#define EDGEDOCTOR_BARRIER() atomic_thread_fence(memory_order_seq_cst)
#endif

void edgedoctor_ipc_init(edgedoctor_ipc_slot_t *slot)
{
    if (slot != NULL)
    {
        slot->message.frame_id = 0U;
        slot->message.score_q15 = 0U;
        slot->message.trigger_reason = 0U;
        slot->message.reserved = 0U;
        EDGEDOCTOR_BARRIER();
        slot->state = EDGEDOCTOR_SLOT_EMPTY;
    }
}

int edgedoctor_ipc_publish(edgedoctor_ipc_slot_t *slot,
                           const edgedoctor_sentinel_message_t *message)
{
    if ((slot == NULL) || (message == NULL))
    {
        return -1;
    }
    if (slot->state != EDGEDOCTOR_SLOT_EMPTY)
    {
        return 0;
    }
    slot->state = EDGEDOCTOR_SLOT_WRITING;
    EDGEDOCTOR_BARRIER();
    slot->message = *message;
    EDGEDOCTOR_BARRIER();
    slot->state = EDGEDOCTOR_SLOT_READY;
    return 1;
}

int edgedoctor_ipc_acquire(edgedoctor_ipc_slot_t *slot,
                           edgedoctor_sentinel_message_t *message)
{
    if ((slot == NULL) || (message == NULL))
    {
        return -1;
    }
    if (slot->state != EDGEDOCTOR_SLOT_READY)
    {
        return 0;
    }
    slot->state = EDGEDOCTOR_SLOT_READING;
    EDGEDOCTOR_BARRIER();
    *message = slot->message;
    return 1;
}

int edgedoctor_ipc_release(edgedoctor_ipc_slot_t *slot)
{
    if (slot == NULL)
    {
        return -1;
    }
    if (slot->state != EDGEDOCTOR_SLOT_READING)
    {
        return 0;
    }
    EDGEDOCTOR_BARRIER();
    slot->state = EDGEDOCTOR_SLOT_EMPTY;
    return 1;
}

