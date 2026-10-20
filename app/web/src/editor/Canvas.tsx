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
import { Button, Caption1, makeStyles, Menu, MenuItem, MenuList, MenuPopover, MenuTrigger, mergeClasses, tokens } from '@fluentui/react-components';
import { ChevronDownRegular } from '@fluentui/react-icons';
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
import {
  adjustment,
  anchorName,
  boundOf,
  clamp,
  copiesOf,
  geometry,
  hitTest,
  panelBounds,
  placeDragged,
  rectOf,
  roundTo,
  signed,
  unionOf,
  valueOf,
  type Geometry,
  type Rect,
} from './geometry';

const MARK = '#ffffff', EDGE = 'rgba(0, 0, 0, .55)';

const useStyles = makeStyles({
  area: {
    flex: 1,
    minWidth: 0,
    minHeight: 0,
    display: 'flex',
    flexDirection: 'column',
    backgroundColor: tokens.colorNeutralBackground4,
  },
  well: { flex: 1, minHeight: 0, position: 'relative' },
  stage: {
    position: 'absolute',
    overflow: 'hidden',
    backgroundColor: '#000',
    borderRadius: tokens.borderRadiusMedium,
    // The lock screen is often dark on a dark canvas: a hairline shows where it ends.
    boxShadow: `${tokens.shadow8}, 0 0 0 1px ${tokens.colorNeutralStroke2}`,
    touchAction: 'none',
    cursor: 'default',
  },
  over: { cursor: 'grab' },
  dragging: { cursor: 'grabbing' },
  layer: { position: 'absolute', inset: 0, width: '100%', height: '100%', display: 'block', pointerEvents: 'none' },
  sprite: { position: 'absolute', inset: 0, backgroundSize: '100% 100%', pointerEvents: 'none', willChange: 'transform' },
  box: { position: 'absolute', pointerEvents: 'none', borderRadius: '2px', boxSizing: 'border-box' },
  selected: {
    border: `1.5px solid ${MARK}`,
    boxShadow: `0 0 0 1px ${EDGE}, inset 0 0 0 1px ${EDGE}`,
    zIndex: 4,
    transitionProperty: 'left, top, width, height',
    transitionDuration: tokens.durationFaster,
    transitionTimingFunction: tokens.curveDecelerateMax,
  },
  still: { transitionProperty: 'none' },
  part: { border: `1px dashed ${MARK}`, boxShadow: `0 0 0 1px ${EDGE}`, zIndex: 4 },
  hover: { border: `1px dashed ${MARK}`, boxShadow: `0 0 0 1px ${EDGE}`, zIndex: 3, opacity: 0.85 },
  handle: {
    position: 'absolute',
    width: '10px',
    height: '10px',
    boxSizing: 'border-box',
    backgroundColor: MARK,
    border: `1px solid ${EDGE}`,
    borderRadius: '2px',
    pointerEvents: 'auto',
    touchAction: 'none',
  },
  marks: { position: 'absolute', inset: 0, width: '100%', height: '100%', pointerEvents: 'none', zIndex: 3, overflow: 'visible' },
  readout: {
    position: 'absolute',
    zIndex: 5,
    pointerEvents: 'none',
    whiteSpace: 'nowrap',
    fontSize: tokens.fontSizeBase200,
    lineHeight: tokens.lineHeightBase200,
    fontVariantNumeric: 'tabular-nums',
    color: '#fff',
    backgroundColor: 'rgba(0, 0, 0, .78)',
    padding: `${tokens.spacingVerticalXXS} ${tokens.spacingHorizontalS}`,
    borderRadius: tokens.borderRadiusMedium,
  },
  foot: { display: 'flex', alignItems: 'center', gap: tokens.spacingHorizontalS, paddingInline: tokens.spacingHorizontalM, minHeight: '36px' },
  quiet: { color: tokens.colorNeutralForeground3 },
});

const HANDLES: [string, CSSProperties][] = [
  ['nw', { left: -6, top: -6, cursor: 'nwse-resize' }],
  ['ne', { right: -6, top: -6, cursor: 'nesw-resize' }],
  ['sw', { left: -6, bottom: -6, cursor: 'nesw-resize' }],
  ['se', { right: -6, bottom: -6, cursor: 'nwse-resize' }],
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
  snapX?: boolean;
  snapY?: boolean;
  // A resize: how much larger the panel is drawn than it was.
  scale?: number;
  readout: string;
  settling?: number;  // the request whose picture ends it
}

const posText = (anchor: string, ox: number, oy: number) => `${anchorName(anchor)} · 水平 ${signed(ox)}% · 垂直 ${signed(oy)}%`;
const pad = (r: Rect, p: number): CSSProperties => ({ left: r.left - p, top: r.top - p, width: r.width + p * 2, height: r.height + p * 2 });

export function Canvas() {
  const s = useStore();
  const styles = useStyles();
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
      const r = el.getBoundingClientRect(), margin = 24, ratio = shape.width / shape.height;
      let w = Math.max(0, r.width - margin * 2), h = w / ratio;
      if (h > r.height - margin * 2) {
        h = Math.max(0, r.height - margin * 2);
        w = h * ratio;
      }
      w = Math.floor(w);
      h = Math.floor(h);
      setStage({ left: Math.round((r.width - w) / 2), top: Math.round((r.height - h) / 2), width: w, height: h });
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
  useEffect(() => {
    if (drag?.settling && pv.shownSeq >= drag.settling) {
      setDrag(null);
      hold(null);
    }
  });
  useEffect(() => {
    if (!drag?.settling) return;
    const timer = window.setTimeout(() => { setDrag(null); hold(null); }, 4000);
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
    if ((e.target as HTMLElement).dataset.handle) {
      e.preventDefault();
      startResize(e);
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

  function startDrag(e: ReactPointerEvent, inst: string, pk: string, partOnClick: string | null) {
    const g = geometry(inst, pk, k, stage, getState().screen);
    if (!g) return;
    const lift = inst !== 'wallpaper' && copiesOf(inst, pk) === 1 ? liftFor(inst) : null;
    const gesture = newGesture();
    let last: Record<string, string> | null = null;
    track((dx, dy) => {
      const place = placeDragged(g, dx, dy);
      last = { [pk + '.anchor']: place.anchor, [pk + '.offset-x']: String(place.ox), [pk + '.offset-y']: String(place.oy) };
      if (lift) hold(lift);
      setDrag({ inst, pk, g, lift, left: place.left, top: place.top, anchor: place.anchor, col: place.col, row: place.row, bx: place.bx, by: place.by,
        rx: place.rx, ry: place.ry, snapX: place.snapX, snapY: place.snapY, readout: posText(place.anchor, place.ox, place.oy) });
      if (!lift) liveSet(inst, last, gesture);
    }, moved => {
      if (!moved) {
        setDrag(null);
        if (partOnClick) select(inst, partOnClick);
        return;
      }
      if (lift && last) {
        const seq = setValues(inst, last, gesture);
        setDrag(d => (d ? { ...d, settling: seq } : null));
      } else {
        endGesture();
        setDrag(null);
      }
    }, e.clientX, e.clientY);
  }

  function startResize(e: ReactPointerEvent) {
    const sel = getState().sel, inst = sel.inst;
    if (!inst || !instOf(inst)) return;
    const pk = panelKey(sel), g = geometry(inst, pk, k, stage, getState().screen);
    if (!g) return;
    const a = adjustment(inst, pk + '.size'), s0 = +(a ? a.value : 10);
    const rs = stageRef.current!.getBoundingClientRect(), cx = rs.left + g.r.left + g.r.width / 2, cy = rs.top + g.r.top + g.r.height / 2;
    const d0 = Math.max(8, Math.hypot(e.clientX - cx, e.clientY - cy));
    const full = inst === 'wallpaper' && valueOf(inst, pk + '.anchor') === 'full';
    const lift = inst !== 'wallpaper' && copiesOf(inst, pk) === 1 ? liftFor(inst) : null;
    const gesture = newGesture();
    let last: Record<string, string> | null = null;
    track((_dx, _dy, ev) => {
      const size = clamp(roundTo((s0 * Math.hypot(ev.clientX - cx, ev.clientY - cy)) / d0, 0.5), a ? a.min : 1, a ? a.max : 50);
      last = { [pk + '.size']: String(size) };
      if (full) last[pk + '.anchor'] = 'center';
      if (lift) hold(lift);
      setDrag({ inst, pk, g, lift, left: g.r.left, top: g.r.top, scale: size / s0, readout: `大小 ${size.toFixed(1)}% 屏高` });
      if (!lift) liveSet(inst, last, gesture);
    }, moved => {
      if (!moved) return setDrag(null);
      if (lift && last) {
        const seq = setValues(inst, last, gesture);
        setDrag(d => (d ? { ...d, settling: seq } : null));
      } else {
        endGesture();
        setDrag(null);
      }
    }, e.clientX, e.clientY);
  }

  /* --- the marks --- */
  const sel = s.sel;
  const whole = !!sel.inst && multiPanel(sel.inst) && !sel.part;
  const pk = panelKey(sel);
  let selRect: Rect | null = null, partRect: Rect | null = null, g: Geometry | null = null;
  if (sel.inst && k) {
    if (whole) {
      const all = panelBounds(sel.inst);
      if (all.length) selRect = rectOf(unionOf(all), k);
    } else {
      const b = boundOf(sel.inst, pk, s.screen);
      if (b) {
        selRect = rectOf(b, k);
        g = geometry(sel.inst, pk, k, stage, s.screen);
        const pb = sel.part && sel.part.includes('.') ? boundOf(sel.inst, sel.part, s.screen) : undefined;
        if (pb) partRect = rectOf(pb, k);
      }
    }
  }
  // While dragging, the box is where the panel is going.
  if (drag && selRect && drag.inst === sel.inst) {
    if (drag.scale != null) {
      const sc = drag.scale, ox = drag.g.rx, oy = drag.g.ry, r = drag.g.r;
      selRect = { left: ox + (r.left - ox) * sc, top: oy + (r.top - oy) * sc, width: r.width * sc, height: r.height * sc };
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
  const readoutStyle = (r: Rect): CSSProperties => ({ left: Math.max(4, r.left - 6), top: r.top - 6 - 26 < 4 ? r.top + r.height + 12 : r.top - 6 - 26 });

  /* --- the lifted component --- */
  const lifted = drag?.lift ?? null;
  let sprite: CSSProperties | null = null, rest: CSSProperties | null = null;
  if (lifted && drag) {
    const multi = multiPanel(drag.inst), r = drag.g.r, W = stage.width, H = stage.height, m = 8;
    const transform = drag.scale != null ? `scale(${drag.scale})` : `translate(${drag.left - r.left}px, ${drag.top - r.top}px)`;
    sprite = {
      backgroundImage: `url(${lifted.only})`,
      transform,
      transformOrigin: `${drag.g.rx}px ${drag.g.ry}px`,
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
    <div className={styles.area}>
      <div className={styles.well} ref={well}>
        {stage.width > 0 && (
          <div
            ref={stageRef}
            data-stage
            className={mergeClasses(styles.stage, over && styles.over, !!drag && !drag.settling && styles.dragging)}
            style={{ left: stage.left, top: stage.top, width: stage.width, height: stage.height }}
            onPointerDown={onDown}
            onPointerMove={onMove}
            onPointerLeave={() => { setOver(false); if (!dragRef.current) setHover(null); }}
          >
            {scene && pv.width > 0 && (
              <SceneFrames theme={t.id} width={pv.width} height={pv.height} moving={motion() && !scene.static} refresh={pv.rev}
                onBounds={rects => { pv.bounds = pv.bounds.filter(b => b.instance !== 'wallpaper').concat(rects as Bound[]); redraw(); }} />
            )}
            {video && <WallpaperVideo key={wallpaper.ref} wallpaper={wallpaper.ref} fit={t.fit} />}
            {pv.url && <Picture url={pv.url} hidden={!!lifted} />}
            {lifted && <img className={styles.layer} src={lifted.base} alt="" draggable={false} />}
            {rest && <div className={styles.sprite} style={rest} />}
            {sprite && <div className={styles.sprite} style={sprite} />}
            {pv.previous && <Leaving url={pv.previous} />}

            <svg className={styles.marks}>
              {pv.screens.length > 1 && pv.screens.map((sc, i) => {
                const a = { x: sc.left * k + 0.5, y: sc.top * k + 0.5, width: Math.max(0, (sc.right - sc.left) * k - 1), height: Math.max(0, (sc.bottom - sc.top) * k - 1), fill: 'none' };
                return (
                  <g key={i}>
                    <rect {...a} stroke="rgba(0,0,0,.45)" strokeWidth={3} />
                    <rect {...a} stroke="rgba(255,255,255,.7)" strokeWidth={1} strokeDasharray="6 4" />
                  </g>
                );
              })}
              {drag && drag.scale == null && <Anchors g={drag.g} />}
              {drag && drag.scale == null && drag.snapX && <Guide x1={drag.bx!} y1={drag.g.f.top} x2={drag.bx!} y2={drag.g.f.top + drag.g.f.height} />}
              {drag && drag.scale == null && drag.snapY && <Guide x1={drag.g.f.left} y1={drag.by!} x2={drag.g.f.left + drag.g.f.width} y2={drag.by!} />}
              {g && !whole && <Tether g={g} drag={drag} />}
            </svg>

            {hoverRects.map((r, i) => <div key={i} className={mergeClasses(styles.box, styles.hover)} style={pad(r, 4)} />)}
            {partRect && <div className={mergeClasses(styles.box, styles.part)} style={pad(partRect, 2)} />}
            {selRect && (
              <div className={mergeClasses(styles.box, styles.selected, (drag || calm()) && styles.still)} style={pad(selRect, 6)}>
                {!whole && !drag?.settling && HANDLES.map(([name, at]) => <span key={name} data-handle={name} className={styles.handle} style={at} />)}
              </div>
            )}
            {drag && !drag.settling && selRect && <div className={styles.readout} style={readoutStyle(selRect)}>{drag.readout}</div>}
          </div>
        )}
      </div>
      <div className={styles.foot}>
        <Menu>
          <MenuTrigger disableButtonEnhancement>
            <Button appearance="subtle" size="small" icon={<ChevronDownRegular />} iconPosition="after">
              {spanning() ? '横跨所有显示器' : `${m.name} · ${ratioLabel(m.width, m.height)}`}
            </Button>
          </MenuTrigger>
          <MenuPopover>
            <MenuList>
              {[[16, 10], [21, 9], [32, 9], [4, 3]].map(([a, b]) => (
                <MenuItem key={a + ':' + b} onClick={() => setState({ lock: { theme: t.id, ratio: a / b } })}>{`以 ${a}:${b} 预览锁屏`}</MenuItem>
              ))}
            </MenuList>
          </MenuPopover>
        </Menu>
        {!s.lib!.settings.components && <Caption1 className={styles.quiet}>组件已在设置里关闭，锁屏上只显示壁纸。</Caption1>}
      </div>
    </div>
  );
}

// The picture on the stage; the first one of a page fades in.
function Picture({ url, hidden }: { url: string; hidden: boolean }) {
  const styles = useStyles();
  const ref = useRef<HTMLImageElement>(null);
  const first = useRef(true);
  useEffect(() => {
    if (first.current && !calm()) ref.current?.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 150, easing: 'linear' });
    first.current = false;
  }, [url]);
  return <img ref={ref} className={styles.layer} style={hidden ? { visibility: 'hidden' } : undefined} src={url} alt="" draggable={false} />;
}

// The picture before a new wallpaper or fit, fading out over the new one.
function Leaving({ url }: { url: string }) {
  const styles = useStyles();
  const ref = useRef<HTMLImageElement>(null);
  useEffect(() => {
    if (calm() || !ref.current) return forgetPrevious();
    const a = ref.current.animate([{ opacity: 1 }, { opacity: 0 }], { duration: 250, easing: 'linear', fill: 'forwards' });
    a.onfinish = () => forgetPrevious();
    return () => a.cancel();
  }, [url]);
  return <img ref={ref} className={styles.layer} src={url} alt="" draggable={false} />;
}

function Anchors({ g }: { g: Geometry }) {
  const dots = [];
  for (let r = 0; r < 3; r++)
    for (let c = 0; c < 3; c++)
      dots.push(<circle key={r * 3 + c} cx={g.f.left + [g.M, g.W / 2, g.W - g.M][c]} cy={g.f.top + [g.M, g.H / 2, g.H - g.M][r]} r={3} fill="rgba(255,255,255,.7)" stroke="rgba(0,0,0,.4)" />);
  return <>{dots}</>;
}

function Guide(p: { x1: number; y1: number; x2: number; y2: number }) {
  return (
    <>
      <line {...p} stroke="rgba(0,0,0,.45)" strokeWidth={3} />
      <line {...p} stroke="#fff" strokeWidth={1} />
    </>
  );
}

// The anchor the selected panel is placed by, and a dashed line to the point of it held there.
function Tether({ g, drag }: { g: Geometry; drag: Drag | null }) {
  let bx = g.bx, by = g.by, rx = g.rx, ry = g.ry;
  if (drag && drag.scale == null && drag.rx != null && drag.ry != null) {
    bx = drag.bx!;
    by = drag.by!;
    rx = drag.rx;
    ry = drag.ry;
  }
  return (
    <>
      {Math.hypot(rx - bx, ry - by) > 2 && (
        <>
          <line x1={bx} y1={by} x2={rx} y2={ry} stroke="rgba(0,0,0,.45)" strokeWidth={3} />
          <line x1={bx} y1={by} x2={rx} y2={ry} stroke="#fff" strokeWidth={1.2} strokeDasharray="4 3" />
        </>
      )}
      <circle cx={bx} cy={by} r={5} fill="#fff" stroke="rgba(0,0,0,.55)" strokeWidth={1.5} />
    </>
  );
}
