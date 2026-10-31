#ifndef EDGEDOCTOR_CSV_H
#define EDGEDOCTOR_CSV_H

#include "edgedoctor_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EDGEDOCTOR_CSV_ROW_MAX 256U

const char *edgedoctor_csv_header(void);
int edgedoctor_csv_format(char *destination,
                          size_t capacity,
                          const edgedoctor_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
