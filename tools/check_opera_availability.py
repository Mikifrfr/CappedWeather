"""Check which OPERA bucket sites are actually sharing volumes right now.

Lists the anonymous CloudFerro bucket (no credentials) for today + yesterday
and prints the ODIM site codes with objects under YYYY/MM/DD/CC/<site>/,
grouped by country segment. Human-diffs against tools/generate_opera_sites.py
(EXCLUDED_COUNTRY_CODES / EXTRA_ACTIVE_SITES / RENAMED_SITES) to decide the
next catalog regeneration.

Usage:
    python tools/check_opera_availability.py [--days 2]

Output: one line per sharing country segment, e.g. "DE: deasb,deboo,...".
Segments sharing nothing leave no trace in S3 (no child prefixes), so a
country absent from the output is dark. Diff the output against
tools/generate_opera_sites.py: an EXCLUDED_COUNTRY_CODES member that
reappears here has resumed sharing (un-exclude it); a site code here that
is missing from the regenerated catalog is an addition candidate.
"""

import datetime as dt
import re
import sys
import urllib.parse
import urllib.request

BUCKET = "https://s3.waw3-1.cloudferro.com/openradar-24h"


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "CappedWeather availability check"})
    return urllib.request.urlopen(req, timeout=30).read().decode("utf-8", errors="ignore")


def child_prefixes(prefix):
    """Site-level children of a prefix via delimiter listing (all pages)."""
    children = set()
    start_after = ""
    while True:
        url = (BUCKET + "?list-type=2&prefix=" + urllib.parse.quote(prefix) +
               "&max-keys=100&delimiter=/")
        if start_after:
            url += "&start-after=" + urllib.parse.quote(start_after)
        data = get(url)
        prefixes = re.findall(r"<Prefix>(.*?)</Prefix>", data)
        # First Prefix is the query echo; the rest are CommonPrefixes children.
        for p in prefixes[1:]:
            parts = p.strip("/").split("/")
            if len(parts) == len(prefix.strip("/").split("/")) + 1:
                children.add(parts[-1])
        if "<IsTruncated>true</IsTruncated>" not in data or not prefixes:
            break
        start_after = prefixes[-1]
    return children


def main():
    days = int(sys.argv[sys.argv.index("--days") + 1]) if "--days" in sys.argv else 2
    today = dt.datetime.now(dt.timezone.utc).date()
    dates = [(today - dt.timedelta(days=n)).strftime("%Y/%m/%d") for n in range(days)]
    # Country segments: probe the union seen recently (cheap: one delimited
    # listing of a date prefix reveals its countries).
    segments = set()
    for d in dates:
        segments |= child_prefixes(d + "/")
    for segment in sorted(segments):
        if len(segment) != 2 or not segment.isupper():
            continue
        sites = set()
        for d in dates:
            sites |= child_prefixes("%s/%s/" % (d, segment))
        print("%s: %s" % (segment, ",".join(sorted(sites)) if sites else "(empty)"))


main()
