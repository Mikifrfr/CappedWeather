#!/usr/bin/env python3
"""Fetch initial NWS storm-based warning polygons from the official IEM archive."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import urllib.parse
import urllib.request

UTC = dt.timezone.utc
USER_AGENT = "CappedWeather-M5-verification/1.0"


def parse_time(value: str) -> dt.datetime:
    parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    return (parsed if parsed.tzinfo else parsed.replace(tzinfo=UTC)).astimezone(UTC)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--start", required=True)
    parser.add_argument("--end", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--phenomena", default="TO,SV")
    args = parser.parse_args()
    start, end = parse_time(args.start), parse_time(args.end)
    query = urllib.parse.urlencode({
        "sts": start.isoformat().replace("+00:00", "Z"),
        "ets": end.isoformat().replace("+00:00", "Z"),
    })
    request = urllib.request.Request(
        f"https://mesonet.agron.iastate.edu/geojson/sbw.geojson?{query}",
        headers={"User-Agent": USER_AGENT},
    )
    with urllib.request.urlopen(request, timeout=120) as response:
        payload = json.load(response)
    allowed = {item.strip().upper() for item in args.phenomena.split(",") if item.strip()}
    unique: dict[str, dict] = {}
    for feature in payload.get("features", []):
        props = feature.get("properties") or {}
        issue_text = props.get("issue") or props.get("polygon_begin")
        if not issue_text or not feature.get("geometry"):
            continue
        issue = parse_time(issue_text)
        if not (start <= issue < end):
            continue
        if props.get("status") != "NEW" or props.get("significance") != "W":
            continue
        if str(props.get("phenomena", "")).upper() not in allowed:
            continue
        unique[str(feature.get("id") or props.get("product_id"))] = feature
    output = {
        "type": "FeatureCollection",
        "source": "IEM storm-based warning archive",
        "source_url": request.full_url,
        "window_start": start.isoformat(),
        "window_end": end.isoformat(),
        "features": list(unique.values()),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2), encoding="utf-8")
    print(f"saved {len(unique)} initial TO/SV warning polygons to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
