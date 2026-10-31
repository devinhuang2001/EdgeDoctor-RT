# Data pipeline

The formal benchmark requires synchronized acoustic and housing-A x/y
vibration recordings from one experiment. Splitting occurs before windowing.

```powershell
$env:PYTHONPATH = "$PWD\tools"
python tools\validate_dataset.py --root data\raw\kaist-v6 --out data\manifests\recordings.csv
python tools\build_split_manifest.py --input data\manifests\recordings.csv --out-dir data\manifests
python tools\convert_edgepack.py --manifest data\manifests\recordings.csv --out-dir data\edgepack
```

The validator refuses an ambiguous sample rate, an unknown fault label, a
missing paired modality, duplicate recording keys, differing Test.Lab absolute
acquisition times, or a SHA-256 mismatch. The pinned KAIST v6 archives fail this
gate: their same-name acoustic and vibration files were recorded on different
dates, and acoustic covers only five zero-load conditions. This is source-data
evidence, not an implementation failure, and must not be bypassed for formal
acceptance. The
split command isolates complete recording IDs and creates E1 plus all three
leave-one-load-out folds. EdgePack contains resampled `int16` time-domain data;
features are always computed by the consumer.
