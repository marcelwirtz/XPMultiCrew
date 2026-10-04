import {
  ApplyUpdate,
  CheckForUpdate,
  ChooseXPlanePath,
  ClaimOwnership,
  CreateSession,
  DeleteSavedServer,
  DisconnectFormation,
  DeleteUserProfile,
  DisconnectSharedCockpit,
  GetAvailablePluginVersion,
  GetDuplicatePluginInstalls,
  GetPrefs,
  GetInstalledPluginVersion,
  GetLastPage,
  GetRecentLogLines,
  GetSavedServers,
  GetXPlanePath,
  InstallPlugin,
  JoinSession,
  ListProfiles,
  LoadProfile,
  ReloadCsl,
  SyncEnv,
  SaveProfile,
  SaveServer,
  ScResync,
  SearchDatarefs,
  SetApproachCoach,
  SetLastPage,
  GetNavCollapsed,
  SetNavCollapsed,
  SetPrefs,
  StartLearn,
  StartSharedCockpit,
  StopLearn,
} from '../wailsjs/go/main/App';

// Everything peer-supplied (callsigns, ICAO types) goes through this before
// landing in innerHTML.
function escapeHtml(text) {
  return String(text ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
}
import { EventsOn } from '../wailsjs/runtime/runtime';
import { updateProgress } from './route-progress.js';
import { recordTracks } from './tracks.js';
import { showChecklists, updateChecklists } from './checklists.js';
import { showLandings, updateLandings } from './landings.js';
import { openAirport, showAirports, updateAirports } from './airports.js';

// The map (MapLibre, ~1 MB) is only loaded the first time its page opens.
let mapModule = null;
let lastStatusForMap = null;
// A page module that fails to load would otherwise leave its page blank
// without a word (a rejected import() is only logged to the console).
function pageLoadFailed(page, e) {
  console.error(`${page} page failed to load`, e);
  const el = document.getElementById(`page-${page}`);
  if (!el || el.querySelector('.page-load-error')) return;
  const msg = document.createElement('div');
  msg.className = 'status err page-load-error';
  msg.textContent = `This page couldn't be loaded (${(e && e.message) || e}).`;
  el.prepend(msg);
}

// The Debrief page brings its own map - loaded the same way.
let debriefModule = null;
let lastStatusForDebrief = null;
let debriefSeenFlightsVersion = null;
function openDebriefPage() {
  import('./debrief.js').then((m) => {
    debriefModule = m;
    m.showDebrief();
    if (lastStatusForDebrief) m.updateDebrief(lastStatusForDebrief, true);
  }).catch((e) => pageLoadFailed('debrief', e));
}

// Airports page <-> map: "Details" in the map's airport popup, "Show on
// map" on the Airports page.
window.addEventListener('xpmc-open-airport', (e) => {
  showPage('airports');
  openAirport(e.detail.ident);
});
// Tours -> Map: plan one leg (auto route + weather briefing).
window.addEventListener('xpmc-plan-leg', (e) => {
  showPage('map');
  import('./map.js').then((m) => m.planLeg(e.detail)).catch((err) => pageLoadFailed('map', err));
});
// Logbook -> Debrief: open one recorded flight.
window.addEventListener('xpmc-open-flight', (e) => {
  showPage('debrief');
  import('./debrief.js').then((m) => m.showFlight(e.detail.id)).catch((err) => pageLoadFailed('debrief', err));
});
window.addEventListener('xpmc-show-on-map', (e) => {
  showPage('map');
  import('./map.js').then((m) => m.centerOn(e.detail.lat, e.detail.lon)).catch((err) => pageLoadFailed('map', err));
});

// Sidebar dot on Debrief: while a flight is being recorded, and after one
// was saved until the page is opened.
function renderDebriefDot(data) {
  if (debriefSeenFlightsVersion === null) debriefSeenFlightsVersion = data.flightsVersion;
  const visible = document.getElementById('page-debrief').classList.contains('active');
  if (visible) debriefSeenFlightsVersion = data.flightsVersion;
  const dot = document.getElementById('nav-dot-debrief');
  const show = !!data.recording || data.flightsVersion !== debriefSeenFlightsVersion;
  dot.className = show ? 'nav-dot shown ok' : 'nav-dot';
  dot.title = data.recording ? 'Recording this flight' : show ? 'New flight recorded' : '';
}

function openMapPage() {
  import('./map.js').then((m) => {
    mapModule = m;
    if (lastStatusForMap) m.updateMap(lastStatusForMap);
    m.showMap();
  }).catch((e) => pageLoadFailed('map', e));
}

// Sidebar navigation - one .page shown at a time (see index.html's
// .app-layout comment for why this replaced one long stacked page).
// The open page is remembered in the config file (SetLastPage below);
// this key is where older versions kept it.
const kLastPageStorageKey = 'xpmulticrew.lastPage';

function showPage(page) {
  document.querySelectorAll('.sidebar button[data-page]').forEach((btn) => {
    btn.classList.toggle('active', btn.dataset.page === page);
  });
  document.querySelectorAll('.page').forEach((el) => {
    el.classList.toggle('active', el.id === `page-${page}`);
  });
  // MapLibre needs a visible container to size itself, so the map is only
  // created the first time its page is actually shown.
  if (page === 'map') openMapPage();
  if (page === 'checklists') showChecklists();
  if (page === 'landings') showLandings();
  if (page === 'airports') showAirports();
  if (page === 'debrief') openDebriefPage();
  if (page === 'tours') import('./tours.js').then((m) => m.showTours()).catch((e) => pageLoadFailed('tours', e));
  if (page === 'logbook') import('./logbook.js').then((m) => m.showLogbook()).catch((e) => pageLoadFailed('logbook', e));
  if (page === 'profiles') refreshProfileList();
  SetLastPage(page).catch(() => {});
}

document.querySelectorAll('.sidebar button[data-page]').forEach((btn) => {
  btn.addEventListener('click', () => showPage(btn.dataset.page));
});

// Collapsed menu = icon rail without the header, so the map gets the room.
// Kept in config.json like the last page (see legacyStoredPage's comment
// for why not localStorage).
function setNavCollapsed(collapsed) {
  document.body.classList.toggle('nav-collapsed', collapsed);
  const toggle = document.getElementById('nav-toggle');
  toggle.title = collapsed ? 'Expand menu' : 'Collapse menu';
  toggle.querySelector('.nav-label').textContent = collapsed ? 'Expand' : 'Collapse';
  // map.js resizes MapLibre on window resize; the container changed size too.
  window.dispatchEvent(new Event('resize'));
}
document.getElementById('nav-toggle').addEventListener('click', () => {
  const collapsed = !document.body.classList.contains('nav-collapsed');
  setNavCollapsed(collapsed);
  SetNavCollapsed(collapsed).catch(() => {});
});
GetNavCollapsed()
  .then(setNavCollapsed)
  .catch(() => {});

// 'setup' as the fallback (not 'formation', despite it being first in the
// sidebar - see index.html's comment) since a genuinely first-ever launch
// needs the X-Plane path chosen and the plugin installed before Formation/
// Shared Cockpit are even usable. Once there's a stored page (i.e. every
// launch after the first), that takes over instead - see below.
// Kept in the companion's config file (app.go's SetLastPage) - WebKit's
// localStorage turned out not to be written reliably when the window is
// closed on Linux. The old localStorage value is only a one-time fallback.
function legacyStoredPage() {
  try {
    return localStorage.getItem(kLastPageStorageKey);
  } catch (e) {
    return null;
  }
}
GetLastPage()
  .catch(() => '')
  .then((stored) => {
    const page = stored || legacyStoredPage();
    showPage(page && document.getElementById(`page-${page}`) ? page : 'setup');
  });

let role = 'MASTER';

// Updated by the 'status' handler below, read by the update button's
// click handler to warn (not block) if a session is currently active -
// restarting the companion app drops its own connection to the
// plugin/rendezvous server, though the plugin itself keeps running
// independently inside X-Plane.
let lastFormationIdle = true;
let lastSharedCockpitIdle = true;

function setSetupStatus(text, kind) {
  const el = document.getElementById('setup-status');
  el.className = 'status ' + (kind || '');
  document.getElementById('setup-status-text').textContent = text;
}

// Refreshes the Installed/Available version display and highlights when
// an update is available - called on load, after choosing a new X-Plane
// folder, and after Install/Update Plugin completes (all three can change
// what "installed" means).
async function refreshVersions() {
  const [installed, available, duplicates] = await Promise.all([
    GetInstalledPluginVersion(),
    GetAvailablePluginVersion(),
    GetDuplicatePluginInstalls(),
  ]);
  document.getElementById('installed-version').textContent = installed || 'not installed';
  document.getElementById('available-version').textContent = available;

  const infoEl = document.getElementById('version-info');
  const updateAvailable = installed && installed !== available;
  infoEl.classList.toggle('update-available', Boolean(updateAvailable));

  document.getElementById('duplicate-plugins').textContent = (duplicates || []).join(', ');
  document.getElementById('duplicate-plugins-row').style.display = duplicates && duplicates.length ? '' : 'none';
}

document.getElementById('xplane-path').value = '';
GetXPlanePath().then((path) => {
  if (path) {
    document.getElementById('xplane-path').value = path;
    setSetupStatus('ready to install/update', '');
  }
  refreshVersions();
});

// Checked on load and then every 6 hours (the app often stays open for a
// whole flying evening), not on the 1s status-poll ticker - see
// App.CheckForUpdate's comment (companion/app.go). Resolves to null both
// when already up to date and for a local dev build. A failed check (no
// internet, GitHub refusing) is retried after 15 minutes.
const kUpdateCheckIntervalMs = 6 * 60 * 60 * 1000;
const kUpdateRetryMs = 15 * 60 * 1000;
function checkForUpdate() {
  CheckForUpdate()
    .then((info) => {
      setTimeout(checkForUpdate, kUpdateCheckIntervalMs);
      if (!info) return;
      document.getElementById('update-banner-text').textContent = `Update available: ${info.version}`;
      document.getElementById('update-banner').style.display = '';
    })
    .catch((e) => {
      console.warn('update check failed, retrying in 15 min:', e);
      setTimeout(checkForUpdate, kUpdateRetryMs);
    });
}
checkForUpdate();

// Address book - one shared saved-servers list backing both the
// Formation and Shared Cockpit server-address rows (a server isn't
// specific to either mode). Each row gets its own <select id="X-saved">
// (auto-fills the matching text input on pick) and its own
// <button id="X-save-btn"> (prompts for a label, saves the input's
// current value under it).
const addressRows = [
  { inputId: 'server', selectId: 'server-saved', saveBtnId: 'server-save-btn', deleteBtnId: 'server-delete-btn' },
  {
    inputId: 'sc-server',
    selectId: 'sc-server-saved',
    saveBtnId: 'sc-server-save-btn',
    deleteBtnId: 'sc-server-delete-btn',
  },
];

async function refreshSavedServers() {
  const servers = await GetSavedServers();
  for (const row of addressRows) {
    const select = document.getElementById(row.selectId);
    const previous = select.value;
    select.innerHTML = '<option value="">Saved…</option>';
    for (const s of servers) {
      const opt = document.createElement('option');
      opt.value = s.hostPort;
      opt.textContent = s.label;
      opt.dataset.label = s.label;
      select.appendChild(opt);
    }
    // Keep whatever was selected, if it still exists, instead of
    // resetting to "Saved…" every refresh (e.g. right after saving the
    // one just picked).
    if ([...select.options].some((o) => o.value === previous)) {
      select.value = previous;
    }
  }
}

for (const row of addressRows) {
  const select = document.getElementById(row.selectId);
  const deleteBtn = document.getElementById(row.deleteBtnId);

  select.addEventListener('change', (e) => {
    if (e.target.value) {
      document.getElementById(row.inputId).value = e.target.value;
    }
    deleteBtn.classList.toggle('visible', Boolean(e.target.value));
  });

  document.getElementById(row.saveBtnId).addEventListener('click', async () => {
    const hostPort = document.getElementById(row.inputId).value.trim();
    if (!hostPort) {
      alert('Enter a server address first.');
      return;
    }
    const label = prompt('Save this address as:', hostPort);
    if (!label) return; // cancelled
    try {
      await SaveServer(label, hostPort);
      await refreshSavedServers();
    } catch (e) {
      alert('Could not save address: ' + e);
    }
  });

  deleteBtn.addEventListener('click', async () => {
    const selected = select.options[select.selectedIndex];
    if (!selected || !selected.value) return;
    const label = selected.dataset.label;
    if (!confirm(`Remove saved address "${label}"?`)) return;
    try {
      await DeleteSavedServer(label);
      deleteBtn.classList.remove('visible');
      await refreshSavedServers();
    } catch (e) {
      alert('Could not delete address: ' + e);
    }
  });
}

refreshSavedServers();

document.getElementById('update-btn').addEventListener('click', async () => {
  if (!lastFormationIdle || !lastSharedCockpitIdle) {
    const proceed = confirm(
      'A Multiplayer or Shared Cockpit session looks active. Updating restarts this app and drops its ' +
        'connection to that session (the X-Plane plugin itself keeps running). Continue?',
    );
    if (!proceed) return;
  }
  const btn = document.getElementById('update-btn');
  btn.disabled = true;
  btn.textContent = 'Updating…';
  try {
    await ApplyUpdate();
    // On success the app restarts itself (companion/app.go's ApplyUpdate
    // relaunches and quits) - nothing left to do here.
  } catch (e) {
    btn.disabled = false;
    btn.textContent = 'Update & Restart';
    alert('Update failed: ' + e);
  }
});

document.getElementById('choose-xplane-btn').addEventListener('click', async () => {
  try {
    const result = await ChooseXPlanePath();
    if (!result) return; // user cancelled the folder picker
    document.getElementById('xplane-path').value = result.path;
    if (result.warning) {
      setSetupStatus(result.warning, 'err');
    } else {
      setSetupStatus('ready to install/update', '');
    }
    refreshVersions();
  } catch (e) {
    setSetupStatus(String(e), 'err');
  }
});

document.getElementById('install-plugin-btn').addEventListener('click', async () => {
  try {
    setSetupStatus('installing…', '');
    await InstallPlugin();
    setSetupStatus('installed — restart X-Plane to load it', 'ok');
    refreshVersions();
  } catch (e) {
    setSetupStatus(String(e), 'err');
  }
});

function setActiveRole(newRole) {
  role = newRole;
  document.getElementById('role-master').classList.toggle('active', role === 'MASTER');
  document.getElementById('role-client').classList.toggle('active', role === 'CLIENT');
}
document.getElementById('role-master').addEventListener('click', () => setActiveRole('MASTER'));
document.getElementById('role-client').addEventListener('click', () => setActiveRole('CLIENT'));

document.getElementById('create-btn').addEventListener('click', async () => {
  try {
    await CreateSession(document.getElementById('server').value, document.getElementById('formation-spectator').checked);
  } catch (e) {
    alert(e);
  }
});

document.getElementById('join-btn').addEventListener('click', async () => {
  try {
    await JoinSession(
      document.getElementById('server').value,
      document.getElementById('code').value,
      document.getElementById('formation-spectator').checked,
    );
  } catch (e) {
    alert(e);
  }
});

document.getElementById('start-sc-btn').addEventListener('click', async () => {
  try {
    await StartSharedCockpit(
      role,
      document.getElementById('sc-server').value,
      document.getElementById('sc-code').value,
    );
  } catch (e) {
    alert(e);
  }
});

// Stops RendezvousClient::on_disconnected's auto-reconnect (see
// control_listener.h's DISCONNECT_FORMATION/DISCONNECT_SHARED_COCKPIT) -
// without these, there was no way to actually leave a session short of
// quitting the whole companion app.
document.getElementById('disconnect-formation-btn').addEventListener('click', async () => {
  try {
    await DisconnectFormation();
  } catch (e) {
    alert(e);
  }
});

document.getElementById('disconnect-sc-btn').addEventListener('click', async () => {
  try {
    await DisconnectSharedCockpit();
  } catch (e) {
    alert(e);
  }
});

// See control_listener.h's RELOAD_CSL - the new model count arrives with
// the next status push (CSL_STATUS), not as this call's result.
// Held off for 5s after a click - the plugin ignores reloads closer
// together than that anyway (see plugin_main.cpp's ReloadCsl).
let cslReloadBlockedUntil = 0;
document.getElementById('reload-csl-btn').addEventListener('click', async () => {
  if (Date.now() < cslReloadBlockedUntil) return;
  cslReloadBlockedUntil = Date.now() + 5000;
  try {
    await ReloadCsl();
  } catch (e) {
    alert(e);
  }
});

// One click handler for all three ownership buttons - each carries its
// own category in data-category (see index.html), and is already
// disabled while it's owned by this side (see the status handler below),
// so a click here always means "take this category from the peer" -
// instantly, no permission round trip (see ownership_tracker.h's
// claim-and-tell model).
document.querySelectorAll('.ownership-toggle button').forEach((btn) => {
  btn.addEventListener('click', async () => {
    if (btn.dataset.category === 'flight' &&
        !confirm('Take the controls? You become the pilot flying (MASTER) and your co-pilot follows your aircraft.')) {
      return;
    }
    try {
      await ClaimOwnership(btn.dataset.category);
    } catch (e) {
      alert(e);
    }
  });
});

// Sets one sidebar nav dot's color from the same 'ok'/'err'/'' classify()
// kind used for the section's own status pill, or hides it entirely (idle,
// no session) - a dot only earns attention when there's something to see.
function setNavDot(id, kind) {
  const el = document.getElementById(id);
  el.classList.toggle('shown', Boolean(kind));
  el.classList.toggle('ok', kind === 'ok');
  el.classList.toggle('err', kind === 'err');
}

function classify(text) {
  if (/error|failed|invalid|lost/i.test(text)) return 'err';
  if (/connected|ready|running/i.test(text)) return 'ok';
  return '';
}

// The plugin's exact idle-state strings (control_listener.h's
// formation_status_/shared_cockpit_status_ defaults, and DISCONNECT_*'s
// reset) - anything else means there's an active or in-progress session
// worth offering to disconnect from, and Create/Join/Start should be
// blocked so a click can't start a second, competing session on top of
// it (including while auto-reconnecting after a connection loss).
function isIdle(text) {
  return text === 'not connected' || text === 'not started' || /plugin not seen yet/i.test(text);
}

// Renders the three ownership buttons from control_listener.h's
// SHARED_COCKPIT_OWNERSHIP map - one of "me"/"peer" per category (missing
// entirely before Shared Cockpit's dataref sync has started, treated the
// same as "peer"). Each button's own enabled state is handled separately
// below (gated on Shared Cockpit actually running, in addition to not
// already being owned) - this only sets how it looks. Clicking a "peer"
// button claims the category immediately (see the click handler above),
// no permission prompt to wait for.
function renderOwnership(ownership) {
  document.querySelectorAll('.ownership-toggle button').forEach((btn) => {
    const state = (ownership && ownership[btn.dataset.category]) || 'peer';
    const owned = state === 'me';
    btn.classList.toggle('owned', owned);
    if (btn.dataset.category === 'flight') {
      btn.textContent = owned ? 'You are flying' : 'Co-pilot is flying · take controls';
    } else {
      const label = btn.dataset.category.charAt(0).toUpperCase() + btn.dataset.category.slice(1);
      btn.textContent = owned ? `${label} · you` : `${label} · peer`;
    }
    btn.dataset.owned = owned ? '1' : ''; // nothing to click while already owned
  });
}

// One row per aircraft: callsign (or a fallback), type, and - for
// rendezvous peers - whether its packets arrive directly (P2P) or via the
// relay server, plus packet loss.
function renderPeerList(peers, linkQuality) {
  const el = document.getElementById('peer-list');
  if (!peers || peers.length === 0) {
    el.innerHTML = '<div class="empty">no peers connected</div>';
    return;
  }
  const paths = (linkQuality && linkQuality.formationPeerPath) || {};
  const loss = (linkQuality && linkQuality.formationPeerLossPct) || {};
  const items = peers
    .map((p) => {
      const name = p.callsign ? `<b>${escapeHtml(p.callsign)}</b>` : `Peer ${p.id}`;
      const path = paths[p.id];
      const pathBadge = path
        ? `<span class="badge ${path === 'direct' ? 'direct' : ''}" title="${path === 'direct' ? 'Peer-to-peer' : 'Through the rendezvous server'}">${path}</span>`
        : '';
      const lossText = loss[p.id] != null ? `<span>${loss[p.id]}% loss</span>` : '';
      return `<li><span class="peer-main">${name}</span><span class="peer-meta">${lossText}${pathBadge}<span class="icao">${escapeHtml(p.icao || 'unknown')}</span></span></li>`;
    })
    .join('');
  el.innerHTML = `<ul>${items}</ul>`;
}

// Renders control_listener.h's LINK_QUALITY reading for the Formation
// panel - server RTT plus each currently-visible peer's packet loss.
// Hidden entirely rather than shown with "?" placeholders when there's
// nothing to report yet (no session, or no reading has arrived), since a
// wall of unknowns isn't useful at a glance.
function renderFormationLinkQuality(linkQuality) {
  const el = document.getElementById('formation-link-quality');
  const rttKnown = linkQuality && linkQuality.formationServerRttMs != null;
  const lossByPeer = (linkQuality && linkQuality.formationPeerLossPct) || {};
  if (!rttKnown && Object.keys(lossByPeer).length === 0) {
    el.style.display = 'none';
    return;
  }
  el.style.display = '';
  const rttText = rttKnown ? `${linkQuality.formationServerRttMs} ms` : '? ms';
  // Per-peer loss and direct/relay are shown in the peer list itself.
  el.textContent = `Server RTT: ${rttText}`;
}

// Same idea as renderFormationLinkQuality, for the Shared Cockpit panel's
// server RTT + master-link loss.
function renderSharedCockpitLinkQuality(linkQuality) {
  const el = document.getElementById('sc-link-quality');
  const rttKnown = linkQuality && linkQuality.sharedCockpitServerRttMs != null;
  const lossKnown = linkQuality && linkQuality.sharedCockpitMasterLossPct != null;
  if (!rttKnown && !lossKnown) {
    el.style.display = 'none';
    return;
  }
  el.style.display = '';
  const rttText = rttKnown ? `${linkQuality.sharedCockpitServerRttMs} ms` : '? ms';
  const lossText = lossKnown ? `<span class="peer-loss">Master link: ${linkQuality.sharedCockpitMasterLossPct}% loss</span>` : '';
  const path = linkQuality && linkQuality.sharedCockpitPath;
  const pathText = path ? `<span class="peer-loss badge ${path === 'direct' ? 'direct' : ''}">${path}</span>` : '';
  el.innerHTML = `Server RTT: ${rttText}${lossText}${pathText}`;
}

// Renders control_listener.h's SHARED_COCKPIT_AIRCRAFT_MISMATCH - a
// client-side warning that the master is flying a different aircraft type
// than this side, so the active per-ICAO dataref profile is likely wrong
// for one of the two. `mismatch` is "<own icao>:<master icao>" or "".
function renderAircraftMismatch(mismatch) {
  const el = document.getElementById('sc-aircraft-mismatch');
  if (!mismatch) {
    el.classList.remove('visible');
    return;
  }
  const [own, master] = mismatch.split(':');
  el.textContent = `Aircraft mismatch: you're flying ${own || 'unknown'}, the master is flying ` +
    `${master || 'unknown'} - the Shared Cockpit dataref profile may not match your aircraft.`;
  el.classList.add('visible');
}

// Shared Cockpit desync report (SC_DESYNC): datarefs whose value differs
// between both cockpits for more than one check, or different profiles.
function renderDesync(desync) {
  const el = document.getElementById('sc-desync');
  if (!desync || (!desync.profilesDiffer && desync.items.length === 0)) {
    el.classList.remove('visible');
    return;
  }
  const text = document.getElementById('sc-desync-text');
  if (desync.profilesDiffer) {
    text.textContent = 'The two cockpits use different profiles for this aircraft - make sure both have the same one (Profiles page).';
    document.getElementById('sc-resync-btn').style.display = 'none';
  } else {
    text.textContent = `Out of sync with your co-pilot: ${desync.items.map((i) => `${i.name.split('/').pop()} (yours: ${i.value})`).join(', ')}`;
    document.getElementById('sc-resync-btn').style.display = '';
  }
  el.classList.add('visible');
}

// Airspace warning banner (companion/navdata.go's airspaceAlert): shown on
// every page while flying. Prohibited/restricted/danger areas in red, the
// rest (CTR, C, D - "needs a clearance") in amber.
const kAirspaceWarningsKey = 'xpmulticrew.airspaceWarnings';
const kAirspaceClassNames = { CTR: 'Control zone', A: 'Class A', B: 'Class B', C: 'Class C', D: 'Class D',
  P: 'Prohibited area', R: 'Restricted area', Q: 'Danger area' };
function airspaceWarningsEnabled() {
  try {
    return localStorage.getItem(kAirspaceWarningsKey) !== '0';
  } catch (e) {
    return true;
  }
}
document.getElementById('airspace-warnings').checked = airspaceWarningsEnabled();
document.getElementById('airspace-warnings').addEventListener('change', (e) => {
  try {
    localStorage.setItem(kAirspaceWarningsKey, e.target.checked ? '1' : '0');
  } catch (err) {
    // per-viewer convenience only
  }
});

function describeAirspace(a) {
  return `${a.name} (${kAirspaceClassNames[a.class] || a.class}, ${a.lower}–${a.upper})`;
}

function renderAirspaceBanner(alert) {
  const el = document.getElementById('airspace-banner');
  const inside = (alert && alert.inside) || [];
  const ahead = alert && alert.ahead;
  if (!airspaceWarningsEnabled() || (inside.length === 0 && !ahead)) {
    el.classList.remove('visible');
    return;
  }
  const parts = [];
  if (inside.length) parts.push(`In: ${inside.map(describeAirspace).join(', ')}`);
  if (ahead) {
    const m = Math.floor(ahead.etaSecs / 60);
    const s = String(ahead.etaSecs % 60).padStart(2, '0');
    parts.push(`Ahead in ${m}:${s}: ${describeAirspace(ahead)}`);
  }
  const danger = [...inside, ...(ahead ? [ahead] : [])].some((a) => ['P', 'R', 'Q'].includes(a.class));
  el.textContent = '⚠ ' + parts.join(' · ');
  el.classList.toggle('danger', danger);
  el.classList.add('visible');
}

// Route progress (route-progress.js): next waypoint, course, distance and
// ETA while flying the route planned on the Map page - on every page.
function formatMin(min) {
  const total = Math.round(min);
  return `${Math.floor(total / 60)}:${String(total % 60).padStart(2, '0')}`;
}

function renderRouteBanner(self) {
  const el = document.getElementById('route-banner');
  const moving = self && self.groundspeedKt > 30;
  const progress = self ? updateProgress(self) : null;
  if (!progress || !moving) {
    el.classList.remove('visible');
    return;
  }
  const w = progress.route.waypoints;
  const nextName = progress.next.kind === 'USR' ? progress.next.name || `WPT${progress.active + 1}` : progress.next.ident;
  const dest = w[w.length - 1];
  el.textContent = progress.arrived
    ? `✓ Arrived at ${dest.ident || 'the last waypoint'}`
    : `▶ ${nextName} · MC ${String(Math.round(progress.magCourse)).padStart(3, '0')}° · ${progress.distNm.toFixed(1)} NM · ` +
      `${progress.next.altFt ? `max ${progress.next.altFt} ft · ` : ''}` +
      `ETA ${formatMin(progress.etaMin)}   —   ${progress.remainingNm.toFixed(0)} NM / ${formatMin(progress.remainingMin)} to ${dest.ident || 'the end'}`;
  el.classList.add('visible');
}

// Only one plugin can own X-Plane's TCAS/AI planes. When another one has
// them (typically LiveTraffic), peers are still drawn but missing from
// TCAS and X-Plane's own map - say so instead of leaving it to Log.txt.
function renderTcasStatus(status) {
  const el = document.getElementById('tcas-warning');
  if (!status || !status.startsWith('blocked:')) {
    el.classList.remove('visible');
    return;
  }
  const owner = status.slice('blocked:'.length) || 'another plugin';
  el.textContent = `TCAS is controlled by ${owner}: other pilots are shown in the sim, but not on TCAS or ` +
    `X-Plane's map. Install the "XPMP2 Remote Client" plugin to see both on TCAS. XPMultiCrew takes TCAS ` +
    `over automatically as soon as ${owner} releases it.`;
  el.classList.add('visible');
}

// Only overwrites the code field when the plugin actually has a code for
// us (i.e. we're the one who just created or joined that session) and the
// field doesn't already show it - avoids fighting with someone mid-typing
// a different code into an unrelated field.
function fillCodeFieldIfEmpty(inputId, code) {
  const el = document.getElementById(inputId);
  if (code && el.value !== code) {
    el.value = code;
  }
}

// The Go side (app.go's pollStatus) emits this every second - no HTTP
// polling loop needed here, and the event is push-based so status updates
// show up immediately rather than up to 2s late.
EventsOn('status', (data) => {
  // Approach Coach: the plugin owns the setting (it's also switchable from
  // X-Plane's menu) - just mirror it; null = no plugin that has one.
  const coach = document.getElementById('pref-approach-coach');
  const coachKnown = data.approachCoach !== null && data.approachCoach !== undefined;
  coach.disabled = !coachKnown;
  if (coachKnown && document.activeElement !== coach) coach.checked = data.approachCoach;
  document.getElementById('approach-coach-hint').style.display = coachKnown ? 'none' : '';

  const formationKind = classify(data.formation);
  const sharedCockpitKind = classify(data.sharedCockpit);

  const fEl = document.getElementById('formation-status');
  fEl.className = 'status ' + formationKind;
  document.getElementById('formation-status-text').textContent = data.formation;

  const sEl = document.getElementById('sc-status');
  sEl.className = 'status ' + sharedCockpitKind;
  document.getElementById('sc-status-text').textContent = data.sharedCockpit;

  // Sidebar nav dots - so a session's ok/error state is still visible at a
  // glance while looking at a different page (e.g. Diagnostics), the way
  // it used to be when both panels were always on screen together.
  setNavDot('nav-dot-formation', formationKind);
  setNavDot('nav-dot-shared-cockpit', sharedCockpitKind);

  fillCodeFieldIfEmpty('code', data.formationCode);
  fillCodeFieldIfEmpty('sc-code', data.sharedCockpitCode);

  renderPeerList(data.peers, data.linkQuality);
  renderCurrentAircraftButton(data.ownIcao);
  lastStatusForMap = data;
  if (mapModule) mapModule.updateMap(data);
  renderOwnership(data.sharedCockpitOwnership);
  renderFormationLinkQuality(data.linkQuality);
  renderSharedCockpitLinkQuality(data.linkQuality);
  renderAircraftMismatch(data.sharedCockpitMismatch);

  document.getElementById('loading-banner').style.display = data.simReady ? 'none' : '';

  // Only shown once the plugin has actually been seen (X-Plane running) -
  // can legitimately differ from "Installed" right after an update, since
  // that only takes effect on X-Plane's next restart.
  const runningRow = document.getElementById('running-version-row');
  if (data.runningVersion) {
    runningRow.style.display = '';
    document.getElementById('running-version').textContent = data.runningVersion;
  } else {
    runningRow.style.display = 'none';
  }

  // Blocked while X-Plane is still loading (a click would otherwise sit
  // unprocessed until loading finishes anyway, but blocking here is more
  // honest about that wait) and once already connected/connecting/
  // reconnecting, so you can't accidentally kick off a second Create/
  // Join/Start on top of a session that's still active or being restored.
  const formationIdle = isIdle(data.formation);
  const sharedCockpitIdle = isIdle(data.sharedCockpit);
  lastFormationIdle = formationIdle;
  lastSharedCockpitIdle = sharedCockpitIdle;
  document.getElementById('create-btn').disabled = !data.simReady || !formationIdle;
  document.getElementById('join-btn').disabled = !data.simReady || !formationIdle;
  document.getElementById('start-sc-btn').disabled = !data.simReady || !sharedCockpitIdle;
  document.getElementById('disconnect-formation-btn').disabled = formationIdle;
  document.getElementById('disconnect-sc-btn').disabled = sharedCockpitIdle;
  document.getElementById('csl-status').textContent = data.cslStatus || '—';
  renderTcasStatus(data.tcasStatus);
  renderAirspaceBanner(data.airspaceAlert);
  renderDesync(data.scDesync);
  recordTracks(data);
  updateChecklists(data);
  updateLandings(data, document.getElementById('page-landings').classList.contains('active'));
  updateAirports(data, document.getElementById('page-airports').classList.contains('active'));
  lastStatusForDebrief = data;
  renderDebriefDot(data);
  if (debriefModule) debriefModule.updateDebrief(data, document.getElementById('page-debrief').classList.contains('active'));
  renderRouteBanner(data.selfPos);
  document.getElementById('profile-learn-btn').disabled = !data.simReady;
  renderLearn(data.learn);
  document.getElementById('reload-csl-btn').disabled = !data.simReady || Date.now() < cslReloadBlockedUntil;
  document.getElementById('sync-env-btn').disabled = !data.simReady;

  // Only meaningful while a Shared Cockpit session is actually running,
  // and pointless to click on a category already owned by this side (see
  // renderOwnership's data-owned).
  document.querySelectorAll('.ownership-toggle button').forEach((btn) => {
    btn.disabled = sharedCockpitIdle || btn.dataset.owned === '1';
  });
});

// Diagnostics page - the plugin's own lines from X-Plane's Log.txt (see
// companion/diagnostics.go), polled every second the same way
// pollStatus() polls plugin status, not pushed. Polls unconditionally
// (cheap - opening and reading a bit of one file once a second) even
// while a different page is open, so everything captured so far is
// already there the moment you switch to Diagnostics, not just lines from
// that point on.
const kMaxDiagnosticsLines = 500;
let diagnosticsOffset = 0;
let diagnosticsLines = [];

function renderDiagnosticsLog() {
  const el = document.getElementById('diagnostics-log');
  const wasNearBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 40;
  if (diagnosticsLines.length === 0) {
    el.innerHTML = '<span class="empty">No plugin log lines yet.</span>';
  } else {
    el.textContent = diagnosticsLines.join('\n');
  }
  if (wasNearBottom) {
    el.scrollTop = el.scrollHeight;
  }
}

async function pollDiagnostics() {
  try {
    const result = await GetRecentLogLines(diagnosticsOffset);
    diagnosticsOffset = result.offset;
    if (result.lines && result.lines.length > 0) {
      diagnosticsLines.push(...result.lines);
      if (diagnosticsLines.length > kMaxDiagnosticsLines) {
        diagnosticsLines = diagnosticsLines.slice(diagnosticsLines.length - kMaxDiagnosticsLines);
      }
      renderDiagnosticsLog();
    }
  } catch (e) {
    // Best-effort - a transient read failure (e.g. X-Plane mid-restart,
    // rewriting Log.txt) shouldn't spam the user with an alert.
  }
}
setInterval(pollDiagnostics, 1000);
pollDiagnostics();

document.getElementById('diagnostics-copy-btn').addEventListener('click', async () => {
  try {
    await navigator.clipboard.writeText(diagnosticsLines.join('\n'));
  } catch (e) {
    alert('Could not copy to clipboard: ' + e);
  }
});

// --- Preferences panel (Setup page): callsign, labels, time & weather sync (companion
// settings pushed to the plugin, see app.go's SetPrefs) ---
function applyPrefsToForm(prefs) {
  document.getElementById('callsign').value = prefs.callsign || '';
  document.getElementById('pref-labels').checked = prefs.showLabels;
  document.getElementById('pref-env-sync').checked = prefs.envSync;
  document.getElementById('pref-right-seat').checked = prefs.rightSeat;
  document.getElementById('pref-direct-p2p').checked = prefs.directP2P;
  document.getElementById('pref-debug-log').checked = prefs.debugLog;
}

async function savePrefs() {
  try {
    const prefs = await SetPrefs(
      document.getElementById('callsign').value,
      document.getElementById('pref-labels').checked,
      document.getElementById('pref-env-sync').checked,
      document.getElementById('pref-right-seat').checked,
      document.getElementById('pref-direct-p2p').checked,
      document.getElementById('pref-debug-log').checked,
    );
    applyPrefsToForm(prefs);
  } catch (e) {
    alert('Could not save settings: ' + e);
  }
}

GetPrefs().then(applyPrefsToForm);
document.getElementById('callsign-save-btn').addEventListener('click', savePrefs);
document.getElementById('callsign').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') savePrefs();
});
document.getElementById('pref-labels').addEventListener('change', savePrefs);
document.getElementById('pref-env-sync').addEventListener('change', savePrefs);
document.getElementById('pref-right-seat').addEventListener('change', savePrefs);
document.getElementById('pref-direct-p2p').addEventListener('change', savePrefs);
document.getElementById('pref-debug-log').addEventListener('change', savePrefs);
// See control_listener.h's SYNC_ENV - the weather follows on the plugin's
// next poll, the time with the host's next broadcast (up to 10 s).
document.getElementById('sync-env-btn').addEventListener('click', () => SyncEnv().catch(alert));
document.getElementById('pref-approach-coach').addEventListener('change', (e) => {
  SetApproachCoach(e.target.checked).catch(alert);
});
document.getElementById('sc-resync-btn').addEventListener('click', () => ScResync().catch(alert));

// --- Profiles page: Shared Cockpit dataref profile editor (see
// companion/profiles.go) ---
const kCategories = ['systems', 'engine', 'avionics'];
let currentProfile = null; // ProfileData from the Go side

function setProfileStatus(text, kind) {
  const el = document.getElementById('profile-status');
  el.className = 'status ' + (kind || '');
  document.getElementById('profile-status-text').textContent = text;
}

function describeSource(profile) {
  if (profile.unsaved) {
    return `New profile for ${profile.icao} - not saved yet.`;
  }
  switch (profile.source) {
    case 'user':
      return `Your profile for ${profile.icao} (overrides the bundled one, if any).`;
    case 'bundled':
      return `Bundled profile for ${profile.icao} - saving creates your own copy.`;
    default:
      return `No profile for ${profile.icao} yet - add datarefs and save.`;
  }
}

function profileRowHtml(entry, index) {
  const options = kCategories
    .map((c) => `<option value="${c}" ${entry.category === c ? 'selected' : ''}>${c}</option>`)
    .join('');
  const warn = entry.warning ? escapeHtml(entry.warning) : '';
  const isCommand = entry.kind === 'command';
  return `<tr data-index="${index}">
    <td><select class="p-kind" title="Value: kept in sync. Button: pressing it presses it in the other cockpit too.">
      <option value="dataref" ${isCommand ? '' : 'selected'}>Value</option>
      <option value="command" ${isCommand ? 'selected' : ''}>Button</option></select></td>
    <td><input type="text" class="p-name ${warn ? 'has-warning' : ''}" value="${escapeHtml(entry.name)}" placeholder="${isCommand ? 'sim/autopilot/...' : 'sim/cockpit2/...'}" title="${warn}"></td>
    <td><select class="p-category">${options}</select></td>
    <td style="text-align: center;"><input type="checkbox" class="p-stream" ${entry.stream && !isCommand ? 'checked' : ''} ${isCommand ? 'disabled' : ''}></td>
    <td><button class="row-delete" type="button" title="Remove">✕</button></td>
  </tr>${warn ? `<tr><td colspan="5" class="warn">${warn}</td></tr>` : ''}`;
}

function readProfileRows() {
  return Array.from(document.querySelectorAll('#profile-rows tr[data-index]')).map((tr) => ({
    kind: tr.querySelector('.p-kind').value,
    name: tr.querySelector('.p-name').value.trim(),
    category: tr.querySelector('.p-category').value,
    stream: tr.querySelector('.p-stream').checked,
  }));
}

function renderProfile(profile) {
  currentProfile = profile;
  document.getElementById('profile-editor').style.display = '';
  document.getElementById('profile-icao').value = profile.icao;
  const rows = document.getElementById('profile-rows');
  rows.innerHTML = profile.entries.map(profileRowHtml).join('');
  rows.querySelectorAll('.row-delete').forEach((btn) => {
    btn.addEventListener('click', () => {
      const entries = readProfileRows();
      entries.splice(Number(btn.closest('tr').dataset.index), 1);
      renderProfile({ ...currentProfile, entries });
    });
  });
  const warnings = profile.entries.filter((e) => e.warning).length;
  let status = describeSource(profile) + ` ${profile.entries.length} dataref(s).`;
  if (!profile.validated) status += ' (Could not read X-Plane\'s DataRefs.txt to check names.)';
  else if (warnings) status += ` ${warnings} need a look.`;
  setProfileStatus(status, warnings ? 'err' : profile.source === 'user' ? 'ok' : '');
  document.getElementById('profile-revert-btn').disabled = profile.source !== 'user';
}

let knownProfiles = [];

async function refreshProfileList(selectIcao) {
  const select = document.getElementById('profile-select');
  try {
    const list = await ListProfiles();
    knownProfiles = list;
    select.innerHTML = '<option value="">Profiles…</option>' +
      list
        .map((p) => `<option value="${escapeHtml(p.icao)}">${escapeHtml(p.icao)}${p.hasUser ? ' (yours)' : ' (bundled)'}</option>`)
        .join('');
    if (selectIcao) select.value = selectIcao;
  } catch (e) {
    select.innerHTML = `<option value="">${escapeHtml(String(e))}</option>`;
  }
}

async function openProfile(icao) {
  if (!icao) return;
  try {
    renderProfile(await LoadProfile(icao));
    document.getElementById('profile-select').value = currentProfile.icao;
  } catch (e) {
    alert(e);
  }
}

// Offers a one-click "open the profile for the aircraft you're sitting in",
// using the type the plugin reports (OWN_ICAO).
let lastOwnIcao = '';
function renderCurrentAircraftButton(ownIcao) {
  if (ownIcao === lastOwnIcao) return;
  lastOwnIcao = ownIcao || '';
  const btn = document.getElementById('profile-current-btn');
  btn.style.display = lastOwnIcao ? '' : 'none';
  btn.textContent = `Current aircraft: ${lastOwnIcao}`;
}

document.getElementById('profile-current-btn').addEventListener('click', () => openProfile(lastOwnIcao));

// "New profile…": ICAO (prefilled with the current aircraft) plus an
// optional existing profile to copy as a starting point. Nothing is
// written until "Save my profile".
function showNewProfileForm(show) {
  document.getElementById('profile-new-form').style.display = show ? '' : 'none';
  if (!show) return;
  const template = document.getElementById('profile-new-template');
  template.innerHTML = '<option value="">Start empty</option>' +
    knownProfiles.map((p) => `<option value="${escapeHtml(p.icao)}">Copy of ${escapeHtml(p.icao)}</option>`).join('');
  const icao = document.getElementById('profile-new-icao');
  icao.value = lastOwnIcao && !knownProfiles.some((p) => p.icao === lastOwnIcao) ? lastOwnIcao : '';
  icao.focus();
}

async function createNewProfile() {
  const icao = document.getElementById('profile-new-icao').value.trim().toUpperCase();
  if (!/^[A-Z0-9]{2,8}$/.test(icao)) {
    alert('Enter the aircraft\'s ICAO type (2-8 letters/digits, e.g. C172).');
    return;
  }
  const existing = knownProfiles.find((p) => p.icao === icao);
  if (existing && existing.hasUser) {
    alert(`You already have a profile for ${icao} - it's opened for editing instead.`);
    showNewProfileForm(false);
    openProfile(icao);
    return;
  }
  const templateIcao = document.getElementById('profile-new-template').value;
  let entries = [];
  let validated;
  try {
    if (templateIcao) {
      const template = await LoadProfile(templateIcao);
      entries = template.entries;
      validated = template.validated;
    } else {
      validated = (await LoadProfile(icao)).validated;
    }
  } catch (e) {
    alert(e);
    return;
  }
  showNewProfileForm(false);
  renderProfile({ icao, source: existing ? 'bundled' : 'new', entries, validated, unsaved: true });
}

document.getElementById('profile-new-btn').addEventListener('click', async () => {
  await refreshProfileList();
  showNewProfileForm(true);
});
document.getElementById('profile-new-cancel').addEventListener('click', () => showNewProfileForm(false));
document.getElementById('profile-new-create').addEventListener('click', createNewProfile);
document.getElementById('profile-new-icao').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') createNewProfile();
});
document.getElementById('profile-select').addEventListener('change', (e) => openProfile(e.target.value));
document.getElementById('profile-open-btn').addEventListener('click', () =>
  openProfile(document.getElementById('profile-icao').value));
document.getElementById('profile-icao').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') openProfile(e.target.value);
});
document.getElementById('profile-add-btn').addEventListener('click', () => {
  if (!currentProfile) return;
  const entries = readProfileRows();
  entries.push({ kind: 'dataref', name: '', category: 'systems', stream: false });
  renderProfile({ ...currentProfile, entries });
  const inputs = document.querySelectorAll('#profile-rows .p-name');
  inputs[inputs.length - 1].focus();
});
document.getElementById('profile-save-btn').addEventListener('click', async () => {
  if (!currentProfile) return;
  try {
    const saved = await SaveProfile(currentProfile.icao, readProfileRows());
    renderProfile(saved);
    refreshProfileList(saved.icao);
  } catch (e) {
    alert('Could not save: ' + e);
  }
});
document.getElementById('profile-revert-btn').addEventListener('click', async () => {
  if (!currentProfile || currentProfile.source !== 'user') return;
  if (!confirm(`Delete your ${currentProfile.icao} profile? The bundled one (if any) applies again.`)) return;
  try {
    await DeleteUserProfile(currentProfile.icao);
    await refreshProfileList();
    openProfile(currentProfile.icao);
  } catch (e) {
    alert(e);
  }
});

// --- Dataref suggestions while typing (companion/profiles.go's
// searchDatarefs: name or description, writable only) ---
const suggestBox = document.getElementById('dataref-suggest');
let suggestInput = null;
let suggestItems = [];
let suggestActive = -1;
let suggestTimer = null;
let suggestSeq = 0;

function hideSuggestions() {
  suggestBox.style.display = 'none';
  suggestItems = [];
  suggestActive = -1;
}

function pickSuggestion(item) {
  if (!suggestInput) return;
  suggestInput.value = item.name;
  suggestInput.classList.remove('has-warning');
  const row = suggestInput.closest('tr');
  const category = row && row.querySelector('.p-category');
  if (category && item.category) category.value = item.category;
  const kind = row && row.querySelector('.p-kind');
  if (kind && item.kind) {
    kind.value = item.kind;
    row.querySelector('.p-stream').disabled = item.kind === 'command';
    if (item.kind === 'command') row.querySelector('.p-stream').checked = false;
  }
  hideSuggestions();
}

function renderSuggestions() {
  if (!suggestInput || suggestItems.length === 0) {
    hideSuggestions();
    return;
  }
  suggestBox.innerHTML = suggestItems
    .map((s, i) => `<div data-i="${i}" class="${i === suggestActive ? 'active' : ''}">
      <span class="s-cat">${s.kind === 'command' ? 'button · ' : ''}${escapeHtml(s.category)}</span>
      <div class="s-name">${escapeHtml(s.name)}</div>
      ${s.description ? `<div class="s-desc">${escapeHtml(s.description)}${s.units ? ` (${escapeHtml(s.units)})` : ''}</div>` : ''}
    </div>`)
    .join('');
  const rect = suggestInput.getBoundingClientRect();
  suggestBox.style.left = `${rect.left}px`;
  suggestBox.style.top = `${rect.bottom + 2}px`;
  suggestBox.style.width = `${Math.max(rect.width, 420)}px`;
  suggestBox.style.display = 'block';
}

suggestBox.addEventListener('mousedown', (e) => {
  const el = e.target.closest('[data-i]');
  if (!el) return;
  e.preventDefault(); // keep focus in the input
  pickSuggestion(suggestItems[Number(el.dataset.i)]);
});

document.getElementById('profile-rows').addEventListener('input', (e) => {
  if (!e.target.classList.contains('p-name')) return;
  suggestInput = e.target;
  clearTimeout(suggestTimer);
  const query = e.target.value.trim();
  if (query.length < 2 || query.includes('/') && query.length > 60) {
    hideSuggestions();
    return;
  }
  suggestTimer = setTimeout(async () => {
    const seq = ++suggestSeq;
    const results = await SearchDatarefs(query).catch(() => []);
    if (seq !== suggestSeq || suggestInput !== e.target) return; // a newer keystroke won
    suggestItems = results || [];
    suggestActive = -1;
    renderSuggestions();
  }, 150);
});

// Switching a row between Value and Button: STREAM only applies to values.
document.getElementById('profile-rows').addEventListener('change', (e) => {
  if (!e.target.classList.contains('p-kind')) return;
  const stream = e.target.closest('tr').querySelector('.p-stream');
  stream.disabled = e.target.value === 'command';
  if (stream.disabled) stream.checked = false;
});

document.getElementById('profile-rows').addEventListener('keydown', (e) => {
  if (!e.target.classList.contains('p-name') || suggestBox.style.display !== 'block') return;
  if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
    e.preventDefault();
    const n = suggestItems.length;
    suggestActive = (suggestActive + (e.key === 'ArrowDown' ? 1 : -1) + n) % n;
    renderSuggestions();
  } else if (e.key === 'Enter' && suggestActive >= 0) {
    e.preventDefault();
    pickSuggestion(suggestItems[suggestActive]);
  } else if (e.key === 'Escape') {
    hideSuggestions();
  }
});

document.getElementById('profile-rows').addEventListener('focusout', () => setTimeout(hideSuggestions, 150));
document.querySelector('.content').addEventListener('scroll', hideSuggestions);

// --- "Learn from cockpit" (plugin's LEARN_START/LEARN_STOP, see
// shared_cockpit/dataref_learner.h) ---
let learnPanelOpen = false;
let lastLearn = null;
const learnUnchecked = new Set(); // names the user unticked
const learnCategory = new Map(); // name -> category the user picked

function profileNames() {
  return new Set(readProfileRows().map((r) => r.name));
}

function renderLearn(learn) {
  lastLearn = learn;
  const panel = document.getElementById('learn-panel');
  if (!learn || (!learnPanelOpen && learn.state === 'idle')) {
    panel.style.display = 'none';
    return;
  }
  learnPanelOpen = true;
  panel.style.display = '';
  const status = document.getElementById('learn-status');
  if (learn.state === 'baseline') {
    status.textContent = `Hands off for a moment - checking which of ${learn.candidates} datarefs change on their own…`;
  } else if (learn.state === 'watching') {
    status.textContent = `Now flip the switches, turn the knobs and press the buttons you want to keep in sync. ` +
      `(${learn.candidates} datarefs watched, ${learn.noisy} ignored because they move by themselves)`;
  } else {
    status.textContent = learn.changes.length ? 'Learning stopped. Tick what you want and add it to the profile.'
      : 'Learning stopped - nothing changed.';
  }
  document.getElementById('learn-stop-btn').style.display = learn.state === 'idle' ? 'none' : '';

  // Don't rebuild the list under the user's cursor if nothing changed.
  const list = document.getElementById('learn-list');
  const signature = JSON.stringify(learn.changes);
  if (list.dataset.signature === signature) return;
  list.dataset.signature = signature;
  const known = profileNames();
  list.innerHTML = learn.changes
    .map((c) => {
      const inProfile = known.has(c.name);
      const category = learnCategory.get(c.name) || c.category;
      const options = kCategories
        .map((k) => `<option value="${k}" ${k === category ? 'selected' : ''}>${k}</option>`)
        .join('');
      return `<li class="${inProfile ? 'known' : ''}" data-name="${escapeHtml(c.name)}">
        <input type="checkbox" ${inProfile ? 'disabled' : learnUnchecked.has(c.name) ? '' : 'checked'}>
        <div class="l-main">
          <div class="l-name">${escapeHtml(c.name)}</div>
          <div class="l-desc">${c.kind === 'command' ? `button, pressed ${escapeHtml(c.after)}×` : `${escapeHtml(c.before)} → ${escapeHtml(c.after)}`}${c.description ? ` · ${escapeHtml(c.description)}` : ''}${inProfile ? ' · already in the profile' : ''}</div>
        </div>
        <select ${inProfile ? 'disabled' : ''}>${options}</select>
      </li>`;
    })
    .join('');
  list.querySelectorAll('li').forEach((li) => {
    const name = li.dataset.name;
    li.querySelector('input').addEventListener('change', (e) => {
      if (e.target.checked) learnUnchecked.delete(name);
      else learnUnchecked.add(name);
    });
    li.querySelector('select').addEventListener('change', (e) => learnCategory.set(name, e.target.value));
  });
}

document.getElementById('profile-learn-btn').addEventListener('click', async () => {
  if (!currentProfile) return;
  learnUnchecked.clear();
  learnCategory.clear();
  learnPanelOpen = true;
  document.getElementById('learn-list').dataset.signature = '';
  try {
    await StartLearn();
  } catch (e) {
    alert(e);
  }
});
document.getElementById('learn-stop-btn').addEventListener('click', () => StopLearn().catch(alert));
document.getElementById('learn-close-btn').addEventListener('click', () => {
  if (lastLearn && lastLearn.state !== 'idle') StopLearn().catch(() => {});
  learnPanelOpen = false;
  document.getElementById('learn-panel').style.display = 'none';
});
document.getElementById('learn-add-btn').addEventListener('click', () => {
  if (!currentProfile || !lastLearn) return;
  const entries = readProfileRows().filter((r) => r.name);
  const known = new Set(entries.map((r) => r.name));
  // (a dataref and a command never share a name, so the name alone is unique)
  let added = 0;
  for (const c of lastLearn.changes) {
    if (known.has(c.name) || learnUnchecked.has(c.name)) continue;
    entries.push({ kind: c.kind || 'dataref', name: c.name, category: learnCategory.get(c.name) || c.category, stream: false });
    known.add(c.name);
    added++;
  }
  renderProfile({ ...currentProfile, entries, unsaved: currentProfile.unsaved || added > 0 });
  document.getElementById('learn-list').dataset.signature = ''; // re-mark "already in the profile"
  renderLearn(lastLearn);
  if (added) setProfileStatus(`${added} dataref(s) added - review them and click "Save my profile".`, 'ok');
});
