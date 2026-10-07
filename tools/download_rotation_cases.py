#!/usr/bin/env python3
"""Download bounded Archive-II windows for rotation validation cases.

Lists the public NOAA bucket (no AWS SDK, plain HTTPS XML listing like the
in-app AWS archive dialog) and downloads an evenly-sampled subset so each
case stays within its max_volumes budget.

Usage:
  python tools/download_rotation_cases.py --cases data/rotation_cases/cases.json --output data/rotation_cases
  python tools/download_rotation_cases.py --cases data/rotation_cases/cases.json --output data/rotation_cases --case-id 20130520-ktlx-moore --dry-run
"""

from __future__ import annotations

import argparse
import concurrent.futures
import datetime as dt
import json
import pathlib
import random
import re
import sys
import time
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

UTC = dt.timezone.utc
BUCKET = "https://unidata-nexrad-level2.s3.amazonaws.com"
USER_AGENT = "CappedWeather-rotation-validation/1.0"
FILE_RE = re.compile(r"^([A-Z0-9]{4})(\d{8})_(\d{6})_V\d{2}(?:\.gz)?$", re.IGNORECASE)


def parse_time(value: str) -> dt.datetime:
    parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    return (parsed if parsed.tzinfo else parsed.replace(tzinfo=UTC)).astimezone(UTC)


def file_time(name: str) -> dt.datetime | None:
    m = FILE_RE.match(name.upper())
    if not m:
        return None
    try:
        return dt.datetime.strptime(m.group(2) + m.group(3), "%Y%m%d%H%M%S").replace(tzinfo=UTC)
    except ValueError:
        return None


def read_url(request: urllib.request.Request, timeout: int, attempts: int = 5) -> bytes:
    for attempt in range(attempts):
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return response.read()
        except Exception:
            if attempt + 1 == attempts:
                raise
            time.sleep((2 ** attempt) + random.random())
    raise AssertionError("unreachable")


def list_day(radar: str, day: dt.date) -> list[tuple[str, int]]:
    prefix = f"{day.year:04d}/{day.month:02d}/{day.day:02d}/{radar}/"
    query = urllib.parse.urlencode({"list-type": "2", "prefix": prefix, "max-keys": "1000"})
    out: list[tuple[str, int]] = []
    token: str | None = None
    while True:
        q = {"list-type": "2", "prefix": prefix, "max-keys": "1000"}
        if token:
            q["continuation-token"] = token
        req = urllib.request.Request(f"{BUCKET}/?{urllib.parse.urlencode(q)}",
                                     headers={"User-Agent": USER_AGENT})
        root = ET.fromstring(read_url(req, timeout=60))
        ns = {"s3": "http://s3.amazonaws.com/doc/2006-03-01/"}
        for content in root.findall("s3:Contents", ns) or root.findall("Contents"):
            key = (content.findtext("s3:Key", default="", namespaces=ns)
                   or content.findtext("Key", default=""))
            size = int(content.findtext("s3:Size", default="0", namespaces=ns)
                       or content.findtext("Size", default="0") or 0)
            if key and size > 0:
                out.append((key, size))
        trunc = (content_text(root, ns, "IsTruncated"))
        if trunc != "true":
            break
        token = content_text(root, ns, "NextContinuationToken")
        if not token:
            break
    return out


def content_text(root: ET.Element, ns: dict, tag: str) -> str:
    return (root.findtext(f"s3:{tag}", default="", namespaces=ns)
            or root.findtext(tag, default=""))


def download_one(url: str, target: pathlib.Path) -> tuple[str, bool, int]:
    target.parent.mkdir(parents=True, exist_ok=True)
    # Legacy pre-2015 objects are outer-gzip wrapped (KTLX..._V06.gz); the
    # in-app decoder takes raw Archive-II, so gunzip to the plain object name.
    plain = target.with_suffix("") if target.suffix.lower() == ".gz" else target
    if plain.exists() and plain.stat().st_size > 0:
        return plain.name, False, plain.stat().st_size
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    partial = plain.with_suffix(plain.suffix + ".part")
    for attempt in range(5):
        try:
            with urllib.request.urlopen(req, timeout=180) as response, partial.open("wb") as stream:
                if url.lower().endswith(".gz"):
                    import gzip
                    raw = response.read()
                    stream.write(gzip.decompress(raw))
                else:
                    while block := response.read(1024 * 1024):
                        stream.write(block)
            break
        except Exception:
            partial.unlink(missing_ok=True)
            if attempt == 4:
                raise
            time.sleep((2 ** attempt) + random.random())
    partial.replace(plain)
    return plain.name, True, plain.stat().st_size


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cases", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--case-id", default=None)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    cases = json.loads(args.cases.read_text(encoding="utf-8"))["cases"]
    if args.case_id:
        cases = [c for c in cases if c["id"] == args.case_id]
        if not cases:
            parser.error(f"unknown case {args.case_id}")

    total_dl = 0
    for case in cases:
        radar = case["radar"].upper()
        start, end = parse_time(case["window_start"]), parse_time(case["window_end"])
        max_vols = int(case.get("max_volumes", 24))
        # Collect one listing per UTC date touched by the window.
        keys: list[tuple[str, int, dt.datetime]] = []
        day = start.date()
        while day <= end.date():
            for key, size in list_day(radar, day):
                name = pathlib.PurePosixPath(key).name
                stamp = file_time(name)
                if stamp is None or not name.upper().startswith(radar):
                    continue
                if start <= stamp <= end:
                    keys.append((key, size, stamp))
            day += dt.timedelta(days=1)
        keys.sort(key=lambda k: k[2])
        print(f"[{case['id']}] {len(keys)} volumes in window {start.isoformat()}..{end.isoformat()}",
              file=sys.stderr)
        if len(keys) > max_vols and max_vols > 0:
            # Even sampling including both ends (same rule as in-app AWS dialog).
            idx = [round(i * (len(keys) - 1) / (max_vols - 1)) for i in range(max_vols)]
            keys = [keys[i] for i in sorted(set(idx))]
            print(f"[{case['id']}] sampled to {len(keys)} volumes", file=sys.stderr)
        case_dir = args.output / case["id"]
        if args.dry_run:
            for key, size, stamp in keys:
                print(f"  {stamp.isoformat()} {key} ({size / 1e6:.1f} MB)")
            continue
        case_dir.mkdir(parents=True, exist_ok=True)
        (case_dir / "volumes.json").write_text(
            json.dumps([{"key": k, "size": s, "time": t.isoformat()} for k, s, t in keys],
                       indent=2), encoding="utf-8")
        jobs = [(f"{BUCKET}/{urllib.parse.quote(k)}", case_dir / pathlib.PurePosixPath(k).name)
                for k, _, _ in keys]
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(args.workers, 6))) as pool:
            futures = [pool.submit(download_one, url, target) for url, target in jobs]
            for i, fut in enumerate(concurrent.futures.as_completed(futures), 1):
                name, fresh, size = fut.result()
                total_dl += int(fresh)
                if i % 4 == 0 or i == len(futures):
                    print(f"[{case['id']}] {i}/{len(futures)} {name} {'new' if fresh else 'cached'}",
                          file=sys.stderr)
    print(f"done downloaded_new={total_dl}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
