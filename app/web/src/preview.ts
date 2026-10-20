// The open theme's picture, drawn natively at the size the page shows it, and the edits on their
// way to it. Edits made while a picture is being drawn wait, and only the newest of them is sent;
// a drag or a slider sends "live" edits that redraw without saving, and its end commits them.
import { calm } from './motion';
import { fetchPicture, ORIGIN, send } from './native';
import { getState } from './store';
import type { Bound, ScreenRect } from './types';

interface Pending {
  inst: string;
  values: Record<string, string>;
  gesture: number;
}

export const pv = {
  rev: 0,           // the newest picture the native side has drawn
  seq: 0,           // numbers what the page asked for, so it can tell which answer is whose
  width: 0,         // its size in device pixels
  height: 0,
  bounds: [] as Bound[],
  screens: [] as ScreenRect[],
  layer: false,     // the picture is the components alone, over a video or scene playing in the page
  asked: '',        // the size last asked for
  waiting: false,
  pending: null as Pending | null,
  commit: false,
  // What is on the stage: an object URL of the picture, its revision and the request it answered.
  url: '',
  previous: '',     // the picture before a change of wallpaper, fading out over the new one
  shown: 0,
  shownSeq: 0,
  shownLayer: false,
  wallpaper: '',    // the wallpaper and fit the shown picture has
};

const listeners = new Set<() => void>();
export function subscribePreview(f: () => void) {
  listeners.add(f);
  return () => { listeners.delete(f); };
}
const notify = () => listeners.forEach(f => f());

// Not when the person asked for less motion, nor once a video would not play here.
export const motion = () => !getState().noVideo && !calm();

let fitTimer = 0;
export function askPreview(width: number, height: number) {
  const key = `${width}x${height}`;
  if (width < 16 || height < 16 || key === pv.asked) return;
  clearTimeout(fitTimer);
  fitTimer = window.setTimeout(() => {
    pv.asked = key;
    send('preview', { width, height, motion: motion(), seq: ++pv.seq });
  }, pv.asked ? 120 : 0);
}
export function requestPreviewAgain() {
  if (!pv.asked) return;
  const [width, height] = pv.asked.split('x').map(Number);
  send('preview', { width, height, motion: motion(), seq: ++pv.seq });
}
export function resetPreview() {
  clearTimeout(fitTimer);
  if (pv.url) URL.revokeObjectURL(pv.url);
  Object.assign(pv, { asked: '', width: 0, height: 0, bounds: [], screens: [], layer: false, url: '', previous: '', wallpaper: '', pending: null, waiting: false, commit: false });
  dropLift();
  notify();
}

export function onPreview(m: { rev: number; seq: number; width: number; height: number; layer: boolean; bounds: Bound[]; screens: ScreenRect[] }) {
  Object.assign(pv, { rev: m.rev, width: m.width, height: m.height, bounds: m.bounds || [], screens: m.screens || [], layer: !!m.layer, waiting: false });
  notify();
  show(m.rev, m.seq, !!m.layer);
  if (pv.pending) pump();
  else if (pv.commit) {
    pv.commit = false;
    send('commit', { seq: ++pv.seq });
  }
}

function show(rev: number, seq: number, layer: boolean) {
  const open = getState().open;
  const wallpaper = open ? open.wallpaper.ref + '/' + open.fit : '';
  fetchPicture(`${ORIGIN}preview?r=${rev}`).then(url => {
    if (!url) return;
    if (rev < pv.shown) {
      URL.revokeObjectURL(url);
      return;
    }
    const old = pv.url;
    // A new wallpaper or fit: the old picture fades out over the new one.
    const changed = !!old && !!pv.wallpaper && wallpaper !== pv.wallpaper;
    if (pv.previous && pv.previous !== old) URL.revokeObjectURL(pv.previous);
    pv.previous = changed ? old : '';
    Object.assign(pv, { url, shown: rev, shownSeq: seq, shownLayer: layer, wallpaper });
    notify();
    if (old && !changed) URL.revokeObjectURL(old);
  });
}
export function forgetPrevious() {
  if (pv.previous && pv.previous !== pv.url) URL.revokeObjectURL(pv.previous);
  pv.previous = '';
  notify();
}

/* --- edits --- */
let gestures = 0;
export const newGesture = () => ++gestures;

// Live edits during a drag or while a slider moves: the picture follows, settings.ini waits for the end.
export function liveSet(inst: string, values: Record<string, string>, gesture: number) {
  const merged = pv.pending && pv.pending.inst === inst && pv.pending.gesture === gesture ? Object.assign(pv.pending.values, values) : values;
  pv.pending = { inst, values: merged, gesture };
  pump();
}
function pump() {
  if (pv.waiting || !pv.pending) return;
  const p = pv.pending;
  pv.pending = null;
  pv.waiting = true;
  send('setMany', { instance: p.inst, values: p.values, gesture: p.gesture, live: true, seq: ++pv.seq });
}
export function endGesture() {
  pv.commit = true;
  if (!pv.waiting && !pv.pending) {
    pv.commit = false;
    send('commit', { seq: ++pv.seq });
  }
}
export const pendingValues = (inst: string) => (pv.pending && pv.pending.inst === inst ? pv.pending.values : {});

export function edit(op: string, args: Record<string, unknown> = {}) {
  const seq = ++pv.seq;
  send(op, { ...args, seq });
  return seq;
}
export const setValue = (inst: string, key: string, value: string | number) => edit('set', { instance: inst, key, value: String(value) });
export const setValues = (inst: string, values: Record<string, string>, gesture?: number) => edit('setMany', { instance: inst, values, ...(gesture ? { gesture } : {}) });
export const clearValues = (inst: string, keys: string[]) => edit('clear', { instance: inst, keys });

/* --- lifting a component out of the picture, so a drag moves its pixels at once --- */
// Two pictures at the preview's size: the theme without the component, and the component alone
// over nothing. While dragging, the page moves the second over the first; the native side draws
// the exact result once the drag ends.
export interface Lift {
  inst: string;
  rev: number;
  base: string;
  only: string;
}
let lift: Lift | null = null;
let held: Lift | null = null;
let fetching = '';

export function prefetchLift(inst: string | null) {
  if (!inst || inst === 'wallpaper' || !pv.width || !pv.url) return;
  const rev = pv.rev, key = inst + '@' + rev;
  if ((lift && lift.inst === inst && lift.rev === rev) || fetching === key) return;
  fetching = key;
  const id = encodeURIComponent(inst);
  Promise.all([fetchPicture(`${ORIGIN}lift/without/${id}?r=${rev}`), fetchPicture(`${ORIGIN}lift/only/${id}?r=${rev}`)]).then(([base, only]) => {
    if (fetching === key) fetching = '';
    if (!base || !only || rev !== pv.rev) {
      if (base) URL.revokeObjectURL(base);
      if (only) URL.revokeObjectURL(only);
      return;
    }
    if (lift && lift !== held) release(lift);
    lift = { inst, rev, base, only };
  });
}
export const liftFor = (inst: string): Lift | null => (lift && lift.inst === inst && lift.rev === pv.rev ? lift : null);
// While a drag uses a lift it is kept, even once newer pictures make it stale.
export function hold(l: Lift | null) {
  if (held && held !== l && held !== lift) release(held);
  held = l;
}
function release(l: Lift) {
  URL.revokeObjectURL(l.base);
  URL.revokeObjectURL(l.only);
}
function dropLift() {
  if (lift && lift !== held) release(lift);
  lift = null;
  fetching = '';
}
