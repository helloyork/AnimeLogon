// Where things are on the stage. The native side reports bounds in the picture's pixels; the
// stage shows the picture scaled by k = stage width / picture width. Panels are placed by an
// anchor (one of nine points of their display), a margin from the display's edges, and an offset
// in percent of the display.
import { pv } from '../preview';
import { instOf } from '../store';
import type { Adjustment, Bound } from '../types';

export const ANCHORS = ['top-left', 'top', 'top-right', 'left', 'center', 'right', 'bottom-left', 'bottom', 'bottom-right'];

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

const anchorX = (g: Geometry, col: number) => g.f.left + [g.M, g.W / 2, g.W - g.M][col];
const anchorY = (g: Geometry, row: number) => g.f.top + [g.M, g.H / 2, g.H - g.M][row];

// How far a panel's offsets may go, in percent, with its box still on its display, when it is
// placed by the given column and row. The native side stops a panel at the display's edges in
// any case (skinview.cpp), so past these an offset would change nothing on the screen.
export function offsetLimits(g: Geometry, col = g.col, row = g.row) {
  const { f, W, H, r } = g;
  const bx = anchorX(g, col), by = anchorY(g, row);
  // The box either side of the point it is held by.
  const left = g.hx[col] - r.left, right = r.left + r.width - g.hx[col];
  const above = g.hy[row] - r.top, below = r.top + r.height - g.hy[row];
  const span = (lo: number, hi: number): [number, number] => {
    if (lo > hi) {
      const mid = clamp(roundTo((lo + hi) / 2, 0.1), -50, 50);  // larger than the display: centred
      return [mid, mid];
    }
    return [Math.max(-50, Math.ceil(lo * 10 - 1e-6) / 10), Math.min(50, Math.floor(hi * 10 + 1e-6) / 10)];
  };
  const [x0, x1] = span(((f.left + left - bx) / W) * 100, ((f.left + W - right - bx) / W) * 100);
  const [y0, y1] = span(((f.top + above - by) / H) * 100, ((f.top + H - below - by) / H) * 100);
  return { x0, x1: Math.max(x0, x1), y0, y1: Math.max(y0, y1) };
}

// The offsets the panel is shown at: the stored ones, unless the display's edges stop it short of
// them (an older setting, or a theme made on a display of another shape). The picture's pixels are
// whole, so within a pixel and a half the stored values are taken as exact.
export function shownOffset(g: Geometry) {
  const vx = ((g.rx - g.bx) / g.W) * 100, vy = ((g.ry - g.by) / g.H) * 100;
  return {
    ox: Math.abs(g.rx - g.bx - (g.ox / 100) * g.W) <= 1.5 ? g.ox : roundTo(vx, 0.1),
    oy: Math.abs(g.ry - g.by - (g.oy / 100) * g.H) <= 1.5 ? g.oy : roundTo(vy, 0.1),
  };
}

// A line the moving panel has snapped to, in the stage's pixels.
export interface SnapLine {
  x1: number;
  y1: number;
  x2: number;
  y2: number;
}

const SNAP = 6;  // how near, in stage pixels, a panel has to come to a line to snap to it

// Where a panel dragged by (dx, dy) lands, said the way the sliders say it: the nearest of the
// nine anchors and an offset from it. Its box stays on its display. Unless `free`, it snaps when
// the point it would be held by comes near an anchor's line (the offset is then exactly zero,
// which puts that point on the anchor, not the middle of its box), when an edge comes near the
// display's edge, or when an edge or its middle comes near another panel's.
export function placeDragged(g: Geometry, dx: number, dy: number, opt: { free?: boolean; others?: Rect[] } = {}) {
  const { f, W, H, r } = g;
  const within = (d: number, lo: number, hi: number) => (lo > hi ? (lo + hi) / 2 : clamp(d, lo, hi));
  const keepX = (d: number) => within(d, f.left - r.left, f.left + W - r.left - r.width);
  const keepY = (d: number) => within(d, f.top - r.top, f.top + H - r.top - r.height);
  dx = keepX(dx);
  dy = keepY(dy);
  const lines: SnapLine[] = [];
  if (!opt.free) {
    // Pairs of where a point of the panel is before the drag and the line it may snap to.
    const xs: [number, number][] = [[r.left, f.left], [r.left + r.width, f.left + W]];
    const ys: [number, number][] = [[r.top, f.top], [r.top + r.height, f.top + H]];
    for (let i = 0; i < 3; i++) {
      xs.push([g.hx[i], anchorX(g, i)]);
      ys.push([g.hy[i], anchorY(g, i)]);
    }
    for (const o of opt.others || []) {
      for (const at of [o.left, o.left + o.width / 2, o.left + o.width])
        for (const p of [r.left, r.left + r.width / 2, r.left + r.width]) xs.push([p, at]);
      for (const at of [o.top, o.top + o.height / 2, o.top + o.height])
        for (const p of [r.top, r.top + r.height / 2, r.top + r.height]) ys.push([p, at]);
    }
    const nearest = (pairs: [number, number][], d: number) => {
      let best: { by: number; at: number } | null = null;
      for (const [p, at] of pairs) {
        const by = at - (p + d);
        if (Math.abs(by) < SNAP && (!best || Math.abs(by) < Math.abs(best.by))) best = { by, at };
      }
      return best;
    };
    const sx = nearest(xs, dx), sy = nearest(ys, dy);
    if (sx) {
      dx = keepX(dx + sx.by);
      lines.push({ x1: sx.at, y1: f.top, x2: sx.at, y2: f.top + H });
    }
    if (sy) {
      dy = keepY(dy + sy.by);
      lines.push({ x1: f.left, y1: sy.at, x2: f.left + W, y2: sy.at });
    }
  }
  const L0 = r.left + dx, T0 = r.top + dy, fx = L0 + r.width / 2 - f.left, fy = T0 + r.height / 2 - f.top;
  const col = fx < W / 3 ? 0 : fx < (W * 2) / 3 ? 1 : 2, row = fy < H / 3 ? 0 : fy < (H * 2) / 3 ? 1 : 2;
  const bx = anchorX(g, col), by = anchorY(g, row);
  const rx = g.hx[col] + dx, ry = g.hy[row] + dy;
  // On the same anchor, the offset moves by exactly what the pointer did.
  const shown = shownOffset(g);
  let ox = col === g.col ? shown.ox + (dx / W) * 100 : ((rx - bx) / W) * 100;
  let oy = row === g.row ? shown.oy + (dy / H) * 100 : ((ry - by) / H) * 100;
  if (Math.abs(rx - bx) < 0.5) ox = 0;
  if (Math.abs(ry - by) < 0.5) oy = 0;
  const lim = offsetLimits(g, col, row);
  ox = clamp(roundTo(ox, 0.1), lim.x0, lim.x1);
  oy = clamp(roundTo(oy, 0.1), lim.y0, lim.y1);
  return { anchor: ANCHORS[row * 3 + col], ox, oy, col, row, bx, by, rx, ry, left: L0, top: T0, lines };
}

// Where a panel is, as the panel beside the stage says it (D60): its middle, in percent of its
// display's width and height from the left and top edges, and how far that can go with the box
// still on the display. The middle is the point the panel is held by when it is centred (the
// middle of its lines without a marker that hangs beside them, such as AM/PM, D57), so a clock
// snapped to the display's middle reads 50. It is reckoned from the stored offsets, not the
// picture's whole pixels, so a value typed in reads back as typed.
function middle(g: Geometry) {
  const s = shownOffset(g);
  return { x: g.bx + (s.ox / 100) * g.W + (g.hx[1] - g.rx), y: g.by + (s.oy / 100) * g.H + (g.hy[1] - g.ry) };
}
export function centreOf(g: Geometry) {
  const { f, W, H, r } = g, m = middle(g);
  const span = (before: number, after: number, size: number): [number, number] => {
    const lo = (before / size) * 100, hi = 100 - (after / size) * 100;
    if (lo > hi) return [50, 50];
    return [Math.ceil(lo * 10 - 1e-6) / 10, Math.floor(hi * 10 + 1e-6) / 10];
  };
  const [x0, x1] = span(g.hx[1] - r.left, r.left + r.width - g.hx[1], W);
  const [y0, y1] = span(g.hy[1] - r.top, r.top + r.height - g.hy[1], H);
  return { x: roundTo(((m.x - f.left) / W) * 100, 0.1), y: roundTo(((m.y - f.top) / H) * 100, 0.1), x0, x1, y0, y1 };
}
// Where the middle is after the panel has been moved by (dx, dy), or scaled about a point.
export function middleMoved(g: Geometry, dx: number, dy: number, scale = 1, about?: { x: number; y: number }) {
  const o = about ?? { x: 0, y: 0 };
  const x = o.x + (g.hx[1] - o.x) * scale + dx, y = o.y + (g.hy[1] - o.y) * scale + dy;
  return { x: ((x - g.f.left) / g.W) * 100, y: ((y - g.f.top) / g.H) * 100 };
}
// The anchor and offsets that put the middle of a panel's box at (x, y) percent of its display,
// either left as it is when null: the anchor follows the position, as it does for a drag.
export function placeCentre(g: Geometry, x: number | null, y: number | null) {
  const { f, W, H } = g, m = middle(g);
  const dx = x == null ? 0 : f.left + (x / 100) * W - m.x;
  const dy = y == null ? 0 : f.top + (y / 100) * H - m.y;
  return placeDragged(g, dx, dy, { free: true });
}

// A panel resized from a corner of its box grows away from the opposite corner, which stays where
// it is. The native side scales a panel about the point it is held by, so the offsets move that
// point to where scaling about the corner takes it.
export function fixedCorner(g: Geometry, handle: string) {
  const { r } = g;
  return { x: handle.includes('w') ? r.left + r.width : r.left, y: handle.includes('n') ? r.top + r.height : r.top };
}
// The largest scale about that corner that keeps the box on its display.
export function largestScale(g: Geometry, o: { x: number; y: number }) {
  const { f, W, H, r } = g;
  const ex = (o.x === r.left ? r.left + r.width : r.left) - o.x, ey = (o.y === r.top ? r.top + r.height : r.top) - o.y;
  let m = Infinity;
  if (ex > 0) m = Math.min(m, (f.left + W - o.x) / ex);
  else if (ex < 0) m = Math.min(m, (f.left - o.x) / ex);
  if (ey > 0) m = Math.min(m, (f.top + H - o.y) / ey);
  else if (ey < 0) m = Math.min(m, (f.top - o.y) / ey);
  return m;
}
export function offsetsScaledAbout(g: Geometry, o: { x: number; y: number }, scale: number) {
  const shown = shownOffset(g);
  const hx = o.x + (g.rx - o.x) * scale, hy = o.y + (g.ry - o.y) * scale;
  return { ox: clamp(roundTo(shown.ox + ((hx - g.rx) / g.W) * 100, 0.1), -50, 50), oy: clamp(roundTo(shown.oy + ((hy - g.ry) / g.H) * 100, 0.1), -50, 50) };
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
