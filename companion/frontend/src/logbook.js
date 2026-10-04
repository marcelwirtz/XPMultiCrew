// Logbook page (companion/logbook.go): every recorded flight with who flew
// along, totals, flying buddies, a map of everything flown and the
// airports visited, and milestones. Clicking a flight or milestone opens
// it on the Debrief page. Loaded the first time the page opens (MapLibre).
import * as maplibregl from 'maplibre-gl';
import 'maplibre-gl/dist/maplibre-gl.css';
import workerUrl from 'maplibre-gl/dist/maplibre-gl-worker.mjs?worker&url';

import { GetLogbook } from '../wailsjs/go/main/App';

maplibregl.setWorkerUrl(workerUrl);

const $ = (id) => document.getElementById(id);
const kStyle = 'https://tiles.openfreemap.org/styles/dark';
const kSoloColor = '#4da3ff';
const kTogetherColor = '#d95926';

let map = null;
let mapReady = false;
let mapFailed = false;
let logbook = null;

function esc(text) {
  return String(text ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
}

function hours(min) {
  const h = Math.floor(min / 60);
  return `${h}:${String(Math.round(min % 60)).padStart(2, '0')}`;
}

function date(unix) {
  return new Date(unix * 1000).toLocaleDateString(undefined, { year: '2-digit', month: '2-digit', day: '2-digit' });
}

function setMessage(text) {
  const el = $('logbook-message');
  el.style.display = text ? '' : 'none';
  el.textContent = text || '';
}

function openFlight(id) {
  window.dispatchEvent(new CustomEvent('xpmc-open-flight', { detail: { id } }));
}

function renderTiles(t) {
  const tiles = [
    [t.flights, 'Flights'],
    [hours(t.airborneMin), 'Hours in the air'],
    [hours(t.minTogether), `Hours together (${t.flightsTogether} flights)`],
    [t.airports, 'Airports'],
    [Math.round(t.distanceNm).toLocaleString(), 'Nautical miles'],
    [t.bestScore >= 0 ? t.bestScore : '–', `Best landing (${t.landings} landings)`],
    [t.nightLandings, 'Night landings'],
  ];
  $('logbook-tiles').innerHTML = tiles.map(([v, k]) => `<div class="lbk-tile"><div class="v">${esc(v)}</div><div class="k">${esc(k)}</div></div>`).join('');
}

function renderMilestones(list) {
  $('logbook-milestones').innerHTML = list.length
    ? list.map((m) => `<div class="lbk-ms link" data-id="${esc(m.flightId)}"><div class="when">${date(m.date)}</div>
        <div class="what"><b>${esc(m.title)}</b>${m.detail ? `<span>${esc(m.detail)}</span>` : ''}</div></div>`).join('')
    : '<div class="hint" style="margin-top: 0;">Your firsts and records show up here.</div>';
}

function renderBuddies(list) {
  $('logbook-buddies').innerHTML = list.length
    ? list.map((b) => `<div class="lbk-ms"><div class="when">${hours(b.minutes)} h</div>
        <div class="what"><b>${esc(b.callsign)}</b><span>${b.flights} flight${b.flights === 1 ? '' : 's'} together · last ${date(b.last)}</span></div></div>`).join('')
    : '<div class="hint" style="margin-top: 0;">Fly with someone in a Multiplayer session or Shared Cockpit and they show up here.</div>';
}

function renderTable(entries) {
  if (!entries.length) {
    $('logbook-table').innerHTML = '';
    return;
  }
  const rows = entries.map((e) => {
    const route = [e.departure || '?', e.arrival || '?'].join(' → ');
    const score = e.bestScore >= 0 ? e.bestScore : '';
    return `<tr class="link" data-id="${esc(e.id)}"><td>${date(e.start)}</td><td>${esc(route)}${e.night ? ' <span class="lbk-night" title="Night landing">☾</span>' : ''}</td>
      <td>${esc(e.icao)}</td><td>${hours(e.airborneMin)}</td><td>${Math.round(e.distanceNm)}</td><td>${e.landings}</td><td>${score}</td>
      <td>${esc(e.buddies.join(', '))}</td></tr>`;
  });
  $('logbook-table').innerHTML = '<tr><th>Date</th><th>Route</th><th>Aircraft</th><th>Time</th><th>NM</th><th>Ldg</th><th>Best</th><th>With</th></tr>' + rows.join('');
}

function geo() {
  const tracks = { type: 'FeatureCollection', features: [] };
  const airports = { type: 'FeatureCollection', features: [] };
  if (!logbook) return { tracks, airports };
  for (const e of logbook.entries) {
    if (e.track.length < 2) continue;
    tracks.features.push({ type: 'Feature', geometry: { type: 'LineString', coordinates: e.track }, properties: { id: e.id, together: e.buddies.length > 0 } });
  }
  for (const a of logbook.airports) {
    airports.features.push({ type: 'Feature', geometry: { type: 'Point', coordinates: [a.lon, a.lat] }, properties: { ident: a.ident, visits: a.visits } });
  }
  return { tracks, airports };
}

function renderMap() {
  if (!map || !mapReady || !logbook) return;
  const { tracks, airports } = geo();
  map.getSource('lbk-tracks').setData(tracks);
  map.getSource('lbk-airports').setData(airports);
  const pts = tracks.features.flatMap((f) => f.geometry.coordinates).concat(airports.features.map((f) => f.geometry.coordinates));
  if (pts.length) {
    const b = pts.reduce((acc, p) => acc.extend(p), new maplibregl.LngLatBounds(pts[0], pts[0]));
    map.fitBounds(b, { padding: 40, maxZoom: 9, duration: 0 });
  }
}

function initMap() {
  if (map || mapFailed) return;
  try {
    map = new maplibregl.Map({ container: 'logbook-map', style: kStyle, center: [8.5, 50], zoom: 5, attributionControl: { compact: true } });
  } catch (e) {
    mapFailed = true;
    setMessage(`The map could not be started (${e.message || e}).`);
    return;
  }
  map.addControl(new maplibregl.NavigationControl({ showCompass: false }), 'top-right');
  map.on('load', () => {
    map.addSource('lbk-tracks', { type: 'geojson', data: { type: 'FeatureCollection', features: [] } });
    map.addSource('lbk-airports', { type: 'geojson', data: { type: 'FeatureCollection', features: [] } });
    map.addLayer({
      id: 'lbk-tracks', type: 'line', source: 'lbk-tracks',
      layout: { 'line-cap': 'round', 'line-join': 'round' },
      paint: { 'line-color': ['case', ['get', 'together'], kTogetherColor, kSoloColor], 'line-width': 2, 'line-opacity': 0.85 },
    });
    map.addLayer({
      id: 'lbk-airports', type: 'circle', source: 'lbk-airports',
      paint: { 'circle-radius': ['min', 9, ['+', 4, ['get', 'visits']]], 'circle-color': '#e6e8eb', 'circle-stroke-color': '#14171c', 'circle-stroke-width': 2 },
    });
    map.addLayer({
      id: 'lbk-airport-labels', type: 'symbol', source: 'lbk-airports',
      layout: { 'text-field': ['get', 'ident'], 'text-font': ['Noto Sans Regular'], 'text-size': 11, 'text-offset': [0, 1.2], 'text-anchor': 'top' },
      paint: { 'text-color': '#e6e8eb', 'text-halo-color': '#14171c', 'text-halo-width': 1.2 },
    });
    map.on('click', 'lbk-tracks', (e) => e.features[0] && openFlight(e.features[0].properties.id));
    map.on('mouseenter', 'lbk-tracks', () => { map.getCanvas().style.cursor = 'pointer'; });
    map.on('mouseleave', 'lbk-tracks', () => { map.getCanvas().style.cursor = ''; });
    mapReady = true;
    renderMap();
  });
}

$('logbook-milestones').addEventListener('click', (e) => {
  const row = e.target.closest('[data-id]');
  if (row && row.dataset.id) openFlight(row.dataset.id);
});
$('logbook-table').addEventListener('click', (e) => {
  const row = e.target.closest('tr[data-id]');
  if (row) openFlight(row.dataset.id);
});

export async function showLogbook() {
  initMap();
  if (map) map.resize();
  try {
    logbook = await GetLogbook();
    setMessage(logbook.entries.length ? '' : 'No flights yet - every flight is recorded automatically and lands here.');
  } catch (e) {
    setMessage(String(e));
    return;
  }
  renderTiles(logbook.totals);
  renderMilestones(logbook.milestones);
  renderBuddies(logbook.buddies);
  renderTable(logbook.entries);
  renderMap();
}
