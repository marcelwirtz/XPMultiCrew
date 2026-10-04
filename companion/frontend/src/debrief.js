// Debrief page: the flights companion/flights.go recorded automatically -
// the whole flight on a map (with everyone who flew along), altitude and
// speed profiles, and the events (takeoff, checklists, waypoints, landings
// with their Butter-Board score). Hover the profile or drag the slider to
// move every aircraft to that moment, or press Play to watch it again.
// Loaded the first time the page opens (MapLibre, like map.js).
import * as maplibregl from 'maplibre-gl';
import 'maplibre-gl/dist/maplibre-gl.css';
import workerUrl from 'maplibre-gl/dist/maplibre-gl-worker.mjs?worker&url';

import { DeleteFlight, ListFlights, LoadFlight } from '../wailsjs/go/main/App';

maplibregl.setWorkerUrl(workerUrl);

const $ = (id) => document.getElementById(id);
const kStyle = 'https://tiles.openfreemap.org/styles/dark';
const kOwnColor = '#4da3ff'; // the app's accent - "you" everywhere
// Categorical slots 2.. of the dataviz reference palette (dark steps), in
// fixed order; aircraft past the 7th share a neutral gray.
const kPeerColors = ['#d95926', '#199e70', '#c98500', '#d55181', '#008300', '#9085e9', '#e66767'];
const kOtherColor = '#8b93a1';
const kGsColor = '#3987e5';
const kIasColor = '#d95926';
const kEventGlyph = { takeoff: '▲', landing: '▼', touchgo: '↻', checklist: '✓', waypoint: '◆' };

// Track columns, see flights.go's Flight.Track.
const T = 0, LAT = 1, LON = 2, ALT = 3, GS = 4, IAS = 5, VS = 6, HDG = 7, GND = 8;

let map = null;
let mapReady = false;
let mapFailed = false;
let flights = [];
let flight = null;
let cursorT = 0;
let playing = false;
let lastFrame = 0;
let shownFlightsVersion = -1;
let recordingNow = false;
let liveTimer = null;

function escapeHtml(text) {
  return String(text ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
}

function peerColor(i) {
  return i < kPeerColors.length ? kPeerColors[i] : kOtherColor;
}

function fmtDuration(seconds) {
  const m = Math.max(0, Math.round(seconds / 60));
  return `${Math.floor(m / 60)}:${String(m % 60).padStart(2, '0')} h`;
}

function fmtClock(seconds) {
  const s = Math.max(0, Math.round(seconds));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  return `${h ? `${h}:` : ''}${String(m).padStart(h ? 2 : 1, '0')}:${String(s % 60).padStart(2, '0')}`;
}

function fmtDate(unix) {
  return new Date(unix * 1000).toLocaleString([], { day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit' });
}

function setMessage(text) {
  const el = $('debrief-message');
  el.textContent = text || '';
  el.style.display = text ? '' : 'none';
}

// --- Interpolation -------------------------------------------------------------

// Index of the last sample with time <= t.
function indexAt(track, t) {
  let lo = 0;
  let hi = track.length - 1;
  if (hi < 0 || t <= track[0][0]) return 0;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (track[mid][0] <= t) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}

function lerpAngle(a, b, f) {
  const d = ((b - a + 540) % 360) - 180;
  return (a + d * f + 360) % 360;
}

function bearing(a, b) {
  const r = Math.PI / 180;
  const y = Math.sin((b[LON] - a[LON]) * r) * Math.cos(b[LAT] * r);
  const x = Math.cos(a[LAT] * r) * Math.sin(b[LAT] * r) - Math.sin(a[LAT] * r) * Math.cos(b[LAT] * r) * Math.cos((b[LON] - a[LON]) * r);
  return (Math.atan2(y, x) / r + 360) % 360;
}

// Own aircraft state at time t (all columns interpolated).
function ownAt(t) {
  const tr = flight.track;
  const i = indexAt(tr, t);
  const a = tr[i];
  const b = tr[Math.min(i + 1, tr.length - 1)];
  const f = b[T] > a[T] ? Math.min(1, Math.max(0, (t - a[T]) / (b[T] - a[T]))) : 0;
  const out = a.map((v, k) => v + (b[k] - v) * f);
  out[HDG] = lerpAngle(a[HDG], b[HDG], f);
  out[GND] = a[GND];
  return out;
}

// A peer at time t: [lat, lon, alt, heading], null when they weren't there.
function peerAt(peer, t) {
  const tr = peer.track;
  if (!tr.length || t < tr[0][0] - 2 || t > tr[tr.length - 1][0] + 2) return null;
  const i = indexAt(tr, t);
  const a = tr[i];
  const b = tr[Math.min(i + 1, tr.length - 1)];
  const f = b[0] > a[0] ? Math.min(1, Math.max(0, (t - a[0]) / (b[0] - a[0]))) : 0;
  const prev = tr[Math.max(i - 1, 0)];
  const hdg = b !== a ? bearing(a, b) : prev !== a ? bearing(prev, a) : 0;
  return [a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f, a[3] + (b[3] - a[3]) * f, hdg];
}

// --- Map -------------------------------------------------------------------------

function makePlaneImage(fill) {
  const size = 48;
  const canvas = document.createElement('canvas');
  canvas.width = size;
  canvas.height = size;
  const ctx = canvas.getContext('2d');
  ctx.translate(size / 2, size / 2);
  ctx.fillStyle = fill;
  ctx.strokeStyle = 'rgba(0,0,0,0.75)';
  ctx.lineWidth = 2;
  ctx.beginPath();
  for (const [x, y] of [[0, -20], [3, -12], [3, -4], [20, 4], [20, 8], [3, 4], [3, 13], [8, 17], [8, 20], [0, 18],
    [-8, 20], [-8, 17], [-3, 13], [-3, 4], [-20, 8], [-20, 4], [-3, -4], [-3, -12]]) {
    ctx.lineTo(x, y);
  }
  ctx.closePath();
  ctx.fill();
  ctx.stroke();
  return ctx.getImageData(0, 0, size, size);
}

const emptyFc = () => ({ type: 'FeatureCollection', features: [] });

function initMap() {
  if (map || mapFailed) return;
  try {
    map = new maplibregl.Map({ container: 'debrief-map', style: kStyle, center: [8.5, 50], zoom: 6, attributionControl: { compact: true } });
  } catch (e) {
    mapFailed = true;
    setMessage(`The map could not be started (${e.message || e}) - the profile below still works.`);
    return;
  }
  map.addControl(new maplibregl.NavigationControl({ showCompass: false }), 'top-right');
  map.on('error', (e) => {
    if (!mapReady && e && e.error) setMessage('Base map not reachable (offline?) - tracks are drawn without it.');
  });
  map.on('load', () => {
    map.addImage('plane-own', makePlaneImage(kOwnColor));
    kPeerColors.concat(kOtherColor).forEach((c, i) => map.addImage(`plane-peer-${i}`, makePlaneImage(c)));
    map.addSource('db-tracks', { type: 'geojson', data: emptyFc() });
    map.addSource('db-events', { type: 'geojson', data: emptyFc() });
    map.addSource('db-planes', { type: 'geojson', data: emptyFc() });
    map.addLayer({
      id: 'db-tracks', type: 'line', source: 'db-tracks',
      layout: { 'line-cap': 'round', 'line-join': 'round' },
      paint: { 'line-color': ['get', 'color'], 'line-width': ['case', ['get', 'own'], 3, 2], 'line-opacity': ['case', ['get', 'own'], 1, 0.8] },
    });
    map.addLayer({
      id: 'db-events', type: 'circle', source: 'db-events',
      paint: { 'circle-radius': 5, 'circle-color': '#e6e8eb', 'circle-stroke-color': '#14171c', 'circle-stroke-width': 2 },
    });
    map.addLayer({
      id: 'db-event-labels', type: 'symbol', source: 'db-events',
      layout: { 'text-field': ['get', 'label'], 'text-size': 11, 'text-offset': [0, 1.2], 'text-anchor': 'top', 'text-optional': true },
      paint: { 'text-color': '#e6e8eb', 'text-halo-color': '#14171c', 'text-halo-width': 1.5 },
    });
    map.addLayer({
      id: 'db-planes', type: 'symbol', source: 'db-planes',
      layout: {
        'icon-image': ['get', 'icon'], 'icon-size': 0.6, 'icon-rotate': ['get', 'heading'],
        'icon-rotation-alignment': 'map', 'icon-allow-overlap': true, 'icon-ignore-placement': true,
        'text-field': ['get', 'label'], 'text-size': 11, 'text-offset': [0, 1.6], 'text-anchor': 'top', 'text-optional': true,
      },
      paint: { 'text-color': '#e6e8eb', 'text-halo-color': '#14171c', 'text-halo-width': 1.5 },
    });
    mapReady = true;
    setMessage('');
    drawFlightOnMap(true);
  });
}

function drawFlightOnMap(fit) {
  if (!mapReady || !flight) return;
  const features = [];
  flight.peers.forEach((p, i) => {
    if (p.track.length >= 2) {
      features.push({
        type: 'Feature', properties: { own: false, color: peerColor(i) },
        geometry: { type: 'LineString', coordinates: p.track.map((s) => [s[2], s[1]]) },
      });
    }
  });
  features.push({
    type: 'Feature', properties: { own: true, color: kOwnColor },
    geometry: { type: 'LineString', coordinates: flight.track.map((s) => [s[LON], s[LAT]]) },
  });
  map.getSource('db-tracks').setData({ type: 'FeatureCollection', features });
  map.getSource('db-events').setData({
    type: 'FeatureCollection',
    features: flight.events
      .filter((e) => e.lat || e.lon)
      .map((e) => ({ type: 'Feature', properties: { label: `${kEventGlyph[e.kind] || '•'} ${e.label}` }, geometry: { type: 'Point', coordinates: [e.lon, e.lat] } })),
  });
  if (fit && flight.track.length) {
    const b = new maplibregl.LngLatBounds();
    flight.track.forEach((s) => b.extend([s[LON], s[LAT]]));
    map.fitBounds(b, { padding: 40, duration: 0, maxZoom: 13 });
  }
  drawCursorOnMap();
}

function drawCursorOnMap() {
  if (!mapReady || !flight || !flight.track.length) return;
  const own = ownAt(cursorT);
  const features = [{
    type: 'Feature', properties: { icon: 'plane-own', heading: own[HDG], label: flight.callsign || 'You' },
    geometry: { type: 'Point', coordinates: [own[LON], own[LAT]] },
  }];
  flight.peers.forEach((p, i) => {
    const at = peerAt(p, cursorT);
    if (!at) return;
    features.push({
      type: 'Feature', properties: { icon: `plane-peer-${Math.min(i, kPeerColors.length)}`, heading: at[3], label: p.callsign || p.icao || `#${p.id}` },
      geometry: { type: 'Point', coordinates: [at[1], at[0]] },
    });
  });
  map.getSource('db-planes').setData({ type: 'FeatureCollection', features });
  if (playing) {
    const bounds = map.getBounds();
    if (!bounds.contains([own[LON], own[LAT]])) map.panTo([own[LON], own[LAT]], { duration: 300 });
  }
}

function renderLegend() {
  const el = $('debrief-legend');
  if (!flight || !flight.peers.length) {
    el.innerHTML = '';
    return;
  }
  const item = (color, name) => `<span><i style="background:${color}"></i>${escapeHtml(name)}</span>`;
  el.innerHTML = item(kOwnColor, flight.callsign ? `${flight.callsign} (you)` : 'You') +
    flight.peers.map((p, i) => item(peerColor(i), p.callsign || p.icao || `#${p.id}`)).join('');
}

// --- Profile charts -------------------------------------------------------------
// Two small charts on one time axis instead of one chart with two scales:
// altitude (ft) and speed (GS/IAS, kt). Events are faint vertical lines.

const kChartH = 84;
const kPadL = 40;
const kPadR = 8;
const kPadT = 8;
const kPadB = 14;

function niceMax(v, step) {
  return Math.max(step, Math.ceil(v / step) * step);
}

function chartWidth(svg) {
  return Math.max(200, svg.clientWidth || svg.parentElement.clientWidth || 600);
}

function xScale(w) {
  const end = flight.track[flight.track.length - 1][T] || 1;
  return (t) => kPadL + (t / end) * (w - kPadL - kPadR);
}

function linePath(points) {
  return points.map(([x, y], i) => `${i ? 'L' : 'M'}${x.toFixed(1)},${y.toFixed(1)}`).join('');
}

// Every sample is a lot of path for a 3-hour flight; one per pixel column is plenty.
function thin(track, w) {
  const step = Math.max(1, Math.floor(track.length / w));
  const out = [];
  for (let i = 0; i < track.length; i += step) out.push(track[i]);
  if (out[out.length - 1] !== track[track.length - 1]) out.push(track[track.length - 1]);
  return out;
}

function gridAndAxis(w, maxV, y) {
  const lines = [];
  for (const v of [0, maxV / 2, maxV]) {
    lines.push(`<line x1="${kPadL}" x2="${w - kPadR}" y1="${y(v)}" y2="${y(v)}" stroke="#262b33" stroke-width="1"/>`);
    lines.push(`<text x="${kPadL - 4}" y="${y(v) + 3}" text-anchor="end">${Math.round(v).toLocaleString()}</text>`);
  }
  return lines.join('');
}

function eventLines(w, x) {
  return flight.events
    .map((e) => `<line x1="${x(e.t)}" x2="${x(e.t)}" y1="${kPadT}" y2="${kChartH - kPadB}" stroke="#8b93a1" stroke-width="1" stroke-dasharray="2 3" opacity="0.6"/>`)
    .join('');
}

function drawCharts() {
  if (!flight || flight.track.length < 2) {
    $('debrief-alt').innerHTML = '';
    $('debrief-speed-chart').innerHTML = '';
    return;
  }
  const alt = $('debrief-alt');
  const w = chartWidth(alt);
  const x = xScale(w);
  const pts = thin(flight.track, w - kPadL - kPadR);
  const plotH = kChartH - kPadT - kPadB;

  const altMax = niceMax(Math.max(...flight.track.map((s) => s[ALT])), 1000);
  const altMin = Math.max(0, Math.floor(Math.min(...flight.track.map((s) => s[ALT])) / 1000) * 1000);
  const ya = (v) => kPadT + plotH - ((v - altMin) / Math.max(1, altMax - altMin)) * plotH;
  const altLine = pts.map((s) => [x(s[T]), ya(s[ALT])]);
  const area = `${linePath(altLine)}L${x(pts[pts.length - 1][T]).toFixed(1)},${ya(altMin)}L${x(pts[0][T]).toFixed(1)},${ya(altMin)}Z`;
  alt.setAttribute('viewBox', `0 0 ${w} ${kChartH}`);
  alt.innerHTML = `${gridAndAxis(w, altMax, ya).replace(/>0</, `>${altMin.toLocaleString()}<`)}
    ${eventLines(w, x)}
    <path d="${area}" fill="${kOwnColor}" opacity="0.18"/>
    <path d="${linePath(altLine)}" fill="none" stroke="${kOwnColor}" stroke-width="2" stroke-linejoin="round"/>
    <text x="${kPadL + 4}" y="${kPadT + 9}" style="fill:#e6e8eb">Altitude (ft)</text>
    <line class="cursor" x1="0" x2="0" y1="${kPadT}" y2="${kChartH - kPadB}" stroke="#e6e8eb" stroke-width="1"/>
    <circle class="cursor-dot" r="4" fill="${kOwnColor}" stroke="#14171c" stroke-width="2"/>`;

  const spd = $('debrief-speed-chart');
  const spdMax = niceMax(Math.max(...flight.track.map((s) => Math.max(s[GS], s[IAS]))), 20);
  const ys = (v) => kPadT + plotH - (v / spdMax) * plotH;
  const gsLine = pts.map((s) => [x(s[T]), ys(s[GS])]);
  const iasLine = pts.map((s) => [x(s[T]), ys(s[IAS])]);
  spd.setAttribute('viewBox', `0 0 ${w} ${kChartH}`);
  spd.innerHTML = `${gridAndAxis(w, spdMax, ys)}
    ${eventLines(w, x)}
    <path d="${linePath(iasLine)}" fill="none" stroke="${kIasColor}" stroke-width="2" stroke-linejoin="round"/>
    <path d="${linePath(gsLine)}" fill="none" stroke="${kGsColor}" stroke-width="2" stroke-linejoin="round"/>
    <g transform="translate(${kPadL + 4},${kPadT + 9})">
      <text style="fill:#e6e8eb">Speed (kt)</text>
      <line x1="66" x2="80" y1="-3" y2="-3" stroke="${kGsColor}" stroke-width="2"/><text x="84">Ground speed</text>
      <line x1="162" x2="176" y1="-3" y2="-3" stroke="${kIasColor}" stroke-width="2"/><text x="180">Indicated</text>
    </g>
    <line class="cursor" x1="0" x2="0" y1="${kPadT}" y2="${kChartH - kPadB}" stroke="#e6e8eb" stroke-width="1"/>
    <circle class="cursor-dot gs" r="4" fill="${kGsColor}" stroke="#14171c" stroke-width="2"/>
    <circle class="cursor-dot ias" r="4" fill="${kIasColor}" stroke="#14171c" stroke-width="2"/>`;

  alt._scale = { x, y: ya };
  spd._scale = { x, y: ys };
  drawCursorOnCharts();
}

function drawCursorOnCharts() {
  if (!flight || flight.track.length < 2) return;
  const s = ownAt(cursorT);
  const alt = $('debrief-alt');
  const spd = $('debrief-speed-chart');
  if (alt._scale) {
    const cx = alt._scale.x(cursorT);
    alt.querySelectorAll('.cursor').forEach((l) => { l.setAttribute('x1', cx); l.setAttribute('x2', cx); });
    const dot = alt.querySelector('.cursor-dot');
    if (dot) { dot.setAttribute('cx', cx); dot.setAttribute('cy', alt._scale.y(s[ALT])); }
  }
  if (spd._scale) {
    const cx = spd._scale.x(cursorT);
    spd.querySelectorAll('.cursor').forEach((l) => { l.setAttribute('x1', cx); l.setAttribute('x2', cx); });
    const gs = spd.querySelector('.cursor-dot.gs');
    const ias = spd.querySelector('.cursor-dot.ias');
    if (gs) { gs.setAttribute('cx', cx); gs.setAttribute('cy', spd._scale.y(s[GS])); }
    if (ias) { ias.setAttribute('cx', cx); ias.setAttribute('cy', spd._scale.y(s[IAS])); }
  }
  const vs = Math.round(s[VS] / 50) * 50;
  $('debrief-readout').innerHTML = `<b>${fmtClock(cursorT)}</b> · <b>${Math.round(s[ALT]).toLocaleString()}</b> ft · ` +
    `GS <b>${Math.round(s[GS])}</b> kt · IAS <b>${Math.round(s[IAS])}</b> kt · VS <b>${vs > 0 ? '+' : ''}${vs}</b> fpm` +
    (s[GND] ? ' · on ground' : '');
  const end = flight.track[flight.track.length - 1][T] || 1;
  $('debrief-slider').value = String(Math.round((cursorT / end) * 1000));
}

function setCursor(t) {
  if (!flight || !flight.track.length) return;
  const end = flight.track[flight.track.length - 1][T];
  cursorT = Math.max(0, Math.min(end, t));
  drawCursorOnCharts();
  drawCursorOnMap();
}

function chartPointerToT(svg, ev) {
  if (!svg._scale || !flight) return null;
  const rect = svg.getBoundingClientRect();
  const w = chartWidth(svg);
  const px = ((ev.clientX - rect.left) / rect.width) * w;
  const end = flight.track[flight.track.length - 1][T] || 1;
  return ((px - kPadL) / (w - kPadL - kPadR)) * end;
}

for (const id of ['debrief-alt', 'debrief-speed-chart']) {
  const svg = $(id);
  svg.addEventListener('pointermove', (ev) => {
    const t = chartPointerToT(svg, ev);
    if (t !== null) {
      stopPlaying();
      setCursor(t);
    }
  });
}
$('debrief-slider').addEventListener('input', (ev) => {
  if (!flight || !flight.track.length) return;
  stopPlaying();
  setCursor((Number(ev.target.value) / 1000) * flight.track[flight.track.length - 1][T]);
});

// --- Events, stats -------------------------------------------------------------

function renderEvents() {
  const el = $('debrief-events');
  if (!flight) {
    el.innerHTML = '';
    return;
  }
  el.innerHTML = flight.events
    .map((e, i) => `<button type="button" data-event="${i}" title="Jump to ${fmtClock(e.t)}">${kEventGlyph[e.kind] || '•'} ${escapeHtml(e.label)}</button>`)
    .join('');
  el.querySelectorAll('[data-event]').forEach((b) =>
    b.addEventListener('click', () => {
      stopPlaying();
      const e = flight.events[Number(b.dataset.event)];
      setCursor(e.t);
      if (mapReady && (e.lat || e.lon)) map.easeTo({ center: [e.lon, e.lat], zoom: Math.max(map.getZoom(), 12), duration: 500 });
    }));
}

function renderStats(summary) {
  const el = $('debrief-stats');
  if (!flight || !summary) {
    el.innerHTML = '';
    return;
  }
  const route = flight.id === 'current' ? 'Flight in progress' : `${summary.departure || '?'} → ${summary.arrival || '?'}`;
  const best = summary.bestScore >= 0 ? ` · best <b>${summary.bestScore}</b>` : '';
  el.innerHTML = [
    `<span><b>${escapeHtml(route)}</b></span>`,
    `<span>${escapeHtml(flight.icao || '')} ${escapeHtml(flight.callsign || '')}</span>`,
    `<span>block <b>${fmtDuration(flight.end - flight.start)}</b></span>`,
    `<span>airborne <b>${fmtDuration(summary.airborneMin * 60)}</b></span>`,
    `<span><b>${summary.distanceNm.toFixed(0)}</b> nm</span>`,
    `<span>max <b>${Math.round(summary.maxAltFt).toLocaleString()}</b> ft</span>`,
    `<span><b>${summary.landings}</b> landing${summary.landings === 1 ? '' : 's'}${best}</span>`,
    flight.peers.length ? `<span>with <b>${flight.peers.length}</b> other${flight.peers.length === 1 ? '' : 's'}</span>` : '',
  ].join('');
}

// Same numbers as flights.go's summarize(), for the flight in progress.
function summarizeLocal(f) {
  let dist = 0;
  let air = 0;
  let maxAlt = 0;
  const r = Math.PI / 180;
  f.track.forEach((p, i) => {
    maxAlt = Math.max(maxAlt, p[ALT]);
    if (!i) return;
    const q = f.track[i - 1];
    const a = Math.sin(((p[LAT] - q[LAT]) * r) / 2) ** 2 + Math.cos(q[LAT] * r) * Math.cos(p[LAT] * r) * Math.sin(((p[LON] - q[LON]) * r) / 2) ** 2;
    dist += (2 * 6371008.8 * Math.asin(Math.min(1, Math.sqrt(a)))) / 1852;
    if (!p[GND] || !q[GND]) air += (p[T] - q[T]) / 60;
  });
  const best = f.landings.reduce((m, l) => Math.max(m, l.score), -1);
  return { departure: '', arrival: '', airborneMin: air, distanceNm: dist, maxAltFt: maxAlt, landings: f.landings.length, bestScore: best };
}

// --- Flight selection -----------------------------------------------------------

function optionLabel(s) {
  const score = s.bestScore >= 0 ? ` · score ${s.bestScore}` : '';
  return `${fmtDate(s.start)} · ${s.departure || '?'} → ${s.arrival || '?'} · ${s.icao || '?'} · ${fmtDuration(s.airborneMin * 60)}${score}${s.peers ? ` · +${s.peers}` : ''}`;
}

function renderFlightSelect(selectId) {
  const sel = $('debrief-flight');
  const opts = [];
  if (recordingNow) opts.push('<option value="current">● Recording now - flight in progress</option>');
  for (const s of flights) opts.push(`<option value="${escapeHtml(s.id)}">${escapeHtml(optionLabel(s))}</option>`);
  if (!opts.length) opts.push('<option value="">No flights recorded yet</option>');
  sel.innerHTML = opts.join('');
  if (selectId && [...sel.options].some((o) => o.value === selectId)) sel.value = selectId;
  $('debrief-delete').disabled = !sel.value || sel.value === 'current';
  $('debrief-play').disabled = !sel.value;
}

async function openFlight(id, keepCursor = false) {
  stopPlaying();
  clearInterval(liveTimer);
  liveTimer = null;
  if (!id) {
    flight = null;
    renderStats(null);
    renderEvents();
    renderLegend();
    drawCharts();
    setMessage('Flights are recorded automatically - from pushback to parking. Take off and they show up here.');
    if (mapReady) {
      map.getSource('db-tracks').setData(emptyFc());
      map.getSource('db-events').setData(emptyFc());
      map.getSource('db-planes').setData(emptyFc());
    }
    return;
  }
  try {
    const previous = flight && flight.id === id;
    flight = await LoadFlight(id);
    flight.events = flight.events || [];
    flight.peers = flight.peers || [];
    flight.landings = flight.landings || [];
    flight.track = flight.track || [];
    if (!mapFailed) setMessage('');
    const summary = id === 'current' ? summarizeLocal(flight) : flights.find((s) => s.id === id);
    renderStats(summary);
    renderEvents();
    renderLegend();
    const end = flight.track.length ? flight.track[flight.track.length - 1][T] : 0;
    cursorT = keepCursor && previous ? Math.min(cursorT, end) : id === 'current' ? end : 0;
    drawCharts();
    drawFlightOnMap(!(keepCursor && previous));
    if (id === 'current') {
      // Live: refresh while this page shows the flight in progress.
      liveTimer = setInterval(() => {
        if ($('page-debrief').classList.contains('active') && $('debrief-flight').value === 'current' && !playing) {
          openFlight('current', true);
        }
      }, 5000);
    }
  } catch (e) {
    flight = null;
    setMessage(`Could not load this flight: ${e}`);
  }
}

async function refreshList(selectId) {
  try {
    flights = (await ListFlights()) || [];
  } catch (e) {
    flights = [];
    setMessage(String(e));
  }
  const want = selectId ?? $('debrief-flight').value ?? '';
  renderFlightSelect(want);
  const sel = $('debrief-flight').value;
  if (!flight || flight.id !== sel) openFlight(sel);
}

$('debrief-flight').addEventListener('change', (e) => {
  $('debrief-delete').disabled = !e.target.value || e.target.value === 'current';
  openFlight(e.target.value);
});

$('debrief-delete').addEventListener('click', async () => {
  const id = $('debrief-flight').value;
  if (!id || id === 'current') return;
  if (!confirm('Delete this flight for good?')) return;
  try {
    await DeleteFlight(id);
  } catch (e) {
    alert(e);
  }
  flight = null;
  refreshList('');
});

// --- Replay -------------------------------------------------------------------------

function stopPlaying() {
  playing = false;
  $('debrief-play').textContent = '▶ Play';
}

function frame(now) {
  if (!playing || !flight) return;
  const dt = Math.min(0.1, (now - lastFrame) / 1000);
  lastFrame = now;
  const end = flight.track[flight.track.length - 1][T];
  setCursor(cursorT + dt * Number($('debrief-speed').value));
  if (cursorT >= end) {
    stopPlaying();
    return;
  }
  requestAnimationFrame(frame);
}

$('debrief-play').addEventListener('click', () => {
  if (!flight || flight.track.length < 2) return;
  if (playing) {
    stopPlaying();
    return;
  }
  if (cursorT >= flight.track[flight.track.length - 1][T] - 1) setCursor(0);
  playing = true;
  $('debrief-play').textContent = '❚❚ Pause';
  lastFrame = performance.now();
  requestAnimationFrame(frame);
});

new ResizeObserver(() => {
  if ($('page-debrief').classList.contains('active')) drawCharts();
}).observe($('debrief-charts'));

// --- Entry points (main.js) -----------------------------------------------------

export function showDebrief() {
  $('nav-dot-debrief').className = 'nav-dot';
  initMap();
  if (map) map.resize();
  refreshList();
}

// Opens one flight (from the Logbook).
export function showFlight(id) {
  initMap();
  refreshList(id);
}

export function updateDebrief(data, pageVisible) {
  const wasRecording = recordingNow;
  recordingNow = !!data.recording;
  const listChanged = data.flightsVersion !== shownFlightsVersion;
  shownFlightsVersion = data.flightsVersion;
  if (!pageVisible) return;
  if (listChanged || wasRecording !== recordingNow) {
    // A just-finished flight replaces "recording now" in the list.
    const sel = $('debrief-flight').value;
    refreshList(sel === 'current' && !recordingNow ? undefined : sel);
  }
}
