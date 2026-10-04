// Weather briefing panel on the map page (companion/weather.go): warnings,
// runway wind, a vertical profile of the route against cloud bases and
// the freezing level, winds per leg and the METARs/TAFs near the route.

export const kCategoryColors = { VFR: '#3ccf6e', MVFR: '#4da3ff', IFR: '#ff4d4d', LIFR: '#d36bff' };

function esc(text) {
  return String(text ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
}

function pad3(deg) {
  return String(Math.round(deg) % 360).padStart(3, '0');
}

// Leg altitude: the waypoint's own (auto-route) altitude or the cruise.
export function legAltFt(route, i) {
  const w = route.waypoints[i];
  return (w && w.altFt) || route.cruiseFt || 3500;
}

// Wind for leg i from the briefing, in route-math's {fromDeg, speedKt}.
export function briefingLegWind(briefing, i) {
  const w = briefing && briefing.legWinds && briefing.legWinds[i];
  return w ? { fromDeg: w.dirDeg, speedKt: w.speedKt } : null;
}

// Cumulative distance at each waypoint, for the profile's x axis.
function waypointDistances(route, distanceNm) {
  const out = [0];
  for (let i = 1; i < route.waypoints.length; i++) out.push(out[i - 1] + distanceNm(route.waypoints[i - 1], route.waypoints[i]));
  return out;
}

function profileSvg(briefing, route, distanceNm) {
  const W = 340;
  const H = 150;
  const padL = 34;
  const padB = 16;
  const dists = waypointDistances(route, distanceNm);
  const total = dists[dists.length - 1] || 1;
  let top = 0;
  for (let i = 1; i < route.waypoints.length; i++) top = Math.max(top, legAltFt(route, i));
  for (const s of briefing.stations) if (s.ceilingFt) top = Math.max(top, Math.min(s.elevFt + s.ceilingFt, top + 3000));
  for (const [, ft] of briefing.terrain || []) top = Math.max(top, ft);
  top = Math.ceil((top + 1500) / 1000) * 1000;
  const x = (nm) => padL + (nm / total) * (W - padL - 4);
  const y = (ft) => H - padB - (Math.max(0, ft) / top) * (H - padB - 6);
  const parts = [];
  // Altitude grid.
  const step = top > 8000 ? 2000 : 1000;
  for (let ft = 0; ft <= top; ft += step) {
    parts.push(`<line x1="${padL}" x2="${W - 4}" y1="${y(ft)}" y2="${y(ft)}" class="grid"/>`);
    parts.push(`<text x="${padL - 4}" y="${y(ft) + 3}" class="axis" text-anchor="end">${ft}</text>`);
  }
  // Freezing level.
  if (briefing.freezingLevelFt && briefing.freezingLevelFt < top) {
    const fy = y(briefing.freezingLevelFt);
    parts.push(`<line x1="${padL}" x2="${W - 4}" y1="${fy}" y2="${fy}" class="freeze"/>`);
    parts.push(`<text x="${W - 6}" y="${fy - 3}" class="freeze-label" text-anchor="end">0 °C ${briefing.freezingLevelFt} ft</text>`);
  }
  // Terrain: the highest ground within a mile of the route.
  const terrain = briefing.terrain || [];
  if (terrain.length > 1) {
    const scale = total / (terrain[terrain.length - 1][0] || total);
    const pts = terrain.map(([nm, ft]) => `${x(nm * scale).toFixed(1)},${y(ft).toFixed(1)}`);
    parts.push(`<polygon points="${x(0)},${y(0)} ${pts.join(' ')} ${x(total)},${y(0)}" class="terrain"><title>Highest ground within 1 NM</title></polygon>`);
  }
  // Cloud bases (BKN/OVC) at each station, as a cloud bar from the base up.
  for (const s of briefing.stations) {
    const sx = x(Math.min(total, s.alongNm));
    const color = kCategoryColors[s.category] || '#8b93a1';
    if (s.ceilingFt) {
      const base = s.elevFt + s.ceilingFt;
      if (base < top) parts.push(`<rect x="${sx - 9}" y="${y(top)}" width="18" height="${y(base) - y(top)}" class="cloud"><title>${esc(s.ident)}: cloud base ${base} ft MSL</title></rect>`);
    }
    parts.push(`<circle cx="${sx}" cy="${y(s.elevFt)}" r="3.5" fill="${color}"><title>${esc(s.ident)} ${esc(s.category)}</title></circle>`);
    parts.push(`<text x="${sx}" y="${H - 3}" class="axis" text-anchor="middle">${esc(s.ident)}</text>`);
  }
  // Planned altitude per leg.
  let path = '';
  for (let i = 1; i < route.waypoints.length; i++) {
    const alt = legAltFt(route, i);
    path += `${i === 1 ? 'M' : 'L'}${x(dists[i - 1]).toFixed(1)},${y(alt).toFixed(1)} L${x(dists[i]).toFixed(1)},${y(alt).toFixed(1)} `;
  }
  parts.push(`<path d="${path}" class="route-alt"/>`);
  return `<svg viewBox="0 0 ${W} ${H}" class="wx-profile" role="img" aria-label="Route altitude against cloud bases">${parts.join('')}</svg>`;
}

function waypointTitle(w, i) {
  if (w.kind === 'USR') return w.name ? w.name.split(' · ')[0] : `WPT${i + 1}`;
  return w.ident;
}

// HTML for the panel body.
export function renderBriefing(briefing, route, distanceNm) {
  const out = [];
  out.push(`<div class="wx-source">${esc(briefing.source)} · ${esc(briefing.fetchedAt)} · real-world weather${briefing.terrainSource ? ` · terrain: ${esc(briefing.terrainSource)}` : ''}</div>`);
  if (briefing.warnings.length) {
    out.push(`<ul class="wx-warnings">${briefing.warnings.map((w) => `<li>${esc(w)}</li>`).join('')}</ul>`);
  } else {
    out.push('<div class="wx-ok">No weather warnings for the planned altitudes.</div>');
  }
  if (briefing.runwayWinds.length) {
    out.push('<h4>Runway wind</h4><div class="wx-rwy">');
    for (const r of briefing.runwayWinds) {
      const head = r.headKt >= 0 ? `${r.headKt} kt head` : `${-r.headKt} kt TAIL`;
      out.push(`<div><b>${esc(r.airport)} RWY ${esc(r.runway)}</b>: ${head}, ${r.crossKt} kt cross${r.gustKt ? `, gusts ${r.gustKt}` : ''} <span class="muted">(${esc(r.windText)})</span></div>`);
    }
    out.push('</div>');
  }
  out.push('<h4>Profile</h4>');
  out.push(profileSvg(briefing, route, distanceNm));
  out.push('<div class="muted wx-legend">Yellow: planned altitude · brown: highest ground within 1 NM · grey: cloud base (BKN/OVC) · dots: stations by category</div>');
  if (briefing.legWinds.some((w) => w)) {
    out.push('<h4>Wind per leg</h4><table class="wx-legs">');
    route.waypoints.forEach((w, i) => {
      const lw = briefing.legWinds[i];
      if (i === 0 || !lw) return;
      out.push(`<tr><td>→ ${esc(waypointTitle(w, i))}</td><td>${lw.altFt} ft</td><td>${pad3(lw.dirDeg)}°/${Math.round(lw.speedKt)} kt</td></tr>`);
    });
    out.push('</table>');
  }
  out.push(`<h4>Stations (${briefing.stations.length})</h4>`);
  for (const s of briefing.stations) {
    const color = kCategoryColors[s.category] || '#8b93a1';
    out.push(`<div class="wx-station">
      <div><span class="wx-cat" style="background:${color}">${esc(s.category)}</span> <b>${esc(s.ident)}</b> <span class="muted">${esc(s.name)} · ${Math.round(s.alongNm)} NM along, ${Math.round(s.offNm)} NM off</span></div>
      <div class="wx-raw">${esc(s.metar)}</div>
      ${s.tafWarn ? `<div class="wx-tafwarn">Next 4 h: ${esc(s.tafWarn)}</div>` : ''}
      ${s.taf ? `<details><summary>TAF</summary><div class="wx-raw">${esc(s.taf)}</div></details>` : ''}
    </div>`);
  }
  return out.join('');
}

// GeoJSON of the stations for the map layer.
export function stationsGeoJson(briefing) {
  return {
    type: 'FeatureCollection',
    features: (briefing ? briefing.stations : []).map((s) => ({
      type: 'Feature',
      geometry: { type: 'Point', coordinates: [s.lon, s.lat] },
      properties: { ident: s.ident, color: kCategoryColors[s.category] || '#8b93a1', label: `${s.ident} ${s.category}` },
    })),
  };
}
