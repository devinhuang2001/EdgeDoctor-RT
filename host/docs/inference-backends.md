# Inference backend implementation

The deployed input remains the 64-band vibration profile, independently of the
208 Hz live monitor. No synchronized acoustic training set exists in the current
project. Audio scores use `EDGEDOCTOR_SCORE_UNAVAILABLE` (65535, outside Q15);
neither the legacy reference nor the new backend represents audio fusion.

## Reproduction

Run from the EdgeDoctor-RT directory:

```powershell
python ai/quantize/export_checkpoint.py
.venv-inference/Scripts/python ai/quantize/export_tflite.py --parameters artifacts/training/kaist-vibration-v1/tflite-export.npz --output artifacts/training/kaist-vibration-v1/model_int8.tflite
.venv-inference/Scripts/vela artifacts/training/kaist-vibration-v1/model_int8.tflite --accelerator-config ethos-u55-128 --optimise Performance --show-cpu-operations --output-dir artifacts/training/kaist-vibration-v1/vela
.venv-inference/Scripts/python ai/quantize/embed_models.py --model artifacts/training/kaist-vibration-v1/model_int8.tflite --vela artifacts/training/kaist-vibration-v1/vela/model_int8_vela.tflite --output firmware/m55/inference/ed_inference_models.cpp
```

The isolated inference environment uses `ai/quantize/requirements.txt`. The
checkpoint bridge runs in the existing PyTorch training environment. Model
normalization is folded into the first fully connected layer. Quantization
calibration uses training recordings only. Two hundred held-out windows are
embedded for on-board parity, not for calibration.

The exported model is 4,824 bytes. The held-out FP32 and INT8 accuracies were
0.965 and 0.970, with prediction agreement 0.995. This is a public vibration
dataset result, not field accuracy or acoustic accuracy. Vela reported two NPU
operators and zero CPU operators; its latency estimate uses default memory
settings and must not be reported as measured board performance.

The CPU backend invokes TFLite Micro's CMSIS-NN fully connected kernel. The
U55 backend invokes the Vela custom operator through the official SDK driver.
Each interpreter has a fixed 24 KiB arena, and invocation is serialized. NPU
interrupt/semaphore/cache hooks are implemented in the application port. The
optional XiaoZhi wake-word model remains excluded.

On the board, `edgeai backend cmsis` and `edgeai backend u55` require the named
backend to be available; they cannot silently select another backend.
`edgeai parity` compares both against the same 200 TFLite golden inputs and
prints maximum integer error, class mismatches, and actual arena usage. Missing
backends, invocation failures, errors above one LSB or class disagreement fail.

Build, programming, and on-board parity remain pending until their raw logs
are produced. The old hand-quantized MLP uses a different quantization scheme;
its class-level accuracy is not proof of bit parity with TFLite.
