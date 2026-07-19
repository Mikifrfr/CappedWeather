/* ═══════════════ CappedWeather Console — prototype logic ═══════════════ */
'use strict';

/* ── data ─────────────────────────────────────────────────────────── */
const STATIONS = [
  { id: 'KEWX', name: 'Austin/San Antonio', type: '88D',  lat: 29.7040, lon: -98.0289, elev: 204, fav: true  },
  { id: 'KGRK', name: 'Fort Hood / Central TX', type: '88D', lat: 30.7218, lon: -97.3829, elev: 168, fav: false },
  { id: 'KDFX', name: 'Laughlin AFB / Del Rio', type: '88D', lat: 29.2728, lon: -100.2806, elev: 348, fav: false },
  { id: 'KHGX', name: 'Houston / Galveston', type: '88D', lat: 29.4719, lon: -95.0789, elev: 5, fav: true },
  { id: 'KSJT', name: 'San Angelo', type: '88D', lat: 31.3711, lon: -100.4925, elev: 575, fav: false },
  { id: 'KCRP', name: 'Corpus Christi', type: '88D', lat: 27.7839, lon: -97.5108, elev: 14, fav: false },
  { id: 'KBRO', name: 'Brownsville', type: '88D', lat: 25.9156, lon: -97.4186, elev: 8, fav: false },
  { id: 'TIAH', name: 'Houston Intl', type: 'TDWR', lat: 30.0100, lon: -95.3700, elev: 33, fav: false },
];

const PRODUCTS = [
  { key: '1', name: 'Reflectivity',        code: 'REF', unit: 'dBZ' },
  { key: '2', name: 'Radial Velocity',     code: 'VEL', unit: 'm/s' },
  { key: '3', name: 'Correlation Coeff.',  code: 'CC',  unit: 'ρhv' },
  { key: '4', name: 'Differential Refl.',  code: 'ZDR', unit: 'dB'  },
  { key: '',  name: 'Storm-Relative Vel.', code: 'SRM', unit: '', soon: true },
  { key: '',  name: 'Spectrum Width',      code: 'SW',  unit: '', soon: true },
];

const TILTS = ['0.5', '0.9', '1.3', '1.8', '2.4', '3.1'];

const ALERTS = [
  {
    sev: 'tor', tag: 'TOR', name: 'Tornado Warning', area: 'Guadalupe & Comal Co., TX',
    exp: 'until 19:45 · <b>in 14 min</b>',
    headline: 'Radar indicated rotation. A severe thunderstorm capable of producing a tornado was located 3 mi SSE of New Braunfels, moving NE at 25 mph.',
    poly: [[29.62,-97.96],[29.66,-97.78],[29.55,-97.66],[29.44,-97.76],[29.48,-97.94]],
  },
  {
    sev: 'svr', tag: 'SVR', name: 'Severe T-storm Warning', area: 'Bexar Co., TX',
    exp: 'until 20:00 · <b>in 29 min</b>',
    headline: '60 mph wind gusts and quarter size hail. Hail damage to vehicles is expected. Expect wind damage to roofs and siding.',
    poly: [[29.78,-98.72],[29.75,-98.30],[29.42,-98.28],[29.38,-98.68]],
  },
  {
    sev: 'ffw', tag: 'FFW', name: 'Flash Flood Warning', area: 'Travis Co., TX',
    exp: 'until 22:30',
    headline: 'Doppler radar indicated thunderstorms producing heavy rain. Between 1 and 2 inches have fallen; 1 additional inch expected.',
    poly: [[30.28,-98.02],[30.30,-97.68],[30.02,-97.62],[29.98,-97.98]],
  },
];

const REPORTS = [
  { g: 'tor',  type: 'Tornado',      loc: '3 SSE New Braunfels, TX', mag: 'EFU',    time: '19:12', lat: 29.660, lon: -97.720 },
  { g: 'hail', type: 'Hail',         loc: 'New Braunfels, TX',       mag: '1.75″',  time: '19:05', lat: 29.703, lon: -98.125 },
  { g: 'wind', type: 'Wind gust',    loc: 'San Antonio Intl, TX',    mag: '68 mph', time: '18:58', lat: 29.534, lon: -98.470 },
  { g: 'hail', type: 'Hail',         loc: 'Seguin, TX',              mag: '1.00″',  time: '18:52', lat: 29.569, lon: -97.965 },
  { g: 'wind', type: 'Wind gust',    loc: 'Schertz, TX',             mag: '61 mph', time: '18:47', lat: 29.552, lon: -98.270 },
];

const SEV_COLORS = { tor: '#ff4040', svr: '#ffa940', ffw: '#f472b6' };
const GLYPH = { tor: '▲', hail: '●', wind: '■' };
const GLYPH_COLOR = { tor: '#ff3b4d', hail: '#53d769', wind: '#44a7ff' };

/* ── palettes ─────────────────────────────────────────────────────── */
const PALETTES = {
  REF: { min: 12, ticks: ['-10', '10', '30', '50', '75'], stops: [[-32,'#646464'],[-10,'#04e9e7'],[5,'#019ff4'],[15,'#0300f4'],[20,'#02fd02'],[30,'#008e00'],[40,'#fdf802'],[50,'#fd9500'],[55,'#fd0000'],[65,'#bc00bc'],[75,'#f8f8f8']] },
  VEL: { min: -80, ticks: ['-80', '-40', '0', '40', '80'], stops: [[-80,'#02fd02'],[-60,'#02c402'],[-40,'#019a01'],[-20,'#016301'],[-8,'#0d3d0d'],[-1.5,'#2b2b2b'],[1.5,'#3d0d0d'],[8,'#630101'],[20,'#9a0101'],[40,'#fd0000'],[60,'#fd6a6a'],[80,'#f8f8f8']] },
  CC:  { min: 0.6, ticks: ['0.6', '0.7', '0.8', '0.9', '1.0'], stops: [[0.6,'#23282e'],[0.75,'#3f5f8a'],[0.82,'#2e8fa3'],[0.88,'#35b57a'],[0.92,'#c9c25a'],[0.95,'#e08a3c'],[0.97,'#d84a4a'],[0.985,'#e07ac0'],[1.0,'#f8f8f8']] },
  ZDR: { min: -2,  ticks: ['-2', '0', '2', '4', '6'], stops: [[-2,'#6a3ec0'],[-1,'#3b5fd0'],[0,'#2a8fd8'],[0.5,'#22c3b5'],[1,'#3fd05a'],[2,'#cfe03c'],[3,'#f0a03c'],[4,'#e04a30'],[6,'#c01560']] },
};

function hexRgb(h) { return [parseInt(h.slice(1,3),16), parseInt(h.slice(3,5),16), parseInt(h.slice(5,7),16)]; }
function samplePalette(code, v) {
  const p = PALETTES[code], s = p.stops;
  if (v <= s[0][0]) return hexRgb(s[0][1]);
  if (v >= s[s.length-1][0]) return hexRgb(s[s.length-1][1]);
  for (let i = 0; i < s.length - 1; i++) {
    if (v >= s[i][0] && v <= s[i+1][0]) {
      const t = (v - s[i][0]) / (s[i+1][0] - s[i][0]);
      const a = hexRgb(s[i][1]), b = hexRgb(s[i+1][1]);
      return [a[0]+(b[0]-a[0])*t, a[1]+(b[1]-a[1])*t, a[2]+(b[2]-a[2])*t];
    }
  }
  return [0,0,0];
}

/* ── echo field simulation (relative to active radar, km E / km N) ── */
function hash(x, y, seed) {
  const s = Math.sin(x * 12.9898 + y * 78.233 + seed * 37.719) * 43758.5453;
  return s - Math.floor(s);
}
const BLOBS = [
  { x: 26, y: 24, sx: 13, sy: 13, p: 58 },   // main core
  { x: 31, y: 15, sx: 7,  sy: 7,  p: 63 },   // hook
  { x: 36, y: 10, sx: 4,  sy: 4,  p: 55 },   // hook tip
  { x: -32, y: -38, sx: 10, sy: 9,  p: 52 }, // line cells SW
  { x: -58, y: -64, sx: 9,  sy: 8,  p: 47 },
  { x: -84, y: -90, sx: 8,  sy: 7,  p: 43 },
  { x: 0,   y: 95,  sx: 70, sy: 30, p: 38 }, // stratiform shield N
];
function gauss(x, y, b) {
  const dx = (x - b.x) / b.sx, dy = (y - b.y) / b.sy;
  return b.p * Math.exp(-(dx*dx + dy*dy));
}
function fieldREF(x, y, r, seed, mx, my) {
  let v = 0;
  for (const b of BLOBS) v = Math.max(v, gauss(x - mx, y - my, b));
  if (r < 6) v = Math.max(v, 18 + hash(x, y, seed) * 6);          // ground clutter
  v -= r * 0.012;                                                  // range attenuation
  v += hash(Math.round(x*2), Math.round(y*2), seed) * 5 - 2.5;     // speckle
  return v;
}
function field(product, x, y, r, az, seed, mx, my) {
  const dbz = fieldREF(x, y, r, seed, mx, my);
  if (product === 'REF') return dbz;
  if (dbz < 9) return NaN;
  if (product === 'VEL') {
    let v = 34 * Math.sin(az - 1.05) * Math.min(1, r / 30);
    const h = BLOBS[1];
    const d1 = ((x-mx)-(h.x+2.5)), d2y = ((y-my)-(h.y+2.5));
    const d2 = ((x-mx)-(h.x-2.5)), d2y2 = ((y-my)-(h.y-2.5));
    v += 40 * Math.exp(-(d1*d1 + d2y*d2y) / 18) - 40 * Math.exp(-(d2*d2 + d2y2*d2y2) / 18);
    return v + hash(x, y, seed) * 2 - 1;
  }
  if (product === 'CC') {
    let cc = 0.985 - hash(Math.round(x*3), Math.round(y*3), seed) * 0.02;
    const dCore = Math.hypot((x-mx)-26, (y-my)-24);
    if (dCore < 13) cc -= 0.13 * (1 - dCore / 13);
    const dDeb = Math.hypot((x-mx)-36, (y-my)-10);
    if (dDeb < 4.5) cc -= 0.28 * (1 - dDeb / 4.5);
    return Math.max(0.6, Math.min(1.0, cc));
  }
  if (product === 'ZDR') {
    let z = 0.2 + 3.6 * Math.exp(-(((x-mx)-36)**2 + ((y-my)-6)**2) / 128);
    const dCore = Math.hypot((x-mx)-26, (y-my)-24);
    if (dCore < 11) z -= 0.9 * (1 - dCore / 11);
    z += (y - my) > 60 ? 0.7 : 0;
    return z + hash(x, y, seed) * 0.3 - 0.15;
  }
  return NaN;
}
function fmtVal(code, v) {
  if (Number.isNaN(v)) return null;
  if (code === 'VEL') return v.toFixed(0) + ' m/s';
  if (code === 'CC') return v.toFixed(2);
  return v.toFixed(1) + ' ' + PALETTES[code].unit;
}

/* ── state ────────────────────────────────────────────────────────── */
const state = {
  station: STATIONS[0],
  product: 'REF',
  tilt: 0,
  zoom: 8.2,
  center: { lon: -98.35, lat: 29.75 },
  frame: 7,
  playing: false,
  speed: 1000,
  opacity: 0.85,
  mapAlpha: 0.7,
  showWarnings: true,
  showReports: true,
  showRings: true,
  highlightAlert: -1,
};

const N_FRAMES = 8;
const FRAME_TIMES = [];
{
  const newest = new Date('2026-07-14T19:26:14Z');
  for (let i = 0; i < N_FRAMES; i++) FRAME_TIMES.push(new Date(newest.getTime() - (N_FRAMES - 1 - i) * 4.5 * 60000));
}
const fmtTime = d => d.toISOString().slice(11, 19);

/* ── map engine ───────────────────────────────────────────────────── */
const mapCanvas = document.getElementById('map');
const mctx = mapCanvas.getContext('2d');
const echoCanvas = document.createElement('canvas');
const ectx = echoCanvas.getContext('2d');
let W = 0, H = 0, DPR = 1;
let echoDirty = true, echoTimer = null;

function worldSize(z) { return 256 * Math.pow(2, z); }
function project(lon, lat, z) {
  const ws = worldSize(z);
  const s = Math.sin(lat * Math.PI / 180);
  return {
    x: (lon + 180) / 360 * ws,
    y: (0.5 - Math.log((1 + s) / (1 - s)) / (4 * Math.PI)) * ws,
  };
}
function unproject(x, y, z) {
  const ws = worldSize(z);
  const lon = x / ws * 360 - 180;
  const n = Math.PI - 2 * Math.PI * y / ws;
  const lat = 180 / Math.PI * Math.atan(0.5 * (Math.exp(n) - Math.exp(-n)));
  return { lon, lat };
}
function metersPerPixel() {
  return 156543.03392 * Math.cos(state.center.lat * Math.PI / 180) / Math.pow(2, state.zoom);
}

const tileCache = new Map();
function getTile(z, x, y) {
  const n = Math.pow(2, z);
  const xx = ((x % n) + n) % n;
  if (y < 0 || y >= n) return null;
  const key = `${z}/${xx}/${y}`;
  let t = tileCache.get(key);
  if (!t) {
    t = { img: new Image(), ok: false };
    t.img.onload = () => { t.ok = true; };
    t.img.src = `https://basemaps.cartocdn.com/dark_all/${z}/${xx}/${y}.png`;
    tileCache.set(key, t);
    if (tileCache.size > 600) tileCache.delete(tileCache.keys().next().value);
  }
  return t.ok ? t.img : null;
}

function resize() {
  const wrap = document.getElementById('mapwrap');
  DPR = Math.min(2, window.devicePixelRatio || 1);
  W = wrap.clientWidth; H = wrap.clientHeight;
  mapCanvas.width = W * DPR; mapCanvas.height = H * DPR;
  echoCanvas.width = Math.round(W * 0.5); echoCanvas.height = Math.round(H * 0.5);
  markEchoDirty(0);
}
window.addEventListener('resize', resize);

function markEchoDirty(debounce = 80) {
  echoDirty = true;
  if (echoTimer) clearTimeout(echoTimer);
  echoTimer = setTimeout(renderEcho, debounce);
}

function renderEcho() {
  if (!echoDirty) return;
  echoDirty = false;
  const ew = echoCanvas.width, eh = echoCanvas.height;
  const img = ectx.createImageData(ew, eh);
  const d = img.data;
  const st = state.station;
  const seed = state.frame * 7 + state.tilt * 3;
  const mx = state.frame * 2.0, my = state.frame * 1.6;   // storm motion NE
  const rMax = 240 - state.tilt * 15;
  const code = state.product;
  const pal = PALETTES[code];
  const cW = project(state.center.lon, state.center.lat, state.zoom);
  const radarW = project(st.lon, st.lat, state.zoom);
  // pixel loop computed via km offsets directly (echo canvas renders at 0.5 res)
  for (let py = 0; py < eh; py++) {
    for (let px = 0; px < ew; px++) {
      const wx = cW.x + (px - ew / 2) * 2;                // world px (full-res equivalent)
      const wy = cW.y + (py - eh / 2) * 2;
      const dxPx = wx - radarW.x, dyPx = wy - radarW.y;
      const kmX = dxPx * metersPerPixel() / 1000;
      const kmY = -dyPx * metersPerPixel() / 1000;
      const r = Math.hypot(kmX, kmY);
      if (r > rMax) continue;
      const az = Math.atan2(kmX, kmY);
      const v = field(code, kmX, kmY, r, az, seed, mx, my);
      if (Number.isNaN(v) || v < pal.min) continue;
      const [cr, cg, cb] = samplePalette(code, v);
      const i = (py * ew + px) * 4;
      let a = 255;
      if (code === 'REF' && v < 20) a = Math.round(255 * (v - 12) / 8);
      d[i] = cr; d[i+1] = cg; d[i+2] = cb; d[i+3] = a;
    }
  }
  ectx.putImageData(img, 0, 0);
}

/* ── map render loop ──────────────────────────────────────────────── */
function drawMap(now) {
  mctx.setTransform(DPR, 0, 0, DPR, 0, 0);
  mctx.clearRect(0, 0, W, H);
  mctx.fillStyle = '#050810';
  mctx.fillRect(0, 0, W, H);

  const zl = Math.max(4, Math.min(14, Math.round(state.zoom)));
  const scale = Math.pow(2, state.zoom - zl);
  const cW = project(state.center.lon, state.center.lat, zl);
  const offX = W / 2 - cW.x * scale;
  const offY = H / 2 - cW.y * scale;

  // basemap tiles
  mctx.globalAlpha = state.mapAlpha;
  const n = Math.pow(2, zl);
  const tx0 = Math.floor((cW.x - W / 2 / scale) / 256), tx1 = Math.floor((cW.x + W / 2 / scale) / 256);
  const ty0 = Math.floor((cW.y - H / 2 / scale) / 256), ty1 = Math.floor((cW.y + H / 2 / scale) / 256);
  for (let ty = ty0; ty <= ty1; ty++) {
    for (let tx = tx0; tx <= tx1; tx++) {
      const img = getTile(zl, tx, ty);
      const dx = tx * 256 * scale + offX, dy = ty * 256 * scale + offY;
      if (img) mctx.drawImage(img, dx, dy, 256 * scale, 256 * scale);
      else { mctx.fillStyle = '#0a0f1a'; mctx.fillRect(dx, dy, 256 * scale, 256 * scale); }
    }
  }
  mctx.globalAlpha = 1;

  const toScreen = (lon, lat) => {
    const p = project(lon, lat, zl);
    return { x: p.x * scale + offX, y: p.y * scale + offY };
  };
  const radar = toScreen(state.station.lon, state.station.lat);
  const mpp = metersPerPixel();

  // echo overlay
  if (echoDirty) renderEcho();
  mctx.globalAlpha = state.opacity;
  mctx.imageSmoothingEnabled = true;
  mctx.drawImage(echoCanvas, 0, 0, W, H);
  mctx.globalAlpha = 1;

  // range rings
  if (state.showRings) {
    mctx.strokeStyle = 'rgba(255,255,255,0.13)';
    mctx.fillStyle = 'rgba(230,237,243,0.4)';
    mctx.font = '9px "IBM Plex Mono", monospace';
    mctx.lineWidth = 1;
    for (const km of [25, 50, 100]) {
      const rPx = km * 1000 / mpp;
      mctx.beginPath();
      mctx.arc(radar.x, radar.y, rPx, 0, Math.PI * 2);
      mctx.stroke();
      mctx.fillText(km + ' km', radar.x + rPx * 0.707 + 4, radar.y - rPx * 0.707 - 3);
    }
  }

  // warning polygons
  if (state.showWarnings) {
    ALERTS.forEach((a, idx) => {
      mctx.beginPath();
      a.poly.forEach(([la, lo], i) => {
        const s = toScreen(lo, la);
        i === 0 ? mctx.moveTo(s.x, s.y) : mctx.lineTo(s.x, s.y);
      });
      mctx.closePath();
      const col = SEV_COLORS[a.sev];
      const hot = state.highlightAlert === idx;
      mctx.fillStyle = col + (hot ? '2e' : '14');
      mctx.fill();
      mctx.strokeStyle = col;
      mctx.lineWidth = hot ? 2.5 : 1.5;
      if (hot) { mctx.shadowColor = col; mctx.shadowBlur = 14; }
      mctx.stroke();
      mctx.shadowBlur = 0;
    });
  }

  // storm reports
  if (state.showReports) {
    mctx.font = '11px sans-serif';
    mctx.textAlign = 'center'; mctx.textBaseline = 'middle';
    for (const r of REPORTS) {
      const s = toScreen(r.lon, r.lat);
      if (s.x < -20 || s.y < -20 || s.x > W + 20 || s.y > H + 20) continue;
      mctx.lineWidth = 3;
      mctx.strokeStyle = 'rgba(245,248,249,0.85)';
      mctx.strokeText(GLYPH[r.g], s.x, s.y);
      mctx.fillStyle = GLYPH_COLOR[r.g];
      mctx.fillText(GLYPH[r.g], s.x, s.y);
    }
    mctx.textAlign = 'left'; mctx.textBaseline = 'alphabetic';
  }

  // station pills
  mctx.font = '600 10px "IBM Plex Mono", monospace';
  for (const st of STATIONS) {
    const s = toScreen(st.lon, st.lat);
    if (s.x < -60 || s.y < -30 || s.x > W + 60 || s.y > H + 30) continue;
    const tw = mctx.measureText(st.id).width;
    const pw = tw + 16, ph = 19;
    const active = st.id === state.station.id;
    mctx.beginPath();
    mctx.roundRect(s.x - pw / 2, s.y - ph / 2, pw, ph, 3);
    if (st.type === 'TDWR') {
      mctx.fillStyle = 'rgba(48,36,58,0.9)';
      mctx.strokeStyle = 'rgba(195,156,224,0.55)';
    } else if (active) {
      mctx.fillStyle = 'rgba(42,56,70,0.92)';
      mctx.strokeStyle = '#7ea6c9';
    } else {
      mctx.fillStyle = 'rgba(20,23,27,0.88)';
      mctx.strokeStyle = 'rgba(126,166,201,0.3)';
    }
    mctx.lineWidth = active ? 1.5 : 1;
    mctx.fill(); mctx.stroke();
    mctx.fillStyle = active ? '#dce7f0' : (st.type === 'TDWR' ? '#c39ce0' : '#b9c2ca');
    mctx.fillText(st.id, s.x - tw / 2, s.y + 3.5);
  }

  // scale bar + zoom label
  updateScaleBar();
  requestAnimationFrame(drawMap);
}

function updateScaleBar() {
  const mpp = metersPerPixel();
  const cands = [1, 2, 5, 10, 20, 50, 100, 200, 500];
  let pick = cands[0];
  for (const km of cands) if (km * 1000 / mpp <= 120) pick = km;
  const px = pick * 1000 / mpp;
  document.getElementById('scaleLine').style.width = px + 'px';
  document.getElementById('scaleText').textContent = pick + ' km';
  document.getElementById('zoomLevel').textContent = 'Z ' + state.zoom.toFixed(1);
}

/* ── pan / zoom ───────────────────────────────────────────────────── */
let drag = null;
mapCanvas.addEventListener('mousedown', e => {
  drag = { x: e.clientX, y: e.clientY, c: { ...state.center }, moved: false };
  mapCanvas.classList.add('dragging');
});
window.addEventListener('mousemove', e => {
  if (!drag) { probeMove(e); return; }
  const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
  if (Math.abs(dx) + Math.abs(dy) > 3) drag.moved = true;
  const ws = worldSize(state.zoom);
  const cW = project(drag.c.lon, drag.c.lat, state.zoom);
  const nW = { x: cW.x - dx, y: cW.y - dy };
  state.center = unproject(nW.x, nW.y, state.zoom);
  hideProbe();
  markEchoDirty();
});
window.addEventListener('mouseup', e => {
  if (drag && !drag.moved) mapClick(e);
  drag = null;
  mapCanvas.classList.remove('dragging');
});
mapCanvas.addEventListener('wheel', e => {
  e.preventDefault();
  const rect = mapCanvas.getBoundingClientRect();
  const mx = e.clientX - rect.left, my = e.clientY - rect.top;
  const before = unproject(
    project(state.center.lon, state.center.lat, state.zoom).x + (mx - W / 2),
    project(state.center.lon, state.center.lat, state.zoom).y + (my - H / 2),
    state.zoom
  );
  state.zoom = Math.max(5, Math.min(12, state.zoom - Math.sign(e.deltaY) * 0.4));
  const after = unproject(
    project(state.center.lon, state.center.lat, state.zoom).x + (mx - W / 2),
    project(state.center.lon, state.center.lat, state.zoom).y + (my - H / 2),
    state.zoom
  );
  state.center.lon += before.lon - after.lon;
  state.center.lat += before.lat - after.lat;
  markEchoDirty();
}, { passive: false });

function zoomStep(d) {
  state.zoom = Math.max(5, Math.min(12, state.zoom + d));
  markEchoDirty();
}
document.getElementById('zoomIn').addEventListener('click', () => zoomStep(0.5));
document.getElementById('zoomOut').addEventListener('click', () => zoomStep(-0.5));

function mapClick(e) {
  const rect = mapCanvas.getBoundingClientRect();
  const mx = e.clientX - rect.left, my = e.clientY - rect.top;
  const zl = Math.max(4, Math.min(14, Math.round(state.zoom)));
  const scale = Math.pow(2, state.zoom - zl);
  const cW = project(state.center.lon, state.center.lat, zl);
  const offX = W / 2 - cW.x * scale, offY = H / 2 - cW.y * scale;
  // station pill hit test
  mctx.font = '600 10px "IBM Plex Mono", monospace';
  for (const st of STATIONS) {
    const p = project(st.lon, st.lat, zl);
    const sx = p.x * scale + offX, sy = p.y * scale + offY;
    const tw = mctx.measureText(st.id).width;
    if (Math.abs(mx - sx) < (tw + 16) / 2 + 4 && Math.abs(my - sy) < 13) {
      selectStation(st);
      return;
    }
  }
}

/* ── probe ────────────────────────────────────────────────────────── */
const probeEl = document.getElementById('probe');
function beamHeight(r, tiltDeg) {
  const Re = 8495; // 4/3 earth radius, km
  const el = tiltDeg * Math.PI / 180;
  return Math.sqrt(r * r + Re * Re + 2 * r * Re * Math.sin(el)) - Re + state.station.elev / 1000;
}
function probeMove(e) {
  const rect = mapCanvas.getBoundingClientRect();
  const mx = e.clientX - rect.left, my = e.clientY - rect.top;
  const mpp = metersPerPixel();
  const radarW = project(state.station.lon, state.station.lat, state.zoom);
  const cW = project(state.center.lon, state.center.lat, state.zoom);
  const dxPx = (cW.x + (mx - W / 2)) - radarW.x;
  const dyPx = (cW.y + (my - H / 2)) - radarW.y;
  const kmX = dxPx * mpp / 1000, kmY = -dyPx * mpp / 1000;
  const r = Math.hypot(kmX, kmY);
  if (r > 250) { hideProbe(); return; }
  const az = Math.atan2(kmX, kmY);
  const azDeg = (az * 180 / Math.PI + 360) % 360;
  const seed = state.frame * 7 + state.tilt * 3;
  const v = field(state.product, kmX, kmY, r, az, seed, state.frame * 2.0, state.frame * 1.6);
  const ll = unproject(cW.x + (mx - W / 2), cW.y + (my - H / 2), state.zoom);
  const valStr = fmtVal(state.product, v);
  const bh = beamHeight(r, TILTS[state.tilt]);
  probeEl.innerHTML = valStr
    ? `<span class="pv">${valStr}</span> <span class="pd">·</span> ${r.toFixed(1)} km <span class="pd">·</span> ${azDeg.toFixed(0)}° <span class="pd">·</span> beam ${bh.toFixed(2)} km <span class="pd">·</span> ${ll.lat.toFixed(3)},${ll.lon.toFixed(3)}`
    : `<span class="pd">no echo ·</span> ${r.toFixed(1)} km <span class="pd">·</span> ${azDeg.toFixed(0)}° <span class="pd">·</span> ${ll.lat.toFixed(3)},${ll.lon.toFixed(3)}`;
  probeEl.style.display = 'block';
  const pw = probeEl.offsetWidth;
  probeEl.style.left = Math.min(mx + 16, W - pw - 12) + 'px';
  probeEl.style.top = (my + 18) + 'px';
  document.getElementById('cursorPos').textContent = `${ll.lat.toFixed(3)}, ${ll.lon.toFixed(3)}`;
}
function hideProbe() {
  probeEl.style.display = 'none';
  document.getElementById('cursorPos').textContent = '—';
}
mapCanvas.addEventListener('mouseleave', hideProbe);

/* ── rails: stations / products / tilts ───────────────────────────── */
function selectStation(st) {
  state.station = st;
  document.querySelector('.station-id').textContent = st.id;
  document.querySelector('.station-meta').textContent = `${st.name} · ${st.type === 'TDWR' ? 'TDWR' : 'WSR-88D'} · ${st.elev} m`;
  state.center = { lon: st.lon - 0.35, lat: st.lat + 0.05 };
  document.querySelectorAll('.station-item').forEach(el => el.classList.toggle('active', el.dataset.sid === st.id));
  log('ok', `station → <b>${st.id}</b> ${st.name}`);
  markEchoDirty(0);
  updateDataChip();
}

function buildStations() {
  const el = document.getElementById('stationList');
  el.innerHTML = '';
  const sorted = [...STATIONS].sort((a, b) => (b.fav - a.fav));
  for (const st of sorted) {
    const d = document.createElement('div');
    d.className = 'station-item' + (st.id === state.station.id ? ' active' : '');
    d.dataset.sid = st.id;
    d.innerHTML = `${st.fav ? '<span class="fav">★</span>' : ''}<span class="sid">${st.id}</span><span class="sname">${st.name}</span><span class="stag ${st.type === 'TDWR' ? 'tdwr' : ''}">${st.type}</span>`;
    d.addEventListener('click', () => selectStation(st));
    el.appendChild(d);
  }
}

function buildProducts() {
  const el = document.getElementById('productList');
  el.innerHTML = '';
  for (const p of PRODUCTS) {
    const d = document.createElement('div');
    d.className = 'product-item' + (p.code === state.product ? ' active' : '');
    if (p.soon) d.style.opacity = '0.38';
    d.innerHTML = `<span class="pkey">${p.key || '·'}</span><span class="pname">${p.name}${p.soon ? ' <span style="font-size:9px;color:var(--tx3)">soon</span>' : ''}</span><span class="pcode">${p.code}</span>`;
    if (!p.soon) d.addEventListener('click', () => selectProduct(p.code));
    el.appendChild(d);
  }
}
function selectProduct(code) {
  state.product = code;
  document.querySelectorAll('.product-item').forEach((el, i) => el.classList.toggle('active', PRODUCTS[i].code === code));
  renderLegend();
  markEchoDirty(0);
  log('ok', `product → <b>${code}</b> tilt ${state.tilt + 1}`);
}

function buildTilts() {
  const el = document.getElementById('tiltStrip');
  el.innerHTML = '';
  TILTS.forEach((t, i) => {
    const b = document.createElement('button');
    b.className = 'tilt-btn' + (i === state.tilt ? ' active' : '');
    b.textContent = t + '°';
    b.addEventListener('click', () => {
      state.tilt = i;
      document.querySelectorAll('.tilt-btn').forEach((x, j) => x.classList.toggle('active', j === i));
      document.querySelector('.scan-meta').textContent = `VCP 212 · tilt ${i + 1} · scan 1/2 · ${t}°`;
      document.querySelector('#sec-tilt .caption').textContent = `tilt ${i + 1} · scan 1/2 · ${t}° cut`;
      markEchoDirty(0);
    });
    el.appendChild(b);
  });
}

/* ── overlays wiring ──────────────────────────────────────────────── */
document.getElementById('ovWarnings').addEventListener('change', e => state.showWarnings = e.target.checked);
document.getElementById('ovReports').addEventListener('change', e => state.showReports = e.target.checked);
document.getElementById('ovRings').addEventListener('change', e => state.showRings = e.target.checked);
document.getElementById('opacitySlider').addEventListener('input', e => {
  state.opacity = e.target.value / 100;
  document.getElementById('opacityVal').textContent = e.target.value + '%';
});
document.getElementById('mapSlider').addEventListener('input', e => {
  state.mapAlpha = e.target.value / 100;
  document.getElementById('mapVal').textContent = e.target.value + '%';
});

/* collapsible sections */
document.querySelectorAll('.section-head').forEach(h => {
  h.addEventListener('click', () => {
    h.classList.toggle('closed');
    document.getElementById(h.dataset.collapse).classList.toggle('closed');
  });
});

/* rail collapse */
document.getElementById('leftToggle').addEventListener('click', () => {
  const r = document.getElementById('rail-left');
  r.classList.toggle('collapsed');
  document.getElementById('leftToggle').textContent = r.classList.contains('collapsed') ? '›' : '‹';
  setTimeout(resize, 300);
});
document.getElementById('rightToggle').addEventListener('click', () => {
  const r = document.getElementById('rail-right');
  r.classList.toggle('collapsed');
  document.getElementById('rightToggle').textContent = r.classList.contains('collapsed') ? '‹' : '›';
  setTimeout(resize, 300);
});

/* panel grid */
document.querySelectorAll('.pg-cell').forEach(b => {
  b.addEventListener('click', () => {
    document.querySelectorAll('.pg-cell').forEach(x => x.classList.remove('active'));
    b.classList.add('active');
    log('warn', `panel layout ${b.textContent} — multi-panel not in this mock`);
  });
});

/* ── right rail: alerts + reports ─────────────────────────────────── */
function buildAlerts() {
  const el = document.getElementById('rtab-alerts');
  el.innerHTML = '';
  ALERTS.forEach((a, i) => {
    const d = document.createElement('div');
    d.className = `alert-card ${a.sev}`;
    d.innerHTML = `<div class="alert-top"><span class="alert-sev">${a.tag}</span><span class="alert-exp">${a.exp}</span></div>
      <div class="alert-name">${a.name}</div><div class="alert-area">${a.area}</div>
      <div class="alert-headline">${a.headline}</div>`;
    d.addEventListener('mouseenter', () => state.highlightAlert = i);
    d.addEventListener('mouseleave', () => state.highlightAlert = -1);
    d.addEventListener('click', () => {
      const cx = a.poly.reduce((s, p) => s + p[1], 0) / a.poly.length;
      const cy = a.poly.reduce((s, p) => s + p[0], 0) / a.poly.length;
      state.center = { lon: cx, lat: cy };
      state.zoom = Math.max(state.zoom, 8.6);
      markEchoDirty();
    });
    el.appendChild(d);
  });
}
function buildReports() {
  const el = document.getElementById('rtab-reports');
  el.innerHTML = '';
  for (const r of REPORTS) {
    const d = document.createElement('div');
    d.className = 'report-row';
    d.innerHTML = `<i class="glyph ${r.g}">${GLYPH[r.g]}</i>
      <div class="report-main"><div class="report-type">${r.type}</div><div class="report-loc">${r.loc}</div></div>
      <div><div class="report-mag">${r.mag}</div><div class="report-time">${r.time}</div></div>`;
    d.addEventListener('click', () => {
      state.center = { lon: r.lon, lat: r.lat };
      state.zoom = Math.max(state.zoom, 9);
      markEchoDirty();
    });
    el.appendChild(d);
  }
}
document.querySelectorAll('.rail-tab').forEach(t => {
  t.addEventListener('click', () => {
    document.querySelectorAll('.rail-tab').forEach(x => x.classList.remove('active'));
    t.classList.add('active');
    document.getElementById('rtab-alerts').classList.toggle('hidden', t.dataset.rtab !== 'alerts');
    document.getElementById('rtab-reports').classList.toggle('hidden', t.dataset.rtab !== 'reports');
  });
});

/* ── legend ───────────────────────────────────────────────────────── */
function renderLegend() {
  const p = PRODUCTS.find(x => x.code === state.product);
  const pal = PALETTES[state.product];
  document.getElementById('legendTitle').textContent = p.name;
  document.getElementById('legendUnit').textContent = p.unit;
  const c = document.getElementById('legendBar');
  const g = c.getContext('2d');
  const grad = g.createLinearGradient(0, 0, c.width, 0);
  for (const [v, col] of pal.stops) {
    const t = (v - pal.stops[0][0]) / (pal.stops[pal.stops.length - 1][0] - pal.stops[0][0]);
    grad.addColorStop(Math.max(0, Math.min(1, t)), col);
  }
  g.clearRect(0, 0, c.width, c.height);
  g.fillStyle = grad;
  g.fillRect(0, 0, c.width, c.height);
  document.getElementById('legendTicks').innerHTML = pal.ticks.map(t => `<span>${t}</span>`).join('');
}

/* ── timeline / playback ──────────────────────────────────────────── */
function buildTimeline() {
  const el = document.getElementById('timeline');
  el.innerHTML = '<div class="tl-track"></div>';
  FRAME_TIMES.forEach((t, i) => {
    const tick = document.createElement('div');
    tick.className = 'tl-tick' + (i === state.frame ? ' current' : '') + (i < state.frame ? ' visited' : '');
    tick.style.left = (4 + (i / (N_FRAMES - 1)) * 92) + '%';
    tick.title = fmtTime(t);
    tick.addEventListener('click', ev => { ev.stopPropagation(); setFrame(i); });
    el.appendChild(tick);
    if (i % 3 === 0 || i === N_FRAMES - 1) {
      const lab = document.createElement('div');
      lab.className = 'tl-labels';
      lab.style.left = (4 + (i / (N_FRAMES - 1)) * 92) + '%';
      lab.textContent = fmtTime(t).slice(0, 5);
      el.appendChild(lab);
    }
  });
  el.addEventListener('click', e => {
    const rect = el.getBoundingClientRect();
    const t = (e.clientX - rect.left) / rect.width;
    const i = Math.round(((t - 0.04) / 0.92) * (N_FRAMES - 1));
    setFrame(Math.max(0, Math.min(N_FRAMES - 1, i)));
  });
}
function setFrame(i) {
  state.frame = i;
  document.querySelectorAll('.tl-tick').forEach((t, j) => {
    t.classList.toggle('current', j === i);
    t.classList.toggle('visited', j < i);
  });
  document.getElementById('frameLabel').innerHTML =
    `<b>F ${i + 1}/${N_FRAMES}</b> · ${fmtTime(FRAME_TIMES[i])} UTC`;
  updateDataChip();
  markEchoDirty(0);
  renderXsect();
}
function updateDataChip() {
  document.getElementById('dataChip').innerHTML =
    `<b>8 520</b> rays · NOAA ${fmtTime(FRAME_TIMES[state.frame])} UTC · 71/71 chunks`;
}

const playBtn = document.getElementById('playBtn');
const playIcon = document.getElementById('playIcon');
let playTimer = null;
function setPlaying(on) {
  state.playing = on;
  playIcon.innerHTML = on
    ? '<path fill="currentColor" d="M3 2h4v12H3zM9 2h4v12H9z"/>'
    : '<path fill="currentColor" d="M4 2l9 6-9 6z"/>';
  if (playTimer) { clearInterval(playTimer); playTimer = null; }
  if (on) {
    playTimer = setInterval(() => {
      setFrame(state.frame >= N_FRAMES - 1 ? 0 : state.frame + 1);
      if (state.frame === 0) log('ok', 'loop restart — history replay');
    }, state.speed);
  }
}
playBtn.addEventListener('click', () => setPlaying(!state.playing));
document.querySelectorAll('.spd').forEach((b, i) => {
  b.addEventListener('click', () => {
    document.querySelectorAll('.spd').forEach(x => x.classList.remove('active'));
    b.classList.add('active');
    state.speed = [1000, 500, 250][i];
    if (state.playing) setPlaying(true);
  });
});

/* dock tabs + collapse */
document.querySelectorAll('.dock-tab').forEach(t => {
  t.addEventListener('click', () => {
    document.querySelectorAll('.dock-tab').forEach(x => x.classList.remove('active'));
    t.classList.add('active');
    document.querySelectorAll('.dtab-page').forEach(p => p.classList.add('hidden'));
    document.getElementById('dtab-' + t.dataset.dtab).classList.remove('hidden');
    document.getElementById('dock').classList.remove('collapsed');
  });
});
document.getElementById('dockCollapse').addEventListener('click', () => {
  document.getElementById('dock').classList.toggle('collapsed');
  setTimeout(resize, 320);
});

/* ── cross-section ────────────────────────────────────────────────── */
function renderXsect() {
  const c = document.getElementById('xsect');
  const g = c.getContext('2d');
  const w = c.width, h = c.height;
  g.fillStyle = '#050810';
  g.fillRect(0, 0, w, h);
  const seed = state.frame * 7 + state.tilt * 3;
  const azRad = 60 * Math.PI / 180;
  const mx = state.frame * 2.0, my = state.frame * 1.6;
  const kmMax = 130, hMax = 15;
  const plotX = 34, plotW = w - plotX - 10, plotH = h - 20;
  for (let px = 0; px < plotW; px++) {
    const dist = px / plotW * kmMax;
    const x = dist * Math.sin(azRad), y = dist * Math.cos(azRad);
    const r0 = Math.hypot(x, y);
    const dbz = fieldREF(x, y, r0, seed, mx, my);
    const top = 2.5 + Math.max(0, dbz) / 6.5;
    for (let py = 0; py < plotH; py++) {
      const hh = (1 - py / plotH) * hMax;
      if (hh > top) continue;
      const z = dbz * Math.max(0, 1 - Math.pow(hh / top, 1.5)) + hash(px, py, seed) * 3 - 1.5;
      if (z < 12) continue;
      const [cr, cg, cb] = samplePalette('REF', z);
      g.fillStyle = `rgb(${cr|0},${cg|0},${cb|0})`;
      g.fillRect(plotX + px, py, 1, 1);
    }
  }
  g.strokeStyle = '#1a2540';
  g.strokeRect(plotX, 0, plotW, plotH);
  g.fillStyle = '#55606e';
  g.font = '9px "IBM Plex Mono", monospace';
  for (let km = 0; km <= kmMax; km += 25) {
    g.fillText(km + '', plotX + km / kmMax * plotW - 4, h - 6);
  }
  for (let hh = 0; hh <= hMax; hh += 5) {
    g.fillText(hh + '', 8, (1 - hh / hMax) * plotH + 3);
  }
  g.fillText('km', w - 22, h - 6);
  g.fillText('km', 4, 10);
}

/* ── log ──────────────────────────────────────────────────────────── */
const logEl = document.getElementById('logScroll');
function log(kind, msg) {
  const d = document.createElement('div');
  d.className = 'log-line ' + kind;
  const t = new Date().toISOString().slice(11, 19);
  d.innerHTML = `<span class="lt">${t}</span><span class="lm">${msg}</span>`;
  logEl.appendChild(d);
  logEl.scrollTop = logEl.scrollHeight;
  while (logEl.children.length > 60) logEl.removeChild(logEl.firstChild);
}
function seedLog() {
  log('ok', `volume KEWX ${fmtTime(FRAME_TIMES[7])} — 71/71 chunks, 0 rejected`);
  log('ok', 'decode 0.42 s · render 86 ms · cache hit 61%');
  log('warn', 'basemap tile 8/62/101 retry 2 (timeout 15 s)');
  log('ok', 'NWS alerts poll — 3 polygons, 5 reports');
  log('err', 'chunk 12 tilt 3 — HTTP 404 (not yet available), will retry');
  log('ok', 'history backfill: 8 volumes · 31 min span');
}

/* ── settings modal ───────────────────────────────────────────────── */
function buildSettings() {
  const wrap = document.createElement('div');
  wrap.id = 'settingsModal';
  wrap.innerHTML = `
    <div class="settings-card">
      <div class="settings-head">SETTINGS <button class="settings-close" id="settingsClose">✕</button></div>
      <div class="settings-group"><h4>RADAR</h4>
        <div class="set-row">Chase mode <span class="sval">off</span></div>
        <div class="set-row">Refresh interval <span class="sval">60 s</span></div>
        <div class="set-row">History volumes <span class="sval">8</span></div>
      </div>
      <div class="settings-group"><h4>NETWORK</h4>
        <div class="set-row">Parallel chunks <span class="sval">8</span></div>
        <div class="set-row">Timeout <span class="sval">15 s</span></div>
        <div class="set-row">Retries <span class="sval">3</span></div>
      </div>
      <div class="settings-group"><h4>CACHE</h4>
        <div class="set-row">Memory <span class="sval">512 MB</span></div>
        <div class="set-row">Disk <span class="sval">2 GB · restart</span></div>
        <div class="set-note">Prototype mock — grouped, tabbed settings would replace the current flat 15-row form.</div>
      </div>
    </div>`;
  document.body.appendChild(wrap);
  document.getElementById('settingsBtn').addEventListener('click', () => wrap.classList.add('open'));
  document.getElementById('settingsClose').addEventListener('click', () => wrap.classList.remove('open'));
  wrap.addEventListener('click', e => { if (e.target === wrap) wrap.classList.remove('open'); });
}

/* ── keyboard ─────────────────────────────────────────────────────── */
window.addEventListener('keydown', e => {
  if (e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT') return;
  if (e.code === 'Space') { e.preventDefault(); setPlaying(!state.playing); }
  else if (e.key === 'ArrowRight') setFrame(Math.min(N_FRAMES - 1, state.frame + 1));
  else if (e.key === 'ArrowLeft') setFrame(Math.max(0, state.frame - 1));
  else if (e.key === 'PageUp') document.querySelectorAll('.tilt-btn')[Math.min(TILTS.length - 1, state.tilt + 1)].click();
  else if (e.key === 'PageDown') document.querySelectorAll('.tilt-btn')[Math.max(0, state.tilt - 1)].click();
  else if (e.key === 'l') setFrame(N_FRAMES - 1);
  else if (e.key === '+' || e.key === '=') zoomStep(0.5);
  else if (e.key === '-') zoomStep(-0.5);
  else if (e.key === 'Escape') { document.getElementById('settingsModal').classList.remove('open'); }
  else {
    const p = PRODUCTS.find(x => x.key === e.key);
    if (p && !p.soon) selectProduct(p.code);
  }
});

/* ── clock ────────────────────────────────────────────────────────── */
setInterval(() => {
  document.getElementById('clock').textContent = new Date().toISOString().slice(11, 19);
}, 1000);

/* ── boot ─────────────────────────────────────────────────────────── */
document.getElementById('aboutBtn').addEventListener('click', () =>
  log('ok', 'CappedWeather 0.3.0 — console concept v2 (mock data)'));
buildStations();
buildProducts();
buildTilts();
buildAlerts();
buildReports();
buildTimeline();
buildSettings();
renderLegend();
seedLog();
resize();
renderXsect();
updateDataChip();
requestAnimationFrame(drawMap);
