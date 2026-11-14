// One control for one value of the open theme, as a row of the panel's grid: the label, the
// control, and room at the end for the button that puts a changed value back. Sliders and the
// colour picker show their changes live and save when let go; a typed number, a choice or a
// switch saves at once.
import { useEffect, useMemo, useRef, useState, type ReactNode } from 'react';
import { AlphaSlider, ColorArea, ColorPicker, ColorSlider, Popover, PopoverSurface, PopoverTrigger, Tooltip } from '@fluentui/react-components';
import { endGesture, liveSet, newGesture, setValue } from '../preview';
import { t, type Key } from '../i18n';
import { useStore } from '../store';
import type { Adjustment, Setting } from '../types';
import { cx, NumberInput, ResetButton, SearchSelect, Segmented, Select, Slider, Switch, type Choice } from '../ui/controls';
import { clamp, decimals, roundTo } from './geometry';

interface Common {
  id: string;
  label: string;
  detail?: string;
  modified: boolean;
  onReset: () => void;
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
export function Prop({ id, label, detail, modified = false, onReset, top, children }: Partial<Common> & { label: string; top?: boolean; children: ReactNode }) {
  return (
    <>
      <label className={cx('plabel', top && 'top')} htmlFor={id} id={id ? id + '-label' : undefined}>
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
    case 'opacity': return { factor: 100, digits: 0, unit: v => (/\.(s\d+|b)\.opacity$/.test(key) && !v ? t('unit.none') : '%') };
  }
  return plain('', decimals(step));
}

export function NumberField({ id, label, detail, min, max, step, value, modified, view, write, onReset }: Common & {
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
    <Prop id={id} label={label} detail={detail} modified={modified} onReset={onReset}>
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

/* --- a setting or an adjustment of an instance --- */
export function writer(instId: string, key: string) {
  return (v: string | number, gesture?: number) => (gesture ? liveSet(instId, { [key]: String(v) }, gesture) : setValue(instId, key, v));
}

export function AdjustmentField({ instId, a, onReset }: { instId: string; a: Adjustment; onReset: () => void }) {
  const id = `adj-${instId}-${a.key}`.replace(/\./g, '_');
  const write = writer(instId, a.key);
  const common = { id, label: a.label, detail: a.detail || undefined, modified: a.modified, onReset };
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
    case 'font':
      return <FontField {...common} value={a.value} write={write} />;
    case 'toggle':
      return <ToggleField {...common} value={a.value === 'true'} write={on => write(on ? 'true' : 'false')} />;
  }
}

export function SettingField({ instId, setting: s, onReset }: { instId: string; setting: Setting; onReset: () => void }) {
  const id = `set-${instId}-${s.id}`;
  const write = writer(instId, s.id);
  const common = { id, label: s.label, detail: s.detail || undefined, modified: s.modified, onReset };
  switch (s.kind) {
    case 'number':
      return <NumberField {...common} min={s.min} max={s.max} step={s.step} value={s.value} view={viewFor(s.id, s.step)} write={write} />;
    case 'toggle':
      return <ToggleField {...common} value={s.value === 'on'} write={on => write(on ? 'on' : 'off')} />;
    case 'color':
      return <ColorField {...common} value={s.value} write={write} />;
    case 'font':
      return <FontField {...common} value={s.value} write={write} />;
    case 'date':
      return <DateField {...common} value={s.value} write={write} />;
    default:
      return <ChoiceField {...common} options={s.options} value={s.value} write={write} />;
  }
}
