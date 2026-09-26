// Leg calculations for the map's route planner: great-circle distance,
// initial true course, magnetic course (variation from the nearest VOR -
// X-Plane's nav data carries each VOR's declination) and flight time at a
// given true airspeed (no wind).

const kEarthRadiusNm = 3440.065;
const rad = (d) => (d * Math.PI) / 180;
const deg = (r) => (r * 180) / Math.PI;

export function distanceNm(a, b) {
  const dLat = rad(b.lat - a.lat);
  const dLon = rad(b.lon - a.lon);
  const h = Math.sin(dLat / 2) ** 2 + Math.cos(rad(a.lat)) * Math.cos(rad(b.lat)) * Math.sin(dLon / 2) ** 2;
  return 2 * kEarthRadiusNm * Math.asin(Math.min(1, Math.sqrt(h)));
}

export function trueCourse(a, b) {
  const y = Math.sin(rad(b.lon - a.lon)) * Math.cos(rad(b.lat));
  const x = Math.cos(rad(a.lat)) * Math.sin(rad(b.lat)) - Math.sin(rad(a.lat)) * Math.cos(rad(b.lat)) * Math.cos(rad(b.lon - a.lon));
  return (deg(Math.atan2(y, x)) + 360) % 360;
}

// Declination (east positive) of the VOR nearest to `p`, 0 if none is
// within 300 NM.
export function variationAt(p, vors) {
  let best = null;
  let bestDist = 300;
  for (const v of vors) {
    if (Math.abs(v.lat - p.lat) > 5) continue;
    const d = distanceNm(p, v);
    if (d < bestDist) {
      bestDist = d;
      best = v;
    }
  }
  return best ? best.magVar || 0 : 0;
}

export function legInfo(from, to, tasKt, vors) {
  const distNm = distanceNm(from, to);
  const tc = trueCourse(from, to);
  const mid = { lat: (from.lat + to.lat) / 2, lon: (from.lon + to.lon) / 2 };
  const magCourse = (tc - variationAt(mid, vors) + 360) % 360;
  return { distNm, trueCourse: tc, magCourse, minutes: (distNm / Math.max(tasKt, 1)) * 60 };
}
