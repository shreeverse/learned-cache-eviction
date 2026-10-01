#!/usr/bin/env python3
"""Regenerate the tiny LightGBM model used by the C++ tests (tests/ml/lightgbm_predictor_test.cpp).

    .venv/bin/python tools/make_test_model.py

Writes tests/data/tiny_model.txt (4 trees, trained on synthetic rows with the real feature names)
and tests/data/tiny_model_cases.csv: feature rows, including NaNs, with the prediction Python's
LightGBM gives for each. The C++ predictor must reproduce those predictions.
"""

from pathlib import Path

import lightgbm as lgb
import numpy as np
import pandas as pd

from train import FEATURES

OUT = Path(__file__).resolve().parent.parent / "tests" / "data"


def synthetic_rows(n, rng):
    x = pd.DataFrame({
        "log_size": rng.uniform(4, 12, n),
        "age": rng.integers(0, 50_000, n),
        "count": rng.integers(1, 20, n),
    })
    for i in range(1, 9):
        x[f"delta_{i}"] = rng.integers(1, 20_000, n).astype(float)
        x.loc[x["count"] <= i, f"delta_{i}"] = np.nan  # a key seen c times has c - 1 gaps
    for i in range(1, 5):
        x[f"edc_{i}"] = rng.uniform(0, 5, n)
    return x[FEATURES].astype(np.float32)


def main():
    rng = np.random.default_rng(7)
    x = synthetic_rows(2000, rng)
    # Any learnable target will do; this one depends on age, a delta that is often NaN, and size.
    y = np.log1p(x["age"] + x["delta_1"].fillna(30_000) + 100 * x["log_size"])
    params = {"objective": "regression", "num_leaves": 7, "learning_rate": 0.3,
              "num_threads": 1, "seed": 1, "deterministic": True, "verbosity": -1}
    booster = lgb.train(params, lgb.Dataset(x, y), num_boost_round=4)

    OUT.mkdir(parents=True, exist_ok=True)
    booster.save_model(OUT / "tiny_model.txt")

    cases = synthetic_rows(16, np.random.default_rng(8))
    cases.iloc[0] = np.nan  # a row with every feature missing
    cases["expected"] = booster.predict(cases)
    # %.9g round-trips float32 features exactly; %.17g round-trips the double predictions.
    with open(OUT / "tiny_model_cases.csv", "w") as f:
        f.write(",".join(FEATURES + ["expected"]) + "\n")
        for row in cases.itertuples(index=False):
            vals = [f"{v:.9g}" for v in row[:-1]] + [f"{row[-1]:.17g}"]
            f.write(",".join(vals) + "\n")
    print(f"wrote {OUT / 'tiny_model.txt'} and {OUT / 'tiny_model_cases.csv'}")


if __name__ == "__main__":
    main()
