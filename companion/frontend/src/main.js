import {
  ChooseXPlanePath,
  CreateSession,
  GetAvailablePluginVersion,
  GetInstalledPluginVersion,
  GetXPlanePath,
  InstallPlugin,
  JoinSession,
  StartSharedCockpit,
} from '../wailsjs/go/main/App';
import { EventsOn } from '../wailsjs/runtime/runtime';

let role = 'MASTER';

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
    await CreateSession(document.getElementById('server').value);
  } catch (e) {
    alert(e);
  }
});

document.getElementById('join-btn').addEventListener('click', async () => {
  try {
    await JoinSession(document.getElementById('server').value, document.getElementById('code').value);
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

function classify(text) {
  if (/error|failed|invalid/i.test(text)) return 'err';
  if (/connected|ready|running/i.test(text)) return 'ok';
  return '';
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

  fillCodeFieldIfEmpty('code', data.formationCode);
  fillCodeFieldIfEmpty('sc-code', data.sharedCockpitCode);

  renderPeerList(data.peers);

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
  // honest about that wait) and once already connected/running, so you
  // can't accidentally kick off a second Create/Join/Start on top of a
  // working session.
  const formationConnected = formationKind === 'ok';
  const sharedCockpitRunning = sharedCockpitKind === 'ok';
  document.getElementById('create-btn').disabled = !data.simReady || formationConnected;
  document.getElementById('join-btn').disabled = !data.simReady || formationConnected;
  document.getElementById('start-sc-btn').disabled = !data.simReady || sharedCockpitRunning;
});
