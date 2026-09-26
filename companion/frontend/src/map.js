// Map page: your own aircraft and the Formation peers on an OpenFreeMap
// base map (MapLibre GL), plus data read at runtime from the user's own
// X-Plane installation - airports/runways (companion/airports.go), VFR
// reporting points, VORs, NDBs and airspaces (companion/navdata.go) - and a
// route planner that exports to X-Plane's GPS/FMS and can be shared with
// the Multiplayer session (companion/routes.go).
//
// Base map: OpenFreeMap (https://openfreemap.org) - free, no API key, no
// usage limits, commercial use allowed; attribution "OpenFreeMap ©
// OpenMapTiles Data from OpenStreetMap" is shown by MapLibre's attribution
// control from the style's own source attribution. MapLibre GL JS is
// BSD-3-Clause, bundled (see THIRD_PARTY_NOTICES.md).
import * as maplibregl from 'maplibre-gl';
import 'maplibre-gl/dist/maplibre-gl.css';
// MapLibre derives its web worker's URL from its own module URL, but only
// for http(s) pages - Wails serves the app from its own scheme on Linux
// (wails://), so the worker would never load. Let Vite build the worker as
// a separate asset and point MapLibre at it explicitly.
import workerUrl from 'maplibre-gl/dist/maplibre-gl-worker.mjs?worker&url';

import {
  ClearSharedRoute,
  ExportFms,
  GetAirportInfo,
  GetAirports,
  GetAirspaces,
  GetNavData,
  ImportFms,
  ShareRoute,
} from '../wailsjs/go/main/App';
import { distanceNm, legInfo, trueCourse, variationAt, windAt } from './route-math.js';
import { activeIndex, setActiveIndex } from './route-progress.js';
import { clearTracks, tracksGeoJson } from './tracks.js';

maplibregl.setWorkerUrl(workerUrl);

const kStyles = {
  dark: 'https://tiles.openfreemap.org/styles/dark',
  liberty: 'https://tiles.openfreemap.org/styles/liberty',
};
const kStyleStorageKey = 'xpmulticrew.mapStyle';
const kFollowStorageKey = 'xpmulticrew.mapFollow';
const kRouteStorageKey = 'xpmulticrew.route';
const kLayerStorageKey = 'xpmulticrew.mapLayers';

let map = null;
let mapFailed = false;
let styleReady = false; // own flag: map.isStyleLoaded() stays false while tiles are still loading
let lastData = null; // latest status event, applied once the map/style is ready
let airportData = null; // cached across style switches
let navData = null; // { points: [...] }
let vors = []; // VORs, for the route planner's magnetic variation
let followSelf = true;
let firstFix = true;
let routeMode = false;
let route = { name: '', cruiseFt: 3500, tasKt: 100, waypoints: [] };
let airspaceTimer = null;
let airspaceSeq = 0;
let measureMode = false;
let measurePoints = [];
let routeListSignature = '';

function storageGet(key) {
  try {
    return localStorage.getItem(key);
  } catch (e) {
    return null;
  }
}
function storageSet(key, value) {
  try {
    localStorage.setItem(key, value);
  } catch (e) {
    // per-viewer convenience only
  }
}

function escapeHtml(text) {
  return String(text ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
}

function setMapMessage(text) {
  const el = document.getElementById('map-message');
  el.textContent = text || '';
  el.style.display = text ? '' : 'none';
}

// --- Symbols, drawn once on a canvas ---------------------------------------

function canvasImage(size, draw) {
  const canvas = document.createElement('canvas');
  canvas.width = size;
  canvas.height = size;
  const ctx = canvas.getContext('2d');
  ctx.translate(size / 2, size / 2);
  draw(ctx);
  return ctx.getImageData(0, 0, size, size);
}

// A simple top-down airplane silhouette, rotated per aircraft by heading.
function makePlaneImage(fill) {
  return canvasImage(48, (ctx) => {
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
  });
}

// Chart-style symbols: VRP triangle, VOR hexagon, NDB dotted circle.
function makeVrpImage() {
  return canvasImage(24, (ctx) => {
    ctx.fillStyle = '#7fb2ff';
    ctx.strokeStyle = '#14171c';
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    ctx.moveTo(0, -8);
    ctx.lineTo(7, 5);
    ctx.lineTo(-7, 5);
    ctx.closePath();
    ctx.fill();
    ctx.stroke();
  });
}
function makeVorImage() {
  return canvasImage(28, (ctx) => {
    ctx.strokeStyle = '#4dd0e1';
    ctx.lineWidth = 2;
    ctx.beginPath();
    for (let i = 0; i < 6; i++) {
      const a = (Math.PI / 3) * i;
      ctx.lineTo(9 * Math.cos(a), 9 * Math.sin(a));
    }
    ctx.closePath();
    ctx.stroke();
    ctx.fillStyle = '#4dd0e1';
    ctx.beginPath();
    ctx.arc(0, 0, 2, 0, 2 * Math.PI);
    ctx.fill();
  });
}
function makeNdbImage() {
  return canvasImage(28, (ctx) => {
    ctx.fillStyle = '#c77dff';
    for (let i = 0; i < 12; i++) {
      const a = (Math.PI / 6) * i;
      ctx.beginPath();
      ctx.arc(9 * Math.cos(a), 9 * Math.sin(a), 1.4, 0, 2 * Math.PI);
      ctx.fill();
    }
    ctx.beginPath();
    ctx.arc(0, 0, 2.5, 0, 2 * Math.PI);
    ctx.fill();
  });
}

function formatAltitude(ft) {
  return ft >= 18000 ? `FL${Math.round(ft / 100)}` : `${Math.round(ft).toLocaleString()} ft`;
}

function textFont() {
  // OpenFreeMap's glyph server provides the Noto Sans family.
  return ['Noto Sans Regular'];
}

// --- GeoJSON builders --------------------------------------------------------

function callsignOf(senderId) {
  const peer = ((lastData && lastData.peers) || []).find((p) => p.id === senderId);
  return peer ? peer.callsign || peer.icao || `Peer ${peer.id}` : 'Someone';
}

function aircraftGeoJson(data) {
  const features = [];
  if (data.selfPos) {
    const s = data.selfPos;
    features.push({
      type: 'Feature',
      geometry: { type: 'Point', coordinates: [s.lon, s.lat] },
      properties: { self: true, heading: s.heading, label: `You\n${formatAltitude(s.altFt)} · ${Math.round(s.groundspeedKt || 0)} kt` },
    });
  }
  for (const p of data.peerPos || []) {
    features.push({
      type: 'Feature',
      geometry: { type: 'Point', coordinates: [p.lon, p.lat] },
      properties: { self: false, heading: p.heading, label: `${callsignOf(p.id)}\n${formatAltitude(p.altFt)}` },
    });
  }
  return { type: 'FeatureCollection', features };
}

function airportsGeoJson(data) {
  return {
    type: 'FeatureCollection',
    features: data.airports.map(([ident, name, lat, lon, kind]) => ({
      type: 'Feature',
      geometry: { type: 'Point', coordinates: [lon, lat] },
      properties: { ident, name, kind },
    })),
  };
}

function runwaysGeoJson(data) {
  return {
    type: 'FeatureCollection',
    features: data.runways.map(([lat1, lon1, lat2, lon2]) => ({
      type: 'Feature',
      geometry: { type: 'LineString', coordinates: [[lon1, lat1], [lon2, lat2]] },
      properties: {},
    })),
  };
}

function navGeoJson(data) {
  return {
    type: 'FeatureCollection',
    features: data.points.map((p) => ({
      type: 'Feature',
      geometry: { type: 'Point', coordinates: [p.lon, p.lat] },
      properties: {
        kind: p.kind,
        ident: p.ident,
        name: p.name,
        airport: p.airport || '',
        label: p.kind === 'VRP' ? p.name : `${p.ident} ${p.freq}`,
      },
    })),
  };
}

function airspaceGeoJson(list) {
  return {
    type: 'FeatureCollection',
    features: list.map((a) => ({
      type: 'Feature',
      geometry: { type: 'Polygon', coordinates: [[...a.poly, a.poly[0]]] },
      properties: {
        name: a.name,
        class: a.class,
        label: `${a.name}\n${a.lowerGnd ? 'GND' : `${a.lowerFt} ft`}–${a.upperFt >= 99999 ? 'UNL' : `${a.upperFt} ft`}`,
      },
    })),
  };
}

function routeGeoJson(waypoints, props = {}) {
  const features = [];
  if (waypoints.length >= 2) {
    features.push({
      type: 'Feature',
      geometry: { type: 'LineString', coordinates: waypoints.map((w) => [w.lon, w.lat]) },
      properties: { ...props, line: true },
    });
  }
  waypoints.forEach((w, i) => {
    features.push({
      type: 'Feature',
      geometry: { type: 'Point', coordinates: [w.lon, w.lat] },
      properties: { ...props, label: w.ident || `WPT${i + 1}` },
    });
  });
  return { type: 'FeatureCollection', features };
}

// --- Layers ------------------------------------------------------------------
// Bottom to top: airspaces, runways/airports, navaids/VRPs, shared route,
// own route, aircraft. Sources that arrive late are inserted below the
// first layer from this list that already exists.
const kLayerOrder = ['airspace-fill', 'airspace-line', 'airspace-labels', 'runways', 'airports', 'airport-labels',
  'nav-points', 'tracks-line', 'shared-route-line', 'shared-route-points', 'route-line', 'route-points',
  'measure-line', 'measure-label', 'aircraft'];

function beforeIdFor(id) {
  const idx = kLayerOrder.indexOf(id);
  for (let i = idx + 1; i < kLayerOrder.length; i++) {
    if (map.getLayer(kLayerOrder[i])) return kLayerOrder[i];
  }
  return undefined;
}

function addLayer(layer) {
  if (!map.getLayer(layer.id)) map.addLayer(layer, beforeIdFor(layer.id));
}

function addAirspaceLayers() {
  if (map.getSource('airspaces')) return;
  map.addSource('airspaces', { type: 'geojson', data: airspaceGeoJson([]) });
  const color = ['match', ['get', 'class'], 'CTR', '#ff6b6b', 'P', '#ff4d4d', 'R', '#ff4d4d', 'Q', '#ff8c42',
    'A', '#2f6fd6', 'B', '#2f6fd6', 'C', '#4da3ff', 'D', '#4da3ff', '#8b93a1'];
  addLayer({ id: 'airspace-fill', type: 'fill', source: 'airspaces', paint: { 'fill-color': color, 'fill-opacity': 0.07 } });
  addLayer({
    id: 'airspace-line',
    type: 'line',
    source: 'airspaces',
    paint: {
      'line-color': color,
      'line-width': ['match', ['get', 'class'], 'CTR', 2, 'P', 2, 'R', 2, 1.2],
      'line-dasharray': ['match', ['get', 'class'], 'CTR', ['literal', [3, 2]], ['literal', [1, 0]]],
      'line-opacity': 0.85,
    },
  });
  addLayer({
    id: 'airspace-labels',
    type: 'symbol',
    source: 'airspaces',
    minzoom: 8,
    layout: { 'text-field': ['get', 'label'], 'text-font': textFont(), 'text-size': 10, 'symbol-placement': 'point' },
    paint: { 'text-color': color, 'text-halo-color': '#14171c', 'text-halo-width': 1.2 },
  });
}

function addAirportLayers() {
  if (!airportData || map.getSource('airports')) return;
  map.addSource('runways', { type: 'geojson', data: runwaysGeoJson(airportData) });
  addLayer({
    id: 'runways',
    type: 'line',
    source: 'runways',
    minzoom: 9,
    paint: { 'line-color': '#b8c0cc', 'line-width': ['interpolate', ['linear'], ['zoom'], 9, 1.5, 14, 8] },
  });
  map.addSource('airports', { type: 'geojson', data: airportsGeoJson(airportData) });
  addLayer({
    id: 'airports',
    type: 'circle',
    source: 'airports',
    minzoom: 6,
    paint: {
      'circle-radius': ['interpolate', ['linear'], ['zoom'], 6, 2, 10, 4],
      'circle-color': ['match', ['get', 'kind'], 17, '#c77dff', 16, '#4dd0e1', '#e6e8eb'],
      'circle-stroke-color': '#14171c',
      'circle-stroke-width': 1,
    },
  });
  addLayer({
    id: 'airport-labels',
    type: 'symbol',
    source: 'airports',
    minzoom: 8,
    layout: { 'text-field': ['get', 'ident'], 'text-font': textFont(), 'text-size': 11, 'text-offset': [0, 1.1], 'text-anchor': 'top' },
    paint: { 'text-color': '#e6e8eb', 'text-halo-color': '#14171c', 'text-halo-width': 1.2 },
  });
}

function addNavLayers() {
  if (!navData || map.getSource('nav')) return;
  if (!map.hasImage('sym-vrp')) map.addImage('sym-vrp', makeVrpImage(), { pixelRatio: 2 });
  if (!map.hasImage('sym-vor')) map.addImage('sym-vor', makeVorImage(), { pixelRatio: 2 });
  if (!map.hasImage('sym-ndb')) map.addImage('sym-ndb', makeNdbImage(), { pixelRatio: 2 });
  map.addSource('nav', { type: 'geojson', data: navGeoJson(navData) });
  addLayer({
    id: 'nav-points',
    type: 'symbol',
    source: 'nav',
    // VRPs only from zoom 8 (hundreds per country), VOR/NDB from 6.
    filter: ['any', ['!=', ['get', 'kind'], 'VRP'], ['>=', ['zoom'], 8]],
    minzoom: 6,
    layout: {
      'icon-image': ['match', ['get', 'kind'], 'VRP', 'sym-vrp', 'VOR', 'sym-vor', 'sym-ndb'],
      'icon-allow-overlap': true,
      'text-field': ['step', ['zoom'], '', 8, ['get', 'label']],
      'text-font': textFont(),
      'text-size': 10,
      'text-offset': [0, 1.2],
      'text-anchor': 'top',
      'text-optional': true,
    },
    paint: {
      'text-color': ['match', ['get', 'kind'], 'VRP', '#9cc3ff', 'VOR', '#4dd0e1', '#c77dff'],
      'text-halo-color': '#14171c',
      'text-halo-width': 1.2,
    },
  });
}

function addRouteLayers() {
  for (const [prefix, color, dash] of [['shared-route', '#ffb347', [2, 2]], ['route', '#f5d142', [1, 0]]]) {
    if (map.getSource(prefix)) continue;
    map.addSource(prefix, { type: 'geojson', data: routeGeoJson([]) });
    addLayer({
      id: `${prefix}-line`,
      type: 'line',
      source: prefix,
      filter: ['==', ['get', 'line'], true],
      paint: { 'line-color': color, 'line-width': 3, 'line-dasharray': ['literal', dash] },
    });
    addLayer({
      id: `${prefix}-points`,
      type: 'circle',
      source: prefix,
      filter: ['!=', ['get', 'line'], true],
      paint: {
        'circle-radius': ['case', ['==', ['get', 'active'], true], 7, 5],
        'circle-color': ['case', ['==', ['get', 'passed'], true], '#6b7280', color],
        'circle-stroke-color': ['case', ['==', ['get', 'active'], true], '#ffffff', '#14171c'],
        'circle-stroke-width': 1.5,
      },
    });
  }
}

function addTracksLayer() {
  if (map.getSource('tracks')) return;
  map.addSource('tracks', { type: 'geojson', data: tracksGeoJson() });
  addLayer({
    id: 'tracks-line',
    type: 'line',
    source: 'tracks',
    paint: {
      'line-color': ['case', ['get', 'self'], '#4da3ff', '#ffb347'],
      'line-width': 2,
      'line-opacity': 0.6,
    },
  });
}

function addMeasureLayers() {
  if (map.getSource('measure')) return;
  map.addSource('measure', { type: 'geojson', data: { type: 'FeatureCollection', features: [] } });
  addLayer({
    id: 'measure-line',
    type: 'line',
    source: 'measure',
    filter: ['==', ['geometry-type'], 'LineString'],
    paint: { 'line-color': '#ffffff', 'line-width': 2, 'line-dasharray': ['literal', [2, 1]] },
  });
  addLayer({
    id: 'measure-label',
    type: 'symbol',
    source: 'measure',
    filter: ['==', ['geometry-type'], 'Point'],
    layout: { 'text-field': ['get', 'label'], 'text-font': textFont(), 'text-size': 12, 'text-allow-overlap': true },
    paint: { 'text-color': '#ffffff', 'text-halo-color': '#14171c', 'text-halo-width': 2 },
  });
}

function addAircraftLayer() {
  if (!map.hasImage('plane-self')) map.addImage('plane-self', makePlaneImage('#4da3ff'), { pixelRatio: 2 });
  if (!map.hasImage('plane-peer')) map.addImage('plane-peer', makePlaneImage('#ffb347'), { pixelRatio: 2 });
  if (map.getSource('aircraft')) return;
  map.addSource('aircraft', { type: 'geojson', data: aircraftGeoJson(lastData || {}) });
  addLayer({
    id: 'aircraft',
    type: 'symbol',
    source: 'aircraft',
    layout: {
      'icon-image': ['case', ['get', 'self'], 'plane-self', 'plane-peer'],
      'icon-rotate': ['get', 'heading'],
      'icon-rotation-alignment': 'map',
      'icon-allow-overlap': true,
      'icon-ignore-placement': true,
      'text-field': ['get', 'label'],
      'text-font': textFont(),
      'text-size': 12,
      'text-offset': [0, 1.6],
      'text-anchor': 'top',
      'text-allow-overlap': true,
    },
    paint: {
      'text-color': ['case', ['get', 'self'], '#4da3ff', '#ffb347'],
      'text-halo-color': '#14171c',
      'text-halo-width': 1.5,
    },
  });
}

// (Re-)adds everything on top of the base style - needed after every
// setStyle(), which throws away custom sources, layers and images.
function addOverlays() {
  addAircraftLayer();
  addRouteLayers();
  addTracksLayer();
  addMeasureLayers();
  addAirspaceLayers();
  addAirportLayers();
  addNavLayers();
  applyLayerVisibility();
  renderRoute();
  refreshAirspaces();
}

function layerPrefs() {
  try {
    return { vfr: true, airspace: true, tracks: true, ...JSON.parse(storageGet(kLayerStorageKey) || '{}') };
  } catch (e) {
    return { vfr: true, airspace: true, tracks: true };
  }
}

function applyLayerVisibility() {
  if (!map || !styleReady) return;
  const prefs = layerPrefs();
  const set = (id, on) => map.getLayer(id) && map.setLayoutProperty(id, 'visibility', on ? 'visible' : 'none');
  set('nav-points', prefs.vfr);
  set('tracks-line', prefs.tracks);
  for (const id of ['airspace-fill', 'airspace-line', 'airspace-labels']) set(id, prefs.airspace);
}

// Airspaces come per visible area (there are ~24k worldwide) and only from
// zoom 6 on - below that they'd just be a wall of outlines.
function refreshAirspaces() {
  if (!map || !styleReady || !map.getSource('airspaces')) return;
  clearTimeout(airspaceTimer);
  airspaceTimer = setTimeout(async () => {
    const source = map.getSource('airspaces');
    if (!source) return;
    if (map.getZoom() < 6 || !layerPrefs().airspace) {
      source.setData(airspaceGeoJson([]));
      return;
    }
    const seq = ++airspaceSeq;
    const b = map.getBounds();
    try {
      const list = await GetAirspaces(b.getWest(), b.getSouth(), b.getEast(), b.getNorth());
      if (seq === airspaceSeq && map.getSource('airspaces')) map.getSource('airspaces').setData(airspaceGeoJson(list || []));
    } catch (e) {
      // no airspace file in this X-Plane - the other layers still work
    }
  }, 300);
}

// --- Aircraft + follow -------------------------------------------------------

function applyData() {
  if (!map || !lastData || !map.getSource('aircraft')) return;
  map.getSource('aircraft').setData(aircraftGeoJson(lastData));
  if (map.getSource('tracks')) map.getSource('tracks').setData(tracksGeoJson());
  renderSharedRoute();
  renderRoute(); // cheap unless wind/active waypoint changed - see routeListSignature
  const self = lastData.selfPos;
  if (self && followSelf) {
    if (firstFix) {
      map.jumpTo({ center: [self.lon, self.lat], zoom: 10 });
      firstFix = false;
    } else {
      map.easeTo({ center: [self.lon, self.lat], duration: 900 });
    }
  }
}

function fitAll() {
  if (!map || !lastData) return;
  const points = [];
  if (lastData.selfPos) points.push([lastData.selfPos.lon, lastData.selfPos.lat]);
  for (const p of lastData.peerPos || []) points.push([p.lon, p.lat]);
  if (points.length === 0) return;
  setFollow(false);
  if (points.length === 1) {
    map.easeTo({ center: points[0], zoom: 11 });
    return;
  }
  const bounds = points.reduce((b, pt) => b.extend(pt), new maplibregl.LngLatBounds(points[0], points[0]));
  map.fitBounds(bounds, { padding: 80, maxZoom: 13, duration: 800 });
}

function setFollow(on) {
  followSelf = on;
  document.getElementById('map-follow').checked = on;
  storageSet(kFollowStorageKey, on ? '1' : '0');
  if (on) applyData();
}

// --- Route planner -----------------------------------------------------------

function loadRoute() {
  try {
    const stored = JSON.parse(storageGet(kRouteStorageKey) || 'null');
    if (stored && Array.isArray(stored.waypoints)) route = { ...route, ...stored };
  } catch (e) {
    // start empty
  }
  document.getElementById('route-name').value = route.name || '';
  document.getElementById('route-cruise').value = route.cruiseFt || 3500;
  document.getElementById('route-tas').value = route.tasKt || 100;
}

function saveRoute() {
  storageSet(kRouteStorageKey, JSON.stringify(route));
}

function readRouteFields() {
  route.name = document.getElementById('route-name').value.trim();
  route.cruiseFt = Math.max(0, parseInt(document.getElementById('route-cruise').value, 10) || 0);
  route.tasKt = Math.max(30, parseInt(document.getElementById('route-tas').value, 10) || 100);
}

function formatMinutes(min) {
  const total = Math.round(min);
  return `${Math.floor(total / 60)}:${String(total % 60).padStart(2, '0')}`;
}

function pad3(deg) {
  return String(Math.round(deg) % 360).padStart(3, '0');
}

// Wind for planning: X-Plane's current wind layers (plugin WIND line),
// interpolated at the planned cruise altitude - constant along the route.
function planningWind() {
  return windAt(lastData && lastData.wind, route.cruiseFt);
}

function renderRoute() {
  readRouteFields();
  saveRoute();
  const wind = planningWind();
  const active = activeIndex(route.waypoints.length >= 2 ? route : null);
  if (map && map.getSource('route')) {
    const geo = routeGeoJson(route.waypoints);
    let pointIndex = 0;
    for (const f of geo.features) {
      if (f.properties.line) continue;
      // Waypoints before the one we're flying to count as passed.
      f.properties.passed = route.waypoints.length >= 2 && pointIndex < active;
      f.properties.active = route.waypoints.length >= 2 && pointIndex === active;
      pointIndex++;
    }
    map.getSource('route').setData(geo);
  }

  // The list only changes with the route, wind or active waypoint - not
  // rebuilt every second, so its buttons stay clickable.
  const signature = JSON.stringify([route, wind && [Math.round(wind.fromDeg), Math.round(wind.speedKt)], active, vors.length]);
  if (signature === routeListSignature) return;
  routeListSignature = signature;

  const list = document.getElementById('route-list');
  const windEl = document.getElementById('route-wind');
  windEl.textContent = wind
    ? `Wind at ${route.cruiseFt} ft: ${pad3(wind.fromDeg)}°/${Math.round(wind.speedKt)} kt (X-Plane, now)`
    : 'No wind data yet (X-Plane not running?) - times without wind.';
  if (route.waypoints.length === 0) {
    list.innerHTML = '<div class="empty">No waypoints yet.</div>';
    document.getElementById('route-total').textContent = '';
    return;
  }
  let totalNm = 0;
  let totalMin = 0;
  list.innerHTML = route.waypoints
    .map((w, i) => {
      let leg = '';
      if (i > 0) {
        const l = legInfo(route.waypoints[i - 1], w, route.tasKt, vors, wind);
        totalNm += l.distNm;
        totalMin += l.minutes;
        const heading = wind ? ` · MH ${pad3(l.magHeading)}° · GS ${Math.round(l.groundspeed)} kt` : '';
        leg = `<div class="wp-leg">MC ${pad3(l.magCourse)}°${heading} · ${l.distNm.toFixed(1)} NM · ${formatMinutes(l.minutes)}</div>`;
      }
      const title = w.kind === 'USR' ? `WPT${i + 1}` : w.ident;
      const sub = w.name && w.name !== w.ident ? ` <span class="wp-leg">${escapeHtml(w.name)}</span>` : '';
      const state = route.waypoints.length >= 2 ? (i < active ? 'passed' : i === active ? 'active' : '') : '';
      return `<div class="wp ${state}" data-i="${i}">
        <div class="wp-main"><span class="wp-ident">${i === active && route.waypoints.length >= 2 ? '▶ ' : ''}${escapeHtml(title)}</span> <span class="wp-leg">${w.kind}</span>${sub}${leg}</div>
        ${i > 0 ? '<button data-act="go" title="Fly to this waypoint next">▶</button>' : ''}
        <button data-act="up" title="Move up">↑</button>
        <button data-act="down" title="Move down">↓</button>
        <button data-act="del" title="Remove">✕</button>
      </div>`;
    })
    .join('');
  document.getElementById('route-total').textContent = route.waypoints.length >= 2
    ? `Total ${totalNm.toFixed(1)} NM · ${formatMinutes(totalMin)} at ${route.tasKt} kt TAS${wind ? ' with wind' : ' (no wind)'}`
    : '';
}

function addWaypoint(w) {
  route.waypoints.push(w);
  renderRoute();
}

// A click in route mode snaps to whatever airport/VRP/navaid is under the
// cursor, else becomes a lat/lon user waypoint.
function waypointAt(e) {
  const pad = 10;
  const box = [[e.point.x - pad, e.point.y - pad], [e.point.x + pad, e.point.y + pad]];
  const layers = ['nav-points', 'airports'].filter((id) => map.getLayer(id));
  const hit = layers.length ? map.queryRenderedFeatures(box, { layers })[0] : null;
  if (hit) {
    const [lon, lat] = hit.geometry.coordinates;
    const p = hit.properties;
    if (hit.layer.id === 'airports') return { kind: 'APT', ident: p.ident, name: p.name, lat, lon };
    return { kind: p.kind, ident: p.ident, name: p.name, lat, lon };
  }
  return { kind: 'USR', ident: '', name: '', lat: e.lngLat.lat, lon: e.lngLat.lng };
}

// --- Measure tool: two clicks -> magnetic course and distance ---------------

function renderMeasure() {
  if (!map || !map.getSource('measure')) return;
  const features = measurePoints.map((p) => ({ type: 'Feature', geometry: { type: 'Point', coordinates: [p.lon, p.lat] }, properties: { label: '+' } }));
  if (measurePoints.length === 2) {
    const [a, b] = measurePoints;
    const self = lastData && lastData.selfPos;
    const variation = self && self.magVar ? self.magVar : variationAt(a, vors);
    const mc = (trueCourse(a, b) - variation + 360) % 360;
    const dist = distanceNm(a, b);
    const gs = self && self.groundspeedKt > 30 ? self.groundspeedKt : route.tasKt;
    features.length = 0;
    features.push({ type: 'Feature', geometry: { type: 'LineString', coordinates: [[a.lon, a.lat], [b.lon, b.lat]] }, properties: {} });
    features.push({
      type: 'Feature',
      geometry: { type: 'Point', coordinates: [(a.lon + b.lon) / 2, (a.lat + b.lat) / 2] },
      properties: { label: `MC ${pad3(mc)}° · ${dist.toFixed(1)} NM · ${formatMinutes((dist / gs) * 60)} at ${Math.round(gs)} kt` },
    });
  }
  map.getSource('measure').setData({ type: 'FeatureCollection', features });
}

function setMeasureMode(on) {
  measureMode = on;
  measurePoints = [];
  document.getElementById('map-measure-btn').classList.toggle('active', on);
  if (on && routeMode) setRouteMode(false);
  if (map) map.getCanvas().style.cursor = on ? 'crosshair' : '';
  renderMeasure();
}

function setRouteMode(on) {
  if (on && measureMode) setMeasureMode(false);
  routeMode = on;
  document.getElementById('route-panel').classList.toggle('open', on);
  document.getElementById('map-route-btn').classList.toggle('active', on);
  if (map) map.getCanvas().style.cursor = on ? 'crosshair' : '';
  if (on) {
    setFollow(false); // otherwise the map keeps jumping back to the aircraft while planning
    renderRoute();
  }
}

function routeForGo() {
  readRouteFields();
  return {
    name: route.name,
    cruiseFt: route.cruiseFt,
    tasKt: route.tasKt,
    waypoints: route.waypoints.map((w) => ({ kind: w.kind, ident: w.ident || '', name: w.name || '', lat: w.lat, lon: w.lon })),
  };
}

function setRouteNote(text) {
  document.getElementById('route-note').textContent = text;
}

// Someone's shared route (or our own, fromSenderId 0) - drawn dashed, with
// a note above the map offering to take it over.
function renderSharedRoute() {
  const shared = lastData && lastData.sharedRoute;
  const note = document.getElementById('shared-route-note');
  const shareBtn = document.getElementById('route-share');
  const own = shared && shared.fromSenderId === 0;
  shareBtn.textContent = own ? 'Stop sharing' : 'Share with group';
  if (map && map.getSource('shared-route')) {
    map.getSource('shared-route').setData(routeGeoJson(shared && !own ? shared.route.waypoints : []));
  }
  if (!shared) {
    note.style.display = 'none';
    note.dataset.key = '';
    return;
  }
  const key = JSON.stringify(shared);
  if (note.dataset.key === key) return;
  note.dataset.key = key;
  note.style.display = '';
  const r = shared.route;
  const name = r.name ? `"${escapeHtml(r.name)}"` : `${escapeHtml(r.waypoints[0].ident)} → ${escapeHtml(r.waypoints[r.waypoints.length - 1].ident)}`;
  if (own) {
    note.innerHTML = `You're sharing ${name} with the group.`;
    return;
  }
  note.innerHTML = `${escapeHtml(callsignOf(shared.fromSenderId))} shared a route: ${name} (${r.waypoints.length} waypoints, dashed orange). ` +
    '<button id="shared-route-take" class="secondary" type="button" style="flex: none; padding: 3px 8px; margin-left: 6px;">Use as my route</button>';
  document.getElementById('shared-route-take').addEventListener('click', () => {
    route = { name: r.name, cruiseFt: r.cruiseFt || route.cruiseFt, tasKt: r.tasKt || route.tasKt, waypoints: r.waypoints.map((w) => ({ ...w })) };
    document.getElementById('route-name').value = route.name;
    document.getElementById('route-cruise').value = route.cruiseFt;
    document.getElementById('route-tas').value = route.tasKt;
    setRouteMode(true);
  });
}

// --- Airport popup -----------------------------------------------------------

async function showAirportPopup(feature) {
  const p = feature.properties;
  const popup = new maplibregl.Popup({ closeButton: true, maxWidth: '280px' })
    .setLngLat(feature.geometry.coordinates)
    .setHTML(`<div class="airport-popup"><h4>${escapeHtml(p.ident)} · ${escapeHtml(p.name)}</h4><div class="muted">loading…</div></div>`)
    .addTo(map);
  try {
    const info = await GetAirportInfo(p.ident);
    const d = info.detail || {};
    const freqs = (d.frequencies || []).map((f) => `<tr><td>${escapeHtml(f.type)}</td><td>${escapeHtml(f.mhz)}</td><td class="muted">${escapeHtml(f.name)}</td></tr>`).join('');
    const vrps = (info.vrps || []).map((v) => escapeHtml(v.name)).join(', ');
    popup.setHTML(`<div class="airport-popup">
      <h4>${escapeHtml(p.ident)} · ${escapeHtml(p.name)}</h4>
      <div>Elevation ${d.elevationFt ?? '?'} ft${(d.runways || []).length ? ` · RWY ${escapeHtml(d.runways.join(', '))}` : ''}</div>
      ${freqs ? `<table>${freqs}</table>` : '<div class="muted">No frequencies listed</div>'}
      ${vrps ? `<div style="margin-top: 4px;"><span class="muted">VFR reporting points:</span> ${vrps}</div>` : ''}
    </div>`);
  } catch (e) {
    popup.setHTML(`<div class="airport-popup"><h4>${escapeHtml(p.ident)}</h4><div class="muted">${escapeHtml(String(e))}</div></div>`);
  }
}

// --- Loading + map setup -----------------------------------------------------

async function loadAirports() {
  try {
    airportData = await GetAirports();
  } catch (e) {
    airportData = null;
    setMapMessage(`Airports not shown: ${e}`);
    return;
  }
  if (map && styleReady) addAirportLayers();
}

async function loadNav() {
  try {
    navData = await GetNavData();
  } catch (e) {
    navData = null;
    return; // older X-Plane without the files - the rest still works
  }
  vors = navData.points.filter((p) => p.kind === 'VOR');
  if (map && styleReady) {
    addNavLayers();
    applyLayerVisibility();
  }
  renderRoute(); // magnetic courses now that VORs (variation) are known
}

function createMap() {
  const styleName = kStyles[storageGet(kStyleStorageKey)] ? storageGet(kStyleStorageKey) : 'dark';
  document.getElementById('map-style').value = styleName;
  followSelf = storageGet(kFollowStorageKey) !== '0';
  document.getElementById('map-follow').checked = followSelf;
  const prefs = layerPrefs();
  document.getElementById('layer-vfr').checked = prefs.vfr;
  document.getElementById('layer-airspace').checked = prefs.airspace;
  document.getElementById('layer-tracks').checked = prefs.tracks;
  loadRoute();
  try {
    map = new maplibregl.Map({
      container: 'map',
      style: kStyles[styleName],
      center: [8.57, 50.03],
      zoom: 4,
      attributionControl: { compact: false },
    });
  } catch (e) {
    // Typically: no WebGL in this system's webview.
    mapFailed = true;
    setMapMessage(`The map can't be shown here (${e.message || e}). Everything else works as usual.`);
    return;
  }
  map.addControl(new maplibregl.NavigationControl({ visualizePitch: false }), 'top-right');
  map.addControl(new maplibregl.ScaleControl({ unit: 'nautical' }), 'bottom-left');
  map.on('style.load', () => {
    styleReady = true;
    addOverlays();
    applyData();
  });
  map.on('dragstart', () => setFollow(false));
  map.on('moveend', refreshAirspaces);
  map.on('click', (e) => {
    if (measureMode) {
      if (measurePoints.length >= 2) measurePoints = [];
      measurePoints.push({ lat: e.lngLat.lat, lon: e.lngLat.lng });
      renderMeasure();
      return;
    }
    if (routeMode) {
      addWaypoint(waypointAt(e));
      return;
    }
    const hit = map.getLayer('airports') ? map.queryRenderedFeatures(
      [[e.point.x - 6, e.point.y - 6], [e.point.x + 6, e.point.y + 6]], { layers: ['airports'] })[0] : null;
    if (hit) showAirportPopup(hit);
  });
  map.on('mousemove', (e) => {
    if (routeMode || measureMode || !map.getLayer('airports')) return;
    const hit = map.queryRenderedFeatures([[e.point.x - 6, e.point.y - 6], [e.point.x + 6, e.point.y + 6]], { layers: ['airports'] });
    map.getCanvas().style.cursor = hit.length ? 'pointer' : '';
  });
  map.on('error', (e) => {
    // Tile/network hiccups land here too; only surface the first one.
    if (e && e.error && !document.getElementById('map-message').textContent) {
      setMapMessage(`Map data couldn't be loaded (${e.error.message || e.error}) - offline?`);
    }
  });
  loadAirports();
  loadNav();
}

// Called by main.js every time the Map page is shown.
export function showMap() {
  if (mapFailed) return;
  if (!map) {
    createMap();
  } else {
    map.resize();
  }
}

// Called by main.js on every status event.
export function updateMap(data) {
  lastData = data;
  applyData();
}

document.getElementById('map-style').addEventListener('change', (e) => {
  storageSet(kStyleStorageKey, e.target.value);
  if (map) {
    styleReady = false;
    map.setStyle(kStyles[e.target.value]);
  }
});
document.getElementById('map-follow').addEventListener('change', (e) => setFollow(e.target.checked));
document.getElementById('map-fit').addEventListener('click', fitAll);
for (const [id, key] of [['layer-vfr', 'vfr'], ['layer-airspace', 'airspace'], ['layer-tracks', 'tracks']]) {
  document.getElementById(id).addEventListener('change', (e) => {
    storageSet(kLayerStorageKey, JSON.stringify({ ...layerPrefs(), [key]: e.target.checked }));
    applyLayerVisibility();
    refreshAirspaces();
  });
}
document.getElementById('map-route-btn').addEventListener('click', () => setRouteMode(!routeMode));
document.getElementById('map-measure-btn').addEventListener('click', () => setMeasureMode(!measureMode));
document.getElementById('tracks-clear').addEventListener('click', () => {
  clearTracks();
  if (map && map.getSource('tracks')) map.getSource('tracks').setData(tracksGeoJson());
});
document.getElementById('route-import').addEventListener('click', async () => {
  try {
    const imported = await ImportFms();
    if (!imported) return; // dialog cancelled
    route = { ...route, ...imported, tasKt: route.tasKt };
    document.getElementById('route-name').value = route.name;
    document.getElementById('route-cruise').value = route.cruiseFt;
    renderRoute();
    setRouteNote(`Imported ${route.waypoints.length} waypoints.`);
    if (map && route.waypoints.length) {
      const pts = route.waypoints.map((w) => [w.lon, w.lat]);
      map.fitBounds(pts.reduce((b, p) => b.extend(p), new maplibregl.LngLatBounds(pts[0], pts[0])), { padding: 60, maxZoom: 11 });
    }
  } catch (e) {
    setRouteNote(`Import failed: ${e}`);
  }
});
for (const id of ['route-name', 'route-cruise', 'route-tas']) {
  document.getElementById(id).addEventListener('change', renderRoute);
}
document.getElementById('route-list').addEventListener('click', (e) => {
  const btn = e.target.closest('button[data-act]');
  if (!btn) return;
  const i = Number(btn.closest('.wp').dataset.i);
  const w = route.waypoints;
  if (btn.dataset.act === 'go') setActiveIndex(i, route);
  if (btn.dataset.act === 'del') w.splice(i, 1);
  if (btn.dataset.act === 'up' && i > 0) [w[i - 1], w[i]] = [w[i], w[i - 1]];
  if (btn.dataset.act === 'down' && i < w.length - 1) [w[i + 1], w[i]] = [w[i], w[i + 1]];
  renderRoute();
});
document.getElementById('route-undo').addEventListener('click', () => {
  route.waypoints.pop();
  renderRoute();
});
document.getElementById('route-clear').addEventListener('click', () => {
  if (route.waypoints.length && !confirm('Remove all waypoints?')) return;
  route.waypoints = [];
  renderRoute();
});
document.getElementById('route-export').addEventListener('click', async () => {
  if (route.waypoints.length < 2) {
    setRouteNote('Add at least two waypoints first.');
    return;
  }
  try {
    const file = await ExportFms(routeForGo());
    setRouteNote(`Saved as "Output/FMS plans/${file}" - load it in the G1000/GNS/FMS flight plan catalogue.`);
  } catch (e) {
    setRouteNote(`Export failed: ${e}`);
  }
});
document.getElementById('route-share').addEventListener('click', async () => {
  const own = lastData && lastData.sharedRoute && lastData.sharedRoute.fromSenderId === 0;
  try {
    if (own) {
      await ClearSharedRoute();
      setRouteNote('Stopped sharing.');
    } else {
      await ShareRoute(routeForGo());
      setRouteNote('Shared - everyone in your Multiplayer session sees it on their map.');
    }
  } catch (e) {
    setRouteNote(String(e));
  }
});
window.addEventListener('resize', () => map && map.resize());
