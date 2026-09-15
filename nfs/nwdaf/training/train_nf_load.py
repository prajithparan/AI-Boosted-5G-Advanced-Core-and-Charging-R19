#!/usr/bin/env python3
"""NWDAF MTLF training sidecar -- NF load prediction (TS 23.288 6.5, ADR-0369).

This is the ONLY place training happens for the NWDAF (CLAUDE.md: training is Python, offline
relative to the request path; inference is in-process C++ in the AnLF through ONNX Runtime,
nfs/nwdaf/src/model_runtime.cpp). The MTLF (nfs/nwdaf/src/mtlf.cpp) invokes this script through
its TrainingExecutor with a dataset file it assembled from the ADRF (Nadrf_DataManagement
retrieval of the NRF NF-status data the AnLF collected, TS 23.288 6.2E.2 step 8) and reads the
report file back; it never imports Python and the AnLF never calls it.

What the model predicts
-----------------------
For one NF instance, the NEXT observed `load` (the NFProfile `load` the NRF reports, 0..100) from
its last LAGS observed loads, their mean, and the share of those observations in which the
instance was REGISTERED. One-step-ahead: the horizon is one observation interval (the AnLF's
collection cadence). The AnLF reports that forecast as the predicted NF load of the requested
future period (TS 23.288 Table 6.5.3-2) with the model's held-out accuracy as `confidence`.
This is disclosed in ADR-0369 as a one-step forecast, not a multi-horizon model.

Accuracy (TS 23.288 5C.1)
-------------------------
"The accuracy value is computed as the number of correct predictions divided by the total number
of predictions"; how a prediction counts as correct is "up to implementation" (NOTE 3). Here: a
prediction is correct when it lands within --accuracy-tolerance load points of the observed value.
The same rule is applied by the AnLF at inference time (ADR-0370, accuracy monitoring), so the
number the MTLF reports as accMLModel and the number the AnLF later measures are comparable.

Cold start
----------
Below --min-samples usable windows the script trains on a clearly labelled SYNTHETIC bootstrap
series (AR(1) around a per-instance baseline with Gaussian noise) so the train -> ONNX -> ADRF ->
AnLF chain is provably functional before enough real observations exist. The data source used is
written to the report (`data_source`), tagged in MLflow, and carried in the provisioning
notification's trainInpInfos.dataStatisticsInfos -- never silently blended.

Input dataset file (written by the MTLF):
  {"event": "NF_LOAD",
   "series": {"<nfInstanceId>": [{"load": 30, "status": "REGISTERED"}, ...], ...},
   "source": {"kind": "adrf", "adrf_id": "...", "data_set_id": "...", "records": N}}
Output: --output <model.onnx>, --report <report.json>.
"""

import argparse
import datetime
import json
import os
import random
import sys

import mlflow
import numpy as np
from sklearn.ensemble import RandomForestRegressor
from sklearn.metrics import mean_absolute_error
from sklearn.model_selection import train_test_split

# The ONNX model's input contract. nfs/nwdaf/src/model_runtime.hpp's kNfLoadFeatureNames MUST
# match this list exactly -- it is one contract with two spellings, not two decisions.
LAGS = 4
FEATURE_NAMES = ["load_lag3", "load_lag2", "load_lag1", "load_lag0", "load_mean", "registered_share"]
MODEL_TYPE = "RandomForestRegressor(n_estimators=30, max_depth=6)"


def windows_from_series(series):
    """Sliding windows over each instance's ordered observations. A window needs LAGS + 1
    consecutive observations that all carry a load; a DEREGISTERED observation carries none and
    breaks the run (an instance that left the NRF has no load to learn from)."""
    xs, ys, instances = [], [], 0
    for _, obs in series.items():
        used = False
        for i in range(LAGS, len(obs)):
            win = obs[i - LAGS:i + 1]
            if any(o.get("load") is None for o in win):
                continue
            loads = [float(o["load"]) for o in win[:-1]]
            registered = sum(1 for o in win[:-1] if o.get("status", "REGISTERED") == "REGISTERED")
            xs.append(loads + [sum(loads) / LAGS, registered / LAGS])
            ys.append(float(win[-1]["load"]))
            used = True
        instances += 1 if used else 0
    return np.array(xs, dtype=np.float64), np.array(ys, dtype=np.float64), instances


def synthetic_bootstrap(seed=42, instances=8, steps=200):
    """AR(1) load around a per-instance baseline, clipped to the NRF's 0..100 scale. A simple,
    documented generative rule -- enough to make the pipeline real, never a claim about real
    load behaviour."""
    rng = random.Random(seed)
    series = {}
    for n in range(instances):
        base = rng.uniform(15, 75)
        load = base
        obs = []
        for _ in range(steps):
            load = base + 0.8 * (load - base) + rng.gauss(0, 5)
            load = max(0.0, min(100.0, load))
            obs.append({"load": round(load), "status": "REGISTERED"})
        series[f"synthetic-{n}"] = obs
    return series


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--input", required=True, help="dataset JSON written by the MTLF")
    parser.add_argument("--output", required=True, help="ONNX model path to write")
    parser.add_argument("--report", required=True, help="training report JSON path to write")
    parser.add_argument("--min-samples", type=int, default=200,
                        help="usable windows below which the synthetic bootstrap is used")
    parser.add_argument("--accuracy-tolerance", type=float, default=10.0,
                        help="load points within which a prediction counts as correct (5C.1)")
    parser.add_argument("--mlflow-tracking-uri", default=os.environ.get(
        "MLFLOW_TRACKING_URI", "sqlite:///" + os.path.abspath("./mlflow.db")))
    parser.add_argument("--experiment", default="nwdaf-nf-load")
    args = parser.parse_args()

    with open(args.input) as f:
        dataset = json.load(f)
    event = dataset.get("event", "NF_LOAD")
    source = dataset.get("source", {})
    X, y, n_instances = windows_from_series(dataset.get("series", {}))
    data_source = source.get("kind", "unknown")
    real_windows = int(len(X))
    if len(X) < args.min_samples:
        print(f"[train_nf_load] {len(X)} usable windows from {n_instances} instances "
              f"(need >= {args.min_samples}); training on the SYNTHETIC bootstrap series instead. "
              f"Re-trains automatically once the ADRF holds enough observations.",
              file=sys.stderr)
        X, y, n_instances = windows_from_series(synthetic_bootstrap())
        data_source = "synthetic_bootstrap"
    else:
        print(f"[train_nf_load] training on {len(X)} windows from {n_instances} instances "
              f"({data_source})")

    X_train, X_test, y_train, y_test = train_test_split(X, y, test_size=0.2, random_state=42)
    model = RandomForestRegressor(n_estimators=30, max_depth=6, random_state=42)
    model.fit(X_train, y_train)
    pred = model.predict(X_test)
    mae = float(mean_absolute_error(y_test, pred))
    correct = int(np.sum(np.abs(pred - y_test) <= args.accuracy_tolerance))
    accuracy_pct = int(round(100.0 * correct / max(1, len(y_test))))
    print(f"[train_nf_load] held-out MAE {mae:.2f} load points; accuracy {accuracy_pct}% "
          f"({correct}/{len(y_test)} within {args.accuracy_tolerance})")

    mlflow.set_tracking_uri(args.mlflow_tracking_uri)
    mlflow.set_experiment(args.experiment)
    trained_at = datetime.datetime.now(datetime.timezone.utc)
    with mlflow.start_run(run_name=f"{event.lower()}-{trained_at:%Y%m%dT%H%M%SZ}") as run:
        mlflow.set_tag("event", event)
        mlflow.set_tag("data_source", data_source)
        for k, v in source.items():
            mlflow.set_tag(f"source_{k}", str(v))
        mlflow.log_param("n_examples", int(len(X)))
        mlflow.log_param("n_real_windows", real_windows)
        mlflow.log_param("n_instances", n_instances)
        mlflow.log_param("feature_names", ",".join(FEATURE_NAMES))
        mlflow.log_param("model_type", MODEL_TYPE)
        mlflow.log_param("accuracy_tolerance", args.accuracy_tolerance)
        mlflow.log_metric("test_mae_load", mae)
        mlflow.log_metric("test_accuracy_pct", accuracy_pct)
        run_id = run.info.run_id
        from skl2onnx import to_onnx
        onnx_model = to_onnx(model, X_train.astype(np.float32), target_opset=17)
        with open(args.output, "wb") as f:
            f.write(onnx_model.SerializeToString())
        mlflow.log_artifact(args.output)

    report = {
        "event": event,
        "model_type": MODEL_TYPE,
        "feature_names": FEATURE_NAMES,
        "n_examples": int(len(X)),
        "n_real_windows": real_windows,
        "n_instances": n_instances,
        "data_source": data_source,
        "source": source,
        "accuracy_pct": accuracy_pct,
        "accuracy_tolerance": args.accuracy_tolerance,
        "test_mae_load": mae,
        "mlflow_run_id": run_id,
        "mlflow_tracking_uri": args.mlflow_tracking_uri,
        "onnx_bytes": os.path.getsize(args.output),
        "trained_at": trained_at.strftime("%Y-%m-%dT%H:%M:%SZ"),
    }
    with open(args.report, "w") as f:
        json.dump(report, f, indent=2)
    print(f"[train_nf_load] wrote {args.output} (MLflow run {run_id}, data_source={data_source})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
