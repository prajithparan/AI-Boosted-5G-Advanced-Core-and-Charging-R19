#!/usr/bin/env python3
"""NWDAF federated-learning CLIENT training step: one local round (ADR-0471 increment 3).

TS 23.288 6.2C.2.2: an FL Client NWDAF trains a local model on its own data, starting from the global
model the FL Server supplies, and reports the interim local model. 3GPP does not define the model
encoding or the aggregation (ADR-0471, "open design issue"); this is the project's choice, labelled
as such: a LINEAR model over the six NF-load features, trained by full-batch gradient descent. Its
parameters can be averaged (nfs/nwdaf/training/fl_aggregate.py, sample-weighted FedAvg); the
RandomForest of train_nf_load.py cannot, so that stays the model for non-FL events.

Model (JSON, "linear-fl-v1"): {"model_type", "feature_names", "weights"[6], "bias", "load_scale"}.
prediction = (w . x_scaled + bias) * load_scale, where x_scaled divides the five load-valued
features by load_scale and leaves registered_share (already 0..1). The scaling is part of the
contract: clients with different scaling would average to nonsense.

Real data only: unlike train_nf_load.py there is NO synthetic bootstrap. A client with no usable
window reports an error rather than a model, because a synthetic client in a federation would
silently poison the aggregate.

Output report: n_samples, accuracy_global_pct (the supplied global model on THIS client's data, the
mLAccChkFlg measurement, null when no global model was supplied), accuracy_local_pct (after the
round), loss before/after, epochs, learning_rate.
"""
import argparse
import datetime
import json
import sys

import numpy as np

from nf_load_features import FEATURE_NAMES, windows_from_series

MODEL_TYPE = "linear-fl-v1"
LOAD_SCALE = 100.0  # the NRF's load is 0..100


def zero_model():
    return {"model_type": MODEL_TYPE, "feature_names": list(FEATURE_NAMES),
            "weights": [0.0] * len(FEATURE_NAMES), "bias": 0.0, "load_scale": LOAD_SCALE}


def check_model(model):
    if model.get("model_type") != MODEL_TYPE:
        raise ValueError(f"model_type {model.get('model_type')!r} is not {MODEL_TYPE!r}")
    if model.get("feature_names") != FEATURE_NAMES:
        raise ValueError("feature_names differ from this client's feature contract")
    if len(model.get("weights", [])) != len(FEATURE_NAMES):
        raise ValueError("weights length differs from the feature count")
    if model.get("load_scale") != LOAD_SCALE:
        raise ValueError("load_scale differs from this client's")


def scale(x):
    xs = np.array(x, dtype=np.float64, copy=True)
    xs[:, :5] /= LOAD_SCALE
    return xs


def predict(model, x):
    return (scale(x) @ np.array(model["weights"]) + model["bias"]) * model["load_scale"]


def accuracy_pct(model, x, y, tolerance):
    """TS 23.288 5C.1 accuracy: correct predictions / predictions; 'correct' is up to implementation
    (NOTE 3) -- here within `tolerance` load points, the same rule as train_nf_load.py."""
    return 100.0 * float(np.mean(np.abs(predict(model, x) - y) <= tolerance))


def mse(model, x, y):
    return float(np.mean(((predict(model, x) - y) / LOAD_SCALE) ** 2))


def local_round(series, global_model, epochs, learning_rate, tolerance):
    x, y, instances = windows_from_series(series)
    if len(y) == 0:
        raise ValueError("no usable window in the local data (a window needs 5 consecutive loads)")
    if global_model is not None:
        check_model(global_model)
    model = json.loads(json.dumps(global_model)) if global_model is not None else zero_model()
    xs, ys = scale(x), y / LOAD_SCALE
    w, b = np.array(model["weights"], dtype=np.float64), float(model["bias"])
    loss_before = mse(model, x, y)
    acc_global = accuracy_pct(global_model, x, y, tolerance) if global_model is not None else None
    for _ in range(epochs):
        err = xs @ w + b - ys
        w -= learning_rate * (2.0 / len(ys)) * (xs.T @ err)
        b -= learning_rate * (2.0 / len(ys)) * float(np.sum(err))
        if not (np.all(np.isfinite(w)) and np.isfinite(b)):
            raise ValueError("training diverged (non-finite parameters); lower the learning rate")
    model["weights"], model["bias"] = [float(v) for v in w], float(b)
    if mse(model, x, y) > max(loss_before, 1e-12) * 10:
        raise ValueError("training diverged (loss grew more than tenfold); lower the learning rate")
    report = {"n_samples": int(len(y)), "n_instances": int(instances),
              "accuracy_global_pct": acc_global,
              "accuracy_local_pct": accuracy_pct(model, x, y, tolerance),
              "loss_before": loss_before, "loss_after": mse(model, x, y),
              "epochs": epochs, "learning_rate": learning_rate, "accuracy_tolerance": tolerance,
              "model_type": MODEL_TYPE, "data_source": "local",
              "trained_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")}
    return model, report


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--input", required=True, help="dataset JSON written by the MTLF (series)")
    ap.add_argument("--global-model", help="global model JSON from the FL Server; zeros if absent")
    ap.add_argument("--output", required=True, help="local model JSON to write")
    ap.add_argument("--report", required=True, help="round report JSON to write")
    ap.add_argument("--epochs", type=int, required=True)
    ap.add_argument("--learning-rate", type=float, required=True)
    ap.add_argument("--accuracy-tolerance", type=float, required=True)
    a = ap.parse_args()
    dataset = json.load(open(a.input))
    global_model = json.load(open(a.global_model)) if a.global_model else None
    try:
        model, report = local_round(dataset.get("series", {}), global_model, a.epochs,
                                    a.learning_rate, a.accuracy_tolerance)
    except ValueError as e:
        print(f"fl_local_round: {e}", file=sys.stderr)
        return 2
    json.dump(model, open(a.output, "w"), indent=1)
    json.dump(report, open(a.report, "w"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
