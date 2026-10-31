#include "ed_inference.h"
#include "ed_inference_models.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_log.h"
#include <rtthread.h>
#include <cmath>
#include <cstring>
#include <new>

extern "C" int ed_npu_init(void);

void MicroPrintf(const char *format, ...)
{
    (void)format;
}

namespace {
constexpr size_t arena_size = 24 * 1024;
alignas(32) unsigned char cpu_arena[arena_size] __attribute__((section(".cy_socmem_data")));
alignas(32) unsigned char npu_arena[arena_size] __attribute__((section(".cy_socmem_data")));
alignas(tflite::MicroInterpreter) unsigned char cpu_storage[sizeof(tflite::MicroInterpreter)];
alignas(tflite::MicroInterpreter) unsigned char npu_storage[sizeof(tflite::MicroInterpreter)];
tflite::MicroMutableOpResolver<2> resolver;
struct rt_mutex inference_mutex;
edgedoctor_backend_t backends[2];
const edgedoctor_backend_t *selected;
bool initialized;

int invoke(const int8_t *input, size_t input_bytes, int8_t *output,
           size_t output_bytes, void *context)
{
    auto *interpreter = static_cast<tflite::MicroInterpreter *>(context);
    if (!interpreter || input_bytes != 64 || output_bytes != 5) return -1;
    auto *in = interpreter->input(0);
    auto *out = interpreter->output(0);
    if (in->type != kTfLiteInt8 || out->type != kTfLiteInt8 ||
        in->bytes != 64 || out->bytes != 5) return -1;
    std::memcpy(in->data.int8, input, input_bytes);
    if (interpreter->Invoke() != kTfLiteOk) return -1;
    std::memcpy(output, out->data.int8, output_bytes);
    return 0;
}


bool prepare(edgedoctor_backend_t &backend, const unsigned char *model,
             unsigned char *arena, unsigned char *storage)
{
    const auto *graph = tflite::GetModel(model);
    if (graph->version() != TFLITE_SCHEMA_VERSION) return false;
    auto *interpreter = new (storage) tflite::MicroInterpreter(graph, resolver, arena, arena_size);
    if (interpreter->AllocateTensors() != kTfLiteOk) return false;
    if (interpreter->input(0)->bytes != 64 || interpreter->output(0)->bytes != 5) return false;
    backend.arena_bytes = interpreter->arena_used_bytes();
    backend.context = interpreter;
    backend.available = 1;
    return true;
}
}

extern "C" int ed_inference_init(void)
{
    if (initialized) return selected ? 0 : -1;
    initialized = true;
    if (rt_mutex_init(&inference_mutex, "ednn", RT_IPC_FLAG_PRIO) != RT_EOK) return -1;
    if (resolver.AddFullyConnected() != kTfLiteOk || resolver.AddEthosU() != kTfLiteOk) return -1;
    backends[0] = {EDGEDOCTOR_BACKEND_CMSIS_NN, "tflm-cmsis-nn", ed_model_cpu_size,
                   0, 0, 0, 0, invoke, nullptr};
    // Coverage comes from the checked-in Vela compile report: both FC ops on NPU.
    backends[1] = {EDGEDOCTOR_BACKEND_U55, "ethos-u55-128", ed_model_u55_size,
                   0, 1000, 0, 0, invoke, nullptr};
    if (!prepare(backends[0], ed_model_cpu, cpu_arena, cpu_storage)) return -1;
    if (ed_npu_init() == 0) prepare(backends[1], ed_model_u55, npu_arena, npu_storage);
    selected = edgedoctor_backend_select(EDGEDOCTOR_BACKEND_U55, backends, 2, 1);
    rt_kprintf("ednn: cmsis=%u u55=%u selected=%s\n", backends[0].available,
               backends[1].available, selected ? selected->name : "unavailable");
    return selected ? 0 : -1;
}

extern "C" const edgedoctor_backend_t *ed_inference_current(void) { return selected; }

extern "C" int ed_inference_choose(edgedoctor_backend_kind_t kind)
{
    if (!initialized) return -1;
    rt_mutex_take(&inference_mutex, RT_WAITING_FOREVER);
    const auto *candidate = edgedoctor_backend_select(kind, backends, 2, 0);
    if (candidate) selected = candidate;
    rt_mutex_release(&inference_mutex);
    return candidate ? 0 : -1;
}

extern "C" int ed_inference_run(const int16_t vibration[64], edgedoctor_result_t *result)
{
    if (!vibration || !result || !selected) return -1;
    int8_t input[64], logits[5];
    for (unsigned i = 0; i < 64; ++i) {
        int value = static_cast<int>(std::lround(vibration[i] / ed_model_input_scale)) + ed_model_input_zero;
        input[i] = static_cast<int8_t>(value < -128 ? -128 : value > 127 ? 127 : value);
    }
    rt_mutex_take(&inference_mutex, RT_WAITING_FOREVER);
    int status = edgedoctor_backend_invoke(selected, input, sizeof(input), logits, sizeof(logits));
    result->backend = selected->kind;
    rt_mutex_release(&inference_mutex);
    if (status != 0) return status;
    unsigned best = 0;
    for (unsigned i = 1; i < 5; ++i) if (logits[i] > logits[best]) best = i;
    float probabilities[5], sum = 0;
    for (unsigned i = 0; i < 5; ++i) {
        probabilities[i] = std::exp((logits[i] - logits[best]) * ed_model_output_scale);
        sum += probabilities[i];
    }
    for (unsigned i = 0; i < 5; ++i)
        result->class_scores_q15[i] = static_cast<uint16_t>(std::lround(32767 * probabilities[i] / sum));
    result->predicted_class = best;
    result->confidence_q15 = result->class_scores_q15[best];
    result->anomaly_q15 = 32767 - result->class_scores_q15[0];
    result->vibration_anomaly_q15 = result->anomaly_q15;
    result->audio_anomaly_q15 = EDGEDOCTOR_SCORE_UNAVAILABLE;
    result->status = EDGEDOCTOR_RESULT_OK;
    return 0;
}

extern "C" int ed_inference_parity(void)
{
    if (!selected) return -1;
    unsigned failures = 0;
    rt_mutex_take(&inference_mutex, RT_WAITING_FOREVER);
    for (const auto &backend : backends) {
        if (!backend.available) { ++failures; continue; }
        unsigned maximum_error = 0, mismatches = 0;
        for (unsigned i = 0; i < ed_model_golden_count; ++i) {
            int8_t output[5];
            if (edgedoctor_backend_invoke(&backend, ed_model_golden_inputs[i], 64, output, 5)) {
                ++failures; break;
            }
            rt_kprintf("ednn_csv,%s,%u,%d,%d,%d,%d,%d\n", backend.name, i,
                       output[0], output[1], output[2], output[3], output[4]);
            unsigned actual = 0, expected = 0;
            for (unsigned k = 0; k < 5; ++k) {
                unsigned error = std::abs(output[k] - ed_model_golden_outputs[i][k]);
                if (error > maximum_error) maximum_error = error;
                if (output[k] > output[actual]) actual = k;
                if (ed_model_golden_outputs[i][k] > ed_model_golden_outputs[i][expected]) expected = k;
            }
            mismatches += actual != expected;
        }
        rt_kprintf("ednn parity backend=%s count=%u max_lsb=%u class_mismatch=%u arena=%u\n",
                   backend.name, ed_model_golden_count, maximum_error, mismatches, backend.arena_bytes);
        failures += maximum_error > 1 || mismatches > 0;
    }
    rt_mutex_release(&inference_mutex);
    return failures ? -1 : 0;
}
