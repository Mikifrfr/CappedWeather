#!/usr/bin/env python3
"""Generate tests/fixtures/odim_specshape_dualpol_3ray.h5.

This fixture is NOT radar data and must never be presented as such. It is a
minimal, hand-constructed ODIM_H5 container whose only job is to pin the
decoder's *layout* handling: several moment data groups inside one dataset,
the U-prefixed (unfiltered) quantity aliases, and a dataset whose only moment
is one the app has no product slot for (KDP).

Gate values are chosen constants, not observations. Every physical value the
test asserts is derived from what/gain/what/offset below, so the test proves
the decode path, not the weather.

Layout follows EUMETNET OPERA ODIM_H5 v2.4.2:
  - datasetN/where   carries elangle/nbins/nrays/rstart/rscale/a1gate
  - datasetN/how     carries the per-ray startazA/stopazA arrays (one entry
                     per ray, len == where/nrays)
  - datasetN/dataM/what carries quantity/gain/offset/nodata/undetect
  - datasetN/dataM/data is a [nrays, nbins] integer array

Regenerate with:  python make_odim_dualpol_specshape.py
Requires h5py + numpy (regeneration only; the tests only read the .h5).
"""

import os

import h5py
import numpy as np

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "odim_specshape_dualpol_3ray.h5")

NRAYS = 3
NBINS = 4
ELANGLE_DEG = 1.5
RSTART_KM = 0.0
RSCALE_M = 500.0


def moment(group, index, quantity, gain, offset, nodata, undetect, raw):
    data_group = group.create_group(f"data{index}")
    what = data_group.create_group("what")
    what.attrs["quantity"] = np.bytes_(quantity)
    what.attrs["gain"] = np.float64(gain)
    what.attrs["offset"] = np.float64(offset)
    what.attrs["nodata"] = np.float64(nodata)
    what.attrs["undetect"] = np.float64(undetect)
    data_group.create_dataset("data", data=np.asarray(raw, dtype=np.uint16),
                              dtype="uint16")


def dataset(root, index, elangle, startaz, quantities):
    group = root.create_group(f"dataset{index}")
    where = group.create_group("where")
    where.attrs["elangle"] = np.float64(elangle)
    where.attrs["nbins"] = np.int64(NBINS)
    where.attrs["nrays"] = np.int64(NRAYS)
    where.attrs["rstart"] = np.float64(RSTART_KM)
    where.attrs["rscale"] = np.float64(RSCALE_M)
    where.attrs["a1gate"] = np.int64(0)
    how = group.create_group("how")
    how.attrs["startazA"] = np.asarray(startaz, dtype=np.float64)
    how.attrs["stopazA"] = np.asarray(
        [(a + 360.0 / NRAYS) % 360.0 for a in startaz], dtype=np.float64)
    how.attrs["NI"] = np.float64(12.5)
    what = group.create_group("what")
    what.attrs["product"] = np.bytes_("SCAN")
    what.attrs["startdate"] = np.bytes_("20260101")
    what.attrs["starttime"] = np.bytes_("000000")
    for i, (quantity, gain, offset, nodata, undetect, raw) in enumerate(quantities, 1):
        moment(group, i, quantity, gain, offset, nodata, undetect, raw)


def main():
    with h5py.File(OUT, "w") as root:
        root.attrs["Conventions"] = np.bytes_("ODIM_H5/V2_2")
        what = root.create_group("what")
        what.attrs["object"] = np.bytes_("PVOL")
        what.attrs["version"] = np.bytes_("H5rad 2.2")
        # Not a site: the source string states plainly that this is synthetic
        # scaffolding, so nobody can mistake the file for an observation.
        what.attrs["source"] = np.bytes_("SPECSHAPE:not-an-observation")
        what.attrs["date"] = np.bytes_("20260101")
        what.attrs["time"] = np.bytes_("000000")
        where = root.create_group("where")
        where.attrs["lat"] = np.float64(0.0)
        where.attrs["lon"] = np.float64(0.0)
        where.attrs["height"] = np.float64(0.0)

        # dataset1: standard (filtered) quantity names, four moments in one
        # dataset, each 3 rays x 4 gates.
        # DBZH: gain 0.5, offset -32 -> raw 100 = 18 dBZ, raw 200 = 68 dBZ,
        #       raw 0 = undetect (below threshold), raw 65535 = nodata.
        # ZDR:  gain 0.01, offset -5 -> raw 1000 = 5 dB, raw 0 = undetect,
        #       raw 65535 = nodata, raw 500 = 0 dB.
        # RHOHV gain 1/65535, offset 0 -> raw 65534 ~ 0.99998, 0 = undetect,
        #       65535 = nodata, 32768 ~ 0.5.
        # PHIDP gain 0.01, offset -180 -> raw 18000 = 0 deg, 0 = undetect,
        #       65535 = nodata, 27000 = 90 deg.
        dbzh = [[100, 200, 0, 65535]] * NRAYS
        zdr = [[1000, 0, 65535, 500]] * NRAYS
        rhohv = [[65534, 0, 65535, 32768]] * NRAYS
        phidp = [[18000, 0, 65535, 27000]] * NRAYS
        dataset(root, 1, ELANGLE_DEG, [0.0, 120.0, 240.0], [
            ("DBZH", 0.5, -32.0, 65535.0, 0.0, dbzh),
            ("ZDR", 0.01, -5.0, 65535.0, 0.0, zdr),
            ("RHOHV", 1.0 / 65535.0, 0.0, 65535.0, 0.0, rhohv),
            ("PHIDP", 0.01, -180.0, 65535.0, 0.0, phidp),
        ])

        # dataset2: the same three dual-pol moments under their U-prefixed
        # (unfiltered) ODIM v2.4 identifiers, to pin the alias fallback.
        uzdr = [[800, 0, 65535, 700]] * NRAYS      # 3 dB, undetect, nodata, 2 dB
        urhohv = [[32768, 0, 65535, 65534]] * NRAYS  # ~0.5, undetect, nodata, ~1
        uphidp = [[9000, 0, 65535, 18000]] * NRAYS  # -90 deg, undetect, nodata, 0
        dataset(root, 2, 2.5, [10.0, 130.0, 250.0], [
            ("UZDR", 0.01, -5.0, 65535.0, 0.0, uzdr),
            ("URHOHV", 1.0 / 65535.0, 0.0, 65535.0, 0.0, urhohv),
            ("UPHIDP", 0.01, -180.0, 65535.0, 0.0, uphidp),
        ])

        # dataset3: only KDP. There is no ProductKind for specific
        # differential phase yet, so this dataset must be rejected (counted,
        # not silently folded into another product).
        ukdp = [[100, 0, 65535, 200]] * NRAYS
        dataset(root, 3, 3.5, [20.0, 140.0, 260.0], [
            ("UKDP", 0.01, 0.0, 65535.0, 0.0, ukdp),
        ])

    print("wrote", OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main()
