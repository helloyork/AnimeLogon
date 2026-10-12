'use strict';
/* The settings app's page. The native side (app/src/host.cpp) holds the themes and the settings
   and draws every picture of a lock screen; this page lays them out, sends what the person does,
   and draws its marks (selection, anchors) over the pictures. */

const ORIGIN = 'https://animelogon.invalid/';
const native = window.chrome && window.chrome.webview;

/* ================= helpers ================= */
const $ = (s, r = document) => r.querySelector(s);
function h(tag, attrs, ...kids) {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v == null || v === false) continue;
    if (k === 'class') n.className = v;
    else if (k === 'style') Object.assign(n.style, v);
    else if (k === 'vars') for (const [name, value] of Object.entries(v)) n.style.setProperty(name, value);
    else if (k.startsWith('on') && typeof v === 'function') n.addEventListener(k.slice(2), v);
    else if (k === 'html') n.innerHTML = v;
    else n.setAttribute(k, v === true ? '' : v);
  }
  for (const c of kids.flat(Infinity)) { if (c == null || c === false) continue; n.append(c.nodeType ? c : document.createTextNode(String(c))); }
  return n;
}
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
const decimals = step => { const s = String(step), i = s.indexOf('.'); return i < 0 ? 0 : s.length - i - 1; };
const roundTo = (v, step) => +(Math.round(v / step) * step).toFixed(decimals(step));
const signed = (v, d = 1) => (v > 0 ? '+' : v < 0 ? '−' : '') + Math.abs(+v).toFixed(d);
function toast(msg) { const t = $('#toast'); t.textContent = msg; t.classList.add('show'); clearTimeout(t._h); t._h = setTimeout(() => t.classList.remove('show'), 3000); }
const ICON = {
  undo: '<svg viewBox="0 0 20 20" width="18" height="18" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M7 4.5 3.5 8 7 11.5"/><path d="M4 8h7.5a4.5 4.5 0 0 1 0 9H9"/></svg>',
  redo: '<svg viewBox="0 0 20 20" width="18" height="18" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M13 4.5 16.5 8 13 11.5"/><path d="M16 8H8.5a4.5 4.5 0 0 0 0 9H11"/></svg>',
  close: '<svg viewBox="0 0 20 20" width="16" height="16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"><path d="M5 5l10 10M15 5 5 15"/></svg>',
  more: '<svg viewBox="0 0 20 20" width="16" height="16" fill="currentColor"><circle cx="4.5" cy="10" r="1.6"/><circle cx="10" cy="10" r="1.6"/><circle cx="15.5" cy="10" r="1.6"/></svg>',
  chev: '<svg viewBox="0 0 20 20" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="m8 5 5 5-5 5"/></svg>',
  down: '<svg viewBox="0 0 20 20" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="m5 8 5 5 5-5"/></svg>',
  reset: '<svg viewBox="0 0 20 20" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M4.5 9.5a5.5 5.5 0 1 1 1.7 4"/><path d="M4.5 5v4.5H9"/></svg>',
  // The Windows convention for "this asks for administrator approval".
  shield: '<svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true"><path d="M8 1 2.5 3.2v4.3C2.5 11 4.8 13.6 8 15c3.2-1.4 5.5-4 5.5-7.5V3.2z" fill="var(--uac-a)"/><path d="M8 1v7H2.5v-.5V3.2zM8 8h5.5c0 3.5-2.3 6.1-5.5 7.5z" fill="var(--uac-b)"/></svg>',
};
const shield = () => h('span', { class: 'uac', title: '需要管理员确认', html: ICON.shield });

const ANCHORS = ['top-left', 'top', 'top-right', 'left', 'center', 'right', 'bottom-left', 'bottom', 'bottom-right'];
const ANCHOR_NAMES = ['左上', '上方居中', '右上', '左侧居中', '居中', '右侧居中', '左下', '下方居中', '右下'];
const WEIGHT_NAMES = { 100: '极细', 200: '特细', 300: '细', 400: '常规', 500: '中等', 600: '半粗', 700: '粗', 800: '特粗', 900: '黑' };
const SWATCHES = [['白色', '#FFFFFF'], ['暖白', '#FFF1DC'], ['樱粉', '#FFC6D6'], ['天蓝', '#A8D8FF'], ['金色', '#FFD27A'], ['墨色', '#16181F']];
const COMMON_TEXT = ['font', 'weight', 'size', 'color'];

/* ================= state ================= */
let L = null;  // the library: shared settings, themes, wallpapers, components
let T = null;  // the theme open on its page, in detail
let ENV = { monitors: [], fonts: [], devices: [], on: false, installed: false };
const UI = { view: 'library', tab: 'mine', sel: { inst: null, part: null }, hover: null, more: false, busy: null, tick: 0 };
// The open theme's picture and the edits on their way to it. Edits made while a picture is being
// drawn wait, and only the newest of them is sent.
const PV = { rev: 0, shown: 0, seq: 0, width: 0, height: 0, bounds: [], asked: '', waiting: false, pending: null, commit: false };
let gestures = 0;

function send(op, args) { if (native) native.postMessage(Object.assign({ op }, args || {})); }

function receive(m) {
  if (!m || !m.type) return;
  switch (m.type) {
    case 'environment': ENV = m; render(); break;
    case 'state':
      L = m.library; T = m.open; UI.clock = m.clock;
      if (UI.view === 'theme' && !T) UI.view = 'library';
      if (T && UI.sel.inst && !T.instances.some(i => i.id === UI.sel.inst)) UI.sel = { inst: null, part: null };
      render();
      break;
    case 'preview': onPreview(m); break;
    case 'toast': toast(m.text); break;
    case 'problem': problem(m.text); break;
    case 'busy': UI.busy = m.label ? { label: m.label, percent: m.percent } : null; renderBusy(); break;
    case 'added': UI.sel = { inst: m.instance, part: null }; break;
    case 'navigate': UI.view = 'theme'; UI.sel = { inst: null, part: null }; break;
  }
}
if (native) native.addEventListener('message', e => receive(e.data));

/* ================= what the library says ================= */
const theme = id => L.themes.find(t => t.id === id);
const perMonitor = () => L.settings.monitorMode === 'per-monitor';
const mainMonitor = () => ENV.monitors.find(m => m.primary) || ENV.monitors[0] || { key: '', name: '显示器', width: 16, height: 9 };
const screenTheme = m => L.settings.screens[m.key] && theme(L.settings.screens[m.key]) ? L.settings.screens[m.key] : L.current;
// The display a theme is shown on: the one it is applied to in per-monitor mode, otherwise the main one.
function screenOf(id) { return (perMonitor() && ENV.monitors.find(m => L.settings.screens[m.key] === id)) || mainMonitor(); }
function shapeVars(id) { const m = screenOf(id); return { '--arw': String(m.width), '--arh': String(m.height) }; }
function usedOn(id) {
  if (!perMonitor()) return L.current === id ? ['使用中'] : [];
  const ms = ENV.monitors.filter(m => screenTheme(m) === id);
  return ms.length && ms.length === ENV.monitors.length ? ['所有屏幕'] : ms.map(m => m.name);
}
const kindName = k => ({ video: '视频', image: '图片', builtin: '内置背景', none: '无壁纸', missing: '壁纸缺失' })[k] || '';
function ratioLabel(w, hgt) {
  const known = [[16, 9], [16, 10], [21, 9], [32, 9], [4, 3], [3, 2]], r = w / hgt;
  const best = known.reduce((a, b) => Math.abs(b[0] / b[1] - r) < Math.abs(a[0] / a[1] - r) ? b : a);
  return best[0] + ':' + best[1];
}
const thumbUrl = (t, w) => `${ORIGIN}thumb/${t.id}.bmp?w=${w}&r=${t.revision}.${UI.tick}`;

/* ================= the page ================= */
function render() {
  if (!L) return;
  closeMenu();
  const app = $('#app');
  const focused = document.activeElement && document.activeElement.id;
  if (UI.view === 'theme' && T) {
    if (app.dataset.theme !== T.id || !$('#stage')) { app.dataset.theme = T.id; app.replaceChildren(themePage()); }
    renderHead(); renderSide(); renderPreviewParts(); fitPreview(); updateSel();
  } else {
    UI.view = 'library';
    const old = $('.scroll'), top = old && app.dataset.tab === UI.tab ? old.scrollTop : 0;
    app.dataset.theme = ''; app.dataset.tab = UI.tab;
    app.replaceChildren(libraryPage());
    const scroller = $('.scroll'); if (scroller) scroller.scrollTop = top;
  }
  renderBusy();
  if (focused) { const f = document.getElementById(focused); if (f) f.focus(); }
}

function appTop() {
  const tabs = [['mine', '主题库'], ['discover', '发现'], ['settings', '设置']], cur = UI.view === 'theme' ? 'mine' : UI.tab;
  return h('header', { class: 'top' },
    h('div', { class: 'brand' }, 'AnimeLogon'),
    h('nav', { class: 'tabs', 'aria-label': '页面' }, tabs.map(([k, l]) => h('button', { 'aria-current': cur === k ? 'page' : null, onclick: () => { if (UI.view === 'theme') send('close'); UI.view = 'library'; UI.tab = k; render(); } }, l))),
    h('div', { class: 'spacer' }),
    h('button', { class: 'btn', onclick: () => send('import') }, [shield(), '导入…']),
    h('button', { class: 'btn accent', onclick: newThemeFlow }, '新建主题'));
}
const busyBar = () => h('div', { class: 'busybar', id: 'busy', hidden: true });
function renderBusy() {
  const b = $('#busy'); if (!b) return;
  b.hidden = !UI.busy;
  if (UI.busy) b.replaceChildren(h('span', null, UI.busy.label), h('div', { class: 'bar' }, h('i', { style: { width: (UI.busy.percent || 0) + '%' } })), h('span', { class: 'muted' }, (UI.busy.percent || 0) + '%'));
}

/* ================= library ================= */
function libraryPage() {
  const body = h('main', { class: 'scroll' }, UI.tab === 'mine' ? mineView() : UI.tab === 'discover' ? discoverView() : settingsView());
  return h('div', { class: 'view' }, appTop(), busyBar(), body);
}
function openTheme(id) { UI.view = 'theme'; UI.sel = { inst: null, part: null }; send('open', { theme: id }); }
function apply(id, monitor) { send('apply', { theme: id, monitor: monitor || '' }); }
function applyFlow(id) {
  if (!perMonitor()) return apply(id);
  let close;
  close = modal(`套用「${theme(id).name}」`, h('div', { class: 'kgrid' },
    ENV.monitors.map(m => h('button', { class: 'kind', onclick: () => { close(); apply(id, m.key); } },
      h('strong', null, m.name + (m.primary ? '（主）' : '')), h('small', null, `${m.width}×${m.height} · 现在是「${theme(screenTheme(m)).name}」`))),
    h('button', { class: 'kind', onclick: () => { close(); apply(id, 'all'); } }, h('strong', null, '所有屏幕'), h('small', null, '每块屏幕都用这个主题'))), null, { narrow: true });
}
function themeMenu(btn, id, onPage) {
  const t = theme(id);
  menu(btn, [
    !onPage && { label: '自定义', run: () => openTheme(id) },
    !onPage && { label: '锁屏预览', run: () => lockPreview(id) },
    !onPage && '-',
    { label: '复制…', uac: true, run: () => nameFlow('复制主题', `${t.name} 副本`, '创建副本', name => send('copy', { theme: id, name })) },
    !t.builtIn && { label: '重命名…', uac: true, run: () => nameFlow('重命名主题', t.name, '重命名', name => send('rename', { theme: id, name })) },
    { label: '导出主题包…', run: () => send('export', { theme: id }) },
    t.modifications && { label: `恢复默认（${t.modifications} 项修改）`, run: () => resetFlow(id) },
    !t.builtIn && '-',
    !t.builtIn && { label: '删除…', uac: true, danger: true, run: () => deleteFlow(id) },
  ]);
}
function card(t) {
  const used = usedOn(t.id);
  const thumb = h('div', { class: 'thumb' + (used.length ? ' on' : ''), tabindex: '0', role: 'button', 'aria-label': `套用「${t.name}」` },
    h('img', { class: 'shot', src: thumbUrl(t, 480), alt: '', vars: shapeVars(t.id), draggable: 'false' }));
  const more = h('button', { class: 'icon-btn', 'aria-label': `「${t.name}」的更多操作`, html: ICON.more, onclick: e => { e.stopPropagation(); themeMenu(more, t.id); } });
  thumb.append(h('div', { class: 'acts' }, h('button', { class: 'btn sm', onclick: e => { e.stopPropagation(); openTheme(t.id); } }, '自定义'), more));
  thumb.addEventListener('click', () => applyFlow(t.id));
  thumb.addEventListener('keydown', e => { if (e.key === 'Enter' && e.target === thumb) applyFlow(t.id); });
  const facts = [kindName(t.wallpaperKind), t.summary, t.modifications ? '已修改' : null, t.author && !t.builtIn ? t.author : null].filter(Boolean).join(' · ');
  return h('article', { class: 'card' }, thumb, h('div', { class: 'meta' }, h('div', null,
    h('h3', null, t.name), h('p', null, used.length ? h('span', { class: 'inuse' }, used.join('、') + ' · ') : null, facts))));
}
function currentView() {
  if (perMonitor()) {
    const showOn = m => L.settings.componentDisplays === 'all' || m.primary;
    return h('div', { class: 'box screens' }, ENV.monitors.map(m => {
      const t = theme(screenTheme(m));
      return h('figure', null, h('img', { class: 'shot', src: thumbUrl(t, 640), alt: '', vars: { '--arw': String(m.width), '--arh': String(m.height) } }),
        h('figcaption', null, h('span', null, m.name + (m.primary ? '（主）' : '') + (showOn(m) ? '' : ' · 不显示组件')), h('button', { class: 'link', onclick: () => openTheme(t.id) }, t.name)));
    }));
  }
  const t = theme(L.current);
  const wallpaper = ['image', 'video'].includes(t.wallpaperKind) ? `${t.wallpaper}（${kindName(t.wallpaperKind)}）` : t.wallpaper;
  return h('div', { class: 'box current' }, h('img', { class: 'shot', src: thumbUrl(t, 960), alt: '', vars: shapeVars(t.id) }), h('div', { class: 'cinfo' },
    h('h3', null, t.name),
    h('p', null, `${wallpaper} · ${L.settings.components ? t.summary : '组件已关闭'}`),
    h('div', { class: 'actions' }, h('button', { class: 'btn accent', onclick: () => openTheme(t.id) }, '自定义'), h('button', { class: 'btn', onclick: () => lockPreview(t.id) }, '锁屏预览'))));
}
function mineView() {
  const off = ENV.installed && !ENV.on ? h('p', { class: 'warn' }, 'AnimeLogon 已关闭，锁屏显示 Windows 默认界面。', h('button', { class: 'link', onclick: () => { UI.tab = 'settings'; render(); } }, '打开设置')) : null;
  const missing = !ENV.installed ? h('p', { class: 'warn' }, '这台电脑上没有安装 AnimeLogon，改动无法保存。') : null;
  return [
    h('section', { class: 'sec' }, h('div', { class: 'sh' }, h('h2', null, '当前主题'), perMonitor() ? h('span', null, '每块屏幕各自使用主题') : null), off, missing, currentView()),
    h('section', { class: 'sec' }, h('div', { class: 'sh' }, h('h2', null, '我的主题'), h('span', null, `${L.themes.length} 个 · 单击套用`)), h('div', { class: 'grid' }, L.themes.map(card))),
  ];
}
function discoverView() {
  return h('section', { class: 'sec' }, h('div', { class: 'sh' }, h('h2', null, '发现')), h('p', { class: 'lead' }, '在线社区还在规划中。'));
}

/* --- flows the native side finishes, most with the elevated helper --- */
function nameFlow(title, value, action, ok) {
  let close;
  const input = h('input', { class: 'textin', id: 'name-input', value, maxlength: '40', autofocus: true });
  const done = () => { const v = input.value.trim(); if (!v) return; close(); ok(v); };
  input.addEventListener('keydown', e => { if (e.key === 'Enter') done(); });
  close = modal(title, [h('label', { class: 'field', for: 'name-input' }, '名称', input)],
    [h('button', { class: 'btn', onclick: () => close() }, '取消'), h('button', { class: 'btn accent', onclick: done }, [shield(), action])], { narrow: true });
  input.select();
}
function resetFlow(id) {
  let close;
  const t = theme(id);
  close = modal('恢复默认', h('p', null, `清除对「${t.name}」的 ${t.modifications} 项修改，包括换过的壁纸和添加的组件。`),
    [h('button', { class: 'btn', onclick: () => close() }, '取消'), h('button', { class: 'btn accent', onclick: () => { close(); if (T && T.id === id) send('reset'); else { send('open', { theme: id }); send('reset'); send('close'); } } }, '恢复默认')], { narrow: true });
}
function deleteFlow(id) {
  let close;
  const t = theme(id);
  close = modal(`删除「${t.name}」`, [h('p', null, '主题会从这台电脑上删除，只有它在用的壁纸和组件也一起删除。'), usedOn(id).length ? h('p', { class: 'muted small' }, '它正在使用，删除后锁屏改用「默认」主题。') : null],
    [h('button', { class: 'btn', onclick: () => close() }, '取消'), h('button', { class: 'btn accent', onclick: () => { close(); if (UI.view === 'theme' && T && T.id === id) UI.view = 'library'; send('remove', { theme: id }); } }, [shield(), '删除'])], { narrow: true });
}
function wallpaperGrid(current, onPick) {
  return h('div', { class: 'vgrid' }, L.wallpapers.map(w => h('button', { class: 'vpick', 'aria-pressed': String(current === w.id), onclick: () => onPick(w.id) },
    h('img', { class: 'shot', src: `${ORIGIN}wallpaper/${w.id}.bmp`, alt: '' }), h('span', null, w.name), h('small', null, w.meta))));
}
function newThemeFlow() {
  let wp = 'default', close;
  const name = h('input', { class: 'textin', id: 'new-name', value: '新主题', maxlength: '40', autofocus: true });
  const grid = () => wallpaperGrid(wp, id => { wp = id; holder.replaceChildren(grid()); });
  const holder = h('div', null, grid());
  close = modal('新建主题', [h('label', { class: 'field', for: 'new-name' }, '名称', name), h('div', { class: 'field' }, '壁纸', holder),
    h('p', { class: 'muted small' }, '新主题带一个时钟。要用新的图片或视频，请用「导入…」。')],
    [h('button', { class: 'btn', onclick: () => close() }, '取消'),
      h('button', { class: 'btn accent', onclick: () => { const n = name.value.trim() || '新主题'; close(); send('newTheme', { name: n, wallpaper: wp }); } }, [shield(), '创建'])]);
}

/* ================= settings every theme shares ================= */
function srow(title, desc, control, cls) { return h('div', { class: 'srow' + (cls ? ' ' + cls : '') }, h('div', { class: 'stext' }, h('div', null, title), desc ? h('p', null, desc) : null), control); }
const global = (key, value) => send('global', { key, value: String(value) });
function rangeCtl(value, onDone, id, label) {
  const val = h('span', { class: 'val' }, value + '%');
  const r = h('input', { type: 'range', min: 0, max: 100, step: 1, value, id, 'aria-label': label });
  r.addEventListener('input', () => { val.textContent = r.value + '%'; });
  r.addEventListener('change', () => onDone(+r.value));
  return h('div', { class: 'srange' }, r, val);
}
// Worded by the native side with the logon screen's own rules and this account's regional format.
const clockSample = () => UI.clock || '';
function settingsView() {
  const s = L.settings, c = s.clock;
  const group = (title, desc, rows) => h('section', { class: 'sgroup' }, h('h2', null, title), desc ? h('p', null, desc) : null, h('div', { class: 'box' }, rows));
  const audioRows = [srow('播放声音', '在锁屏上播放声音。默认关闭。', toggle(s.audio.enabled, v => global('audio.enabled', v), '播放声音', 'set-audio'))];
  if (s.audio.enabled) audioRows.push(
    srow('输出设备', null, select([['', '自动（跟随系统）'], ...ENV.devices.map(d => [d.id, d.name])], s.audio.device, v => global('audio.device', v), '输出设备', 'set-device'), 'sub'),
    srow('总音量', null, rangeCtl(s.audio.volume, v => global('audio.volume', v), 'set-volume', '总音量'), 'sub'),
    srow('视频的声音', '播放视频壁纸自带的声音。', toggle(s.audio.video, v => global('audio.video', v), '视频的声音', 'set-video-audio'), 'sub'),
    s.audio.video ? srow('视频声音的音量', null, rangeCtl(s.audio.videoVolume, v => global('audio.videoVolume', v), 'set-video-volume', '视频声音的音量'), 'sub') : null);
  const screenRows = [srow('多显示器', `这台电脑有 ${ENV.monitors.length} 块显示器。`, seg([['duplicate', '每块屏幕相同'], ['span', '横跨所有屏幕'], ['per-monitor', '每块屏幕各自']], s.monitorMode, v => global('monitorMode', v), '多显示器', 'set-monitors'))];
  if (perMonitor()) for (const m of ENV.monitors)
    screenRows.push(srow(m.name + (m.primary ? '（主）' : ''), `${m.width}×${m.height}`, select([['', `跟随当前主题（${theme(L.current).name}）`], ...L.themes.map(t => [t.id, t.name])], s.screens[m.key] || '', v => global('screen.' + m.key, v), m.name, 'set-screen-' + m.key), 'sub'));
  return [
    group('时钟', '对所有主题生效。时钟的样式在各个主题里调整。', [
      srow('现在显示为', null, h('span', { class: 'sample' }, clockSample())),
      srow('时间制式', '「跟随区域格式」与任务栏一致。', seg([['auto', '跟随区域格式'], ['12', '12 小时制'], ['24', '24 小时制']], c.hours, v => global('clock.hours', v), '时间制式', 'set-hours')),
      srow('显示上午/下午', '仅用于 12 小时制。', toggle(c.ampm, v => global('clock.ampm', v), '显示上午/下午', 'set-ampm')),
      srow('日期', null, seg([['weekday', '星期与月日'], ['long', '完整日期'], ['none', '不显示']], c.date, v => global('clock.date', v), '日期', 'set-date')),
      srow('语言', '时间和日期使用的语言。', select([['', '跟随区域格式'], ['zh-CN', '简体中文'], ['zh-TW', '繁體中文'], ['ja-JP', '日本語'], ['ko-KR', '한국어'], ['en-US', 'English']], c.language, v => global('clock.language', v), '语言', 'set-lang')),
    ]),
    group('组件', null, [
      srow('显示组件', '关闭后所有主题只显示壁纸。', toggle(s.components, v => global('components', v), '显示组件', 'set-components')),
      srow('组件显示在', '壁纸显示在每块屏幕上，组件只显示在这里。', select([['auto', '密码框所在的屏幕'], ['primary', '主屏幕'], ['all', '所有屏幕']], s.componentDisplays, v => global('componentDisplays', v), '组件显示在', 'set-component-displays')),
    ]),
    group('屏幕', null, screenRows),
    group('声音', null, audioRows),
    group('系统', null, [
      srow('启用 AnimeLogon', '关闭后锁屏恢复 Windows 默认，主题和设置保留。', h('div', { class: 'arow' }, shield(), toggle(ENV.on, v => send('switch', { on: v }), '启用 AnimeLogon', 'set-on'))),
      srow('日志', '反馈问题时附上。', h('button', { class: 'btn', onclick: () => send('logs') }, '打开')),
      srow('卸载 AnimeLogon', '移除本程序并还原所有系统更改。', h('button', { class: 'btn', onclick: () => send('uninstall') }, [shield(), '卸载'])),
      srow('关于', 'AnimeLogon 0.1.0 · MIT 许可证', null),
    ]),
  ];
}

/* ================= a theme's page ================= */
function themePage() {
  const head = h('div', { class: 'phd', id: 'phd' });
  const st = h('div', { class: 'stage', id: 'stage' });
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg'); svg.setAttribute('class', 'anchors'); svg.id = 'anchors';
  st.append(h('img', { class: 'pv', id: 'pv', alt: '', draggable: 'false' }), svg, h('div', { id: 'hovers' }),
    h('div', { class: 'selbox', id: 'selbox', hidden: true }, ['hnw', 'hne', 'hsw', 'hse'].map(c => h('span', { class: 'hd ' + c }))),
    h('div', { class: 'partbox', id: 'partbox', hidden: true }), h('div', { class: 'readout', id: 'readout', hidden: true }));
  st.addEventListener('pointerdown', onStageDown);
  st.addEventListener('pointermove', onStageHover);
  st.addEventListener('pointerleave', () => setHover(null));
  new ResizeObserver(() => { fitPreview(); updateSel(); }).observe(st);
  const notice = h('p', { class: 'notice', id: 'notice', hidden: true }, '组件已在设置里关闭，锁屏上只显示壁纸。');
  return h('div', { class: 'view' }, appTop(), busyBar(), head,
    h('main', { class: 'scroll' }, h('div', { class: 'tpage' }, h('div', { class: 'tpv' }, notice, st, h('p', { class: 'pvcap', id: 'pvcap' })), h('div', { class: 'tcol', id: 'side' }))));
}
function leaveTheme() { send('close'); UI.view = 'library'; UI.tab = 'mine'; render(); }
function renderHead() {
  const used = usedOn(T.id);
  const more = h('button', { class: 'icon-btn', title: '更多', 'aria-label': `「${T.name}」的更多操作`, html: ICON.more, onclick: () => themeMenu(more, T.id, true) });
  $('#phd').replaceChildren(
    h('h1', { class: 'crumbs' }, h('button', { onclick: leaveTheme }, '我的主题'), h('span', { class: 'sep', 'aria-hidden': 'true' }, '›'), h('span', null, T.name)),
    h('div', { class: 'spacer' }), h('span', { class: 'saved' }, '修改会自动保存'),
    h('button', { class: 'icon-btn', id: 'undo', title: '撤销 (Ctrl+Z)', 'aria-label': '撤销', html: ICON.undo, disabled: !T.canUndo, onclick: () => send('undo', { seq: ++PV.seq }) }),
    h('button', { class: 'icon-btn', id: 'redo', title: '重做 (Ctrl+Y)', 'aria-label': '重做', html: ICON.redo, disabled: !T.canRedo, onclick: () => send('redo', { seq: ++PV.seq }) }),
    h('button', { class: 'btn', onclick: () => lockPreview(T.id) }, '锁屏预览'),
    used.length ? h('span', { class: 'inuse-tag' }, used[0] === '使用中' ? '使用中' : `用于${used.join('、')}`) : h('button', { class: 'btn accent', onclick: () => applyFlow(T.id) }, '套用'),
    more);
}
function renderPreviewParts() {
  const st = $('#stage');
  for (const [k, v] of Object.entries(shapeVars(T.id))) st.style.setProperty(k, v);
  const m = screenOf(T.id);
  $('#pvcap').replaceChildren(h('span', null, `按${m.name} 的比例（${ratioLabel(m.width, m.height)}）显示`), h('span', null, '拖动组件可以移动，拖四角可以缩放'),
    h('button', { class: 'link', onclick: e => ratioMenu(e.currentTarget) }, '用其他比例预览…'));
  $('#notice').hidden = L.settings.components;
}
function ratioMenu(btn) { menu(btn, [[16, 10], [21, 9], [32, 9], [4, 3]].map(([a, b]) => ({ label: `${a}:${b}`, run: () => lockPreview(T.id, a / b) }))); }

/* --- the picture, drawn natively at the size the page shows it --- */
let fitTimer = 0;
function fitPreview() {
  const st = $('#stage'); if (!st) return;
  const r = st.getBoundingClientRect(), k = window.devicePixelRatio || 1;
  const w = Math.round(r.width * k), hh = Math.round(r.height * k), key = w + 'x' + hh;
  if (w < 16 || hh < 16 || key === PV.asked) return;
  clearTimeout(fitTimer);
  fitTimer = setTimeout(() => { PV.asked = key; send('preview', { width: w, height: hh, seq: ++PV.seq }); }, PV.asked ? 120 : 0);
}
function onPreview(m) {
  PV.rev = m.rev; PV.width = m.width; PV.height = m.height; PV.bounds = m.bounds || [];
  PV.waiting = false;
  showPreview(); updateSel();
  if (PV.pending) pump();
  else if (PV.commit) { PV.commit = false; send('commit', { seq: ++PV.seq }); }
}
function showPreview() {
  const img = $('#pv'); if (!img || !PV.width) return;
  const rev = PV.rev, url = `${ORIGIN}preview.bmp?r=${rev}`, next = new Image();
  next.src = url;
  next.decode().then(() => { if (rev >= PV.shown && $('#pv') === img) { img.src = url; PV.shown = rev; } }, () => {});
}
// Live edits during a drag or while a slider moves: the picture follows, settings.ini waits for the end.
function liveSet(inst, values, gesture) {
  const merged = PV.pending && PV.pending.inst === inst && PV.pending.gesture === gesture ? Object.assign(PV.pending.values, values) : values;
  PV.pending = { inst, values: merged, gesture };
  pump();
}
function pump() {
  if (PV.waiting || !PV.pending) return;
  const p = PV.pending; PV.pending = null; PV.waiting = true;
  send('setMany', { instance: p.inst, values: p.values, gesture: p.gesture, live: true, seq: ++PV.seq });
}
function endGesture() {
  PV.commit = true;
  if (!PV.waiting && !PV.pending) { PV.commit = false; send('commit', { seq: ++PV.seq }); }
}
function setValue(inst, key, value) { send('set', { instance: inst, key, value: String(value), seq: ++PV.seq }); }
function setValues(inst, values) { send('setMany', { instance: inst, values, seq: ++PV.seq }); }
function clearValues(inst, keys) { send('clear', { instance: inst, keys, seq: ++PV.seq }); }

/* --- the marks over the picture --- */
const instOf = id => T && T.instances.find(i => i.id === id);
const partOf = (inst, key) => inst && inst.parts && inst.parts.find(p => p.key === key);
const panelKey = sel => sel.part ? sel.part.split('.')[0] : 'p1';
// A component with several panels has no position of its own: each panel is placed by itself.
const multiPanel = instId => { const inst = instOf(instId); return !!inst && inst.panels > 1; };
function adjustment(inst, key) {
  if (!inst || !inst.parts) return null;
  for (const p of inst.parts) for (const a of p.adjustments) if (a.key === key) return a;
  return null;
}
const valueOf = (inst, key) => { const a = adjustment(inst, key); return a ? a.value : ''; };
function scale() { const st = $('#stage'); return st && PV.width ? st.getBoundingClientRect().width / PV.width : 0; }
function boundOf(instId, key) { return PV.bounds.find(b => b.instance === instId && b.key === key); }
function rectOf(b) { const k = scale(); return { left: b.left * k, top: b.top * k, width: (b.right - b.left) * k, height: (b.bottom - b.top) * k }; }
function place(box, r, pad) { Object.assign(box.style, { left: (r.left - pad) + 'px', top: (r.top - pad) + 'px', width: (r.width + pad * 2) + 'px', height: (r.height + pad * 2) + 'px' }); box.hidden = false; }
// The panel's anchor point and the point of the panel it holds, in the stage's pixels.
function geometry(instId, pk) {
  const st = $('#stage'), inst = instOf(instId), b = boundOf(instId, pk);
  if (!st || !inst || !b) return null;
  const s = st.getBoundingClientRect(), r = rectOf(b), W = s.width, H = s.height;
  const idx = Math.max(0, ANCHORS.indexOf(valueOf(inst, pk + '.anchor'))), col = idx % 3, row = Math.floor(idx / 3);
  const M = (+valueOf(inst, pk + '.margin') || 0) / 100 * Math.min(W, H);
  return { W, H, M, col, row, L: r.left, T: r.top, w: r.width, h: r.height,
    ox: +valueOf(inst, pk + '.offset-x') || 0, oy: +valueOf(inst, pk + '.offset-y') || 0,
    bx: [M, W / 2, W - M][col], by: [M, H / 2, H - M][row], rx: [r.left, r.left + r.width / 2, r.left + r.width][col], ry: [r.top, r.top + r.height / 2, r.top + r.height][row] };
}
let dragShift = null;  // while dragging, the selection box moves ahead of the picture
const panelBounds = instId => PV.bounds.filter(b => b.instance === instId && !b.key.includes('.'));
function unionOf(bs) {
  return bs.reduce((u, b) => ({ left: Math.min(u.left, b.left), top: Math.min(u.top, b.top), right: Math.max(u.right, b.right), bottom: Math.max(u.bottom, b.bottom) }),
    { left: Infinity, top: Infinity, right: -Infinity, bottom: -Infinity });
}
function updateSel() {
  const sel = $('#selbox'), part = $('#partbox'), svg = $('#anchors');
  if (!sel) return;
  svg.replaceChildren();
  updateHover();
  // A component of several panels, selected as a whole: one box around all of them. Each panel
  // is moved and sized by itself, so the box has no handles.
  const whole = !!UI.sel.inst && multiPanel(UI.sel.inst) && !UI.sel.part;
  sel.classList.toggle('whole', whole);
  if (whole) {
    const all = panelBounds(UI.sel.inst);
    part.hidden = true;
    if (all.length) place(sel, rectOf(unionOf(all)), 6); else sel.hidden = true;
    return;
  }
  const pk = panelKey(UI.sel), b = UI.sel.inst && boundOf(UI.sel.inst, pk);
  if (!b) { sel.hidden = true; part.hidden = true; return; }
  const r = rectOf(b);
  if (dragShift) { r.left = dragShift.left; r.top = dragShift.top; }
  place(sel, r, 6);
  const pb = UI.sel.part && UI.sel.part.includes('.') && boundOf(UI.sel.inst, UI.sel.part);
  if (pb && !dragShift) place(part, rectOf(pb), 2); else part.hidden = true;
  const g = geometry(UI.sel.inst, pk); if (!g) return;
  const NS = 'http://www.w3.org/2000/svg', el = (tag, a) => { const e = document.createElementNS(NS, tag); for (const [k, v] of Object.entries(a)) e.setAttribute(k, v); return e; };
  if (dragShift) for (let rr = 0; rr < 3; rr++) for (let cc = 0; cc < 3; cc++)
    svg.append(el('circle', { cx: [g.M, g.W / 2, g.W - g.M][cc], cy: [g.M, g.H / 2, g.H - g.M][rr], r: 3, fill: 'rgba(255,255,255,.7)', stroke: 'rgba(0,0,0,.4)' }));
  const rx = dragShift ? [r.left, r.left + r.width / 2, r.left + r.width][g.col] : g.rx, ry = dragShift ? [r.top, r.top + r.height / 2, r.top + r.height][g.row] : g.ry;
  if (Math.hypot(rx - g.bx, ry - g.by) > 2) {
    svg.append(el('line', { x1: g.bx, y1: g.by, x2: rx, y2: ry, stroke: 'rgba(0,0,0,.45)', 'stroke-width': 3 }));
    svg.append(el('line', { x1: g.bx, y1: g.by, x2: rx, y2: ry, stroke: '#fff', 'stroke-width': 1.2, 'stroke-dasharray': '4 3' }));
  }
  svg.append(el('circle', { cx: g.bx, cy: g.by, r: 5, fill: '#fff', stroke: 'rgba(0,0,0,.55)', 'stroke-width': 1.5 }));
}
function showReadout(text) {
  const ro = $('#readout'), sb = $('#selbox'); if (!ro) return;
  ro.hidden = false; ro.textContent = text;
  const top = parseFloat(sb.style.top) - 26;
  ro.style.left = Math.max(4, parseFloat(sb.style.left)) + 'px';
  ro.style.top = (top < 4 ? parseFloat(sb.style.top) + parseFloat(sb.style.height) + 6 : top) + 'px';
}
function hitTest(e) {
  const st = $('#stage').getBoundingClientRect(), k = scale(); if (!k) return null;
  const x = (e.clientX - st.left) / k, y = (e.clientY - st.top) / k, pad = 4 / k;
  let best = null, area = 0;
  for (const b of PV.bounds) {
    if (x < b.left - pad || x > b.right + pad || y < b.top - pad || y > b.bottom + pad) continue;
    const a = (b.right - b.left) * (b.bottom - b.top);
    if (!best || a < area) { best = b; area = a; }
  }
  return best;
}
// A dashed outline around what the pointer is over, on the picture or in the list: one panel,
// or every panel of a component. What is selected already has its own box.
function setHover(over) {
  const same = (a, b) => a === b || (a && b && a.inst === b.inst && a.pk === b.pk);
  if (same(UI.hover, over)) return;
  UI.hover = over; updateHover();
}
function updateHover() {
  const box = $('#hovers'); if (!box) return;
  box.replaceChildren();
  const hv = UI.hover; if (!hv || dragShift || !instOf(hv.inst)) return;
  for (const b of panelBounds(hv.inst)) {
    if (hv.pk && b.key !== hv.pk) continue;
    if (UI.sel.inst === hv.inst && (!multiPanel(hv.inst) || !UI.sel.part || panelKey(UI.sel) === b.key)) continue;
    const r = rectOf(b), d = h('div', { class: 'hoverbox' });
    place(d, r, 4); box.append(d);
  }
}
function onStageHover(e) {
  if (dragShift) return;
  const hit = hitTest(e);
  $('#stage').classList.toggle('over', !!hit);
  setHover(hit ? { inst: hit.instance, pk: hit.key.split('.')[0] } : null);
}
// The first click on a component selects it -- one of several panels, that panel -- and a second
// click on one of its texts selects that text.
function onStageDown(e) {
  if (e.button !== 0) return;
  if (e.target.closest('.hd')) { e.preventDefault(); startResize(e); return; }
  const hit = hitTest(e);
  if (!hit) { selectSel(null); return; }
  e.preventDefault();
  const pk = hit.key.split('.')[0], multi = multiPanel(hit.instance);
  const was = UI.sel.inst === hit.instance && (!multi || (!!UI.sel.part && panelKey(UI.sel) === pk));
  if (!was) selectSel(hit.instance, multi ? pk : null, true);
  startDrag(e, hit.instance, pk, was && hit.key.includes('.') ? hit.key : null);
}
const posText = (g, anchor, ox, oy) => `${ANCHOR_NAMES[Math.max(0, ANCHORS.indexOf(anchor))]} · 水平 ${signed(ox)}% · 垂直 ${signed(oy)}%`;
// Dragging moves the panel and says where it is the way the sliders do: the nearest of the nine
// anchors, and an offset from it. Near an anchor the offset snaps to zero.
function startDrag(e, instId, pk, partOnClick) {
  const g0 = geometry(instId, pk); if (!g0) return;
  const st = $('#stage'), p = { x: e.clientX, y: e.clientY, moved: false }, gesture = ++gestures;
  const move = ev => {
    const dx = ev.clientX - p.x, dy = ev.clientY - p.y;
    if (!p.moved && Math.hypot(dx, dy) < 3) return;
    p.moved = true; st.classList.add('dragging');
    const L0 = g0.L + dx, T0 = g0.T + dy, cx = L0 + g0.w / 2, cy = T0 + g0.h / 2, W = g0.W, H = g0.H, M = g0.M;
    const col = cx < W / 3 ? 0 : cx < W * 2 / 3 ? 1 : 2, row = cy < H / 3 ? 0 : cy < H * 2 / 3 ? 1 : 2;
    const bx = [M, W / 2, W - M][col], by = [M, H / 2, H - M][row], rx = [L0, cx, L0 + g0.w][col], ry = [T0, cy, T0 + g0.h][row];
    // On the same anchor, the offset moves by exactly what the pointer did.
    let ox = col === g0.col ? g0.ox + dx / W * 100 : (rx - bx) / W * 100;
    let oy = row === g0.row ? g0.oy + dy / H * 100 : (ry - by) / H * 100;
    if (Math.abs(rx - bx) < 7) ox = 0;
    if (Math.abs(ry - by) < 7) oy = 0;
    ox = clamp(roundTo(ox, .1), -50, 50); oy = clamp(roundTo(oy, .1), -50, 50);
    const anchor = ANCHORS[row * 3 + col];
    dragShift = { left: L0, top: T0 };
    updateSel(); showReadout(posText(g0, anchor, ox, oy));
    liveSet(instId, { [pk + '.anchor']: anchor, [pk + '.offset-x']: String(ox), [pk + '.offset-y']: String(oy) }, gesture);
  };
  const up = () => {
    window.removeEventListener('pointermove', move); window.removeEventListener('pointerup', up); window.removeEventListener('pointercancel', up);
    st.classList.remove('dragging'); $('#readout').hidden = true; dragShift = null;
    if (p.moved) endGesture(); else if (partOnClick) selectSel(instId, partOnClick);
    updateSel();
  };
  window.addEventListener('pointermove', move); window.addEventListener('pointerup', up); window.addEventListener('pointercancel', up);
}
function startResize(e) {
  const inst = instOf(UI.sel.inst); if (!inst) return;
  const pk = panelKey(UI.sel), b = boundOf(inst.id, pk); if (!b) return;
  const st = $('#stage').getBoundingClientRect(), r = rectOf(b), cx = st.left + r.left + r.width / 2, cy = st.top + r.top + r.height / 2;
  const a = adjustment(inst, pk + '.size'), d0 = Math.max(8, Math.hypot(e.clientX - cx, e.clientY - cy)), s0 = +(a ? a.value : 10), gesture = ++gestures;
  const move = ev => {
    const size = clamp(roundTo(s0 * Math.hypot(ev.clientX - cx, ev.clientY - cy) / d0, .5), a ? a.min : 1, a ? a.max : 50);
    showReadout(`大小 ${size.toFixed(1)}% 屏高`);
    liveSet(inst.id, { [pk + '.size']: String(size) }, gesture);
  };
  const up = () => { window.removeEventListener('pointermove', move); window.removeEventListener('pointerup', up); window.removeEventListener('pointercancel', up); $('#readout').hidden = true; endGesture(); };
  window.addEventListener('pointermove', move); window.addEventListener('pointerup', up); window.addEventListener('pointercancel', up);
}

/* --- the list beside the picture: the wallpaper, then each component --- */
function selectSel(inst, part, reveal) {
  UI.sel = { inst: inst || null, part: part || null };
  renderSide(); updateSel();
  if (reveal && inst && matchMedia('(min-width: 961px)').matches) {
    const c = document.querySelector(`[data-card="${inst}"]`);
    if (c) c.scrollIntoView({ block: 'nearest', behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth' });
  }
}
function renderSide() {
  const side = $('#side'); if (!side || !T) return;
  side.replaceChildren(wallpaperSection(), componentSection());
}
function wallpaperSection() {
  const wp = T.wallpaper, rows = [h('div', { class: 'wprow' },
    h('img', { class: 'shot', src: `${ORIGIN}wallpaper/${wp.ref}.bmp`, alt: '' }),
    h('div', null, h('div', null, wp.name), h('p', null, wp.meta)),
    h('div', { class: 'wpacts' },
      wp.modified ? h('button', { class: 'rst', title: '换回主题自带的壁纸', 'aria-label': '换回主题自带的壁纸', html: ICON.reset, onclick: () => send('wallpaper', { ref: T.ownWallpaper, seq: ++PV.seq }) }) : null,
      h('button', { class: 'btn', id: 'wp-change', onclick: pickWallpaper }, '更换…')))];
  if (wp.kind === 'video' || wp.kind === 'image') rows.push(h('div', { class: 'brow' },
    labRow('wp-fit', '契合度', '画面比例与屏幕不同时怎样显示。', null, T.fitModified, () => send('fit', { value: T.ownFit, seq: ++PV.seq })),
    seg([['fill', '填充'], ['fit', '适应'], ['stretch', '拉伸']], T.fit, v => send('fit', { value: v, seq: ++PV.seq }), '契合度', 'wp-fit')));
  if (wp.audio) rows.push(h('p', { class: 'bnote' }, '视频的声音在「设置 > 声音」里开关。'));
  return h('section', { class: 'tsec' }, h('h2', null, '壁纸'), h('div', { class: 'box' }, rows));
}
function componentSection() {
  const full = T.room <= 0;
  return h('section', { class: 'tsec' },
    h('h2', null, '组件', h('span', { class: 'count', title: '一个主题最多 8 个组件，隐藏的也算' }, `${T.instances.length} / 8`)),
    h('div', { class: 'box clip' }, T.instances.map(compEntry),
      h('div', { class: 'addrow' }, h('button', { class: 'btn', disabled: full, onclick: addMenu }, '添加组件'), full ? h('span', null, '已有 8 个组件，隐藏的也算在内。') : null)));
}
// One component in the list. Its entry is open while it is selected.
function compEntry(inst) {
  const open = UI.sel.inst === inst.id, toggleOpen = () => selectSel(open ? null : inst.id, null);
  const sub = [inst.missing ? '组件缺失' : inst.builtIn ? '内置组件' : inst.author ? `作者 ${inst.author}` : '导入的组件', inst.modified ? '已修改' : null, inst.visible ? null : '已隐藏'].filter(Boolean).join(' · ');
  const entry = h('div', { class: 'ccard' + (open ? ' open' : ''), 'data-card': inst.id },
    h('div', { class: 'chead' },
      h('button', { class: 'cname', 'aria-expanded': String(open), onclick: toggleOpen }, h('span', null, inst.label), h('small', null, sub)),
      toggle(inst.visible, v => send('visible', { instance: inst.id, on: v, seq: ++PV.seq }), `在锁屏上显示「${inst.label}」`, `vis-${inst.id}`),
      h('button', { class: 'chev', 'aria-expanded': String(open), 'aria-label': (open ? '收起' : '展开') + inst.label, html: ICON.down, onclick: toggleOpen })));
  if (open && !inst.missing) { const body = h('div'); fillEntry(body, inst); entry.append(body); }
  entry.addEventListener('pointerenter', () => setHover({ inst: inst.id }));
  entry.addEventListener('pointerleave', () => setHover(null));
  return entry;
}
const group = (title, rows) => h('section', { class: 'igroup' }, h('h3', null, title), rows);
function fillEntry(p, inst) {
  if (UI.sel.part) {
    const pt = partOf(inst, UI.sel.part);
    if (pt) return partEntry(p, inst, pt);
    UI.sel.part = null;
  }
  if (!inst.visible) p.append(h('p', { class: 'note' }, inst.base ? '已隐藏。主题自带的组件只能隐藏，不能删除。' : '已隐藏，锁屏上不显示。'));
  if (inst.panels === 1) p.append(positionGroup(inst, 'p1'));
  const own = inst.settings.filter(s => !s.folded);
  if (own.length) p.append(group('样式', own.map(s => settingControl(inst, s))));
  const parts = inst.parts.filter(pt => inst.panels > 1 || pt.key.includes('.'));
  p.append(group('逐项调整', h('div', { class: 'plist' }, parts.map(pt => h('button', { onclick: () => selectSel(inst.id, pt.key) },
    h('span', null, pt.label), pt.modified ? h('span', { class: 'mod' }, '已修改') : null, h('span', { html: ICON.chev }))))));
  const keys = [...inst.settings.map(s => s.id), ...inst.parts.flatMap(pt => pt.adjustments.map(a => a.key))];
  p.append(h('div', { class: 'cfoot' },
    h('button', { class: 'btn', disabled: !inst.modified, onclick: () => clearValues(inst.id, keys) }, '恢复这个组件'),
    inst.base ? null : h('button', { class: 'btn danger', onclick: () => { UI.sel = { inst: null, part: null }; send('removeInstance', { instance: inst.id, seq: ++PV.seq }); } }, '删除')));
}
// One part of a component: a text, a panel, its shadow or its backdrop.
function partEntry(p, inst, pt) {
  p.append(h('div', { class: 'crumb' }, h('button', { onclick: () => selectSel(inst.id, null) }, inst.label), h('span', { html: ICON.chev }), h('strong', null, pt.label)));
  const attr = a => a.key.split('.').pop();
  if (/^p\d+\.l\d+\.t\d+$/.test(pt.key)) {
    if (/^(时间|上午\/下午|日期)$/.test(pt.label)) p.append(h('p', { class: 'note' }, '时间制式、日期和语言在', h('button', { class: 'link', onclick: () => { leaveTheme(); UI.tab = 'settings'; render(); } }, '设置 > 时钟'), '里改，对所有主题生效。'));
    p.append(group('常用', pt.adjustments.filter(a => COMMON_TEXT.includes(attr(a)) && a.key.startsWith(pt.key + '.')).map(a => adjControl(inst, a))));
    const det = h('details', { class: 'more', open: UI.more }, h('summary', null, '更多'), pt.adjustments.filter(a => !(COMMON_TEXT.includes(attr(a)) && a.key.startsWith(pt.key + '.'))).map(a => adjControl(inst, a)));
    det.addEventListener('toggle', () => { UI.more = det.open; });
    p.append(det);
  } else if (/^p\d+$/.test(pt.key)) {
    p.append(positionGroup(inst, pt.key));
  } else {
    p.append(group('外观', pt.adjustments.map(a => adjControl(inst, a))));
  }
  const keys = pt.adjustments.map(a => a.key);
  p.append(h('div', { class: 'cfoot' }, h('button', { class: 'btn', disabled: !pt.modified, onclick: () => clearValues(inst.id, keys) }, `恢复「${pt.label}」`)));
}
function positionGroup(inst, pk) {
  const anchor = adjustment(inst, pk + '.anchor'), cur = anchor ? anchor.value : 'top';
  const grid = h('div', { class: 'agrid', role: 'group', 'aria-label': '位置' }, ANCHORS.map((val, i) => h('button', { type: 'button', title: ANCHOR_NAMES[i], 'aria-label': ANCHOR_NAMES[i], 'aria-pressed': String(val === cur),
    onclick: () => setValues(inst.id, { [pk + '.anchor']: val, [pk + '.offset-x']: '0', [pk + '.offset-y']: '0' }) })));
  const more = h('details', { class: 'more', open: UI.more }, h('summary', null, '更多'),
    [pk + '.margin', pk + '.align'].map(k => adjustment(inst, k)).filter(Boolean).map(a => adjControl(inst, a)));
  more.addEventListener('toggle', () => { UI.more = more.open; });
  const posModified = ['anchor', 'offset-x', 'offset-y'].some(k => { const a = adjustment(inst, pk + '.' + k); return a && a.modified; });
  return group('位置与大小', [
    h('div', { class: 'row' }, labRow('pos-grid', '位置', '相对屏幕的哪个位置。', ANCHOR_NAMES[Math.max(0, ANCHORS.indexOf(cur))], posModified,
      () => clearValues(inst.id, [pk + '.anchor', pk + '.offset-x', pk + '.offset-y'])),
      h('div', { class: 'arow' }, grid, h('p', null, '也可以在预览里直接拖动。'))),
    [pk + '.offset-x', pk + '.offset-y', pk + '.size'].map(k => adjustment(inst, k)).filter(Boolean).map(a => adjControl(inst, a)),
    more,
  ]);
}

/* --- controls for one value --- */
function labRow(id, label, detail, valText, modified, onReset) {
  return h('div', { class: 'lab' }, h('label', { for: id, title: detail || null }, label),
    modified ? h('button', { class: 'rst', title: '恢复这一项', 'aria-label': `恢复${label}`, html: ICON.reset, onclick: onReset }) : null,
    valText != null ? h('span', { class: 'val' }, valText) : null);
}
// How a value reads beside its slider, from what the attribute is.
function formatFor(key, control) {
  const attr = key.split('.').pop(), panel = /^p\d+\.[^.]+$/.test(key), fixed = d => v => (+v).toFixed(d);
  if (control !== 'slider') return v => String(v);
  switch (attr) {
    case 'offset-x': case 'offset-y': return v => `${signed(v)}%`;
    case 'size': return panel ? v => `${(+v).toFixed(1)}% 屏高` : v => `${(+v).toFixed(2)} 倍`;
    case 'margin': return v => `${(+v).toFixed(1)}%`;
    case 'weight': return v => `${v} · ${WEIGHT_NAMES[Math.round(v / 100) * 100] || ''}`;
    case 'tracking': case 'x': case 'y': return v => `${signed(v, 3)} em`;
    case 'space': case 'gap': return v => `${fixed(2)(v)} em`;
    case 'blur': return v => `${fixed(3)(v)} em`;
    case 'opacity': return /\.(s\d+|b)\.opacity$/.test(key) ? v => (+v ? `${Math.round(v * 100)}%` : '不画') : v => `${Math.round(v * 100)}%`;
  }
  return v => String(+v);
}
// One control for one value. Sliders and the colour wheel show their changes live and save at the end.
function control(o) {
  const { id, label, detail, kind, options, min, max, step, value, modified, write, reset } = o;
  if (kind === 'slider') {
    const fmt = o.fmt || (v => v), lab = labRow(id, label, detail, fmt(+value), modified, reset);
    const inp = h('input', { type: 'range', id, min, max, step, value: +value });
    let gesture = 0;
    inp.addEventListener('input', () => { if (!gesture) gesture = ++gestures; lab.querySelector('.val').textContent = fmt(+inp.value); write(inp.value, gesture); });
    inp.addEventListener('change', () => { gesture = 0; endGesture(); });
    return h('div', { class: 'row' }, lab, inp);
  }
  if (kind === 'choice') return h('div', { class: 'row' }, labRow(id, label, detail, null, modified, reset),
    options.length > 4 ? select(options.map(x => [x.value, x.label]), value, v => write(v), label, id) : seg(options.map(x => [x.value, x.label]), value, v => write(v), label, id));
  if (kind === 'toggle') return h('div', { class: 'row' }, h('div', { class: 'lab' }, h('label', { for: id, title: detail || null }, label),
    modified ? h('button', { class: 'rst', title: '恢复这一项', html: ICON.reset, onclick: reset }) : null, toggle(value === 'on', v => write(v ? 'on' : 'off'), label, id)));
  if (kind === 'font') return h('div', { class: 'row' }, labRow(id, label, detail, null, modified, reset),
    h('button', { class: 'fontbtn', id, onclick: () => fontPicker(value, v => write(v)) }, h('span', { style: { fontFamily: fontStack(value) } }, fontLabel(value)), h('span', { html: ICON.chev })));
  if (kind === 'color') {
    const v0 = String(value).toUpperCase(), wrap = h('div', { class: 'swatches' });
    const hex = h('input', { class: 'textin hexin', id: id + '-hex', value: v0, maxlength: '9', 'aria-label': label + '（十六进制）' });
    const custom = h('input', { type: 'color', value: v0.slice(0, 7).toLowerCase(), 'aria-label': '自定义颜色', id });
    for (const [n, c] of SWATCHES) wrap.append(h('button', { type: 'button', class: 'sw', title: n, 'aria-label': n, 'aria-pressed': String(v0 === c), style: { background: c }, onclick: () => write(c) }));
    let gesture = 0;
    custom.addEventListener('input', () => { if (!gesture) gesture = ++gestures; hex.value = custom.value.toUpperCase(); write(custom.value.toUpperCase(), gesture); });
    custom.addEventListener('change', () => { gesture = 0; endGesture(); });
    hex.addEventListener('change', () => { const v = hex.value.trim().toUpperCase(); if (/^#[0-9A-F]{6}([0-9A-F]{2})?$/.test(v)) write(v); else { hex.value = v0; toast('颜色写成 #RRGGBB，例如 #FFD27A'); } });
    wrap.append(h('label', { class: 'sw custom', title: '自定义颜色' }, custom), hex);
    return h('div', { class: 'row' }, labRow(id + '-hex', label, detail, null, modified, reset), wrap);
  }
  return null;
}
// write(value, gesture): with a gesture, a live change on its way; without, a change made at once.
function writer(inst, key) { return (v, gesture) => (gesture ? liveSet(inst.id, { [key]: String(v) }, gesture) : setValue(inst.id, key, v)); }
function adjControl(inst, a) {
  return control({ id: `adj-${inst.id}-${a.key}`.replace(/\./g, '_'), label: a.label, detail: a.detail, kind: a.control, options: a.options,
    min: a.min, max: a.max, step: a.step, value: a.value, modified: a.modified, fmt: formatFor(a.key, a.control),
    write: writer(inst, a.key), reset: () => clearValues(inst.id, [a.key]) });
}
function settingControl(inst, s) {
  const kind = s.kind === 'number' ? 'slider' : s.kind;
  return control({ id: `set-${inst.id}-${s.id}`, label: s.label, detail: s.detail, kind, options: s.options, min: s.min, max: s.max, step: s.step,
    value: s.value, modified: s.modified, fmt: v => String(+v), write: writer(inst, s.id), reset: () => clearValues(inst.id, [s.id]) });
}
const fontLabel = stored => { if (!stored) return '默认（Segoe UI Variable）'; const f = ENV.fonts.find(x => x.stored === stored); return f ? f.name : stored; };
const fontStack = stored => stored ? `"${stored}", var(--ui)` : '"Segoe UI Variable Display", var(--ui)';
function fontPicker(current, onPick) {
  let close;
  const d = new Date(), sample = `${d.getHours()}:${String(d.getMinutes()).padStart(2, '0')} 月`;
  const q = h('input', { class: 'textin', id: 'font-q', placeholder: '搜索字体', autofocus: true });
  const grid = h('div', { class: 'fonts', role: 'group', 'aria-label': '字体' });
  const list = [{ name: '默认（Segoe UI Variable）', stored: '' }, ...ENV.fonts];
  const fill = () => { const k = q.value.trim().toLowerCase(); grid.replaceChildren(...list.filter(f => !k || f.name.toLowerCase().includes(k) || f.stored.toLowerCase().includes(k)).slice(0, 300).map(f =>
    h('button', { type: 'button', class: 'ftile', 'aria-pressed': String(f.stored === current), onclick: () => { close(); onPick(f.stored); } }, h('span', { class: 'fs', style: { fontFamily: fontStack(f.stored) } }, sample), h('span', { class: 'fl' }, f.name)))); };
  q.addEventListener('input', fill); fill();
  close = modal('字体', [q, grid, h('p', { class: 'muted small' }, '只列出为所有用户安装的字体。锁屏以系统身份运行，看不到只为某个用户安装的字体。')]);
}
function pickWallpaper() {
  let close;
  close = modal('更换壁纸', [wallpaperGrid(T.wallpaper.ref, ref => { close(); send('wallpaper', { ref, seq: ++PV.seq }); }),
    h('p', { class: 'muted small' }, '从文件导入的图片或视频会加入壁纸库，其他主题也可以用。')],
    [h('button', { class: 'btn', onclick: () => { close(); send('importWallpaper'); } }, [shield(), '从文件导入…'])]);
}
function addMenu() {
  let close;
  const grid = h('div', { class: 'kgrid' }, L.components.map(c => h('button', { class: 'kind', onclick: () => { close(); send('add', { component: c.id, seq: ++PV.seq }); } },
      h('strong', null, c.name), h('small', null, c.builtIn ? '内置组件' : c.author ? `作者 ${c.author}` : '导入的组件'), h('span', { class: 'tag' }, c.builtIn ? '内置' : ''))),
    h('button', { class: 'kind', onclick: () => { close(); send('importComponent', { add: true }); } }, h('strong', null, '从文件导入…'), h('small', null, '一个 .xml 组件文件。需要管理员确认。')));
  close = modal('添加组件', [grid, h('p', { class: 'muted small' }, `这个主题还能添加 ${T.room} 个组件。`)]);
}

/* ================= shared controls ================= */
function seg(options, value, onChange, label, id) {
  const g = h('div', { class: 'seg', role: 'group', 'aria-label': label || null, id });
  for (const [v, l] of options) {
    const b = h('button', { type: 'button', 'aria-pressed': String(v === value) }, l);
    b.addEventListener('click', () => { g.querySelectorAll('button').forEach(x => x.setAttribute('aria-pressed', String(x === b))); onChange(v); });
    g.append(b);
  }
  return g;
}
function toggle(on, onChange, label, id) {
  const b = h('button', { type: 'button', class: 'tg', role: 'switch', 'aria-checked': String(on), 'aria-label': label, id });
  b.addEventListener('click', () => { const v = b.getAttribute('aria-checked') !== 'true'; b.setAttribute('aria-checked', String(v)); onChange(v); });
  return b;
}
function select(options, value, onChange, label, id) {
  const s = h('select', { class: 'select', 'aria-label': label, id });
  for (const [v, l] of options) s.append(h('option', { value: v, selected: v === value }, l));
  s.addEventListener('change', () => onChange(s.value));
  return s;
}
function modal(title, body, foot, o = {}) {
  closeMenu();
  const bg = h('div', { class: 'modal-bg' });
  const close = () => { document.removeEventListener('keydown', key, true); bg.remove(); };
  const key = e => { if (e.key === 'Escape') { e.stopPropagation(); close(); } };
  bg.addEventListener('pointerdown', e => { if (e.target === bg) close(); });
  bg.append(h('div', { class: 'modal' + (o.narrow ? ' narrow' : ''), role: 'dialog', 'aria-modal': 'true', 'aria-label': title },
    h('div', { class: 'mhead' }, h('h2', null, title), h('button', { class: 'icon-btn', 'aria-label': '关闭', html: ICON.close, onclick: close })),
    h('div', { class: 'mbody' }, body), foot ? h('div', { class: 'mfoot' }, foot) : null));
  document.addEventListener('keydown', key, true);
  document.body.append(bg);
  (bg.querySelector('[autofocus]') || bg.querySelector('.mfoot .accent') || bg.querySelector('button')).focus();
  return close;
}
function problem(text) {
  let close;
  close = modal('出现问题', h('p', { class: 'selectable' }, text), [h('button', { class: 'btn accent', onclick: () => close() }, '确定')], { narrow: true });
}
let menuClose = null;
function closeMenu() { if (menuClose) menuClose(); }
function menu(anchor, items) {
  closeMenu();
  const m = h('div', { class: 'menu', role: 'menu' });
  for (const it of items) {
    if (it === '-') { m.append(h('hr')); continue; }
    if (!it) continue;
    m.append(h('button', { role: 'menuitem', class: it.danger ? 'danger' : null, onclick: () => { closeMenu(); it.run(); } }, h('span', null, it.label), it.uac ? shield() : null));
  }
  document.body.append(m);
  const r = anchor.getBoundingClientRect(), mr = m.getBoundingClientRect();
  m.style.left = clamp(r.right - mr.width, 8, innerWidth - mr.width - 8) + 'px';
  m.style.top = (r.bottom + 6 + mr.height > innerHeight - 8 ? r.top - mr.height - 6 : r.bottom + 6) + 'px';
  const away = e => { if (!m.contains(e.target)) closeMenu(); };
  const key = e => { if (e.key === 'Escape') { e.stopPropagation(); closeMenu(); } };
  setTimeout(() => document.addEventListener('pointerdown', away, true));
  document.addEventListener('keydown', key, true);
  menuClose = () => { document.removeEventListener('pointerdown', away, true); document.removeEventListener('keydown', key, true); m.remove(); menuClose = null; };
  m.querySelector('button').focus();
}

/* ================= the lock screen, as it will behave ================= */
// ratio: as a display of that shape would show it, with black bars around it.
function lockPreview(id, ratio) {
  closeMenu();
  const m = screenOf(id), w = Math.min(2560, Math.round(screen.width * (window.devicePixelRatio || 1)));
  const shape = ratio || m.width / m.height, hh = Math.round(w / shape), t = theme(id);
  const q = `w=${w}${ratio ? '&h=' + hh : ''}&r=${t.revision}.${UI.tick}`;
  const ov = h('div', { class: 'lockpv', role: 'dialog', 'aria-label': '锁屏预览', tabindex: '-1' });
  const frame = h('div', { class: 'frame', style: { width: `min(100vw, ${(100 * shape).toFixed(2)}vh)`, height: `min(100vh, ${(100 / shape).toFixed(2)}vw)` } });
  const signin = h('div', { class: 'signin' }, h('img', { src: `${ORIGIN}lock/${id}/still.bmp?${q}`, alt: '' }),
    h('div', { class: 'sbox' }, h('div', { class: 'avatar' }, 'A'), h('div', { class: 'uname' }, '用户'), h('div', { class: 'pin' }, 'PIN'), h('small', null, 'Windows 登录界面（示意）')));
  // The still covers the screen at once; the components come in over it.
  const still = h('img', { src: `${ORIGIN}lock/${id}/still.bmp?${q}`, alt: '' });
  const full = h('img', { class: 'full', src: `${ORIGIN}lock/${id}/full.bmp?${q}`, alt: '' });
  full.addEventListener('load', () => setTimeout(() => { full.style.opacity = '1'; }, 300));
  frame.append(signin, still, full);
  const hint = h('div', { class: 'lhint' }, '按任意键或单击，模拟唤醒');
  const exit = h('button', { class: 'btn exit', onclick: e => { e.stopPropagation(); close(); } }, '退出预览 (Esc)');
  ov.append(frame, hint, exit);
  document.body.append(ov); ov.focus();
  let woke = false;
  const wake = () => { if (woke) return; woke = true; still.classList.add('fading'); full.classList.add('fading'); hint.textContent = '之后是 Windows 的登录界面'; };
  ov.addEventListener('pointerdown', e => { if (e.target !== exit) wake(); });
  const key = e => { e.stopPropagation(); e.preventDefault(); if (e.key === 'Escape') close(); else wake(); };
  document.addEventListener('keydown', key, true);
  function close() { document.removeEventListener('keydown', key, true); ov.remove(); }
}

/* ================= keys on the theme page ================= */
let nudge = { at: 0, timer: 0, gesture: 0 };
document.addEventListener('keydown', e => {
  if (UI.view !== 'theme' || !T || document.querySelector('.lockpv, .modal-bg, .menu')) return;
  const tag = (e.target.tagName || '').toLowerCase();
  const typing = (tag === 'input' && e.target.type !== 'range' && e.target.type !== 'color') || tag === 'textarea' || tag === 'select';
  const mod = e.ctrlKey || e.metaKey, k = e.key.toLowerCase();
  if (mod && k === 'z' && !typing) { e.preventDefault(); send(e.shiftKey ? 'redo' : 'undo', { seq: ++PV.seq }); return; }
  if (mod && k === 'y' && !typing) { e.preventDefault(); send('redo', { seq: ++PV.seq }); return; }
  if (typing || e.target.type === 'range') return;
  // Esc, Delete and the arrow keys act on the picture, never on a control in the list or the bars.
  if (e.target.closest && e.target.closest('.tcol, .phd, .top')) return;
  if (e.key === 'Escape') { if (UI.sel.part) selectSel(UI.sel.inst, null); else selectSel(null); return; }
  const inst = instOf(UI.sel.inst); if (!inst) return;
  if (e.key === 'Delete' || e.key === 'Backspace') {
    e.preventDefault();
    if (inst.base) send('visible', { instance: inst.id, on: false, seq: ++PV.seq });
    else { UI.sel = { inst: null, part: null }; send('removeInstance', { instance: inst.id, seq: ++PV.seq }); }
    return;
  }
  const step = e.shiftKey ? 1 : .1, d = { ArrowLeft: [-step, 0], ArrowRight: [step, 0], ArrowUp: [0, -step], ArrowDown: [0, step] }[e.key];
  if (!d || (multiPanel(inst.id) && !UI.sel.part)) return;  // several panels: the arrows move one at a time
  e.preventDefault();
  const pk = panelKey(UI.sel), now = performance.now();
  if (now - nudge.at > 700) nudge.gesture = ++gestures;
  nudge.at = now;
  const pending = PV.pending && PV.pending.inst === inst.id ? PV.pending.values : {};
  const ox = clamp(roundTo(+(pending[pk + '.offset-x'] ?? nudge.ox ?? valueOf(inst, pk + '.offset-x')) + d[0], .1), -50, 50);
  const oy = clamp(roundTo(+(pending[pk + '.offset-y'] ?? nudge.oy ?? valueOf(inst, pk + '.offset-y')) + d[1], .1), -50, 50);
  nudge.ox = ox; nudge.oy = oy;
  liveSet(inst.id, { [pk + '.offset-x']: String(ox), [pk + '.offset-y']: String(oy) }, nudge.gesture);
  showReadout(posText(null, valueOf(inst, pk + '.anchor'), ox, oy));
  clearTimeout(nudge.timer);
  nudge.timer = setTimeout(() => { $('#readout') && ($('#readout').hidden = true); nudge.ox = nudge.oy = undefined; endGesture(); }, 600);
});

/* ================= the clock moves on ================= */
let shownMinute = new Date().getMinutes();
setInterval(() => {
  const m = new Date().getMinutes(); if (m === shownMinute) return; shownMinute = m;
  UI.tick++;
  if (UI.view === 'theme' && T && PV.asked) { const [w, hh] = PV.asked.split('x').map(Number); send('preview', { width: w, height: hh, seq: ++PV.seq }); }
  else if (L) send('refresh');
}, 1000);

send('ready');
