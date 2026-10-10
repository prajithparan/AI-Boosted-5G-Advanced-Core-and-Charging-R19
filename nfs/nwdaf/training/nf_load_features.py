"""NF-load feature construction shared by the centralised trainer and the federated one.

The ONNX model's input contract (nfs/nwdaf/src/model_runtime.hpp kNfLoadFeatureNames) is one contract
with several spellings: this list, the C++ list, and the federated linear model's weight order. They
MUST match. numpy only, so the federated modules and their tests need no sklearn/mlflow.
"""
import numpy as np

LAGS = 4
FEATURE_NAMES = ["load_lag3", "load_lag2", "load_lag1", "load_lag0", "load_mean", "registered_share"]


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
