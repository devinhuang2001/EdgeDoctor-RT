#ifndef EDGEDOCTOR_LOGGER_H
#define EDGEDOCTOR_LOGGER_H

#include "edgedoctor_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint32_t queued;
    uint32_t written;
    uint32_t dropped;
    uint32_t write_errors;
} edgedoctor_logger_stats_t;

int edgedoctor_logger_init(const char *path);
int edgedoctor_logger_submit(const edgedoctor_result_t *result);
void edgedoctor_logger_stats(edgedoctor_logger_stats_t *stats);
const char *edgedoctor_logger_path(void);

#ifdef __cplusplus
}
#endif

#endif
