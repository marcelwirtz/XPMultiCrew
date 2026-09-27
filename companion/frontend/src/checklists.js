// Checklists page: shared, self-checking checklists for Shared Cockpit
// (companion/checklists.go for the file format). Items with a condition
// tick themselves from live dataref values (the plugin's WATCH /
// WATCH_VALUES); the rest are ticked by hand. Which list is open and the
// hand-ticked items are shared with the co-pilot (CHECKLIST_SYNC ->
// CHECKLIST_REMOTE), newest change wins.
import {
  DeleteUserChecklists,
  LoadChecklists,
  SaveChecklists,
  AddFlightEvent,
  SetChecklistWatch,
  ShareChecklistState,
} from '../wailsjs/go/main/App';

let file = null; // ChecklistFile
let active = 0;
let ticks = []; // per list: Set of manually ticked item indices
let changedAt = 0; // ms, for "newest change wins"
let lastRemote = '';
let values = {};
let sharedSession = false;
let ownIcao = '';
let lastWatchSent = 0;
let reportedComplete = new Set(); // list titles already marked on the Debrief timeline

const $ = (id) => document.getElementById(id);

function escapeHtml(text) {
  return String(text ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
}

function conditionMet(cond) {
  const v = values[cond.key];
  if (v === undefined) return null; // no value (yet) - unknown
  switch (cond.op) {
    case '==': return Math.abs(v - cond.value) < 1e-3;
    case '!=': return Math.abs(v - cond.value) >= 1e-3;
    case '<': return v < cond.value;
    case '<=': return v <= cond.value + 1e-6;
    case '>': return v > cond.value;
    case '>=': return v >= cond.value - 1e-6;
    default: return null;
  }
}

function itemState(listIndex, itemIndex) {
  const item = file.lists[listIndex].items[itemIndex];
  if (ticks[listIndex] && ticks[listIndex].has(itemIndex)) return 'done';
  if (item.condition) {
    const met = conditionMet(item.condition);
    if (met === true) return 'auto';
    if (met === false) return 'open-auto';
  }
  return 'open';
}

// --- Sharing -------------------------------------------------------------------

function encodeState() {
  const masks = ticks.map((set) => {
    let mask = 0n;
    for (const i of set) mask |= 1n << BigInt(i);
    return mask.toString(16);
  });
  return `v1|${file.icao}|${active}|${masks.join(',')}|${changedAt}`;
}

function share() {
  if (!file || file.lists.length === 0) return;
  ShareChecklistState(encodeState()).catch(() => {});
}

function applyRemote(payload) {
  if (!payload || payload === lastRemote || !file) return;
  lastRemote = payload;
  const f = payload.split('|');
  if (f.length !== 5 || f[0] !== 'v1' || f[1] !== file.icao) return;
  const ts = Number(f[4]);
  if (!(ts > changedAt)) return;
  changedAt = ts;
  active = Math.min(Math.max(Number(f[2]) || 0, 0), file.lists.length - 1);
  const masks = f[3].split(',');
  ticks = file.lists.map((list, li) => {
    const set = new Set();
    let mask = 0n;
    try {
      mask = BigInt('0x' + (masks[li] || '0'));
    } catch (e) {
      mask = 0n;
    }
    list.items.forEach((_, i) => {
      if ((mask >> BigInt(i)) & 1n) set.add(i);
    });
    return set;
  });
  watchActiveList();
  render();
}

function localChange() {
  changedAt = Date.now();
  share();
  render();
}

// --- Rendering -------------------------------------------------------------------

function watchActiveList() {
  lastWatchSent = Date.now();
  if (!file || !file.lists[active]) {
    SetChecklistWatch([]).catch(() => {});
    return;
  }
  const keys = file.lists[active].items.filter((i) => i.condition).map((i) => i.condition.key);
  SetChecklistWatch([...new Set(keys)]).catch(() => {});
}

function render() {
  const body = $('checklist-body');
  $('checklist-edit-btn').disabled = !file;
  if (!file) {
    body.innerHTML = '';
    return;
  }
  if (file.lists.length === 0) {
    body.innerHTML = `<div class="hint">No checklists for ${escapeHtml(file.icao)} yet - click "Edit" to write some ` +
      '(or copy the C172 one as a starting point).</div>';
    $('checklist-source').textContent = '';
    return;
  }
  $('checklist-source').textContent =
    `${file.source === 'user' ? 'Your checklists' : 'Bundled checklists'} for ${file.icao} · ` +
    (sharedSession ? 'shared with your co-pilot' : 'not in a Shared Cockpit session - only you see your ticks');
  const tabs = file.lists
    .map((list, li) => {
      const done = list.items.filter((_, i) => ['done', 'auto'].includes(itemState(li, i))).length;
      return `<button type="button" class="${li === active ? 'active' : ''}" data-list="${li}">${escapeHtml(list.title)} ` +
        `<span class="cl-count">${done}/${list.items.length}</span></button>`;
    })
    .join('');
  const list = file.lists[active];
  const icons = { done: '✓', auto: '✓', 'open-auto': '✗', open: '○' };
  const titles = {
    done: 'Ticked by hand', auto: 'Checked automatically - the switch is in that position',
    'open-auto': 'Not yet - the switch is not in that position', open: 'Click to tick',
  };
  const items = list.items
    .map((item, i) => {
      const state = itemState(active, i);
      return `<li class="cl-item ${state}" data-item="${i}" title="${titles[state]}">
        <span class="cl-icon">${icons[state]}</span>
        <span class="cl-challenge">${escapeHtml(item.challenge)}</span>
        <span class="cl-dots"></span>
        <span class="cl-response">${escapeHtml(item.response)}</span>
      </li>`;
    })
    .join('');
  const complete = list.items.every((_, i) => ['done', 'auto'].includes(itemState(active, i)));
  // Each completed list once on the Debrief timeline (companion/flights.go);
  // resetting it and working through it again marks it again.
  if (complete && !reportedComplete.has(list.title)) {
    reportedComplete.add(list.title);
    AddFlightEvent('checklist', list.title).catch(() => {});
  } else if (!complete) {
    reportedComplete.delete(list.title);
  }
  body.innerHTML = `<div class="cl-tabs">${tabs}</div>
    <ul class="cl-list">${items}</ul>
    <div class="row buttons">
      <button type="button" class="secondary" id="cl-reset">Reset this list</button>
      <button type="button" id="cl-next" ${active >= file.lists.length - 1 ? 'disabled' : ''}>${complete ? '✓ ' : ''}Next checklist →</button>
    </div>`;
  body.querySelectorAll('[data-list]').forEach((b) =>
    b.addEventListener('click', () => {
      active = Number(b.dataset.list);
      watchActiveList();
      localChange();
    }));
  body.querySelectorAll('[data-item]').forEach((li) =>
    li.addEventListener('click', () => {
      const i = Number(li.dataset.item);
      if (ticks[active].has(i)) ticks[active].delete(i);
      else ticks[active].add(i);
      localChange();
    }));
  $('cl-reset').addEventListener('click', () => {
    ticks[active] = new Set();
    localChange();
  });
  $('cl-next').addEventListener('click', () => {
    active = Math.min(active + 1, file.lists.length - 1);
    watchActiveList();
    localChange();
  });
}

async function load(icao) {
  if (!icao) return;
  try {
    file = await LoadChecklists(icao);
    $('checklist-error').textContent = '';
  } catch (e) {
    file = null;
    $('checklist-error').textContent = String(e);
  }
  active = 0;
  changedAt = 0;
  lastRemote = '';
  ticks = file ? file.lists.map(() => new Set()) : [];
  $('checklist-icao').value = file ? file.icao : icao;
  watchActiveList();
  render();
}

// --- Editor ------------------------------------------------------------------------

function openEditor(show) {
  $('checklist-editor').style.display = show ? '' : 'none';
  $('checklist-body').style.display = show ? 'none' : '';
  if (show && file) {
    $('checklist-text').value = file.text || '# CHECKLIST <title>\n# ITEM <challenge> | <response> [| <dataref> <op> <value>]\n\nCHECKLIST Before Start\nITEM Parking brake | SET\n';
    $('checklist-delete').disabled = file.source !== 'user';
  }
}

$('checklist-edit-btn').addEventListener('click', () => openEditor(true));
$('checklist-cancel').addEventListener('click', () => openEditor(false));
$('checklist-save').addEventListener('click', async () => {
  try {
    const saved = await SaveChecklists(file.icao, $('checklist-text').value);
    file = saved;
    ticks = file.lists.map(() => new Set());
    active = Math.min(active, Math.max(file.lists.length - 1, 0));
    $('checklist-error').textContent = '';
    openEditor(false);
    watchActiveList();
    render();
  } catch (e) {
    $('checklist-error').textContent = String(e);
  }
});
$('checklist-delete').addEventListener('click', async () => {
  if (!confirm(`Delete your ${file.icao} checklists? The bundled ones (if any) apply again.`)) return;
  await DeleteUserChecklists(file.icao).catch((e) => alert(e));
  openEditor(false);
  load(file.icao);
});
$('checklist-load').addEventListener('click', () => load($('checklist-icao').value.trim()));
$('checklist-icao').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') load(e.target.value.trim());
});
$('checklist-current').addEventListener('click', () => load(ownIcao));

// Called by main.js on every status event.
export function updateChecklists(data) {
  ownIcao = data.ownIcao || '';
  const btn = $('checklist-current');
  btn.style.display = ownIcao ? '' : 'none';
  btn.textContent = `Current aircraft: ${ownIcao}`;
  sharedSession = !/^(not started|unknown)/.test(data.sharedCockpit || '');
  const newValues = data.watchValues || {};
  const changed = JSON.stringify(newValues) !== JSON.stringify(values);
  values = newValues;
  if (!file && ownIcao) load(ownIcao);
  // The plugin forgets the watch list when X-Plane restarts - re-send it
  // if values are missing.
  const wantsValues = file && file.lists[active] && file.lists[active].items.some((i) => i.condition);
  if (wantsValues && Object.keys(values).length === 0 && Date.now() - lastWatchSent > 5000) watchActiveList();
  applyRemote(data.checklistRemote);
  if (changed && $('checklist-editor').style.display === 'none') render();
}

// Called when the page is opened.
export function showChecklists() {
  if (file) watchActiveList();
}
