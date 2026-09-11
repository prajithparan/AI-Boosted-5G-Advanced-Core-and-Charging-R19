#!/usr/bin/env python3
"""P4.8 (CHARGING_PROMPT.md Angle 1a): predictive quota sizing training sidecar.

CLAUDE.md's mandated pattern: training happens here (Python), never at runtime -- CHF's own
C++ inference wrapper (nfs/chf/src/ai_inference.cpp) only ever loads the ONNX artifact this
script produces and calls it in-process. Nothing in this file runs inside CHF.

What this model predicts, and why: the real, honest training target is "how much will this
SUPI+ratingGroup actually consume in its next charging-request window" (a real regression
problem over TS 32.291 fields CHF already writes to Apache Doris -- ADR-0192,
nfs/chf/schema.doris.sql), NOT an invented "correct multiplier" label -- no such
label exists anywhere in this project's real data. CHF's own deterministic rule
(build_rating_grant, ADR-0074) is what turns a predicted
usage figure into a bounded grant-size multiplier; this script never decides a grant, only
predicts a usage quantity. "This model informs the decision. The deterministic rating engine
makes it." (CHARGING_PROMPT.md Section B, the line that must not be crossed.)

Feature vector, in this EXACT order (nfs/chf/src/ai_inference.hpp's real
kQuotaSizingFeatureCount / kQuotaSizingFeatureNames documents the same order on the C++ side --
change one, change both, or the ONNX model silently reads garbage):
  0: avg_used_last3           -- mean of used_total_volume (octets) over up to the last 3
                                  usage-bearing CDR rows for this SUPI+ratingGroup
  1: velocity                 -- most-recent used_total_volume minus the one before it (octets;
                                  can be negative -- usage slowing down is a real, useful signal)
  2: inter_invocation_interval_sec -- seconds since the previous invocation for this
                                  SUPI+ratingGroup (real invocation_time_stamp delta)
  3: prior_granted_total_volume -- granted_total_volume (octets) of the previous CDR row for
                                  this SUPI+ratingGroup -- the baseline the deterministic clamp
                                  measures its multiplier against

Real, disclosed cold-start gap: this is a freshly-built lab environment with no real production
usage history. If real Doris CDR data does not clear MIN_REAL_EXAMPLES, this script falls
back to a clearly-labeled SYNTHETIC bootstrap dataset (a simple, documented generative rule with
noise) rather than training on too few real rows to mean anything, or refusing to produce a model
at all. The data source actually used is logged to MLflow as a tag (data_source=real_cdr |
synthetic_bootstrap) and printed to stdout -- never silently blended or hidden. Re-run this
script once real CDR volume accumulates to replace the bootstrap model with a real one.
"""

import argparse
import datetime
import os
import random
import sys

import mlflow
import numpy as np
from sklearn.ensemble import RandomForestRegressor
from sklearn.metrics import mean_absolute_error
from sklearn.model_selection import train_test_split

FEATURE_NAMES = [
    "avg_used_last3",
    "velocity",
    "inter_invocation_interval_sec",
    "prior_granted_total_volume",
]
# ADR-0336: raised from 20 after measuring what "20" actually admits.
#
# The lab had 63 CDR rows, 16 distinct subscribers and 24 usage-bearing rows -- which CLEARS a
# threshold of 20. The next run would have trained on 24 examples spread over 16 subscribers and
# tagged the result `data_source=real_cdr`: technically true, materially misleading. A model fit to
# 24 points generalises to nothing and would very likely be WORSE than the documented synthetic
# bootstrap it replaced, while carrying a label that reads as trustworthy.
#
# A threshold's job is not to be cleared. It is to refuse until there is enough data to mean
# something, and 20 was low enough to make "real" the easier label to earn than the honest one.
#
# 2000 examples over 200 distinct subscribers is the bar now. Both are checked: 2000 rows from 5
# subscribers is a model of five people, not of a subscriber base, and row count alone cannot see
# that.
MIN_REAL_EXAMPLES = 2000
MIN_REAL_SUBSCRIBERS = 200


def fetch_real_examples(doris_host: str, doris_port: int, database: str,
                         user: str, password: str) -> tuple[np.ndarray, np.ndarray, int]:
    """Real Doris query (ADR-0192) against nfs/chf/schema.doris.sql's own `cdr` table,
    over Doris's real MySQL wire protocol.

    Builds one (features, target) pair per usage-bearing row that has at least one prior
    usage-bearing row for the same (subscriber_identifier, rating_group) -- the real sliding
    window this project's own schema supports, nothing invented beyond it.
    """
    import pymysql

    conn = pymysql.connect(
        host=doris_host, port=doris_port, database=database,
        user=user, password=password,
    )
    try:
        with conn.cursor() as cursor:
            cursor.execute(
                """
                SELECT subscriber_identifier, rating_group, used_total_volume,
                       granted_total_volume, invocation_time_stamp
                FROM cdr
                WHERE used_total_volume IS NOT NULL AND rating_group IS NOT NULL
                ORDER BY subscriber_identifier, rating_group, invocation_time_stamp
                """
            )
            rows = cursor.fetchall()
    finally:
        conn.close()

    sequences: dict[tuple[str, int], list[tuple]] = {}
    for supi, rating_group, used, granted, ts in rows:
        sequences.setdefault((supi, rating_group), []).append((used, granted, ts))

    features: list[list[float]] = []
    targets: list[float] = []
    for seq in sequences.values():
        for k in range(1, len(seq)):
            history = seq[max(0, k - 3):k]
            used_history = [h[0] for h in history]
            avg_used_last3 = float(np.mean(used_history))
            velocity = float(history[-1][0] - history[-2][0]) if len(history) >= 2 else 0.0
            interval_sec = (seq[k][2] - seq[k - 1][2]).total_seconds()
            prior_granted = float(seq[k - 1][1] or 0)
            features.append([avg_used_last3, velocity, interval_sec, prior_granted])
            targets.append(float(seq[k][0]))

    # ADR-0336: the distinct-subscriber count travels with the data. Row count alone cannot
    # distinguish a real subscriber base from one very chatty test SIM, and a model trained on the
    # latter learns that SIM rather than the network.
    distinct_subscribers = len({supi for (supi, _rg) in sequences})
    return (np.array(features, dtype=np.float64),
            np.array(targets, dtype=np.float64),
            distinct_subscribers)


def synthetic_bootstrap_examples(n: int = 200, seed: int = 42) -> tuple[np.ndarray, np.ndarray]:
    """Clearly-labeled SYNTHETIC data -- a documented generative rule, not real learned behavior.

    Rule: next usage tracks the recent average with a mild trend continuation plus noise, loosely
    bounded by the prior grant (a real subscriber rarely reports usage wildly beyond what was
    actually granted). This exists ONLY to prove the training->ONNX->C++-inference pipeline is
    real and functional before real CDR volume exists -- not a claim about real subscriber
    behavior.
    """
    # Real, disclosed bug found and fixed via live end-to-end testing (ADR-0074): the first version
    # of this function drew avg_used_last3/prior_granted from [1e6, 5e8] octets (1-500MB) -- far
    # below a realistic GB-scale base grant (build_rating_grant's own real "GB" unit conversion
    # produces totalVolume=1,000,000,000 for a 1GB price). A RandomForestRegressor given an input
    # far outside its training range doesn't extrapolate meaningfully -- it just returns something
    # near its training data's own output ceiling, which live-verification caught (a real 1GB
    # scenario produced a materially-shrunk grant that traced back to this scale mismatch, not a
    # genuine learned signal). Ranges now span realistic mobile-data grant sizes (roughly 10MB to
    # 10GB), so a real 1GB base grant sits inside the trained distribution instead of being
    # extrapolated from.
    rng = random.Random(seed)
    features: list[list[float]] = []
    targets: list[float] = []
    for _ in range(n):
        avg_used_last3 = rng.uniform(1e7, 1e10)
        velocity = rng.uniform(-2e8, 2e8)
        interval_sec = rng.uniform(30, 900)
        prior_granted = avg_used_last3 * rng.uniform(0.8, 1.5)
        trend = avg_used_last3 + velocity * 0.5
        noise = rng.gauss(0, avg_used_last3 * 0.1)
        target = max(0.0, min(trend + noise, prior_granted * 1.8))
        features.append([avg_used_last3, velocity, interval_sec, prior_granted])
        targets.append(target)
    return np.array(features, dtype=np.float64), np.array(targets, dtype=np.float64)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--doris-host", default=os.environ.get("CHF_DORIS_HOST", "127.0.0.1"))
    parser.add_argument("--doris-port", type=int, default=9030)
    parser.add_argument("--doris-database", default=os.environ.get("CHF_DORIS_DATABASE", "chf_cdr"))
    parser.add_argument("--doris-user", default=os.environ.get("CHF_DORIS_USER", "root"))
    parser.add_argument("--doris-password", default=os.environ.get("CHF_DORIS_PASSWORD", ""))
    # Real finding from live testing: MLflow 3.x's filesystem tracking store ("file://./mlruns")
    # is now in maintenance mode and refuses new writes (MlflowException pointing at
    # `mlflow migrate-filestore`) -- a local SQLite backend is the real, still-fully-local
    # replacement, not a hosted-server requirement.
    parser.add_argument("--mlflow-tracking-uri", default=os.environ.get(
        "MLFLOW_TRACKING_URI", "sqlite:///" + os.path.abspath("./mlflow.db")))
    parser.add_argument("--output", default="quota_sizing.onnx",
                         help="Output ONNX model path (CHF_QUOTA_MODEL_PATH should point here)")
    args = parser.parse_args()

    data_source = "real_cdr"
    real_subscribers = 0
    try:
        X, y, real_subscribers = fetch_real_examples(args.doris_host, args.doris_port,
                                                     args.doris_database, args.doris_user,
                                                     args.doris_password)
    except Exception as exc:  # real, disclosed: Doris unreachable is a real possible state
        print(f"[train_quota_sizing] real Doris query failed ({exc}); "
              f"falling back to synthetic bootstrap data", file=sys.stderr)
        X, y = np.empty((0, 4)), np.empty((0,))

    # ADR-0336: BOTH gates. Enough examples AND enough distinct subscribers -- see
    # MIN_REAL_EXAMPLES' own comment for the measurement that prompted raising these.
    if len(X) < MIN_REAL_EXAMPLES or real_subscribers < MIN_REAL_SUBSCRIBERS:
        print(f"[train_quota_sizing] real data is too thin to train on: "
              f"{len(X)} examples (need >= {MIN_REAL_EXAMPLES}) from "
              f"{real_subscribers} subscribers (need >= {MIN_REAL_SUBSCRIBERS}). "
              f"Using SYNTHETIC bootstrap data instead -- a model fitted to this little real data "
              f"would carry a trustworthy label and generalise to nothing. "
              f"Re-run once real CDR volume accumulates.", file=sys.stderr)
        X, y = synthetic_bootstrap_examples()
        data_source = "synthetic_bootstrap"
    else:
        print(f"[train_quota_sizing] training on {len(X)} REAL CDR-derived examples")

    X_train, X_test, y_train, y_test = train_test_split(X, y, test_size=0.2, random_state=42)

    model = RandomForestRegressor(n_estimators=20, max_depth=4, random_state=42)
    model.fit(X_train, y_train)
    mae = mean_absolute_error(y_test, model.predict(X_test))
    print(f"[train_quota_sizing] test MAE: {mae:.2f} octets (data_source={data_source}, "
          f"n_examples={len(X)})")

    mlflow.set_tracking_uri(args.mlflow_tracking_uri)
    mlflow.set_experiment("chf-quota-sizing")
    run_name = f"quota-sizing-{datetime.datetime.now(datetime.timezone.utc):%Y%m%dT%H%M%SZ}"
    with mlflow.start_run(run_name=run_name) as run:
        mlflow.set_tag("data_source", data_source)
        mlflow.log_param("n_examples", len(X))
        mlflow.log_param("feature_names", ",".join(FEATURE_NAMES))
        mlflow.log_param("model_type", "RandomForestRegressor(n_estimators=20, max_depth=4)")
        mlflow.log_metric("test_mae_octets", mae)
        model_version = run.info.run_id

        from skl2onnx import to_onnx
        onnx_model = to_onnx(model, X_train.astype(np.float32), target_opset=17)
        with open(args.output, "wb") as f:
            f.write(onnx_model.SerializeToString())
        mlflow.log_artifact(args.output)
        print(f"[train_quota_sizing] wrote {args.output} "
              f"(MLflow run {model_version}, data_source={data_source})")

    # CHF's inference wrapper reads this file to know which MLflow run produced the model it
    # loaded, for governance logging (ai_advisory.model_version) -- see ai_inference.cpp.
    with open(args.output + ".version", "w") as f:
        f.write(model_version)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
