#!/usr/bin/env python3
import argparse
import csv
import math
from typing import List


def to_float(value: str):
    if value is None:
        return None
    value = value.strip()
    if value == "":
        return None
    try:
        return float(value)
    except ValueError:
        return None


def percentile(values: List[float], p: float) -> float:
    if not values:
        return float("nan")
    if len(values) == 1:
        return values[0]
    values_sorted = sorted(values)
    pos = (len(values_sorted) - 1) * p
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return values_sorted[lo]
    frac = pos - lo
    return values_sorted[lo] * (1.0 - frac) + values_sorted[hi] * frac


def correlation(xs: List[float], ys: List[float]) -> float:
    if len(xs) != len(ys) or len(xs) < 2:
        return float("nan")
    mx = sum(xs) / len(xs)
    my = sum(ys) / len(ys)
    num = 0.0
    dx2 = 0.0
    dy2 = 0.0
    for x, y in zip(xs, ys):
        dx = x - mx
        dy = y - my
        num += dx * dy
        dx2 += dx * dx
        dy2 += dy * dy
    if dx2 <= 0.0 or dy2 <= 0.0:
        return float("nan")
    return num / math.sqrt(dx2 * dy2)


def fit_linear(xs: List[float], ys: List[float]):
    if len(xs) != len(ys) or len(xs) < 2:
        return float("nan"), float("nan"), float("nan"), []

    mx = sum(xs) / len(xs)
    my = sum(ys) / len(ys)

    num = 0.0
    den = 0.0
    for x, y in zip(xs, ys):
        dx = x - mx
        num += dx * (y - my)
        den += dx * dx

    if den <= 0.0:
        return float("nan"), float("nan"), float("nan"), []

    slope = num / den
    intercept = my - slope * mx
    yhat = [intercept + slope * x for x in xs]

    ss_res = sum((y - yh) ** 2 for y, yh in zip(ys, yhat))
    ss_tot = sum((y - my) ** 2 for y in ys)
    if ss_tot <= 0.0:
        r2 = float("nan")
    else:
        r2 = 1.0 - (ss_res / ss_tot)

    return intercept, slope, r2, yhat


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Plot MT9M114 gain against AtomISP S3A metrics from joined CSV"
    )
    parser.add_argument("--input", "-i", default="joined.csv",
                        help="Input joined CSV from parse_mt9m114_atomisp_logs.py")
    parser.add_argument("--output", "-o", default="gain_vs_s3a.png",
                        help="Output PNG path")
    parser.add_argument("--max-delta-ms", type=float, default=400.0,
                        help="Only use rows with match_delta_ms <= this value")
    parser.add_argument("--invert-timeseries", choices=["auto", "yes", "no"],
                        default="auto",
                        help="Invert scaled s3a_y_hist_mean in time-series (default: auto)")
    parser.add_argument("--min-valid-gain", type=float, default=1.0,
                        help="Discard rows with mt_analog_gain below this value (default: 1)")
    args = parser.parse_args()

    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib is required: install with 'python3 -m pip install matplotlib'")
        return 2

    mt_gain: List[float] = []
    s3a_y_hist_mean: List[float] = []
    s3a_ae_y_avg: List[float] = []
    mt_uptime: List[float] = []
    dropped_quality = 0

    with open(args.input, "r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            delta = to_float(row.get("match_delta_ms", ""))
            if delta is None or delta > args.max_delta_ms:
                continue

            gain = to_float(row.get("mt_analog_gain", ""))
            yh = to_float(row.get("s3a_y_hist_mean", ""))
            ae = to_float(row.get("s3a_ae_y_avg", ""))
            up = to_float(row.get("mt_uptime_ms", ""))
            fps = to_float(row.get("mt_fps", ""))
            exp = to_float(row.get("mt_exp", ""))
            frame_live = to_float(row.get("mt_frame_length_live", ""))
            streaming = to_float(row.get("mt_streaming", ""))

            if gain is None or yh is None or ae is None or up is None:
                continue

            # Drop transient read failures / invalid mixed-state samples.
            if (gain < args.min_valid_gain or
                (fps is not None and fps <= 0.0) or
                (exp is not None and exp <= 0.0) or
                (frame_live is not None and frame_live <= 0.0) or
                (streaming is not None and streaming < 1.0)):
                dropped_quality += 1
                continue

            mt_gain.append(gain)
            s3a_y_hist_mean.append(yh)
            s3a_ae_y_avg.append(ae)
            mt_uptime.append(up)

    if not mt_gain:
        print("No matched rows found with required columns; check input CSV and --max-delta-ms")
        return 1

    corr_gain_hist = correlation(mt_gain, s3a_y_hist_mean)
    corr_gain_ae = correlation(mt_gain, s3a_ae_y_avg)

    fig, axes = plt.subplots(1, 3, figsize=(18, 5), constrained_layout=True)

    sc1 = axes[0].scatter(s3a_y_hist_mean, mt_gain, c=mt_uptime, s=10, cmap="viridis")
    axes[0].set_title(f"mt_analog_gain vs s3a_y_hist_mean\nN={len(mt_gain)} r={corr_gain_hist:.3f}")
    axes[0].set_xlabel("s3a_y_hist_mean")
    axes[0].set_ylabel("mt_analog_gain")
    cb1 = fig.colorbar(sc1, ax=axes[0])
    cb1.set_label("mt_uptime_ms")

    sc2 = axes[1].scatter(s3a_ae_y_avg, mt_gain, c=mt_uptime, s=10, cmap="plasma")
    axes[1].set_title(f"mt_analog_gain vs s3a_ae_y_avg\nN={len(mt_gain)} r={corr_gain_ae:.3f}")
    axes[1].set_xlabel("s3a_ae_y_avg")
    axes[1].set_ylabel("mt_analog_gain")
    cb2 = fig.colorbar(sc2, ax=axes[1])
    cb2.set_label("mt_uptime_ms")

    min_t = min(mt_uptime)
    tsec = [(t - min_t) / 1000.0 for t in mt_uptime]
    axes[2].plot(tsec, mt_gain, label="mt_analog_gain", linewidth=1.0)

    yh_p95 = percentile(s3a_y_hist_mean, 0.95)
    fit_intercept = float("nan")
    fit_slope = float("nan")
    fit_r2 = float("nan")
    invert = False
    if yh_p95 and not math.isnan(yh_p95):
        yh_norm = [v / yh_p95 for v in s3a_y_hist_mean]
        yh_norm = [min(max(v, 0.0), 1.0) for v in yh_norm]

        if args.invert_timeseries == "yes":
            invert = True
        elif args.invert_timeseries == "no":
            invert = False
        else:
            invert = not math.isnan(corr_gain_hist) and corr_gain_hist < 0.0

        if invert:
            yh_norm = [1.0 - v for v in yh_norm]

        fit_intercept, fit_slope, fit_r2, yhat = fit_linear(yh_norm, mt_gain)
        if yhat:
            label = "fit gain from inv(s3a_y_hist_mean)" if invert else "fit gain from s3a_y_hist_mean"
            axes[2].plot(tsec, yhat, label=label, linewidth=1.0)

    axes[2].set_title("Time series")
    axes[2].set_xlabel("time (s, relative)")
    axes[2].set_ylabel("value")
    axes[2].legend(loc="best")

    fig.suptitle("MT9M114 gain vs AtomISP S3A metrics", fontsize=14)
    fig.savefig(args.output, dpi=140)

    print(f"saved plot: {args.output}")
    print(f"rows used: {len(mt_gain)}")
    print(f"rows dropped by quality filter: {dropped_quality}")
    print(f"corr(mt_analog_gain, s3a_y_hist_mean) = {corr_gain_hist:.6f}")
    print(f"corr(mt_analog_gain, s3a_ae_y_avg)   = {corr_gain_ae:.6f}")
    if not math.isnan(fit_slope):
        print(f"fit(gain ~ a + b*x_hist): a={fit_intercept:.6f} b={fit_slope:.6f} r2={fit_r2:.6f}")
        print(f"x_hist uses inversion: {invert}")
    if args.invert_timeseries == "auto":
        print(f"timeseries inversion mode: auto (applied={corr_gain_hist < 0.0 if not math.isnan(corr_gain_hist) else False})")
    else:
        print(f"timeseries inversion mode: forced {args.invert_timeseries}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
