#include "edgedoctor_backend.h"

int edgedoctor_backend_validate(const edgedoctor_backend_t *backend)
{
    if ((backend == NULL) || (backend->name == NULL) ||
        (backend->kind > EDGEDOCTOR_BACKEND_U55))
    {
        return EDGEDOCTOR_BACKEND_INVALID;
    }
    if ((backend->available == 0U) || (backend->invoke == NULL))
    {
        return EDGEDOCTOR_BACKEND_UNAVAILABLE;
    }
    if (backend->model_bytes > EDGEDOCTOR_MODEL_MAX_BYTES)
    {
        return EDGEDOCTOR_BACKEND_MODEL_LIMIT;
    }
    if (backend->arena_bytes > EDGEDOCTOR_ARENA_MAX_BYTES)
    {
        return EDGEDOCTOR_BACKEND_ARENA_LIMIT;
    }
    if ((backend->kind == EDGEDOCTOR_BACKEND_U55) &&
        (backend->npu_coverage_permille < EDGEDOCTOR_NPU_MIN_COVERAGE_PERMILLE))
    {
        return EDGEDOCTOR_BACKEND_COVERAGE_LIMIT;
    }
    return EDGEDOCTOR_BACKEND_OK;
}

const edgedoctor_backend_t *edgedoctor_backend_select(
    edgedoctor_backend_kind_t requested,
    const edgedoctor_backend_t *backends,
    size_t count,
    int allow_fallback)
{
    const edgedoctor_backend_t *fallback = NULL;
    size_t index;
    if ((backends == NULL) || (requested > EDGEDOCTOR_BACKEND_U55))
    {
        return NULL;
    }
    for (index = 0U; index < count; ++index)
    {
        if (edgedoctor_backend_validate(&backends[index]) != EDGEDOCTOR_BACKEND_OK)
        {
            continue;
        }
        if (backends[index].kind == requested)
        {
            return &backends[index];
        }
        if ((fallback == NULL) || (backends[index].kind > fallback->kind))
        {
            fallback = &backends[index];
        }
    }
    return (allow_fallback != 0) ? fallback : NULL;
}

int edgedoctor_backend_invoke(const edgedoctor_backend_t *backend,
                              const int8_t *input,
                              size_t input_bytes,
                              int8_t *output,
                              size_t output_bytes)
{
    int status = edgedoctor_backend_validate(backend);
    if (status != EDGEDOCTOR_BACKEND_OK)
    {
        return status;
    }
    if ((input == NULL) || (input_bytes == 0U) ||
        (output == NULL) || (output_bytes < EDGEDOCTOR_CLASS_COUNT))
    {
        return EDGEDOCTOR_BACKEND_INVALID;
    }
    return (backend->invoke(input, input_bytes, output, output_bytes, backend->context) == 0)
        ? EDGEDOCTOR_BACKEND_OK : EDGEDOCTOR_BACKEND_INVOKE_FAILED;
}
