#!/usr/bin/env python3
"""M3 figures for docs/m3-results.md, drawn only from measured outputs (no typed-in numbers).

    .venv/bin/python tools/plot_m3.py

Reads the CSVs written by tools/m3_*.sh and the eviction_bench JSON, writes PNGs to docs/img/.
Colors: the dataviz reference palette's categorical slots 1-4 in their fixed order (blue,
orange, aqua, yellow); Belady and other references are gray dashed lines, not series colors.
"""

import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import pandas as pd  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
M3 = ROOT / "build" / "m3"
OUT = ROOT / "docs" / "img"

# Light-mode roles from the reference palette.
SURFACE, INK, INK2, MUTED, GRID, AXIS = "#fcfcfb", "#0b0b0b", "#52514e", "#898781", "#e1e0d9", "#c3c2b7"
SERIES = {  # fixed order: the entity keeps its color in every chart
    "learned": "#2a78d6",
    "wtinylfu": "#eb6834",
    "lru": "#1baf7a",
    "random-sampled": "#eda100",
}
LABEL = {"learned": "learned (128×63)", "wtinylfu": "W-TinyLFU", "lru": "LRU",
         "random-sampled": "random-sampled", "belady": "Belady"}

plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "font.family": "sans-serif", "font.size": 10, "text.color": INK,
    "axes.edgecolor": AXIS, "axes.labelcolor": INK2, "xtick.color": MUTED, "ytick.color": MUTED,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6,
    "axes.spines.top": False, "axes.spines.right": False, "legend.frameon": False,
})


def rate(path, policy=None, cache_size=None):
    df = pd.read_csv(path)
    if policy is not None:
        df = df[df.policy == policy]
    if cache_size is not None:
        df = df[df.cache_size == cache_size]
    if len(df) != 1:
        raise SystemExit(f"{path}: expected one row for {policy} {cache_size}, got {len(df)}")
    return 100 * float(df.hit_rate.iloc[0])


def size_label(nbytes):
    for unit, n in (("GiB", 2**30), ("MiB", 2**20), ("KiB", 2**10)):
        if nbytes >= n:
            v = nbytes / n
            return f"{v:g} {unit}"
    return f"{nbytes} B"


def hit_rate_by_size():
    """Rows: (trace, cache size in bytes, policy, hit rate %)."""
    rows = []
    tw = [("512KiB", 2**19), ("1MiB", 2**20), ("2MiB", 2**21), ("4MiB", 2**22)]
    for s, b in tw:
        if s == "1MiB":
            base = M3 / "mw" / "test_baselines.csv"
            learned = M3 / "mw" / "test_big.csv"
        else:
            base = M3 / "sizes" / f"test_baselines_{s}.csv"
            learned = M3 / "sizes" / f"test_retrained_128x63_{s}.csv"
        for p in ("lru", "wtinylfu", "random-sampled", "belady"):
            rows.append(("Twitter cluster26 (test split)", b, p, rate(base, p, b)))
        rows.append(("Twitter cluster26 (test split)", b, "learned", rate(learned)))
    for s, b in [("64MiB", 2**26), ("256MiB", 2**28), ("1GiB", 2**30)]:
        base = M3 / "cloudphysics" / f"test_baselines_{s}.csv"
        for p in ("lru", "wtinylfu", "random-sampled", "belady"):
            rows.append(("cloudPhysicsIO (second half)", b, p, rate(base, p, b)))
        rows.append(("cloudPhysicsIO (second half)", b, "learned",
                     rate(M3 / "cloudphysics" / f"test_learned_128x63_{s}.csv")))
    return pd.DataFrame(rows, columns=["trace", "bytes", "policy", "hit_rate"])


def fig_hit_rate_vs_size():
    df = hit_rate_by_size()
    traces = list(dict.fromkeys(df.trace))
    fig, axes = plt.subplots(1, len(traces), figsize=(10, 4.2), sharey=True)
    for ax, trace in zip(axes, traces):
        d = df[df.trace == trace]
        sizes = sorted(d.bytes.unique())
        x = range(len(sizes))
        b = d[d.policy == "belady"].set_index("bytes").loc[sizes, "hit_rate"]
        ax.plot(x, b.values, color=MUTED, linestyle="--", linewidth=1.5, label=LABEL["belady"])
        for p in ("lru", "wtinylfu", "random-sampled", "learned"):
            y = d[d.policy == p].set_index("bytes").loc[sizes, "hit_rate"]
            ax.plot(x, y.values, color=SERIES[p], linewidth=2, marker="o", markersize=6,
                    markeredgecolor=SURFACE, markeredgewidth=1.5, label=LABEL[p], zorder=3)
        # Direct labels on the two lines the chart is about; values in text ink.
        last = len(sizes) - 1
        ends = {p: d[(d.policy == p) & (d.bytes == sizes[last])].hit_rate.iloc[0]
                for p in ("learned", "wtinylfu")}
        gap = 7  # points; push the two labels apart when their values are close
        hi = max(ends, key=ends.get)
        close = abs(ends["learned"] - ends["wtinylfu"]) < 5
        for p, v in ends.items():
            dy = (gap if p == hi else -gap) if close else 0
            ax.annotate(f"{LABEL[p]} {v:.1f}%", (last, v), xytext=(7, dy), textcoords="offset points",
                        va="center", fontsize=8.5, color=INK2)
        ax.set_xticks(list(x), [size_label(s) for s in sizes])
        ax.set_xlim(-0.3, last + 1.4)
        ax.set_title(trace, fontsize=10.5, color=INK, loc="left")
        ax.set_xlabel("cache size")
    axes[0].set_ylabel("hit rate (%)")
    axes[0].set_ylim(0, 100)
    handles, labels = axes[0].get_legend_handles_labels()
    order = [labels.index(LABEL[p]) for p in ("learned", "wtinylfu", "lru", "random-sampled", "belady")]
    fig.legend([handles[i] for i in order], [labels[i] for i in order], loc="upper center",
               ncol=5, bbox_to_anchor=(0.5, 1.0), fontsize=9)
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    fig.savefig(OUT / "m3_hit_rate_vs_size.png", dpi=150)
    df.to_csv(OUT / "m3_hit_rate_vs_size.csv", index=False)  # the table view of the chart


def fig_latency_tradeoff(bench_json):
    bench = {b["run_name"]: b for b in json.load(open(bench_json))["benchmarks"]
             if b.get("aggregate_name") == "median"}

    def us(name):
        b = bench[name]
        scale = {"ns": 1e-3, "us": 1.0, "ms": 1e3}[b["time_unit"]]
        return b["real_time"] * scale

    rows = []
    # Both are "learned" (blue, as in the other figures); the model size is told apart by line
    # style and marker fill rather than a new hue.
    for m in ("128x63", "32x31"):
        for k in (16, 32, 64):
            rows.append((f"learned {m}", k, us(f"evict/learned_{m}_compiled/k:{k}"),
                         rate(M3 / "tradeoff" / f"dev_{m}_k{k}.csv"), m))
    dev_base = M3 / "mw" / "dev_baselines.csv"
    refs = [("LRU", us("evict/lru"), rate(dev_base, "lru")),
            ("W-TinyLFU", us("evict/wtinylfu"), rate(dev_base, "wtinylfu"))]
    for k in (16, 32):
        refs.append((f"random k={k}", us(f"evict/random-sampled/k:{k}"),
                     rate(M3 / "tradeoff" / f"dev_random_k{k}.csv")))
    refs.append(("random k=64", us("evict/random-sampled/k:64"), rate(dev_base, "random-sampled")))

    fig, ax = plt.subplots(figsize=(7.5, 4.6))
    for name, x, y in refs:
        ax.plot(x, y, "o", color=MUTED, markersize=6)
        below = name.startswith("random")  # three close points: label under each, centered
        ax.annotate(name.replace("random ", "") if below else name, (x, y),
                    xytext=(0, -13) if below else (6, -3), textcoords="offset points",
                    ha="center" if below else "left", fontsize=8.5, color=INK2)
    rx = [x for n, x, _ in refs if n.startswith("random")]
    ry = [y for n, _, y in refs if n.startswith("random")]
    ax.annotate("random-sampled", (rx[0], max(ry)), xytext=(0, 9), textcoords="offset points",
                fontsize=8.5, color=INK2)
    for series in dict.fromkeys(r[0] for r in rows):
        pts = [r for r in rows if r[0] == series]
        big = pts[0][4] == "128x63"
        ax.plot([p[2] for p in pts], [p[3] for p in pts], color=SERIES["learned"], linewidth=2,
                linestyle="-" if big else "--", marker="o", markersize=7,
                markerfacecolor=SERIES["learned"] if big else SURFACE,
                markeredgecolor=SURFACE if big else SERIES["learned"], markeredgewidth=1.5,
                label=series.replace("x", " × "), zorder=3)
        for _, k, x, y, _c in pts:
            ax.annotate(f"k={k}", (x, y), xytext=(6, -10), textcoords="offset points", fontsize=8.5,
                        color=INK2)
    ax.set_xscale("log")
    ax.set_xlabel("time per eviction, µs (median, log scale; compiled predictor)")
    ax.set_ylabel("hit rate on dev split (%)")
    ax.set_title("Twitter cluster26, 1 MiB: latency vs hit rate", fontsize=10.5, loc="left")
    ax.legend(loc="lower right", fontsize=9, handlelength=4)
    fig.tight_layout()
    fig.savefig(OUT / "m3_latency_tradeoff.png", dpi=150)
    pd.DataFrame([(r[0], r[1], r[2], r[3]) for r in rows] + [(n, "", x, y) for n, x, y in refs],
                 columns=["policy", "k", "us_per_eviction", "dev_hit_rate"]).to_csv(
        OUT / "m3_latency_tradeoff.csv", index=False)


def fig_diagnosis():
    """Same split for every bar: hit rate on the last 5M requests, 1 MiB."""
    mw = M3 / "mw"
    steps = [
        ("v1: LRU-collected data", rate(M3 / "twitter_learned_1MiB.csv", "learned")),
        ("+ memory window (D17)", rate(mw / "eval_mw1_h10000.csv")),
        ("+ window, + on-policy round", rate(mw / "eval_mw2_h10000.csv")),
        ("+ window, + random-cache data (D18)", rate(mw / "eval_rnd.csv")),
    ]
    base = M3 / "twitter_baselines.csv"
    refs = [("LRU", rate(base, "lru", 2**20)), ("W-TinyLFU", rate(base, "wtinylfu", 2**20)),
            ("random-sampled", rate(M3 / "twitter_learned_1MiB.csv", "random-sampled"))]
    fig, ax = plt.subplots(figsize=(7.5, 3.4))
    names = [s[0] for s in steps][::-1]
    vals = [s[1] for s in steps][::-1]
    ax.barh(names, vals, color=SERIES["learned"], height=0.55, zorder=3)
    for i, v in enumerate(vals):
        ax.annotate(f"{v:.1f}%", (v, i), xytext=(4, 0), textcoords="offset points", va="center",
                    fontsize=9, color=INK)
    ax.set_ylim(-0.6, len(steps) + 0.3)  # headroom above the bars for the reference labels
    for i, (name, v) in enumerate(refs):
        ax.axvline(v, color=MUTED, linestyle="--", linewidth=1.2, zorder=2)
        ax.annotate(f"{name} {v:.1f}%", (v, len(steps) - 0.45 + 0.28 * i), xytext=(3, 0),
                    textcoords="offset points", fontsize=8, color=INK2, va="center",
                    bbox={"boxstyle": "square,pad=0.15", "fc": SURFACE, "ec": "none"}, zorder=4)
    ax.set_xlim(0, 85)
    ax.set_xlabel("hit rate on requests 5M–10M (%), 32 × 31 model")
    ax.set_title("Diagnosing the first learned model (Twitter, 1 MiB)", fontsize=10.5, loc="left")
    ax.grid(axis="y", visible=False)
    fig.tight_layout()
    fig.savefig(OUT / "m3_diagnosis.png", dpi=150)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    fig_hit_rate_vs_size()
    fig_latency_tradeoff(M3 / "bench_final.json")
    fig_diagnosis()
    print(f"wrote figures to {OUT}")


if __name__ == "__main__":
    main()
