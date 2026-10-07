#!/usr/bin/env python3
"""Sweep rotation detector thresholds and rank by CSI (mirrors run_m5_suite.py).

Runs rotation_replay for each threshold combo, scores vs TOR warnings and/or
SPC reports with rotation_verify, and writes a ranked JSON + markdown table.

Usage:
  python tools/rotation_sweep.py --case data/rotation_cases/20130520-ktlx-moore --replay build/Release/rotation_replay.exe --output data/rotation_cases/20130520-ktlx-moore/sweep
"""

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
from rotation_verify import load_detections, load_reports, load_warnings, verify  # noqa: E402


def floats(value: str) -> list[float]:
    return [float(i) for i in value.split(",") if i.strip()]


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--case", type=pathlib.Path, required=True)
    p.add_argument("--replay", type=pathlib.Path, required=True)
    p.add_argument("--output", type=pathlib.Path, required=True)
    p.add_argument("--meso-dv", default="10,14,18")
    p.add_argument("--tvs-dv", default="25,30")
    p.add_argument("--min-reflect", default="20,30,35")
    p.add_argument("--max-dv", default="120")
    p.add_argument("--nyquist", default="28")
    p.add_argument("--same-sign-boost", default="1.8",
                   help="comma list; 0 disables the same-sign path")
    p.add_argument("--min-gate", default="5")
    p.add_argument("--single-tilt-tvs", default="0",
                   help="comma list of 0/1")
    p.add_argument("--rot-span", default="15",
                   help="comma list of ROT minimumVelocitySpan")
    p.add_argument("--rot-shear", default="0.0015",
                   help="comma list of ROT minimumAzimuthalShear")
    p.add_argument("--kinds", default="TVS,MESO")
    p.add_argument("--radius-km", type=float, default=15.0)
    p.add_argument("--lead-min", type=float, default=-10.0)
    p.add_argument("--lead-max", type=float, default=45.0)
    p.add_argument("--runtime-bin", type=pathlib.Path, default=None)
    args = p.parse_args()

    case = json.loads((args.case / ".." / "cases.json").read_text(encoding="utf-8"))
    cfg = next(c for c in case["cases"] if c["id"] == args.case.name)
    warnings_path = args.case / "warnings.geojson"
    reports_path = args.case / "spc_rpts.csv"
    warnings = load_warnings(warnings_path, {"TO"}) if warnings_path.exists() else []
    reports = []
    rlat, rlon = cfg["radar_lat"], cfg["radar_lon"]
    from rotation_verify import haversine_km
    if warnings:
        # Same domain rule as verify(): a warning counts only if any vertex
        # is within 150 km of the radar. Out-of-domain polygons (St Louis /
        # Chicago / Little Rock on the Mayfield map) can never be matched.
        kept = []
        for ev in warnings:
            if ev.poly is not None and any(
                    haversine_km(rlat, rlon, lat, lon) <= 150.0
                    for ring in ev.poly for lon, lat in ring):
                kept.append(ev)
        print(f"warning domain filter: {len(warnings)} -> {len(kept)} within 150 km",
              flush=True)
        warnings = kept
    if reports_path.exists():
        import datetime as dt
        reports = load_reports(reports_path, dt.date.fromisoformat(cfg["convective_date"]))
        reports = [e for e in reports
                   if e.lat is not None and haversine_km(rlat, rlon, e.lat, e.lon) <= 150.0]
    print(f"truth: {len(warnings)} TOR warnings, {len(reports)} domain SPC tornado reports")

    env = os.environ.copy()
    qt_bin = "C:/Qt/6.8.3/msvc2022_64/bin"
    if args.runtime_bin:
        env["PATH"] = str(args.runtime_bin) + os.pathsep + env.get("PATH", "")
    elif qt_bin and pathlib.Path(qt_bin).exists():
        env["PATH"] = qt_bin + os.pathsep + env.get("PATH", "")

    args.output.mkdir(parents=True, exist_ok=True)
    results = []
    combos = list(itertools.product(floats(args.meso_dv), floats(args.tvs_dv),
                                    floats(args.min_reflect), floats(args.max_dv),
                                    floats(args.same_sign_boost), floats(args.min_gate),
                                    floats(args.single_tilt_tvs),
                                    floats(args.rot_span), floats(args.rot_shear)))
    for meso, tvs, ref, maxdv, ssb, mingate, sttvs, rotspan, rotshear in combos:
        tag = (f"meso{meso:g}-tvs{tvs:g}-ref{ref:g}-max{maxdv:g}"
               f"-ssb{ssb:g}-mg{mingate:g}-st{sttvs:g}-rs{rotspan:g}-rh{rotshear:g}")
        det_path = args.output / f"detections-{tag}.csv"
        cmd = [str(args.replay), str(args.case), "--lat", str(cfg["radar_lat"]),
               "--lon", str(cfg["radar_lon"]), "--elev", str(cfg["radar_elev_m"]),
               "--nyquist", args.nyquist, "--meso-dv", str(meso), "--tvs-dv", str(tvs),
               "--min-reflect", str(ref), "--max-dv", str(maxdv),
               "--same-sign-boost", str(ssb), "--min-gate", str(mingate),
               "--single-tilt-tvs", str(int(sttvs)),
               "--rot-span", str(rotspan), "--rot-shear", str(rotshear), "--csv"]
        with det_path.open("w", encoding="utf-8", newline="") as stream:
            subprocess.run(cmd, stdout=stream, check=True, env=env)
        kinds = {k.strip().upper() for k in args.kinds.split(",") if k.strip()}
        dets = load_detections(det_path, kinds)
        row: dict = {"meso_dv": meso, "tvs_dv": tvs, "min_reflect": ref, "max_dv": maxdv,
                     "same_sign_boost": ssb, "min_gate": mingate,
                     "single_tilt_tvs": sttvs, "rot_span": rotspan, "rot_shear": rotshear,
                     "detections": len(dets), "tag": tag}
        for name, events in (("warn", warnings), ("spc", reports)):
            if not events:
                continue
            m = verify(dets, events, args.lead_min, args.lead_max, args.radius_km)
            row[f"{name}_pod"] = m.pod
            row[f"{name}_far"] = m.far
            row[f"{name}_csi"] = m.csi
            row[f"{name}_hits"] = m.hits
            row[f"{name}_events"] = m.events
            row[f"{name}_lead_median"] = m.lead_median
        # Rank key: warning CSI first (richer truth: 9 polygons vs 2 reports).
        results.append(row)
        print(f"{tag}: n={len(dets)} "
              f"warn CSI={row.get('warn_csi', float('nan')):.3f} POD={row.get('warn_pod', 0):.2f} "
              f"FAR={row.get('warn_far', 0):.2f} | spc CSI={row.get('spc_csi', float('nan')):.3f}",
              flush=True)

    results.sort(key=lambda r: (-r.get("warn_csi", -1), -r.get("spc_csi", -1), r["detections"]))
    (args.output / "sweep-results.json").write_text(json.dumps({"results": results}, indent=2),
                                                    encoding="utf-8")
    lines = ["# Rotation threshold sweep", "",
             "| Rank | meso | tvs | ref | ssb | mg | st | rs | rh | n | warn POD | warn FAR | warn CSI | spc CSI |",
             "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for rank, r in enumerate(results, 1):
        lines.append(f"| {rank} | {r['meso_dv']:g} | {r['tvs_dv']:g} | {r['min_reflect']:g} | "
                     f"{r.get('same_sign_boost', 0):g} | {r.get('min_gate', 0):g} | "
                     f"{r.get('single_tilt_tvs', 0):g} | {r.get('rot_span', 0):g} | "
                     f"{r.get('rot_shear', 0):g} | "
                     f"{r['detections']} | {r.get('warn_pod', 0):.3f} | {r.get('warn_far', 0):.3f} | "
                     f"{r.get('warn_csi', 0):.3f} | {r.get('spc_csi', 0):.3f} |")
    (args.output / "sweep-report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(json.dumps(results[0], indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
