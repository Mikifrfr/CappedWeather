# Moment sentinel convention: below threshold vs missing

Recorded 2026-10-20 by decode (card `t_71ee1a27`). This is the contract a
consumer needs to read a packed gate correctly, and the evidence that each
decoder sets it.

## Why a chunk has to say which format it came from

Both decoded formats pack a reflectivity gate into a 16-bit code with a
`scale`/`offset` pair. Codes 0 and 1 are not measurements, but the two formats
put the two meanings the other way round:

| Format | code 0 | code 1 | first valid |
|---|---|---|---|
| NEXRAD Message 31 | below threshold | range folded / missing | 2 |
| OPERA ODIM_H5 (`odim_reader.cpp`) | missing (nodata) | below threshold (undetect) | 2 |

`nativeValue(encoded, scale, offset)` returns `nullopt` for both codes, which is
correct for value math and stays that way. The problem was that nothing recorded
*which* code meant what, so a consumer that needs the difference (rainfall
accumulation: a dry gate contributes zero depth, a coverage gap must be flagged)
had to guess. Guessing "both are no rain" reads a gap as a dry gate and
understates a total silently.

## API (`src/core/level2_decoder.hpp`)

```cpp
enum class MomentEncoding : std::uint8_t {
    Unknown = 0,      // not recorded (legacy spill file): <= 1 is "no data", kind unknown
    NexradMessage31,  // 0 = below threshold, 1 = range folded / missing
    Odim,             // 0 = missing / nodata, 1 = below threshold (undetect)
};

struct MomentSentinelCodes { std::uint16_t belowThreshold; std::uint16_t missing; };

std::optional<MomentSentinelCodes> momentSentinelCodes(MomentEncoding) noexcept;

enum class MomentGateState : std::uint8_t { Valid, BelowThreshold, Missing, Undetermined };
MomentGateState momentGateState(MomentEncoding, std::uint16_t encoded) noexcept;
```

`DecodedChunk::momentEncoding` carries the format. It is the last field of the
struct on purpose: existing aggregate initialisers (`DecodedChunk{"KTLX", {ray}, 1}`)
keep compiling.

`momentGateState` is the one call a consumer needs. For `Unknown` it returns
`Undetermined` for codes <= 1 — the old reading, without pretending to know
which code it saw.

## Who sets it

- `decodeMessage31Chunk` → `NexradMessage31`. Also covers the Archive-II path
  (`decodeLevel2Archive`) and live NEXRAD chunks (`nexrad_source`), since both
  call this decoder.
- `readOdimPvol` → `Odim`.
- `mergeDecoded` (Archive-II records) and `OperaSource::mergeScanParts` copy it
  and fall back to `Unknown` if two parts ever disagree.
- `spillVolume` appends one trailer byte after the last ray; `loadSpilledVolume`
  reads it when present. Files written before this change end at the last ray
  and load as `Unknown` — no version bump, old spill files stay readable. A
  chunk with `Unknown` is never upgraded by inference.
- Not yet propagated: `MainWindow::acceptDecodedChunk` merges chunks into
  `session->pendingVolume` and does not copy `momentEncoding`. Until it does,
  the assembled volume that reaches products is `Unknown`. Assigned to the
  manager (see card comment).

## Evidence

Committed fixtures, `tests/core_tests.cpp`:

| Test | Input | Result |
|---|---|---|
| `testMessage31SentinelConvention` | minimal single-radial Message 31 layout (ICD 2620001), not a volume | codes `{0,1,66}` → BelowThreshold, Missing, Valid |
| `testMessage31RealArchiveChunkSentinelConvention` | `nexrad_m31_kinx_20130520.bin`: 60 real radials cut verbatim from KINX20130520_201333_V06 (Moore, OK), elevation 2, transfer headers included | 60 messages, 0 rejected: below=60510, folded=101, valid=10909 |
| `testOdimSentinelConvention` | real DWD `odim_deneu_dbzh.h5` | codes 0/1 = missing/below; dense clear-air sweep: missing=0, below=259200, valid=0 |
| `testOdimSentinelConvention` | real OPERA `odim_nohur_dbzh.h5` | missing=0, below=1687021, valid=441299 |
| `testOdimMinimalContainerMixedSentinels` | `odim_minimal_mixed.h5`: minimal ODIM_H5 container per the ODIM spec, 1 ray × 4 gates, **not** a radar volume | gates `{0,1,106,126}` → Missing, BelowThreshold, Valid, Valid |
| `testMomentSentinelConventionFlipsBetweenFormats` | in-memory chunk, codes 0/1/66 | the same codes classify opposite ways under the two formats; `Unknown` gives Undetermined |
| `testSpillCarriesMomentEncoding` | spill round trip | Nexrad/Odim survive; Unknown writes no trailer and stays Unknown |

The real fixtures happen to contain no nodata gates at all (both are dense), so
the ODIM `missing` code is only exercised by the 2 KB spec-minimal container.
That file exists for that one path and is not evidence of any radar behaviour.

## Constraint kept

`nativeValue(...)` still returns `nullopt` for encoded <= 1 on both overloads.
Derived products (`appendDerivedProducts`, echo tops, VIL) depend on that and
were not touched.

## Open follow-ups

- science: `accumulateRainfall` must use `momentGateState` on
  `samples[i].chunk->momentEncoding` so only `Missing` gates set
  `RainfallGateIncomplete`; `BelowThreshold` stays zero depth with no flag.
  `Undetermined` (legacy spill) keeps today's behaviour: zero depth, no flag,
  because the gap cannot be claimed. Also strike limitation 8 in
  `docs/rainfall-validation.md`.
- manager: copy `momentEncoding` in `MainWindow::acceptDecodedChunk`.
