#!/usr/bin/env python3
"""Download SPC filtered storm reports for rotation validation cases.

SPC daily files live at https://www.spc.noaa.gov/climo/reports/<yymmdd>_rpts.csv
where <yymmdd> is the convective day (12Z-12Z). The cases.json entry carries
the right file per case; this script just mirrors it next to the volumes.

Usage:
  python tools/fetch_spc_reports.py --cases data/rotation_cases/cases.json --output data/rotation_cases
"""

from __future__ import annotations

import argparse
import json
import pathlib
import urllib.request

USER_AGENT = "CappedWeather-rotation-validation/1.0"
BASE = "https://www.spc.noaa.gov/climo/reports"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cases", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    cases = json.loads(args.cases.read_text(encoding="utf-8"))["cases"]
    for case in cases:
        name = case.get("spc_file")
        if not name:
            continue
        case_dir = args.output / case["id"]
        case_dir.mkdir(parents=True, exist_ok=True)
        target = case_dir / "spc_rpts.csv"
        if target.exists() and target.stat().st_size > 0:
            print(f"[{case['id']}] cached {target}")
            continue
        req = urllib.request.Request(f"{BASE}/{name}", headers={"User-Agent": USER_AGENT})
        with urllib.request.urlopen(req, timeout=120) as response:
            data = response.read()
        target.write_bytes(data)
        print(f"[{case['id']}] saved {len(data)} bytes -> {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
