// Atrium's page. It keeps no data of its own: Rust (src/app.rs) pushes a full snapshot with
// atrium.update(...) and the page redraws from it. Actions go back with ipc.postMessage.
import { headsetIcons, icon } from './icons.js';

let S = null;
const ui = { page: 'library', selected: null, search: '', source: 'all', version: {}, confirmRemove: null };
const drawn = { page: null, sig: null, side: null, drawer: null };

const $ = (sel, el = document) => el.querySelector(sel);
const send = (cmd, args = {}) => window.ipc.postMessage(JSON.stringify({ cmd, ...args }));
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
const art = (u) => (!u ? null : u.startsWith('file://') ? '/art?p=' + encodeURIComponent(u.slice(7)) : u);
const SOURCE = { steam: 'Steam', meta: 'Meta', local: 'Local' };
const API = { openxr: 'OpenXR', openvr: 'OpenVR', oculus: 'Oculus SDK' };

function ago(ms) {
  if (!ms) return 'Never';
  const s = (Date.now() - ms) / 1000;
  if (s < 60) return 'Just now';
  if (s < 3600) return `${Math.floor(s / 60)} min ago`;
  if (s < 86400) return `${Math.floor(s / 3600)} h ago`;
  if (s < 86400 * 30) return `${Math.floor(s / 86400)} days ago`;
  return new Date(ms).toLocaleDateString();
}
const size = (b) => (b >= 1e9 ? `${(b / 1e9).toFixed(1)} GB` : `${Math.round(b / 1e6)} MB`);

// Placeholder art: a gradient picked from the name, so every game looks distinct.
function tint(name) {
  let h = 0;
  for (const c of name) h = (h * 31 + c.charCodeAt(0)) >>> 0;
  const a = h % 360, b = (a + 40 + (h >> 8) % 60) % 360;
  return `linear-gradient(160deg, hsl(${a} 45% 26%), hsl(${b} 50% 12%))`;
}

// <img> with fallbacks: data-alt holds the next URLs to try; when none load, the
// placeholder underneath shows through.
function img(urls, cls = '') {
  const list = urls.map(art).filter(Boolean);
  if (!list.length) return '';
  return `<img class="${cls}" src="${esc(list[0])}" data-alt="${esc(list.slice(1).join('|'))}" loading="lazy" draggable="false" alt="">`;
}
document.addEventListener('error', (e) => {
  const el = e.target;
  if (el.tagName !== 'IMG') return;
  const alt = (el.dataset.alt || '').split('|').filter(Boolean);
  if (alt.length) { el.dataset.alt = alt.slice(1).join('|'); el.src = alt[0]; } else el.remove();
}, true);

// ---- sidebar ------------------------------------------------------------------------------
const PAGES = [['library', 'Library', icon.library], ['devices', 'Devices', icon.devices], ['mix', 'Mix & match', icon.mix], ['runtime', 'Runtime', icon.runtimes], ['accounts', 'Accounts', icon.accounts], ['settings', 'Settings', icon.settings]];

function primary() {
  const hs = S?.hw?.headsets || [];
  return hs.find((h) => h.active) || hs[0];
}

function renderSide() {
  const h = primary();
  const rt = S?.rt;
  const sig = JSON.stringify([ui.page, h, rt?.registered, !!S?.hw]);
  if (sig === drawn.side) return;
  drawn.side = sig;
  $('#side').innerHTML = `
    <div class="brand">${icon.logo}<span>Atrium</span></div>
    ${PAGES.map(([id, label, ic]) => `<button class="nav ${ui.page === id ? 'on' : ''}" data-page="${id}">${ic}<span>${label}</span></button>`).join('')}
    <div class="side-foot">
      ${S?.hw ? `<div class="mini" data-page="devices">
        <div class="hs ${h ? '' : 'off'}">${headsetIcons[h?.family] || headsetIcons.generic}</div>
        <div style="min-width:0"><b>${esc(h ? h.name : 'No headset')}</b><span>${esc(h ? h.via : 'Plug one in or start streaming')}</span></div>
      </div>` : ''}
      ${rt ? `<div class="rt-line"><i class="dot ${rt.registered ? 'on' : 'warn'}"></i>${rt.registered ? 'Atrium Runtime active' : 'Atrium Runtime not active'}</div>` : ''}
    </div>`;
}

// ---- library ------------------------------------------------------------------------------
function visible() {
  const q = ui.search.trim().toLowerCase();
  return S.games.filter((g) => (!q || g.name.toLowerCase().includes(q)) && (ui.source === 'all' || g.source === ui.source) && (!S.vrOnly || g.vr));
}

function card(g) {
  const urls = [g.art.cover, g.art.wide, g.art.hero];
  return `<div class="game ${g.installed ? '' : 'out'} ${ui.selected === g.id ? 'sel' : ''}" data-game="${esc(g.id)}" tabindex="0">
    <div class="cover" style="background:${tint(g.name)}">
      <div class="fallback">${esc(g.name)}</div>
      ${img(urls)}
      <span class="src ${g.source}">${SOURCE[g.source]}</span>
      ${g.installed ? `<button class="play" data-play="${esc(g.id)}" title="Play">${icon.play}</button>` : ''}
    </div>
    <div class="name" title="${esc(g.name)}">${esc(g.name)}</div>
    <div class="sub" ${S.downloads[g.app_id] && g.source === 'meta' ? `data-dl-sub="${esc(g.app_id)}"` : ''}>${S.downloads[g.app_id] && g.source === 'meta' ? dlSub(S.downloads[g.app_id]) : g.installed ? (g.lastPlayed || g.last_played ? ago(g.last_played) : 'Installed') : 'Not installed'}</div>
    ${S.downloads[g.app_id] && g.source === 'meta' ? `<div class="meter thin"><i data-dl-bar="${esc(g.app_id)}" style="width:${pct(S.downloads[g.app_id])}%"></i></div>` : ''}
  </div>`;
}

function hero() {
  const g = S.games.filter((x) => x.installed && (!S.vrOnly || x.vr)).sort((a, b) => b.last_played - a.last_played)[0];
  if (!g) return '';
  const bg = art(g.art.hero || g.art.wide || g.art.cover);
  const fg = art(g.art.hero || g.art.wide || g.art.cover);
  return `<section class="hero">
    ${bg ? `<div class="bg" style="background-image:url('${esc(bg)}')"></div><div class="art" style="background-image:url('${esc(fg)}')"></div>` : `<div class="bg" style="background:${tint(g.name)}"></div>`}
    <div class="copy">
      <div class="kicker">${g.last_played ? 'Continue playing' : 'Ready to play'}</div>
      <h3>${esc(g.name)}</h3>
      <div class="pills"><span class="pill ${g.source}">${SOURCE[g.source]}</span>${g.vr_api ? `<span class="pill accent">${API[g.vr_api]}</span>` : ''}</div>
      <div class="row">
        <button class="btn primary big" data-play="${esc(g.id)}" ${S.busy.launching ? 'disabled' : ''}>${icon.play}${S.busy.launching === g.name ? 'Starting…' : 'Play'}</button>
        <button class="btn big" data-game="${esc(g.id)}">Details</button>
        <span class="when">${g.last_played ? 'Last played ' + ago(g.last_played).toLowerCase() : ''}</span>
      </div>
    </div>
  </section>`;
}

function libraryShell() {
  return `<div class="page">
    <div class="head">
      <h1>Library</h1><div class="grow"></div>
      <div class="search">${icon.search}<input class="input" id="q" placeholder="Search your games" value="${esc(ui.search)}"></div>
      <div class="seg" id="src">${['all', 'steam', 'meta', 'local'].map((s) => `<button data-src="${s}" class="${ui.source === s ? 'on' : ''}">${s === 'all' ? 'All' : SOURCE[s]}</button>`).join('')}</div>
      <span class="toggle" id="vr"><i></i>VR only</span>
      <button class="btn icon-btn ghost" id="refresh" title="Refresh">${icon.refresh}</button>
    </div>
    <div id="lib"></div>
  </div>`;
}

function libraryBody() {
  $('#vr')?.classList.toggle('on', S.vrOnly);
  $('#refresh')?.classList.toggle('spin', S.busy.refreshing || S.busy.scanning);
  const games = visible();
  const inst = games.filter((g) => g.installed), rest = games.filter((g) => !g.installed);
  let html = '';
  if (!S.games.length) {
    html = S.busy.scanning ? '' : `<div class="empty"><b>No games yet</b>Sign in to Steam or Meta to see what you own, or add a game yourself.<br><button class="btn primary" data-page="accounts">Go to Accounts</button></div>`;
  } else if (!games.length) {
    html = `<div class="empty"><b>Nothing matches</b>Try another search or filter.</div>`;
  } else {
    if (!ui.search && ui.source === 'all') html += hero();
    if (inst.length) html += `<h2>Installed <span class="n">${inst.length}</span></h2><div class="grid">${inst.map(card).join('')}</div>`;
    if (rest.length) html += `<h2>In your library <span class="n">${rest.length}</span></h2><div class="grid">${rest.map(card).join('')}</div>`;
  }
  $('#lib').innerHTML = html;
}

// ---- Meta downloads -----------------------------------------------------------------------
// The build the download picker shows for a game: the user's pick, else the newest one not
// downloaded yet (else the newest).
function latest(g) {
  const b = S.versions[g.app_id]?.builds;
  if (!b?.length) return null;
  return b.find((x) => x.binary_id === ui.version[g.app_id]) || b.find((x) => !g.versions.includes(x.version)) || b[0];
}

const pct = (p) => (p.total ? Math.min(100, Math.floor((p.done / p.total) * 100)) : 0);
const dlSub = (p) => `Downloading ${pct(p)}%`;
const dlHint = (p) => `${p.phase}${p.total ? ` · ${size(p.done)} of ${size(p.total)} (${pct(p)}%)` : ''}`;

// Progress ticks arrive a few times a second: update the bars and text in place (redrawing the
// library or the drawer each time reloads their images, which flickers).
function paintDownloads() {
  for (const [id, p] of Object.entries(S.downloads)) {
    const q = (attr) => document.querySelectorAll(`[${attr}="${CSS.escape(id)}"]`);
    q('data-dl-bar').forEach((el) => { el.style.width = `${pct(p)}%`; });
    q('data-dl-sub').forEach((el) => { el.textContent = dlSub(p); });
    q('data-dl-hint').forEach((el) => { el.textContent = dlHint(p); });
  }
}

function versionField(g, dl) {
  const v = S.versions[g.app_id];
  // Downloaded versions: one is played, any other is a click away; each can be removed.
  const mine = g.versions.length ? `<div class="field"><label>Your versions</label><div class="versions">${g.versions.map((ver) => {
    const key = `${g.app_id}|${ver}`;
    const playing = ver === g.version;
    return `<div class="ver ${playing ? 'on' : ''}"><span class="grow mono">${esc(ver)}</span>
      ${playing ? '<span class="pill good">Plays</span>' : `<button class="btn" data-use-version="${esc(ver)}" ${dl ? 'disabled' : ''}>Use this version</button>`}
      <button class="btn ghost danger" data-remove-dl="${esc(key)}" ${dl ? 'disabled' : ''}>${ui.confirmRemove === key ? 'Delete its files?' : 'Remove'}</button></div>`;
  }).join('')}</div><div class="hint">Each version has its own folder; switching is instant. Play starts the one marked Plays.</div></div>` : '';
  if (dl) {
    return mine + `<div class="field"><label>Downloading ${esc(dl.version)}</label>
      <div class="meter"><i data-dl-bar="${esc(g.app_id)}" style="width:${pct(dl)}%"></i></div>
      <div class="hint" data-dl-hint="${esc(g.app_id)}">${esc(dlHint(dl))}</div>
      <div class="row" style="margin-top:10px"><button class="btn" data-cancel-dl="${esc(g.app_id)}">Stop</button></div></div>`;
  }
  if (!v) return mine + `<div class="field"><label>Download a version</label><div class="hint">Asking Meta for versions…</div></div>`;
  if (v.error) return mine + `<div class="field"><label>Download a version</label><div class="hint">${esc(v.error)}</div></div>`;
  const pick = latest(g);
  const have = g.versions.includes(pick.version);
  const label = (b, i) => `${b.version}${i === 0 ? ' (latest)' : ''}${g.versions.includes(b.version) ? ' · downloaded' : ''}`;
  return mine + `<div class="field"><label>Download a version</label>
    <div class="row"><select class="input grow" id="version">${v.builds.map((b, i) => `<option value="${esc(b.binary_id)}" ${b.binary_id === pick.binary_id ? 'selected' : ''}>${esc(label(b, i))}</option>`).join('')}</select>
    <button class="btn primary" data-download="${esc(g.app_id)}" ${have ? 'disabled' : ''}>${icon.download}${have ? 'Downloaded' : 'Download'}</button></div>
    <div class="hint">${v.builds.length} versions from Meta. ${g.versions.length ? 'Pieces your other versions already have are copied, not downloaded again.' : `Saves to ${esc(S.downloadDir || 'a folder you pick (Settings)')}.`}</div></div>`;
}

// ---- drawer -------------------------------------------------------------------------------
function renderDrawer() {
  const d = $('#drawer');
  const g = ui.selected && S?.games.find((x) => x.id === ui.selected);
  const sig = g ? JSON.stringify([g, S.modes[g.id], S.starts[g.id], S.ovr, S.busy.launching, S.versions[g.app_id], S.downloads[g.app_id]?.version, S.meta.length, ui.version[g.app_id], ui.confirmRemove]) : null;
  if (sig === drawn.drawer) return;
  drawn.drawer = sig;
  if (!g) { d.hidden = true; d.innerHTML = ''; return; }
  // Meta games: Atrium downloads any version Meta still serves, with the signed-in account.
  const downloadable = g.source === 'meta' && (g.owned || g.downloaded) && S.meta.length;
  if (downloadable && !S.versions[g.app_id]) send('metaVersions', { appId: g.app_id });
  const dl = S.downloads[g.app_id];
  const modes = [['auto', 'Automatic']];
  if (g.source === 'steam') modes.push(['steam', 'Through Steam']);
  if (g.exe) { modes.push(['direct', 'Directly']); if (S.ovr) modes.push(['translate', 'Through Atrium OVR (OpenXR)']); }
  const mode = S.modes[g.id] || 'auto';
  const heroUrl = art(g.art.hero || g.art.wide || g.art.cover);
  const vr = g.vr_only ? 'VR only' : g.vr ? 'Supports VR' : g.vr_known ? 'No' : 'Unknown';
  const fresh = d.hidden;
  d.hidden = false;
  d.innerHTML = `<div class="scrim" data-close></div>
    <div class="panel" style="${fresh ? '' : 'animation:none'}">
      <div class="d-hero" style="background-image:${heroUrl ? `url('${esc(heroUrl)}')` : tint(g.name)}"><button class="btn icon-btn" data-close title="Close">${icon.close}</button></div>
      <div class="d-body">
        <h3>${esc(g.name)}</h3>
        <div class="pills">
          <span class="pill ${g.source}">${SOURCE[g.source]}</span>
          ${g.installed ? '<span class="pill good">Installed</span>' : g.owned ? '<span class="pill">Owned</span>' : ''}
          ${g.vr_api ? `<span class="pill accent">${API[g.vr_api]}</span>` : ''}
        </div>
        <div class="actions">
          ${g.installed ? `<button class="btn primary big" data-play="${esc(g.id)}" ${S.busy.launching ? 'disabled' : ''}>${icon.play}${S.busy.launching === g.name ? 'Starting…' : 'Play'}</button>`
            : g.source === 'local' ? '<button class="btn big" disabled>File missing</button>'
            : downloadable ? `<button class="btn primary big" data-download="${esc(g.app_id)}" ${dl || !latest(g) ? 'disabled' : ''}>${icon.download}${dl ? 'Downloading…' : latest(g) ? `Download ${esc(latest(g).version)}` : 'Download'}</button>`
            : `<button class="btn primary big" data-play="${esc(g.id)}">${g.source === 'steam' ? 'Install' : 'Get in Meta app'}</button>`}
          ${g.install_dir && g.installed ? `<button class="btn big icon-btn" data-folder="${esc(g.id)}" title="Open folder">${icon.folder}</button>` : ''}
        </div>
        ${g.installed ? `<div class="field"><label>Start</label>
          <select class="input" id="mode" style="width:100%">${modes.map(([k, l]) => `<option value="${k}" ${k === mode ? 'selected' : ''}>${l}</option>`).join('')}</select>
          <div class="hint">Will start: ${esc(S.starts[g.id] || '')}</div></div>` : ''}
        ${downloadable ? versionField(g, dl) : ''}
        <div class="field"><label>Details</label><dl class="facts">
          <dt>Last played</dt><dd>${ago(g.last_played)}</dd>
          ${g.size ? `<dt>Size</dt><dd>${size(g.size)}</dd>` : ''}
          <dt>VR</dt><dd>${vr}</dd>
          ${g.app_id && g.source !== 'local' ? `<dt>App id</dt><dd>${esc(g.app_id)}</dd>` : ''}
          ${g.install_dir ? `<dt>Folder</dt><dd class="mono">${esc(g.install_dir)}</dd>` : ''}
        </dl></div>
        ${g.source === 'local' ? `<div class="field"><button class="btn danger" data-remove="${esc(g.id)}">Remove from library</button></div>` : ''}
      </div>
    </div>`;
}

// ---- devices ------------------------------------------------------------------------------
function devices() {
  const hw = S.hw;
  if (!hw) return `<div class="page"><div class="head"><h1>Devices</h1></div><div class="empty">Looking for headsets…</div></div>`;
  const m = hw.meta && hw.meta.hmdPresent && hw.meta.width ? hw.meta : null;
  return `<div class="page">
    <div class="head"><div><h1>Devices</h1><p class="lede">Updates every few seconds.</p></div></div>
    <h2>Headsets</h2>
    ${hw.headsets.length ? `<div class="cards">${hw.headsets.map((h) => `<div class="card headset ${h.active ? 'on' : ''}">
      <div class="ic">${headsetIcons[h.family] || headsetIcons.generic}</div>
      <div><b>${esc(h.name)}</b><div class="sub">${esc([h.vendor, h.model].filter(Boolean).join(' · '))}</div>
      <div class="pills"><span class="pill">${esc(h.via)}</span>${h.active ? '<span class="pill good">In use</span>' : ''}${h.is_virtual ? '<span class="pill warn">Virtual</span>' : ''}</div></div>
    </div>`).join('')}</div>`
    : `<div class="card"><h4>No headset found</h4><p>Plug one in over USB, or start Link, Virtual Desktop or ALVR.</p></div>`}
    ${m ? `<p class="lede" style="margin-top:12px">Meta runtime reports ${esc(m.product)} · ${m.width}×${m.height} per eye · ${Math.round(m.refresh)} Hz</p>` : ''}
    ${hw.controllers.length || hw.accessories.length ? `<h2>Controllers and trackers</h2><div class="card list">
      ${hw.controllers.map(([n, via]) => `<div class="item"><i class="dot on"></i>${esc(n)}<span class="sub">${esc(via)}</span></div>`).join('')}
      ${hw.accessories.map((a) => `<div class="item"><i class="dot on"></i>${esc(a.name)}${a.count > 1 ? ` ×${a.count}` : ''}<span class="sub">USB</span></div>`).join('')}
    </div>` : ''}
    <h2>Standalone headsets</h2>
    ${hw.standalone.length ? `<div class="card list">${hw.standalone.map((s) => `<div class="item">
      <i class="dot ${s.state === 'device' ? 'on' : ''}"></i>${esc(s.model || 'Headset')}
      <span class="sub">${s.state !== 'device' ? 'Allow USB debugging in the headset' : s.atrium_version ? `Atrium ${esc(s.atrium_version)} installed` : 'Atrium not installed'}</span>
      ${s.state === 'device' && hw.standalone_apk ? `<button class="btn ${s.atrium_version ? 'ghost' : 'primary'}" data-cmd="installAtrium" data-serial="${esc(s.serial)}">${s.atrium_version ? 'Update' : 'Install Atrium'}</button>` : ''}
      ${s.state === 'device' && s.atrium_version ? `<button class="btn" data-cmd="openAtrium" data-serial="${esc(s.serial)}">Open on headset</button>` : ''}
    </div>`).join('')}</div>`
    : '<div class="card"><h4>No Quest or Pico found</h4><p>Turn on developer mode on the headset, plug it in over USB and allow USB debugging. Atrium installs its headset app from here.</p></div>'}
    <h2>VR software</h2>
    <div class="chips">
      <span class="chip"><i class="dot ${hw.meta_running ? 'on' : ''}"></i>Meta runtime</span>
      <span class="chip"><i class="dot ${hw.steamvr_running ? 'on' : ''}"></i>SteamVR</span>
      ${hw.streaming.map((s) => `<span class="chip"><i class="dot ${s.running ? 'on' : ''}"></i>${esc(s.name)}</span>`).join('')}
    </div>
    ${hw.emulation || hw.sim_available ? `<h2>Testing</h2><div class="card">
      ${hw.sim_available ? `<div class="row"><div class="grow"><h4>Simulated Steam Frame</h4><p>Runs on Meta's emulated headset.</p></div>
        ${hw.sim_running ? '<span class="pill good">Running</span>' : '<button class="btn" data-cmd="startSim">Start</button>'}</div>` : ''}
      ${hw.emulation ? `<div class="note">Meta headset emulation is on (OVREmulationPlugin=${esc(hw.emulation)}). A real Rift won't show until it's off and Meta's service restarts.</div>` : ''}
    </div>` : ''}
  </div>`;
}

// ---- mix & match --------------------------------------------------------------------------
// Any headset with any controllers you have. For now: Rift CV1 Touches (Atrium tracks them with
// the Rift sensors) with a streamed headset like a Quest on Virtual Desktop (src/services/mix.rs).
const SENSOR_KIND = { winusb: ['on', 'On Atrium\'s driver'], 'winusb-noguid': ['warn', 'Needs set-up'], meta: ['warn', 'On Meta\'s driver'], other: ['warn', 'Unknown driver'], none: ['warn', 'No driver'] };

function step(ok, title, body, action = '') {
  return `<div class="item"><i class="dot ${ok ? 'on' : 'warn'}"></i><div class="grow"><b>${title}</b><div class="sub-l">${body}</div></div>${action}</div>`;
}

// Room calibration (the tracking service places every sensor but the first; rift_room.json).
const ROOM_FIRST_SOLVE = 60;

function roomStep(st) {
  const room = st.room || [];
  if (room.length < 2) return '';
  const waiting = room.filter((s) => !s.placed);
  if (!waiting.length) return step(true, 'Room set up', `${room.length} sensors placed, and kept refined while you play.`);
  const body = !st.service_running ? 'The sensors are placed while the tracking service runs.'
    : waiting.map((s) => `Sensor ${room.indexOf(s) + 1}: ${s.sightings} points${s.sightings < ROOM_FIRST_SOLVE ? ` of ${ROOM_FIRST_SOLVE}` : ', keep going, over a wider area'}`).join(' · ')
      + '. As in Meta\'s setup: hold the Rift headset (or a Touch) where both sensors see it and slowly move it around, up and down and side to side, over a metre or so.';
  return step(false, 'Room set up', body);
}

// The room from above: x right, -z up the page (the way the reference sensor looks across).
function roomMap(room) {
  const placed = room.filter((s) => s.placed);
  const W = 460, H = 280, pad = 40;
  const xs = placed.map((s) => s.pose[0]), zs = placed.map((s) => s.pose[2]);
  const cx = xs.length ? (Math.min(...xs) + Math.max(...xs)) / 2 : 0, cz = zs.length ? (Math.min(...zs) + Math.max(...zs)) / 2 : 0;
  const span = Math.max(3, ...xs.map((x) => Math.abs(x - cx) * 2 + 1), ...zs.map((z) => Math.abs(z - cz) * 2 + 1));
  const k = Math.min(W - 2 * pad, H - 2 * pad) / span;
  const P = (x, z) => [W / 2 + (x - cx) * k, H / 2 + (z - cz) * k];
  // A camera looks along its -z: turn (0, 0, -1) by the pose's quaternion.
  const look = ([, , , qx, qy, qz, qw]) => [-(2 * (qx * qz + qw * qy)), -(1 - 2 * (qx * qx + qy * qy))];
  let grid = '';
  for (let m = -10; m <= 10; m++) {
    const [gx] = P(Math.round(cx) + m, 0), [, gz] = P(0, Math.round(cz) + m);
    if (gx > 0 && gx < W) grid += `<line x1="${gx}" y1="0" x2="${gx}" y2="${H}"/>`;
    if (gz > 0 && gz < H) grid += `<line x1="0" y1="${gz}" x2="${W}" y2="${gz}"/>`;
  }
  const marks = placed.map((s) => {
    const [x, y] = P(s.pose[0], s.pose[2]);
    let [dx, dz] = look(s.pose);
    const l = Math.hypot(dx, dz) || 1; dx /= l; dz /= l;
    const a = Math.atan2(dz, dx), fov = 0.7, r = 70;
    const cone = `M${x},${y} L${x + r * Math.cos(a - fov)},${y + r * Math.sin(a - fov)} A${r},${r} 0 0,1 ${x + r * Math.cos(a + fov)},${y + r * Math.sin(a + fov)} Z`;
    const i = room.indexOf(s) + 1;
    return `<path class="cone" d="${cone}"/><circle class="${s.reference ? 'ref' : 'cam'}" cx="${x}" cy="${y}" r="7"/>
      <text x="${x}" y="${y - 13}" text-anchor="middle">Sensor ${i}${s.reference ? ' (reference)' : ''}</text>
      <text class="h" x="${x}" y="${y + 22}" text-anchor="middle">${s.pose[1].toFixed(2)} m up</text>`;
  }).join('');
  const waiting = room.filter((s) => !s.placed).map((s) => `<div class="sub-l">Sensor ${room.indexOf(s) + 1} (${esc(s.serial)}): not placed yet, ${s.sightings} points</div>`).join('');
  // The live layer (roomLiveLoop) draws the headset and Touches here, with the same mapping.
  return `<svg id="roomsvg" viewBox="0 0 ${W} ${H}" data-cx="${cx}" data-cz="${cz}" data-k="${k}" role="img" aria-label="Rift sensors seen from above"><g class="grid">${grid}</g>${marks}<g id="roomlive"></g></svg>
    <div class="sub-l">Grid: 1 m. Live: <span class="key touch"></span>Touches, where the sensors see them now.</div>${waiting}`;
}

// The headset and the Touches on the room map, ~10 times a second while Mix & match is open (/room-live,
// rift_live.json: tracker device 0 is the headset, the Touches follow).
const roomLive = { running: false };
async function roomLiveLoop() {
  if (roomLive.running) return;
  roomLive.running = true;
  while (ui.page === 'mix') {
    const svg = $('#roomsvg'), layer = $('#roomlive');
    const r = svg ? await fetch('/room-live', { cache: 'no-store' }).catch(() => null) : null;
    if (svg && layer) {
      const j = r && r.ok ? await r.json().catch(() => null) : null;
      const W = 460, H = 280, cx = +svg.dataset.cx, cz = +svg.dataset.cz, k = +svg.dataset.k;
      layer.innerHTML = (j?.devices || []).filter((d) => d.age_ms < 1500).map((d) => {
        // In Mix & match the Rift headset is only the radio and isn't tracked (patch 0015): everything the
        // sensors see is a Touch, whatever the tracker's numbering.
        const x = W / 2 + (d.pos[0] - cx) * k, y = H / 2 + (d.pos[2] - cz) * k;
        return `<g class="live touch" opacity="${d.age_ms > 300 ? 0.4 : 1}">
          <circle cx="${x}" cy="${y}" r="5"/>
          <text x="${x}" y="${y + 18}" text-anchor="middle">Touch · ${d.pos[1].toFixed(2)} m</text></g>`;
      }).join('');
    }
    await new Promise((res) => setTimeout(res, 100));
  }
  roomLive.running = false;
}

// The Touches in SteamVR games (TouchLink's driver): rows for the steps list...
function touchlinkSteps(t, btn) {
  const loaded = t.registered && t.running;
  return step(loaded, 'Touches in SteamVR', !t.registered ? 'Adds the Touches to SteamVR as Oculus Touch controllers, for every SteamVR game.'
      : t.running ? (t.connected ? 'Loaded in SteamVR and connected to the tracking service.' : 'Loaded in SteamVR, waiting for the tracking service.')
      : 'Added. Start (or restart) SteamVR to load it.',
      t.registered ? '' : btn('mixInstallTouchLink', 'Add to SteamVR', true, 'touchlink'))
    + step(t.calibrated && !t.calibrating, 'Lined up with SteamVR', t.calibrating ? esc(t.calibration_message)
      : t.calibrated ? `${(t.error_mm / 10).toFixed(1)} cm fit. Again only if the sensors or base stations move.`
      : 'Once: hold a Touch against the side of the headset and look around until both Touches buzz.',
      t.running && !t.calibrating ? btn('mixCalibrateTouchLink', t.calibrated ? 'Calibrate again' : 'Calibrate', !t.calibrated) : '');
}

// ...and a live card: what's tracked right now, and the calibration as it happens.
function touchlinkCard(t) {
  if (!t.running) return '';
  const hand = (i) => !t.active[i] ? ['', 'Off: press a button'] : t.tracked[i] ? ['on', 'Seen by a sensor'] : ['warn', 'Not seen: face a sensor'];
  const row = (cls, name, text) => `<div class="item"><i class="dot ${cls}"></i><div class="grow"><b>${name}</b><div class="sub-l">${text}</div></div></div>`;
  return `<h2>Touches in SteamVR</h2>
    <div class="card list steps">
      ${row(t.hmd_tracked ? 'on' : 'warn', 'Headset', t.hmd_tracked ? 'Tracked by the base stations' : 'Not tracked: wake it (put it on)')}
      ${row(hand(0)[0], 'Left Touch', hand(0)[1])}
      ${row(hand(1)[0], 'Right Touch', hand(1)[1])}
      ${t.calibrating ? row('warn', 'Calibrating', `${esc(t.calibration_message)}<br>Hold either Touch flat and firm against the side of the headset (the side facing the sensors) and slowly look left, right, up, down, and tilt your head.`) : ''}
      ${t.camera ? row(t.camera_updates ? 'on' : 'warn', 'Headset cameras', `${esc(t.camera)}${t.camera_mm ? ` · ${(t.camera_mm / 10).toFixed(1)} cm fit` : ''}${t.camera_updates ? ` · corrected the calibration ${t.camera_updates}×` : ''}. They see the Touches' lights and keep the calibration right while you play.`) : ''}
      ${t.message ? row('warn', 'TouchLink', esc(t.message)) : ''}
    </div>`;
}

function mix() {
  const m = S.mix || {}, st = m.state, busy = m.busy;
  const others = (S.hw?.headsets || []).filter((h) => !(h.family === 'rift' && h.via === 'USB'));
  // Streamers running now (Atrium's own tracking service shows as "monado": not a headset).
  const streams = (S.hw?.streaming || []).filter((s) => s.running && s.id !== 'monado');
  const headset = others.find((h) => h.active) || others[0] || (streams[0] && { name: streams[0].name, via: 'streaming' });
  const head = `<div class="head"><div><h1>Mix & match</h1><p class="lede">Use any headset with any controllers you have.</p></div><div class="grow"></div><span class="pill accent">Currently only Rift CV1 controllers</span></div>`;
  if (!st) return `<div class="page">${head}<div class="empty">Checking your devices…</div></div>`;
  const sensorsOk = st.sensors.length > 0 && st.sensors.every(([, k]) => k === 'winusb') && st.lpm_off;
  const cal = st.calibration;
  // A wired headset (an Index, ...) is a SteamVR headset: its games get the Touches through TouchLink.
  const steamvr = !!headset && headset.via === 'USB' && !!st.touchlink;
  // Option: Meta's runtime tracks the Touches (TouchLink's meta source); its sensors on Meta's driver.
  const metaTracking = m.tracking === 'meta';
  const metaSensorsOk = st.sensors.length > 0 && st.sensors.every(([, k]) => k === 'meta');
  const btn = (cmd, label, primary = false, which = '') => `<button class="btn ${primary ? 'primary' : ''}" data-cmd="${cmd}" ${busy ? 'disabled' : ''}>${busy && busy === which ? 'Waiting for Windows…' : label}</button>`;
  return `<div class="page">
    ${head}
    <div class="card">
      <div class="row"><div class="grow"><h4>Headset</h4><p>${esc(headset ? `${headset.name} · ${headset.via}` : 'None found yet: start Virtual Desktop on your headset')}</p></div></div>
      <div class="row"><div class="grow"><h4>Controllers</h4><p>${m.enabled ? 'Games get your Rift CV1 Touches instead of the headset\'s own controllers.' : 'Games get the headset\'s own controllers.'}</p></div>
        <select class="input" id="mixcontrollers" style="width:240px">
          <option value="own" ${m.enabled ? '' : 'selected'}>Headset's own controllers</option>
          <option value="cv1" ${m.enabled ? 'selected' : ''}>Rift CV1 Touches</option>
        </select></div>
      ${m.enabled ? `<div class="row"><div class="grow"><h4>Touch tracking</h4><p>${metaTracking
        ? 'Meta\'s runtime tracks the Touches. It needs the Oculus app installed and the Rift fully connected, HDMI as well as USB (Meta\'s runtime won\'t open a Rift without its display). Atrium keeps Meta\'s software running and the sensors on Meta\'s driver.'
        : 'Atrium tracks the Touches with your Rift sensors. The Rift only needs its USB cable. No Meta software.'}</p></div>
        <select class="input" id="mixtracking" style="width:240px">
          <option value="atrium" ${metaTracking ? '' : 'selected'}>Atrium (Rift on USB)</option>
          <option value="meta" ${metaTracking ? 'selected' : ''}>Meta's runtime (Rift on HDMI + USB)</option>
        </select></div>` : ''}
      ${st.available ? '' : '<div class="note">Atrium\'s tracking service isn\'t built yet: native\\atrium_monado\\build.ps1 -Rift, then native\\build.bat.</div>'}
    </div>
    <h2>Getting the Touches ready</h2>
    <div class="card list steps">
      ${step(!!headset, 'Headset connected', headset ? `${esc(headset.name)} (${esc(headset.via)}): games show on it.` : 'Start Virtual Desktop on your headset and connect to this PC. Games can\'t start without a headset.')}
      ${step(st.rift, 'Rift CV1 headset on USB', 'The Touches talk to the PC through the Rift\'s radio. It only needs its USB cable; the display isn\'t used.')}
      ${metaTracking ? `
      ${step(st.meta_running, 'Meta\'s runtime', st.meta_running ? 'Running: it tracks the Touches.' : m.enabled ? (st.rift ? 'Starting…' : 'Starts when the Rift is plugged in.') : 'Starts when you pick Rift CV1 Touches above.')}
      ${step(metaSensorsOk, `Sensors on Meta's driver${st.sensors.length ? ` (${st.sensors.length})` : ''}`,
        st.sensors.length ? st.sensors.map(([, k]) => esc((SENSOR_KIND[k] || SENSOR_KIND.other)[1])).join(' · ') : 'No Rift sensor plugged in.',
        st.sensors.length && !metaSensorsOk ? btn('mixGiveToMeta', 'Set up for Meta', true, 'tracking') : '')}` : `
      ${step(!st.meta_running, 'Meta\'s software off', st.meta_running ? 'Meta\'s runtime is running and fights Atrium for the Rift. It comes back when you restart the PC.' : 'Atrium drives the Rift itself.',
        st.meta_running ? btn('mixStopMeta', 'Turn it off', true, 'meta') : '')}
      ${step(sensorsOk, `Sensors on Atrium's driver${st.sensors.length ? ` (${st.sensors.length})` : ''}`,
        st.sensors.length ? st.sensors.map(([, k]) => esc((SENSOR_KIND[k] || SENSOR_KIND.other)[1])).join(' · ') + (st.lpm_off ? '' : ' · USB power saving still on') : 'No Rift sensor plugged in.',
        st.sensors.length ? (sensorsOk ? btn('mixRestoreSensors', 'Give back to Meta', false, 'sensors') : btn('mixSetupSensors', 'Set up sensors', true, 'sensors')) : '')}
      ${step(st.service_running, 'Atrium\'s tracking service', st.service_running ? 'Running: tracking the Touches with your sensors.' : m.enabled ? (st.rift ? 'Starting…' : 'Starts when the Rift is plugged in.') : 'Starts when you pick Rift CV1 Touches above.',
        st.service_running ? btn('mixRestartService', 'Restart', false, 'service') : '')}
      ${roomStep(st)}`}
      ${steamvr ? touchlinkSteps(st.touchlink, btn) : step(!!cal, 'Calibrated', st.calibrating ? 'Waiting for a game: start one with a Touch in each hand; when they buzz, move both hands around, turning your wrists, for 5 seconds.'
        : cal ? `Lined up with your headset's ${esc(cal.reference)} space, ${(cal.error_mm / 10).toFixed(1)} cm off on average (${ago(cal.when).toLowerCase()}).`
        : 'Lines the Touches up with your headset, once; no other controllers needed (the headset\'s hand tracking sees your hands). In a game, with a Touch in each hand: press X, Y, A and B together (or Calibrate here), then move both hands around, turning your wrists, for 5 seconds. A short buzz means done.',
        m.enabled ? btn('mixCalibrate', cal ? 'Calibrate again' : 'Calibrate', !cal) : '')}
    </div>
    ${st.room.length && !metaTracking ? `<h2>Room</h2>
    <div class="card room">
      <div class="row"><div class="grow"><p>Where the tracking service has put each sensor, seen from above. The first sensor is the reference; the others are placed from where the Rift headset or a Touch is while a placed sensor sees it too (as Meta's setup did), and keep getting refined.</p></div>
        ${btn('mixResetRoom', 'Place again', false, 'room')}</div>
      <div class="roommap">${roomMap(st.room)}</div>
    </div>` : ''}
    ${steamvr && st.touchlink ? touchlinkCard(st.touchlink, btn) : ''}
    <h2>What the sensors see</h2>
    <div class="card">
      <div class="row"><div class="grow"><p>A live picture from each sensor, brightened so you can see the room. Point them so they see where you play; the Touches' lights show as bright dots.</p></div>
        <span class="toggle ${view.on ? 'on' : ''}" id="viewtoggle"><i></i>Live view</span></div>
      <div class="row"><label class="grow-0">Brightness</label><input type="range" id="viewgain" min="1" max="64" value="${view.gain}" style="width:240px"></div>
      <div class="views" id="views">${view.on ? '' : '<p class="sub-l">Turn on the live view to see what each sensor sees.</p>'}</div>
    </div>
  </div>`;
}

// The live sensor view: polls /sensor-view (ui.rs) for each sensor about 30 times a second while
// the tab is open and the view is on, and draws the picture brightened.
const view = { on: false, gain: 12, running: false, lut: null, lutGain: 0 };

function viewLut() {
  if (view.lutGain === view.gain) return view.lut;
  const lut = new Uint8ClampedArray(256);
  for (let v = 0; v < 256; v++) lut[v] = 255 * Math.sqrt(Math.min(1, (v / 255) * view.gain));
  view.lut = lut; view.lutGain = view.gain;
  return lut;
}

async function viewFrame(i) {
  const r = await fetch(`/sensor-view?i=${i}`, { cache: 'no-store' }).catch(() => null);
  const box = $('#views');
  let wrap = box && box.querySelector(`[data-view="${i}"]`);
  if (!r || !r.ok) { wrap?.remove(); return false; }
  const buf = new DataView(await r.arrayBuffer());
  const w = buf.getUint32(0, true), h = buf.getUint32(4, true);
  const serial = new TextDecoder().decode(new Uint8Array(buf.buffer, 8, 32)).replace(/\0.*$/, '');
  if (!box) return true;
  if (!wrap) {
    box.querySelector('p')?.remove();
    wrap = document.createElement('figure');
    wrap.dataset.view = i;
    wrap.innerHTML = `<canvas width="${w}" height="${h}"></canvas><figcaption></figcaption>`;
    box.append(wrap);
  }
  wrap.querySelector('figcaption').textContent = `Sensor ${i + 1} · ${serial}`;
  const c = wrap.querySelector('canvas');
  if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
  const ctx = c.getContext('2d');
  const img = ctx.createImageData(w, h);
  const px = new Uint8Array(buf.buffer, 40, w * h), out = img.data, lut = viewLut();
  for (let p = 0, o = 0; p < px.length; p++, o += 4) { const v = lut[px[p]]; out[o] = v; out[o + 1] = v; out[o + 2] = v; out[o + 3] = 255; }
  ctx.putImageData(img, 0, 0);
  return true;
}

async function viewLoop() {
  if (view.running) return;
  view.running = true;
  while (view.on && ui.page === 'mix') {
    const t = performance.now();
    const got = await Promise.all([0, 1, 2, 3].map(viewFrame));
    const box = $('#views');
    if (box && !got.some(Boolean) && !box.querySelector('p')) box.innerHTML = '<p class="sub-l">No picture yet: the sensors stream while Atrium\'s tracking service runs (pick Rift CV1 Touches above).</p>';
    await new Promise((res) => setTimeout(res, Math.max(0, 33 - (performance.now() - t))));
  }
  view.running = false;
}

// ---- runtime ------------------------------------------------------------------------------
function runtime() {
  const rt = S.rt;
  if (!rt) return `<div class="page"><div class="head"><h1>Runtime</h1></div><div class="empty">Checking…</div></div>`;
  const [cls, title, body] = rt.registered ? ['good', 'Atrium is your OpenXR runtime', 'Every OpenXR game on this PC runs through Atrium.']
    : rt.manifest ? ['warn', 'Installed, not active', 'Games still go straight to another runtime.']
    : ['bad', 'Not built yet', 'Build it with native\\build.bat; atrium_openxr.json goes next to atrium.exe.'];
  return `<div class="page">
    <div class="head"><div><h1>Atrium Runtime</h1><p class="lede">Games only ever see Atrium. It drives your headset through the stack below.</p></div></div>
    <div class="card">
      <div class="row"><div class="status grow"><div class="ring ${cls}">${icon.runtimes}</div><div><div class="big">${title}</div><p>${body}</p></div></div>
        ${rt.manifest && !rt.registered ? '<button class="btn primary" data-cmd="registerRuntime">Make Atrium the OpenXR runtime</button>' : ''}</div>
      ${rt.system_active ? `<div class="row"><span class="mono">System runtime now: ${esc(rt.system_active)}</span></div>` : ''}
      ${rt.manifest && !rt.registered ? '<div class="row"><p>This changes a Windows setting and asks for administrator permission once.</p></div>' : ''}
    </div>
    <h2>Headset driven through</h2>
    <div class="card">${rt.chosen ? `<h4>${esc(rt.chosen.label)}</h4><p>${esc(rt.reason)}</p>` : '<h4>Nothing found</h4><p>Install Meta Horizon Link, SteamVR, or on Linux Monado / WiVRn.</p>'}</div>
    <h2>Oculus SDK games</h2>
    <div class="card"><div class="row">
      <div class="status grow"><div class="ring ${S.ovr ? 'good' : 'bad'}">${icon.runtimes}</div>
        <div><div class="big">Atrium OVR ${S.ovr ? 'ready' : 'not built'}</div>
        <p>${S.ovr ? 'Games made for the Oculus SDK run on OpenXR through Atrium\'s own translation layer.' : 'Build it with native\\build.bat (native/atrium_ovr).'}</p>
        ${S.ovr ? `<p class="mono">${esc(S.ovr)}</p>` : ''}</div></div>
    </div></div>
    ${rt.backends.length ? `<h2>Available</h2><div class="card list">${rt.backends.map((b) => `<div class="item"><i class="dot ${rt.chosen && rt.chosen.manifest === b.manifest ? 'on' : ''}"></i>${esc(b.label)}<span class="sub mono">${esc(b.manifest)}</span></div>`).join('')}</div>` : ''}
  </div>`;
}

// ---- accounts -----------------------------------------------------------------------------
function accounts() {
  const busy = S.busy.signingIn;
  const waiting = (which, text) => busy === which ? `<span class="pill accent">${text}</span><button class="btn ghost" data-cmd="cancelSignIn">Cancel</button>` : null;
  return `<div class="page">
    <div class="head"><div><h1>Accounts</h1><p class="lede">Sign in to see the games you own. Installed games show without signing in.</p></div></div>
    <h2>Steam</h2>
    <div class="card">
      ${S.steam ? `<div class="account"><div class="avatar steam">${esc((S.steam.name || 'S')[0].toUpperCase())}</div>
        <div class="grow"><b>${esc(S.steam.name || S.steam.id)}</b><span>${esc(S.steam.id)}</span></div>
        <button class="btn ghost" data-signout="steam">Sign out</button></div>`
      : `<div class="row">${waiting('steam', 'Finish in your browser…') || `<button class="btn primary" data-signin="steam" ${busy ? 'disabled' : ''}>Sign in with Steam</button>`}</div>`}
      <div class="row" style="margin-top:18px"><p class="grow">Games you own but haven't installed need a Steam Web API key; Steam doesn't share the full library otherwise.</p></div>
      <div class="row">
        <input class="input" id="apikey" type="password" style="width:320px" placeholder="${S.steamKey ? 'Key saved. Paste a new one to replace it' : 'Steam Web API key'}">
        <button class="btn" id="savekey">Save key</button>
        ${S.steamKey ? '<button class="btn ghost" data-cmd="removeApiKey">Remove key</button>' : ''}
        <a class="btn ghost" href="https://steamcommunity.com/dev/apikey" data-url>Get a key</a>
      </div>
    </div>
    <h2>Meta</h2>
    <div class="card">
      ${S.meta.map((a) => `<div class="account"><div class="avatar meta">${esc((a.name || 'M')[0].toUpperCase())}</div>
        <div class="grow"><b>${esc(a.name)}</b><span>${a.valid ? 'Signed in' : 'Sign in again on this PC'}</span></div>
        <button class="btn ghost" data-signout="meta" data-id="${esc(a.id)}">Sign out</button></div>`).join('')}
      <div class="row" style="${S.meta.length ? 'margin-top:12px' : ''}">${waiting('meta', 'Sign in in the Meta window…') || `<button class="btn ${S.meta.length ? '' : 'primary'}" data-signin="meta" ${busy ? 'disabled' : ''}>${S.meta.length ? 'Add another Meta account' : 'Sign in with Meta'}</button>`}</div>
    </div>
    <h2>Games added by hand</h2>
    <div class="card">
      ${S.localGames.length ? `<div class="list">${S.localGames.map((l) => `<div class="item">${esc(l.name)}<span class="sub mono">${esc(l.exe)}</span></div>`).join('')}</div>` : '<p>Any VR game that isn\'t from Steam or Meta: pick its .exe.</p>'}
      <div class="row" style="margin-top:14px"><button class="btn" data-cmd="addGame">${icon.plus}Add a game</button></div>
    </div>
  </div>`;
}

// ---- settings -----------------------------------------------------------------------------
function settings() {
  return `<div class="page">
    <div class="head"><div><h1>Settings</h1></div></div>
    <div class="card"><div class="row">
      <div class="grow"><h4>Start Atrium when I sign in</h4><p>Atrium opens with Windows, so your headset is ready before you pick a game.</p></div>
      <span class="toggle ${S.autostart ? 'on' : ''}" id="autostart"><i></i></span>
    </div>
    <div class="row">
      <div class="grow"><h4>When I close the window</h4><p>In the tray Atrium keeps your headset ready; quitting stops Meta's runtime and streaming too.</p></div>
      <select class="input" id="closeaction" style="width:200px">
        ${[['ask', 'Ask me'], ['tray', 'Close to tray'], ['quit', 'Quit Atrium']].map(([v, l]) => `<option value="${v}" ${S.closeAction === v ? 'selected' : ''}>${l}</option>`).join('')}
      </select>
    </div>
    <div class="row">
      <div class="grow"><h4>Downloads folder</h4><p class="mono">${esc(S.downloadDir || 'Not set: Atrium asks on your first download')}</p><p>Meta games Atrium downloads go here: a folder per game, and one inside it per version.</p></div>
      <button class="btn" id="pickdl">${icon.folder}Change…</button>
    </div>
    <div class="row">
      <div class="grow"><h4>Desktop view shows</h4><p>For Oculus games that mirror one eye to the desktop window, like Echo VR. The headset isn't affected.</p></div>
      <select class="input" id="mirroreye" style="width:200px">
        ${[['left', 'Left eye'], ['right', 'Right eye']].map(([v, l]) => `<option value="${v}" ${S.mirrorEye === v ? 'selected' : ''}>${l}</option>`).join('')}
      </select>
    </div></div>
    ${dashSettings()}
    ${touchSettings()}
    ${S.riftAvailable ? riftSettings() : ''}
  </div>`;
}

// ---- dash and Rift settings (the dash shows the same; Atrium keeps one copy) --------------
const DASH_BUTTONS = [['left_menu', 'Menu'], ['x', 'X'], ['y', 'Y'], ['a', 'A'], ['b', 'B'], ['left_stick', 'Left stick'], ['right_stick', 'Right stick']];
const ACCENTS = ['Aurora', 'Mint', 'Ember', 'Orchid', 'Gold'];
const opts = (list, cur) => list.map(([v, l]) => `<option value="${esc(v)}" ${String(cur) === String(v) ? 'selected' : ''}>${esc(l)}</option>`).join('');
const setRow = (title, sub, control) => `<div class="row"><div class="grow"><h4>${title}</h4><p>${sub}</p></div>${control}</div>`;
const slider = (group, key, min, max, step, value, fmt) =>
  `<span class="slide"><input type="range" data-${group}="${key}" data-kind="num" min="${min}" max="${max}" step="${step}" value="${value}"><b data-shows="${group}.${key}">${fmt(value)}</b></span>`;
const pick = (group, key, kind, list, cur) => `<select class="input" data-${group}="${key}" data-kind="${kind}" style="width:220px">${opts(list, cur)}</select>`;

function dashSettings() {
  const d = S.dash || {};
  const remap = d.remap || {};
  return `<h2>Dash</h2><div class="card">
    ${setRow('Which dash', 'Atrium\'s own, or SteamVR\'s dashboard or Meta\'s Dash with Atrium\'s closed. Meta\'s asks Windows once to switch it back on.', pick('dash', 'provider', 'str', [['atrium', 'Atrium'], ['steamvr', 'SteamVR'], ['meta', 'Meta']], d.provider || 'atrium'))}
    ${setRow('Distance', 'How far in front of you the dash floats.', slider('dash', 'distance', 0.6, 2.5, 0.05, d.distance, (v) => `${(+v).toFixed(2)} m`))}
    ${setRow('Size', 'How wide the panel is.', slider('dash', 'width', 0.9, 3, 0.1, d.width, (v) => `${(+v).toFixed(1)} m`))}
    ${setRow('Curve', 'Flat to strongly curved.', slider('dash', 'curve', 0, 1, 0.1, d.curve, (v) => `${Math.round(v * 100)}%`))}
    ${setRow('Colour', 'The dash\'s accent colour.', pick('dash', 'accent', 'int', ACCENTS.map((n, i) => [i, n]), d.accent))}
    ${setRow('Home surroundings', 'What\'s around you with no game running.', pick('dash', 'environment', 'bool', [[true, 'Space'], [false, 'Plain']], d.environment))}
  </div>
  <h2>Dash controls</h2><div class="card">
    ${setRow('Dash button', 'The button that opens and closes the dash in games. The Oculus button works with Meta\'s runtime.', pick('dash', 'open_button', 'str', [...DASH_BUTTONS, ['oculus', 'Oculus button']], d.open_button))}
    ${setRow('Open the dash by', 'Holding: a short press still reaches the game. Pressing: the game never sees that button.', pick('dash', 'open_mode', 'str', [['hold', 'Holding it'], ['press', 'Pressing it']], d.open_mode))}
    ${setRow('Pause the game while the dash is open', 'Some games slow down while paused, which can make the dash stutter.', pick('dash', 'pause_game', 'bool', [[false, 'Keep running'], [true, 'Pause']], d.pause_game))}
  </div>
  <h2>Button remapping</h2><div class="card">
    <p style="margin:0 0 10px">What games get when you press each button. Applies from the next game start.</p>
    ${DASH_BUTTONS.map(([id, name]) => setRow(`${name}`, '', `<select class="input" data-remap="${id}" style="width:220px">${opts([[id, 'Same'], ...DASH_BUTTONS.filter(([b]) => b !== id)], remap[id] || id)}</select>`)).join('')}
    <div class="row"><div class="grow"></div><button class="btn" id="remapreset">Reset all</button></div>
  </div>`;
}

// Where the CV1 Touches sit in the hand in SteamVR (TouchLink applies it at once).
function touchSettings() {
  const o = S.touch || {};
  const deg = (v) => `${(+v).toFixed(0)}°`, cm = (v) => `${(+v).toFixed(1)} cm`;
  return `<h2>Touch controllers in SteamVR</h2><div class="card">
    <p style="margin:0 0 10px">If your CV1 Touches look tilted or set off in your hands (Mix & match, TouchLink). Changes apply at once, on top of the calibration.</p>
    ${setRow('Tilt', 'Tip up or down.', slider('touch', 'pitch', -45, 45, 1, o.pitch || 0, deg))}
    ${setRow('Turn', 'Point left or right.', slider('touch', 'yaw', -45, 45, 1, o.yaw || 0, deg))}
    ${setRow('Roll', 'Roll left or right.', slider('touch', 'roll', -45, 45, 1, o.roll || 0, deg))}
    ${setRow('Move right / left', '', slider('touch', 'x', -10, 10, 0.5, o.x || 0, cm))}
    ${setRow('Move up / down', '', slider('touch', 'y', -10, 10, 0.5, o.y || 0, cm))}
    ${setRow('Move back / forward', '', slider('touch', 'z', -10, 10, 0.5, o.z || 0, cm))}
    <div class="row"><div class="grow"></div><button class="btn" id="touchreset">Reset</button></div>
  </div>`;
}

function riftSettings() {
  const r = S.rift || {};
  const ss = [[0, 'Game default'], ...[0.8, 1, 1.2, 1.4, 1.5, 1.6, 1.8, 2, 2.5].map((v) => [v, `${v.toFixed(1)}×`])];
  return `<h2>Rift (Meta runtime)</h2><div class="card">
    <p style="margin:0 0 10px">For wired Rift headsets. Atrium applies these to Meta's runtime now and every time it starts it.</p>
    ${setRow('Supersampling', 'Renders games sharper at a GPU cost (pixels per display pixel).', pick('rift', 'supersampling', 'num', ss, r.supersampling))}
    ${setRow('Asynchronous SpaceWarp', 'Auto lets Meta\'s runtime fill in frames when a game falls behind; locking holds the game at a lower rate.', pick('rift', 'asw', 'str', [['auto', 'Auto'], ['off', 'Off'], ['45', 'Locked 45 Hz'], ['30', 'Locked 30 Hz'], ['18', 'Locked 18 Hz']], r.asw))}
    ${setRow('Field of view, horizontal', 'Below 100% renders less of the edges for more speed; above it renders a wider view.', slider('rift', 'fov_x', 0.5, 2, 0.05, r.fov_x, (v) => `${Math.round(v * 100)}%`))}
    ${setRow('Field of view, vertical', '', slider('rift', 'fov_y', 0.5, 2, 0.05, r.fov_y, (v) => `${Math.round(v * 100)}%`))}
    ${setRow('Performance overlay', 'Meta\'s performance HUD inside the headset.', pick('rift', 'perf_hud', 'int', [[0, 'Off'], [1, 'Summary'], [2, 'Latency'], [3, 'App render'], [4, 'Compositor'], [5, 'ASW'], [6, 'Version']], r.perf_hud))}
    ${setRow('Adaptive GPU performance scale', 'Lowers resolution when the GPU falls behind.', pick('rift', 'adaptive_gpu', 'bool', [[false, 'Off'], [true, 'On']], r.adaptive_gpu))}
    ${setRow('Mipmaps on all layers', 'Smoother distant text on menus and overlays.', pick('rift', 'mipmaps_all_layers', 'bool', [[false, 'Off'], [true, 'On']], r.mipmaps_all_layers))}
    ${setRow('FOV stencil', 'Skips pixels the lenses never show.', pick('rift', 'fov_stencil', 'bool', [[true, 'On'], [false, 'Off']], r.fov_stencil))}
    ${setRow('Touch controllers never sleep', 'Keeps them awake between games; uses more battery.', pick('rift', 'touch_never_sleep', 'bool', [[false, 'Off'], [true, 'On']], r.touch_never_sleep))}
  </div>`;
}

// Which settings group a control belongs to (data-dash, data-rift, data-touch).
const groupOf = (el) => ['dash', 'rift', 'touch'].find((g) => el.dataset && el.dataset[g] !== undefined);

// A changed dash/Rift/Touch control: the whole group goes to Atrium.
function settingChanged(el) {
  const group = groupOf(el);
  const key = el.dataset[group];
  const kind = el.dataset.kind;
  const v = el.value;
  const val = kind === 'num' ? parseFloat(v) : kind === 'int' ? parseInt(v, 10) : kind === 'bool' ? v === 'true' : v;
  const obj = { ...(S[group] || {}), [key]: val };
  if (group === 'dash') send('setDash', { dash: obj });
  else if (group === 'touch') send('setTouch', { touch: obj });
  else send('setRift', { rift: obj });
}

// ---- render -------------------------------------------------------------------------------
function pageSig() {
  switch (ui.page) {
    case 'library': return JSON.stringify([S.games, Object.keys(S.downloads), S.vrOnly, S.busy.scanning, S.busy.refreshing, S.busy.launching, ui.search, ui.source, ui.selected]);
    case 'devices': return JSON.stringify(S.hw);
    case 'runtime': return JSON.stringify([S.rt, S.ovr]);
    case 'mix': return JSON.stringify([S.mix, (S.hw?.headsets || []).map((h) => [h.name, h.via, h.active])]);
    case 'settings': return JSON.stringify([S.autostart, S.closeAction, S.mirrorEye, S.downloadDir, S.dash, S.rift, S.riftAvailable, S.touch]);
    default: return JSON.stringify([S.steam, S.steamKey, S.meta, S.localGames, S.busy.signingIn]);
  }
}

// Mix & match changes by the second (the room, the Touches): replace only the cards that changed,
// so the rest (the live sensor view, an open dropdown, the scroll) stays as it is.
function patchPage(main, html) {
  const t = document.createElement('template');
  t.innerHTML = html.trim();
  const fresh = t.content.firstElementChild, cur = main.firstElementChild;
  const parts = [...(fresh?.children || [])].map((c) => c.outerHTML);
  if (!cur || !drawn.parts || drawn.parts.length !== parts.length || drawn.partsPage !== ui.page) {
    main.innerHTML = html;
  } else {
    parts.forEach((h, i) => { if (drawn.parts[i] !== h) cur.children[i].replaceWith(fresh.children[i].cloneNode(true)); });
  }
  drawn.parts = parts;
  drawn.partsPage = ui.page;
}

function render() {
  if (!S) return;
  renderSide();
  const main = $('#main');
  if (drawn.page !== ui.page) {
    drawn.page = ui.page; drawn.sig = null; drawn.parts = null;
    main.scrollTop = 0;
    if (ui.page === 'library') main.innerHTML = libraryShell();
  }
  const sig = pageSig();
  if (sig !== drawn.sig) {
    drawn.sig = sig;
    if (ui.page === 'library') libraryBody();
    else if (ui.page === 'mix') patchPage(main, mix());
    else main.innerHTML = { devices, mix, runtime, accounts, settings }[ui.page]();
    if (ui.page === 'mix' && view.on) viewLoop();
    if (ui.page === 'mix') roomLiveLoop();
  }
  renderDrawer();
  paintDownloads();
}

function go(page) { ui.page = page; if (page !== 'library') ui.selected = null; render(); }

// ---- events -------------------------------------------------------------------------------
document.addEventListener('click', (e) => {
  const t = e.target.closest('[data-page],[data-play],[data-game],[data-close],[data-cmd],[data-signin],[data-signout],[data-folder],[data-remove],[data-src],[data-url],[data-download],[data-cancel-dl],[data-remove-dl],[data-use-version],#vr,#autostart,#refresh,#savekey,#pickdl,#viewtoggle,#remapreset');
  if (!t) return;
  const d = t.dataset;
  if (d.url !== undefined) { e.preventDefault(); send('openUrl', { url: t.href }); }
  else if (d.page) go(d.page);
  else if (d.play) { e.stopPropagation(); send('launch', { id: d.play }); }
  else if (d.game) { ui.selected = d.game; render(); }
  else if (d.close !== undefined) { ui.selected = null; render(); }
  else if (d.cmd) send(d.cmd, d.serial ? { serial: d.serial } : undefined);
  else if (d.signin) send('signIn', { which: d.signin });
  else if (d.signout) send('signOut', { which: d.signout, id: d.id });
  else if (d.folder) send('openFolder', { id: d.folder });
  else if (d.download) { const g = S.games.find((x) => x.app_id === d.download && x.source === 'meta'); const b = g && latest(g); if (b) send('download', { appId: d.download, binary: b.binary_id }); }
  else if (d.cancelDl) send('cancelDownload', { appId: d.cancelDl });
  else if (d.useVersion) send('useVersion', { appId: S.games.find((x) => x.id === ui.selected)?.app_id, version: d.useVersion });
  else if (d.removeDl) {
    const [appId, version] = d.removeDl.split('|');
    if (ui.confirmRemove === d.removeDl) { ui.confirmRemove = null; send('removeDownload', { appId, version }); }
    else { ui.confirmRemove = d.removeDl; render(); }
  }
  else if (d.remove) { ui.selected = null; send('removeGame', { id: d.remove }); }
  else if (d.src) { ui.source = d.src; document.querySelectorAll('#src button').forEach((b) => b.classList.toggle('on', b === t)); render(); }
  else if (t.id === 'vr') send('setVrOnly', { on: !S.vrOnly });
  else if (t.id === 'autostart') send('setAutostart', { on: !S.autostart });
  else if (t.id === 'refresh') send('refresh');
  else if (t.id === 'pickdl') send('pickDownloadDir');
  else if (t.id === 'remapreset') send('setDash', { dash: { ...S.dash, remap: {} } });
  else if (t.id === 'touchreset') send('setTouch', { touch: { pitch: 0, yaw: 0, roll: 0, x: 0, y: 0, z: 0 } });
  else if (t.id === 'viewtoggle') {
    view.on = !view.on;
    t.classList.toggle('on', view.on);
    const box = $('#views');
    if (box) box.innerHTML = view.on ? '' : '<p class="sub-l">Turn on the live view to see what each sensor sees.</p>';
    if (view.on) viewLoop();
  }
  else if (t.id === 'savekey') { const k = $('#apikey'); send('saveApiKey', { key: k.value }); k.value = ''; }
});
document.addEventListener('dblclick', (e) => { const g = e.target.closest('.game'); if (g) send('launch', { id: g.dataset.game }); });
document.addEventListener('input', (e) => {
  if (e.target.type === 'range' && e.target.dataset.kind === 'num') {
    const group = groupOf(e.target);
    const key = e.target.dataset[group];
    const b = document.querySelector(`[data-shows="${group}.${key}"]`);
    const v = +e.target.value;
    if (b) b.textContent = group === 'touch' ? (['pitch', 'yaw', 'roll'].includes(key) ? `${v.toFixed(0)}°` : `${v.toFixed(1)} cm`)
      : key === 'distance' ? `${v.toFixed(2)} m` : key === 'width' ? `${v.toFixed(1)} m` : `${Math.round(v * 100)}%`;
  }
  if (e.target.id === 'q') { ui.search = e.target.value; render(); }
  if (e.target.id === 'viewgain') view.gain = Number(e.target.value);
});
document.addEventListener('change', (e) => {
  if (e.target.id === 'mode') send('setMode', { id: ui.selected, mode: e.target.value });
  if (e.target.id === 'version') { const g = S.games.find((x) => x.id === ui.selected); if (g) { ui.version[g.app_id] = e.target.value; render(); } }
  if (e.target.id === 'closeaction') send('setCloseAction', { value: e.target.value });
  if (e.target.id === 'mirroreye') send('setMirrorEye', { value: e.target.value });
  if (groupOf(e.target)) settingChanged(e.target);
  if (e.target.dataset && e.target.dataset.remap !== undefined) {
    const remap = { ...((S.dash || {}).remap || {}) };
    const from = e.target.dataset.remap;
    if (e.target.value === from) delete remap[from]; else remap[from] = e.target.value;
    send('setDash', { dash: { ...S.dash, remap } });
  }
  if (e.target.id === 'mixcontrollers') send('mixSet', { enabled: e.target.value === 'cv1' });
  if (e.target.id === 'mixtracking') send('mixTracking', { tracking: e.target.value });
});
document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape' && ui.selected) { ui.selected = null; render(); }
  if (e.key === 'Enter' && e.target.classList?.contains('game')) { ui.selected = e.target.dataset.game; render(); }
  if (e.key === 'Enter' && e.target.id === 'apikey') $('#savekey').click();
});
document.addEventListener('contextmenu', (e) => { if (!e.target.closest('input')) e.preventDefault(); });

function notice(text) {
  const el = document.createElement('div');
  el.className = 'toast';
  el.textContent = text;
  $('#toasts').append(el);
  setTimeout(() => { el.classList.add('out'); setTimeout(() => el.remove(), 300); }, 4500);
}

// Window chrome: drag from the title bar or the sidebar's empty space, double-click to maximise,
// resize from the edges (src/ui.rs does the moving).
const RESTORE = '<svg viewBox="0 0 12 12"><rect x="1.5" y="3.5" width="7" height="7" rx="1"/><path d="M3.5 3.5V2.5a1 1 0 0 1 1-1h5a1 1 0 0 1 1 1v5a1 1 0 0 1-1 1h-1"/></svg>';
const MAXIMIZE = '<svg viewBox="0 0 12 12"><rect x="1.5" y="1.5" width="9" height="9" rx="1"/></svg>';
const draggable = (t) => (t.closest('[data-drag]') || t.id === 'side' || t.classList?.contains('side-gap')) && !t.closest('button,input,select,a,.nav,.mini');
document.addEventListener('mousedown', (e) => {
  if (e.button !== 0) return;
  const edge = e.target.closest('[data-edge]');
  if (edge) { e.preventDefault(); send('window', { action: 'resize', edge: edge.dataset.edge }); return; }
  if (e.detail === 1 && draggable(e.target)) { e.preventDefault(); send('window', { action: 'drag' }); }
});
document.addEventListener('dblclick', (e) => { if (draggable(e.target)) send('window', { action: 'maximize' }); });
document.addEventListener('click', (e) => { const b = e.target.closest('[data-win]'); if (b) send('window', { action: b.dataset.win }); });
function maximized(on) {
  document.body.classList.toggle('maxed', on);
  const b = $('#maxbtn');
  if (b) { b.innerHTML = on ? RESTORE : MAXIMIZE; b.title = on ? 'Restore' : 'Maximise'; }
}

// Closing the window: ask "Close to tray instead?", or do what the user chose in Settings.
// In the tray Atrium keeps running (OVRServer, streaming); the tray icon brings it back.
function requestClose() {
  const choice = S?.closeAction || 'ask';
  if (choice !== 'ask') { send('window', { action: choice === 'tray' ? 'hide' : 'quit' }); return; }
  if ($('#closeask')) return;
  const el = document.createElement('div');
  el.id = 'closeask';
  el.className = 'modal';
  el.innerHTML = `<div class="scrim"></div><div class="box" role="dialog" aria-modal="true" aria-labelledby="closeask-t">
    <h3 id="closeask-t">Close to tray instead?</h3>
    <p>In the tray Atrium keeps your headset ready (Meta's runtime for a wired Rift, streaming for Quest and Pico). Quitting stops all of it.</p>
    <label class="check"><input type="checkbox" id="closeask-remember"> Don't ask again (change it in Settings)</label>
    <div class="row"><button class="btn ghost" data-close-choice="cancel">Cancel</button>
      <button class="btn" data-close-choice="quit">Quit Atrium</button>
      <button class="btn primary" data-close-choice="tray">Close to tray</button></div></div>`;
  document.body.append(el);
  el.querySelector('[data-close-choice="tray"]').focus();
}
document.addEventListener('click', (e) => {
  const b = e.target.closest('[data-close-choice]');
  if (!b) { if (e.target.closest('#closeask .scrim')) $('#closeask')?.remove(); return; }
  const choice = b.dataset.closeChoice;
  const remember = $('#closeask-remember')?.checked;
  $('#closeask')?.remove();
  if (choice === 'cancel') return;
  if (remember) send('setCloseAction', { value: choice });
  send('window', { action: choice === 'tray' ? 'hide' : 'quit' });
});
document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && $('#closeask')) $('#closeask').remove(); });

window.atrium = { update(s) { S = s; render(); }, notice, maximized, requestClose };
send('ready');
