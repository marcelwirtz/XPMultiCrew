// Airports page: everything X-Plane knows about an airport
// (companion/airport_detail.go) - runways with wind components for the
// current wind, instrument approaches, frequencies, and an airport diagram
// drawn by us from apt.dat (paved areas, runways, taxiways with names,
// parking positions). Official charts are licensed, so they are only
// linked (ChartFox, free account), never shown in the app.
import { BrowserOpenURL } from '../wailsjs/runtime/runtime';
import { GetAirportLayout, NearestAirports, SearchAirports } from '../wailsjs/go/main/App';

const $ = (id) => document.getElementById(id);

let layout = null;
let lastStatus = null;
let manualWind = null; // { from, kt } typed by the user; null = sim wind
let nearbyAt = null; // position the nearby list was fetched for
let searchSeq = 0;
// Diagram view: centre in airport metres, zoom in pixels per metre.
let view = { cx: 0, cy: 0, zoom: 0.2 };

function escapeHtml(text) {
  return String(text ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
}

function setMessage(text) {
  const el = $('airport-message');
  el.textContent = text || '';
  el.style.display = text ? '' : 'none';
}

const pad3 = (deg) => String(Math.round(((deg % 360) + 360) % 360) || 360).padStart(3, '0');

function distNm(lat1, lon1, lat2, lon2) {
  const r = Math.PI / 180;
  const a = Math.sin(((lat2 - lat1) * r) / 2) ** 2 + Math.cos(lat1 * r) * Math.cos(lat2 * r) * Math.sin(((lon2 - lon1) * r) / 2) ** 2;
  return (2 * 6371008.8 * Math.asin(Math.min(1, Math.sqrt(a)))) / 1852;
}

// --- Wind ------------------------------------------------------------------------

// The sim reports the wind layers at the aircraft - only meaningful for an
// airport nearby. The lowest layer is the surface wind.
function simWind() {
  if (!layout || !lastStatus || !lastStatus.selfPos || !(lastStatus.wind || []).length) return null;
  const self = lastStatus.selfPos;
  if (distNm(self.lat, self.lon, layout.lat, layout.lon) > 30) return null;
  const lowest = [...lastStatus.wind].sort((a, b) => a.altFt - b.altFt)[0];
  return { from: lowest.fromDeg, kt: lowest.speedKt };
}

function currentWind() {
  return manualWind || simWind();
}

function components(windFrom, kt, headingTrue) {
  const a = ((windFrom - headingTrue) * Math.PI) / 180;
  return { head: kt * Math.cos(a), cross: kt * Math.sin(a) };
}

function renderWind() {
  const w = currentWind();
  const src = $('airport-wind-source');
  if (!manualWind) {
    $('airport-wind-dir').value = w ? pad3(w.from - (layout ? layout.magVar : 0)) : '';
    $('airport-wind-kt').value = w ? Math.round(w.kt) : '';
  }
  src.textContent = manualWind
    ? 'entered by you'
    : w
      ? 'sim wind at your aircraft'
      : 'type a wind (e.g. from the ATIS) - the sim only knows the wind where you are';
  $('airport-wind-reset').style.display = manualWind && simWind() ? '' : 'none';
}

function readManualWind() {
  const dir = Number($('airport-wind-dir').value);
  const kt = Number($('airport-wind-kt').value);
  if ($('airport-wind-dir').value === '' || $('airport-wind-kt').value === '' || !isFinite(dir) || !isFinite(kt)) {
    manualWind = null;
  } else {
    // Typed like an ATIS: magnetic.
    manualWind = { from: (dir + (layout ? layout.magVar : 0) + 360) % 360, kt: Math.max(0, kt) };
  }
  renderRunways();
  renderWind();
}

// --- Info tables -------------------------------------------------------------------

function renderRunways() {
  const el = $('airport-runways');
  if (!layout) {
    el.innerHTML = '';
    return;
  }
  const wind = currentWind();
  const ends = [];
  layout.runways.forEach((r) => r.ends.forEach((e) => ends.push({ r, e })));
  let best = null;
  if (wind && wind.kt >= 3) {
    for (const x of ends) {
      const c = components(wind.from, wind.kt, x.e.headingTrue);
      x.c = c;
      if (!best || c.head > best.c.head) best = x;
    }
  } else if (wind) {
    ends.forEach((x) => (x.c = components(wind.from, wind.kt, x.e.headingTrue)));
  }
  const approachesFor = (name) => layout.approaches.filter((a) => a.runway === name);
  const rows = ends.map((x) => {
    const { r, e } = x;
    const apps = approachesFor(e.name)
      .map((a) => {
        const course = a.courseMag || Math.round(a.course - layout.magVar);
        return `<div>${escapeHtml(a.kind)} <b>${escapeHtml(a.ident)}</b>${a.freq ? ` ${escapeHtml(a.freq)}` : ''} · ${pad3(course)}°${a.glideDeg ? ` · ${a.glideDeg.toFixed(1)}°` : ''}</div>`;
      })
      .join('');
    let windCell = '<span class="muted">—</span>';
    if (x.c) {
      const head = Math.round(x.c.head);
      const cross = Math.round(Math.abs(x.c.cross));
      const side = x.c.cross >= 0 ? 'R' : 'L';
      windCell = `<span class="${head < 0 ? 'ap-tail' : ''}">${head >= 0 ? '↓' : '↑'} ${Math.abs(head)} kt ${head >= 0 ? 'head' : 'tail'}</span>` +
        `<br><span class="muted">${cross} kt from ${side}</span>`;
    }
    const isBest = best && best.e === e;
    return `<tr class="${isBest ? 'ap-best' : ''}">
      <td><b>${escapeHtml(e.name)}</b>${isBest ? '<div class="ap-best-tag">likely in use</div>' : ''}</td>
      <td class="num">${pad3(e.headingTrue - layout.magVar)}°</td>
      <td class="num">${Math.round(r.lengthM - e.displacedM)} m${e.displacedM ? `<div class="muted">thr. displaced ${Math.round(e.displacedM)} m</div>` : ''}</td>
      <td>${Math.round(r.widthM)} m · ${escapeHtml(r.surface)}</td>
      <td>${e.papiDeg ? `PAPI ${e.papiDeg.toFixed(1)}°` : '<span class="muted">—</span>'}</td>
      <td class="ap-apps">${apps || '<span class="muted">visual</span>'}</td>
      <td>${windCell}</td>
    </tr>`;
  });
  el.innerHTML = rows.length
    ? `<table class="lb-table ap-table"><tr><th>RWY</th><th class="num">MAG</th><th class="num">Landing length</th><th>Width · surface</th>
        <th>Glide</th><th>Approaches</th><th>Wind</th></tr>${rows.join('')}</table>`
    : '<div class="hint">No land runways (heliport or seaplane base).</div>';
}

function renderInfo() {
  if (!layout) {
    $('airport-head').innerHTML = '';
    $('airport-freqs').innerHTML = '';
    return;
  }
  const mv = layout.magVar;
  $('airport-head').innerHTML = `<h2 style="margin: 0;">${escapeHtml(layout.ident)} · ${escapeHtml(layout.name)}</h2>
    <div class="hint" style="margin-top: 4px;">Elevation ${layout.elevationFt} ft · variation ${Math.abs(mv).toFixed(1)}° ${mv >= 0 ? 'E' : 'W'}
      · ${layout.lat.toFixed(4)}, ${layout.lon.toFixed(4)}</div>`;
  const freqs = layout.frequencies
    .map((f) => `<tr><td>${escapeHtml(f.type || '')}</td><td class="num"><b>${escapeHtml(f.mhz)}</b></td><td class="muted">${escapeHtml(f.name)}</td></tr>`)
    .join('');
  $('airport-freqs').innerHTML = freqs ? `<table class="lb-table">${freqs}</table>` : '<div class="hint">No frequencies listed.</div>';
  renderRunways();
  renderWind();
}

// --- Diagram -----------------------------------------------------------------------
// Drawn in airport metres (x east, y north; the SVG's y is flipped). Line
// widths don't scale with the zoom, labels are re-sized on every zoom so
// they stay readable.

const kSvgNs = 'http://www.w3.org/2000/svg';

function pathFromRing(ring) {
  return ring.map(([x, y], i) => `${i ? 'L' : 'M'}${x},${-y}`).join('') + 'Z';
}

function runwayPolygon(r) {
  const [a, b] = r.ends;
  const dx = b.x - a.x;
  const dy = b.y - a.y;
  const len = Math.hypot(dx, dy) || 1;
  const nx = (-dy / len) * (r.widthM / 2);
  const ny = (dx / len) * (r.widthM / 2);
  return `M${a.x + nx},${-(a.y + ny)}L${b.x + nx},${-(b.y + ny)}L${b.x - nx},${-(b.y - ny)}L${a.x - nx},${-(a.y - ny)}Z`;
}

// One label per taxiway name, at the midpoint of its longest segment.
function taxiwayLabels() {
  const best = new Map();
  for (const t of layout.taxiways) {
    if (!t.name) continue;
    const len = Math.hypot(t.b[0] - t.a[0], t.b[1] - t.a[1]);
    const cur = best.get(t.name);
    if (!cur || len > cur.len) best.set(t.name, { len, x: (t.a[0] + t.b[0]) / 2, y: (t.a[1] + t.b[1]) / 2 });
  }
  return [...best.entries()];
}

function drawDiagram() {
  const svg = $('airport-diagram');
  if (!layout) {
    svg.innerHTML = '';
    return;
  }
  const pavement = layout.pavement.map((poly) => `<path d="${poly.map(pathFromRing).join('')}" fill-rule="evenodd"/>`).join('');
  const runways = layout.runways.map((r) => `<path d="${runwayPolygon(r)}"/>`).join('');
  const centerlines = layout.runways
    .map((r) => `<line x1="${r.ends[0].x}" y1="${-r.ends[0].y}" x2="${r.ends[1].x}" y2="${-r.ends[1].y}"/>`)
    .join('');
  const taxi = layout.taxiways.map((t) => `<line x1="${t.a[0]}" y1="${-t.a[1]}" x2="${t.b[0]}" y2="${-t.b[1]}"/>`).join('');
  const parking = layout.parking.map((p) => `<circle cx="${p.x}" cy="${-p.y}" r="1"/>`).join('');
  const windsocks = layout.windsocks.map(([x, y]) => `<circle cx="${x}" cy="${-y}" r="1"/>`).join('');
  svg.innerHTML = `<g id="ad-root">
      <g class="ad-pavement">${pavement}</g>
      <g class="ad-runway">${runways}</g>
      <g class="ad-centerline">${centerlines}</g>
      <g class="ad-taxi">${taxi}</g>
      <g class="ad-parking">${parking}</g>
      <g class="ad-windsock">${windsocks}</g>
      <g class="ad-self"></g>
      <g class="ad-labels"></g>
    </g>`;
  fitDiagram();
}

function fitDiagram() {
  if (!layout) return;
  const xs = [];
  const ys = [];
  layout.runways.forEach((r) => r.ends.forEach((e) => { xs.push(e.x); ys.push(e.y); }));
  layout.pavement.forEach((poly) => poly[0] && poly[0].forEach(([x, y]) => { xs.push(x); ys.push(y); }));
  if (!xs.length) { xs.push(-500, 500); ys.push(-500, 500); }
  const minX = Math.min(...xs), maxX = Math.max(...xs), minY = Math.min(...ys), maxY = Math.max(...ys);
  const { w, h } = diagramSize();
  // 40 px margin for the runway numbers beyond the ends.
  const zoom = Math.min((w - 80) / Math.max(maxX - minX, 100), (h - 80) / Math.max(maxY - minY, 100));
  view = { cx: (minX + maxX) / 2, cy: (minY + maxY) / 2, zoom: Math.max(0.02, zoom) };
  applyView();
}

// The SVG's size on screen (clientWidth isn't reliable for <svg>).
function diagramSize() {
  const r = $('airport-diagram').getBoundingClientRect();
  return { w: r.width || 600, h: r.height || 380 };
}

function applyView() {
  const svg = $('airport-diagram');
  if (!layout || !svg.firstChild) return;
  const { w, h } = diagramSize();
  svg.setAttribute('viewBox', `${view.cx - w / 2 / view.zoom} ${-view.cy - h / 2 / view.zoom} ${w / view.zoom} ${h / view.zoom}`);
  const px = 1 / view.zoom; // one screen pixel in metres
  svg.querySelectorAll('.ad-parking circle').forEach((c) => c.setAttribute('r', 2.5 * px));
  svg.querySelectorAll('.ad-windsock circle').forEach((c) => c.setAttribute('r', 4 * px));

  // Labels in screen-constant size: runway numbers always, taxiways from
  // a moderate zoom, parking names only close in.
  // Labels that would overlap one already placed are skipped (runway
  // numbers go first, so they always win).
  const placed = [];
  const label = (x, y, text, cls, size, force = false) => {
    const w = (text.length * size * 0.62 + 4) * px;
    const h = (size + 2) * px;
    const box = [x - w / 2, y - h / 2, x + w / 2, y + h / 2];
    if (!force && placed.some((b) => box[0] < b[2] && box[2] > b[0] && box[1] < b[3] && box[3] > b[1])) return '';
    placed.push(box);
    return `<text x="${x}" y="${-y}" class="${cls}" font-size="${size * px}" text-anchor="middle" dominant-baseline="middle">${escapeHtml(text)}</text>`;
  };
  const out = [];
  if (layout.tower) out.push(label(layout.tower[0], layout.tower[1], 'TWR', 'ad-twr-label', 10, true));
  for (const r of layout.runways) {
    for (let i = 0; i < 2; i++) {
      const e = r.ends[i];
      const o = r.ends[1 - i];
      const len = Math.hypot(o.x - e.x, o.y - e.y) || 1;
      const off = 14 * px;
      out.push(label(e.x - ((o.x - e.x) / len) * off, e.y - ((o.y - e.y) / len) * off, e.name, 'ad-rwy-label', 12, true));
    }
  }
  if (view.zoom > 0.15) taxiwayLabels().forEach(([name, p]) => out.push(label(p.x, p.y, name, 'ad-taxi-label', 10)));
  if (view.zoom > 1.6) layout.parking.forEach((p) => p.name && out.push(label(p.x, p.y + 8 * px, p.name, 'ad-park-label', 9)));
  svg.querySelector('.ad-labels').innerHTML = out.join('');
  drawSelf();
}

// The own aircraft on the diagram when it's at this airport.
function drawSelf() {
  const g = $('airport-diagram').querySelector('.ad-self');
  if (!g) return;
  const self = lastStatus && lastStatus.selfPos;
  if (!layout || !self || distNm(self.lat, self.lon, layout.lat, layout.lon) > 5) {
    g.innerHTML = '';
    return;
  }
  const r = Math.PI / 180;
  const x = (self.lon - layout.lon) * r * Math.cos(layout.lat * r) * 6371008.8;
  const y = (self.lat - layout.lat) * r * 6371008.8;
  const s = 1 / view.zoom;
  g.innerHTML = `<g transform="translate(${x},${-y}) rotate(${self.heading}) scale(${s})">
    <path d="M0,-11 L2,-6 L2,-2 L11,2 L11,4 L2,2 L2,7 L5,9 L5,11 L0,10 L-5,11 L-5,9 L-2,7 L-2,2 L-11,4 L-11,2 L-2,-2 L-2,-6 Z"
      fill="#4da3ff" stroke="#14171c" stroke-width="1.5"/></g>`;
}

function setupDiagramInput() {
  const svg = $('airport-diagram');
  let drag = null;
  svg.addEventListener('wheel', (ev) => {
    if (!layout) return;
    ev.preventDefault();
    const rect = svg.getBoundingClientRect();
    const mx = view.cx + (ev.clientX - rect.left - rect.width / 2) / view.zoom;
    const my = view.cy - (ev.clientY - rect.top - rect.height / 2) / view.zoom;
    const factor = Math.exp(-ev.deltaY * 0.0015);
    const zoom = Math.min(20, Math.max(0.02, view.zoom * factor));
    // Keep the point under the cursor where it is.
    view.cx = mx - (ev.clientX - rect.left - rect.width / 2) / zoom;
    view.cy = my + (ev.clientY - rect.top - rect.height / 2) / zoom;
    view.zoom = zoom;
    applyView();
  }, { passive: false });
  svg.addEventListener('pointerdown', (ev) => {
    drag = { x: ev.clientX, y: ev.clientY, cx: view.cx, cy: view.cy };
    svg.setPointerCapture(ev.pointerId);
  });
  svg.addEventListener('pointermove', (ev) => {
    if (!drag) return;
    view.cx = drag.cx - (ev.clientX - drag.x) / view.zoom;
    view.cy = drag.cy + (ev.clientY - drag.y) / view.zoom;
    applyView();
  });
  svg.addEventListener('pointerup', () => (drag = null));
  svg.addEventListener('dblclick', fitDiagram);
  new ResizeObserver(() => applyView()).observe(svg);
}

// --- Loading, search -------------------------------------------------------------

export async function openAirport(ident) {
  if (!ident) return;
  setMessage('');
  $('airport-search').value = ident;
  hideResults();
  try {
    layout = await GetAirportLayout(ident);
  } catch (e) {
    layout = null;
    setMessage(String(e));
  }
  manualWind = null;
  $('airport-body').style.display = layout ? '' : 'none';
  $('airport-charts').disabled = !layout;
  $('airport-on-map').disabled = !layout;
  renderInfo();
  drawDiagram();
  try {
    if (layout) localStorage.setItem('xpmulticrew.airport', layout.ident);
  } catch (e) {
    // per-viewer convenience only
  }
}

function hideResults() {
  $('airport-results').innerHTML = '';
  $('airport-results').style.display = 'none';
}

async function search(query) {
  const seq = ++searchSeq;
  if (query.trim().length < 2) {
    hideResults();
    return;
  }
  let hits = [];
  try {
    hits = await SearchAirports(query);
  } catch (e) {
    setMessage(String(e));
  }
  if (seq !== searchSeq) return;
  const el = $('airport-results');
  el.innerHTML = hits.map((h) => `<button type="button" data-ident="${escapeHtml(h.ident)}"><b>${escapeHtml(h.ident)}</b> ${escapeHtml(h.name)}</button>`).join('');
  el.style.display = hits.length ? '' : 'none';
  el.querySelectorAll('[data-ident]').forEach((b) => b.addEventListener('click', () => openAirport(b.dataset.ident)));
}

async function refreshNearby() {
  const self = lastStatus && lastStatus.selfPos;
  if (!self) return;
  if (nearbyAt && distNm(nearbyAt.lat, nearbyAt.lon, self.lat, self.lon) < 5) return;
  nearbyAt = { lat: self.lat, lon: self.lon };
  let hits = [];
  try {
    hits = await NearestAirports(self.lat, self.lon);
  } catch (e) {
    return;
  }
  // Idents starting with X or containing digits are X-Plane's placeholders
  // for strips without an ICAO code - listed after the real ones.
  const placeholder = (h) => /^X|\d/.test(h.ident);
  hits = [...hits.filter((h) => !placeholder(h)), ...hits.filter(placeholder)].slice(0, 6);
  const el = $('airport-nearby');
  el.innerHTML = hits.length
    ? `<span class="muted">Nearby:</span> ` +
      hits.map((h) => `<button type="button" data-ident="${escapeHtml(h.ident)}" title="${escapeHtml(h.name)}">${escapeHtml(h.ident)} <span class="muted">${h.distNm.toFixed(0)} nm</span></button>`).join('')
    : '';
  el.querySelectorAll('[data-ident]').forEach((b) => b.addEventListener('click', () => openAirport(b.dataset.ident)));
  if (!layout && hits.length && !$('airport-search').value) openAirport(hits[0].ident);
}

$('airport-search').addEventListener('input', (e) => search(e.target.value));
$('airport-search').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') {
    const first = $('airport-results').querySelector('[data-ident]');
    openAirport(first ? first.dataset.ident : e.target.value.trim().toUpperCase());
  }
  if (e.key === 'Escape') hideResults();
});
$('airport-wind-dir').addEventListener('input', readManualWind);
$('airport-wind-kt').addEventListener('input', readManualWind);
$('airport-wind-reset').addEventListener('click', () => {
  manualWind = null;
  renderRunways();
  renderWind();
});
$('airport-charts').addEventListener('click', () => {
  if (layout) BrowserOpenURL(`https://chartfox.org/${encodeURIComponent(layout.ident)}`);
});
$('airport-on-map').addEventListener('click', () => {
  if (layout) window.dispatchEvent(new CustomEvent('xpmc-show-on-map', { detail: { lat: layout.lat, lon: layout.lon } }));
});
$('airport-fit').addEventListener('click', fitDiagram);
setupDiagramInput();

// --- Entry points (main.js) --------------------------------------------------------

export function showAirports() {
  refreshNearby();
  if (!layout) {
    let stored = null;
    try {
      stored = localStorage.getItem('xpmulticrew.airport');
    } catch (e) {
      stored = null;
    }
    if (stored) openAirport(stored);
  } else {
    applyView();
  }
}

export function updateAirports(data, pageVisible) {
  lastStatus = data;
  if (!pageVisible) return;
  refreshNearby();
  if (layout) {
    if (!manualWind) {
      renderRunways();
      renderWind();
    }
    drawSelf();
  }
}
