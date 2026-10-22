# Level III KDP + reflectivity pairing — the R(KDP) reference grid

Status: **decided** (card `t_f4d324e8`, 2026-10-22). Implementation: `t_cde4dc7d`
(decode); the single-tilt guard this exposes: `t_639665e7` (science). This
file is the decision record; it does not claim any validation of R(KDP) itself
(see `rainfall-validation.md`).

Supersedes the open acceptance line in `t_0d919180` ("KDP on rays that also carry
reflectivity at the same geometry"). The decoder was finished; the missing piece
was this product rule.

## Decision

**Option A, on the reflectivity grid.** Decode Level III product 153 (N0B, digital
base reflectivity, 0.5° × 0.25 km) and attach product 163 (N0K, digital specific
differential phase) to its 720 rays, one KDP radial per reflectivity ray, gates
copied by index. Every KDP value placed on the grid is a published RPG gate from
the same volume scan; nothing is interpolated, averaged, smoothed, or filled.

Two consequences are part of the decision, not accidents:

* The reference grid is the **reflectivity** product's geometry, so the rainfall
  engine's documented invariant ("the newest volume's lowest reflectivity tilt is
  the reference for every relation") keeps holding, and the grid keeps the full
  460 km reflectivity extent.
* KDP is 1° data presented on a 0.5° grid. That is **replication of the covering
  radial**, never interpolation, and the merged chunk is an analysis input only —
  see "Limits".

Rejected: **B** (teach the engine a KDP-only reference grid). R(KDP) would run
with no vertical guard at all — reflectivity is the guard's only column source —
and the dry-reflectivity veto would disappear, which matters on real data: 77.1%
of the gates in the volume's reflectivity product are below threshold. It also
breaks the engine's stated invariant and would put a second, weaker reference-grid
rule in `derived_products.cpp`.

Rejected: **C** (decode-only). The same-volume reflectivity product exists, is
public, and the pairing is measurable; leaving R(KDP) unusable on real data was
a waiting state, not a product position. Decode-only remains true of the *app*
until a live feed lands (see "Not decided here"), but not of the engine's
real-data test.

## Measured geometry (real files, 2026-10-22)

All four files from the public NOAA NODD bucket
`https://unidata-nexrad-level3.s3.amazonaws.com/` (unmodified downloads):

| file | product | vol | volume start | julian | VCP | tilt | rays × gates | spacing | range | bytes | sha256 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `TLX_N0K_2024_05_06_23_54_39` | 163 KDP | 77 | 86 079 s | 19 850 | 212 | #1 @ 0.5° | 360 × 1200 | 250 m | 300 km | 16 646 | `d7faa81f418ccfecaacdc32d33e0e48f6c2999efd29b2502fbb0ae30cbd3d03d` |
| `TLX_N0B_2024_05_06_23_54_39` | 153 Z | 77 | 86 079 s | 19 850 | 212 | #1 @ 0.5° | 720 × 1840 | 250 m | 460 km | 224 892 | `fdae5985e9e186ca507d4fbb08aa5e8f8dbe0dcaf3b34c6ef3e6cc9881bfd160` |
| `TLX_N0K_2024_05_06_23_44_57` | 163 KDP | 75 | 85 497 s | 19 850 | 212 | #5 @ 0.5° | 360 × 1200 | 250 m | 300 km | 16 159 | `89b5d87653139740b523f80bfbee71cfcf4a496dd7aa6e02a42379c56d32a841` |
| `TLX_N0B_2024_05_06_23_44_57` | 153 Z | 75 | 85 497 s | 19 850 | 212 | #5 @ 0.5° | 720 × 1840 | 250 m | 460 km | 224 587 | `1c40134c269819e9321df4e94a44f802015a1c5612d731b94c82e052d6c3c14a` |

The two N0K hashes are the fixtures committed by `t_0d919180`; the two N0B files
are new. The pair spans 86 079 − 85 497 = **582 s**, so a two-volume, one-interval
R(KDP) accumulation is possible with committed bytes alone.

Encoding, measured from the bytes:

* **163 (KDP):** product-description thresholds 1-2 are a float32 **scale 20.0**,
  3-4 a float32 **offset 43.0**, so KDP = (code − 43) / 20 deg/km. Max data
  level 243; codes 0/1 are the Message 31 sentinel pair (below threshold /
  range folded).
* **153 (Z):** the threshold table is (−320, 5, 254): code 2 is −32.0 dBZ, step
  0.5 dBZ, max code 254. So **dBZ = (code − 66) / 2**, which is bit-for-bit the
  app's reflectivity packing (`nativeValue`: (encoded − 66) / 2), and codes 0/1
  are the same sentinel pair. 1 324 800 gates: 1 021 330 code 0 (77.1% below
  threshold), 0 code 1, max code 196 (65.0 dBZ).
* Both products write `scale_factor 999` in the radial header, the same quirk the
  163 reader already handles: gate spacing comes from the product table.

Azimuth geometry, measured:

* The radial table starts at an arbitrary azimuth and tiles the circle with
  0.1°-quantised widths (Z: 0.4/0.5/0.6°, Σ = 359.9° in vol 77 and 360.0° in
  vol 75; KDP: 0.9/1.0/1.1°, Σ = 359.9° in both).
* Vol 77 starts Z at 119.0° and KDP at 144.0°; vol 75 starts Z at 239.0° and KDP
  at 264.0°. **Index parity is therefore invalid** — KDP radial *k* is not the
  pair of Z radials *2k/2k+1*. The mapping has to be computed from the decoded
  azimuths. This is the one part of the original card's wording ("drop every
  second radial" / "duplicate onto the two rays it covers") that was a guess; the
  files disagree with it.
* Nearest-centre mapping measured on both volumes: 360 of 360 KDP radials used,
  exactly **2 Z rays per KDP radial** (720 assignments), 0 unmapped, 0 ties,
  distance to the selected radial's centre max 0.35°, mean 0.25°.
* Range lattices are identical (first bin 0, 250 m), so KDP gates 0..1199 sit on
  the same ranges as Z gates 0..1199: index-for-index copy, no resampling.

Spot checks that the rule lands the right radial on the right echo:

| volume | Z ray (centre) | gate | Z | KDP radial (centre) | KDP |
|---|---|---|---|---|---|
| 77 | 404 (321.25°) | 492 (123.0 km) | code 183 = 58.5 dBZ | 177 (321.40°) | code 131 = 4.40 deg/km |
| 75 | 125 (301.70°) | 567 (141.75 km) | code 184 = 59.0 dBZ | 37 (301.45°) | code 114 = 3.55 deg/km |

Both are the KDP peak inside a 58-59 dBZ core, with the adjacent gates showing a
smooth KDP bump (vol 77: 112, 121, 128, 131, 129, 124, 116), which is what a
correct azimuth pairing looks like.

## The rule (normative)

`pairLevel3ReflectivityAndKdp(reflectivity, kdp, error)` returns the reflectivity
chunk with a KDP moment attached, or refuses by name.

1. **Identity.** Product codes 153 (reflectivity) and 163 (KDP); equal
   `volumeScanNumber`, `julianDay`, `volumeStartSeconds`, latitude, longitude,
   VCP, `elevationNumber`, and `elevationDegrees`. Any mismatch refuses the merge
   ("these two files are not the same volume scan") — pairing across volumes or
   tilts is never invented. Note the RPG tilt index is *not* a cross-volume
   constant: vol 77 is tilt #1, vol 75 is tilt #5.
2. **Grid.** The result starts as a copy of the reflectivity chunk: rays,
   azimuths, widths, elevation, tilt number, VCP, `radarId`, counters, moment
   convention. The reflectivity field is untouched.
3. **Range.** Require equal `firstGateByProduct` and `gateSpacingByProduct`
   between the two decoders' output, and `kdpGateCount ≤ zGateCount`. The KDP
   field is `kdpGateCount` gates long, gate *g* copied from KDP gate *g*. Gates
   past that length carry no KDP; nothing is extrapolated.
4. **Azimuth.** For each reflectivity ray, the KDP radial whose **centre azimuth
   is nearest** (circular distance), ties to the lower normalised centre azimuth.
   Require the distance ≤ 0.6° (measured max 0.35°); a ray with no KDP radial
   inside that bound gets no KDP field and the merge reports it by count rather
   than stretching the sweep.
5. **Coverage.** Require the KDP radial table to sample the full circle: Σ widths
   ≥ 359.0° and no gap between consecutive centres > 1.5°. A sector-only KDP
   product must not be stretched around the circle.
6. **Encoding.** KDP codes are copied in the app's 16-bit packing the 163 reader
   already produces (scale 1023.9375, offset 32768). They are never re-encoded
   or converted through a float and back.
7. **Provenance.** The result carries both headers, `kdpRadialPerRay` (one entry
   per ray) and the KDP product's per-ray azimuth widths, so a consumer can state
   the 1° sampling without guessing. The merged chunk is **not** installed as a
   display volume: no panel may read 0.5° cells off it while the moment behind
   them was measured at 1°.
8. **No invention.** No interpolation across radials or gates, no smoothing, no
   gap filling, no fallback to a coarser product. A missing KDP code stays a
   missing code.

## What the engine does with it (independent replay)

A Python replay of `accumulateRainfall`'s KDP path (same guards, same clamps,
same left-hold integration) over the two paired volumes, reference grid = vol 77,
one interval of 582 s:

* 720 reference rays × 1840 gates; `relationAvailable` true; `sampleCount` 2;
  `windowSeconds` = `coveredSeconds` = 582 s; `intervalCount` 1.
* gates with depth > 0: **10 731** (0.81% of the grid);
* max **20.15 mm** at reference ray 364 (chunk index), gate 567 — 141.75 km,
  azimuth 301.25°. Ray 365 (301.75°) carries the same KDP radial and the same
  depth, so the engine's first-in-grid tie-break settles it; both are real.
* mean over depth gates **4.10 mm** (sum over gates 44 019.0 mm, not an area
  total);
* `noVerticalCheckGateCount` **5 982**, `incompleteGateCount` 0,
  `beamHighGateCount` 95 760 (133 gates/ray, 197.0-230.0 km),
  `rangeLimitedGateCount` 661 680 (919 gates/ray, ≥ 230.25 km);
* gates reached but not credited: dry reflectivity 8 055, dry KDP 501 394,
  |KDP| below the 0.1 deg/km floor 29 363; no rate ever hit the 150 mm/h cap;
* quality **Marginal** (2 samples < 4 and the vertical-check flags), valid true.

These are expectations, not a scored result: the implementation card requires the
engine's own numbers to be measured and pinned, and any difference from this
replay to be explained rather than absorbed.

## Limits this decision creates, and they stay visible

1. **1° KDP on a 0.5° grid.** Two adjacent reflectivity rays carry the same KDP
   radial's gates. Areal totals do not double (720 × 0.5° = 360 × 1°), and the
   mean over gates is unchanged, but the product must never be described as 0.5°
   KDP. The merged chunk is an analysis input; a future KDP panel reads the
   original 163 chunk (1° rays), not this one.
2. **The vertical guard has no column.** A 153+163 pair is a single tilt. At 0.5°
   the beam passes 3 000 m at gate 655 (163.75 km) and 4 000 m at gate 788
   (197.0 km), so strong echoes in gates 655-787 (133 gates per ray,
   163.75-196.75 km) would pass the AP/clutter check without a column and
   *without a flag*, while closer gates correctly say `NoVerticalCheck` and
   farther ones are refused as too high (gates 788-920, 133 per ray). A
   single-tilt chunk cannot run a vertical check at all; science owns making
   that say so (follow-up card from `t_f4d324e8`). The replay's 5 982
   `NoVerticalCheck` gates are the honest part.
3. **No correlation coefficient.** The Level III pair carries no CC, so the
   `NonMeteorological` veto cannot run on it. Product 162 (N0C) is a separate,
   later pairing decision.
4. **R(KDP) is still unscored.** No gauge or MRMS comparison exists
   (`rainfall-validation.md`). This decision makes the relation runnable on real
   data; it does not validate it.

## Not decided here

* **Live attachment.** The app's session chunks are Level II volumes (multi-tilt
  Z, first gate from the radial header, not 0). Whether to fetch 163 per volume
  and attach it to the session chunk, or to fetch the 153 pair, is a feed
  decision (volume matching, latency, download budget). A Level II target also
  needs a *range* selection rule (its gate lattice starts at its own first gate,
  so KDP gates cannot be copied index-for-index). No code may assume that path
  until a card decides it.
* Whether the rainfall readout should state the KDP sampling resolution; it is
  text-only today and there is no KDP map panel yet.
* Scoring, and any change to the relation constants.

## Evidence

Every number above was reproduced from the four files by parsing the NIDS header,
the product-description block, the bzip2-compressed symbology block and packet 16
radials directly (stdlib `bz2`/`struct`, no C++, no MetPy), and by replaying the
engine's documented gate rules. The files are pinned by sha256 in the table, so
the measurements are re-derivable byte for byte from the public bucket.
