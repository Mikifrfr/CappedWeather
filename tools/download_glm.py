#!/usr/bin/env python3
"""Download a bounded GOES GLM L2 LCFA archive window without an AWS SDK."""

from __future__ import annotations

import argparse
import concurrent.futures
import datetime as dt
import pathlib
import re
import random
import sys
import time
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

UTC = dt.timezone.utc
SCAN_RE = re.compile(r"_s(\d{4})(\d{3})(\d{2})(\d{2})(\d{2})\d_")
BUCKETS = {"G18": "noaa-goes18", "G19": "noaa-goes19"}
USER_AGENT = "CappedWeather-M5-verification/1.0"


def read_url(request: urllib.request.Request, timeout: int, attempts: int = 5) -> bytes:
    for attempt in range(attempts):
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return response.read()
        except Exception:
            if attempt + 1 == attempts:
                raise
            time.sleep((2**attempt) + random.random())
    raise AssertionError("unreachable")


def parse_time(value: str) -> dt.datetime:
    parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=UTC)
    return parsed.astimezone(UTC)


def scan_start(key: str) -> dt.datetime | None:
    match = SCAN_RE.search(pathlib.PurePosixPath(key).name)
    if not match:
        return None
    year, doy, hour, minute, second = map(int, match.groups())
    return dt.datetime(year, 1, 1, tzinfo=UTC) + dt.timedelta(
        days=doy - 1, hours=hour, minutes=minute, seconds=second
    )


def hours_between(start: dt.datetime, end: dt.datetime):
    current = start.replace(minute=0, second=0, microsecond=0)
    while current < end:
        yield current
        current += dt.timedelta(hours=1)


def list_hour(satellite: str, hour: dt.datetime) -> list[str]:
    bucket = BUCKETS[satellite]
    prefix = f"GLM-L2-LCFA/{hour.year:04d}/{hour.timetuple().tm_yday:03d}/{hour.hour:02d}/"
    query = urllib.parse.urlencode({"list-type": "2", "prefix": prefix})
    request = urllib.request.Request(
        f"https://{bucket}.s3.amazonaws.com/?{query}", headers={"User-Agent": USER_AGENT}
    )
    root = ET.fromstring(read_url(request, timeout=60))
    return [node.text for node in root.findall(".//{*}Key") if node.text]


def download_one(item: tuple[str, str], output: pathlib.Path) -> tuple[str, bool, int]:
    satellite, key = item
    target = output / satellite / pathlib.PurePosixPath(key).name
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists() and target.stat().st_size > 0:
        return target.name, False, target.stat().st_size
    url = f"https://{BUCKETS[satellite]}.s3.amazonaws.com/{urllib.parse.quote(key)}"
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    partial = target.with_suffix(target.suffix + ".part")
    for attempt in range(5):
        try:
            with urllib.request.urlopen(request, timeout=120) as response, partial.open("wb") as stream:
                while block := response.read(1024 * 1024):
                    stream.write(block)
            break
        except Exception:
            partial.unlink(missing_ok=True)
            if attempt == 4:
                raise
            time.sleep((2**attempt) + random.random())
    partial.replace(target)
    return target.name, True, target.stat().st_size


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--start", required=True, help="UTC ISO8601 start, inclusive")
    parser.add_argument("--end", required=True, help="UTC ISO8601 end, exclusive")
    parser.add_argument("--satellite", choices=["G18", "G19", "both"], default="G19")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--workers", type=int, default=4)
    args = parser.parse_args()
    start, end = parse_time(args.start), parse_time(args.end)
    if end <= start:
        parser.error("--end must be after --start")
    satellites = list(BUCKETS) if args.satellite == "both" else [args.satellite]
    selected: list[tuple[str, str]] = []
    for satellite in satellites:
        for hour in hours_between(start, end):
            for key in list_hour(satellite, hour):
                stamp = scan_start(key)
                if stamp is not None and start <= stamp < end:
                    selected.append((satellite, key))
    selected.sort(key=lambda item: (scan_start(item[1]) or start, item[0]))
    print(f"selected {len(selected)} LCFA files", file=sys.stderr)
    downloaded = 0
    total_bytes = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(args.workers, 8))) as pool:
        futures = [pool.submit(download_one, item, args.output) for item in selected]
        for index, future in enumerate(concurrent.futures.as_completed(futures), 1):
            name, fresh, size = future.result()
            downloaded += int(fresh)
            total_bytes += size
            if index % 25 == 0 or index == len(futures):
                print(f"{index}/{len(futures)} {name}", file=sys.stderr)
    print(f"files={len(selected)} downloaded={downloaded} bytes={total_bytes}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
