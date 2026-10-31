"""Reproducible KAIST vibration training and publication-figure pipeline."""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import os
import platform
import random
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from scipy.signal import resample_poly
from sklearn.decomposition import PCA
from sklearn.metrics import (
    ConfusionMatrixDisplay,
    accuracy_score,
    auc,
    classification_report,
    confusion_matrix,
    f1_score,
    precision_recall_curve,
    roc_auc_score,
    roc_curve,
)
from sklearn.preprocessing import label_binarize
import torch
from torch import nn

PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "tools"))

from edgedoctor.kaist import load_vibration_recording  # noqa: E402
from edgedoctor.manifest import LABELS, parse_kaist_name  # noqa: E402
from edgedoctor.preprocess import physical_to_int16, vibration_log_spectrum  # noqa: E402


LABEL_NAMES = ["Normal", "Inner race", "Outer race", "Misalignment", "Unbalance"]
FEATURE_NAMES = [f"Vibration band {index:02d}" for index in range(64)]


@dataclass(frozen=True)
class RecordingMeta:
    path: str
    key: str
    label: int
    fault: str
    load_nm: int
    severity: str


class TinyMLP(nn.Module):
    def __init__(self) -> None:
        super().__init__()
        self.fc1 = nn.Linear(64, 32)
        self.fc2 = nn.Linear(32, 5)

    def forward(self, values: torch.Tensor) -> torch.Tensor:
        return self.fc2(torch.relu(self.fc1(values)))


def seed_everything(seed: int) -> None:
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    torch.use_deterministic_algorithms(True)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def discover_recordings(root: Path) -> list[RecordingMeta]:
    recordings = []
    for path in sorted(root.glob("*.mat")):
        parsed = parse_kaist_name(path)
        recordings.append(
            RecordingMeta(
                path=str(path.resolve()),
                key=parsed.recording_key,
                label=LABELS[parsed.fault],
                fault=parsed.fault,
                load_nm=parsed.load_nm,
                severity=parsed.severity,
            )
        )
    if len(recordings) != 45:
        raise RuntimeError(f"expected 45 KAIST vibration recordings, found {len(recordings)}")
    return recordings


def feature_vector(profile: np.ndarray) -> np.ndarray:
    return np.asarray(profile, dtype=np.float32)


def extract_dataset(
    recordings: list[RecordingMeta], windows_per_recording: int, cache: Path
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    if cache.exists():
        saved = np.load(cache, allow_pickle=False)
        if int(saved["windows_per_recording"]) == windows_per_recording and saved["x"].shape[1] == 64:
            print(f"FEATURE_CACHE hit {cache}", flush=True)
            return saved["x"], saved["y"], saved["groups"], saved["loads"]
    features: list[np.ndarray] = []
    labels: list[int] = []
    groups: list[str] = []
    loads: list[int] = []
    for number, recording in enumerate(recordings, start=1):
        started = time.perf_counter()
        rate, values = load_vibration_recording(recording.path)
        duration_seconds = values.shape[0] / rate
        starts = np.linspace(2.0, duration_seconds - 3.0, windows_per_recording)
        for start_seconds in starts:
            first = int(round(float(start_seconds) * rate))
            window = np.ascontiguousarray(values[first : first + rate, :2])
            downsampled = resample_poly(window, 1, 4, axis=0)
            quantized = physical_to_int16(downsampled, 50.0)
            spectrum = vibration_log_spectrum(quantized, sample_rate=6400)
            profile = np.mean(spectrum, axis=(1, 2))
            minimum = float(profile.min())
            maximum = float(profile.max())
            normalized = (profile - minimum) * (100.0 / max(maximum - minimum, 1e-9))
            features.append(feature_vector(normalized))
            labels.append(recording.label)
            groups.append(recording.key)
            loads.append(recording.load_nm)
        print(
            f"FEATURES {number:02d}/{len(recordings)} {recording.key} "
            f"{time.perf_counter() - started:.2f}s",
            flush=True,
        )
    x = np.stack(features).astype(np.float32)
    y = np.asarray(labels, dtype=np.int64)
    group_array = np.asarray(groups, dtype="U64")
    load_array = np.asarray(loads, dtype=np.int64)
    cache.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(
        cache,
        x=x,
        y=y,
        groups=group_array,
        loads=load_array,
        windows_per_recording=np.asarray(windows_per_recording),
    )
    return x, y, group_array, load_array


def recording_split(recordings: list[RecordingMeta]) -> dict[str, str]:
    result: dict[str, str] = {}
    for label in range(5):
        members = sorted(
            (item for item in recordings if item.label == label),
            key=lambda item: (item.load_nm, item.severity, item.key),
        )
        result[members[0].key] = "validation"
        result[members[1].key] = "test"
        for item in members[2:]:
            result[item.key] = "train"
    return result


def fit_scaler(x: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    mean = x.mean(axis=0)
    scale = x.std(axis=0)
    scale[scale < 1e-6] = 1.0
    return mean.astype(np.float32), scale.astype(np.float32)


def train_model(
    x_train: np.ndarray,
    y_train: np.ndarray,
    x_val: np.ndarray,
    y_val: np.ndarray,
    epochs: int,
    seed: int,
) -> tuple[TinyMLP, list[dict[str, float]]]:
    seed_everything(seed)
    model = TinyMLP()
    counts = np.bincount(y_train, minlength=5).astype(np.float32)
    class_weights = counts.sum() / np.maximum(counts * 5.0, 1.0)
    criterion = nn.CrossEntropyLoss(weight=torch.from_numpy(class_weights))
    optimizer = torch.optim.AdamW(model.parameters(), lr=3e-3, weight_decay=1e-4)
    train_x = torch.from_numpy(x_train)
    train_y = torch.from_numpy(y_train)
    val_x = torch.from_numpy(x_val)
    history: list[dict[str, float]] = []
    best_state: dict[str, torch.Tensor] | None = None
    best_f1 = -1.0
    for epoch in range(1, epochs + 1):
        model.train()
        generator = torch.Generator().manual_seed(seed + epoch)
        order = torch.randperm(len(train_x), generator=generator)
        epoch_loss = 0.0
        for first in range(0, len(order), 64):
            batch = order[first : first + 64]
            optimizer.zero_grad(set_to_none=True)
            logits = model(train_x[batch])
            loss = criterion(logits, train_y[batch])
            loss.backward()
            optimizer.step()
            epoch_loss += float(loss.detach()) * len(batch)
        model.eval()
        with torch.no_grad():
            train_pred = model(train_x).argmax(dim=1).numpy()
            val_logits = model(val_x)
            val_loss = float(criterion(val_logits, torch.from_numpy(y_val)))
            val_pred = val_logits.argmax(dim=1).numpy()
        row = {
            "epoch": float(epoch),
            "train_loss": epoch_loss / len(train_x),
            "val_loss": val_loss,
            "train_accuracy": accuracy_score(y_train, train_pred),
            "val_accuracy": accuracy_score(y_val, val_pred),
            "val_macro_f1": f1_score(y_val, val_pred, average="macro"),
        }
        history.append(row)
        if row["val_macro_f1"] > best_f1:
            best_f1 = row["val_macro_f1"]
            best_state = {name: value.detach().clone() for name, value in model.state_dict().items()}
        if epoch == 1 or epoch % 10 == 0 or epoch == epochs:
            print(
                f"EPOCH {epoch:03d}/{epochs} loss={row['train_loss']:.4f} "
                f"val_f1={row['val_macro_f1']:.4f}",
                flush=True,
            )
    assert best_state is not None
    model.load_state_dict(best_state)
    model.eval()
    return model, history


def probabilities(model: nn.Module, x: np.ndarray) -> np.ndarray:
    with torch.no_grad():
        return torch.softmax(model(torch.from_numpy(x)), dim=1).numpy()


def model_bytes(model: nn.Module) -> int:
    stream = io.BytesIO()
    torch.save(model.state_dict(), stream)
    return len(stream.getvalue())


def evaluate(model: nn.Module, x: np.ndarray, y: np.ndarray) -> dict[str, object]:
    probability = probabilities(model, x)
    prediction = probability.argmax(axis=1)
    binary = label_binarize(y, classes=np.arange(5))
    return {
        "accuracy": float(accuracy_score(y, prediction)),
        "macro_f1": float(f1_score(y, prediction, average="macro")),
        "macro_auc_ovr": float(roc_auc_score(binary, probability, average="macro", multi_class="ovr")),
        "confusion_matrix": confusion_matrix(y, prediction, labels=np.arange(5)).tolist(),
        "classification_report": classification_report(
            y, prediction, labels=np.arange(5), target_names=LABEL_NAMES,
            output_dict=True, zero_division=0
        ),
        "probabilities": probability,
        "predictions": prediction,
    }


def write_history(path: Path, history: list[dict[str, float]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(history[0]))
        writer.writeheader()
        writer.writerows(history)


def plot_style() -> None:
    plt.rcParams.update(
        {
            "font.family": "DejaVu Sans",
            "font.size": 10,
            "axes.spines.top": False,
            "axes.spines.right": False,
            "figure.dpi": 160,
            "savefig.dpi": 240,
        }
    )


def save_figures(
    out: Path,
    history: list[dict[str, float]],
    y_test: np.ndarray,
    probability: np.ndarray,
    prediction: np.ndarray,
    x: np.ndarray,
    y: np.ndarray,
    loads: np.ndarray,
    lolo: dict[str, dict[str, float]],
    fp32_accuracy: float,
    int8_accuracy: float,
) -> None:
    plot_style()
    colors = ["#17A673", "#347FE8", "#E32222", "#E58A12", "#7656B5"]
    epochs = [row["epoch"] for row in history]
    fig, axes = plt.subplots(1, 2, figsize=(10, 4))
    axes[0].plot(epochs, [row["train_loss"] for row in history], label="Train")
    axes[0].plot(epochs, [row["val_loss"] for row in history], label="Validation")
    axes[0].set(xlabel="Epoch", ylabel="Cross-entropy", title="Training loss")
    axes[0].legend(frameon=False)
    axes[1].plot(epochs, [row["train_accuracy"] for row in history], label="Train")
    axes[1].plot(epochs, [row["val_accuracy"] for row in history], label="Validation")
    axes[1].plot(epochs, [row["val_macro_f1"] for row in history], label="Val macro-F1")
    axes[1].set(xlabel="Epoch", ylabel="Score", ylim=(0, 1.02), title="Learning curves")
    axes[1].legend(frameon=False)
    fig.tight_layout()
    fig.savefig(out / "training_curves.png", bbox_inches="tight")
    plt.close(fig)

    fig, axis = plt.subplots(figsize=(6, 5))
    ConfusionMatrixDisplay.from_predictions(
        y_test, prediction, display_labels=LABEL_NAMES, cmap="Blues",
        normalize="true", values_format=".2f", ax=axis, colorbar=False
    )
    axis.set_title("Normalized confusion matrix (held-out recordings)")
    axis.tick_params(axis="x", rotation=30)
    fig.tight_layout()
    fig.savefig(out / "confusion_matrix.png", bbox_inches="tight")
    plt.close(fig)

    binary = label_binarize(y_test, classes=np.arange(5))
    fig, axis = plt.subplots(figsize=(6, 5))
    for index, name in enumerate(LABEL_NAMES):
        fpr, tpr, _ = roc_curve(binary[:, index], probability[:, index])
        axis.plot(fpr, tpr, color=colors[index], label=f"{name} (AUC={auc(fpr, tpr):.3f})")
    axis.plot([0, 1], [0, 1], "--", color="#8B949E", linewidth=1)
    axis.set(xlabel="False-positive rate", ylabel="True-positive rate", title="One-vs-rest ROC")
    axis.legend(frameon=False, fontsize=8)
    fig.tight_layout()
    fig.savefig(out / "roc_curves.png", bbox_inches="tight")
    plt.close(fig)

    fig, axis = plt.subplots(figsize=(6, 5))
    for index, name in enumerate(LABEL_NAMES):
        precision, recall, _ = precision_recall_curve(binary[:, index], probability[:, index])
        axis.plot(recall, precision, color=colors[index], label=f"{name} (AP={auc(recall, precision):.3f})")
    axis.set(xlabel="Recall", ylabel="Precision", title="One-vs-rest precision-recall")
    axis.legend(frameon=False, fontsize=8)
    fig.tight_layout()
    fig.savefig(out / "pr_curves.png", bbox_inches="tight")
    plt.close(fig)

    counts = np.zeros((5, 3), dtype=int)
    for label in range(5):
        for load_index, load in enumerate((0, 2, 4)):
            counts[label, load_index] = int(np.sum((y == label) & (loads == load)))
    fig, axis = plt.subplots(figsize=(8, 4.5))
    bottom = np.zeros(5)
    for load_index, load in enumerate((0, 2, 4)):
        axis.bar(LABEL_NAMES, counts[:, load_index], bottom=bottom,
                 label=f"{load} Nm", color=colors[load_index])
        bottom += counts[:, load_index]
    axis.set(ylabel="One-second windows", title="KAIST vibration training distribution")
    axis.tick_params(axis="x", rotation=25)
    axis.legend(frameon=False, ncol=3)
    fig.tight_layout()
    fig.savefig(out / "dataset_distribution.png", bbox_inches="tight")
    plt.close(fig)

    embedding = PCA(n_components=2, random_state=0).fit_transform(x)
    fig, axis = plt.subplots(figsize=(7, 5))
    for label, name in enumerate(LABEL_NAMES):
        mask = y == label
        axis.scatter(embedding[mask, 0], embedding[mask, 1], s=8, alpha=0.55,
                     color=colors[label], label=name)
    axis.set(xlabel="Principal component 1", ylabel="Principal component 2",
             title="Feature-space projection")
    axis.legend(frameon=False, fontsize=8)
    fig.tight_layout()
    fig.savefig(out / "feature_embedding.png", bbox_inches="tight")
    plt.close(fig)

    fig, axes = plt.subplots(1, 2, figsize=(9, 4))
    names = list(lolo)
    axes[0].bar(names, [lolo[name]["macro_f1"] for name in names], color=colors[:3])
    axes[0].axhline(0.75, color="#E32222", linestyle="--", linewidth=1)
    axes[0].set(ylabel="Macro-F1", ylim=(0, 1.02), title="Leave-one-load-out")
    axes[1].bar(["FP32", "Dynamic INT8"], [fp32_accuracy, int8_accuracy],
                color=["#347FE8", "#17A673"])
    axes[1].set(ylabel="Accuracy", ylim=(0, 1.02), title="Post-training quantization check")
    fig.tight_layout()
    fig.savefig(out / "generalization_quantization.png", bbox_inches="tight")
    plt.close(fig)


def c_float_array(name: str, values: np.ndarray) -> str:
    dimensions = "".join(f"[{size}]" for size in values.shape)
    if values.ndim == 1:
        body = ",\n    ".join(", ".join(f"{value:.9g}f" for value in values[i : i + 8]) for i in range(0, len(values), 8))
    else:
        rows = []
        for row in values:
            cells = ",\n        ".join(", ".join(f"{value:.9g}f" for value in row[i : i + 8]) for i in range(0, len(row), 8))
            rows.append("    {\n        " + cells + "\n    }")
        body = ",\n".join(rows)
    return f"static const float {name}{dimensions} = {{\n{body}\n}};\n"


def c_int8_array(name: str, values: np.ndarray) -> str:
    dimensions = "".join(f"[{size}]" for size in values.shape)
    if values.ndim == 1:
        body = ",\n    ".join(
            ", ".join(str(int(value)) for value in values[i : i + 16])
            for i in range(0, len(values), 16)
        )
    else:
        rows = []
        for row in values:
            cells = ",\n        ".join(
                ", ".join(str(int(value)) for value in row[i : i + 16])
                for i in range(0, len(row), 16)
            )
            rows.append("    {\n        " + cells + "\n    }")
        body = ",\n".join(rows)
    return f"static const int8_t {name}{dimensions} = {{\n{body}\n}};\n"


def c_int32_array(name: str, values: np.ndarray) -> str:
    dimensions = "".join(f"[{size}]" for size in values.shape)
    body = ",\n    ".join(
        ", ".join(str(int(value)) for value in values[i : i + 8])
        for i in range(0, len(values), 8)
    )
    return f"static const int32_t {name}{dimensions} = {{\n    {body}\n}};\n"


def static_int8_parameters(
    model: TinyMLP, mean: np.ndarray, scale: np.ndarray, calibration_x: np.ndarray
) -> dict[str, np.ndarray]:
    """Fold standardization into FC1 and quantize weights per output channel."""
    state = model.state_dict()
    fc1_weight = state["fc1.weight"].numpy().astype(np.float32) / scale[np.newaxis, :]
    fc1_bias = state["fc1.bias"].numpy().astype(np.float32) - fc1_weight @ mean
    fc2_weight = state["fc2.weight"].numpy().astype(np.float32)
    fc2_bias = state["fc2.bias"].numpy().astype(np.float32)

    def quantize_rows(values: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        row_scale = np.max(np.abs(values), axis=1).astype(np.float32) / 127.0
        row_scale[row_scale < 1e-12] = 1.0
        quantized = np.clip(np.rint(values / row_scale[:, np.newaxis]), -127, 127).astype(np.int8)
        return quantized, row_scale

    fc1_q, fc1_scale = quantize_rows(fc1_weight)
    fc2_q, fc2_scale = quantize_rows(fc2_weight)
    calibration_input = np.clip(np.rint(calibration_x), 0, 100).astype(np.int32)
    calibration_accumulator = calibration_input @ fc1_q.astype(np.int32).T
    calibration_hidden = np.maximum(
        calibration_accumulator.astype(np.float32) * fc1_scale[np.newaxis, :] +
        fc1_bias[np.newaxis, :], 0.0,
    )
    hidden_scale = np.asarray(
        [max(float(np.max(calibration_hidden)) / 127.0, 1e-9)], dtype=np.float32
    )
    fc1_multiplier_q15 = np.rint(fc1_scale / hidden_scale[0] * 32768.0).astype(np.int32)
    fc1_bias_q = np.rint(fc1_bias / hidden_scale[0]).astype(np.int32)
    return {
        "fc1_weight": fc1_q,
        "fc1_scale": fc1_scale,
        "fc1_bias": fc1_bias,
        "fc2_weight": fc2_q,
        "fc2_scale": fc2_scale,
        "fc2_bias": fc2_bias,
        "hidden_scale": hidden_scale,
        "fc1_multiplier_q15": fc1_multiplier_q15,
        "fc1_bias_q": fc1_bias_q,
    }


def static_int8_probabilities(parameters: dict[str, np.ndarray], x: np.ndarray) -> np.ndarray:
    input_q = np.clip(np.rint(x), 0, 100).astype(np.int32)
    fc1 = input_q @ parameters["fc1_weight"].astype(np.int32).T
    hidden_q = (
        (fc1 * parameters["fc1_multiplier_q15"][np.newaxis, :] + 16384) >> 15
    ) + parameters["fc1_bias_q"][np.newaxis, :]
    hidden_q = np.clip(hidden_q, 0, 127).astype(np.int32)
    fc2 = hidden_q @ parameters["fc2_weight"].astype(np.int32).T
    logits = (fc2.astype(np.float32) *
              (parameters["hidden_scale"][0] * parameters["fc2_scale"])[np.newaxis, :] +
              parameters["fc2_bias"][np.newaxis, :])
    shifted = logits - logits.max(axis=1, keepdims=True)
    probability = np.exp(shifted)
    return probability / probability.sum(axis=1, keepdims=True)


def export_header(
    path: Path, model: TinyMLP, mean: np.ndarray, scale: np.ndarray, calibration_x: np.ndarray
) -> None:
    parameters = static_int8_parameters(model, mean, scale, calibration_x)
    text = """#ifndef EDGEDOCTOR_ONBOARD_MODEL_PARAMS_H
#define EDGEDOCTOR_ONBOARD_MODEL_PARAMS_H

/* Generated by ai/train/train_kaist_vibration.py from KAIST v6.
 * Input is the canonical 0..100 vibration profile, not LVGL state. */
#include <stdint.h>
#define EDGEDOCTOR_ONBOARD_INPUT 64U
#define EDGEDOCTOR_ONBOARD_HIDDEN 32U

"""
    text += c_int8_array("edgedoctor_model_fc1_weight_q7", parameters["fc1_weight"])
    text += c_int8_array("edgedoctor_model_fc2_weight_q7", parameters["fc2_weight"])
    text += c_float_array("edgedoctor_model_fc2_scale", parameters["fc2_scale"])
    text += c_float_array("edgedoctor_model_fc2_bias", parameters["fc2_bias"])
    text += c_float_array("edgedoctor_model_hidden_scale", parameters["hidden_scale"])
    text += c_int32_array("edgedoctor_model_fc1_multiplier_q15", parameters["fc1_multiplier_q15"])
    text += c_int32_array("edgedoctor_model_fc1_bias_q", parameters["fc1_bias_q"])
    text += "\n#endif\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def export_golden_header(
    path: Path, parameters: dict[str, np.ndarray], x: np.ndarray, y: np.ndarray
) -> None:
    """Export representative integer inputs for host and board self-tests."""
    selected = [int(np.flatnonzero(y == label)[0]) for label in range(5)]
    inputs = np.clip(np.rint(x[selected]), 0, 100).astype(np.int16)
    probability = static_int8_probabilities(parameters, inputs)
    scores = np.rint(probability * 32767.0).astype(np.uint16)
    predicted = probability.argmax(axis=1).astype(np.uint8)
    input_rows = ",\n".join(
        "    {" + ", ".join(str(int(value)) for value in row) + "}"
        for row in inputs
    )
    score_rows = ",\n".join(
        "    {" + ", ".join(str(int(value)) for value in row) + "}"
        for row in scores
    )
    text = f"""#ifndef EDGEDOCTOR_ONBOARD_MODEL_GOLDEN_H
#define EDGEDOCTOR_ONBOARD_MODEL_GOLDEN_H

#include <stdint.h>

#define EDGEDOCTOR_MODEL_GOLDEN_VECTORS {len(selected)}U

static const int16_t edgedoctor_model_golden_input[EDGEDOCTOR_MODEL_GOLDEN_VECTORS][64] = {{
{input_rows}
}};
static const uint8_t edgedoctor_model_golden_class[EDGEDOCTOR_MODEL_GOLDEN_VECTORS] = {{
    {", ".join(str(int(value)) for value in predicted)}
}};
static const uint16_t edgedoctor_model_golden_score_q15[EDGEDOCTOR_MODEL_GOLDEN_VECTORS][5] = {{
{score_rows}
}};

#endif
"""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-root", type=Path, default=PROJECT_ROOT / "data/raw/kaist-v6/vibration")
    parser.add_argument("--out-dir", type=Path, default=PROJECT_ROOT / "artifacts/training/kaist-vibration-v1")
    parser.add_argument("--cache", type=Path, default=PROJECT_ROOT / "data/features/kaist_vibration_40w.npz")
    parser.add_argument("--export-header", type=Path, default=PROJECT_ROOT / "firmware/common/include/edgedoctor_onboard_model_params.h")
    parser.add_argument("--export-golden", type=Path, default=PROJECT_ROOT / "firmware/common/include/edgedoctor_onboard_model_golden.h")
    parser.add_argument("--windows-per-recording", type=int, default=40)
    parser.add_argument("--epochs", type=int, default=100)
    parser.add_argument("--seed", type=int, default=20260901)
    args = parser.parse_args()
    started = time.time()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    recordings = discover_recordings(args.data_root)
    x, y, groups, loads = extract_dataset(recordings, args.windows_per_recording, args.cache)
    split_map = recording_split(recordings)
    splits = np.asarray([split_map[group] for group in groups])
    train_mask = splits == "train"
    val_mask = splits == "validation"
    test_mask = splits == "test"
    mean, scale = fit_scaler(x[train_mask])
    normalized = ((x - mean) / scale).astype(np.float32)
    model, history = train_model(
        normalized[train_mask], y[train_mask], normalized[val_mask], y[val_mask],
        args.epochs, args.seed
    )
    result = evaluate(model, normalized[test_mask], y[test_mask])
    quantized = torch.ao.quantization.quantize_dynamic(model, {nn.Linear}, dtype=torch.qint8)
    quantized_result = evaluate(quantized, normalized[test_mask], y[test_mask])
    static_int8 = static_int8_parameters(model, mean, scale, x[train_mask])
    static_probability = static_int8_probabilities(static_int8, x[test_mask])
    static_prediction = static_probability.argmax(axis=1)
    static_int8_result = {
        "accuracy": float(accuracy_score(y[test_mask], static_prediction)),
        "macro_f1": float(f1_score(y[test_mask], static_prediction, average="macro")),
        "prediction_agreement_with_fp32": float(
            np.mean(static_prediction == result["predictions"])
        ),
        "parameter_bytes": int(
            static_int8["fc1_weight"].nbytes + static_int8["fc2_weight"].nbytes +
            static_int8["fc2_scale"].nbytes + static_int8["fc2_bias"].nbytes +
            static_int8["hidden_scale"].nbytes +
            static_int8["fc1_multiplier_q15"].nbytes + static_int8["fc1_bias_q"].nbytes
        ),
    }

    lolo: dict[str, dict[str, float]] = {}
    for held_load in (0, 2, 4):
        fold_test = loads == held_load
        fold_train = ~fold_test
        fold_groups = sorted(set(groups[fold_train]))
        validation_groups = {group for index, group in enumerate(fold_groups) if index % 7 == 0}
        fold_val = np.asarray([group in validation_groups for group in groups]) & fold_train
        fold_fit = fold_train & ~fold_val
        fold_mean, fold_scale = fit_scaler(x[fold_fit])
        fold_x = ((x - fold_mean) / fold_scale).astype(np.float32)
        fold_model, _ = train_model(
            fold_x[fold_fit], y[fold_fit], fold_x[fold_val], y[fold_val],
            max(50, args.epochs // 2), args.seed + held_load + 1
        )
        fold_result = evaluate(fold_model, fold_x[fold_test], y[fold_test])
        lolo[f"{held_load} Nm"] = {
            "accuracy": float(fold_result["accuracy"]),
            "macro_f1": float(fold_result["macro_f1"]),
            "macro_auc_ovr": float(fold_result["macro_auc_ovr"]),
        }

    torch.save(
        {"state_dict": model.state_dict(), "mean": mean, "scale": scale,
         "labels": LABEL_NAMES, "seed": args.seed},
        args.out_dir / "model_fp32.pt",
    )
    export_header(args.export_header, model, mean, scale, x[train_mask])
    export_golden_header(args.export_golden, static_int8, x[test_mask], y[test_mask])
    write_history(args.out_dir / "history.csv", history)
    with (args.out_dir / "splits.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=["key", "fault", "label", "load_nm", "severity", "split", "path"])
        writer.writeheader()
        for recording in recordings:
            writer.writerow({**asdict(recording), "split": split_map[recording.key]})

    metrics = {
        "dataset": {
            "name": "KAIST rotating machine vibration",
            "doi": "10.17632/ztmf3m7h5x.6",
            "license": "CC BY 4.0",
            "recordings": len(recordings),
            "windows": int(len(x)),
            "windows_per_recording": args.windows_per_recording,
            "vibration_archive_sha256": sha256(args.data_root.parent / "vibration.zip"),
            "acoustic_limitation": "5 zero-load files; not synchronized with vibration; excluded from five-class training",
        },
        "split": {
            "unit": "recording",
            "train_windows": int(train_mask.sum()),
            "validation_windows": int(val_mask.sum()),
            "test_windows": int(test_mask.sum()),
        },
        "model": {
            "architecture": "64-32-5 MLP ReLU on normalized vibration spectrum",
            "fp32_state_bytes": model_bytes(model),
            "dynamic_int8_state_bytes": model_bytes(quantized),
            "static_int8_parameter_bytes": static_int8_result["parameter_bytes"],
            "board_export": str(args.export_header),
        },
        "e1_fp32": {key: value for key, value in result.items() if key not in {"probabilities", "predictions"}},
        "e1_dynamic_int8": {key: value for key, value in quantized_result.items() if key not in {"probabilities", "predictions"}},
        "e1_static_int8": static_int8_result,
        "lolo": lolo,
        "training": {
            "seed": args.seed,
            "epochs": args.epochs,
            "best_validation_macro_f1": max(row["val_macro_f1"] for row in history),
            "elapsed_seconds": time.time() - started,
        },
        "environment": {
            "python": sys.version,
            "platform": platform.platform(),
            "numpy": np.__version__,
            "torch": torch.__version__,
        },
    }
    (args.out_dir / "metrics.json").write_text(
        json.dumps(metrics, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    save_figures(
        args.out_dir, history, y[test_mask], result["probabilities"],
        result["predictions"], normalized, y, loads, lolo,
        float(result["accuracy"]), float(quantized_result["accuracy"])
    )
    print(
        "TRAINING_COMPLETE "
        f"accuracy={result['accuracy']:.4f} macro_f1={result['macro_f1']:.4f} "
        f"auc={result['macro_auc_ovr']:.4f} out={args.out_dir}",
        flush=True,
    )


if __name__ == "__main__":
    main()
