#!/usr/bin/env python3
"""Run reproducible multi-case LJA parameter verification and rank by CSI."""

from __future__ import annotations

import argparse
import itertools
import json
import os
import pathlib
import statistics
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from m5_verify import load_jumps, load_warnings, parse_time, verify  # noqa: E402


def floats(value: str) -> list[float]:
    return [float(item) for item in value.split(",") if item.strip()]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cases", type=pathlib.Path, required=True)
    parser.add_argument("--replay", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--cell", default="0.25")
    parser.add_argument("--min-rate", default="8,10,12")
    parser.add_argument("--window", default="720")
    parser.add_argument("--resolution", default="60")
    parser.add_argument("--runtime-bin", type=pathlib.Path,
                        help="directory containing netCDF/HDF5 runtime DLLs")
    parser.add_argument("--reuse-events", action="store_true",
                        help="reuse existing per-case replay CSV files when present")
    args = parser.parse_args()
    cases = json.loads(args.cases.read_text(encoding="utf-8"))["cases"]
    root = args.cases.resolve().parents[2]
    args.output.mkdir(parents=True, exist_ok=True)
    results = []
    for cell, min_rate, window, resolution in itertools.product(
            floats(args.cell), floats(args.min_rate), floats(args.window), floats(args.resolution)):
        aggregate = {"warnings": 0, "hits": 0, "misses": 0, "eligible_jumps": 0,
                     "hit_jumps": 0, "false_alarm_jumps": 0}
        case_lead_medians = []
        case_results = []
        for case in cases:
            event_path = args.output / f"{case['id']}-c{cell:g}-r{min_rate:g}-w{window:g}.csv"
            command = [str(args.replay), str(root / case["glm_dir"]), "--cell", str(cell),
                       "--min-rate", str(min_rate), "--window", str(window),
                       "--resolution", str(resolution), "--csv"]
            environment = os.environ.copy()
            if args.runtime_bin:
                environment["PATH"] = str(args.runtime_bin) + os.pathsep + environment.get("PATH", "")
            if not (args.reuse_events and event_path.exists()):
                with event_path.open("w", encoding="utf-8", newline="") as stream:
                    subprocess.run(command, stdout=stream, check=True, env=environment)
            warnings = load_warnings(root / case["warnings"], set(case.get("phenomena", ["TO", "SV"])))
            jumps = load_jumps(event_path)
            if case.get("coverage_windows"):
                windows = [(parse_time(start), parse_time(end))
                           for start, end in case["coverage_windows"]]
                warnings = [warning for warning in warnings
                            if any(start <= warning.issue < end for start, end in windows)]
                earliest = min(start for start, _ in windows)
                latest = max(end for _, end in windows)
                jumps = [jump for jump in jumps if earliest <= jump.time < latest]
            metrics = verify(jumps, warnings,
                             case.get("lead_min", 0), case.get("lead_max", 120),
                             case.get("radius_km", 35), case.get("domain_margin_deg", 1.0))
            payload = metrics.__dict__.copy()
            payload["id"] = case["id"]
            case_results.append(payload)
            for key in aggregate:
                aggregate[key] += getattr(metrics, key)
            if metrics.lead_median is not None:
                case_lead_medians.append(metrics.lead_median)
        hits, misses = aggregate["hits"], aggregate["misses"]
        false = aggregate["false_alarm_jumps"]
        eligible = aggregate["eligible_jumps"]
        result = {"cell": cell, "min_rate": min_rate, "window": window,
                  "resolution": resolution, **aggregate,
                  "pod": hits / (hits + misses) if hits + misses else 0.0,
                  "far": false / eligible if eligible else 0.0,
                  "csi": hits / (hits + misses + false) if hits + misses + false else 0.0,
                  "lead_median_across_cases": statistics.median(case_lead_medians)
                  if case_lead_medians else None,
                  "cases": case_results}
        results.append(result)
    results.sort(key=lambda row: (-row["csi"], -row["pod"], row["far"]))
    (args.output / "m5-results.json").write_text(json.dumps({"results": results}, indent=2),
                                                   encoding="utf-8")
    lines = ["# M5 lightning-jump verification", "",
             "| Rank | Cell | Min rate | Window | POD | FAR | CSI | Warnings |",
             "|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for rank, row in enumerate(results, 1):
        lines.append(f"| {rank} | {row['cell']:.2f} | {row['min_rate']:.1f} | "
                     f"{row['window']:.0f}s | {row['pod']:.3f} | {row['far']:.3f} | "
                     f"{row['csi']:.3f} | {row['warnings']} |")
    (args.output / "m5-report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(json.dumps(results[0], indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
