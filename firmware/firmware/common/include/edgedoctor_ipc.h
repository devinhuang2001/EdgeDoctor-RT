#ifndef EDGEDOCTOR_IPC_H
#define EDGEDOCTOR_IPC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    EDGEDOCTOR_SLOT_EMPTY = 0,
    EDGEDOCTOR_SLOT_WRITING = 1,
    EDGEDOCTOR_SLOT_READY = 2,
    EDGEDOCTOR_SLOT_READING = 3
} edgedoctor_slot_state_t;

typedef struct
{
    uint32_t frame_id;
    uint16_t score_q15;
    uint8_t trigger_reason;
    uint8_t reserved;
} edgedoctor_sentinel_message_t;

typedef struct
{
    volatile uint32_t state;
    edgedoctor_sentinel_message_t message;
} edgedoctor_ipc_slot_t;

void edgedoctor_ipc_init(edgedoctor_ipc_slot_t *slot);
int edgedoctor_ipc_publish(edgedoctor_ipc_slot_t *slot,
                           const edgedoctor_sentinel_message_t *message);
int edgedoctor_ipc_acquire(edgedoctor_ipc_slot_t *slot,
                           edgedoctor_sentinel_message_t *message);
int edgedoctor_ipc_release(edgedoctor_ipc_slot_t *slot);

#ifdef __cplusplus
}
#endif

#endif

