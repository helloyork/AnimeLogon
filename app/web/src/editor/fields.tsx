// One control for one value of the open theme. Sliders and the colour picker show their changes
// live and save when let go; a typed number, a choice or a switch saves at once.
import { useEffect, useMemo, useRef, useState, type ReactNode } from 'react';
import {
  AlphaSlider,
  Button,
  Caption1,
  ColorArea,
  ColorPicker,
  ColorSlider,
  ColorSwatch,
  Combobox,
  Dropdown,
  Input,
  Label,
  makeStyles,
  mergeClasses,
  Option,
  Popover,
  PopoverSurface,
  PopoverTrigger,
  Slider,
  SpinButton,
  Switch,
  SwatchPicker,
  Tooltip,
  tokens,
} from '@fluentui/react-components';
import { ArrowResetRegular } from '@fluentui/react-icons';
import { endGesture, liveSet, newGesture, setValue } from '../preview';
import { useStore } from '../store';
import type { Adjustment, Setting } from '../types';
import { ANCHOR_NAMES, ANCHORS, clamp, decimals, roundTo } from './geometry';

const useStyles = makeStyles({
  field: { display: 'flex', flexDirection: 'column', gap: tokens.spacingVerticalXS, paddingBlock: tokens.spacingVerticalS },
  head: { display: 'flex', alignItems: 'center', gap: tokens.spacingHorizontalXS, minHeight: '24px' },
  grow: { flex: 1, minWidth: 0 },
  value: { color: tokens.colorNeutralForeground3, fontVariantNumeric: 'tabular-nums' },
  line: { display: 'flex', alignItems: 'center', gap: tokens.spacingHorizontalS },
  slider: { flex: 1, minWidth: 0, marginInline: `calc(-1 * ${tokens.spacingHorizontalS})` },
  // Fluent marks every step on the rail; at a tenth of a percent that is a thousand marks.
  rail: { '::before': { backgroundImage: 'none' } },
  label: { cursor: 'default' },
  spin: { width: '92px', flex: 'none' },
  unit: { color: tokens.colorNeutralForeground3, minWidth: '3.2em', fontVariantNumeric: 'tabular-nums' },
  full: { width: '100%' },
  grid: {
    display: 'grid',
    gridTemplateColumns: 'repeat(3, 1fr)',
    gridTemplateRows: 'repeat(3, 1fr)',
    width: '112px',
    height: '72px',
    padding: tokens.spacingHorizontalXS,
    gap: tokens.spacingHorizontalXXS,
    boxSizing: 'border-box',
    backgroundColor: tokens.colorNeutralBackground1,
    border: `${tokens.strokeWidthThin} solid ${tokens.colorNeutralStroke1}`,
    borderRadius: tokens.borderRadiusMedium,
    flex: 'none',
  },
  cell: {
    display: 'grid',
    placeItems: 'center',
    padding: 0,
    border: 'none',
    borderRadius: tokens.borderRadiusSmall,
    backgroundColor: 'transparent',
    cursor: 'pointer',
    ':hover': { backgroundColor: tokens.colorSubtleBackgroundHover },
    ':active': { backgroundColor: tokens.colorSubtleBackgroundPressed },
    '::after': {
      content: '""',
      width: '6px',
      height: '6px',
      borderRadius: '50%',
      backgroundColor: tokens.colorNeutralForeground3,
      transitionProperty: 'width, height, background-color',
      transitionDuration: tokens.durationFaster,
      transitionTimingFunction: tokens.curveDecelerateMax,
    },
  },
  cellOn: { '::after': { width: '10px', height: '10px', backgroundColor: tokens.colorCompoundBrandBackground } },
  colors: { display: 'flex', alignItems: 'center', gap: tokens.spacingHorizontalS, flexWrap: 'wrap' },
  hex: { width: '108px', fontFamily: tokens.fontFamilyMonospace },
  pickerSurface: { display: 'flex', flexDirection: 'column', gap: tokens.spacingVerticalM, width: '260px' },
});

// The label, the button that puts the value back, and what the value reads as.
function Head({ id, label, detail, modified, onReset, value }: { id: string; label: string; detail?: string; modified?: boolean; onReset?: () => void; value?: ReactNode }) {
  const styles = useStyles();
  return (
    <div className={styles.head}>
      <div className={styles.grow}>
        {detail ? (
          <Tooltip content={detail} relationship="description" positioning="before">
            <Label htmlFor={id} className={styles.label}>{label}</Label>
          </Tooltip>
        ) : <Label htmlFor={id}>{label}</Label>}
      </div>
      {value != null && <Caption1 className={styles.value}>{value}</Caption1>}
      {modified && onReset && (
        <Tooltip content="恢复这一项" relationship="label">
          <Button appearance="subtle" size="small" icon={<ArrowResetRegular />} onClick={onReset} />
        </Tooltip>
      )}
    </div>
  );
}

/* --- numbers --- */
const WEIGHT_NAMES: Record<number, string> = { 100: '极细', 200: '特细', 300: '细', 400: '常规', 500: '中等', 600: '半粗', 700: '粗', 800: '特粗', 900: '黑' };

// How a value reads beside its slider: in what units, scaled how, with how many digits.
interface View {
  factor: number;
  digits: number;
  unit: (v: number) => string;
}
export function viewFor(key: string, step: number): View {
  const attr = key.split('.').pop() || '', panel = /^p\d+\.[^.]+$/.test(key);
  const plain = (unit: string, digits: number): View => ({ factor: 1, digits, unit: () => unit });
  switch (attr) {
    case 'offset-x': case 'offset-y': return plain('%', 1);
    case 'size': return panel ? plain('% 屏高', 1) : plain('倍', 2);
    case 'margin': return plain('%', 1);
    case 'weight': return { factor: 1, digits: 0, unit: v => WEIGHT_NAMES[Math.round(v / 100) * 100] || '' };
    case 'tracking': case 'x': case 'y': case 'blur': return plain('em', 3);
    case 'space': case 'gap': return plain('em', 2);
    case 'opacity': return { factor: 100, digits: 0, unit: v => (/\.(s\d+|b)\.opacity$/.test(key) && !v ? '不画' : '%') };
  }
  return plain('', decimals(step));
}

export function NumberField({ id, label, detail, min, max, step, value, modified, view, write, onReset }: {
  id: string;
  label: string;
  detail?: string;
  min: number;
  max: number;
  step: number;
  value: string;
  modified: boolean;
  view: View;
  write: (v: number, gesture?: number) => void;
  onReset: () => void;
}) {
  const styles = useStyles();
  const [live, setLive] = useState<number | null>(null);
  const gesture = useRef(0);
  // What the native side says wins once it has answered.
  useEffect(() => setLive(null), [value]);
  const shown = live ?? +value;
  const end = () => {
    if (!gesture.current) return;
    gesture.current = 0;
    endGesture();
  };
  const f = view.factor, digits = view.digits;
  const display = +(shown * f).toFixed(digits);
  return (
    <div className={styles.field}>
      <Head id={id} label={label} detail={detail} modified={modified} onReset={onReset} />
      <div className={styles.line}>
        <Slider id={id} className={styles.slider} rail={{ className: styles.rail }} min={min} max={max} step={step} value={shown}
          onChange={(_, d) => {
            if (!gesture.current) gesture.current = newGesture();
            setLive(d.value);
            write(d.value, gesture.current);
          }}
          onPointerUp={end} onKeyUp={end} onBlur={end} />
        <SpinButton className={styles.spin} aria-label={label} value={display} min={min * f} max={max * f} step={step * f} precision={digits}
          onChange={(_, d) => {
            const typed = d.value ?? parseFloat(String(d.displayValue ?? '').replace(/[^\d.+\-−]/g, '').replace('−', '-'));
            if (d.value === null && d.displayValue === undefined) return;
            if (!Number.isFinite(typed)) return;
            const v = clamp(roundTo(typed / f, step), min, max);
            setLive(v);
            write(v);
          }} />
        <Caption1 className={styles.unit}>{view.unit(display)}</Caption1>
      </div>
    </div>
  );
}

/* --- choices, switches, dates --- */
export function ChoiceField({ id, label, detail, options, value, modified, write, onReset }: {
  id: string; label: string; detail?: string; options: { value: string; label: string }[]; value: string; modified: boolean; write: (v: string) => void; onReset: () => void;
}) {
  const styles = useStyles();
  const selected = options.find(o => o.value === value);
  return (
    <div className={styles.field}>
      <Head id={id} label={label} detail={detail} modified={modified} onReset={onReset} />
      <Dropdown id={id} className={styles.full} value={selected?.label ?? value} selectedOptions={[value]} onOptionSelect={(_, d) => d.optionValue != null && write(d.optionValue)}>
        {options.map(o => <Option key={o.value} value={o.value}>{o.label}</Option>)}
      </Dropdown>
    </div>
  );
}

export function ToggleField({ id, label, detail, value, modified, write, onReset }: { id: string; label: string; detail?: string; value: boolean; modified: boolean; write: (on: boolean) => void; onReset: () => void }) {
  const styles = useStyles();
  return (
    <div className={mergeClasses(styles.field, styles.line)}>
      <div className={styles.grow}><Head id={id} label={label} detail={detail} modified={modified} onReset={onReset} /></div>
      <Switch id={id} checked={value} onChange={(_, d) => write(d.checked)} />
    </div>
  );
}

export function DateField({ id, label, detail, value, modified, write, onReset }: { id: string; label: string; detail?: string; value: string; modified: boolean; write: (v: string) => void; onReset: () => void }) {
  const styles = useStyles();
  const [text, setText] = useState(value);
  useEffect(() => setText(value), [value]);
  return (
    <div className={styles.field}>
      <Head id={id} label={label} detail={detail} modified={modified} onReset={onReset} />
      <Input id={id} type="date" value={text} min="1601-01-01" max="9999-12-31" onChange={(_, d) => setText(d.value)}
        onBlur={() => (/^\d{4}-\d{2}-\d{2}$/.test(text) ? text !== value && write(text) : setText(value))} />
    </div>
  );
}

/* --- fonts --- */
const fontStack = (stored: string) => (stored ? `"${stored}", ${tokens.fontFamilyBase}` : `"Segoe UI Variable Display", ${tokens.fontFamilyBase}`);
const DEFAULT_FONT = '默认（Segoe UI Variable）';

// Only fonts installed for every user: the logon screen runs as the system and sees no others.
export function FontField({ id, label, detail, value, modified, write, onReset }: { id: string; label: string; detail?: string; value: string; modified: boolean; write: (v: string) => void; onReset: () => void }) {
  const styles = useStyles();
  const { env } = useStore();
  const list = useMemo(() => [{ name: DEFAULT_FONT, stored: '' }, ...env.fonts], [env.fonts]);
  const current = list.find(f => f.stored === value)?.name ?? value;
  const [query, setQuery] = useState<string | null>(null);
  const q = (query ?? '').trim().toLowerCase();
  const shown = (q ? list.filter(f => f.name.toLowerCase().includes(q) || f.stored.toLowerCase().includes(q)) : list).slice(0, 200);
  return (
    <div className={styles.field}>
      <Head id={id} label={label} detail={detail ?? '只列出为所有用户安装的字体。'} modified={modified} onReset={onReset} />
      <Combobox id={id} className={styles.full} value={query ?? current} selectedOptions={[value]} placeholder="搜索字体"
        onChange={e => setQuery(e.target.value)} onOpenChange={(_, d) => !d.open && setQuery(null)}
        onOptionSelect={(_, d) => { setQuery(null); if (d.optionValue != null && d.optionValue !== value) write(d.optionValue); }}>
        {shown.map(f => <Option key={f.stored || '-'} value={f.stored} text={f.name} style={{ fontFamily: fontStack(f.stored) }}>{f.name}</Option>)}
      </Combobox>
    </div>
  );
}

/* --- colours --- */
type HsvColor = { h: number; s: number; v: number; a?: number };  // as ColorPicker takes it
const SWATCHES: [string, string][] = [['白色', '#FFFFFF'], ['暖白', '#FFF1DC'], ['樱粉', '#FFC6D6'], ['天蓝', '#A8D8FF'], ['金色', '#FFD27A'], ['墨色', '#16181F']];

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

export function ColorField({ id, label, detail, value, modified, write, onReset }: { id: string; label: string; detail?: string; value: string; modified: boolean; write: (v: string, gesture?: number) => void; onReset: () => void }) {
  const styles = useStyles();
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
    <div className={styles.field}>
      <Head id={id} label={label} detail={detail} modified={modified} onReset={onReset} />
      <div className={styles.colors}>
        <SwatchPicker aria-label={label} size="small" shape="circular" selectedValue={v0.slice(0, 7)} onSelectionChange={(_, d) => write(d.selectedValue.toUpperCase())}>
          {SWATCHES.map(([name, c]) => <ColorSwatch key={c} value={c} color={c} aria-label={name} />)}
        </SwatchPicker>
        <Popover positioning="below-end" onOpenChange={(_, d) => { if (!d.open && gesture.current) { gesture.current = 0; endGesture(); } }}>
          <PopoverTrigger disableButtonEnhancement>
            <Button size="small">自定义…</Button>
          </PopoverTrigger>
          <PopoverSurface className={styles.pickerSurface} aria-label="自定义颜色">
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
        <Input id={id} className={styles.hex} value={hex} maxLength={9} aria-label={label + '（十六进制）'} onChange={(_, d) => setHex(d.value)}
          onBlur={commitHex} onKeyDown={e => e.key === 'Enter' && commitHex()} />
      </div>
    </div>
  );
}

/* --- the nine anchors --- */
export function AnchorPicker({ value, onPick }: { value: string; onPick: (anchor: string) => void }) {
  const styles = useStyles();
  return (
    <div className={styles.grid} role="radiogroup" aria-label="位置">
      {ANCHORS.map((a, i) => (
        <button key={a} type="button" role="radio" aria-checked={a === value} aria-label={ANCHOR_NAMES[i]} title={ANCHOR_NAMES[i]}
          className={mergeClasses(styles.cell, a === value && styles.cellOn)} onClick={() => onPick(a)} />
      ))}
    </div>
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
      return <NumberField {...common} min={a.min} max={a.max} step={a.step} value={a.value} view={viewFor(a.key, a.step)} write={write} />;
    case 'choice':
      return <ChoiceField {...common} options={a.options} value={a.value} write={write} />;
    case 'color':
      return <ColorField {...common} value={a.value} write={write} />;
    case 'font':
      return <FontField {...common} value={a.value} write={write} />;
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
