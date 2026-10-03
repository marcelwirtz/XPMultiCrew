// Landings page ("Butter-Board"): every landing the plugin measured - ours
// and everyone's in the Multiplayer session - rated against the runway
// (companion/landings.go). The latest one is shown big, with a runway
// sketch of where the wheels touched.
import { DeleteLanding, GetLandingBoard } from '../wailsjs/go/main/App';

const $ = (id) => document.getElementById(id);

let board = { session: [], log: [] };
let shownVersion = -1;
let selectedKey = null; // clicked row; null = latest
let fetching = false;
const emptyHeroHtml = $('landing-hero').innerHTML;

function escapeHtml(text) {
  return String(text ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
}

// Score bands use the app's status colors - always next to the number and
// the verdict, never on their own.
function scoreColor(score) {
  if (score >= 80) return 'var(--ok)';
  if (score >= 55) return '#f0b429';
  return 'var(--err)';
}

function pilotName(l) {
  if (l.own) return l.callsign ? `${l.callsign} (you)` : 'You';
  return l.callsign || `Pilot ${l.senderId}`;
}

function where(l) {
  return l.airport ? `${l.airport} ${l.runway}` : '—';
}

function timeText(unix) {
  const d = new Date(unix * 1000);
  return d.toLocaleString([], { day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit' });
}

// The Approach Coach's verdict on the approach that ended in this landing.
function approachHtml(l) {
  const a = l.approach;
  if (!a) return '';
  const gate = a.stableAt500
    ? '<b class="ok">stable at 500 ft</b>'
    : `<b class="bad">unstable at 500 ft</b> (${a.atGate.map(escapeHtml).join(', ')})`;
  const below = a.warningsBelow.length ? ` · warnings below 500 ft: ${a.warningsBelow.map(escapeHtml).join(', ')}` : '';
  return `<div class="lb-approach">Approach: ${gate}${below} · max sink ${Math.round(a.maxSinkFpm)} fpm</div>`;
}

function fmtSigned(v, digits = 0) {
  return (v > 0 ? '+' : '') + v.toFixed(digits);
}

// --- Hero card ---------------------------------------------------------------

function ringSvg(score) {
  const r = 52;
  const c = 2 * Math.PI * r;
  const filled = (Math.max(0, Math.min(100, score)) / 100) * c;
  return `<svg viewBox="0 0 120 120" aria-hidden="true">
    <circle cx="60" cy="60" r="${r}" fill="none" stroke="#262b33" stroke-width="10"/>
    <circle cx="60" cy="60" r="${r}" fill="none" stroke="${scoreColor(score)}" stroke-width="10" stroke-linecap="round"
      stroke-dasharray="${filled} ${c}"/>
  </svg>`;
}

// Top view of the runway, landing direction left to right. Length to
// scale; the width is stretched (a real runway would be a hairline), so
// the centerline offset is drawn against the runway's own width.
function runwaySvg(l) {
  if (!l.airport || l.pastThrM === undefined || l.pastThrM === null) {
    return '<div class="hint">No runway found under the touchdown point (grass strip, water, or no apt.dat) - rated by the numbers only.</div>';
  }
  const W = 600;
  const H = 86;
  const pad = 26;
  const length = l.runwayLengthM || 2000;
  const scale = (W - 2 * pad) / length;
  const y0 = 22;
  const h = 40;
  const x = (m) => pad + m * scale;
  const halfW = Math.max((l.runwayWidthM || 30) / 2, 5);
  const td = Math.max(-pad / scale, Math.min(length + 10, l.pastThrM));
  const cl = Math.max(-1.3, Math.min(1.3, (l.centerlineM || 0) / halfW)); // -1..1 = runway edge
  // Direction: positive centerline = right of the aircraft = down in the sketch.
  const tdY = y0 + h / 2 + cl * (h / 2);
  const zone = [150, Math.min(900, length / 3)];
  const bars = [];
  for (let i = 0; i < 6; i++) bars.push(`<rect x="${x(8)}" y="${y0 + 4 + i * 6}" width="${Math.max(3, 30 * scale)}" height="3" fill="#8b93a1"/>`);
  return `<svg viewBox="0 0 ${W} ${H}" aria-label="Touchdown point on runway ${escapeHtml(l.runway)}">
    <rect x="${x(0)}" y="${y0}" width="${length * scale}" height="${h}" rx="3" fill="#2a2f37"/>
    <rect x="${x(zone[0])}" y="${y0}" width="${(zone[1] - zone[0]) * scale}" height="${h}" fill="rgba(95,217,122,0.10)"/>
    ${bars.join('')}
    <line x1="${x(60)}" y1="${y0 + h / 2}" x2="${x(length - 60)}" y2="${y0 + h / 2}" stroke="#8b93a1" stroke-width="1.5" stroke-dasharray="10 8"/>
    <rect x="${x(300)}" y="${y0 + 6}" width="${Math.max(4, 45 * scale)}" height="6" fill="#c9ced6"/>
    <rect x="${x(300)}" y="${y0 + h - 12}" width="${Math.max(4, 45 * scale)}" height="6" fill="#c9ced6"/>
    <text x="${x(0)}" y="14" font-size="11" fill="#8b93a1">${escapeHtml(l.runway)} ▸</text>
    <text x="${x(zone[0])}" y="${y0 + h + 14}" font-size="10" fill="#8b93a1">touchdown zone</text>
    <text x="${x(length)}" y="14" font-size="11" fill="#8b93a1" text-anchor="end">${Math.round(length)} m</text>
    <line x1="${x(td)}" y1="${y0 - 6}" x2="${x(td)}" y2="${y0 + h + 4}" stroke="${scoreColor(l.score)}" stroke-width="1" opacity="0.6"/>
    <circle cx="${x(td)}" cy="${tdY}" r="6" fill="${scoreColor(l.score)}" stroke="#14171c" stroke-width="2"/>
  </svg>`;
}

function renderHero() {
  const hero = $('landing-hero');
  const all = [...board.session, ...board.log];
  const l = (selectedKey && all.find((x) => x.key === selectedKey)) || board.session[0] || board.log[0];
  if (!l) {
    hero.innerHTML = emptyHeroHtml;
    return;
  }
  const side = (l.centerlineM || 0) >= 0 ? 'right' : 'left';
  const stats = [
    ['Sink rate', `${Math.round(l.vsFpm)} fpm`],
    ['G load', `${l.peakG.toFixed(2)} G`],
    ['Past threshold', l.pastThrM !== undefined && l.pastThrM !== null ? `${Math.round(l.pastThrM)} m` : '—'],
    ['Centerline', l.centerlineM !== undefined && l.centerlineM !== null ? `${Math.abs(l.centerlineM).toFixed(1)} m ${side}` : '—'],
    ['Drift', `${fmtSigned(l.driftDeg, 1)}°`],
    ['Bounces', String(l.bounces)],
    ['Float (50 ft)', l.flareM >= 0 ? `${Math.round(l.flareM)} m` : '—'],
    ['Ground speed', `${Math.round(l.gsKt)} kt`],
  ];
  hero.innerHTML = `<div class="lb-hero">
      <div class="lb-ring">${ringSvg(l.score)}<div class="lb-score"><b>${l.score}</b><span>of 100</span></div></div>
      <div>
        <div class="lb-verdict">${escapeHtml(l.verdict)}${l.touchAndGo ? ' <span class="hint">(touch &amp; go)</span>' : ''}</div>
        <div class="lb-who">${escapeHtml(pilotName(l))} · ${escapeHtml(l.icao || '?')} · ${escapeHtml(where(l))} · ${timeText(l.time)}</div>
        <div class="lb-stats">${stats.map(([k, v]) => `<div class="lb-stat"><span>${k}</span><b>${escapeHtml(v)}</b></div>`).join('')}</div>
      </div>
    </div>
    <div class="lb-runway">${runwaySvg(l)}</div>
    <div class="lb-notes">${l.notes && l.notes.length ? `Deductions: ${l.notes.map(escapeHtml).join(' · ')}` : 'No deductions - textbook.'}</div>
    ${approachHtml(l)}`;
}

// --- Tables --------------------------------------------------------------------

function rowHtml(l, i, withDelete) {
  return `<tr data-key="${escapeHtml(l.key)}" class="${l.own ? 'own' : ''}">
    <td class="num">${i + 1}</td>
    <td>${escapeHtml(pilotName(l))}</td>
    <td>${escapeHtml(l.icao || '')}</td>
    <td>${escapeHtml(where(l))}</td>
    <td class="num">${Math.round(l.vsFpm)}</td>
    <td class="num">${l.peakG.toFixed(2)}</td>
    <td class="num"><span class="lb-pill" style="background:${scoreColor(l.score)}">${l.score}</span></td>
    ${withDelete ? `<td class="num">${timeText(l.time)} <button class="row-delete" data-delete="${escapeHtml(l.key)}" title="Remove from my log">×</button></td>` : ''}
  </tr>`;
}

const kHead = (last) => `<tr><th class="num">#</th><th>Pilot</th><th>Type</th><th>Runway</th><th class="num">fpm</th>
  <th class="num">G</th><th class="num">Score</th>${last ? `<th class="num">${last}</th>` : ''}</tr>`;

function renderTables() {
  const ranked = [...board.session].sort((a, b) => b.score - a.score || Math.abs(a.vsFpm) - Math.abs(b.vsFpm));
  $('session-table').innerHTML = ranked.length ? kHead('') + ranked.map((l, i) => rowHtml(l, i, false)).join('') : '';
  $('session-empty').textContent = ranked.length
    ? 'Ranked by score - who greased it best?'
    : 'Landings of everyone in the session since the app was started.';

  const log = board.log.slice(0, 100);
  $('log-table').innerHTML = log.length ? kHead('When') + log.map((l, i) => rowHtml(l, i, true)).join('') : '';

  const records = $('landing-records');
  if (!board.log.length) {
    records.innerHTML = '<div class="hint" style="margin: 0; grid-column: 1 / -1;">Your own landings are kept here across sessions.</div>';
  } else {
    const best = board.log.reduce((a, b) => (b.score > a.score ? b : a));
    const softest = board.log.reduce((a, b) => (Math.abs(b.vsFpm) < Math.abs(a.vsFpm) ? b : a));
    const avg = board.log.reduce((s, l) => s + l.score, 0) / board.log.length;
    const butter = board.log.filter((l) => Math.abs(l.vsFpm) < 60).length;
    const tile = (label, value, sub) =>
      `<div class="lb-stat"><span>${label}</span><b>${escapeHtml(value)}</b><div class="hint" style="margin:0">${escapeHtml(sub)}</div></div>`;
    records.innerHTML =
      tile('Landings', String(board.log.length), `${butter} butter`) +
      tile('Average score', avg.toFixed(0), 'all landings') +
      tile('Best score', String(best.score), where(best)) +
      tile('Softest', `${Math.round(softest.vsFpm)} fpm`, where(softest));
  }

  document.querySelectorAll('#page-landings tr[data-key]').forEach((tr) =>
    tr.addEventListener('click', () => {
      selectedKey = tr.dataset.key;
      renderHero();
      $('page-landings').closest('.content').scrollTo({ top: 0, behavior: 'smooth' });
    }));
  document.querySelectorAll('#page-landings [data-delete]').forEach((b) =>
    b.addEventListener('click', async (e) => {
      e.stopPropagation();
      if (!confirm('Remove this landing from your log?')) return;
      await DeleteLanding(b.dataset.delete);
      refresh();
    }));
}

async function refresh() {
  if (fetching) return;
  fetching = true;
  try {
    board = await GetLandingBoard();
    board.session = board.session || [];
    board.log = board.log || [];
    renderHero();
    renderTables();
  } finally {
    fetching = false;
  }
}

// Called by main.js on every status event: refetch when a landing came in,
// and point at the page with the sidebar dot.
export function updateLandings(data, pageVisible) {
  if (data.landingsVersion === shownVersion) return;
  const first = shownVersion === -1;
  shownVersion = data.landingsVersion;
  selectedKey = null; // a new landing takes the hero spot
  if (!first && !pageVisible) {
    const dot = $('nav-dot-landings');
    dot.className = 'nav-dot shown ok';
  }
  refresh();
}

export function showLandings() {
  $('nav-dot-landings').className = 'nav-dot';
  refresh();
}
