#!/usr/bin/env python3
"""Write a Leaflet map HTML for one case: detections + TOR polygons.

Base layer is Esri World Street Map (no key, file://-safe with attribution):
OSM tiles 403-block local files and CARTO now demands an API key.
(TVS / MESO / ROT+SHEAR) with ROT off by default so the tornado markers
are visible under the spray; TOR polygons are toggleable too.
"""
import csv
import json
import pathlib
import sys

case = sys.argv[1] if len(sys.argv) > 1 else 'data/rotation_cases/20130520-ktlx-moore'
det_file = sys.argv[2] if len(sys.argv) > 2 else 'detections-v3.csv'
case_p = pathlib.Path(case)
cfg = next(c for c in json.loads((case_p / '..' / 'cases.json').read_text())['cases']
           if c['id'] == case_p.name)

dets = list(csv.DictReader((case_p / det_file).open()))
warn = json.loads((case_p / 'warnings.geojson').read_text()) if (case_p / 'warnings.geojson').exists() else {}
feats = []
for f in warn.get('features', []):
    if str(f.get('properties', {}).get('phenomena', '')).upper() != 'TO':
        continue
    feats.append({'id': str(f.get('id')), 'geom': f.get('geometry'),
                  'issue': f.get('properties', {}).get('issue')})

rows = [{'t': d['volume_time'], 'k': d['kind'], 'lat': float(d['lat']),
         'lon': float(d['lon']), 'dv': float(d['delta_v_mps'])} for d in dets]

html = f"""<!DOCTYPE html><html><head><meta charset="utf-8"><title>{case_p.name} rotation</title>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css">
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<style>#m{{height:95vh}}</style></head><body>
<div id="m"></div><script>
var map = L.map('m').setView([{cfg['radar_lat']},{cfg['radar_lon']}], 8);
L.tileLayer('https://server.arcgisonline.com/ArcGIS/rest/services/World_Street_Map/MapServer/tile/{{z}}/{{y}}/{{x}}', {{
  maxZoom: 12,
  attribution: 'Esri, HERE, Garmin, OpenStreetMap contributors'
}}).addTo(map);
L.marker([{cfg['radar_lat']},{cfg['radar_lon']}]).addTo(map).bindPopup("{cfg['radar']} radar");
var gTvs = L.layerGroup(), gMeso = L.layerGroup(), gRot = L.layerGroup(), gTor = L.layerGroup();
var dets = {json.dumps(rows)};
dets.forEach(function(d){{
  var isTvs = d.k=='TVS', isMeso = d.k=='MESO';
  var c = isTvs ? 'red' : (isMeso ? 'orange' : (d.k=='SHEAR' ? 'purple' : 'pink'));
  var m = L.circleMarker([d.lat,d.lon],{{radius: isTvs?7:5, color:c, weight:2, fillOpacity:0.3}})
    .bindPopup(d.k+' '+d.dv+' m/s<br>'+d.t);
  (isTvs ? gTvs : (isMeso ? gMeso : gRot)).addLayer(m);
}});
var polys = {json.dumps(feats)};
polys.forEach(function(f){{
  L.geoJSON(f.geom, {{style:{{color:'red', weight:1, fillOpacity:0.05}}}})
    .bindPopup('TOR '+f.id+'<br>'+f.issue).addTo(gTor);
}});
gTvs.addTo(map); gMeso.addTo(map); gTor.addTo(map);
L.control.layers(null, {{'TVS': gTvs, 'MESO': gMeso, 'ROT+SHEAR (off: spray)': gRot, 'TOR polygons': gTor}}).addTo(map);
</script></body></html>"""
out = case_p / 'map.html'
out.write_text(html)
print('wrote', out, len(dets), 'dets', len(feats), 'TOR polys')
