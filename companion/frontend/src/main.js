import {
  ApplyUpdate,
  CheckForUpdate,
  ChooseXPlanePath,
  ClaimOwnership,
  CreateSession,
  DeleteSavedServer,
  DisconnectFormation,
  DisconnectSharedCockpit,
  DiscoverLanPeers,
  GetAvailablePluginVersion,
  GetInstalledPluginVersion,
  GetRecentLogLines,
  GetSavedServers,
  GetXPlanePath,
  InstallPlugin,
  JoinSession,
  LanConnectFormation,
  SaveServer,
  StartSharedCockpit,
} from '../wailsjs/go/main/App';
import { EventsOn } from '../wailsjs/runtime/runtime';

// Sidebar navigation - one .page shown at a time (see index.html's
// .app-layout comment for why this replaced one long stacked page).
// Persists the last-open page in localStorage purely as a per-viewer
// convenience (wrapped in try/catch: private windows/blocked storage must
// still leave the app usable, just without remembering the choice) - never
// anything read back by Go or shared between machines.
const kLastPageStorageKey = 'xpmulticrew.lastPage';

function showPage(page) {
  document.querySelectorAll('.sidebar button').forEach((btn) => {
    btn.classList.toggle('active', btn.dataset.page === page);
  });
  document.querySelectorAll('.page').forEach((el) => {
    el.classList.toggle('active', el.id === `page-${page}`);
  });
  try {
    localStorage.setItem(kLastPageStorageKey, page);
  } catch (e) {
    // Ignore - see kLastPageStorageKey's comment.
  }
}

document.querySelectorAll('.sidebar button').forEach((btn) => {
  btn.addEventListener('click', () => showPage(btn.dataset.page));
});

// 'setup' as the fallback (not 'formation', despite it being first in the
// sidebar - see index.html's comment) since a genuinely first-ever launch
// needs the X-Plane path chosen and the plugin installed before Formation/
// Shared Cockpit are even usable. Once there's a stored page (i.e. every
// launch after the first), that takes over instead - see below.
let initialPage = 'setup';
try {
  const stored = localStorage.getItem(kLastPageStorageKey);
  if (stored && document.getElementById(`page-${stored}`)) {
    initialPage = stored;
  }
} catch (e) {
  // Ignore - see kLastPageStorageKey's comment.
}
showPage(initialPage);

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
  const [installed, available] = await Promise.all([GetInstalledPluginVersion(), GetAvailablePluginVersion()]);
  document.getElementById('installed-version').textContent = installed || 'not installed';
  document.getElementById('available-version').textContent = available;

  const infoEl = document.getElementById('version-info');
  const updateAvailable = installed && installed !== available;
  infoEl.classList.toggle('update-available', Boolean(updateAvailable));
}

document.getElementById('xplane-path').value = '';
GetXPlanePath().then((path) => {
  if (path) {
    document.getElementById('xplane-path').value = path;
    setSetupStatus('ready to install/update', '');
  }
  refreshVersions();
});

// Checked once on load, not on the 1s status-poll ticker - see
// App.CheckForUpdate's comment (companion/app.go). Resolves to null both
// when already up to date and for a local dev build.
CheckForUpdate().then((info) => {
  if (!info) return;
  document.getElementById('update-banner-text').textContent = `Update available: ${info.version}`;
  document.getElementById('update-banner').style.display = '';
});

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

// One click handler for all three ownership buttons - each carries its
// own category in data-category (see index.html), and is already
// disabled while it's owned by this side (see the status handler below),
// so a click here always means "take this category from the peer" -
// instantly, no permission round trip (see ownership_tracker.h's
// claim-and-tell model).
document.querySelectorAll('.ownership-toggle button').forEach((btn) => {
  btn.addEventListener('click', async () => {
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
    const label = btn.dataset.category.charAt(0).toUpperCase() + btn.dataset.category.slice(1);
    btn.textContent = owned ? `${label} · you` : `${label} · peer`;
    btn.dataset.owned = owned ? '1' : ''; // nothing to click while already owned
  });
}

function renderPeerList(peers) {
  const el = document.getElementById('peer-list');
  if (!peers || peers.length === 0) {
    el.innerHTML = '<div class="empty">no peers connected</div>';
    return;
  }
  const items = peers
    .map((p) => `<li><span>Peer ${p.id}</span><span class="icao">${p.icao || 'unknown'}</span></li>`)
    .join('');
  el.innerHTML = `<ul>${items}</ul>`;
}

// Renders app.go's DiscoverLanPeers result as the "Nearby on LAN" list -
// each entry gets its own Connect button, sending the code currently typed
// into #lan-code (a separate, manually-agreed secret from the rendezvous
// session codes above - see app.go's LanConnectFormation).
function renderLanPeers(peers) {
  const el = document.getElementById('lan-peer-list');
  if (!peers || peers.length === 0) {
    el.innerHTML = '<div class="empty">no XPMultiCrew instances found nearby</div>';
    return;
  }
  const items = peers
    .map((p) => {
      const hostPort = `${p.host}:${p.port}`;
      return (
        `<li><span>${p.name} <span class="addr">(${hostPort})</span></span>` +
        `<button class="secondary lan-connect-btn" data-host-port="${hostPort}">Connect</button></li>`
      );
    })
    .join('');
  el.innerHTML = `<ul>${items}</ul>`;
  el.querySelectorAll('.lan-connect-btn').forEach((btn) => {
    btn.addEventListener('click', () => connectLanPeer(btn.dataset.hostPort));
  });
}

async function connectLanPeer(hostPort) {
  const code = document.getElementById('lan-code').value.trim();
  if (!code) {
    alert('Enter the session code you agreed with your LAN peer first.');
    return;
  }
  try {
    await LanConnectFormation(hostPort, code);
  } catch (e) {
    alert(e);
  }
}

// mDNS discovery itself takes a couple of seconds (see
// companion/lan_discovery.go's discoverLanPeers timeout) - a background
// poll rather than gating it on the Refresh click alone, so the list stays
// current without the user needing to keep clicking it, same "cheap, poll
// unconditionally" reasoning as pollDiagnostics below.
async function refreshLanPeers() {
  try {
    renderLanPeers(await DiscoverLanPeers());
  } catch (e) {
    // Best-effort - a transient mDNS failure shouldn't spam an alert.
  }
}
document.getElementById('lan-refresh-btn').addEventListener('click', refreshLanPeers);
setInterval(refreshLanPeers, 10000);
refreshLanPeers();

// Renders control_listener.h's LINK_QUALITY reading for the Formation
// panel - server RTT plus each currently-visible peer's packet loss.
// Hidden entirely rather than shown with "?" placeholders when there's
// nothing to report yet (no session, or no reading has arrived), since a
// wall of unknowns isn't useful at a glance.
function renderFormationLinkQuality(linkQuality, peers) {
  const el = document.getElementById('formation-link-quality');
  const rttKnown = linkQuality && linkQuality.formationServerRttMs != null;
  const lossByPeer = (linkQuality && linkQuality.formationPeerLossPct) || {};
  if (!rttKnown && Object.keys(lossByPeer).length === 0) {
    el.style.display = 'none';
    return;
  }
  el.style.display = '';
  const rttText = rttKnown ? `${linkQuality.formationServerRttMs} ms` : '? ms';
  const peerLossText = (peers || [])
    .filter((p) => lossByPeer[p.id] != null)
    .map((p) => `<span class="peer-loss">Peer ${p.id}: ${lossByPeer[p.id]}% loss</span>`)
    .join('');
  el.innerHTML = `Server RTT: ${rttText}${peerLossText}`;
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
  el.innerHTML = `Server RTT: ${rttText}${lossText}`;
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

  renderPeerList(data.peers);
  renderOwnership(data.sharedCockpitOwnership);
  renderFormationLinkQuality(data.linkQuality, data.peers);
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
