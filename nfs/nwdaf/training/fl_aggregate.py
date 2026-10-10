#!/usr/bin/env python3
"""NWDAF federated-learning SERVER aggregation: sample-count-weighted FedAvg (ADR-0471 increment 4).

TS 23.288 6.2C.2.2 step 5: the FL Server aggregates the interim local models into a global model.
The algorithm is not defined by 3GPP; the project's choice is FedAvg (McMahan et al. 2017): the
global parameters are the average of the clients' parameters weighted by each client's number of
training samples. Only models of type "linear-fl-v1" with the same feature contract aggregate;
anything else is refused, never coerced.

Input file(s): {"model": <linear-fl-v1 model>, "n_samples": <int > 0>}. Output: the global model JSON
plus {"aggregation": {"algorithm": "FedAvg", "n_clients", "total_samples"}}.
"""
import argparse
import json
import sys

from fl_local_round import check_model


def fedavg(updates):
    if not updates:
        raise ValueError("no client updates to aggregate")
    total = 0
    for u in updates:
        check_model(u["model"])
        n = u.get("n_samples")
        if not isinstance(n, int) or isinstance(n, bool) or n <= 0:
            raise ValueError(f"n_samples must be a positive integer, got {n!r}")
        total += n
    first = updates[0]["model"]
    k = len(first["weights"])
    weights = [sum(u["model"]["weights"][i] * u["n_samples"] for u in updates) / total for i in range(k)]
    bias = sum(u["model"]["bias"] * u["n_samples"] for u in updates) / total
    out = {key: first[key] for key in ("model_type", "feature_names", "load_scale")}
    out.update({"weights": weights, "bias": bias,
                "aggregation": {"algorithm": "FedAvg", "n_clients": len(updates),
                                "total_samples": total}})
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--inputs", nargs="+", required=True, help="client update JSON files")
    ap.add_argument("--output", required=True, help="global model JSON to write")
    a = ap.parse_args()
    try:
        out = fedavg([json.load(open(p)) for p in a.inputs])
    except ValueError as e:
        print(f"fl_aggregate: {e}", file=sys.stderr)
        return 2
    json.dump(out, open(a.output, "w"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
