#!/usr/bin/env python3
import argparse
import bisect
import csv
import re
import sys
from typing import Dict, List, Tuple

MT_PATTERN = re.compile(r"mt9m114 .*?: mt9m114 stream status: (.*)$")
ATOMISP_PATTERN = re.compile(r"atomisp.*?: atomisp s3a status: (.*)$")


def parse_value(value: str):
    value = value.strip()
    if value.startswith("0x"):
        try:
            return int(value, 16)
        except ValueError:
            return value
    try:
        if "." in value:
            return float(value)
        return int(value)
    except ValueError:
        return value


def parse_kv_blob(blob: str) -> Dict[str, object]:
    out: Dict[str, object] = {}
    for token in blob.split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        out[key.strip()] = parse_value(value)
    return out


def parse_uptime_ms(row: Dict[str, object]):
    value = row.get("uptime_ms")
    if value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def parse_log_lines(lines: List[str]) -> Tuple[List[Dict[str, object]], List[Dict[str, object]]]:
    mt_rows: List[Dict[str, object]] = []
    atomisp_rows: List[Dict[str, object]] = []
    dropped_mt = 0
    dropped_atomisp = 0

    for raw in lines:
        line = raw.strip()
        mt_match = MT_PATTERN.search(line)
        if mt_match:
            row = parse_kv_blob(mt_match.group(1))
            if parse_uptime_ms(row) is not None:
                mt_rows.append(row)
            else:
                dropped_mt += 1
            continue

        atomisp_match = ATOMISP_PATTERN.search(line)
        if atomisp_match:
            row = parse_kv_blob(atomisp_match.group(1))
            if parse_uptime_ms(row) is not None:
                atomisp_rows.append(row)
            else:
                dropped_atomisp += 1

    mt_rows.sort(key=lambda r: parse_uptime_ms(r))
    atomisp_rows.sort(key=lambda r: parse_uptime_ms(r))

    if dropped_mt or dropped_atomisp:
        print(f"warning: dropped malformed rows mt={dropped_mt} atomisp={dropped_atomisp}")

    return mt_rows, atomisp_rows


def nearest_atomisp_row(mt_uptime: int,
                        atomisp_uptimes: List[int],
                        atomisp_rows: List[Dict[str, object]],
                        max_delta_ms: int):
    if not atomisp_uptimes:
        return None, None

    idx = bisect.bisect_left(atomisp_uptimes, mt_uptime)
    candidates = []
    if idx < len(atomisp_uptimes):
        candidates.append(idx)
    if idx > 0:
        candidates.append(idx - 1)

    best = None
    best_delta = None
    for c in candidates:
        delta = abs(atomisp_uptimes[c] - mt_uptime)
        if best is None or delta < best_delta:
            best = atomisp_rows[c]
            best_delta = delta

    if best_delta is not None and best_delta <= max_delta_ms:
        return best, best_delta

    return None, None


def write_csv(path: str, rows: List[Dict[str, object]], prefix: str = "") -> None:
    if not rows:
        with open(path, "w", newline="", encoding="utf-8") as f:
            f.write("")
        return

    keys = sorted({k for row in rows for k in row.keys()})
    if prefix:
        headers = [f"{prefix}{k}" for k in keys]
    else:
        headers = keys

    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(headers)
        for row in rows:
            w.writerow([row.get(k, "") for k in keys])


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Parse mt9m114 + atomisp s3a dmesg lines and join by uptime_ms"
    )
    parser.add_argument("--input", "-i", default="-",
                        help="Input file (default: stdin)")
    parser.add_argument("--out", "-o", default="joined.csv",
                        help="Joined output CSV path")
    parser.add_argument("--mt-out", default="",
                        help="Optional: write parsed mt9m114 rows to CSV")
    parser.add_argument("--atomisp-out", default="",
                        help="Optional: write parsed atomisp rows to CSV")
    parser.add_argument("--max-delta-ms", type=int, default=400,
                        help="Max |uptime_ms delta| for matching (default: 400)")
    args = parser.parse_args()

    if args.input == "-":
        lines = sys.stdin.readlines()
    else:
        with open(args.input, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()

    mt_rows, atomisp_rows = parse_log_lines(lines)

    atomisp_uptimes = [parse_uptime_ms(r) for r in atomisp_rows]

    joined_rows: List[Dict[str, object]] = []
    matched = 0
    for mt in mt_rows:
        mt_uptime = parse_uptime_ms(mt)
        if mt_uptime is None:
            continue
        atomisp, delta = nearest_atomisp_row(mt_uptime, atomisp_uptimes,
                                             atomisp_rows, args.max_delta_ms)

        row: Dict[str, object] = {}
        for k, v in mt.items():
            row[f"mt_{k}"] = v

        if atomisp is not None:
            matched += 1
            row["match_delta_ms"] = delta
            for k, v in atomisp.items():
                row[f"s3a_{k}"] = v
        else:
            row["match_delta_ms"] = ""

        joined_rows.append(row)

    write_csv(args.out, joined_rows)

    if args.mt_out:
        write_csv(args.mt_out, mt_rows)
    if args.atomisp_out:
        write_csv(args.atomisp_out, atomisp_rows)

    print(f"parsed mt_rows={len(mt_rows)} atomisp_rows={len(atomisp_rows)} joined_rows={len(joined_rows)} matched={matched} max_delta_ms={args.max_delta_ms}")
    print(f"wrote joined CSV: {args.out}")
    if args.mt_out:
        print(f"wrote mt CSV: {args.mt_out}")
    if args.atomisp_out:
        print(f"wrote atomisp CSV: {args.atomisp_out}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
