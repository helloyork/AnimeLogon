// The page's state: what the native side last said (environment, library, open theme) and where
// the person is (page, tab, selection, dialogs). Components read it with useStore; only actions
// here change it.
import { useSyncExternalStore } from 'react';
import { listen, send } from './native';
import { onPreview, pv, requestPreviewAgain, resetPreview } from './preview';
import type { Environment, FromNative, Instance, Library, OpenTheme, SceneControls } from './types';

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
}

let state: State = {
  env: { on: false, installed: false, monitors: [], fonts: [], devices: [] },
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
  return ms.find(m => m.primary) || ms[0] || { key: '', name: '显示器', model: '', left: 0, top: 0, width: 16, height: 9, primary: true };
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
  if (!perMonitor()) return state.lib.current === id ? ['使用中'] : [];
  const ms = state.env.monitors.filter(m => screenTheme(m.key) === id);
  return ms.length && ms.length === state.env.monitors.length ? ['所有屏幕'] : ms.map(m => m.name);
}
export const kindName = (k: string) =>
  ({ scene: '场景', video: '视频', image: '图片', builtin: '内置背景', none: '无壁纸', missing: '壁纸缺失' } as Record<string, string>)[k] || '';
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
export function openTheme(id: string) {
  resetPreview();
  setState({ view: 'theme', sel: { inst: null, part: null } });
  send('open', { theme: id, seq: ++pv.seq });
}
export function leaveTheme(tab: Tab = 'mine') {
  send('close');
  setState({ view: 'library', tab, sel: { inst: null, part: null } });
}
export function setTab(tab: Tab) {
  if (state.view === 'theme') send('close');
  setState({ view: 'library', tab, sel: { inst: null, part: null } });
}
export function select(inst: string | null, part: string | null = null) {
  setState({ sel: { inst, part } });
}

/* --- library actions --- */
export const apply = (id: string, monitor = '') => send('apply', { theme: id, monitor });
export function applyFlow(id: string) {
  if (!perMonitor()) apply(id);
  else openDialog({ kind: 'monitor', theme: id });
}
export function copyFlow(id: string) {
  const t = themeOf(id);
  if (t) openDialog({ kind: 'name', title: '复制主题', value: `${t.name} 副本`, action: '创建副本', run: name => send('copy', { theme: id, name }) });
}
export function renameFlow(id: string) {
  const t = themeOf(id);
  if (t) openDialog({ kind: 'name', title: '重命名主题', value: t.name, action: '重命名', run: name => send('rename', { theme: id, name }) });
}
export function resetFlow(id: string) {
  const t = themeOf(id);
  if (!t) return;
  openDialog({
    kind: 'confirm', title: '恢复默认', action: '恢复默认',
    body: [`清除对「${t.name}」的 ${t.modifications} 项修改，包括换过的壁纸和添加的组件。`],
    run: () => {
      if (state.open?.id === id) send('reset', { seq: ++pv.seq });
      else { send('open', { theme: id }); send('reset'); send('close'); }
    },
  });
}
export function deleteFlow(id: string) {
  const t = themeOf(id);
  if (!t) return;
  openDialog({
    kind: 'confirm', title: `删除「${t.name}」`, action: '删除', elevated: true,
    body: ['主题会从这台电脑上删除，只有它在用的壁纸和组件也一起删除。', ...(usedOn(id).length ? ['它正在使用，删除后锁屏改用「默认」主题。'] : [])],
    run: () => {
      if (state.view === 'theme' && state.open?.id === id) setState({ view: 'library' });
      send('remove', { theme: id });
    },
  });
}
export const global = (key: string, value: string | number | boolean) => send('global', { key, value: String(value) });

/* --- what the native side says --- */
function receive(m: FromNative) {
  switch (m.type) {
    case 'environment': {
      const { type: _type, ...env } = m;
      setState({ env });
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
    case 'navigate': resetPreview(); setState({ view: 'theme', sel: { inst: null, part: null } }); break;
  }
}
listen(receive);

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
