// Flown tracks for the map: collected from every status event (1 Hz) in
// main.js, whether or not the map page is open, so the trail is there when
// you switch to it. Kept in memory only (this app session).
import { distanceNm } from './route-math.js';

const kMinStepNm = 0.02; // ~40 m - don't store points while parked
const kMaxPoints = 4000; // per aircraft, ~1 h at 1 Hz; oldest dropped first
const tracks = new Map(); // id (0 = own aircraft) -> [{lat, lon}]

function add(id, lat, lon) {
  let t = tracks.get(id);
  if (!t) {
    t = [];
    tracks.set(id, t);
  }
  const last = t[t.length - 1];
  if (last && distanceNm(last, { lat, lon }) < kMinStepNm) return;
  t.push({ lat, lon });
  if (t.length > kMaxPoints) t.splice(0, t.length - kMaxPoints);
}

export function recordTracks(data) {
  if (data.selfPos) add(0, data.selfPos.lat, data.selfPos.lon);
  for (const p of data.peerPos || []) add(p.id, p.lat, p.lon);
}

export function clearTracks() {
  tracks.clear();
}

export function tracksGeoJson() {
  const features = [];
  for (const [id, points] of tracks) {
    if (points.length < 2) continue;
    features.push({
      type: 'Feature',
      geometry: { type: 'LineString', coordinates: points.map((p) => [p.lon, p.lat]) },
      properties: { self: id === 0 },
    });
  }
  return { type: 'FeatureCollection', features };
}
