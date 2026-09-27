// In-flight route progress for the global route banner and the map: which
// waypoint is next, and automatic sequencing - the next waypoint is taken
// as reached within kReachedNm or once the aircraft is abeam/past it along
// the leg. The route itself lives in localStorage (written by map.js's
// planner), so this works on every page even if the map was never opened.
import { alongTrackNm, distanceNm, legInfo, trueCourse, variationAt } from './route-math.js';
import { AddFlightEvent } from '../wailsjs/go/main/App';

const kRouteKey = 'xpmulticrew.route';
const kProgressKey = 'xpmulticrew.routeProgress';
const kReachedNm = 1.0;

function read(key) {
  try {
    return JSON.parse(localStorage.getItem(key) || 'null');
  } catch (e) {
    return null;
  }
}
function write(key, value) {
  try {
    localStorage.setItem(key, JSON.stringify(value));
  } catch (e) {
    // per-viewer convenience only
  }
}

export function currentRoute() {
  const r = read(kRouteKey);
  return r && Array.isArray(r.waypoints) && r.waypoints.length >= 2 ? r : null;
}

function routeKey(r) {
  return r.waypoints.map((w) => `${w.lat.toFixed(5)},${w.lon.toFixed(5)}`).join(';');
}

// Index of the waypoint we're flying to (1..n-1), reset when the route changes.
export function activeIndex(route = currentRoute()) {
  if (!route) return 1;
  const p = read(kProgressKey);
  return p && p.key === routeKey(route) ? Math.min(Math.max(p.active, 1), route.waypoints.length - 1) : 1;
}

export function setActiveIndex(i, route = currentRoute()) {
  if (!route) return;
  write(kProgressKey, { key: routeKey(route), active: Math.min(Math.max(i, 1), route.waypoints.length - 1) });
}

// Advances the active waypoint for position `self`; returns the progress
// to display, or null without a route/position.
export function updateProgress(self, vors = []) {
  const route = currentRoute();
  if (!route || !self) return null;
  let active = activeIndex(route);
  const w = route.waypoints;
  for (let guard = 0; guard < w.length && active < w.length - 1; guard++) {
    const from = w[active - 1];
    const to = w[active];
    const reached = distanceNm(self, to) < kReachedNm || alongTrackNm(from, to, self) >= distanceNm(from, to);
    if (!reached) break;
    // Marked on the Debrief timeline (companion/flights.go).
    AddFlightEvent('waypoint', to.ident || to.name || `WPT ${active}`).catch(() => {});
    active++;
  }
  setActiveIndex(active, route);
  const to = w[active];
  const distNm = distanceNm(self, to);
  const gs = self.groundspeedKt > 30 ? self.groundspeedKt : route.tasKt || 100;
  // The plugin reports the local variation at the aircraft (SELF_POS); the
  // nearest-VOR estimate is only a fallback for older plugins.
  const variation = self.magVar !== undefined && self.magVar !== 0 ? self.magVar : variationAt(self, vors);
  const magCourse = (trueCourse(self, to) - variation + 360) % 360;
  const remainingNm = distNm + w.slice(active + 1).reduce((sum, p, i) => sum + distanceNm(w[active + i], p), 0);
  return {
    route,
    active,
    next: to,
    magCourse,
    distNm,
    etaMin: (distNm / gs) * 60,
    remainingNm,
    remainingMin: (remainingNm / gs) * 60,
    arrived: active === w.length - 1 && distNm < kReachedNm,
  };
}

export { legInfo };
