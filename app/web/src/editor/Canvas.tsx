// The theme's lock screen at the shape of its display, with the marks for adjusting it drawn over
// it: what the pointer is over, what is selected, the anchor it is placed by, and while dragging,
// the nine anchors and a guide when it snaps to one.
//
// A drag moves pixels at once: when the component was lifted out of the picture beforehand (on
// hover or selection, see preview.ts), the stage shows the picture without it and moves the
// component's own layer under the pointer; the native side draws the exact result when the drag
// ends. Without a lift (scene layers; a component drawn on several displays of a wall) the
// picture is redrawn live as before.
import { useEffect, useLayoutEffect, useReducer, useRef, useState, type CSSProperties, type PointerEvent as ReactPointerEvent } from 'react';
import { t as tr } from '../i18n';
import { Desktop16Regular } from '@fluentui/react-icons';
import { SceneFrames, WallpaperVideo } from '../media';
import { calm } from '../motion';
import {
  endGesture,
  forgetPrevious,
  hold,
  liftFor,
  liveSet,
  motion,
  newGesture,
  prefetchLift,
  pv,
  setValues,
  subscribePreview,
  askPreview,
  type Lift,
} from '../preview';
import { getState, instOf, multiPanel, panelKey, ratioLabel, screenOf, select, setState, shapeOf, spanning, useStore } from '../store';
import type { Bound } from '../types';
import { cx } from '../ui/controls';
import { setDraft } from './draft';
import {
  adjustment,
  boundOf,
  clamp,
  copiesOf,
  fixedCorner,
  geometry,
  hitTest,
  largestScale,
  middleMoved,
  offsetsScaledAbout,
  panelBounds,
  placeDragged,
  rectOf,
  roundTo,
  unionOf,
  valueOf,
  type Geometry,
  type Rect,
  type SnapLine,
} from './geometry';

const HANDLES: [string, CSSProperties][] = [
  ['nw', { left: -5, top: -5, cursor: 'nwse-resize' }],
  ['ne', { right: -5, top: -5, cursor: 'nesw-resize' }],
  ['sw', { left: -5, bottom: -5, cursor: 'nesw-resize' }],
  ['se', { right: -5, bottom: -5, cursor: 'nwse-resize' }],
];

// A drag or resize under way, or one that has ended and waits for its exact picture.
interface Drag {
  inst: string;
  pk: string;
  g: Geometry;
  lift: Lift | null;
  // A move: where the panel is, and what the sliders would say.
  left: number;
  top: number;
  anchor?: string;
  col?: number;
  row?: number;
  bx?: number;
  by?: number;
  rx?: number;      // the point it is held by
  ry?: number;
  lines?: SnapLine[];  // what it has snapped to
  // A resize: how much larger the panel is drawn than it was, about which point.
  scale?: number;
  origin?: { x: number; y: number };
  readout: string;
  settling?: number;  // the request whose picture ends it
}

// Where the middle of the moving box is, as the panel's horizontal and vertical position say it (D60).
const centreText = (x: number, y: number) => tr('canvas.position', { x: x.toFixed(1), y: y.toFixed(1) });
const pad = (r: Rect, p: number): CSSProperties => ({ left: r.left - p, top: r.top - p, width: r.width + p * 2, height: r.height + p * 2 });

export function Canvas() {
  const s = useStore();
  const t = s.open!;
  const [, redraw] = useReducer((x: number) => x + 1, 0);
  useEffect(() => subscribePreview(redraw), []);
  const well = useRef<HTMLDivElement>(null);
  const stageRef = useRef<HTMLDivElement>(null);
  const [stage, setStage] = useState({ left: 0, top: 0, width: 0, height: 0 });
  const [drag, setDrag] = useState<Drag | null>(null);
  const [over, setOver] = useState(false);
  const dragRef = useRef<Drag | null>(null);
  dragRef.current = drag;

  /* --- the stage's size: the display's shape, as large as the well allows --- */
  const shape = shapeOf(t.id);
  useLayoutEffect(() => {
    const el = well.current;
    if (!el) return;
    const fit = () => {
      // Room around the picture, more below it for the line that says which display it is.
      const r = el.getBoundingClientRect(), side = 32, above = 32, below = 48, ratio = shape.width / shape.height;
      let w = Math.max(0, r.width - side * 2), h = w / ratio;
      if (h > r.height - above - below) {
        h = Math.max(0, r.height - above - below);
        w = h * ratio;
      }
      w = Math.floor(w);
      h = Math.floor(h);
      setStage({ left: Math.round((r.width - w) / 2), top: Math.round(above + (r.height - above - below - h) / 2), width: w, height: h });
      const dpr = window.devicePixelRatio || 1;
      askPreview(Math.round(w * dpr), Math.round(h * dpr));
    };
    const ro = new ResizeObserver(fit);
    ro.observe(el);
    fit();
    return () => ro.disconnect();
  }, [shape.width, shape.height, s.noVideo]);
  // A video that would not play: the picture is asked for again, with the wallpaper in it.
  useEffect(() => {
    if (s.noVideo) pv.asked = '';
  }, [s.noVideo]);

  const k = pv.width ? stage.width / pv.width : 0;

  /* --- lifting: what the pointer is over or what is selected is made ready to drag --- */
  const hoverInst = s.hover?.inst ?? null;
  // Not while a drag holds a lift; again once it has settled, for the next one.
  const busy = !!drag;
  useEffect(() => {
    if (!busy) prefetchLift(s.sel.inst);
  }, [s.sel.inst, pv.rev, pv.url, busy]);
  useEffect(() => {
    if (!busy) prefetchLift(hoverInst);
  }, [hoverInst, pv.rev, pv.url, busy]);

  // A finished drag stays on the stage until the picture that answers it is shown.
  const settle = () => {
    setDrag(null);
    hold(null);
    setDraft(null);
  };
  useEffect(() => {
    if (drag?.settling && pv.shownSeq >= drag.settling) settle();
  });
  useEffect(() => {
    if (!drag?.settling) return;
    const timer = window.setTimeout(settle, 4000);
    return () => clearTimeout(timer);
  }, [drag?.settling]);

  /* --- pointer --- */
  const local = (e: { clientX: number; clientY: number }) => {
    const r = stageRef.current!.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  };
  const setHover = (hover: { inst: string; pk: string | null } | null) => {
    const cur = getState().hover;
    if (cur === hover || (cur && hover && cur.inst === hover.inst && cur.pk === hover.pk)) return;
    setState({ hover });
  };
  const onMove = (e: ReactPointerEvent) => {
    if (dragRef.current || e.buttons) return;
    const p = local(e), hit = hitTest(p.x, p.y, k);
    setOver(!!hit);
    setHover(hit ? { inst: hit.instance, pk: hit.key.split('.')[0] } : null);
  };
  const onDown = (e: ReactPointerEvent) => {
    if (e.button !== 0 || dragRef.current?.settling) return;
    const handle = (e.target as HTMLElement).dataset.handle;
    if (handle) {
      e.preventDefault();
      startResize(e, handle);
      return;
    }
    const p = local(e), hit = hitTest(p.x, p.y, k);
    if (!hit) {
      select(null);
      return;
    }
    e.preventDefault();
    const sel = getState().sel, pk = hit.key.split('.')[0], multi = multiPanel(hit.instance);
    const was = sel.inst === hit.instance && (!multi || (!!sel.part && panelKey(sel) === pk));
    setState({ screen: hit.screen });
    if (!was) select(hit.instance, multi || hit.instance === 'wallpaper' ? pk : null);
    startDrag(e, hit.instance, pk, was && hit.key.includes('.') ? hit.key : null);
  };

  function track(onMoveTo: (dx: number, dy: number, ev: PointerEvent) => void, onEnd: (moved: boolean) => void, x0: number, y0: number) {
    let moved = false;
    const move = (ev: PointerEvent) => {
      const dx = ev.clientX - x0, dy = ev.clientY - y0;
      if (!moved && Math.hypot(dx, dy) < 3) return;
      moved = true;
      onMoveTo(dx, dy, ev);
    };
    const up = () => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', up);
      window.removeEventListener('pointercancel', up);
      onEnd(moved);
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', up);
    window.addEventListener('pointercancel', up);
  }

  // The other panels on the same display, which a dragged one snaps to.
  const othersOf = (inst: string, pk: string, screen: number) => {
    const own = boundOf(inst, pk, screen);
    return pv.bounds
      .filter(b => b.instance !== 'wallpaper' && /^p\d+$/.test(b.key) && !(b.instance === inst && b.key === pk) && b.screen === own?.screen)
      .map(b => rectOf(b, k));
  };

  // The end of a drag or resize: what it set is written, and the stage keeps showing the moved
  // component until the picture drawn with it arrives.
  const finish = (inst: string, last: Record<string, string> | null, lift: Lift | null, gesture: number) => {
    if (lift && last) {
      const seq = setValues(inst, last, gesture);
      setDrag(d => (d ? { ...d, settling: seq } : null));
    } else {
      endGesture();
      settle();
    }
  };

  function startDrag(e: ReactPointerEvent, inst: string, pk: string, partOnClick: string | null) {
    const screen = getState().screen, g = geometry(inst, pk, k, stage, screen);
    if (!g) return;
    const lift = inst !== 'wallpaper' && copiesOf(inst, pk) === 1 ? liftFor(inst) : null;
    const others = othersOf(inst, pk, screen);
    const gesture = newGesture();
    let last: Record<string, string> | null = null;
    track((dx, dy, ev) => {
      // Shift keeps to the axis it has moved along most; Ctrl drags without snapping.
      if (ev.shiftKey) {
        if (Math.abs(dx) >= Math.abs(dy)) dy = 0;
        else dx = 0;
      }
      const place = placeDragged(g, dx, dy, { free: ev.ctrlKey, others });
      last = { [pk + '.anchor']: place.anchor, [pk + '.offset-x']: String(place.ox), [pk + '.offset-y']: String(place.oy) };
      const at = middleMoved(g, place.left - g.r.left, place.top - g.r.top);
      if (lift) hold(lift);
      setDrag({ inst, pk, g, lift, left: place.left, top: place.top, anchor: place.anchor, col: place.col, row: place.row, bx: place.bx, by: place.by,
        rx: place.rx, ry: place.ry, lines: place.lines, readout: centreText(at.x, at.y) });
      setDraft({ inst, values: { ...last, '#x': at.x.toFixed(1), '#y': at.y.toFixed(1) } });
      if (!lift) liveSet(inst, last, gesture);
    }, moved => {
      if (!moved) {
        settle();
        if (partOnClick) select(inst, partOnClick);
        return;
      }
      finish(inst, last, lift, gesture);
    }, e.clientX, e.clientY);
  }

  function startResize(e: ReactPointerEvent, handle: string) {
    const sel = getState().sel, inst = sel.inst;
    if (!inst || !instOf(inst)) return;
    const pk = panelKey(sel), g = geometry(inst, pk, k, stage, getState().screen);
    if (!g) return;
    const a = adjustment(inst, pk + '.size'), s0 = +(a ? a.value : 10);
    const lo = a ? a.min : 1, hi = a ? a.max : 50;
    // A layer filling the picture has no corner to keep: it grows about the point it is held by.
    const full = inst === 'wallpaper' && valueOf(inst, pk + '.anchor') === 'full';
    const origin = full ? { x: g.rx, y: g.ry } : fixedCorner(g, handle);
    const top = full ? hi : Math.max(lo, Math.min(hi, Math.floor(s0 * largestScale(g, origin) * 2) / 2));
    // How far along the box's diagonal the pointer is, from the fixed corner; the size follows it.
    const rs = stageRef.current!.getBoundingClientRect();
    const far = { x: origin.x === g.r.left ? g.r.left + g.r.width : g.r.left, y: origin.y === g.r.top ? g.r.top + g.r.height : g.r.top };
    const dx0 = far.x - origin.x || 1, dy0 = far.y - origin.y || 1, len = dx0 * dx0 + dy0 * dy0;
    const along = (x: number, y: number) => ((x - rs.left - origin.x) * dx0 + (y - rs.top - origin.y) * dy0) / len;
    const t0 = Math.max(0.05, along(e.clientX, e.clientY));
    const lift = inst !== 'wallpaper' && copiesOf(inst, pk) === 1 ? liftFor(inst) : null;
    const gesture = newGesture();
    let last: Record<string, string> | null = null;
    track((_dx, _dy, ev) => {
      const size = clamp(roundTo((s0 * along(ev.clientX, ev.clientY)) / t0, 0.5), lo, top);
      const scale = size / s0;
      last = { [pk + '.size']: String(size) };
      if (full) last[pk + '.anchor'] = 'center';
      else {
        const o = offsetsScaledAbout(g, origin, scale);
        last[pk + '.offset-x'] = String(o.ox);
        last[pk + '.offset-y'] = String(o.oy);
      }
      if (lift) hold(lift);
      setDrag({ inst, pk, g, lift, left: g.r.left, top: g.r.top, scale, origin, readout: tr('canvas.size', { size: size.toFixed(1) }) });
      const at = middleMoved(g, 0, 0, scale, origin);
      setDraft({ inst, values: { ...last, '#x': at.x.toFixed(1), '#y': at.y.toFixed(1) } });
      if (!lift) liveSet(inst, last, gesture);
    }, moved => {
      if (!moved) return settle();
      finish(inst, last, lift, gesture);
    }, e.clientX, e.clientY);
  }

  /* --- the marks --- */
  const sel = s.sel;
  const whole = !!sel.inst && multiPanel(sel.inst) && !sel.part;
  const pk = panelKey(sel);
  let selRect: Rect | null = null, partRect: Rect | null = null;
  if (sel.inst && k) {
    if (whole) {
      const all = panelBounds(sel.inst);
      if (all.length) selRect = rectOf(unionOf(all), k);
    } else {
      const b = boundOf(sel.inst, pk, s.screen);
      if (b) {
        selRect = rectOf(b, k);
        const pb = sel.part && sel.part.includes('.') ? boundOf(sel.inst, sel.part, s.screen) : undefined;
        if (pb) partRect = rectOf(pb, k);
      }
    }
  }
  // While dragging, the box is where the panel is going.
  if (drag && selRect && drag.inst === sel.inst) {
    if (drag.scale != null) {
      const sc = drag.scale, o = drag.origin ?? { x: drag.g.rx, y: drag.g.ry }, r = drag.g.r;
      selRect = { left: o.x + (r.left - o.x) * sc, top: o.y + (r.top - o.y) * sc, width: r.width * sc, height: r.height * sc };
    } else selRect = { ...selRect, left: drag.left, top: drag.top };
    partRect = null;
  }
  const hover = s.hover;
  const hoverRects: Rect[] = [];
  if (hover && !drag && instOf(hover.inst) && k) {
    for (const b of panelBounds(hover.inst)) {
      if (hover.pk && b.key !== hover.pk) continue;
      if (sel.inst === hover.inst && (!multiPanel(hover.inst) || !sel.part || panelKey(sel) === b.key)) continue;
      hoverRects.push(rectOf(b, k));
    }
  }
  // Above the box, or below it near the top; from its left edge, or from its right edge on the
  // right half of the stage, so the text stays on the stage.
  const readoutStyle = (r: Rect): CSSProperties => ({
    ...(r.left + r.width / 2 > stage.width / 2 ? { right: Math.max(4, stage.width - r.left - r.width - 6) } : { left: Math.max(4, r.left - 6) }),
    top: r.top - 6 - 26 < 4 ? r.top + r.height + 12 : r.top - 6 - 26,
  });

  /* --- the lifted component --- */
  const lifted = drag?.lift ?? null;
  let sprite: CSSProperties | null = null, rest: CSSProperties | null = null;
  if (lifted && drag) {
    const multi = multiPanel(drag.inst), r = drag.g.r, W = stage.width, H = stage.height, m = 8;
    const transform = drag.scale != null ? `scale(${drag.scale})` : `translate(${drag.left - r.left}px, ${drag.top - r.top}px)`;
    sprite = {
      backgroundImage: `url(${lifted.only})`,
      transform,
      transformOrigin: `${(drag.origin ?? { x: drag.g.rx }).x}px ${(drag.origin ?? { y: drag.g.ry }).y}px`,
      ...(multi ? { clipPath: `inset(${r.top - m}px ${W - r.left - r.width - m}px ${H - r.top - r.height - m}px ${r.left - m}px)` } : {}),
    };
    if (multi) {
      const l = r.left - m, tp = r.top - m, rr = r.left + r.width + m, b = r.top + r.height + m;
      rest = { backgroundImage: `url(${lifted.only})`, clipPath: `path(evenodd, "M0 0 H${W} V${H} H0 Z M${l} ${tp} H${rr} V${b} H${l} Z")` };
    }
  }

  const wallpaper = t.wallpaper;
  const scene = wallpaper.kind === 'scene' && wallpaper.scene;
  const video = wallpaper.kind === 'video' && pv.shownLayer && !scene;
  const m = screenOf(t.id);
  return (
    <div className="stage-area">
      <div className="well" ref={well}>
        {stage.width > 0 && (
          <div
            ref={stageRef}
            data-stage
            className={cx('stage', over && 'over', !!drag && !drag.settling && 'dragging')}
            style={{ left: stage.left, top: stage.top, width: stage.width, height: stage.height }}
            onPointerDown={onDown}
            onPointerMove={onMove}
            onPointerLeave={() => { setOver(false); if (!dragRef.current) setHover(null); }}
          >
            {scene && pv.width > 0 && (
              <SceneFrames theme={t.id} width={pv.width} height={pv.height} moving={motion() && !scene.static} refresh={pv.rev}
                onBounds={rects => { pv.bounds = pv.bounds.filter(b => b.instance !== 'wallpaper').concat(rects as Bound[]); redraw(); }} />
            )}
            {video && <WallpaperVideo key={wallpaper.ref} wallpaper={wallpaper.ref} fit={t.fit} brightness={t.brightness} />}
            {pv.url && <Picture url={pv.url} hidden={!!lifted} />}
            {lifted && <img className="layer" src={lifted.base} alt="" draggable={false} />}
            {rest && <div className="sprite" style={rest} />}
            {sprite && <div className="sprite" style={sprite} />}
            {pv.previous && <Leaving url={pv.previous} />}

            <svg className="marks">
              {pv.screens.length > 1 && pv.screens.map((sc, i) => {
                const a = { x: sc.left * k + 0.5, y: sc.top * k + 0.5, width: Math.max(0, (sc.right - sc.left) * k - 1), height: Math.max(0, (sc.bottom - sc.top) * k - 1), fill: 'none' };
                return (
                  <g key={i}>
                    <rect {...a} stroke="rgba(0,0,0,.45)" strokeWidth={3} />
                    <rect {...a} stroke="rgba(255,255,255,.7)" strokeWidth={1} strokeDasharray="6 4" />
                  </g>
                );
              })}
              {drag && !drag.settling && drag.lines?.map((l, i) => <Guide key={i} {...l} />)}
            </svg>

            {hoverRects.map((r, i) => <div key={i} className="box hov" style={pad(r, 4)} />)}
            {partRect && <div className="box part" style={pad(partRect, 2)} />}
            {selRect && (
              <div className={cx('box', 'sel', (drag || calm()) && 'still')} style={pad(selRect, 6)}>
                {!whole && !drag?.settling && HANDLES.map(([name, at]) => <span key={name} data-handle={name} className="handle" style={at} />)}
              </div>
            )}
            {drag && !drag.settling && selRect && <div className="readout num" style={readoutStyle(selRect)}>{drag.readout}</div>}
          </div>
        )}
      </div>
      {/* Which display's shape the stage has; other shapes are in the toolbar's lock screen preview. */}
      <div className="stage-foot">
        <Desktop16Regular />
        <span>{spanning() ? tr('settings.monitors.span') : `${m.name} · ${ratioLabel(m.width, m.height)}`}</span>
        {!s.lib!.settings.components && <span>· {tr('canvas.components_off')}</span>}
      </div>
    </div>
  );
}

// The picture on the stage; the first one of a page fades in.
function Picture({ url, hidden }: { url: string; hidden: boolean }) {
  const ref = useRef<HTMLImageElement>(null);
  const first = useRef(true);
  useEffect(() => {
    if (first.current && !calm()) ref.current?.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 150, easing: 'linear' });
    first.current = false;
  }, [url]);
  return <img ref={ref} className="layer" style={hidden ? { visibility: 'hidden' } : undefined} src={url} alt="" draggable={false} />;
}

// The picture before a new wallpaper or fit, fading out over the new one.
function Leaving({ url }: { url: string }) {
  const ref = useRef<HTMLImageElement>(null);
  useEffect(() => {
    if (calm() || !ref.current) return forgetPrevious();
    const a = ref.current.animate([{ opacity: 1 }, { opacity: 0 }], { duration: 250, easing: 'linear', fill: 'forwards' });
    a.onfinish = () => forgetPrevious();
    return () => a.cancel();
  }, [url]);
  return <img ref={ref} className="layer" src={url} alt="" draggable={false} />;
}

function Guide(p: { x1: number; y1: number; x2: number; y2: number }) {
  return (
    <>
      <line {...p} stroke="rgba(0,0,0,.45)" strokeWidth={3} />
      <line {...p} stroke="#fff" strokeWidth={1} />
    </>
  );
}
