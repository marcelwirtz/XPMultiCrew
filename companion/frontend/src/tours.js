// Tours page (companion/tours.go): journeys over several evenings, flown
// leg by leg. Generate one (round trip, one way or exploring), re-roll the
// legs you don't like, then on the evening "Plan this leg" opens the map
// with an auto route and the weather briefing. Legs tick themselves off
// from the logbook; each has a note for the travel journal. A tour is
// shared as a code to paste. Loaded the first time the page opens.
import * as maplibregl from 'maplibre-gl';
import 'maplibre-gl/dist/maplibre-gl.css';
import workerUrl from 'maplibre-gl/dist/maplibre-gl-worker.mjs?worker&url';

import { DeleteTour, GenerateTour, ImportTourCode, ListTours, SaveTour, TourLegSights, TourShareCode } from '../wailsjs/go/main/App';

maplibregl.setWorkerUrl(workerUrl);

const $ = (id) => document.getElementById(id);
const kStyle = 'https://tiles.openfreemap.org/styles/dark';
const kDone = '#3ccf6e';
const kNext = '#f5d142';
const kLater = '#8b93a1';

let map = null;
let mapReady = false;
let mapFailed = false;
let tours = [];
let current = null; // the tour shown (saved, or a preview with no id)
let lastRequest = null;
const legSightsCache = new Map(); // "from>to" -> sights on the way (sights.go)
const kSightIcons = { castle: '🏰', ruins: '🏚', palace: '🏛', lighthouse: '🗼', mountain: '⛰', volcano: '🌋', dam: '🧱', waterfall: '💧',
  lake: '🌊', reservoir: '🌊', tower: '🗼', cathedral: '⛪', bridge: '🌉', island: '🏝', glacier: '🧊', fjord: '🌊', attraction: '★' };
let seed = 0;

function esc(text) {
  return String(text ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
}

function minutes(min) {
  const m = Math.round(min);
  return `${Math.floor(m / 60)}:${String(m % 60).padStart(2, '0')}`;
}

function setMessage(text, isError) {
  const el = $('tours-message');
  el.style.display = text ? '' : 'none';
  el.className = isError ? 'status err' : 'status';
  el.textContent = text || '';
}

// --- List -----------------------------------------------------------------------

function progressOf(t) {
  const p = t.progress || { done: 0, flownNm: 0, totalNm: 0 };
  return { p, pct: t.legs.length ? Math.round((p.done / t.legs.length) * 100) : 0 };
}

function renderList() {
  $('tours-list').innerHTML = tours.length
    ? tours.map((t) => {
      const { p, pct } = progressOf(t);
      return `<div class="tour-card${current && current.id === t.id ? ' active' : ''}" data-id="${esc(t.id)}">
        <div class="t">${esc(t.name)}</div>
        <div class="m">${p.finished ? 'Finished ✓' : `${p.done}/${t.legs.length} legs`} · ${Math.round(p.totalNm)} NM</div>
        <div class="bar"><div style="width:${pct}%"></div></div></div>`;
    }).join('')
    : '<div class="hint" style="margin-top: 0;">No tours yet. A tour is a journey over several evenings - press "New tour".</div>';
}

async function refresh(selectId) {
  try {
    tours = (await ListTours()) || [];
  } catch (e) {
    setMessage(String(e), true);
    tours = [];
  }
  const want = selectId ?? (current && current.id);
  const found = tours.find((t) => t.id === want);
  if (found) showTour(found);
  else if (!current && tours.length) showTour(tours[0]);
  renderList();
}

// --- Form -------------------------------------------------------------------------

function showForm(on) {
  $('tours-form').style.display = on ? '' : 'none';
  if (on) {
    $('tour-detail').style.display = 'none';
    setMessage('');
  }
}

function syncModeFields() {
  const mode = $('tf-mode').value;
  $('tf-to-wrap').style.display = mode === 'oneway' ? '' : 'none';
  $('tf-dir-wrap').style.display = mode === 'oneway' ? 'none' : '';
}

function formRequest() {
  return {
    name: $('tf-name').value.trim(),
    from: $('tf-from').value.trim().toUpperCase(),
    legs: parseInt($('tf-legs').value, 10) || 5,
    hoursPerLeg: parseFloat($('tf-hours').value) || 1.5,
    tasKt: parseInt($('tf-tas').value, 10) || 100,
    minRunwayM: parseInt($('tf-rwy').value, 10) || 0,
    mode: $('tf-mode').value,
    to: $('tf-to').value.trim().toUpperCase(),
    direction: parseFloat($('tf-dir').value) || 0,
    seed: 0,
    style: { cruiseFt: parseInt($('tf-cruise').value, 10) || 3500, avoidControlled: $('tf-avoid').checked, radioNav: $('tf-radio').checked },
  };
}

async function generate(req) {
  setMessage('Picking the stops...');
  try {
    const t = await GenerateTour(req);
    lastRequest = req;
    showForm(false);
    setMessage('');
    showTour(t);
  } catch (e) {
    setMessage(String(e), true);
  }
}

// --- Detail ------------------------------------------------------------------------

function legColor(t, i) {
  const l = t.legs[i];
  if (l.done) return kDone;
  return t.progress && t.progress.next === i ? kNext : kLater;
}

function renderMap() {
  if (!map || !mapReady || !current) return;
  const t = current;
  const lines = [];
  const points = [{ type: 'Feature', geometry: { type: 'Point', coordinates: [t.fromLon, t.fromLat] }, properties: { label: t.from, color: '#e6e8eb' } }];
  let prev = [t.fromLon, t.fromLat];
  t.legs.forEach((l, i) => {
    const here = [l.lon, l.lat];
    lines.push({ type: 'Feature', geometry: { type: 'LineString', coordinates: [prev, here] }, properties: { color: legColor(t, i) } });
    if (l.to !== t.from) points.push({ type: 'Feature', geometry: { type: 'Point', coordinates: here }, properties: { label: `${i + 1} ${l.to}`, color: legColor(t, i) } });
    prev = here;
  });
  map.getSource('tour-legs').setData({ type: 'FeatureCollection', features: lines });
  map.getSource('tour-stops').setData({ type: 'FeatureCollection', features: points });
  const all = points.map((p) => p.geometry.coordinates);
  const b = all.reduce((acc, p) => acc.extend(p), new maplibregl.LngLatBounds(all[0], all[0]));
  map.fitBounds(b, { padding: 50, maxZoom: 9, duration: 0 });
}

function initMap() {
  if (map || mapFailed) return;
  try {
    map = new maplibregl.Map({ container: 'tours-map', style: kStyle, center: [8.5, 50], zoom: 5, attributionControl: { compact: true } });
  } catch (e) {
    mapFailed = true;
    return;
  }
  map.addControl(new maplibregl.NavigationControl({ showCompass: false }), 'top-right');
  map.on('load', () => {
    const empty = { type: 'FeatureCollection', features: [] };
    map.addSource('tour-legs', { type: 'geojson', data: empty });
    map.addSource('tour-stops', { type: 'geojson', data: empty });
    map.addLayer({ id: 'tour-legs', type: 'line', source: 'tour-legs', layout: { 'line-cap': 'round' }, paint: { 'line-color': ['get', 'color'], 'line-width': 3 } });
    map.addLayer({
      id: 'tour-stops', type: 'circle', source: 'tour-stops',
      paint: { 'circle-radius': 6, 'circle-color': ['get', 'color'], 'circle-stroke-color': '#14171c', 'circle-stroke-width': 2 },
    });
    map.addLayer({
      id: 'tour-stop-labels', type: 'symbol', source: 'tour-stops',
      layout: { 'text-field': ['get', 'label'], 'text-font': ['Noto Sans Regular'], 'text-size': 11, 'text-offset': [0, 1.2], 'text-anchor': 'top' },
      paint: { 'text-color': ['get', 'color'], 'text-halo-color': '#14171c', 'text-halo-width': 1.2 },
    });
    mapReady = true;
    renderMap();
  });
}

function showTour(t) {
  current = t;
  showForm(false);
  $('tour-detail').style.display = '';
  $('td-name').value = t.name;
  const saved = !!t.id;
  $('td-actions').innerHTML = saved
    ? '<button id="td-share" class="secondary" type="button" title="Copy a code your buddy can import">Copy share code</button> <button id="td-delete" class="secondary" type="button">Delete</button>'
    : '<button id="td-save" type="button">Save tour</button> <button id="td-other" class="secondary" type="button">Other suggestion</button> <button id="td-discard" class="secondary" type="button">Discard</button>';
  const { p, pct } = progressOf(t);
  $('td-bar').style.width = `${pct}%`;
  $('td-progress').textContent = saved
    ? `${p.done}/${t.legs.length} legs · ${Math.round(p.flownNm)} of ${Math.round(p.totalNm)} NM${p.flownMin ? ` · ${minutes(p.flownMin)} h flown` : ''}${p.finished ? ' · finished!' : ''}`
    : `Preview: ${t.legs.length} legs, ${Math.round(p.totalNm)} NM - save it to start flying`;
  $('td-legs').innerHTML = t.legs.map((l, i) => {
    const next = saved && p.next === i;
    const status = l.done ? `<span class="st done">✓ flown${l.doneAt ? ` ${new Date(l.doneAt * 1000).toLocaleDateString()}` : ''}</span>`
      : next ? '<span class="st next">▶ next</span>' : '<span class="st"></span>';
    const tags = l.tags.length ? `<div class="tags">${l.tags.map((g) => `<span>${esc(g)}</span>`).join('')}</div>` : '';
    const acts = [`<button data-act="plan" data-i="${i}" type="button">Plan this leg</button>`];
    if (l.flightId) acts.push(`<button class="secondary" data-act="debrief" data-i="${i}" type="button">Debrief</button>`);
    if (!l.done || !saved) acts.push(`<button class="secondary" data-act="reroll" data-i="${i}" type="button" title="New stops from here on">Re-roll from here</button>`);
    if (saved && !l.flightId) acts.push(`<label class="checkbox-row" style="margin:0"><input type="checkbox" data-act="manual" data-i="${i}" ${l.manualDone ? 'checked' : ''}><span>flown</span></label>`);
    return `<div class="tour-leg">
      <div class="top"><span class="n">${i + 1}</span><span class="r">${esc(l.from)} → ${esc(l.to)}</span><span class="meta">${esc(l.toName)} · ${Math.round(l.distanceNm)} NM · ~${minutes(l.flightMin)} h</span>${status}</div>
      ${tags}
      <div class="leg-sights" data-leg="${esc(l.from)}>${esc(l.to)}">${legSightsHtml(l)}</div>
      <div class="acts">${acts.join('')}</div>
      ${saved ? `<textarea data-i="${i}" placeholder="Travel journal: how was it?">${esc(l.note)}</textarea>` : ''}
    </div>`;
  }).join('');
  renderList();
  initMap();
  if (map) map.resize();
  renderMap();
  loadLegSights(t);
}

function legSightsHtml(l) {
  const list = legSightsCache.get(`${l.from}>${l.to}`);
  if (!list) return '';
  if (!list.length) return '';
  return `On the way: ${list.map((s) => `${kSightIcons[s.kind] || '★'} ${esc(s.name)}`).join(' · ')}`;
}

// Fills in the "on the way" sights of the legs not looked up yet.
async function loadLegSights(t) {
  const missing = t.legs.filter((l) => !legSightsCache.has(`${l.from}>${l.to}`));
  if (!missing.length) return;
  try {
    const res = await TourLegSights({ ...t, legs: t.legs });
    t.legs.forEach((l, i) => legSightsCache.set(`${l.from}>${l.to}`, res[i] || []));
  } catch (e) {
    return; // offline - the tour works without them
  }
  if (current !== t) return;
  document.querySelectorAll('#td-legs .leg-sights').forEach((el) => {
    const [from, to] = el.dataset.leg.split('>');
    el.innerHTML = legSightsHtml({ from, to });
  });
}

async function save(t) {
  try {
    const saved = await SaveTour(t);
    current = saved;
    await refresh(saved.id);
  } catch (e) {
    setMessage(String(e), true);
  }
}

async function reroll(i) {
  const t = current;
  const base = lastRequest && !t.id ? lastRequest : {
    name: t.name, from: t.from, legs: t.legs.length, hoursPerLeg: t.hoursPerLeg, tasKt: t.tasKt, minRunwayM: t.minRunwayM,
    mode: t.mode, to: t.to || '', direction: t.direction, style: t.style,
  };
  seed++;
  try {
    const fresh = await GenerateTour({ ...base, name: t.name, seed, keep: t.legs.slice(0, i) });
    if (t.id) {
      // Keep the saved tour's identity, journal and ticks of the kept legs.
      await save({ ...t, legs: fresh.legs.map((l, k) => (k < i ? t.legs[k] : l)) });
    } else {
      showTour(fresh);
    }
  } catch (e) {
    setMessage(String(e), true);
  }
}

function planLeg(i) {
  const t = current;
  const l = t.legs[i];
  window.dispatchEvent(new CustomEvent('xpmc-plan-leg', {
    detail: { from: l.from, to: l.to, tasKt: t.tasKt, cruiseFt: t.style.cruiseFt || 3500, avoidControlled: t.style.avoidControlled, radioNav: t.style.radioNav, name: `${t.name} ${i + 1}/${t.legs.length}` },
  }));
}

// --- Events -----------------------------------------------------------------------

$('tours-list').addEventListener('click', (e) => {
  const card = e.target.closest('.tour-card');
  if (!card) return;
  const t = tours.find((x) => x.id === card.dataset.id);
  if (t) showTour(t);
});
$('tours-new').addEventListener('click', () => {
  showForm(true);
  syncModeFields();
});
$('tf-mode').addEventListener('change', syncModeFields);
$('tf-cancel').addEventListener('click', () => {
  showForm(false);
  if (current) showTour(current);
});
$('tf-generate').addEventListener('click', () => {
  seed = 0;
  generate(formRequest());
});
$('tours-import').addEventListener('click', async () => {
  const code = prompt('Paste the tour code from your buddy:');
  if (!code) return;
  try {
    const t = await ImportTourCode(code);
    setMessage(`Imported "${t.name}".`);
    await refresh(t.id);
  } catch (e) {
    setMessage(String(e), true);
  }
});

$('td-actions').addEventListener('click', async (e) => {
  const id = e.target.id;
  if (id === 'td-save') save({ ...current, name: $('td-name').value.trim() || current.name });
  if (id === 'td-other') {
    seed++;
    generate({ ...lastRequest, seed });
  }
  if (id === 'td-discard') {
    current = null;
    $('tour-detail').style.display = 'none';
    refresh();
  }
  if (id === 'td-delete') {
    if (!confirm(`Delete the tour "${current.name}"? Its journal notes go with it.`)) return;
    try {
      await DeleteTour(current.id);
    } catch (err) {
      setMessage(String(err), true);
    }
    current = null;
    $('tour-detail').style.display = 'none';
    refresh();
  }
  if (id === 'td-share') {
    try {
      const code = await TourShareCode(current);
      try {
        await navigator.clipboard.writeText(code);
        setMessage('Tour code copied - send it to your buddy, they press "Import code".');
      } catch (err) {
        prompt('Copy this tour code for your buddy:', code);
      }
    } catch (err) {
      setMessage(String(err), true);
    }
  }
});

$('td-name').addEventListener('change', () => {
  const name = $('td-name').value.trim();
  if (!name || !current) return;
  current.name = name;
  if (current.id) save(current);
});

$('td-legs').addEventListener('click', (e) => {
  const btn = e.target.closest('button[data-act]');
  if (!btn) return;
  const i = Number(btn.dataset.i);
  if (btn.dataset.act === 'plan') planLeg(i);
  if (btn.dataset.act === 'reroll') reroll(i);
  if (btn.dataset.act === 'debrief') window.dispatchEvent(new CustomEvent('xpmc-open-flight', { detail: { id: current.legs[i].flightId } }));
});
$('td-legs').addEventListener('change', (e) => {
  const i = Number(e.target.dataset.i);
  if (!current || !current.id || Number.isNaN(i)) return;
  if (e.target.dataset.act === 'manual') {
    current.legs[i].manualDone = e.target.checked;
    save(current);
  } else if (e.target.tagName === 'TEXTAREA') {
    current.legs[i].note = e.target.value;
    save(current);
  }
});

export function showTours() {
  initMap();
  if (map) map.resize();
  refresh();
}
