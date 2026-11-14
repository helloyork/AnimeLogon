// One control for one value of the open theme, as a row of the panel's grid: the label, the
// control, and room at the end for the button that puts a changed value back. Sliders and the
// colour picker show their changes live and save when let go; a typed number, a choice or a
// switch saves at once.
import { useEffect, useMemo, useRef, useState, type PointerEvent as ReactPointerEvent, type ReactNode } from 'react';
import { AlphaSlider, ColorArea, ColorPicker, ColorSlider, Popover, PopoverSurface, PopoverTrigger, Tooltip } from '@fluentui/react-components';
import { endGesture, liveSet, newGesture, setValue } from '../preview';
import { t, type Key } from '../i18n';
import { useStore } from '../store';
import type { Adjustment, Setting } from '../types';
import { Delete16Regular } from '@fluentui/react-icons';
import { cx, IconButton, NumberInput, ResetButton, SearchSelect, Segmented, Select, Slider, Switch, type Choice } from '../ui/controls';
import { clamp, decimals, roundTo } from './geometry';

interface Common {
  id: string;
  label: string;
  detail?: string;
  modified: boolean;
  onReset: () => void;
  sub?: boolean;  // set under the row above it, which turns it on (a strength under its switch)
}

// What a value is, said on the label: a tooltip, for those who point at it.
function Name({ label, detail }: { label: string; detail?: string }) {
  if (!detail) return <>{label}</>;
  return (
    <Tooltip content={detail} relationship="description" positioning="above-start" withArrow={false}>
      <span>{label}</span>
    </Tooltip>
  );
}

function Reset({ modified, onReset }: { modified: boolean; onReset?: () => void }) {
  return <span className="preset">{modified && onReset && <ResetButton onClick={onReset} />}</span>;
}

// A row of the grid: the label in the first column, the control and its reset button in the second.
export function Prop({ id, label, detail, modified = false, onReset, top, sub, children }: Partial<Common> & { label: string; top?: boolean; children: ReactNode }) {
  return (
    <>
      <label className={cx('plabel', top && 'top', sub && 'sub')} htmlFor={id} id={id ? id + '-label' : undefined}>
        <Name label={label} detail={detail} />
      </label>
      <div className="pctl">
        <div className="pmain">{children}</div>
        <Reset modified={modified} onReset={onReset} />
      </div>
    </>
  );
}

/* --- numbers --- */
// The weights' common names, by the number CSS and DirectWrite give them.
const weightName = (w: number) => (w % 100 === 0 && w >= 100 && w <= 900 ? t(`weight.${w}` as Key) : '');

// How a value reads beside its slider: in what units, scaled how, with how many digits.
export interface View {
  factor: number;
  digits: number;
  unit: (v: number) => string;
}
export function viewFor(key: string, step: number): View {
  const attr = key.split('.').pop() || '', panel = /^p\d+\.[^.]+$/.test(key);
  const plain = (unit: string, digits: number): View => ({ factor: 1, digits, unit: () => unit });
  // A spectrum's (p1.l1.a1.gap): shares of something in percent, frequencies in hertz.
  if (/\.a\d+\.[^.]+$/.test(key)) {
    switch (attr) {
      case 'gap': case 'floor': case 'idle-opacity': return { factor: 100, digits: 0, unit: () => '%' };
      case 'peak': return { factor: 100, digits: 0, unit: v => (!v ? t('unit.none') : '%') };
      case 'low': case 'high': return plain('Hz', 0);
      case 'sensitivity': return plain(t('unit.times'), 2);
      case 'stroke': return plain('em', 3);
      case 'flow': return { factor: 1, digits: 1, unit: v => (v ? t('unit.seconds') : t('unit.none')) };
    }
  }
  switch (attr) {
    case 'offset-x': return plain(t('unit.screen_width'), 1);
    case 'offset-y': return plain(t('unit.screen_height'), 1);
    case 'size': return panel ? plain(t('unit.screen_height'), 1) : plain(t('unit.times'), 2);
    case 'margin': return plain(t('unit.short_side'), 1);
    case 'tracking': case 'x': case 'y': case 'blur': case 'rise': return plain('em', 3);
    case 'space': case 'gap': case 'padding-x': case 'padding-y': return plain('em', 2);
    // A plate's corners are in em; a bar's are a share of its thickness.
    case 'radius': return /\.b\.radius$/.test(key) ? plain('em', 2) : plain('', decimals(step));
    case 'max': return { factor: 1, digits: 1, unit: v => (v ? 'em' : t('unit.none')) };
    case 'enter-delay': case 'enter-duration': return plain(t('unit.seconds'), 2);
    // A spectrum's colours flowing, as the built-in spectrum's setting names it.
    case 'flow': return { factor: 1, digits: 1, unit: v => (v ? t('unit.seconds') : t('unit.none')) };
    case 'opacity': return { factor: 100, digits: 0, unit: v => (/\.(s\d+|b)\.opacity$/.test(key) && !v ? t('unit.none') : '%') };
  }
  return plain('', decimals(step));
}

export function NumberField({ id, label, detail, min, max, step, value, modified, view, write, onReset, sub }: Common & {
  min: number;
  max: number;
  step: number;
  value: string;
  view: View;
  write: (v: number, gesture?: number) => void;
}) {
  const [live, setLive] = useState<number | null>(null);
  const gesture = useRef(0);
  // What the native side says wins once it has answered; while the slider is held, the slider
  // does, since a value drawn from the moving picture trails the pointer.
  useEffect(() => {
    if (!gesture.current) setLive(null);
  }, [value]);
  const shown = live ?? +value;
  const end = () => {
    if (!gesture.current) return;
    gesture.current = 0;
    endGesture();
  };
  const f = view.factor, display = +(shown * f).toFixed(view.digits);
  return (
    <Prop id={id} label={label} detail={detail} modified={modified} onReset={onReset} sub={sub}>
      <Slider id={id} label={label} min={min} max={max} step={step} value={shown} onDone={end}
        onChange={v => {
          if (!gesture.current) gesture.current = newGesture();
          setLive(v);
          write(v, gesture.current);
        }} />
      <NumberInput label={label} value={display} min={+(min * f).toFixed(view.digits)} max={+(max * f).toFixed(view.digits)} step={+(step * f).toFixed(6)} digits={view.digits}
        unit={view.unit(display)}
        onCommit={typed => {
          const v = clamp(roundTo(typed / f, step), min, max);
          setLive(v);
          write(v);
        }} />
    </Prop>
  );
}

/* --- choices, switches, dates --- */
// A few short choices side by side; a longer list drops down.
export function ChoiceField({ id, label, detail, options, value, modified, write, onReset }: Common & { options: Choice[]; value: string; write: (v: string) => void }) {
  const short = options.length <= 4 && options.every(o => o.label.length <= 2);
  return (
    <Prop id={id} label={label} detail={detail} modified={modified} onReset={onReset}>
      {short ? <Segmented id={id} label={label} options={options} value={value} onChange={write} />
        : <Select id={id} label={label} size="sm" options={options} value={value} onChange={write} />}
    </Prop>
  );
}

// A switch sits at the end of its row, its label free to take the row's width.
export function ToggleField({ id, label, detail, value, modified, write, onReset }: Partial<Common> & { id: string; label: string; value: boolean; write: (on: boolean) => void }) {
  return (
    <div className="ptoggle">
      <label className="plabel" htmlFor={id}><Name label={label} detail={detail} /></label>
      <Switch id={id} checked={value} onChange={write} />
      <Reset modified={!!modified} onReset={onReset} />
    </div>
  );
}

export function DateField({ id, label, detail, value, modified, write, onReset }: Common & { value: string; write: (v: string) => void }) {
  const [text, setText] = useState(value);
  useEffect(() => setText(value), [value]);
  return (
    <Prop id={id} label={label} detail={detail} modified={modified} onReset={onReset}>
      <label className="field sm">
        <input id={id} type="date" value={text} min="1601-01-01" max="9999-12-31" onChange={e => setText(e.currentTarget.value)}
          onBlur={() => (/^\d{4}-\d{2}-\d{2}$/.test(text) ? text !== value && write(text) : setText(value))} />
      </label>
    </Prop>
  );
}

/* --- fonts --- */
const fontStack = (stored: string) => (stored ? `"${stored}", var(--font)` : 'var(--font-display)');

// Only fonts installed for every user: the logon screen runs as the system and sees no others.
export function FontField({ id, label, detail, value, modified, write, onReset }: Common & { value: string; write: (v: string) => void }) {
  const { env } = useStore();
  const options = useMemo(() => [{ value: '', label: t('font.default') }, ...env.fonts.map(f => ({ value: f.stored, label: f.name }))], [env.fonts, env.lang]);
  return (
    <Prop id={id} label={label} detail={detail ?? t('font.detail')} modified={modified} onReset={onReset}>
      <SearchSelect id={id} label={label} options={options} value={value} onChange={write} placeholder={t('font.search')} render={o => ({ fontFamily: fontStack(o.value) })} />
    </Prop>
  );
}

/* --- colours --- */
type HsvColor = { h: number; s: number; v: number; a?: number };  // as ColorPicker takes it
const SWATCHES: [Key, string][] = [['color.white', '#FFFFFF'], ['color.warm', '#FFF1DC'], ['color.pink', '#FFC6D6'], ['color.sky', '#A8D8FF'], ['color.gold', '#FFD27A'], ['color.ink', '#16181F']];

function hexToHsv(hex: string): HsvColor {
  const m = /^#?([0-9a-f]{6})([0-9a-f]{2})?$/i.exec(hex) || ['', 'ffffff', ''];
  const n = parseInt(m[1], 16), r = (n >> 16) / 255, g = ((n >> 8) & 255) / 255, b = (n & 255) / 255;
  const max = Math.max(r, g, b), min = Math.min(r, g, b), d = max - min;
  let h = 0;
  if (d) h = max === r ? ((g - b) / d) % 6 : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
  return { h: (h * 60 + 360) % 360, s: max ? d / max : 0, v: max, a: m[2] ? parseInt(m[2], 16) / 255 : 1 };
}
function hsvToHex({ h, s, v, a = 1 }: HsvColor) {
  const f = (n: number) => {
    const k = (n + h / 60) % 6;
    return Math.round((v - v * s * Math.max(0, Math.min(k, 4 - k, 1))) * 255);
  };
  const hex = (x: number) => x.toString(16).padStart(2, '0').toUpperCase();
  return '#' + hex(f(5)) + hex(f(3)) + hex(f(1)) + (a < 1 ? hex(Math.round(a * 255)) : '');
}

// The colour as typed, its chip opening a picker, and a row of colours that suit a lock screen.
export function ColorField({ id, label, detail, value, modified, write, onReset }: Common & { value: string; write: (v: string, gesture?: number) => void }) {
  const v0 = String(value).toUpperCase();
  const [hex, setHex] = useState(v0);
  const [hsv, setHsv] = useState<HsvColor>(() => hexToHsv(v0));
  const gesture = useRef(0);
  useEffect(() => {
    setHex(v0);
    setHsv(hexToHsv(v0));
  }, [v0]);
  const commitHex = () => {
    const v = hex.trim().toUpperCase();
    if (/^#[0-9A-F]{6}([0-9A-F]{2})?$/.test(v)) { if (v !== v0) write(v); } else setHex(v0);
  };
  return (
    <Prop id={id} label={label} detail={detail} modified={modified} onReset={onReset} top>
      <div className="pcolor">
        <div className="field sm mono">
          <Popover positioning={{ position: 'below', align: 'start', offset: 6 }} trapFocus
            onOpenChange={(_, d) => { if (!d.open && gesture.current) { gesture.current = 0; endGesture(); } }}>
            <PopoverTrigger disableButtonEnhancement>
              <button type="button" className="chip" style={{ background: v0 }} aria-label={t('color.custom')} />
            </PopoverTrigger>
            <PopoverSurface className="pop picker" aria-label={t('color.custom')}>
              <ColorPicker color={hsv} onColorChange={(_, d) => {
                setHsv(d.color);
                const next = hsvToHex(d.color);
                setHex(next);
                if (!gesture.current) gesture.current = newGesture();
                write(next, gesture.current);
              }}>
                <ColorArea />
                <ColorSlider />
                <AlphaSlider />
              </ColorPicker>
            </PopoverSurface>
          </Popover>
          <input id={id} value={hex} maxLength={9} spellCheck={false} aria-label={t('color.hex', { label })} onChange={e => setHex(e.currentTarget.value)}
            onBlur={commitHex} onKeyDown={e => e.key === 'Enter' && commitHex()} />
        </div>
        <div className="swatches" role="radiogroup" aria-label={label}>
          {SWATCHES.map(([name, c]) => (
            <button key={c} type="button" role="radio" aria-checked={v0.slice(0, 7) === c && v0.length === 7} aria-label={t(name)} title={t(name)} style={{ background: c }}
              onClick={() => write(c)} />
          ))}
        </div>
      </div>
    </Prop>
  );
}

/* --- gradients --- */
// Colours at places along a gradient, stored as "#RRGGBB[AA] P%, ..." (skin::ParseStops): one to
// eight, in order of place. One colour is a plain colour. `key` follows a stop while it is dragged
// past another and the list is sorted again.
interface GStop { at: number; color: string; key: number }
const MAX_STOPS = 8;
let stopKeys = 0;
const PRESETS: [Key, string][] = [
  ['gradient.preset.sky', '#8FD3FF 0%, #C6B9FF 50%, #F3A6FF 100%'],
  ['gradient.preset.rainbow', '#FF5E5E 0%, #FFB347 17%, #FFF06A 33%, #7CFF8A 50%, #5EC8FF 67%, #8F7CFF 83%, #E77CFF 100%'],
  ['gradient.preset.neon', '#00F5FF 0%, #7B2FFF 50%, #FF2E9A 100%'],
  ['gradient.preset.sunset', '#FFB36B 0%, #FF6F91 50%, #8E5CFF 100%'],
  ['gradient.preset.ocean', '#00C9A7 0%, #0089BA 50%, #2C3E9E 100%'],
  ['gradient.preset.forest', '#C6FF8E 0%, #4BC27A 100%'],
  ['gradient.preset.fire', '#FFE259 0%, #FF7A3D 50%, #E52D27 100%'],
  ['gradient.preset.white', '#FFFFFF 0%'],
];

function parseStops(text: string): GStop[] {
  const out: GStop[] = [];
  for (const part of String(text).split(',')) {
    const m = /^\s*(#[0-9a-f]{6}(?:[0-9a-f]{2})?)\s+(\d+(?:\.\d+)?)%\s*$/i.exec(part);
    if (!m || +m[2] > 100) return [{ at: 0, color: '#FFFFFF', key: ++stopKeys }];
    out.push({ at: +m[2], color: m[1].toUpperCase(), key: ++stopKeys });
  }
  return out.sort((a, b) => a.at - b.at);
}
const formatStops = (stops: GStop[]) => stops.map(s => `${s.color} ${+s.at.toFixed(1)}%`).join(', ');
// As CSS draws it, for the bar and the presets.
const cssGradient = (stops: { at: number; color: string }[]) =>
  stops.length === 1 ? stops[0].color : `linear-gradient(to right, ${stops.map(s => `${s.color} ${s.at}%`).join(', ')})`;

// The colour the gradient has at `at`, so a stop added there leaves the gradient as it was.
function colorAt(stops: GStop[], at: number): string {
  const channels = (hex: string) => [1, 3, 5, 7].map(i => (i < hex.length ? parseInt(hex.slice(i, i + 2), 16) : 255));
  let i = stops.findIndex(s => s.at >= at);
  if (i <= 0) return stops[i < 0 ? stops.length - 1 : 0].color;
  const a = stops[i - 1], b = stops[i], f = b.at > a.at ? (at - a.at) / (b.at - a.at) : 0;
  const ca = channels(a.color), cb = channels(b.color);
  const mixed = ca.map((c, k) => Math.round(c + (cb[k] - c) * f));
  const hex = (x: number) => x.toString(16).padStart(2, '0').toUpperCase();
  return '#' + mixed.slice(0, 3).map(hex).join('') + (mixed[3] < 255 ? hex(mixed[3]) : '');
}

// A bar showing the gradient, a handle on it for each colour: pressing the bar adds a colour
// there and dragging moves it, as does dragging a handle. Below, the chosen colour and its place,
// and a row of gradients to start from.
export function GradientField({ id, label, detail, value, modified, write, onReset }: Common & { value: string; write: (v: string, gesture?: number) => void }) {
  const [stops, setStops] = useState<GStop[]>(() => parseStops(value));
  const [sel, setSel] = useState(() => stops[0].key);
  const [hsv, setHsv] = useState<HsvColor>(() => hexToHsv(stops[0].color));
  const [hex, setHex] = useState(stops[0].color);
  const written = useRef(String(value));
  const gesture = useRef(0);
  const bar = useRef<HTMLDivElement>(null);
  const chosen = stops.find(s => s.key === sel) ?? stops[0];
  // A value from elsewhere (put back, another theme) is read again, the same place in the list
  // staying chosen; the echo of what this field wrote, or one that arrives mid-drag, is not.
  useEffect(() => {
    if (gesture.current || String(value) === written.current) return;
    written.current = String(value);
    const next = parseStops(value), at = Math.max(0, stops.findIndex(s => s.key === sel));
    setStops(next);
    setSel(next[Math.min(at, next.length - 1)].key);
  }, [value]);
  useEffect(() => {
    setHex(chosen.color);
    setHsv(h => (hsvToHex(h) === chosen.color ? h : hexToHsv(chosen.color)));
  }, [chosen.color, chosen.key]);

  const put = (next: GStop[], live?: number) => {
    next.sort((a, b) => a.at - b.at);
    setStops(next);
    written.current = formatStops(next);
    write(written.current, live);
  };
  const change = (key: number, edit: Partial<GStop>, live?: number) => put(stops.map(s => (s.key === key ? { ...s, ...edit } : s)), live);
  const remove = (key: number) => {
    if (stops.length < 2) return;
    const at = stops.findIndex(s => s.key === key), next = stops.filter(s => s.key !== key);
    setSel(next[Math.min(at, next.length - 1)].key);
    put(next);
  };
  const placeAt = (clientX: number) => {
    const r = bar.current!.getBoundingClientRect();
    return clamp(Math.round(((clientX - r.left) / r.width) * 100), 0, 100);
  };
  // Drags `key` with the pointer until it is let go; the picture follows, settings.ini waits.
  const drag = (e: ReactPointerEvent, key: number, from: GStop[]) => {
    e.preventDefault();
    const target = e.currentTarget as HTMLElement;
    target.setPointerCapture(e.pointerId);
    let list = from;
    const move = (ev: PointerEvent) => {
      const at = placeAt(ev.clientX);
      if (list.find(s => s.key === key)!.at === at) return;
      if (!gesture.current) gesture.current = newGesture();
      list = list.map(s => (s.key === key ? { ...s, at } : s)).sort((a, b) => a.at - b.at);
      put(list, gesture.current);
    };
    const up = () => {
      target.removeEventListener('pointermove', move);
      target.removeEventListener('pointerup', up);
      target.removeEventListener('pointercancel', up);
      if (gesture.current) { gesture.current = 0; endGesture(); }
    };
    target.addEventListener('pointermove', move);
    target.addEventListener('pointerup', up);
    target.addEventListener('pointercancel', up);
  };
  const full = stops.length >= MAX_STOPS;
  const commitHex = () => {
    const v = hex.trim().toUpperCase();
    if (/^#[0-9A-F]{6}([0-9A-F]{2})?$/.test(v)) { if (v !== chosen.color) change(chosen.key, { color: v }); } else setHex(chosen.color);
  };
  const current = formatStops(stops);
  return (
    <Prop id={id} label={label} detail={detail} modified={modified} onReset={onReset} top>
      <div className="pgrad">
        <div ref={bar} className={cx('gbar', full && 'full')} role="group" aria-label={label} title={full ? t('gradient.full', { n: MAX_STOPS }) : t('gradient.hint')}
          onPointerDown={e => {
            if (e.button !== 0 || full || e.target !== e.currentTarget) return;
            // A colour added where the bar is pressed, as the gradient already is there, then dragged.
            const at = placeAt(e.clientX), added = { at, color: colorAt(stops, at), key: ++stopKeys };
            const next = [...stops, added].sort((a, b) => a.at - b.at);
            setSel(added.key);
            gesture.current = newGesture();
            put(next, gesture.current);
            drag(e, added.key, next);
          }}>
          <i className="gfill" style={{ background: cssGradient(stops) }} />
          {stops.map((s, n) => (
            <button key={s.key} type="button" role="slider" className={cx('gstop', s.key === chosen.key && 'on')} style={{ left: `${s.at}%`, color: s.color }}
              id={n === 0 ? id : undefined} aria-label={t('gradient.stop', { n: n + 1 })} aria-valuemin={0} aria-valuemax={100} aria-valuenow={s.at} aria-valuetext={`${s.at}%, ${s.color}`}
              onPointerDown={e => { if (e.button === 0) { setSel(s.key); drag(e, s.key, stops); } }}
              onFocus={() => setSel(s.key)}
              onKeyDown={e => {
                const step = e.shiftKey ? 10 : 1;
                if (e.key === 'ArrowLeft' || e.key === 'ArrowDown') change(s.key, { at: Math.max(0, Math.round(s.at) - step) });
                else if (e.key === 'ArrowRight' || e.key === 'ArrowUp') change(s.key, { at: Math.min(100, Math.round(s.at) + step) });
                else if (e.key === 'Home') change(s.key, { at: 0 });
                else if (e.key === 'End') change(s.key, { at: 100 });
                else if (e.key === 'Delete' || e.key === 'Backspace') remove(s.key);
                else return;
                e.preventDefault();
              }} />
          ))}
        </div>
        <div className="gedit">
          <div className="field sm mono">
            <Popover positioning={{ position: 'below', align: 'start', offset: 6 }} trapFocus
              onOpenChange={(_, d) => { if (!d.open && gesture.current) { gesture.current = 0; endGesture(); } }}>
              <PopoverTrigger disableButtonEnhancement>
                <button type="button" className="chip" style={{ background: chosen.color }} aria-label={t('color.custom')} />
              </PopoverTrigger>
              <PopoverSurface className="pop picker" aria-label={t('color.custom')}>
                <ColorPicker color={hsv} onColorChange={(_, d) => {
                  setHsv(d.color);
                  if (!gesture.current) gesture.current = newGesture();
                  change(chosen.key, { color: hsvToHex(d.color) }, gesture.current);
                }}>
                  <ColorArea />
                  <ColorSlider />
                  <AlphaSlider />
                </ColorPicker>
              </PopoverSurface>
            </Popover>
            <input value={hex} maxLength={9} spellCheck={false} aria-label={t('color.hex', { label: t('gradient.stop', { n: stops.indexOf(chosen) + 1 }) })}
              onChange={e => setHex(e.currentTarget.value)} onBlur={commitHex} onKeyDown={e => e.key === 'Enter' && commitHex()} />
          </div>
          <NumberInput label={t('gradient.position')} value={chosen.at} min={0} max={100} step={1} digits={0} unit="%"
            onCommit={at => change(chosen.key, { at })} />
          <IconButton size="sm" label={t('gradient.remove')} icon={<Delete16Regular />} disabled={stops.length < 2} onClick={() => remove(chosen.key)} />
        </div>
        <div className="swatches gpresets" role="group" aria-label={t('gradient.presets')}>
          {PRESETS.map(([name, v]) => (
            <button key={name} type="button" aria-pressed={current === v} aria-label={t(name)} title={t(name)} style={{ background: cssGradient(parseStops(v)) }}
              onClick={() => {
                const next = parseStops(v);
                setSel(next[0].key);
                put(next);
              }} />
          ))}
        </div>
      </div>
    </Prop>
  );
}

/* --- a setting or an adjustment of an instance --- */
export function writer(instId: string, key: string) {
  return (v: string | number, gesture?: number) => (gesture ? liveSet(instId, { [key]: String(v) }, gesture) : setValue(instId, key, v));
}

export function AdjustmentField({ instId, a, onReset, sub }: { instId: string; a: Adjustment; onReset: () => void; sub?: boolean }) {
  const id = `adj-${instId}-${a.key}`.replace(/\./g, '_');
  const write = writer(instId, a.key);
  const common = { id, label: a.label, detail: a.detail || undefined, modified: a.modified, onReset, sub };
  switch (a.control) {
    case 'slider':
      // A font's weight is one of its named weights, as Windows' and Office's font lists say them.
      if (a.key.endsWith('.weight')) {
        const options = [];
        for (let w = Math.ceil(a.min / 100) * 100; w <= a.max; w += 100) options.push({ value: String(w), label: t('weight.label', { name: weightName(w), w }) });
        return <ChoiceField {...common} options={options} value={String(Math.round(+a.value / 100) * 100)} write={write} />;
      }
      return <NumberField {...common} min={a.min} max={a.max} step={a.step} value={a.value} view={viewFor(a.key, a.step)} write={write} />;
    case 'choice':
      return <ChoiceField {...common} options={a.options} value={a.value} write={write} />;
    case 'color':
      return <ColorField {...common} value={a.value} write={write} />;
    case 'colors':
      return <GradientField {...common} value={a.value} write={write} />;
    case 'font':
      return <FontField {...common} value={a.value} write={write} />;
    case 'toggle':
      return <ToggleField {...common} value={a.value === 'true'} write={on => write(on ? 'true' : 'false')} />;
  }
}

// A switch that turns one value on and off, with the slider for that value under it while the
// switch is in the state that shows it (the clock's backdrop and how strong it is). The slider
// starts at the switch's own value; turning the switch puts the slider back, so off is off and on
// starts again from there. The switch's reset puts back both. The slider also shows where its value
// was set by hand in the other state, so that what is drawn can be seen and put back.
function StrengthField({ instId, setting: s, strength, onReset }: { instId: string; setting: Setting; strength: NonNullable<Setting['strength']>; onReset: (keys: string[]) => void }) {
  const write = writer(instId, s.id);
  return (
    <>
      <ToggleField id={`set-${instId}-${s.id}`} label={s.label} detail={s.detail || undefined} modified={s.modified || strength.modified}
        value={s.value === 'on'} write={on => write(on ? 'on' : 'off')} onReset={() => onReset([s.id, strength.key])} />
      {(s.value === strength.when || strength.modified) && (
        <AdjustmentField instId={instId} a={strength} sub onReset={() => onReset([strength.key])} />
      )}
    </>
  );
}

// `onReset` is given the keys to put back: the setting's own, and with a strength its slider's.
export function SettingField({ instId, setting: s, onReset }: { instId: string; setting: Setting; onReset: (keys: string[]) => void }) {
  if (s.kind === 'toggle' && s.strength) return <StrengthField instId={instId} setting={s} strength={s.strength} onReset={onReset} />;
  const id = `set-${instId}-${s.id}`;
  const write = writer(instId, s.id);
  const common = { id, label: s.label, detail: s.detail || undefined, modified: s.modified, onReset: () => onReset([s.id]) };
  switch (s.kind) {
    case 'number':
      return <NumberField {...common} min={s.min} max={s.max} step={s.step} value={s.value} view={viewFor(s.id, s.step)} write={write} />;
    case 'toggle':
      return <ToggleField {...common} value={s.value === 'on'} write={on => write(on ? 'on' : 'off')} />;
    case 'color':
      return <ColorField {...common} value={s.value} write={write} />;
    case 'colors':
      return <GradientField {...common} value={s.value} write={write} />;
    case 'font':
      return <FontField {...common} value={s.value} write={write} />;
    case 'date':
      return <DateField {...common} value={s.value} write={write} />;
    default:
      return <ChoiceField {...common} options={s.options} value={s.value} write={write} />;
  }
}
