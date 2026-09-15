# NWDAF MTLF -- training sidecar

ADR-0369. `train_nf_load.py` is what the NWDAF containing MTLF (`nfs/nwdaf`, `role: mtlf`)
runs through its `TrainingExecutor` (`src/training_executor.hpp`) to produce the NF_LOAD
model it provisions over `Nnwdaf_MLModelProvision`. The MTLF assembles the dataset from the
ADRF, invokes this script as a subprocess with `--input/--output/--report`, stores the ONNX it
writes through `Nadrf_MLModelManagement`, and notifies its subscribers. The AnLF loads that
ONNX in-process with ONNX Runtime (`src/model_runtime.cpp`) -- Python never runs on a request
path, and never inside the AnLF.

The module docstring is the specification of what is predicted, how accuracy is measured, and
what the synthetic bootstrap is. Read it before changing `FEATURE_NAMES`: the C++ side carries
the same list and both must change together.

```
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

Point the MTLF at the interpreter with `training.python` in `config/nwdaf.json`
(`NWDAF_TRAINING_PYTHON`). MLflow tracking defaults to a SQLite file next to the workdir
(`training.mlflow_tracking_uri`, `NWDAF_TRAINING_MLFLOW_TRACKING_URI`); an operator can point it
at a self-hosted MLflow server. Every training run is an MLflow run with the data source, the
ADRF data set it came from, the sample counts, the held-out accuracy and the ONNX artifact.

The executor is an interface so training can move off the NF's host (a Kubeflow/Flyte
executor, CLAUDE.md's "swappable backend") without touching the MTLF's service logic.
