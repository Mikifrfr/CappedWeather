#!/usr/bin/env python3
"""Score rotation detections against TOR warnings and/or SPC tornado reports.

Mirrors tools/m5_verify.py semantics so rotation numbers are directly
comparable to the lightning-jump validation in docs/validation-2025-05-16.md:
POD / FAR / CSI + lead-time distribution.

Detection CSV (from tools/rotation_replay --csv):
  volume_time,volume_file,kind,lat,lon,delta_v_mps,diameter_km,depth_m,tilts

Truth sources (either or both):
  --warnings data/rotation_cases/<id>/warnings.geojson  (via tools/fetch_verification_truth.py, phenomena TO)
  --reports  data/rotation_cases/<id>/spc_rpts.csv      (SPC filtered reports, tornado section only)
  --convective-date YYYY-MM-DD  (needed to resolve SPC HHMM times; 12Z boundary like the app)

Matching:
  A truth event (warning issue time / report time) is a HIT when at least one
  unmatched detection of an allowed --kinds set falls within --radius-km and
  the detection time is within [lead-min, lead-max] minutes BEFORE the event.
  Earliest unmatched precursor wins (same rule as m5_verify).

Examples:
  python tools/rotation_verify.py --detections data/rotation_cases/20130520-ktlx-moore/detections.csv --reports data/rotation_cases/20130520-ktlx-moore/spc_rpts.csv --convective-date 2013-05-20
  python tools/rotation_verify.py --detections out.csv --warnings warnings.geojson --phenomena TO --kinds TVS,MESO --radius-km 15
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import math
import pathlib
import statistics
from dataclasses import asdict, dataclass

UTC = dt.timezone.utc


@dataclass(frozen=True)
class Detection:
    time: dt.datetime
    kind: str
    lat: float
    lon: float


@dataclass(frozen=True)
class Event:
    time: dt.datetime
    label: str
    lat: float | None
    lon: float | None
    poly: list[list[tuple[float, float]]] | None  # lon/lat rings for warnings


@dataclass
class Metrics:
    events: int
    hits: int
    misses: int
    eligible_detections: int
    hit_detections: int
    false_alarm_detections: int
    pod: float
    far: float
    csi: float
    lead_min: float | None
    lead_median: float | None
    lead_max: float | None


def parse_time(value: str) -> dt.datetime:
    parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    return (parsed if parsed.tzinfo else parsed.replace(tzinfo=UTC)).astimezone(UTC)


def load_detections(path: pathlib.Path, kinds: set[str]) -> list[Detection]:
    out = []
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            kind = row.get("kind", "").strip().upper()
            if kinds and kind not in kinds:
                continue
            try:
                out.append(Detection(parse_time(row["volume_time"]), kind,
                                     float(row["lat"]), float(row["lon"])))
            except (KeyError, ValueError):
                continue
    return sorted(out, key=lambda d: d.time)


def geometry_polygons(geometry: dict) -> list[list[tuple[float, float]]]:
    kind = geometry.get("type")
    coords = geometry.get("coordinates") or []
    raw = [coords] if kind == "Polygon" else coords if kind == "MultiPolygon" else []
    return [[(float(p[0]), float(p[1])) for p in poly[0]] for poly in raw if poly and poly[0]]


def load_warnings(path: pathlib.Path, phenomena: set[str]) -> list[Event]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    out, seen = [], set()
    for feat in payload.get("features", []):
        props = feat.get("properties") or {}
        if str(props.get("phenomena", "")).upper() not in phenomena:
            continue
        ident = str(feat.get("id") or props.get("product_id"))
        if ident in seen:
            continue
        issue = props.get("issue") or props.get("polygon_begin")
        polys = geometry_polygons(feat.get("geometry") or {})
        if not issue or not polys:
            continue
        seen.add(ident)
        out.append(Event(parse_time(issue), f"{props.get('phenomena')} {ident[-12:]}",
                         None, None, polys))
    return sorted(out, key=lambda e: e.time)


def load_reports(path: pathlib.Path, convective_date: dt.date) -> list[Event]:
    """SPC yymmdd_rpts.csv: tornado section is the rows before the first wind header."""
    lines = path.read_text(encoding="utf-8").splitlines()
    # Find tornado block: starts with Time,F_Scale header, ends at next Time,Speed header.
    # Pre-2012 files spell it Time,F-Scale (hyphen).
    header_idx = next((i for i, l in enumerate(lines)
                       if l.startswith("Time,F_Scale") or l.startswith("Time,F-Scale")), -1)
    wind_idx = next((i for i, l in enumerate(lines) if l.startswith("Time,Speed")), len(lines))
    if header_idx < 0:
        return []
    out = []
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(lines[header_idx:wind_idx])
        for row in reader:
            try:
                hhmm = int(str(row.get("Time", "")).strip())
                lat, lon = float(row.get("Lat", "")), float(row.get("Lon", ""))
            except (ValueError, TypeError):
                continue
            if not (0 <= hhmm <= 2359 and hhmm % 100 < 60):
                continue
            day = convective_date + (dt.timedelta(days=1) if hhmm < 1200 else dt.timedelta(0))
            t = dt.datetime(day.year, day.month, day.day, hhmm // 100, hhmm % 100, tzinfo=UTC)
            out.append(Event(t, f"{row.get('Location','')} {row.get('State','')} {hhmm:04d}",
                             lat, lon, None))
    return sorted(out, key=lambda e: e.time)


def haversine_km(a_lat: float, a_lon: float, b_lat: float, b_lon: float) -> float:
    r = 6371.0
    p1, p2 = math.radians(a_lat), math.radians(b_lat)
    dp, dl = p2 - p1, math.radians(b_lon - a_lon)
    h = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(h))


def point_in_ring(lon: float, lat: float, ring: list[tuple[float, float]]) -> bool:
    inside, j = False, len(ring) - 1
    for i, (xi, yi) in enumerate(ring):
        xj, yj = ring[j]
        if (yi > lat) != (yj > lat):
            if lon < (xj - xi) * (lat - yi) / ((yj - yi) or 1e-12) + xi:
                inside = not inside
        j = i
    return inside


def dist_to_event_km(det: Detection, ev: Event) -> float:
    if ev.poly:
        best = math.inf
        for ring in ev.poly:
            if point_in_ring(det.lon, det.lat, ring):
                return 0.0
            for i, p in enumerate(ring):
                q = ring[(i + 1) % len(ring)]
                # local equirectangular segment distance
                sx = 111.195 * math.cos(math.radians(det.lat))
                ax, ay = (p[0] - det.lon) * sx, (p[1] - det.lat) * 111.195
                bx, by = (q[0] - det.lon) * sx, (q[1] - det.lat) * 111.195
                dx, dy = bx - ax, by - ay
                t = max(0.0, min(1.0, -(ax * dx + ay * dy) / (dx * dx + dy * dy or 1e-12)))
                best = min(best, math.hypot(ax + t * dx, ay + t * dy))
        return best
    assert ev.lat is not None and ev.lon is not None
    return haversine_km(det.lat, det.lon, ev.lat, ev.lon)


def verify(dets: list[Detection], events: list[Event], lead_min: float = 0.0,
           lead_max: float = 45.0, radius_km: float = 15.0) -> Metrics:
    if not events:
        raise ValueError("event set is empty")
    first = min(e.time for e in events) - dt.timedelta(minutes=lead_max)
    last = max(e.time for e in events) - dt.timedelta(minutes=lead_min)
    eligible = [d for d in dets if first <= d.time <= last]
    used: set[int] = set()
    leads: list[float] = []
    for ev in events:
        cands = []
        for i, d in enumerate(eligible):
            if i in used:
                continue
            lead = (ev.time - d.time).total_seconds() / 60.0
            if lead_min <= lead <= lead_max and dist_to_event_km(d, ev) <= radius_km:
                cands.append((d.time, i, lead))
        if cands:
            _, i, lead = min(cands)
            used.add(i)
            leads.append(lead)
    hits, misses = len(leads), len(events) - len(leads)
    fa = len(eligible) - len(used)
    return Metrics(len(events), hits, misses, len(eligible), len(used), fa,
                   hits / len(events), fa / len(eligible) if eligible else 0.0,
                   hits / (hits + misses + fa) if hits + misses + fa else 0.0,
                   min(leads) if leads else None,
                   statistics.median(leads) if leads else None,
                   max(leads) if leads else None)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--detections", type=pathlib.Path, required=True)
    p.add_argument("--warnings", type=pathlib.Path)
    p.add_argument("--reports", type=pathlib.Path)
    p.add_argument("--convective-date")
    p.add_argument("--phenomena", default="TO")
    p.add_argument("--kinds", default="TVS,MESO,ROT")
    p.add_argument("--lead-min", type=float, default=0.0)
    p.add_argument("--lead-max", type=float, default=45.0)
    p.add_argument("--radius-km", type=float, default=15.0)
    p.add_argument("--radar", default=None,
                   help="LAT,LON filter: keep only SPC reports within --max-range-km of the radar")
    p.add_argument("--max-range-km", type=float, default=150.0)
    p.add_argument("--json-out", type=pathlib.Path)
    args = p.parse_args()
    kinds = {k.strip().upper() for k in args.kinds.split(",") if k.strip()}
    dets = load_detections(args.detections, kinds)
    events: list[Event] = []
    if args.warnings:
        events += load_warnings(args.warnings,
                                {s.strip().upper() for s in args.phenomena.split(",") if s.strip()})
    if args.reports:
        if not args.convective_date:
            p.error("--convective-date is required with --reports")
        cd = dt.date.fromisoformat(args.convective_date)
        events += load_reports(args.reports, cd)
    if args.radar:
        try:
            rlat, rlon = (float(x) for x in args.radar.split(","))
        except ValueError:
            p.error("--radar must be LAT,LON")
        before = len(events)
        kept = []
        for ev in events:
            if ev.poly is not None:
                # Domain-filter warnings too: a polygon hundreds of km from
                # the radar (Mayfield had 80 spanning 4 states) can never be
                # matched by single-radar detections and only deflates POD.
                # Keep it when any vertex is within range (elongated tracks).
                inside = False
                for ring in ev.poly:
                    for lon, lat in ring:
                        if haversine_km(rlat, rlon, lat, lon) <= args.max_range_km:
                            inside = True
                            break
                    if inside:
                        break
                if inside:
                    kept.append(ev)
                continue
            if ev.lat is not None and haversine_km(rlat, rlon, ev.lat, ev.lon) <= args.max_range_km:
                kept.append(ev)
        events = kept
        print(f"radar filter: {before} -> {len(events)} events within "
              f"{args.max_range_km:g} km", flush=True)
    if not events:
        p.error("no events loaded (pass --warnings and/or --reports)")
    events.sort(key=lambda e: e.time)
    m = verify(dets, events, args.lead_min, args.lead_max, args.radius_km)
    print(json.dumps(asdict(m), indent=2))
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(asdict(m), indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
