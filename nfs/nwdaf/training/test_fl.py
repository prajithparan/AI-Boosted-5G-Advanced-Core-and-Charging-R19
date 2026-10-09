#!/usr/bin/env python3
"""Tests for the federated-learning Python side (ADR-0471 increments 3-4). numpy only.

Thresholds come from measurement (2026-10-09, seeded data below): 5 FedAvg rounds of 2 clients with
different load regimes reached 87.0% accuracy (within 10 load points) on the pooled data at
lr=0.1/200 epochs, against 92.5% for centralised training and 4.9% for the zero model. The asserts
leave margin below the measured value and claim no parity with centralised training.
"""
import random
import unittest

from fl_aggregate import fedavg
from fl_local_round import accuracy_pct, local_round, mse, zero_model
from nf_load_features import FEATURE_NAMES, windows_from_series

TOL, EPOCHS, LR = 10.0, 200, 0.1


def series(seed, base, n=10, steps=120):
    rng = random.Random(seed)
    out = {}
    for k in range(n):
        b = base + rng.uniform(-8, 8)
        load, obs = b, []
        for _ in range(steps):
            load = max(0.0, min(100.0, b + 0.8 * (load - b) + rng.gauss(0, 5)))
            obs.append({"load": round(load), "status": "REGISTERED"})
        out[f"i{seed}-{k}"] = obs
    return out


CLIENT_A, CLIENT_B = series(1, 25), series(2, 70, n=6)


def model(weights, bias):
    m = zero_model()
    m["weights"], m["bias"] = list(weights), bias
    return m


class LocalRound(unittest.TestCase):
    def test_a_round_reduces_loss_and_beats_the_zero_model(self):
        x, y, _ = windows_from_series(CLIENT_A)
        m, rep = local_round(CLIENT_A, None, EPOCHS, LR, TOL)
        self.assertLess(rep["loss_after"], rep["loss_before"])
        self.assertGreater(accuracy_pct(m, x, y, TOL), accuracy_pct(zero_model(), x, y, TOL))
        self.assertEqual(rep["n_samples"], len(y))

    def test_global_accuracy_is_reported_only_when_a_global_model_is_supplied(self):
        _, rep = local_round(CLIENT_A, None, 5, LR, TOL)
        self.assertIsNone(rep["accuracy_global_pct"])  # mLAccChkFlg has nothing to measure
        g, _ = local_round(CLIENT_B, None, EPOCHS, LR, TOL)
        _, rep = local_round(CLIENT_A, g, 5, LR, TOL)
        self.assertIsInstance(rep["accuracy_global_pct"], float)

    def test_a_round_starts_from_the_global_model(self):
        g = model([0.1] * 6, 0.05)
        m, _ = local_round(CLIENT_A, g, 0, LR, TOL)  # zero epochs: unchanged
        self.assertEqual(m["weights"], g["weights"])
        self.assertEqual(m["bias"], g["bias"])

    def test_same_input_gives_the_same_model(self):
        self.assertEqual(local_round(CLIENT_A, None, 20, LR, TOL)[0],
                         local_round(CLIENT_A, None, 20, LR, TOL)[0])

    def test_a_client_with_no_usable_window_gets_an_error_not_a_synthetic_model(self):
        with self.assertRaises(ValueError):
            local_round({"x": [{"load": 5}] * 3}, None, 5, LR, TOL)
        with self.assertRaises(ValueError):
            local_round({}, None, 5, LR, TOL)

    def test_divergence_is_refused_instead_of_emitting_non_finite_weights(self):
        with self.assertRaises(ValueError) as cm:
            local_round({**CLIENT_A, **CLIENT_B}, None, 50, 0.5, TOL)  # measured: diverges
        self.assertIn("diverged", str(cm.exception))

    def test_a_foreign_model_is_refused(self):
        bad = zero_model()
        bad["feature_names"] = list(reversed(FEATURE_NAMES))
        with self.assertRaises(ValueError):
            local_round(CLIENT_A, bad, 5, LR, TOL)


class FedAvg(unittest.TestCase):
    def test_weighted_mean_of_the_parameters(self):
        out = fedavg([{"model": model([1, 2, 3, 4, 5, 6], 1.0), "n_samples": 1},
                      {"model": model([5, 6, 7, 8, 9, 10], 5.0), "n_samples": 3}])
        self.assertEqual(out["weights"], [4.0, 5.0, 6.0, 7.0, 8.0, 9.0])  # (1*a + 3*b) / 4
        self.assertEqual(out["bias"], 4.0)
        self.assertEqual(out["aggregation"], {"algorithm": "FedAvg", "n_clients": 2, "total_samples": 4})

    def test_a_single_client_is_returned_unchanged(self):
        m = model([1, 2, 3, 4, 5, 6], 2.0)
        out = fedavg([{"model": m, "n_samples": 7}])
        self.assertEqual(out["weights"], m["weights"])
        self.assertEqual(out["bias"], m["bias"])

    def test_invalid_updates_are_refused(self):
        ok = model([0] * 6, 0.0)
        for n in (0, -1, 1.5, True, None):
            with self.assertRaises(ValueError, msg=f"n_samples={n!r}"):
                fedavg([{"model": ok, "n_samples": n}])
        with self.assertRaises(ValueError):
            fedavg([])
        foreign = model([0] * 6, 0.0)
        foreign["model_type"] = "random-forest"
        with self.assertRaises(ValueError):
            fedavg([{"model": ok, "n_samples": 1}, {"model": foreign, "n_samples": 1}])

    def test_five_rounds_of_two_uneven_clients_learn_the_pooled_data(self):
        pool = {**CLIENT_A, **CLIENT_B}
        x, y, _ = windows_from_series(pool)
        g = None
        for _ in range(5):
            ups = []
            for data in (CLIENT_A, CLIENT_B):
                m, rep = local_round(data, g, EPOCHS, LR, TOL)
                ups.append({"model": m, "n_samples": rep["n_samples"]})
            g = fedavg(ups)
        acc = accuracy_pct(g, x, y, TOL)
        self.assertGreaterEqual(acc, 80.0, f"measured 87.0 on 2026-10-09, got {acc:.1f}")
        self.assertLess(mse(g, x, y), mse(zero_model(), x, y) / 10)


if __name__ == "__main__":
    unittest.main()
