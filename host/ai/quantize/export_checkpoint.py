"""Bridge the existing PyTorch training artifact into the isolated TF environment."""
from pathlib import Path
import sys

import numpy as np
import torch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "ai" / "train"))
from train_kaist_vibration import discover_recordings, recording_split


def main() -> None:
    checkpoint = torch.load(ROOT / "artifacts/training/kaist-vibration-v1/model_fp32.pt",
                            map_location="cpu", weights_only=False)
    cache = np.load(ROOT / "data/features/kaist_vibration_40w.npz", allow_pickle=False)
    split = recording_split(discover_recordings(ROOT / "data/raw/kaist-v6/vibration"))
    training = np.asarray([split[str(group)] == "train" for group in cache["groups"]])
    testing = np.asarray([split[str(group)] == "test" for group in cache["groups"]])
    if not training.any() or not testing.any() or np.any(training & testing):
        raise ValueError("Invalid recording-level split")
    values = {key.replace(".", "_"): tensor.detach().cpu().numpy()
              for key, tensor in checkpoint["state_dict"].items()}
    output = ROOT / "artifacts/training/kaist-vibration-v1/tflite-export.npz"
    np.savez_compressed(output, **values, mean=checkpoint["mean"], scale=checkpoint["scale"],
                        calibration=cache["x"][training], evaluation=cache["x"][testing],
                        evaluation_labels=cache["y"][testing])
    print(output)


if __name__ == "__main__":
    main()
