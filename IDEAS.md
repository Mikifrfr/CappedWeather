# IDEAS — proposals that respect the honesty bar

Not a roadmap. A shortlist of things worth building, each scored against the two
constraints that actually decide feasibility here: **does real data exist for it
today** and **can it be proven hermetically**. Anything that fails either one is
listed at the bottom as a non-idea, so nobody re-proposes it next month.

Scoring: effort S/M/L, risk = chance of a wrong-looking-but-plausible result
(the failure mode this app exists to avoid).

---

## A. Export provenance — make an export unable to lie

**Effort S · Risk S · Data: already in memory**

Every exported artifact (PNG snapshot, KML frame, GIF/WebM, cross-section CSV)
currently carries pixels or numbers with no record of what produced them. A PNG
pasted into a briefing is indistinguishable from one taken of a different product,
tilt, or a derived field that was still re-deriving.

Write the provenance into the artifact itself:
- PNG: `tEXt` chunks — radar, volume id, scan time UTC, product kind, tilt,
  palette name, derived yes/no, CAPPI height when applicable.
- KML: the existing GroundOverlay description field.
- CSV: `#` header lines above the table body.
- GIF/WebM: a sidecar `.txt` next to the file (frame containers have nowhere
  clean to put it).

**First test:** build the provenance block from a synthetic `RadarFrame` and
assert field-by-field that the parser reads back exactly what it wrote, including
the "derived, CAPPI 3000 m" case. No disk, no Qt GUI.

**Why now:** `frameWorldFile()` already established that exports get a sidecar
when they need one. This is the same discipline applied to meaning instead of
geometry.

---

## B. "Why is this disabled?" — explanations instead of silent absence

**Effort S · Risk S · Data: already computed**

A greyed-out VEL or ZDR chip currently says nothing. The user cannot tell a
single-pol site from an OPERA volume whose dual-pol has not landed from a frame
that failed to decode. All three look identical.

The new availability mask makes this nearly free: when a product is unavailable,
name the reason in the chip tooltip and the product label — "no dual-pol in this
volume", "not carried by this station", "volume failed to decode", "no volume
shown". A short `availableProductReason()` beside `availableProductMask()`.

**First test:** every reason variant is reachable from a constructed
`DecodedChunk` plus a `RadarFrame` state, asserted as exact strings, and a guard
that no product renders disabled with an empty reason.

**Why now:** the mask landed today. Without this, the mask is invisible
machinery; with it, the honesty bar reaches the UI.

---

## C. Decode health panel — surface the counters we already throw away

**Effort S/M · Risk S · Data: already decoded**

`DecodedChunk` already counts `messageCount`, `rejectedMessages`, and
`rejectedBlocks`, and `RadarFrame` already counts `chunkCount` and
`failedChunks`. Almost none of this reaches the user. A partial volume that lost
messages to a bad uplink looks exactly like a clean one that has a data gap.

One panel: per-volume decoded/rejected counts, reject ratio, per-chunk failure
list, and the honest "how much of this volume did we actually get" figure.

**First test:** a chunk with a known number of valid and corrupted messages
produces the exact expected counts; the ratio math is a closed-form test.

**Why now:** zero new data sources, zero new dependencies. The decoder work that
would make this interesting is already done.

---

## D. Cache and network ledger — what the app actually fetched

**Effort M · Risk S · Data: already happening, unrecorded**

There are at least five caches (basemap tiles, NWS zone geometry, GOES grids,
warning feeds, offline packs) and no single answer to "how much data did this
session pull?" For a field user on a phone tether, that is the number that
matters, and the data-saver preset currently changes behaviour without ever
showing its effect.

A read-only ledger: per-source requests, cache hits, bytes, and the effect of the
data-saver preset. Explicitly a measurement surface, not a throttle.

**First test:** the counters are a plain struct; assert increments and the hit
ratio arithmetic. The UI only renders it.

**Why now:** the zone disk cache work already proved the request reduction
(~3.4x) by hand. This makes that permanent rather than a one-off probe result.

---

## E. Storm report vs detection scoreboard — keep the markers honest

**Effort M · Risk M · Data: both feeds already live**

The app draws TVS/mesocyclone markers and separately draws SPC storm reports.
Nobody checks them against each other. That is exactly how a detector drifts into
"guidance" that nobody has validated since the last tuning change.

A quiet scoreboard in the scan diagnostics: for the loaded day, detections that
have a nearby report within N minutes, detections with none, and reports with no
detection. Publish a miss rate instead of a hit count — hit counts flatter.

**First test:** hand-built detection and report sets with known match/no-match
outcomes, including the off-by-one on the time window.

**Risk note:** this can turn into a false-precision project. It stays a count of
matches at a stated window, never a "detection probability", until there is a
season of data.

---

## Not ideas (re-proposed every few months; record the verdict here)

- **GPU volume rendering** — designed in `gpu-volume-rendering.md`, not started.
  Blocked on a shared `VolumeBrick` refactor that is itself a project. Not a
  quick win, do not slip it into a polish pass.
- **ML nowcasting** — needs labeled archive infrastructure that does not exist.
  Motion extrapolation is already at the honest limit.
- **Level III as a second product path** — competing formats for the same
  question; Level II stays first.
- **Mosaic as a shipping layer** — the ~8-10 dB seam floor is beam physics, not
  a bug. Stays a probe until per-gate blending beats CAPPI on the paired-cell
  test (`docs/mosaic-seams.md`).
- **Fake "loading" echo / skeleton frames** — violates the honesty bar outright.
