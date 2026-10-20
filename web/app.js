// Atrium's page. It keeps no data of its own: Rust (src/app.rs) pushes a full snapshot with
// atrium.update(...) and the page redraws from it. Actions go back with ipc.postMessage.
import { headsetIcons, icon } from './icons.js';

let S = null;
const ui = { page: 'library', selected: null, search: '', source: 'all' };
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
const PAGES = [['library', 'Library', icon.library], ['devices', 'Devices', icon.devices], ['runtime', 'Runtime', icon.runtimes], ['accounts', 'Accounts', icon.accounts], ['settings', 'Settings', icon.settings]];

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
    <div class="sub">${g.installed ? (g.lastPlayed || g.last_played ? ago(g.last_played) : 'Installed') : 'Not installed'}</div>
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

// ---- drawer -------------------------------------------------------------------------------
function renderDrawer() {
  const d = $('#drawer');
  const g = ui.selected && S?.games.find((x) => x.id === ui.selected);
  const sig = g ? JSON.stringify([g, S.modes[g.id], S.starts[g.id], S.ovr, S.busy.launching]) : null;
  if (sig === drawn.drawer) return;
  drawn.drawer = sig;
  if (!g) { d.hidden = true; d.innerHTML = ''; return; }
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
            : `<button class="btn primary big" data-play="${esc(g.id)}">${g.source === 'steam' ? 'Install' : 'Get in Meta app'}</button>`}
          ${g.install_dir && g.installed ? `<button class="btn big icon-btn" data-folder="${esc(g.id)}" title="Open folder">${icon.folder}</button>` : ''}
        </div>
        ${g.installed ? `<div class="field"><label>Start</label>
          <select class="input" id="mode" style="width:100%">${modes.map(([k, l]) => `<option value="${k}" ${k === mode ? 'selected' : ''}>${l}</option>`).join('')}</select>
          <div class="hint">Will start: ${esc(S.starts[g.id] || '')}</div></div>` : ''}
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
    </div></div>
  </div>`;
}

// ---- render -------------------------------------------------------------------------------
function pageSig() {
  switch (ui.page) {
    case 'library': return JSON.stringify([S.games, S.vrOnly, S.busy.scanning, S.busy.refreshing, S.busy.launching, ui.search, ui.source, ui.selected]);
    case 'devices': return JSON.stringify(S.hw);
    case 'runtime': return JSON.stringify([S.rt, S.ovr]);
    case 'settings': return JSON.stringify([S.autostart]);
    default: return JSON.stringify([S.steam, S.steamKey, S.meta, S.localGames, S.busy.signingIn]);
  }
}

function render() {
  if (!S) return;
  renderSide();
  const main = $('#main');
  if (drawn.page !== ui.page) {
    drawn.page = ui.page; drawn.sig = null;
    main.scrollTop = 0;
    if (ui.page === 'library') main.innerHTML = libraryShell();
  }
  const sig = pageSig();
  if (sig !== drawn.sig) {
    drawn.sig = sig;
    if (ui.page === 'library') libraryBody();
    else main.innerHTML = { devices, runtime, accounts, settings }[ui.page]();
  }
  renderDrawer();
}

function go(page) { ui.page = page; if (page !== 'library') ui.selected = null; render(); }

// ---- events -------------------------------------------------------------------------------
document.addEventListener('click', (e) => {
  const t = e.target.closest('[data-page],[data-play],[data-game],[data-close],[data-cmd],[data-signin],[data-signout],[data-folder],[data-remove],[data-src],[data-url],#vr,#autostart,#refresh,#savekey');
  if (!t) return;
  const d = t.dataset;
  if (d.url !== undefined) { e.preventDefault(); send('openUrl', { url: t.href }); }
  else if (d.page) go(d.page);
  else if (d.play) { e.stopPropagation(); send('launch', { id: d.play }); }
  else if (d.game) { ui.selected = d.game; render(); }
  else if (d.close !== undefined) { ui.selected = null; render(); }
  else if (d.cmd) send(d.cmd);
  else if (d.signin) send('signIn', { which: d.signin });
  else if (d.signout) send('signOut', { which: d.signout, id: d.id });
  else if (d.folder) send('openFolder', { id: d.folder });
  else if (d.remove) { ui.selected = null; send('removeGame', { id: d.remove }); }
  else if (d.src) { ui.source = d.src; document.querySelectorAll('#src button').forEach((b) => b.classList.toggle('on', b === t)); render(); }
  else if (t.id === 'vr') send('setVrOnly', { on: !S.vrOnly });
  else if (t.id === 'autostart') send('setAutostart', { on: !S.autostart });
  else if (t.id === 'refresh') send('refresh');
  else if (t.id === 'savekey') { const k = $('#apikey'); send('saveApiKey', { key: k.value }); k.value = ''; }
});
document.addEventListener('dblclick', (e) => { const g = e.target.closest('.game'); if (g) send('launch', { id: g.dataset.game }); });
document.addEventListener('input', (e) => { if (e.target.id === 'q') { ui.search = e.target.value; render(); } });
document.addEventListener('change', (e) => { if (e.target.id === 'mode') send('setMode', { id: ui.selected, mode: e.target.value }); });
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

window.atrium = { update(s) { S = s; render(); }, notice, maximized };
send('ready');
