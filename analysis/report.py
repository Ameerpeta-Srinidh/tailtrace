"""TailTrace - turn result directories into plots and a markdown summary.

Usage:
    python3 analysis/report.py results/poisson_block results/hawkes_block ... --out results/report

Produces, in --out:
    percentile_spectrum.png  latency vs "how many nines", one line per run
    stages.png               p50 / p99 / p99.9 of every stage, per run
    arrivals.png             packets per 100 us for the first 20 ms of each run
    tail_vs_burst.png        latency vs how many packets arrived just before
    summary.md               the numbers behind the plots
"""
import argparse
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402

STAGES = ["sender_lateness", "send_to_kernel", "kernel_to_app", "batch_wait", "parse"]
BURST_WINDOW_NS = 100_000


def load_kv(path):
    if not os.path.exists(path):
        return {}
    df = pd.read_csv(path)
    return dict(zip(df["key"], df["value"]))


def load_run(d):
    run = {"name": os.path.basename(os.path.normpath(d)), "dir": d}
    run["summary"] = pd.read_csv(os.path.join(d, "summary.csv")).set_index("stage")
    run["recv"] = load_kv(os.path.join(d, "recv_stats.csv"))
    run["gen"] = load_kv(os.path.join(d, "gen_stats.csv"))
    run["hist"] = pd.read_csv(os.path.join(d, "hist_from_schedule.csv"))
    tp = os.path.join(d, "trace.csv")
    run["trace"] = pd.read_csv(tp) if os.path.exists(tp) else None
    return run


def percentile_spectrum(ax, run):
    # HdrHistogram-style plot: x axis is 1/(1-p), so 90%, 99%, 99.9% ... are
    # evenly spaced and the tail gets as much room as the median.
    h = run["hist"]
    counts = h["count"].to_numpy()
    cum = np.cumsum(counts) / counts.sum()
    x = 1.0 / np.clip(1.0 - cum, 1e-7, None)
    ax.step(x, h["high_ns"] / 1000.0, where="post", label=run["name"])


def preceding_arrivals(trace):
    # For each traced packet: how many packets were scheduled in the 100 us
    # before it. Only valid when every packet was traced (--trace-every 1).
    t = trace["sched_ns"].to_numpy()
    lo = np.searchsorted(t, t - BURST_WINDOW_NS, side="left")
    return np.arange(len(t)) - lo


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--out", default="results/report")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    runs = [load_run(d) for d in args.runs]

    # 1. Percentile spectrum of the honest total.
    fig, ax = plt.subplots(figsize=(8, 5))
    for r in runs:
        percentile_spectrum(ax, r)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ticks = [1, 2, 10, 100, 1000, 10000, 100000]
    ax.set_xticks(ticks, ["0%", "50%", "90%", "99%", "99.9%", "99.99%", "99.999%"])
    ax.set_xlabel("percentile")
    ax.set_ylabel("latency from intended send time (us)")
    ax.set_title("Percentile spectrum: from_schedule")
    ax.grid(True, which="both", alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(os.path.join(args.out, "percentile_spectrum.png"), dpi=120)
    plt.close(fig)

    # 2. Per-stage percentiles.
    fig, axes = plt.subplots(1, 3, figsize=(14, 4.5), sharey=True)
    width = 0.8 / len(runs)
    for ax, col in zip(axes, ["p50_ns", "p99_ns", "p99.9_ns"]):
        for i, r in enumerate(runs):
            vals = [r["summary"].loc[s, col] / 1000.0 for s in STAGES]
            ax.bar(np.arange(len(STAGES)) + i * width, vals, width, label=r["name"])
        ax.set_xticks(np.arange(len(STAGES)) + 0.4 - width / 2, STAGES, rotation=30, ha="right")
        ax.set_yscale("log")
        ax.set_title(col.replace("_ns", ""))
        ax.grid(True, axis="y", alpha=0.3)
    axes[0].set_ylabel("us")
    axes[0].legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(os.path.join(args.out, "stages.png"), dpi=120)
    plt.close(fig)

    # 3. Arrivals and 4. tail vs preceding burst, for runs with a full trace.
    # Burst analysis needs every packet, i.e. a run made with --trace-every 1.
    def fully_traced(r):
        tr = r["trace"]
        return tr is not None and len(tr) > 1 and bool((tr["seq"].diff()[1:] == tr["count"].shift()[1:]).all())

    traced = [r for r in runs if fully_traced(r)]
    burst_rows = []
    if traced:
        fig, axes = plt.subplots(len(traced), 1, figsize=(10, 2.2 * len(traced)), sharex=True, squeeze=False)
        for ax, r in zip(axes[:, 0], traced):
            t = r["trace"]["sched_ns"].to_numpy()
            rel = (t - t[0]) / 1000.0
            ax.hist(rel[rel < 20_000], bins=200, color="tab:blue")
            ax.set_ylabel("pkts / 100us")
            ax.set_title(r["name"], fontsize=9)
        axes[-1, 0].set_xlabel("time since start (us)")
        fig.tight_layout()
        fig.savefig(os.path.join(args.out, "arrivals.png"), dpi=120)
        plt.close(fig)

        fig, ax = plt.subplots(figsize=(8, 5))
        for r in traced:
            tr = r["trace"]
            lat = (tr["p1_ns"] - tr["sched_ns"]).to_numpy() / 1000.0
            pre = preceding_arrivals(tr)
            ax.scatter(pre, lat, s=2, alpha=0.3, label=r["name"])
            thr = np.percentile(lat, 99.9)
            tail = lat >= thr
            burst_rows.append(
                {
                    "run": r["name"],
                    "p99.9_us": round(float(thr), 1),
                    "mean_preceding_all": round(float(pre.mean()), 2),
                    "mean_preceding_tail": round(float(pre[tail].mean()), 2),
                    "mean_batch_all": round(float(tr["batch_size"].mean()), 2),
                    "mean_batch_tail": round(float(tr["batch_size"][tail].mean()), 2),
                }
            )
        ax.set_yscale("log")
        ax.set_xlabel(f"packets scheduled in the previous {BURST_WINDOW_NS // 1000} us")
        ax.set_ylabel("latency from intended send time (us)")
        ax.set_title("Are slow packets the ones right after a burst?")
        ax.grid(True, alpha=0.3)
        ax.legend(markerscale=5)
        fig.tight_layout()
        fig.savefig(os.path.join(args.out, "tail_vs_burst.png"), dpi=120)
        plt.close(fig)

    # 5. Markdown summary.
    lines = ["# TailTrace results", ""]
    lines.append("| run | arrivals | pps | dispersion (1 ms) | max pkts in 100 us | lost | kernel drops "
                 "| e2e p99.9 (us) | from_schedule p50 / p99 / p99.9 / max (us) |")
    lines.append("|---|---|---|---|---|---|---|---|---|")
    for r in runs:
        s, g, rv = r["summary"], r["gen"], r["recv"]
        fs = s.loc["from_schedule"]
        lines.append(
            f"| {r['name']} | {g.get('arrivals', '?')} | {g.get('achieved_rate_pps', '?')} "
            f"| {float(g.get('dispersion_1ms', 0)):.2f} | {g.get('max_pkts_in_100us', '?')} "
            f"| {rv.get('seq_lost', '?')} | {rv.get('kernel_socket_drops', '?')} "
            f"| {s.loc['end_to_end', 'p99.9_ns'] / 1000:.1f} "
            f"| {fs['p50_ns'] / 1000:.1f} / {fs['p99_ns'] / 1000:.1f} / {fs['p99.9_ns'] / 1000:.1f} "
            f"/ {fs['max_ns'] / 1000:.1f} |"
        )
    lines += ["", "## Stage breakdown (us, p50 / p99 / p99.9)", ""]
    lines.append("| run | " + " | ".join(STAGES) + " |")
    lines.append("|---|" + "---|" * len(STAGES))
    for r in runs:
        s = r["summary"]
        cells = [
            f"{s.loc[st, 'p50_ns'] / 1000:.1f} / {s.loc[st, 'p99_ns'] / 1000:.1f} / {s.loc[st, 'p99.9_ns'] / 1000:.1f}"
            for st in STAGES
        ]
        lines.append(f"| {r['name']} | " + " | ".join(cells) + " |")
    if burst_rows:
        lines += ["", "## Tail packets vs the traffic just before them", ""]
        keys = list(burst_rows[0].keys())
        lines.append("| " + " | ".join(keys) + " |")
        lines.append("|---" * len(keys) + "|")
        for row in burst_rows:
            lines.append("| " + " | ".join(str(row[k]) for k in keys) + " |")
    with open(os.path.join(args.out, "summary.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
