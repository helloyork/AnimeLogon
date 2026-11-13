// The page's own controls, drawn by styles/controls.css on the scale in styles/tokens.css. What
// needs a layer over the page (menus, lists of options, tooltips, dialogs) is Fluent's, restyled
// in styles/fluent.css; everything else is here.
import { useEffect, useRef, useState, type ButtonHTMLAttributes, type CSSProperties, type KeyboardEvent, type ReactNode, type Ref } from 'react';
import { t } from '../i18n';
import { Combobox as FluentCombobox, Dropdown, Option, Tooltip } from '@fluentui/react-components';
import { ArrowCounterclockwise16Regular, ChevronDown12Regular } from '@fluentui/react-icons';
import { UacShield } from '../icons';

export const cx = (...names: (string | false | null | undefined)[]) => names.filter(Boolean).join(' ');

/* --- buttons --- */
export type ButtonProps = ButtonHTMLAttributes<HTMLButtonElement> & {
  variant?: 'secondary' | 'primary' | 'ghost';
  size?: 'md' | 'sm' | 'xs';
  icon?: ReactNode;
  // Windows' shield before the words: this asks for an administrator's approval.
  shield?: boolean;
  danger?: boolean;
  quiet?: boolean;
  ref?: Ref<HTMLButtonElement>;
};

export function Button({ variant = 'secondary', size = 'md', icon, shield, danger, quiet, className, children, type = 'button', ...rest }: ButtonProps) {
  const iconOnly = !children;
  return (
    <button type={type} className={cx('btn', variant !== 'secondary' && variant, size !== 'md' && size, iconOnly && 'icon', danger && 'danger', quiet && 'quiet', className)} {...rest}>
      {shield && <UacShield className="shield" />}
      {icon}
      {children}
    </button>
  );
}

// A button that is only an icon: its name is said as a tooltip and to screen readers.
export function IconButton({ label, tip, ...rest }: Omit<ButtonProps, 'children'> & { label: string; tip?: string }) {
  return (
    <Tooltip content={tip ?? label} relationship="label" withArrow={false}>
      <Button variant="ghost" aria-label={label} {...rest} />
    </Tooltip>
  );
}

// Puts a modified value back as the theme has it.
export function ResetButton({ onClick, label = t('field.reset') }: { onClick: () => void; label?: string }) {
  return <IconButton size="xs" quiet label={label} icon={<ArrowCounterclockwise16Regular />} onClick={onClick} />;
}

/* --- switches --- */
export function Switch({ checked, onChange, label, id, disabled, labelledBy }: {
  checked: boolean;
  onChange: (on: boolean) => void;
  label?: string;
  id?: string;
  disabled?: boolean;
  labelledBy?: string;
}) {
  return (
    <input type="checkbox" role="switch" className="switch" id={id} checked={checked} disabled={disabled} aria-label={label} aria-labelledby={labelledBy}
      onChange={e => onChange(e.currentTarget.checked)} />
  );
}

/* --- sliders --- */
// Moves freely; `onChange` follows the thumb and `onDone` says when it is let go.
export function Slider({ id, label, min, max, step, value, onChange, onDone, disabled }: {
  id?: string;
  label?: string;
  min: number;
  max: number;
  step: number;
  value: number;
  onChange: (v: number) => void;
  onDone?: () => void;
  disabled?: boolean;
}) {
  const f = max > min ? Math.min(1, Math.max(0, (value - min) / (max - min))) : 0;
  return (
    <input type="range" className="slider" id={id} aria-label={label} min={min} max={max} step={step} value={value} disabled={disabled}
      style={{ '--f': String(f) } as CSSProperties}
      onChange={e => onChange(+e.currentTarget.value)} onPointerUp={onDone} onKeyUp={onDone} onBlur={onDone} />
  );
}

/* --- numbers --- */
const roundTo = (v: number, step: number) => {
  const digits = (String(step).split('.')[1] || '').length;
  return +(Math.round(v / step) * step).toFixed(digits);
};

// A number typed or stepped with the arrow keys, kept between min and max; Enter or leaving the
// field writes it, Esc puts back what it was.
export function NumberInput({ id, label, value, min, max, step, digits, unit, onCommit, size = 'sm' }: {
  id?: string;
  label?: string;
  value: number;
  min: number;
  max: number;
  step: number;
  digits: number;
  unit?: string;
  onCommit: (v: number) => void;
  size?: 'md' | 'sm';
}) {
  const [text, setText] = useState<string | null>(null);
  const shown = text ?? value.toFixed(digits);
  const chars = Math.max(min.toFixed(digits).length, max.toFixed(digits).length, 2);
  const write = (v: number) => {
    const next = Math.min(max, Math.max(min, roundTo(v, step)));
    if (next !== value) onCommit(next);
  };
  const commit = () => {
    if (text == null) return;
    const typed = parseFloat(text.replace('−', '-').replace(/[^\d.+-]/g, ''));
    setText(null);
    if (Number.isFinite(typed)) write(typed);
  };
  const key = (e: KeyboardEvent<HTMLInputElement>) => {
    if (e.key === 'Enter') commit();
    else if (e.key === 'Escape') { setText(null); e.currentTarget.blur(); }
    else if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
      e.preventDefault();
      setText(null);
      write(value + (e.key === 'ArrowUp' ? 1 : -1) * step * (e.shiftKey ? 10 : 1));
    }
  };
  return (
    <label className={cx('field', 'num', size === 'sm' && 'sm')}>
      <input id={id} aria-label={label} inputMode="decimal" value={shown} style={{ width: chars + 0.5 + 'ch' }}
        onFocus={e => e.currentTarget.select()} onChange={e => setText(e.currentTarget.value)} onBlur={commit} onKeyDown={key} />
      {unit && <span className="unit">{unit}</span>}
    </label>
  );
}

// A line of text, written when Enter is pressed or the field is left.
export function TextInput({ id, label, value, onCommit, size = 'md', maxLength, mono, autoFocus, onEnter, live }: {
  id?: string;
  label?: string;
  value: string;
  onCommit?: (v: string) => void;
  size?: 'md' | 'sm';
  maxLength?: number;
  mono?: boolean;
  autoFocus?: boolean;
  onEnter?: () => void;
  live?: (v: string) => void;  // every keystroke, for dialogs that hold the value themselves
}) {
  const [text, setText] = useState(value);
  useEffect(() => setText(value), [value]);
  return (
    <label className={cx('field', size === 'sm' && 'sm', mono && 'mono')}>
      <input id={id} aria-label={label} value={text} maxLength={maxLength} autoFocus={autoFocus} spellCheck={false}
        onFocus={e => e.currentTarget.select()}
        onChange={e => { setText(e.currentTarget.value); live?.(e.currentTarget.value); }}
        onBlur={() => onCommit?.(text)}
        onKeyDown={e => { if (e.key === 'Enter') { onCommit?.(text); onEnter?.(); } }} />
    </label>
  );
}

/* --- a few short choices side by side --- */
export function Segmented({ label, options, value, onChange, id }: { label: string; options: { value: string; label: string }[]; value: string; onChange: (v: string) => void; id?: string }) {
  const refs = useRef<(HTMLButtonElement | null)[]>([]);
  const at = Math.max(0, options.findIndex(o => o.value === value));
  const move = (e: KeyboardEvent, d: number) => {
    e.preventDefault();
    const i = (at + d + options.length) % options.length;
    onChange(options[i].value);
    refs.current[i]?.focus();
  };
  return (
    <div className="seg" role="radiogroup" aria-label={label} id={id}>
      {options.map((o, i) => (
        <button key={o.value} ref={el => { refs.current[i] = el; }} type="button" role="radio" aria-checked={i === at} tabIndex={i === at ? 0 : -1}
          onClick={() => onChange(o.value)}
          onKeyDown={e => {
            if (e.key === 'ArrowRight' || e.key === 'ArrowDown') move(e, 1);
            else if (e.key === 'ArrowLeft' || e.key === 'ArrowUp') move(e, -1);
          }}>
          {o.label}
        </button>
      ))}
    </div>
  );
}

/* --- one of a list: Fluent's dropdown, drawn as the page's fields are --- */
export interface Choice {
  value: string;
  label: string;
}

export function Select({ id, label, options, value, onChange, size = 'md', disabled, className }: {
  id?: string;
  label?: string;
  options: Choice[];
  value: string;
  onChange: (v: string) => void;
  size?: 'md' | 'sm';
  disabled?: boolean;
  className?: string;
}) {
  const selected = options.find(o => o.value === value);
  return (
    <Dropdown id={id} aria-label={label} disabled={disabled} className={cx('select', size === 'sm' && 'sm', className)} size={size === 'sm' ? 'small' : 'medium'}
      expandIcon={<ChevronDown12Regular />} listbox={{ className: 'listbox' }} positioning={{ position: 'below', align: 'start', offset: 4 }}
      value={selected?.label ?? value} selectedOptions={[value]} onOptionSelect={(_, d) => d.optionValue != null && d.optionValue !== value && onChange(d.optionValue)}>
      {options.map(o => <Option key={o.value} value={o.value} text={o.label}>{o.label}</Option>)}
    </Dropdown>
  );
}

// A list one can also type into to find an entry (the fonts).
export function SearchSelect({ id, label, options, value, onChange, size = 'sm', render, placeholder }: {
  id?: string;
  label?: string;
  options: Choice[];
  value: string;
  onChange: (v: string) => void;
  size?: 'md' | 'sm';
  render?: (o: Choice) => CSSProperties;
  placeholder?: string;
}) {
  const current = options.find(o => o.value === value)?.label ?? value;
  const [query, setQuery] = useState<string | null>(null);
  const q = (query ?? '').trim().toLowerCase();
  const shown = (q ? options.filter(o => o.label.toLowerCase().includes(q) || o.value.toLowerCase().includes(q)) : options).slice(0, 200);
  return (
    <FluentCombobox id={id} aria-label={label} className={cx('select', size === 'sm' && 'sm')} size={size === 'sm' ? 'small' : 'medium'}
      expandIcon={<ChevronDown12Regular />} listbox={{ className: 'listbox' }} positioning={{ position: 'below', align: 'start', offset: 4 }}
      value={query ?? current} selectedOptions={[value]} placeholder={placeholder}
      onChange={e => setQuery(e.target.value)} onOpenChange={(_, d) => !d.open && setQuery(null)}
      onOptionSelect={(_, d) => { setQuery(null); if (d.optionValue != null && d.optionValue !== value) onChange(d.optionValue); }}>
      {shown.map(o => <Option key={o.value || '-'} value={o.value} text={o.label} style={render?.(o)}>{o.label}</Option>)}
    </FluentCombobox>
  );
}

/* --- progress and breadcrumbs --- */
export function Progress({ value, label }: { value: number; label: string }) {
  return (
    <div className="progress" role="progressbar" aria-label={label} aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round(value)}>
      <i style={{ width: Math.max(0, Math.min(100, value)) + '%' }} />
    </div>
  );
}

// Where one is, each level above it a way back: Themes / 4K, Components / Clock / Time.
export function Crumbs({ label, items, className }: { label: string; items: { label: string; go?: () => void }[]; className?: string }) {
  return (
    <nav className={cx('crumbs', className)} aria-label={label}>
      {items.map((it, i) => (
        <span key={i} style={{ display: 'contents' }}>
          {i > 0 && <span className="sep" aria-hidden="true">/</span>}
          {it.go ? <button type="button" className="crumb" onClick={it.go}>{it.label}</button> : <span className="crumb now" aria-current="page">{it.label}</span>}
        </span>
      ))}
    </nav>
  );
}
