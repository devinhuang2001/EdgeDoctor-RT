#ifndef EDGEDOCTOR_SNAPSHOT_H
#define EDGEDOCTOR_SNAPSHOT_H

#include "edgedoctor_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    edgedoctor_result_t result[2];
    volatile uint32_t generation;
    volatile uint8_t active;
    uint8_t reserved[3];
} edgedoctor_snapshot_t;

void edgedoctor_snapshot_init(edgedoctor_snapshot_t *snapshot);
void edgedoctor_snapshot_publish(edgedoctor_snapshot_t *snapshot,
                                 const edgedoctor_result_t *result);
int edgedoctor_snapshot_read(const edgedoctor_snapshot_t *snapshot,
                             edgedoctor_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
