// Where things are on the stage. The native side reports bounds in the picture's pixels; the
// stage shows the picture scaled by k = stage width / picture width. Panels are placed by an
// anchor (one of nine points of their display), a margin from the display's edges, and an offset
// in percent of the display.
import { pv } from '../preview';
import { instOf } from '../store';
import type { Adjustment, Bound } from '../types';

export const ANCHORS = ['top-left', 'top', 'top-right', 'left', 'center', 'right', 'bottom-left', 'bottom', 'bottom-right'];
export const ANCHOR_NAMES = ['左上', '上方居中', '右上', '左侧居中', '居中', '右侧居中', '左下', '下方居中', '右下'];
export const anchorName = (anchor: string) => ANCHOR_NAMES[Math.max(0, ANCHORS.indexOf(anchor))];

export interface Rect {
  left: number;
  top: number;
  width: number;
  height: number;
}

export const clamp = (v: number, a: number, b: number) => Math.min(b, Math.max(a, v));
export const decimals = (step: number) => {
  const s = String(step), i = s.indexOf('.');
  return i < 0 ? 0 : s.length - i - 1;
};
export const roundTo = (v: number, step: number) => +(Math.round(v / step) * step).toFixed(decimals(step));
export const signed = (v: number, d = 1) => (v > 0 ? '+' : v < 0 ? '−' : '') + Math.abs(+v).toFixed(d);

export function adjustment(instId: string | null, key: string): Adjustment | null {
  const inst = instOf(instId);
  if (!inst || !inst.parts) return null;
  for (const p of inst.parts) for (const a of p.adjustments) if (a.key === key) return a;
  return null;
}
export const valueOf = (instId: string | null, key: string) => adjustment(instId, key)?.value ?? '';

// Spanning with components on every display, an instance is drawn once per display: the marks
// follow the copy last clicked.
export function boundOf(instId: string, key: string, screen: number): Bound | undefined {
  const all = pv.bounds.filter(b => b.instance === instId && b.key === key);
  return all.find(b => b.screen === screen) || all[0];
}
export const copiesOf = (instId: string, key: string) => pv.bounds.filter(b => b.instance === instId && b.key === key).length;
export const panelBounds = (instId: string) => pv.bounds.filter(b => b.instance === instId && !b.key.includes('.'));
export const rectOf = (b: { left: number; top: number; right: number; bottom: number }, k: number): Rect =>
  ({ left: b.left * k, top: b.top * k, width: (b.right - b.left) * k, height: (b.bottom - b.top) * k });
export function unionOf(bs: Bound[]) {
  return bs.reduce((u, b) => ({ left: Math.min(u.left, b.left), top: Math.min(u.top, b.top), right: Math.max(u.right, b.right), bottom: Math.max(u.bottom, b.bottom) }),
    { left: Infinity, top: Infinity, right: -Infinity, bottom: -Infinity });
}

// The display a mark is placed on, in the stage's pixels: one of the wall's when spanning, else all of it.
export function frameOf(b: Bound | undefined, k: number, stage: { width: number; height: number }): Rect {
  const s = b && b.instance !== 'wallpaper' ? pv.screens[b.screen] : undefined;
  return s ? { left: s.left * k, top: s.top * k, width: (s.right - s.left) * k, height: (s.bottom - s.top) * k } : { left: 0, top: 0, width: stage.width, height: stage.height };
}

export interface Geometry {
  f: Rect;          // the panel's display
  W: number;
  H: number;
  M: number;        // the margin, in stage pixels
  col: number;      // the anchor's column and row
  row: number;
  r: Rect;          // the panel
  ox: number;       // its offsets, in percent
  oy: number;
  bx: number;       // the anchor point
  by: number;
  rx: number;       // the panel's point held at the anchor
  ry: number;
  hx: number[];     // the points it would be held by at each column and row
  hy: number[];
}

export function geometry(instId: string, pk: string, k: number, stage: { width: number; height: number }, screen: number): Geometry | null {
  const b = boundOf(instId, pk, screen);
  if (!b || !instOf(instId)) return null;
  const f = frameOf(b, k, stage), r = rectOf(b, k), W = f.width, H = f.height;
  const idx = Math.max(0, ANCHORS.indexOf(valueOf(instId, pk + '.anchor'))), col = idx % 3, row = Math.floor(idx / 3);
  const M = (+valueOf(instId, pk + '.margin') || 0) / 100 * Math.min(W, H);
  const hx = b.hold ? b.hold.slice(0, 3).map(v => v * k) : [r.left, r.left + r.width / 2, r.left + r.width];
  const hy = b.hold ? b.hold.slice(3, 6).map(v => v * k) : [r.top, r.top + r.height / 2, r.top + r.height];
  return {
    f, W, H, M, col, row, r, hx, hy,
    ox: +valueOf(instId, pk + '.offset-x') || 0,
    oy: +valueOf(instId, pk + '.offset-y') || 0,
    bx: f.left + [M, W / 2, W - M][col],
    by: f.top + [M, H / 2, H - M][row],
    rx: hx[col],
    ry: hy[row],
  };
}

// Where a panel dragged by (dx, dy) lands, said the way the sliders say it: the nearest of the
// nine anchors and an offset from it. Near an anchor the offset snaps to zero, which puts the
// point the panel is held by on the anchor, not the middle of its box.
export function placeDragged(g: Geometry, dx: number, dy: number) {
  const L0 = g.r.left + dx, T0 = g.r.top + dy, cx = L0 + g.r.width / 2, cy = T0 + g.r.height / 2;
  const { W, H, M, f } = g, fx = cx - f.left, fy = cy - f.top;
  const col = fx < W / 3 ? 0 : fx < (W * 2) / 3 ? 1 : 2, row = fy < H / 3 ? 0 : fy < (H * 2) / 3 ? 1 : 2;
  const bx = f.left + [M, W / 2, W - M][col], by = f.top + [M, H / 2, H - M][row];
  const rx = g.hx[col] + dx, ry = g.hy[row] + dy;
  // On the same anchor, the offset moves by exactly what the pointer did.
  let ox = col === g.col ? g.ox + (dx / W) * 100 : ((rx - bx) / W) * 100;
  let oy = row === g.row ? g.oy + (dy / H) * 100 : ((ry - by) / H) * 100;
  const snapX = Math.abs(rx - bx) < 7, snapY = Math.abs(ry - by) < 7;
  if (snapX) ox = 0;
  if (snapY) oy = 0;
  ox = clamp(roundTo(ox, 0.1), -50, 50);
  oy = clamp(roundTo(oy, 0.1), -50, 50);
  // Where the panel actually sits with the snapped offset, so the moving picture snaps too.
  const sx = snapX ? bx - rx : 0, sy = snapY ? by - ry : 0;
  return { anchor: ANCHORS[row * 3 + col], ox, oy, col, row, bx, by, rx: rx + sx, ry: ry + sy, left: L0 + sx, top: T0 + sy, snapX, snapY };
}

// The smallest panel or text under a point of the stage.
export function hitTest(x: number, y: number, k: number): Bound | null {
  if (!k) return null;
  const px = x / k, py = y / k, pad = 4 / k;
  let best: Bound | null = null, area = 0;
  for (const b of pv.bounds) {
    if (px < b.left - pad || px > b.right + pad || py < b.top - pad || py > b.bottom + pad) continue;
    const a = (b.right - b.left) * (b.bottom - b.top);
    if (!best || a < area) {
      best = b;
      area = a;
    }
  }
  return best;
}
