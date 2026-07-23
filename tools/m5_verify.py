#!/usr/bin/env python3
"""Score lightning-jump starts against archived NWS warning polygons."""

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
class Jump:
    time: dt.datetime
    latitude: float
    longitude: float
    sigma: float
    rate: float


@dataclass(frozen=True)
class Warning:
    identifier: str
    issue: dt.datetime
    phenomenon: str
    polygons: list[list[tuple[float, float]]]


@dataclass
class Metrics:
    warnings: int
    hits: int
    misses: int
    eligible_jumps: int
    hit_jumps: int
    false_alarm_jumps: int
    pod: float
    far: float
    csi: float
    lead_min: float | None
    lead_median: float | None
    lead_max: float | None


def parse_time(value: str) -> dt.datetime:
    parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    return (parsed if parsed.tzinfo else parsed.replace(tzinfo=UTC)).astimezone(UTC)


def load_jumps(path: pathlib.Path) -> list[Jump]:
    with path.open(newline="", encoding="utf-8") as stream:
        rows = csv.DictReader(stream)
        return [
            Jump(parse_time(row["time"]), float(row["latitude"]), float(row["longitude"]),
                 float(row["sigma_level"]), float(row["flash_rate_per_min"]))
            for row in rows if row["kind"].strip() == "JUMP_START"
        ]


def geometry_polygons(geometry: dict) -> list[list[tuple[float, float]]]:
    kind = geometry.get("type")
    coordinates = geometry.get("coordinates") or []
    raw = [coordinates] if kind == "Polygon" else coordinates if kind == "MultiPolygon" else []
    return [[(float(point[0]), float(point[1])) for point in polygon[0]]
            for polygon in raw if polygon and polygon[0]]


def load_warnings(path: pathlib.Path, phenomena: set[str] | None = None) -> list[Warning]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    result = []
    seen = set()
    for feature in payload.get("features", []):
        props = feature.get("properties") or {}
        phenomenon = str(props.get("phenomena", "")).upper()
        if phenomena and phenomenon not in phenomena:
            continue
        identifier = str(feature.get("id") or props.get("product_id"))
        if identifier in seen:
            continue
        polygons = geometry_polygons(feature.get("geometry") or {})
        issue = props.get("issue") or props.get("polygon_begin")
        if not polygons or not issue:
            continue
        seen.add(identifier)
        result.append(Warning(identifier, parse_time(issue), phenomenon, polygons))
    return sorted(result, key=lambda warning: warning.issue)


def point_in_ring(lon: float, lat: float, ring: list[tuple[float, float]]) -> bool:
    inside = False
    j = len(ring) - 1
    for i, (xi, yi) in enumerate(ring):
        xj, yj = ring[j]
        if (yi > lat) != (yj > lat):
            crossing = (xj - xi) * (lat - yi) / ((yj - yi) or 1e-12) + xi
            if lon < crossing:
                inside = not inside
        j = i
    return inside


def point_segment_km(lon: float, lat: float, a: tuple[float, float], b: tuple[float, float]) -> float:
    scale_x = 111.195 * math.cos(math.radians(lat))
    ax, ay = (a[0] - lon) * scale_x, (a[1] - lat) * 111.195
    bx, by = (b[0] - lon) * scale_x, (b[1] - lat) * 111.195
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, -(ax * dx + ay * dy) / (dx * dx + dy * dy or 1e-12)))
    return math.hypot(ax + t * dx, ay + t * dy)


def distance_to_warning_km(jump: Jump, warning: Warning) -> float:
    best = math.inf
    for ring in warning.polygons:
        if point_in_ring(jump.longitude, jump.latitude, ring):
            return 0.0
        for index, point in enumerate(ring):
            best = min(best, point_segment_km(jump.longitude, jump.latitude,
                                               point, ring[(index + 1) % len(ring)]))
    return best


def warning_bounds(warnings: list[Warning], margin_deg: float) -> tuple[float, float, float, float]:
    points = [point for warning in warnings for ring in warning.polygons for point in ring]
    return (min(p[0] for p in points) - margin_deg, min(p[1] for p in points) - margin_deg,
            max(p[0] for p in points) + margin_deg, max(p[1] for p in points) + margin_deg)


def verify(jumps: list[Jump], warnings: list[Warning], lead_min: float = 0.0,
           lead_max: float = 120.0, radius_km: float = 35.0,
           domain_margin_deg: float = 1.0) -> Metrics:
    if not warnings:
        raise ValueError("warning set is empty")
    bounds = warning_bounds(warnings, domain_margin_deg)
    first_time = min(w.issue for w in warnings) - dt.timedelta(minutes=lead_max)
    last_time = max(w.issue for w in warnings) - dt.timedelta(minutes=lead_min)
    eligible = [jump for jump in jumps if first_time <= jump.time <= last_time and
                bounds[0] <= jump.longitude <= bounds[2] and bounds[1] <= jump.latitude <= bounds[3]]
    matched_jump_indexes = set()
    leads = []
    for warning in warnings:
        matches = []
        for index, jump in enumerate(eligible):
            if index in matched_jump_indexes:
                continue
            lead = (warning.issue - jump.time).total_seconds() / 60.0
            if lead_min <= lead <= lead_max and distance_to_warning_km(jump, warning) <= radius_km:
                matches.append((jump.time, index, lead))
        if matches:
            _, index, lead = min(matches)  # earliest unmatched operational precursor
            matched_jump_indexes.add(index)
            leads.append(lead)
    hits = len(leads)
    misses = len(warnings) - hits
    false_alarms = len(eligible) - len(matched_jump_indexes)
    pod = hits / len(warnings)
    far = false_alarms / len(eligible) if eligible else 0.0
    csi = hits / (hits + misses + false_alarms) if hits + misses + false_alarms else 0.0
    return Metrics(len(warnings), hits, misses, len(eligible), len(matched_jump_indexes),
                   false_alarms, pod, far, csi,
                   min(leads) if leads else None,
                   statistics.median(leads) if leads else None,
                   max(leads) if leads else None)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--events", type=pathlib.Path, required=True)
    parser.add_argument("--warnings", type=pathlib.Path, required=True)
    parser.add_argument("--phenomena", default="TO,SV")
    parser.add_argument("--lead-min", type=float, default=0.0)
    parser.add_argument("--lead-max", type=float, default=120.0)
    parser.add_argument("--radius-km", type=float, default=35.0)
    parser.add_argument("--domain-margin-deg", type=float, default=1.0)
    parser.add_argument("--json-out", type=pathlib.Path)
    args = parser.parse_args()
    phenomena = {item.strip().upper() for item in args.phenomena.split(",") if item.strip()}
    metrics = verify(load_jumps(args.events), load_warnings(args.warnings, phenomena),
                     args.lead_min, args.lead_max, args.radius_km, args.domain_margin_deg)
    payload = asdict(metrics)
    print(json.dumps(payload, indent=2))
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
