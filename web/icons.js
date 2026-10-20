// Line-art icons drawn for Atrium. No vendor artwork: each headset family gets a silhouette
// that's recognisable without copying anyone's product images.
const S = (body, vb = '0 0 96 64') => `<svg viewBox="${vb}" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${body}</svg>`;

export const headsetIcons = {
  // Rift CV1: soft front, rigid side arms, on-ear headphones, top strap
  rift: S(`<path d="M22 20c0-6 6-10 26-10s26 4 26 10v16c0 8-6 12-14 12-6 0-8-6-12-6s-6 6-12 6c-8 0-14-4-14-12z"/>
    <path d="M22 26h-8M74 26h8"/><circle cx="12" cy="40" r="7"/><circle cx="84" cy="40" r="7"/><path d="M12 33v-7M84 33v-7"/>
    <path d="M36 11c2-5 22-5 24 0"/>`),
  // Rift S: halo band around a front box
  rifts: S(`<path d="M24 20c0-5 6-8 24-8s24 3 24 8v14c0 8-5 12-12 12-5 0-8-5-12-5s-7 5-12 5c-7 0-12-4-12-12z"/>
    <ellipse cx="48" cy="24" rx="44" ry="10"/><circle cx="34" cy="20" r="2"/><circle cx="62" cy="20" r="2"/>`),
  // Quest: wide rounded body, four corner cameras
  quest: S(`<rect x="14" y="12" width="68" height="36" rx="16"/><circle cx="24" cy="20" r="2.5"/><circle cx="72" cy="20" r="2.5"/>
    <circle cx="24" cy="40" r="2.5"/><circle cx="72" cy="40" r="2.5"/><path d="M14 30H6M82 30h8"/>`),
  // Index: angled front plate, off-ear speakers
  index: S(`<path d="M20 16l10-6h36l10 6v22l-8 10H28l-8-10z"/><path d="M38 22h20"/>
    <rect x="4" y="30" width="10" height="16" rx="4"/><rect x="82" y="30" width="10" height="16" rx="4"/><path d="M14 30l6-4M82 30l-6-4"/>`),
  // Vive: front dotted with sensor dimples
  vive: S(`<rect x="16" y="12" width="64" height="36" rx="12"/>
    ${[0, 1, 2, 3, 4].map((i) => `<circle cx="${28 + i * 10}" cy="22" r="2"/>`).join('')}
    ${[0, 1, 2, 3].map((i) => `<circle cx="${33 + i * 10}" cy="36" r="2"/>`).join('')}<path d="M16 30H6M80 30h10"/>`),
  // Steam Frame: slim visor, band
  frame: S(`<path d="M18 22c0-6 8-10 30-10s30 4 30 10v10c0 7-6 10-14 10H32c-8 0-14-3-14-10z"/>
    <path d="M26 26h44" stroke-width="5" opacity=".55"/><path d="M18 26H6M78 26h12"/><circle cx="28" cy="18" r="1.5"/><circle cx="68" cy="18" r="1.5"/>`),
  // Windows Mixed Reality: visor with two forward cameras
  wmr: S(`<path d="M16 18c0-4 10-8 32-8s32 4 32 8v18c0 8-8 12-16 12H32c-8 0-16-4-16-12z"/><circle cx="30" cy="24" r="4"/><circle cx="66" cy="24" r="4"/><path d="M48 10V4"/>`),
  pimax: S(`<path d="M8 22c0-6 16-12 40-12s40 6 40 12v12c0 8-8 12-18 12H26c-10 0-18-4-18-12z"/><path d="M30 28h36"/>`),
  beyond: S(`<rect x="20" y="20" width="22" height="18" rx="9"/><rect x="54" y="20" width="22" height="18" rx="9"/><path d="M42 28h12M20 28H8M76 28h12"/>`),
  generic: S(`<rect x="14" y="14" width="68" height="32" rx="12"/><path d="M14 30H6M82 30h8M40 46c2-6 14-6 16 0"/>`),
};

export const icon = {
  controller: S(`<circle cx="40" cy="22" r="16"/><path d="M40 38v20"/><circle cx="40" cy="22" r="5"/>`, '0 0 80 64'),
  sensor: S(`<circle cx="32" cy="22" r="12"/><circle cx="32" cy="22" r="4"/><path d="M32 34v16M20 54h24"/>`, '0 0 64 64'),
  wifi: S(`<path d="M8 24c14-14 34-14 48 0M16 32c9-9 23-9 32 0M24 40c4-4 12-4 16 0"/><circle cx="32" cy="48" r="2"/>`, '0 0 64 64'),
  home: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 11l9-7 9 7v9a1 1 0 0 1-1 1h-5v-6h-6v6H4a1 1 0 0 1-1-1z"/></svg>',
  library: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="7" rx="1.5"/><rect x="3" y="14" width="7" height="7" rx="1.5"/><rect x="14" y="14" width="7" height="7" rx="1.5"/></svg>',
  devices: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="2" y="7" width="20" height="10" rx="4"/><path d="M8 17c1-2 7-2 8 0"/></svg>',
  runtimes: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 7h10M4 17h6M18 7h2M14 17h6"/><circle cx="16" cy="7" r="2"/><circle cx="12" cy="17" r="2"/></svg>',
  accounts: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="8" r="4"/><path d="M4 21c1-4 4-6 8-6s7 2 8 6"/></svg>',
  settings: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z"/></svg>',
  play: '<svg viewBox="0 0 24 24" fill="currentColor"><path d="M8 5v14l11-7z"/></svg>',
  search: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="11" cy="11" r="7"/><path d="M20 20l-4-4"/></svg>',
  refresh: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M20 11a8 8 0 0 0-14-5L4 8M4 4v4h4M4 13a8 8 0 0 0 14 5l2-2M20 20v-4h-4"/></svg>',
  plus: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M12 5v14M5 12h14"/></svg>',
  folder: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linejoin="round"><path d="M3 6a1 1 0 0 1 1-1h5l2 2h9a1 1 0 0 1 1 1v10a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1z"/></svg>',
  close: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M6 6l12 12M18 6L6 18"/></svg>',
  logo: '<svg viewBox="0 0 32 32" fill="none"><defs><linearGradient id="lg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#7c9cff"/><stop offset="1" stop-color="#3be0c4"/></linearGradient></defs><path d="M6 28V15a10 10 0 0 1 20 0v13" stroke="url(#lg)" stroke-width="3.2" stroke-linecap="round"/><path d="M12 28v-9a4 4 0 0 1 8 0v9" stroke="url(#lg)" stroke-width="3.2" stroke-linecap="round" opacity=".6"/></svg>',
};

export const sourceBadge = {
  steam: 'Steam',
  meta: 'Meta',
  local: 'Local',
};
