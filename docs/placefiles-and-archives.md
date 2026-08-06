# Placefiles and scoped Level II archives

CappedWeather supports two P2 interoperability workflows: a deliberately bounded GR-compatible placefile reader and case-study loading of NOAA Archive-II Level II volumes. Both feed the normal map/timeline pipeline rather than creating screenshot-only overlays or a separate demo viewer.

## GR-compatible placefiles

Use **GR placefiles → File…** in the control dock to open one or more local text files, or **URL…** to add an HTTP(S) source. Remote URLs are remembered without embedded passwords and refreshed automatically. The refresh interval comes from `RefreshSeconds:` or `Refresh:` and is clamped to 15–3600 seconds. Requests include the conventional `lat`, `lon`, and `version=1.2` query values when the source did not already provide them.

### Implemented subset

The parser is case-insensitive and supports the high-value ecosystem primitives used by chaser, spotter, and boundary feeds:

- `Title`, `Refresh`, `RefreshSeconds`, `Threshold`, and `Color` state;
- point labels through `Place` and `Text`;
- chaser/spotter positions through `Icon`;
- multi-point `Line ... End` boundaries;
- filled `Polygon ... End` boundaries;
- quoted comma-separated labels and hover text, plus semicolon comments.

`IconFile`, `Font`, `Object`, and `TimeRange` declarations are accepted as metadata so otherwise useful feeds do not fail. Icon sprite sheets are **not** downloaded or interpreted in this slice: an `Icon` is rendered as a colored, rotated triangular marker and retains its hover text. Unsupported directives produce parser warnings rather than aborting all valid features.

`Threshold` is evaluated against the visible map radius in nautical miles. Placefile markers, labels, lines, and polygons participate in map hit testing; clicking one shows its feed title, hover text, and credential-scrubbed source.

### Limits and safety

- Maximum source/response size: 2 MiB.
- Maximum drawable elements: 50,000.
- Maximum coordinates in one line/polygon block: 10,000.
- Coordinates and colors are range-checked.
- Remote sources must use HTTP or HTTPS, redirect only to an equally or more secure scheme, and time out with the normal network timeout.
- URLs containing passwords or token-like query parameters are session-only; credentials are redacted from map/status details and are never written to `QSettings`.
- A failed refresh leaves the last successfully parsed document on the map.

This is a useful read subset, not a claim of byte-for-byte compatibility with every vendor extension in the GR placefile ecosystem.

## Scoped Archive-II loading

The playback dock exposes three offline/case-study actions:

- **L2 files…** opens one or more local Archive-II files;
- **L2 folder…** opens every regular file in a chosen folder and lets invalid files fail independently;
- **AWS archive…** queries the public `unidata-nexrad-level2` bucket for one selected radar and UTC time window.

Local files must match the currently selected radar. This prevents a mislabeled file from silently entering the wrong radar session. Filename timestamps such as `KTLX20260729_000234_V06` are used as the timeline time; otherwise the local file modification time is used.

The AWS dialog defaults to the previous six hours and up to 24 evenly sampled volumes. Sampling includes both ends of the selected window. A request can span up to 31 days and lists each UTC date prefix directly over public HTTPS—no AWS credentials or cloud SDK are used.

Starting any archive action cancels live/archive acquisition for that radar and replaces its active timeline with a dedicated case-study timeline. Local file/folder actions keep the most recent 24 selected files; AWS exposes the same honest 24-frame in-memory ceiling. This prevents a large request from downloading hundreds of volumes only to silently discard all but the normal history limit. Deeper or disk-backed history remains a separate roadmap item.

### Decoder behavior

Archive decoding runs on worker threads. It:

1. validates the 24-byte `AR2V`/`ARCHIVE2` header and station ID;
2. reads signed big-endian Archive-II record lengths;
3. decompresses internal bzip2 records with per-record and whole-file safety limits;
4. decodes and merges real Message 31 radials;
5. publishes the volume through the same history, tilt, product, probe, and ET18/VIL derivation paths as live data.

The shared bounded bzip2 decompressor is also used by the real-time chunk source, so live and archive framing cannot drift into separate implementations.

Current scope intentionally excludes a national archive browser, automatic cross-site discovery, AWS credentials/private buckets, and outer `.gz` wrappers used by some legacy distributors. Current public Unidata Archive-II objects and ordinary unwrapped local Archive-II files are supported.

## Verification coverage

Automated tests cover stateful placefile parsing, quoted text/comments, malformed-coordinate rejection, map rendering/hit testing and visibility controls, synthetic Archive-II bzip framing with a real Message 31 radial, signed first-gate preservation, truncated-record rejection, AWS XML filtering/time selection, asynchronous local-file publication, and presence of all operator controls.

The optional `CAPPEDWEATHER_ARCHIVE_FILE` test hook decodes a real local Archive-II object. `CAPPEDWEATHER_TEST_AWS_ARCHIVE=1` exercises public AWS listing, download, worker-thread decoding, and ready-volume publication. The implementation was verified against a real KTLX object from `https://unidata-nexrad-level2.s3.amazonaws.com/`.

## References

- GRLevelX placefile format: <https://www.grlevelx.com/manuals/gis/files_places.htm>
- NOAA NEXRAD public data registry: <https://registry.opendata.aws/noaa-nexrad/>
- Public Archive-II bucket: <https://unidata-nexrad-level2.s3.amazonaws.com/>
