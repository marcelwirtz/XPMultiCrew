// Map page: your own aircraft and the Formation peers on an OpenFreeMap
// base map (MapLibre GL), plus airports/runways read from the user's own
// X-Plane installation (companion/airports.go).
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

maplibregl.setWorkerUrl(workerUrl);

import { GetAirports } from '../wailsjs/go/main/App';

const kStyles = {
  dark: 'https://tiles.openfreemap.org/styles/dark',
  liberty: 'https://tiles.openfreemap.org/styles/liberty',
};
const kStyleStorageKey = 'xpmulticrew.mapStyle';
const kFollowStorageKey = 'xpmulticrew.mapFollow';

let map = null;
let mapFailed = false;
let styleReady = false; // own flag: map.isStyleLoaded() stays false while tiles are still loading
let lastData = null; // latest status event, applied once the map/style is ready
let airportData = null; // cached across style switches
let followSelf = true;
let firstFix = true;

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

function setMapMessage(text) {
  const el = document.getElementById('map-message');
  el.textContent = text || '';
  el.style.display = text ? '' : 'none';
}

// A simple top-down airplane silhouette, drawn once per color. Registered
// as an SDF-free image so each aircraft can be rotated by its heading.
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
  ctx.moveTo(0, -20); // nose
  ctx.lineTo(3, -12);
  ctx.lineTo(3, -4);
  ctx.lineTo(20, 4); // right wing
  ctx.lineTo(20, 8);
  ctx.lineTo(3, 4);
  ctx.lineTo(3, 13);
  ctx.lineTo(8, 17); // right stabilizer
  ctx.lineTo(8, 20);
  ctx.lineTo(0, 18);
  ctx.lineTo(-8, 20);
  ctx.lineTo(-8, 17);
  ctx.lineTo(-3, 13);
  ctx.lineTo(-3, 4);
  ctx.lineTo(-20, 8);
  ctx.lineTo(-20, 4);
  ctx.lineTo(-3, -4);
  ctx.lineTo(-3, -12);
  ctx.closePath();
  ctx.fill();
  ctx.stroke();
  return ctx.getImageData(0, 0, size, size);
}

function formatAltitude(ft) {
  return ft >= 18000 ? `FL${Math.round(ft / 100)}` : `${Math.round(ft).toLocaleString()} ft`;
}

function aircraftGeoJson(data) {
  const features = [];
  const callsigns = new Map((data.peers || []).map((p) => [p.id, p.callsign || p.icao || `Peer ${p.id}`]));
  if (data.selfPos) {
    const s = data.selfPos;
    features.push({
      type: 'Feature',
      geometry: { type: 'Point', coordinates: [s.lon, s.lat] },
      properties: {
        self: true,
        heading: s.heading,
        label: `You\n${formatAltitude(s.altFt)} · ${Math.round(s.groundspeedKt || 0)} kt`,
      },
    });
  }
  for (const p of data.peerPos || []) {
    features.push({
      type: 'Feature',
      geometry: { type: 'Point', coordinates: [p.lon, p.lat] },
      properties: {
        self: false,
        heading: p.heading,
        label: `${callsigns.get(p.id) || `Peer ${p.id}`}\n${formatAltitude(p.altFt)}`,
      },
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

function textFont() {
  // OpenFreeMap's glyph server provides the Noto Sans family.
  return ['Noto Sans Regular'];
}

// Airports/runways from the user's X-Plane - inserted below the aircraft
// layer if that already exists, a no-op if they're already there or the
// data hasn't arrived yet.
function addAirportLayers() {
  if (!airportData || map.getSource('airports')) return;
  const before = map.getLayer('aircraft') ? 'aircraft' : undefined;

  map.addSource('runways', { type: 'geojson', data: runwaysGeoJson(airportData) });
  map.addLayer(
    {
      id: 'runways',
      type: 'line',
      source: 'runways',
      minzoom: 9,
      paint: { 'line-color': '#b8c0cc', 'line-width': ['interpolate', ['linear'], ['zoom'], 9, 1.5, 14, 8] },
    },
    before,
  );

  map.addSource('airports', { type: 'geojson', data: airportsGeoJson(airportData) });
  map.addLayer(
    {
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
    },
    before,
  );
  map.addLayer(
    {
      id: 'airport-labels',
      type: 'symbol',
      source: 'airports',
      minzoom: 8,
      layout: {
        'text-field': ['get', 'ident'],
        'text-font': textFont(),
        'text-size': 11,
        'text-offset': [0, 1.1],
        'text-anchor': 'top',
      },
      paint: { 'text-color': '#e6e8eb', 'text-halo-color': '#14171c', 'text-halo-width': 1.2 },
    },
    before,
  );
}

// (Re-)adds everything on top of the base style - needed after every
// setStyle(), which throws away custom sources, layers and images.
function addOverlays() {
  if (!map.hasImage('plane-self')) map.addImage('plane-self', makePlaneImage('#4da3ff'), { pixelRatio: 2 });
  if (!map.hasImage('plane-peer')) map.addImage('plane-peer', makePlaneImage('#ffb347'), { pixelRatio: 2 });
  addAirportLayers();

  map.addSource('aircraft', { type: 'geojson', data: aircraftGeoJson(lastData || {}) });
  map.addLayer({
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

function applyData() {
  if (!map || !lastData || !map.getSource('aircraft')) return;
  map.getSource('aircraft').setData(aircraftGeoJson(lastData));
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

async function loadAirports() {
  try {
    airportData = await GetAirports();
  } catch (e) {
    airportData = null;
    setMapMessage(`Airports not shown: ${e}`);
    return;
  }
  if (map && styleReady) {
    addAirportLayers();
  }
}

function createMap() {
  const styleName = kStyles[storageGet(kStyleStorageKey)] ? storageGet(kStyleStorageKey) : 'dark';
  document.getElementById('map-style').value = styleName;
  followSelf = storageGet(kFollowStorageKey) !== '0';
  document.getElementById('map-follow').checked = followSelf;
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
  map.on('error', (e) => {
    // Tile/network hiccups land here too; only surface the first one.
    if (e && e.error && !document.getElementById('map-message').textContent) {
      setMapMessage(`Map data couldn't be loaded (${e.error.message || e.error}) - offline?`);
    }
  });
  loadAirports();
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
window.addEventListener('resize', () => map && map.resize());
