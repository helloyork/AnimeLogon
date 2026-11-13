// The page's state: what the native side last said (environment, library, open theme) and where
// the person is (page, tab, selection, dialogs). Components read it with useStore; only actions
// here change it.
import { useSyncExternalStore } from 'react';
import { onCalmChange } from './motion';
import { list, setLang, t } from './i18n';
import { listen, send } from './native';
import { onPreview, pv, requestPreviewAgain, resetPreview } from './preview';
import type { Environment, FromNative, ImportKind, Instance, Library, OpenTheme, SceneControls, Workshop } from './types';

export type Tab = 'mine' | 'settings';

export interface Selection {
  inst: string | null;  // an instance id, or 'wallpaper' for a scene's layers
  part: string | null;  // p1, or p1.l1.t1
}

export type Dialog = { id: number } & (
  | { kind: 'problem'; text: string }
  | { kind: 'monitor'; theme: string }
  | { kind: 'name'; title: string; value: string; action: string; run: (name: string) => void }
  | { kind: 'confirm'; title: string; body: string[]; action: string; elevated?: boolean; run: () => void }
  | { kind: 'newTheme' }
  | { kind: 'wallpaper' }
  | { kind: 'addComponent' }
  | { kind: 'copyComponents' }  // the open theme's components, to other themes
  | { kind: 'workshop'; wallpaper: boolean }  // with `wallpaper`, one, for the open theme
  | { kind: 'import'; from?: number }  // what to import; `from` is the dialog it was opened from
);

export interface State {
  env: Environment;
  lib: Library | null;
  open: OpenTheme | null;
  clock: string;                    // the clock as the logon screen words it now
  view: 'library' | 'theme';
  tab: Tab;
  sel: Selection;
  screen: number;                   // spanning with components on every screen: which copy was clicked
  hover: { inst: string; pk: string | null } | null;  // what the pointer is over, on the stage or in the list
  busy: { label: string; percent: number } | null;
  noVideo: boolean;                 // a video would not play here: stills from now on
  tick: number;                     // the minute, so pictures with a clock are fetched again
  dialogs: Dialog[];
  lock: { theme: string; ratio?: number } | null;
  workshop: Workshop | null;        // Wallpaper Engine's wallpapers, as last looked for
  // A setting to point out, because the icon in the system tray opened the app at it. A new object
  // each time it is asked for; the settings page clears it once it has.
  reveal: { what: 'switch' } | null;
}

let state: State = {
  env: { on: false, installed: false, others: [], version: '', user: '', accountPicture: false, lang: '', langChoice: '', wallpaperEngine: false, importable: { video: [], picture: [], package: [] }, monitors: [], fonts: [], devices: [] },
  lib: null,
  open: null,
  clock: '',
  view: 'library',
  tab: 'mine',
  sel: { inst: null, part: null },
  screen: 0,
  hover: null,
  busy: null,
  noVideo: false,
  tick: 0,
  dialogs: [],
  lock: null,
  workshop: null,
  reveal: null,
};
const listeners = new Set<() => void>();

export const getState = () => state;
export function setState(patch: Partial<State>) {
  state = { ...state, ...patch };
  for (const f of listeners) f();
}
const subscribe = (f: () => void) => {
  listeners.add(f);
  return () => { listeners.delete(f); };
};
export const useStore = () => useSyncExternalStore(subscribe, getState);

/* --- toasts: shown by the Toaster in App --- */
const toastListeners = new Set<(text: string) => void>();
export const onToast = (f: (text: string) => void) => {
  toastListeners.add(f);
  return () => { toastListeners.delete(f); };
};
export const toast = (text: string) => toastListeners.forEach(f => f(text));

/* --- dialogs --- */
let dialogSeq = 0;
// A dialog as it is asked for, before it has an id.
type NewDialog = Dialog extends infer D ? (D extends Dialog ? Omit<D, 'id'> : never) : never;
export function openDialog(d: NewDialog) {
  setState({ dialogs: [...state.dialogs, { ...d, id: ++dialogSeq } as Dialog] });
}
export function closeDialog(id: number) {
  setState({ dialogs: state.dialogs.filter(d => d.id !== id) });
}

/* --- what the library says --- */
export const themeOf = (id: string) => state.lib?.themes.find(t => t.id === id);
export const perMonitor = () => state.lib?.settings.monitorMode === 'per-monitor';
export const spanning = () => state.lib?.settings.monitorMode === 'span' && state.env.monitors.length > 1;
export function mainMonitor() {
  const ms = state.env.monitors;
  return ms.find(m => m.primary) || ms[0] || { key: '', name: t('monitor.name', { n: 1 }), model: '', left: 0, top: 0, width: 16, height: 9, primary: true };
}
export function screenTheme(key: string) {
  const id = state.lib?.settings.screens[key];
  return id && themeOf(id) ? id : state.lib?.current || '';
}
// The display a theme is shown on: the one it is applied to in per-monitor mode, otherwise the main one.
export function screenOf(id: string) {
  return (perMonitor() && state.env.monitors.find(m => state.lib?.settings.screens[m.key] === id)) || mainMonitor();
}
// What a theme's pictures are shaped like: the wall of every display when spanning, else its display.
export function shapeOf(id: string) {
  if (!spanning()) return screenOf(id);
  const ms = state.env.monitors;
  const left = Math.min(...ms.map(m => m.left)), top = Math.min(...ms.map(m => m.top));
  return { width: Math.max(...ms.map(m => m.left + m.width)) - left, height: Math.max(...ms.map(m => m.top + m.height)) - top };
}
export function usedOn(id: string): string[] {
  if (!state.lib) return [];
  if (!perMonitor()) return state.lib.current === id ? [t('theme.in_use')] : [];
  const ms = state.env.monitors.filter(m => screenTheme(m.key) === id);
  return ms.length && ms.length === state.env.monitors.length ? [t('monitor.all')] : ms.map(m => m.name);
}
const KINDS: Record<string, Parameters<typeof t>[0]> = {
  scene: 'kind.scene', video: 'kind.video', image: 'kind.image', builtin: 'kind.builtin', none: 'kind.none', missing: 'kind.missing',
};
export const kindName = (k: string) => (KINDS[k] ? t(KINDS[k]) : '');
export function ratioLabel(w: number, h: number) {
  const known = [[16, 9], [16, 10], [21, 9], [32, 9], [4, 3], [3, 2]], r = w / h;
  const best = known.reduce((a, b) => (Math.abs(b[0] / b[1] - r) < Math.abs(a[0] / a[1] - r) ? b : a));
  return best[0] + ':' + best[1];
}

/* --- the open theme --- */
export function instOf(id: string | null): Instance | SceneControls | undefined {
  const t = state.open;
  if (!t || !id) return undefined;
  return id === 'wallpaper' ? t.wallpaper.scene : t.instances.find(i => i.id === id);
}
export const multiPanel = (id: string | null) => {
  const i = instOf(id);
  return !!i && (i.panels ?? 1) > 1;
};
export const panelKey = (sel: Selection) => (sel.part ? sel.part.split('.')[0] : 'p1');

/* --- navigation --- */
// A theme's page sits under the library's (D59).
export function openTheme(id: string) {
  resetPreview();
  setState({ view: 'theme', tab: 'mine', sel: { inst: null, part: null } });
  send('open', { theme: id, seq: ++pv.seq });
}
// Back to the list of themes, closing the open one.
export function leaveTheme() {
  send('close');
  setState({ view: 'library', tab: 'mine', sel: { inst: null, part: null }, hover: null });
}
// The pages of the app bar. A theme open under the library stays open while the settings are
// shown, so coming back finds it as it was, selection and undo included. Choosing the library
// again while on a theme's page goes back to the list, as Windows' settings do.
export function setTab(tab: Tab) {
  if (tab === 'mine' && state.tab === 'mine' && state.view === 'theme') return leaveTheme();
  setState({ tab, hover: null });
}
export function select(inst: string | null, part: string | null = null) {
  setState({ sel: { inst, part } });
}
// Back, as a window's back button, Alt+Left or the mouse's back button go: up one level of where one
// is (a part to its component, a component to the theme's list), out of a theme's page to the
// library, and from the settings back to the page left for them. Nothing while a dialog or the
// lock-screen preview is over the page: Esc closes those.
export function goBack() {
  if (state.dialogs.length || state.lock) return;
  if (state.tab === 'settings') return setTab('mine');
  if (state.view !== 'theme') return;
  const { inst, part } = state.sel;
  if (part) return select(inst === 'wallpaper' ? null : inst, null);
  if (inst) return select(null);
  leaveTheme();
}

/* --- library actions --- */
export const apply = (id: string, monitor = '') => send('apply', { theme: id, monitor });
export function applyFlow(id: string) {
  if (!perMonitor()) apply(id);
  else openDialog({ kind: 'monitor', theme: id });
}
export function copyFlow(id: string) {
  const th = themeOf(id);
  if (th) openDialog({ kind: 'name', title: t('dialog.copy.title'), value: t('theme.copy_name', { name: th.name }), action: t('dialog.copy.action'), run: name => send('copy', { theme: id, name }) });
}
export function renameFlow(id: string) {
  const th = themeOf(id);
  if (th) openDialog({ kind: 'name', title: t('dialog.rename.title'), value: th.name, action: t('dialog.rename.action'), run: name => send('rename', { theme: id, name }) });
}
export function resetFlow(id: string) {
  const th = themeOf(id);
  if (!th) return;
  openDialog({
    kind: 'confirm', title: t('dialog.reset.title'), action: t('dialog.reset.action'),
    body: [t('dialog.reset.body', { name: th.name })],
    run: () => {
      if (state.open?.id === id) send('reset', { seq: ++pv.seq });
      else { send('open', { theme: id }); send('reset'); send('close'); }
    },
  });
}
export function deleteFlow(id: string) {
  const th = themeOf(id);
  if (!th) return;
  openDialog({
    kind: 'confirm', title: t('dialog.delete.title', { name: th.name }), action: t('dialog.delete.action'), elevated: true,
    body: [t('dialog.delete.body'), ...(usedOn(id).length ? [t('dialog.delete.in_use')] : [])],
    run: () => {
      if (state.view === 'theme' && state.open?.id === id) setState({ view: 'library' });
      send('remove', { theme: id });
    },
  });
}
export const global = (key: string, value: string | number | boolean) => send('global', { key, value: String(value) });
// The language the app is shown in; empty to follow Windows. The native side stores it and says
// everything again in it.
export const setLanguage = (tag: string) => send('language', { tag });
// The project's pages and the licences, by name (app/src/host.cpp opens them).
export const openLink = (to: 'home' | 'issues' | 'releases' | 'license' | 'notices') => send('link', { to });
// Wallpaper Engine's video wallpapers, looked for afresh each time they are shown: as new themes,
// or with `wallpaper` one as the open theme's wallpaper.
export function workshopFlow(wallpaper: boolean) {
  send('workshop');
  openDialog({ kind: 'workshop', wallpaper });
}
// Bringing something in as a new theme: first what it is, then the file dialog for that kind
// (app/src/importable.h) or Wallpaper Engine's wallpapers. Opened from another dialog, that one
// stays under it until a kind is chosen, so Cancel goes back to it.
export const importFlow = (from?: number) => openDialog({ kind: 'import', from });
// The kind chosen: the dialog it was asked from closes, and the file dialog or Wallpaper Engine's
// wallpapers follow.
export function importKind(kind: ImportKind | 'workshop', from?: number) {
  if (from !== undefined) closeDialog(from);
  if (kind === 'workshop') workshopFlow(false);
  else send('import', { kind });
}

/* --- the switch --- */
// The other programs drawing the logon screen that are on now, by name.
export const othersOn = () => state.env.others.filter(o => o.on).map(o => o.name);
// Switching on switches the others off, so that is asked first.
export function switchOn() {
  const names = list(othersOn());
  if (!names) return send('switch', { on: true });
  openDialog({
    kind: 'confirm', title: t('settings.enable'), action: t('library.turn_on'), elevated: true,
    body: [t('dialog.switch_on.body', { names }), t('dialog.switch_on.detail', { names })],
    run: () => send('switch', { on: true }),
  });
}

/* --- what the native side says --- */
function receive(m: FromNative) {
  switch (m.type) {
    case 'environment': {
      const { type: _type, ...env } = m;
      // The language first, so that everything drawn for this environment is in it. The pictures
      // are drawn again too: what a component shows when nothing is there (a sample song) is in it.
      const relanguaged = !!state.env.lang && env.lang !== state.env.lang;
      setLang(env.lang);
      setState(relanguaged ? { env, tick: state.tick + 1 } : { env });
      if (relanguaged && state.view === 'theme' && state.open) requestPreviewAgain();
      break;
    }
    case 'state': {
      const patch: Partial<State> = { lib: m.library, open: m.open, clock: m.clock };
      if (state.view === 'theme' && !m.open) patch.view = 'library';
      const inst = state.sel.inst;
      if (inst && m.open && !(inst === 'wallpaper' ? m.open.wallpaper.scene : m.open.instances.find(i => i.id === inst)))
        patch.sel = { inst: null, part: null };
      setState(patch);
      break;
    }
    case 'preview': onPreview(m); break;
    case 'toast': toast(m.text); break;
    case 'problem': openDialog({ kind: 'problem', text: m.text }); break;
    case 'busy': setState({ busy: m.label ? { label: m.label, percent: m.percent ?? 0 } : null }); break;
    case 'added': setState({ sel: { inst: m.instance, part: null } }); break;
    case 'navigate': resetPreview(); setState({ view: 'theme', tab: 'mine', sel: { inst: null, part: null } }); break;
    case 'show': show(m.what); break;
    case 'workshop': {
      const { type: _type, ...found } = m;
      setState({ workshop: found });
      break;
    }
  }
}

// The icon in the system tray asked for something that needs this window (app/src/tray.h): the
// switch that turns AnimeLogon on and off, which needs an administrator, pointed out on the
// settings page; or the lock-screen preview of what the main display shows.
function show(what: 'switch' | 'lock') {
  if (what === 'switch') {
    setState({ tab: 'settings', lock: null, hover: null, reveal: { what } });
  } else if (what === 'lock' && state.lib) {
    const theme = state.lib.settings.monitorMode === 'per-monitor' ? screenTheme(mainMonitor().key) : state.lib.current;
    if (theme) setState({ lock: { theme } });
  }
}
listen(receive);

// Windows' animation effects switched while the window is open: the page draws itself again at
// once, so videos and scenes stop or start, and an open theme's picture is asked for again, since
// the native side draws it with or without the wallpaper as the page plays it or not (preview.ts).
onCalmChange(() => {
  setState({});
  if (state.view === 'theme' && state.open) requestPreviewAgain();
});

// Pictures with a clock change every minute.
let shownMinute = new Date().getMinutes();
setInterval(() => {
  const minute = new Date().getMinutes();
  if (minute === shownMinute) return;
  shownMinute = minute;
  setState({ tick: state.tick + 1 });
  if (state.view === 'theme' && state.open) requestPreviewAgain();
  else if (state.lib) send('refresh');
}, 1000);

export function start() {
  send('ready');
}
