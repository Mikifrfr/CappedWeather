# Hail cores and debris signatures: validation

Status: shipped with literature thresholds plus archive scoring below.
Both layers are guidance only, never warnings.

## What they are

**Hail cores** (`src/core/hail_detector.cpp`): VIL-density proxy after
Amburn and Wolf 1997 and NWS Louisville guidance. VIL density is VIL
divided by echo-top height and normalizes out the airmass dependence
that makes raw VIL a per-day guessing game. Grid-based tiers:

| VIL density (g/m^3) | Meaning |
|---|---|
| 3.5-3.9 | severe possible (dime to nickel in the study sample) |
| 4.0-4.4 | likely severe (nickel to golfball) |
| 4.5+ | significant (golfball or larger possible) |

Guards: VIL at least 10 kg/m2, echo top at least 3000 m, collocated
reflectivity at least 50 dBZ, greedy 5 km peak clustering, cap 24.
Size labels are ranges from the study tables, never diameters. This is
NOT the Witt et al. MESH/SHI model: that needs freezing-level heights
we do not ingest, so no MEHS inches are claimed anywhere.

**Debris signature** (`src/core/debris_detector.cpp`): polarimetric TDS
after Ryzhkov et al. 2005, Schultz et al. 2012, and WDTB training:
CC minimum at or below 0.80 inside 2.5 km of a validated rotation
couplet, reflectivity at least 20 dBZ at the low-CC gate, and a
compact-hole contrast gate (surrounding annulus median CC exceeds the
inner minimum by 0.05+, which screens broad hail-shield depressions).
ZDR is recorded as extra confidence only and never gates. No CC moment
(TDWR, single-pol OPERA) means no output, never a guess. Markers need
the same 2-volume persistence as TVS/meso before they draw.

## How it is scored

`tools/rotation_replay` emits HAIL/TDS rows on the same three archive
cases as rotation (`data/rotation_cases/cases.json`), reusing the exact
live pipeline plus `appendDerivedProducts`. `tools/rotation_verify.py`
scores TDS against tornado reports and TOR warnings (`--kinds TDS`)
and HAIL against hail reports (`--report-kind hail --min-size-in 1.0`)
and severe warnings (`--phenomena TO,SV`), radius 15 km for reports,
polygon match for warnings, lead window [-10,+45] min, radar domain
150 km. Same semantics as `docs/rotation-validation.md`.

## Numbers (raw replay output, no persistence)

TDS vs in-domain tornado reports (r15, lead [-10,+45]) and TOR warning
polygons, shipped contrast 0.05:

| Case | n | Report POD / FAR / CSI | Warning POD / CSI |
|---|---|---|---|
| Mayfield KPAH (24 vols) | 3 | 0.08 / 0.33 / 0.08 (24 events) | 0.03 / 0.03 (32 events) |
| Moore KTLX (20 vols) | 55 | 0.50 / 0.98 / 0.02 (2 events) | 0.75 / 0.11 (8 events) |
| Hackleburg KHTX (24 vols, pre-dual-pol) | 0 | n/a (no CC moment) | n/a |

HAIL vs severe hail reports (>= 1 in, r15) and TO/SV warning polygons,
shipped cluster 10 km:

| Case | n | Report POD / CSI | Warning POD / CSI |
|---|---|---|---|
| Mayfield (53 at k5 -> 36 at k10) | 36 | 0.67 / 0.06 (3 events) | 0.15 / 0.09 (40 events) |
| Moore (155 -> 96) | 96 | 0.30 / 0.06 (23 events) | 0.95 / 0.20 (20 events) |
| Hackleburg (116 -> 76) | 76 | 0.26 / 0.10 (39 events) | 0.32 / 0.21 (87 events) |

## Reading these honestly

- TDS is a confirmer, not a predictor. The literature says the same:
  debris must already be lofted, so POD is structurally low and the
  value is precision on strong tornadoes (Moore TOR-warning POD 0.75
  at 6/8 warnings; Mayfield report CSI 0.08 against a rotation-layer
  SPC band around 0.15). Moore's 55 raw detections re-fire on the
  same long-lived debris ball across 20 volumes (replay scores raw
  output); live display adds 2-volume persistence plus a 12-track cap,
  so the map shows one or two markers, not 55. Report FAR is an upper
  bound: SPC tornado rows are sparse next to a 40-minute EF5 track.
- KHTX emits zero TDS rows: April 2011 predates the dual-pol upgrade,
  so there is no CC moment and the detector fails closed as designed.
- Hail cores find the warned storms (Moore warning POD 0.95,
  Hackleburg 0.32 across a multi-storm outbreak window) but emit a few
  cores per storm, so report/warning FAR is high by construction.
  Extra cores inside an already-warned storm are splitting
  granularity, not clear-air false alarms; the layer reads as hail
  footprint extent, and the tier colors (not the count) carry the
  severity message.
- Hail report truth is sparse (3 severe-hail events in the Mayfield
  domain window, overnight rural). Treat report FAR as an upper bound,
  warning POD as the fairer number.

## Contrast sweep (TDS)

`--tds-contrast` 0 / 0.05 / 0.10 on Moore + Mayfield: counts identical
(Moore 55, Mayfield 3) at every setting, so the gate has no tuning power
on these cases. The CC depressions around real couplets are already
compact holes at these ranges; broad shields either do not survive the
couplet collocation requirement or sit outside the 2.5 km disc. Shipped
default 0.05 stays as a principled guard (the synthetic broad-shield
test proves it fires), not as a tuned knob. Re-sweep any time new
dual-pol cases land in `cases.json`, especially a marginal-EF0/QLCS
case where hail overlap should be stronger.

## Cluster sweep (hail)

`--hail-cluster` 5 / 8 / 10 km, warning CSI (TO+SV polygons):

| Cluster | Mayfield CSI | Moore CSI | KHTX CSI |
|---|---|---|---|
| 5 | 0.069 | 0.122 | 0.187 |
| 8 | 0.078 | 0.171 | 0.204 |
| 10 (shipped) | 0.086 | 0.196 | 0.207 |

Warning POD is flat on Mayfield (0.15) and Moore (0.95) across all
three; KHTX costs 4 warning hits (32 to 28) going 5 to 10 while report
POD holds everywhere (2/3, 7/23, 10/39). One marker per ~10 km reads
as footprint extent instead of label soup, so 10 ships.

## Failed or deferred experiments

- CC 0.70 strict line: kills hail overlap but also the Mayfield
  confirmer hits (precipitation ingestion raises debris CC; WDTB
  allows up to 0.95 as a local minimum). 0.80 plus contrast kept.
- TVS-parents-only TDS: Mayfield's 3 report hits are all MESO-parented
  (debris lofted by a sub-TVS couplet at volume time). Parent-kind
  gating trades those for nothing measurable.
- Hail cluster choice is settled by the sweep above (10 km ships).
  Revisit only with new cases where distinct cells under 10 km apart
  both produce severe hail and the merged marker demonstrably hides one.
- Full MESH/SHI with model freezing levels: needs a sounding ingest
  path that does not exist yet. VIL-density proxy stays until then,
  and the UI never prints an MEHS inch value.
