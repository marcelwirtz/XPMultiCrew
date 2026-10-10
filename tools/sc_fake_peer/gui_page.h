#pragma once

// The GUI page served by main.cpp's little HTTP server. Plain HTML/JS, no
// external files; it polls /api/state and sends the console's own command
// lines to /api/cmd. Split into several literals because MSVC rejects a
// single string literal over ~16 KB.

#include <string>

namespace sc_fake {

inline const std::string& GuiPage() {
    static const std::string page = std::string(R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>SC Fake Peer</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--fg:#1b1f24;--muted:#677180;--line:#dde1e6;--accent:#2563eb;--accent-fg:#fff;
--ok:#15803d;--warn:#b45309;--bad:#b91c1c;--on:#16a34a;--chip:#eef1f5;--mono:ui-monospace,SFMono-Regular,Consolas,monospace}
@media (prefers-color-scheme:dark){:root{--bg:#111418;--card:#1a1e24;--fg:#e6e9ee;--muted:#8d96a3;--line:#2c323b;
--accent:#3b82f6;--ok:#4ade80;--warn:#fbbf24;--bad:#f87171;--on:#22c55e;--chip:#242a32}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
header{position:sticky;top:0;z-index:5;display:flex;flex-wrap:wrap;gap:10px;align-items:center;padding:10px 16px;
background:var(--card);border-bottom:1px solid var(--line)}
header h1{font-size:16px;margin:0 8px 0 0}
.pill{padding:3px 10px;border-radius:999px;background:var(--chip);font-weight:600}
.pill.ok{color:var(--ok)}.pill.warn{color:var(--warn)}.pill.bad{color:var(--bad)}
.muted{color:var(--muted)}.mono{font-family:var(--mono)}
.spacer{flex:1}
button{font:inherit;border:1px solid var(--line);background:var(--chip);color:var(--fg);border-radius:6px;padding:5px 10px;cursor:pointer}
button:hover{border-color:var(--accent)}
button.primary{background:var(--accent);color:var(--accent-fg);border-color:var(--accent)}
button.on{background:var(--on);color:#fff;border-color:var(--on)}
button.held{outline:2px solid var(--accent)}
button:disabled{opacity:.5;cursor:default}
input,select{font:inherit;color:var(--fg);background:var(--bg);border:1px solid var(--line);border-radius:6px;padding:5px 7px;min-width:0}
input[type=range]{padding:0;width:100%}
main{display:grid;grid-template-columns:minmax(300px,420px) 1fr;gap:14px;padding:14px 16px;max-width:1400px}
@media (max-width:860px){main{grid-template-columns:1fr}}
.col{display:flex;flex-direction:column;gap:14px;min-width:0}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 14px;min-width:0}
.card h2{font-size:13px;text-transform:uppercase;letter-spacing:.04em;color:var(--muted);margin:0 0 10px;display:flex;gap:8px;align-items:center}
.card h2 .spacer{flex:1}
.card h3{font-size:12px;color:var(--muted);margin:12px 0 4px;font-weight:600}
#connect{max-width:560px;margin:24px auto}
.form{display:grid;grid-template-columns:110px 1fr;gap:8px 10px;align-items:center}
.seg{display:inline-flex;border:1px solid var(--line);border-radius:7px;overflow:hidden}
.seg button{border:0;border-radius:0;background:transparent}
.seg button.sel{background:var(--accent);color:var(--accent-fg)}
.row3{display:grid;grid-template-columns:repeat(3,1fr);gap:6px}
.hint{font-size:12px;color:var(--muted);margin-top:8px}
.stats{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
.stat{background:var(--chip);border-radius:8px;padding:6px 8px}
.stat b{display:block;font-size:18px;font-variant-numeric:tabular-nums}
.stat span{font-size:11px;color:var(--muted);text-transform:uppercase}
.sliders{display:grid;grid-template-columns:80px 1fr 56px;gap:6px 8px;align-items:center;margin-top:10px}
.sliders output{text-align:right;font-variant-numeric:tabular-nums}
.ctl{display:flex;gap:16px;align-items:flex-end;flex-wrap:wrap}
.yoke{width:120px;height:120px;border:1px solid var(--line);border-radius:8px;position:relative;background:
linear-gradient(var(--line),var(--line)) center/1px 100% no-repeat,linear-gradient(var(--line),var(--line)) center/100% 1px no-repeat}
.yoke i{position:absolute;width:14px;height:14px;margin:-7px;border-radius:50%;background:var(--accent);left:50%;top:50%}
.rud{width:120px;height:10px;border:1px solid var(--line);border-radius:5px;position:relative;margin-top:8px}
.rud i{position:absolute;top:-3px;width:6px;height:14px;margin-left:-3px;background:var(--accent);border-radius:2px;left:50%}
.bars{display:flex;gap:10px}
.bar{display:flex;flex-direction:column;align-items:center;gap:4px;font-size:11px;color:var(--muted)}
.bar div{width:22px;height:110px;border:1px solid var(--line);border-radius:5px;position:relative;overflow:hidden}
.bar div i{position:absolute;left:0;right:0;bottom:0;background:var(--accent)}
.stale{color:var(--warn);font-size:12px}
#cockpit{columns:2 380px;column-gap:28px}
.group{break-inside:avoid;padding-bottom:6px}
.grp{display:grid;grid-template-columns:minmax(110px,1fr) 64px minmax(150px,1.4fr);gap:4px 10px;align-items:center}
.grp .lbl{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.grp .val{font-family:var(--mono);text-align:right;font-variant-numeric:tabular-nums}
.grp .val.flash{color:var(--accent)}
.edit{display:flex;gap:4px;align-items:center;flex-wrap:wrap}
.edit input{width:76px}
.chips button{padding:2px 7px;font-size:12px}
.cmds{display:flex;flex-wrap:wrap;gap:6px}
.cmds button{user-select:none;touch-action:none}
.custom{display:flex;gap:6px;flex-wrap:wrap;margin-top:10px}
.custom input{flex:1;min-width:140px}
table{width:100%;border-collapse:collapse;font-size:13px}
td{padding:3px 6px;border-bottom:1px solid var(--line);vertical-align:top}
td.n{font-family:var(--mono);word-break:break-all}
td.v{font-family:var(--mono);text-align:right}
#log{font:12px/1.45 var(--mono);height:300px;overflow:auto;white-space:pre;margin:0;background:var(--bg);
border-radius:6px;padding:8px}
.bigcode{font:700 28px var(--mono);letter-spacing:.12em}
.hidden{display:none!important}
</style>
</head>
<body>
<header>
  <h1>SC Fake Peer</h1>
  <span id="pill" class="pill">Not connected</span>
  <span id="hdrcode" class="mono"></span>
  <span id="link" class="muted"></span>
  <span class="spacer"></span>
  <button id="take" class="primary hidden">Take control</button>
  <button id="disc" class="hidden">Disconnect</button>
</header>

<section id="connect" class="card">
  <h2>Connect a fake second pilot</h2>
  <div class="form">
    <label>Fake is</label>
    <div class="seg"><button id="m-join" class="sel">Co-pilot (join)</button><button id="m-host">Pilot flying (host)</button></div>
    <label for="f-server">Server</label><input id="f-server" placeholder="127.0.0.1:45000">
    <label for="f-code" class="j">Session code</label><input id="f-code" class="j mono" placeholder="from the companion">
    <label class="h" for="f-airport">Airport</label>
    <div class="h row3"><input id="f-airport" class="mono" placeholder="ICAO, e.g. EDDK"><input id="f-runway" placeholder="runway (longest)"><span></span></div>
    <label class="h">or position</label>
    <div class="h row3"><input id="f-lat" placeholder="lat"><input id="f-lon" placeholder="lon"><input id="f-elev" placeholder="elev m MSL"></div>
    <label class="h">Heading / speed</label>
    <div class="h row3"><input id="f-hdg" placeholder="hdg" value="0"><input id="f-speed" placeholder="kt (auto)"><label><input type="checkbox" id="f-ground"> on ground</label></div>
    <span></span><button id="go" class="primary">Connect</button>
  </div>
  <p class="hint j">Host Shared Cockpit in the companion (same server), then paste its code here.
  The fake joins as co-pilot and shows what your cockpit sends.</p>
  <p class="hint h hidden">The fake creates a session and flies; join it from the companion with the code shown at the top.
  With an airport it lines up on the runway (from your X-Plane's apt.dat; untick <em>on ground</em> to orbit
  1000 ft above it instead) - position and heading are then ignored. With a position, <em>on ground</em>
  takes the elevation as the field elevation in meters.</p>
</section>

<main id="live" class="hidden">
  <div class="col">
    <section class="card" id="codecard">
      <h2>Session code</h2>
      <div class="bigcode" id="bigcode"></div>
      <p class="hint">Companion &rarr; Shared Cockpit &rarr; join with this code.</p>
    </section>
    <section class="card">
      <h2><span id="actitle">Aircraft</span></h2>
      <div id="acnone" class="muted">Waiting for the other side&hellip;</div>
      <div id="acstats" class="stats"></div>
      <div id="acstale" class="stale"></div>
      <div id="pfctl" class="hidden">
        <div class="sliders" id="sliders"></div>
        <div style="display:flex;gap:6px;margin-top:10px">
          <button id="b-ground">On ground</button><button id="b-wiggle">Wiggle yoke</button>
        </div>
        <p class="hint">Take control back from the companion's Shared Cockpit page.</p>
      </div>
    </section>
    <section class="card">
      <h2><span id="ctltitle">Controls</span><span class="spacer"></span><span id="ctlstale" class="stale"></span></h2>
      <div id="ctlnone" class="muted">No controls stream.</div>
      <div id="ctlbox" class="ctl hidden">
        <div><div class="yoke"><i id="yk"></i></div><div class="rud"><i id="rd"></i></div>
          <div class="muted" style="font-size:11px;margin-top:4px">yoke &amp; rudder</div></div>
        <div class="bars" id="bars"></div>
      </div>
    </section>
    <section class="card">
      <h2>Log<span class="spacer"></span><label style="text-transform:none;font-weight:400"><input type="checkbox" id="quiet"> hide switch/button lines</label></h2>
      <pre id="log"></pre>
    </section>
  </div>
  <div class="col">
    <section class="card">
      <h2>C172 cockpit<span class="spacer"></span><span id="scen"></span><button id="resync">Resync</button></h2>
      <div id="cockpit"></div>
    </section>
    <section class="card">
      <h2>Buttons <span class="muted" style="text-transform:none;font-weight:400">(hold = held in the other cockpit)</span></h2>
      <div id="commands"></div>
      <div class="custom"><input id="c-name" class="mono" placeholder="any command, e.g. sim/starters/engage_starter_1">
        <button id="c-press">Hold</button></div>
    </section>
    <section class="card">
      <h2>Other datarefs</h2>
      <table id="others"></table>
      <div class="custom"><input id="d-name" class="mono" placeholder="dataref">
        <select id="d-type"><option value="">auto</option><option>i</option><option>f</option><option>d</option><option>ia</option><option>fa</option></select>
        <input id="d-val" style="flex:0 1 110px" placeholder="value (1,0,0)"><button id="d-set">Set</button></div>
    </section>
  </div>
</main>
)HTML") + std::string(R"JS(<script>
"use strict";
const $ = id => document.getElementById(id);
function h(tag, attrs, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else if (k === "class") e.className = v;
    else if (k === "text") e.textContent = v;
    else e.setAttribute(k, v);
  }
  for (const k of kids) e.append(k);
  return e;
}
const post = text => fetch("/api/cmd", {method: "POST", headers: {"X-Fake-Peer": "1"}, body: text}).catch(() => {});
const clean = s => String(s).trim().replace(/\s+/g, "");
const fmt = (v, d = 0) => v == null ? "—" : Number(v).toFixed(d);

let P = null, S = null, logSeq = -1, mode = "join";
const rows = {};          // dataref name -> {val, sw, input}
const known = new Set();  // preset dataref names

// ---------- connect form ----------
const fields = ["server", "code", "airport", "runway", "lat", "lon", "elev", "hdg", "speed"];
function loadForm() {
  let saved = {};
  try { saved = JSON.parse(localStorage.getItem("scfp") || "{}"); } catch (e) {}
  for (const f of fields) if (saved[f] != null) $("f-" + f).value = saved[f];
  $("f-ground").checked = !!saved.ground;
  setMode(saved.mode || "join");
}
function saveForm() {
  const o = {mode, ground: $("f-ground").checked};
  for (const f of fields) o[f] = $("f-" + f).value;
  try { localStorage.setItem("scfp", JSON.stringify(o)); } catch (e) {}
}
function setMode(m) {
  mode = m;
  $("m-join").classList.toggle("sel", m === "join");
  $("m-host").classList.toggle("sel", m === "host");
  document.querySelectorAll("#connect .j").forEach(e => e.classList.toggle("hidden", m !== "join"));
  document.querySelectorAll("#connect .h").forEach(e => e.classList.toggle("hidden", m !== "host"));
}
$("m-join").onclick = () => setMode("join");
$("m-host").onclick = () => setMode("host");
$("go").onclick = () => {
  saveForm();
  let line = "connect mode=" + mode + " server=" + clean($("f-server").value);
  if (mode === "join") line += " code=" + clean($("f-code").value).toUpperCase();
  else {
    const airport = clean($("f-airport").value).toUpperCase();
    if (airport) {
      line += " airport=" + airport + " runway=" + clean($("f-runway").value) + " speed=" + clean($("f-speed").value);
      line += " air=" + ($("f-ground").checked ? 0 : 1);
    } else {
      for (const f of ["lat", "lon", "elev", "hdg", "speed"]) line += " " + f + "=" + clean($("f-" + f).value);
      line += " ground=" + ($("f-ground").checked ? 1 : 0);
    }
  }
  post(line);
};
$("disc").onclick = () => post("disconnect");
$("take").onclick = () => post("take");
$("resync").onclick = () => post("resync");
$("quiet").onchange = e => post("quiet " + (e.target.checked ? "on" : "off"));

// ---------- cockpit panel (built once from the preset) ----------
const CHIPS = {
  "sim/flightmodel/controls/flaprqst": [["UP", 0], ["10", 0.333], ["20", 0.667], ["FULL", 1]],
  "sim/flightmodel/controls/parkbrake": [["off", 0], ["set", 1]],
  "sim/cockpit2/engine/actuators/carb_heat_ratio": [["off", 0], ["on", 1]],
  "sim/cockpit2/controls/elevator_trim": [["-.2", -0.2], ["0", 0], ["+.2", 0.2]],
  "sim/cockpit/radios/transponder_mode": [["off", 0], ["stby", 1], ["on", 2], ["alt", 3]],
};
const first = v => Array.isArray(v) ? v[0] : v;

function buildCockpit() {
  const box = $("cockpit");
  const groups = {};
  for (const d of P.datarefs) (groups[d.group] = groups[d.group] || []).push(d);
  for (const [g, list] of Object.entries(groups)) {
    const grid = h("div", {class: "grp"});
    box.append(h("div", {class: "group"}, h("h3", {text: g}), grid));
    for (const d of list) {
      known.add(d.name);
      const r = rows[d.name] = {d};
      r.val = h("span", {class: "val", text: "—"});
      const edit = h("div", {class: "edit"});
      if (d.kind === "switch") {
        r.sw = h("button", {text: "OFF", onclick: () => {
          const cur = S && S.values[d.name] ? first(S.values[d.name].v) : 0;
          post("set0 " + d.name + " " + (cur ? 0 : 1));
        }});
        edit.append(r.sw);
      } else {
        r.input = h("input", {placeholder: d.t, title: d.name});
        const send = () => { if (r.input.value.trim() !== "") post("set0 " + d.name + " " + clean(r.input.value)); r.input.value = ""; };
        r.input.addEventListener("keydown", e => { if (e.key === "Enter") send(); });
        r.setBtn = h("button", {text: "Set", onclick: send});
        edit.append(r.input, r.setBtn);
        const chips = CHIPS[d.name];
        if (chips) {
          const c = h("span", {class: "chips"});
          for (const [label, v] of chips) c.append(h("button", {text: label, onclick: () => post("set0 " + d.name + " " + v)}));
          edit.append(c);
        }
        if (d.kind === "output") r.output = true;
      }
      grid.append(h("span", {class: "lbl", title: d.name, text: d.label}), r.val, edit);
    }
  }
  P.scenarios.forEach((label, i) => $("scen").append(h("button", {text: label, style: "margin-right:6px", onclick: () => post("scenario " + i)})));
}

// ---------- buttons: pointer down = command begin, up = end ----------
function holdButton(btn, nameFn) {
  let held = null;
  const up = () => { if (held) { post("end " + held); held = null; btn.classList.remove("held"); } };
  btn.addEventListener("pointerdown", e => {
    const name = nameFn(); if (!name) return;
    btn.setPointerCapture(e.pointerId); held = name; btn.classList.add("held"); post("begin " + name);
  });
  btn.addEventListener("pointerup", up);
  btn.addEventListener("pointercancel", up);
  btn.addEventListener("lostpointercapture", up);
}
function buildCommands() {
  const box = $("commands");
  const groups = {};
  for (const c of P.commands) (groups[c.group] = groups[c.group] || []).push(c);
  for (const [g, list] of Object.entries(groups)) {
    box.append(h("h3", {text: g}));
    const row = h("div", {class: "cmds"});
    for (const c of list) {
      const b = h("button", {text: c.label, title: c.name});
      holdButton(b, () => c.name);
      row.append(b);
    }
    box.append(row);
  }
  holdButton($("c-press"), () => clean($("c-name").value));
}
$("d-set").onclick = () => {
  const name = clean($("d-name").value), val = clean($("d-val").value), t = $("d-type").value;
  if (name && val) post("set " + name + (t ? " " + t : "") + " " + val);
};
)JS") + std::string(R"JS(
// ---------- PF sliders (when the fake flies) ----------
const SLIDERS = [
  ["speed", "Speed kt", 0, 160, 1, s => s.flight.gs_kt],
  ["turn", "Turn °/s", -3, 3, 0.5, s => s.flight.turn],
  ["vs", "VS fpm", -1000, 1000, 100, s => s.flight.vs_fpm],
  ["thr", "Throttle", 0, 1, 0.05, s => s.ctl && s.ctl.thr],
  ["mix", "Mixture", 0, 1, 0.05, s => s.ctl && s.ctl.mix],
  ["brake", "Brakes", 0, 1, 0.05, s => s.ctl && s.ctl.lb],
];
const sliders = {};
const DECIMALS = {speed: 0, turn: 1, vs: 0, thr: 2, mix: 2, brake: 2};
function buildSliders() {
  const box = $("sliders");
  for (const [cmd, label, min, max, step] of SLIDERS) {
    const input = h("input", {type: "range", min, max, step});
    const out = h("output");
    let last = 0, dragging = false;
    input.addEventListener("pointerdown", () => dragging = true);
    input.addEventListener("pointerup", () => dragging = false);
    input.addEventListener("input", () => {
      out.textContent = input.value;
      const now = Date.now();
      if (now - last > 120) { last = now; post(cmd + " " + input.value); }
    });
    input.addEventListener("change", () => post(cmd + " " + input.value));
    sliders[cmd] = {input, out, busy: () => dragging || document.activeElement === input};
    box.append(h("span", {text: label}), input, out);
  }
  $("b-ground").onclick = () => post("ground " + (S.flight.ground ? "off" : "on"));
  $("b-wiggle").onclick = () => post("wiggle " + (S.flight.wiggle ? "off" : "on"));
}

// ---------- rendering ----------
function stat(label, value) { return h("div", {class: "stat"}, h("b", {text: value}), h("span", {text: label})); }

function renderHeader(s) {
  const pill = $("pill");
  let text = "Not connected", cls = "";
  if (s.connected) {
    if (!s.session) { text = "Connecting…"; cls = "warn"; }
    else if (!s.peer) { text = s.mode === "host" ? "Waiting for co-pilot" : "Waiting for host"; cls = "warn"; }
    else { text = s.role === "pf" ? "Fake is flying" : "Fake is co-pilot"; cls = "ok"; }
  }
  pill.textContent = text; pill.className = "pill " + cls;
  $("hdrcode").textContent = s.connected && s.code ? "code " + s.code : "";
  $("link").textContent = s.connected && s.peer ? s.path + (s.rtt != null ? " · server " + s.rtt + " ms" : "") : "";
  $("take").classList.toggle("hidden", !(s.connected && s.running && s.role === "cp"));
  $("disc").classList.toggle("hidden", !s.connected);
  $("connect").classList.toggle("hidden", !!s.connected);
  $("live").classList.toggle("hidden", !s.connected);
  $("codecard").classList.toggle("hidden", !(s.connected && s.mode === "host" && s.code && !s.peer));
  $("bigcode").textContent = s.code || "";
}

function renderAircraft(s) {
  const pf = s.role === "pf";
  $("actitle").textContent = pf ? "Fake aircraft (this side flies)" : "Your aircraft (pilot flying)";
  const box = $("acstats");
  box.replaceChildren();
  $("pfctl").classList.toggle("hidden", !(pf && s.running));
  $("acstale").textContent = "";
  if (pf) {
    const f = s.flight;
    box.append(stat("Alt ft", fmt(f.alt_ft)), stat("Hdg", fmt(f.hdg)), stat("GS kt", fmt(f.gs_kt)),
               stat("VS fpm", fmt(f.vs_fpm)), stat("Turn °/s", fmt(f.turn, 1)), stat("State", f.ground ? "GND" : "AIR"),
               stat("Engine", f.running ? "RUNNING" : f.cranking ? "CRANKING" : "OFF"),
               stat("Key", ["OFF", "R", "L", "BOTH", "START"][f.key] || f.key), stat("RPM", fmt(f.rpm)));
    $("acnone").classList.add("hidden");
    $("b-ground").classList.toggle("on", f.ground);
    $("b-wiggle").classList.toggle("on", f.wiggle);
    for (const [cmd, , , , , get] of SLIDERS) {
      const sl = sliders[cmd], v = get(s);
      if (v != null && !sl.busy()) { sl.input.value = v; sl.out.textContent = Number(v).toFixed(DECIMALS[cmd]); }
    }
  } else if (s.pf) {
    const p = s.pf;
    box.append(stat("Alt ft", fmt(p.alt_ft)), stat("Hdg", fmt(p.hdg)), stat("GS kt", fmt(p.gs_kt)),
               stat("VS fpm", fmt(p.vs_fpm)), stat("RPM", fmt(p.rpm)), stat(p.icao || "State", p.ground ? "GND" : "AIR"),
               stat("Pitch", fmt(p.pitch, 1)), stat("Bank", fmt(p.roll, 1)), stat("Pos/s", fmt(p.rate, 1)));
    $("acnone").classList.add("hidden");
    if (p.age > 1.5) $("acstale").textContent = "No position for " + p.age.toFixed(0) + " s";
  } else {
    $("acnone").classList.remove("hidden");
  }
}

function renderControls(s) {
  const c = s.ctl;
  $("ctltitle").textContent = s.role === "pf" ? "Controls sent" : "Controls received";
  $("ctlnone").classList.toggle("hidden", !!c);
  $("ctlbox").classList.toggle("hidden", !c);
  $("ctlstale").textContent = c && c.age > 1 ? "stale (" + c.age.toFixed(0) + " s) – co-pilot overrides released" : "";
  if (!c) return;
  const clamp = v => Math.max(-1, Math.min(1, v || 0));
  $("yk").style.left = (50 + 50 * clamp(c.yr)) + "%";
  $("yk").style.top = (50 - 50 * clamp(c.yp)) + "%";
  $("rd").style.left = (50 + 50 * clamp(c.yh)) + "%";
  const bars = [["THR", c.thr], ["MIX", c.mix], ["PROP", c.prop], ["L BRK", c.lb], ["R BRK", c.rb]];
  const box = $("bars");
  if (!box.children.length) for (const [l] of bars) box.append(h("div", {class: "bar"}, h("div", {}, h("i")), h("span", {text: l})));
  bars.forEach(([, v], i) => box.children[i].querySelector("i").style.height = (100 * Math.max(0, Math.min(1, v || 0))) + "%");
}

const shown = {};
function showValue(v) {
  if (!v) return "—";
  const one = x => x == null ? "?" : (Number.isInteger(x) ? String(x) : Number(x).toFixed(3).replace(/\.?0+$/, ""));
  return Array.isArray(v.v) ? one(v.v[0]) + (v.v.length > 1 ? " …" : "") : one(v.v);
}
function renderCockpit(s) {
  for (const [name, r] of Object.entries(rows)) {
    const v = s.values[name];
    const text = showValue(v);
    if (shown[name] !== undefined && shown[name] !== text) {
      r.val.classList.add("flash"); setTimeout(() => r.val.classList.remove("flash"), 1200);
    }
    shown[name] = text;
    r.val.textContent = text;
    r.val.title = v ? JSON.stringify(v.v) + " (" + v.t + ")" : "not received yet";
    if (r.sw) { const on = v && first(v.v); r.sw.textContent = on ? "ON" : "OFF"; r.sw.classList.toggle("on", !!on); }
    if (r.output) {
      const pf = s.role === "pf";
      r.input.disabled = r.setBtn.disabled = !pf;
      r.input.placeholder = pf ? r.d.t : "PF only";
    }
  }
  const table = $("others");
  table.replaceChildren();
  for (const [name, v] of Object.entries(s.values).sort()) {
    if (known.has(name)) continue;
    table.append(h("tr", {}, h("td", {class: "n", text: name}), h("td", {class: "muted", text: v.t}),
                 h("td", {class: "v", text: JSON.stringify(v.v)})));
  }
  if (!table.children.length) table.append(h("tr", {}, h("td", {class: "muted", text: "none"})));
}

function renderLog(s) {
  if (s.log_seq === logSeq) return;
  logSeq = s.log_seq;
  const log = $("log");
  const atBottom = log.scrollHeight - log.scrollTop - log.clientHeight < 30;
  log.textContent = s.log.join("\n");
  if (atBottom) log.scrollTop = log.scrollHeight;
}

async function poll() {
  try {
    S = await (await fetch("/api/state")).json();
    renderHeader(S);
    if (S.connected) { renderAircraft(S); renderControls(S); renderCockpit(S); $("quiet").checked = S.quiet; }
    renderLog(S);
  } catch (e) {
    $("pill").textContent = "Program not running"; $("pill").className = "pill bad";
  }
  setTimeout(poll, 250);
}

(async () => {
  loadForm();
  P = await (await fetch("/api/preset")).json();
  buildCockpit(); buildCommands(); buildSliders();
  poll();
})();
</script>
</body>
</html>
)JS");
    return page;
}

} // namespace sc_fake
