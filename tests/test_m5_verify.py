import datetime as dt
import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from m5_verify import Jump, load_warnings, verify


class VerificationTest(unittest.TestCase):
    def test_polygon_hit_lead_and_false_alarm(self):
        issue = dt.datetime(2025, 5, 18, 23, 0, tzinfo=dt.timezone.utc)
        payload = {"type": "FeatureCollection", "features": [{
            "id": "TEST.TO.W.1",
            "properties": {"phenomena": "TO", "issue": issue.isoformat()},
            "geometry": {"type": "Polygon", "coordinates": [[
                [-100.2, 37.8], [-99.8, 37.8], [-99.8, 38.2], [-100.2, 38.2], [-100.2, 37.8]
            ]]}
        }]}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "warnings.geojson"
            path.write_text(json.dumps(payload), encoding="utf-8")
            warnings = load_warnings(path, {"TO"})
        jumps = [
            Jump(issue - dt.timedelta(minutes=20), 38.0, -100.0, 3.0, 30.0),
            Jump(issue - dt.timedelta(minutes=10), 38.8, -100.0, 2.2, 15.0),
        ]
        metrics = verify(jumps, warnings, radius_km=10, domain_margin_deg=1.0)
        self.assertEqual(metrics.hits, 1)
        self.assertEqual(metrics.false_alarm_jumps, 1)
        self.assertEqual(metrics.lead_median, 20.0)
        self.assertAlmostEqual(metrics.pod, 1.0)
        self.assertAlmostEqual(metrics.far, 0.5)

    def test_one_jump_cannot_hit_two_warnings(self):
        issue = dt.datetime(2025, 5, 18, 23, 0, tzinfo=dt.timezone.utc)
        ring = [[-100.2, 37.8], [-99.8, 37.8], [-99.8, 38.2],
                [-100.2, 38.2], [-100.2, 37.8]]
        payload = {"type": "FeatureCollection", "features": [
            {"id": "TEST.SV.W.1", "properties": {"phenomena": "SV", "issue": issue.isoformat()},
             "geometry": {"type": "Polygon", "coordinates": [ring]}},
            {"id": "TEST.TO.W.2", "properties": {"phenomena": "TO", "issue": issue.isoformat()},
             "geometry": {"type": "Polygon", "coordinates": [ring]}},
        ]}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "warnings.geojson"
            path.write_text(json.dumps(payload), encoding="utf-8")
            warnings = load_warnings(path, {"TO", "SV"})
        jump = Jump(issue - dt.timedelta(minutes=20), 38.0, -100.0, 3.0, 30.0)
        metrics = verify([jump], warnings, radius_km=10, domain_margin_deg=1.0)
        self.assertEqual(metrics.hits, 1)
        self.assertEqual(metrics.misses, 1)
        self.assertEqual(metrics.hit_jumps, 1)
        self.assertEqual(metrics.false_alarm_jumps, 0)


if __name__ == "__main__":
    unittest.main()
