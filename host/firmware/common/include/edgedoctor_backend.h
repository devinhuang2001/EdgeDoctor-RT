#ifndef EDGEDOCTOR_BACKEND_H
#define EDGEDOCTOR_BACKEND_H

#include "edgedoctor_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EDGEDOCTOR_MODEL_MAX_BYTES (300U * 1024U)
#define EDGEDOCTOR_ARENA_MAX_BYTES (1024U * 1024U)
#define EDGEDOCTOR_NPU_MIN_COVERAGE_PERMILLE 950U

typedef enum
{
    EDGEDOCTOR_BACKEND_REFERENCE = 0,
    EDGEDOCTOR_BACKEND_CMSIS_NN = 1,
    EDGEDOCTOR_BACKEND_U55 = 2
} edgedoctor_backend_kind_t;

typedef enum
{
    EDGEDOCTOR_BACKEND_OK = 0,
    EDGEDOCTOR_BACKEND_INVALID = -1,
    EDGEDOCTOR_BACKEND_UNAVAILABLE = -2,
    EDGEDOCTOR_BACKEND_MODEL_LIMIT = -3,
    EDGEDOCTOR_BACKEND_ARENA_LIMIT = -4,
    EDGEDOCTOR_BACKEND_COVERAGE_LIMIT = -5,
    EDGEDOCTOR_BACKEND_INVOKE_FAILED = -6
} edgedoctor_backend_status_t;

typedef int (*edgedoctor_backend_invoke_fn)(const int8_t *input,
                                             size_t input_bytes,
                                             int8_t *output,
                                             size_t output_bytes,
                                             void *context);

typedef struct
{
    edgedoctor_backend_kind_t kind;
    const char *name;
    uint32_t model_bytes;
    uint32_t arena_bytes;
    uint16_t npu_coverage_permille;
    uint8_t available;
    uint8_t reserved;
    edgedoctor_backend_invoke_fn invoke;
    void *context;
} edgedoctor_backend_t;

int edgedoctor_backend_validate(const edgedoctor_backend_t *backend);
const edgedoctor_backend_t *edgedoctor_backend_select(
    edgedoctor_backend_kind_t requested,
    const edgedoctor_backend_t *backends,
    size_t count,
    int allow_fallback);
int edgedoctor_backend_invoke(const edgedoctor_backend_t *backend,
                              const int8_t *input,
                              size_t input_bytes,
                              int8_t *output,
                              size_t output_bytes);

#ifdef __cplusplus
}
#endif

#endif
