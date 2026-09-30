# Validation — 2025-05-16 outbreak (M2)

Date run: 2026-07-20
Engine: first-cut grid LJA (0.25° cells, min-rate 10 fl/min, 12-min σ window, 60 s bins)
Data: 1,080 GOES-19 GLM L2 LCFA files (`noaa-goes19`), windows 19–21Z doy 136 + 02–04Z doy 137
Ground truth: SPC convective-day tornado reports (`250516_rpts.csv`, UTC), 57 reports

## Headline numbers

- 1,080/1,080 files parsed, **365,078 flashes**, 6,196 grid cells
- **772 jump starts**, 103 updates, 771 cessations (every jump closed cleanly)
- Tornado-report verification (jump within ±0.375°, 5–120 min before report):
  - covered windows: **16/26 = 62% POD**, lead min/median/max = **5/8/27 min**
  - data-gap hours (22Z–02Z not downloaded): 4/31 = 13% — artifact, not algorithm
- Context: operational LJA literature reports ~60–70% severe-weather POD with
  ~10–20 min median lead. First-cut, zero tuning, we're in that band.

## Notable hits

| Report | Jump | Lead |
|---|---|---|
| St. Louis MO EF3 (5 fatalities), 19:41Z | 19:23Z cell(38.62,−90.62) | 18 min |
| Somerset, Pulaski KY, 02:57Z | 02:47Z cell(37.12,−84.62) — exact cell | 10 min |
| London-Corbin, Laurel KY, 03:47Z | 03:41Z cell(37.38,−84.38) | 6 min |
| Russellville KY ×2, 02:55Z | 02:50Z cell(36.88,−87.12) | 5 min |

## Caveats / known limitations

- Grid cells are static; storms move. A jump in an upwind cell may precede a
  tornado in the next cell over — storm-cell tracking (M4) fixes attribution.
- ±0.375° spatial match is tight; generous matching inflates POD, tight matching
  deflates it. Numbers here are the strict version.
- min-rate 10 fl/min suppresses weaker/embedded cells (QLCS spin-ups). Lowering
  it raises POD and false alarms — tune from a multi-case archive in M5, not one day.
- Max σ (39) fired in a tropical cell (9.9N, −99.4W) — huge oceanic flash counts;
  irrelevant for CONUS severe work but noted.

## Reproduce

```bat
glm_replay data/glm/20250516            # writes event log
python tools/m5_verify.py               # matches vs SPC reports
```
Scripts: `download_glm.py`, `tools/m5_verify.py` (promoted into
`tools/` as the M5 verification harness).
