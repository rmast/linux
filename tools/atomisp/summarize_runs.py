#!/usr/bin/env python3
import argparse
import csv
import fnmatch
import glob
import math
import os
import statistics
from typing import Dict, List, Optional, Tuple


def to_float(value: str) -> Optional[float]:
    if value is None:
        return None
    value = value.strip()
    if value == "":
        return None
    try:
        return float(value)
    except ValueError:
        return None


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


def fit_linear(xs: List[float], ys: List[float]) -> Tuple[float, float, float]:
    if len(xs) != len(ys) or len(xs) < 2:
        return float("nan"), float("nan"), float("nan")

    mx = sum(xs) / len(xs)
    my = sum(ys) / len(ys)

    num = 0.0
    den = 0.0
    for x, y in zip(xs, ys):
        dx = x - mx
        num += dx * (y - my)
        den += dx * dx

    if den <= 0.0:
        return float("nan"), float("nan"), float("nan")

    slope = num / den
    intercept = my - slope * mx

    yhat = [intercept + slope * x for x in xs]
    ss_res = sum((y - yh) ** 2 for y, yh in zip(ys, yhat))
    ss_tot = sum((y - my) ** 2 for y in ys)
    if ss_tot <= 0.0:
        r2 = float("nan")
    else:
        r2 = 1.0 - ss_res / ss_tot

    return intercept, slope, r2


def safe_median(values: List[float]) -> float:
    vals = [v for v in values if not math.isnan(v)]
    if not vals:
        return float("nan")
    return statistics.median(vals)


def safe_std(values: List[float]) -> float:
    vals = [v for v in values if not math.isnan(v)]
    if len(vals) < 2:
        return float("nan")
    return statistics.pstdev(vals)


def parse_run_path(path: str) -> Tuple[str, str, str]:
    parts = path.split(os.sep)
    label = "?"
    fps = "native"
    rep = "?"

    for i, part in enumerate(parts):
        if fnmatch.fnmatch(part, "rep_*"):
            rep = part
            if i >= 1:
                prev = parts[i - 1]
                if fnmatch.fnmatch(prev, "fps_*"):
                    fps = prev.replace("fps_", "")
                    if i >= 2:
                        label = parts[i - 2]
                else:
                    label = prev
            break

    return label, fps, rep


def summarize_file(path: str, max_delta_ms: float) -> Dict[str, object]:
    gains: List[float] = []
    yhist: List[float] = []
    ae_y: List[float] = []
    exps: List[float] = []
    fps_vals: List[float] = []
    deltas: List[float] = []
    dropped_quality = 0

    with open(path, "r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            delta = to_float(row.get("match_delta_ms", ""))
            if delta is None or delta > max_delta_ms:
                continue

            gain = to_float(row.get("mt_analog_gain", ""))
            yhm = to_float(row.get("s3a_y_hist_mean", ""))
            aey = to_float(row.get("s3a_ae_y_avg", ""))
            fps = to_float(row.get("mt_fps", ""))
            exp = to_float(row.get("mt_exp", ""))
            frame_live = to_float(row.get("mt_frame_length_live", ""))
            streaming = to_float(row.get("mt_streaming", ""))
            if gain is None or yhm is None or aey is None:
                continue

            if (gain < 1.0 or
                (fps is not None and fps <= 0.0) or
                (exp is not None and exp <= 0.0) or
                (frame_live is not None and frame_live <= 0.0) or
                (streaming is not None and streaming < 1.0)):
                dropped_quality += 1
                continue

            gains.append(gain)
            yhist.append(yhm)
            ae_y.append(aey)
            if exp is not None:
                exps.append(exp)
            if fps is not None:
                fps_vals.append(fps)
            deltas.append(delta)

    out: Dict[str, object] = {
        "file": path,
        "rows": len(gains),
        "dropped_quality": dropped_quality,
        "gain_std": safe_std(gains),
        "exp_std": safe_std(exps),
        "fps_std": safe_std(fps_vals),
        "yhist_std": safe_std(yhist),
        "delta_median": statistics.median(deltas) if deltas else float("nan"),
    }

    if len(gains) < 2:
        out.update({
            "corr_gain_yhist": float("nan"),
            "corr_gain_aey": float("nan"),
            "fit_intercept": float("nan"),
            "fit_slope": float("nan"),
            "fit_r2": float("nan"),
            "fit_inverted": False,
        })
        return out

    corr_hist = correlation(gains, yhist)
    corr_ae = correlation(gains, ae_y)

    invert = not math.isnan(corr_hist) and corr_hist < 0.0
    yh_max = max(yhist) if yhist else 0.0
    if yh_max > 0.0:
        x_raw = [v / yh_max for v in yhist]
    else:
        x_raw = [0.0 for _ in yhist]

    if invert:
        x_fit = [1.0 - v for v in x_raw]
    else:
        x_fit = x_raw

    fit_a, fit_b, fit_r2 = fit_linear(x_fit, gains)

    out.update({
        "corr_gain_yhist": corr_hist,
        "corr_gain_aey": corr_ae,
        "fit_intercept": fit_a,
        "fit_slope": fit_b,
        "fit_r2": fit_r2,
        "fit_inverted": invert,
    })
    return out


def is_ae_active(summary: Dict[str, object], min_rows: int, gain_std_min: float, exp_std_min: float) -> bool:
    rows = int(summary.get("rows", 0))
    if rows < min_rows:
        return False

    gain_std = float(summary.get("gain_std", float("nan")))
    exp_std = float(summary.get("exp_std", float("nan")))

    return (
        (not math.isnan(gain_std) and gain_std >= gain_std_min) or
        (not math.isnan(exp_std) and exp_std >= exp_std_min)
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Summarize fitted gain-vs-S3A behavior across automated run repeats"
    )
    parser.add_argument("--root", "-r", required=True,
                        help="Root output dir from automate_lowlight_runs.sh")
    parser.add_argument("--max-delta-ms", type=float, default=400.0,
                        help="Use only rows with match_delta_ms <= this value")
    parser.add_argument("--gain-std-min", type=float, default=8.0,
                        help="Minimum gain std-dev to call run AE-active (default: 8)")
    parser.add_argument("--exp-std-min", type=float, default=30.0,
                        help="Minimum exposure std-dev to call run AE-active (default: 30)")
    parser.add_argument("--min-rows", type=int, default=80,
                        help="Minimum valid rows for run quality assessment (default: 80)")
    parser.add_argument("--out-csv", default="",
                        help="Optional per-file summary CSV path")
    args = parser.parse_args()

    pattern = os.path.join(args.root, "**", "joined.csv")
    files = sorted(p for p in glob.glob(pattern, recursive=True) if "rep_" in p)
    if not files:
        print(f"No joined.csv files found under pattern: {pattern}")
        return 1

    summaries = [summarize_file(path, args.max_delta_ms) for path in files]

    print("Per-run summary")
    print("label,fps,repeat,rows,dropped_quality,ae_active,gain_std,exp_std,corr_gain_yhist,corr_gain_aey,fit_inverted,fit_a,fit_b,fit_r2,median_delta_ms")

    per_fps: Dict[str, List[Dict[str, object]]] = {}
    for s in summaries:
        label, fps, rep = parse_run_path(str(s["file"]))
        active = is_ae_active(s, args.min_rows, args.gain_std_min, args.exp_std_min)
        s["label"] = label
        s["fps"] = fps
        s["rep"] = rep
        s["ae_active"] = active
        per_fps.setdefault(fps, []).append(s)

        print(
            f"{label},{fps},{rep},{s['rows']},{s['dropped_quality']},{int(active)},"
            f"{s['gain_std']:.3f},{s['exp_std']:.3f},{s['corr_gain_yhist']:.6f},{s['corr_gain_aey']:.6f},"
            f"{int(bool(s['fit_inverted']))},{s['fit_intercept']:.6f},{s['fit_slope']:.6f},{s['fit_r2']:.6f},{s['delta_median']:.3f}"
        )

    agg_corr_hist = safe_median([float(s["corr_gain_yhist"]) for s in summaries])
    agg_corr_ae = safe_median([float(s["corr_gain_aey"]) for s in summaries])
    agg_fit_a = safe_median([float(s["fit_intercept"]) for s in summaries])
    agg_fit_b = safe_median([float(s["fit_slope"]) for s in summaries])
    agg_fit_r2 = safe_median([float(s["fit_r2"]) for s in summaries])
    agg_delta = safe_median([float(s["delta_median"]) for s in summaries])

    inv_count = sum(1 for s in summaries if bool(s["fit_inverted"]))

    print("\nAggregate (median across runs)")
    print(f"runs={len(summaries)}")
    print(f"median corr(gain,y_hist)={agg_corr_hist:.6f}")
    print(f"median corr(gain,ae_y_avg)={agg_corr_ae:.6f}")
    print(f"median fit: gain ~= a + b*x_hist ; a={agg_fit_a:.6f}, b={agg_fit_b:.6f}, r2={agg_fit_r2:.6f}")
    print(f"inversion_applied_in {inv_count}/{len(summaries)} runs")
    print(f"median match_delta_ms={agg_delta:.3f}")

    print("\nPer-FPS AE activity")
    print("fps,runs,active_runs,active_ratio,median_gain_std,median_exp_std")

    def fps_sort_key(val: str):
        if val == "native":
            return (0, 0.0)
        try:
            return (1, float(val))
        except ValueError:
            return (2, 1e9)

    for fps in sorted(per_fps.keys(), key=fps_sort_key):
        group = per_fps[fps]
        active = sum(1 for s in group if bool(s.get("ae_active")))
        med_gstd = safe_median([float(s["gain_std"]) for s in group])
        med_estd = safe_median([float(s["exp_std"]) for s in group])
        ratio = active / len(group) if group else 0.0
        print(f"{fps},{len(group)},{active},{ratio:.2f},{med_gstd:.3f},{med_estd:.3f}")

    if args.out_csv:
        keys = [
            "file", "label", "fps", "rep", "rows", "dropped_quality", "ae_active",
            "gain_std", "exp_std", "fps_std", "yhist_std",
            "corr_gain_yhist", "corr_gain_aey",
            "fit_inverted", "fit_intercept", "fit_slope", "fit_r2", "delta_median"
        ]
        with open(args.out_csv, "w", encoding="utf-8", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=keys)
            writer.writeheader()
            for s in summaries:
                writer.writerow({k: s.get(k, "") for k in keys})
        print(f"wrote summary CSV: {args.out_csv}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
