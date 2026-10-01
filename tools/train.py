#!/usr/bin/env python3
"""Train the LearnedPolicy model from an lcache-export CSV (docs/m3-design.md section 2).

    .venv/bin/python tools/train.py build/m3/train.csv --window 5000000 -o build/m3/model.txt

Target: log1p(distance to next access), regression (decisions D12). Rows whose key is not
requested again within the exported window (next_distance = -1) get distance 2 * window.

Validation is a time split inside the training window: the last --valid-frac of evictions.
Besides RMSE it reports what the policy actually needs, the *eviction accuracy*: within each
eviction's candidate set, how often the candidate with the highest prediction is one whose true
next access is the furthest. Sampled LRU (highest age) is printed as the baseline.
"""

import argparse
import sys
import time

import lightgbm as lgb
import numpy as np
import pandas as pd

# Must match FeatureTracker::feature_names(); the C++ predictor checks this again at load time.
FEATURES = (
    ["log_size", "age"]
    + [f"delta_{i}" for i in range(1, 9)]
    + ["count"]
    + [f"edc_{i}" for i in range(1, 5)]
)


def parse_args():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("csv", help="training rows written by lcache-export")
    p.add_argument("-o", "--out", required=True, help="where to write the LightGBM model (text)")
    p.add_argument("--window", type=int, required=True,
                   help="number of requests the export replayed (its -n); sets the never-again cap")
    p.add_argument("--valid-frac", type=float, default=0.2)
    p.add_argument("--num-trees", type=int, default=32)
    p.add_argument("--num-leaves", type=int, default=31)
    p.add_argument("--learning-rate", type=float, default=0.1)
    p.add_argument("--threads", type=int, default=4)
    p.add_argument("--seed", type=int, default=1)
    return p.parse_args()


def load(path):
    dtypes = {"eviction": np.int64, "now": np.int64, "key": np.uint64, "next_distance": np.int64}
    dtypes.update({f: np.float32 for f in FEATURES})
    df = pd.read_csv(path, dtype=dtypes)
    expected = ["eviction", "now", "key"] + FEATURES + ["next_distance"]
    if list(df.columns) != expected:
        sys.exit(f"unexpected columns:\n  got      {list(df.columns)}\n  expected {expected}")
    return df


def eviction_accuracy(eviction, true_dist, score):
    """Fraction of evictions whose top-scored candidate has the largest true distance."""
    df = pd.DataFrame({"eviction": eviction, "true": true_dist, "score": score})
    best_true = df.groupby("eviction")["true"].transform("max")
    chosen = df.loc[df.groupby("eviction")["score"].idxmax()]
    return float((chosen["true"].to_numpy() == best_true.loc[chosen.index].to_numpy()).mean())


def main():
    args = parse_args()
    t0 = time.time()
    df = load(args.csv)
    cap = 2 * args.window
    dist = np.where(df["next_distance"] < 0, cap, df["next_distance"]).astype(np.float64)
    y = np.log1p(dist)
    print(f"rows {len(df):,}  evictions {df['eviction'].nunique():,}  "
          f"never-again {np.mean(df['next_distance'] < 0):.1%}  (loaded in {time.time() - t0:.1f}s)")

    # Time split: validation = the latest evictions (the CSV is written in time order). Splitting
    # on eviction keeps every candidate set whole.
    split = df["eviction"].quantile(1 - args.valid_frac)
    is_train = (df["eviction"] < split).to_numpy()
    X = df[FEATURES]
    train = lgb.Dataset(X[is_train], y[is_train], feature_name=FEATURES, free_raw_data=False)
    valid = lgb.Dataset(X[~is_train], y[~is_train], reference=train)

    params = {
        "objective": "regression",
        "num_leaves": args.num_leaves,
        "learning_rate": args.learning_rate,
        "num_threads": args.threads,
        "seed": args.seed,
        "deterministic": True,
        "force_row_wise": True,
        "verbosity": -1,
    }
    t0 = time.time()
    booster = lgb.train(params, train, num_boost_round=args.num_trees, valid_sets=[valid],
                        valid_names=["valid"])
    print(f"trained {args.num_trees} trees x {args.num_leaves} leaves in {time.time() - t0:.1f}s")

    pred = booster.predict(X[~is_train])
    y_valid = y[~is_train]
    rmse = float(np.sqrt(np.mean((pred - y_valid) ** 2)))
    rmse_const = float(np.sqrt(np.mean((y[is_train].mean() - y_valid) ** 2)))
    v = df[~is_train]
    acc_model = eviction_accuracy(v["eviction"].to_numpy(), dist[~is_train], pred)
    acc_age = eviction_accuracy(v["eviction"].to_numpy(), dist[~is_train], v["age"].to_numpy())

    print(f"validation: {(~is_train).sum():,} rows, {v['eviction'].nunique():,} evictions")
    print(f"  RMSE log1p(distance): model {rmse:.4f}   predict-the-mean {rmse_const:.4f}")
    print(f"  eviction accuracy:    model {acc_model:.2%}   sampled LRU (max age) {acc_age:.2%}")

    gain = booster.feature_importance(importance_type="gain")
    print("feature importance (share of total gain):")
    for name, g in sorted(zip(FEATURES, gain), key=lambda t: -t[1]):
        print(f"  {name:10s} {g / gain.sum():6.1%}")

    booster.save_model(args.out)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
