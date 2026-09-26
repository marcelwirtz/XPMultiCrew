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

// Wind at `altFt`, linearly interpolated between X-Plane's wind layers
// (plugin WIND line). Direction is interpolated as a vector so 350°/010°
// average to 000°, not 180°. null without layers.
export function windAt(layers, altFt) {
  if (!layers || layers.length === 0) return null;
  const sorted = [...layers].sort((a, b) => a.altFt - b.altFt);
  let lo = sorted[0];
  let hi = sorted[sorted.length - 1];
  for (let i = 0; i < sorted.length - 1; i++) {
    if (altFt >= sorted[i].altFt && altFt <= sorted[i + 1].altFt) {
      lo = sorted[i];
      hi = sorted[i + 1];
      break;
    }
  }
  if (altFt <= sorted[0].altFt) hi = lo;
  if (altFt >= sorted[sorted.length - 1].altFt) lo = hi;
  const t = hi.altFt === lo.altFt ? 0 : (altFt - lo.altFt) / (hi.altFt - lo.altFt);
  const vec = (w) => [w.speedKt * Math.sin(rad(w.fromDeg)), w.speedKt * Math.cos(rad(w.fromDeg))];
  const [x1, y1] = vec(lo);
  const [x2, y2] = vec(hi);
  const x = x1 + (x2 - x1) * t;
  const y = y1 + (y2 - y1) * t;
  return { fromDeg: (deg(Math.atan2(x, y)) + 360) % 360, speedKt: Math.hypot(x, y) };
}

// Wind triangle: heading to fly and ground speed for true course `tc` at
// `tasKt` with wind from `wind.fromDeg` at `wind.speedKt`.
export function windCorrection(tc, tasKt, wind) {
  if (!wind || wind.speedKt < 0.5) return { wca: 0, trueHeading: tc, groundspeed: tasKt };
  const angle = rad(wind.fromDeg - tc);
  const ratio = Math.max(-1, Math.min(1, (wind.speedKt / Math.max(tasKt, 1)) * Math.sin(angle)));
  const wca = deg(Math.asin(ratio));
  const groundspeed = tasKt * Math.cos(rad(wca)) - wind.speedKt * Math.cos(angle);
  return { wca, trueHeading: (tc + wca + 360) % 360, groundspeed: Math.max(groundspeed, 1) };
}

export function legInfo(from, to, tasKt, vors, wind = null) {
  const distNm = distanceNm(from, to);
  const tc = trueCourse(from, to);
  const mid = { lat: (from.lat + to.lat) / 2, lon: (from.lon + to.lon) / 2 };
  const variation = variationAt(mid, vors);
  const magCourse = (tc - variation + 360) % 360;
  const wc = windCorrection(tc, tasKt, wind);
  return {
    distNm,
    trueCourse: tc,
    magCourse,
    wca: wc.wca,
    magHeading: (wc.trueHeading - variation + 360) % 360,
    groundspeed: wc.groundspeed,
    minutes: (distNm / wc.groundspeed) * 60,
  };
}

// Signed along-track distance (NM) of `p` on the leg a->b, measured from a.
export function alongTrackNm(a, b, p) {
  const d13 = distanceNm(a, p) / kEarthRadiusNm;
  const t13 = rad(trueCourse(a, p));
  const t12 = rad(trueCourse(a, b));
  const xt = Math.asin(Math.sin(d13) * Math.sin(t13 - t12));
  const at = Math.acos(Math.max(-1, Math.min(1, Math.cos(d13) / Math.cos(xt))));
  return at * kEarthRadiusNm * Math.sign(Math.cos(t13 - t12));
}
