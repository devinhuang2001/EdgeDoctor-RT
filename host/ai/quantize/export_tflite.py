"""Export a trained KAIST checkpoint to a fully integer TFLite model.

Run with the inference environment after export_checkpoint.py. Calibration uses
training recordings only; validation/test windows never set quantization ranges.
This is the vibration model: it makes no audio or synchronization claim.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
import tensorflow as tf


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--parameters", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    data = np.load(args.parameters, allow_pickle=False)
    model = tf.keras.Sequential([
        tf.keras.Input(shape=(64,), batch_size=1),
        tf.keras.layers.Dense(32, activation="relu"),
        tf.keras.layers.Dense(5),
    ])
    # Fold the training-only standardizer into the first affine layer.
    first = data["fc1_weight"] / data["scale"][None, :]
    bias = data["fc1_bias"] - first @ data["mean"]
    model.layers[0].set_weights([first.T, bias])
    model.layers[1].set_weights([data["fc2_weight"].T, data["fc2_bias"]])
    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = lambda: (
        [row[None, :].astype(np.float32)] for row in data["calibration"]
    )
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    binary = converter.convert()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(binary)
    interpreter = tf.lite.Interpreter(model_content=binary)
    interpreter.allocate_tensors()
    inp = interpreter.get_input_details()[0]
    out = interpreter.get_output_details()[0]
    predictions = []
    outputs = []
    quantized_inputs = []
    for row in data["evaluation"]:
        scale, zero = inp["quantization"]
        q = np.clip(np.rint(row / scale) + zero, -128, 127).astype(np.int8)[None, :]
        interpreter.set_tensor(inp["index"], q)
        interpreter.invoke()
        logits = interpreter.get_tensor(out["index"])[0]
        predictions.append(int(np.argmax(logits)))
        outputs.append(logits.copy())
        quantized_inputs.append(q[0])
    fp32 = np.argmax(model(data["evaluation"]).numpy(), axis=1)
    labels = data["evaluation_labels"]
    report = {
        "model": "kaist-vibration-tflite-int8-v1", "modality": "vibration",
        "sha256": hashlib.sha256(binary).hexdigest(), "bytes": len(binary),
        "input_quantization": list(inp["quantization"]),
        "output_quantization": list(out["quantization"]),
        "evaluation_windows": len(labels),
        "fp32_accuracy": float(np.mean(fp32 == labels)),
        "int8_accuracy": float(np.mean(np.asarray(predictions) == labels)),
        "prediction_agreement": float(np.mean(fp32 == predictions)),
        "board_parity": "MISSING_EVIDENCE",
    }
    report["degradation_percentage_points"] = 100 * (report["fp32_accuracy"] - report["int8_accuracy"])
    args.output.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")
    np.savez_compressed(args.output.with_suffix(".golden.npz"),
                        inputs=np.asarray(quantized_inputs), outputs=np.asarray(outputs), labels=labels)
    print(json.dumps(report, indent=2))
    if len(binary) > 300 * 1024 or report["degradation_percentage_points"] > 2:
        raise SystemExit("Model export failed the size/quantization gate")


if __name__ == "__main__":
    main()
