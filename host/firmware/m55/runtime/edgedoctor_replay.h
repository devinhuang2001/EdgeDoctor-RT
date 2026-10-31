#ifndef EDGEDOCTOR_REPLAY_H
#define EDGEDOCTOR_REPLAY_H

#include "edgedoctor_edgepack.h"
#include "edgedoctor_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*edgedoctor_replay_callback_t)(
    const edgedoctor_edgepack_header_t *header,
    const edgedoctor_chunk_t *chunk,
    void *context);

typedef enum
{
    EDGEDOCTOR_REPLAY_OK = 0,
    EDGEDOCTOR_REPLAY_ARGUMENT = -100,
    EDGEDOCTOR_REPLAY_OPEN = -101,
    EDGEDOCTOR_REPLAY_IO = -102,
    EDGEDOCTOR_REPLAY_FORMAT = -103,
    EDGEDOCTOR_REPLAY_UNSUPPORTED = -104,
    EDGEDOCTOR_REPLAY_CALLBACK = -105
} edgedoctor_replay_status_t;

int edgedoctor_replay_file(const char *path,
                           int real_time,
                           edgedoctor_replay_callback_t callback,
                           void *context);

#ifdef __cplusplus
}
#endif

#endif
