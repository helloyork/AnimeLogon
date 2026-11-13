// A theme's own page (D40), under the library: a line saying which theme it is with the tools
// for it, the lock screen on the left and the panel of adjustments on the right. Changes save
// themselves; undo and redo go back and forth through them.
import { useEffect } from 'react';
import { Menu, MenuItem, MenuList, MenuPopover, MenuTrigger } from '@fluentui/react-components';
import { ArrowLeft16Regular, ArrowRedo16Regular, ArrowUndo16Regular, ChevronDown12Regular, MoreHorizontal16Regular, Play16Regular } from '@fluentui/react-icons';
import { t as tr } from '../i18n';
import { ThemeMenuItems } from '../library/LibraryPage';
import { edit, endGesture, liveSet, newGesture, pendingValues, pv, setValue } from '../preview';
import { applyFlow, getState, goBack, instOf, leaveTheme, multiPanel, panelKey, select, setState, themeOf, usedOn, useStore } from '../store';
import { Button, Crumbs, IconButton } from '../ui/controls';
import { Canvas } from './Canvas';
import { clamp, geometry, offsetLimits, roundTo, shownOffset, valueOf } from './geometry';
import { Inspector } from './Inspector';

const RATIOS = [[16, 9], [16, 10], [21, 9], [32, 9], [4, 3]];

export function EditorPage() {
  const s = useStore();
  const t = s.open!;
  const entry = themeOf(t.id);
  const used = usedOn(t.id);
  useKeys();
  return (
    <div className="themepage">
      <header className="subhead" data-own-keys>
        {/* Back up to the library, as Windows' own pages go back; Alt+Left and the mouse's back button do the same. */}
        <IconButton size="sm" label={tr('nav.back')} tip={tr('nav.back.tip')} icon={<ArrowLeft16Regular />} onClick={goBack} />
        <Crumbs label={tr('theme.page')} items={[{ label: tr('nav.library'), go: () => leaveTheme() }, { label: t.name }]} />
        <div className="tools">
          <IconButton size="sm" label={tr('edit.undo')} tip={tr('edit.undo.tip')} icon={<ArrowUndo16Regular />} disabled={!t.canUndo} onClick={() => edit('undo')} />
          <IconButton size="sm" label={tr('edit.redo')} tip={tr('edit.redo.tip')} icon={<ArrowRedo16Regular />} disabled={!t.canRedo} onClick={() => edit('redo')} />
          <span className="vsep" />
          {/* In use already: said by the button that would apply it, which then does nothing. */}
          {used.length ? <Button size="sm" disabled>{tr('theme.in_use')}</Button>
            : <Button size="sm" variant="primary" onClick={() => applyFlow(t.id)}>{tr('theme.apply')}</Button>}
          {/* The lock screen at this display's shape; the menu shows it at others. */}
          <div className="split">
            <Button size="sm" icon={<Play16Regular />} onClick={() => setState({ lock: { theme: t.id } })}>{tr('theme.lock_preview')}</Button>
            <Menu positioning={{ position: 'below', align: 'end', offset: 4 }}>
              <MenuTrigger disableButtonEnhancement>
                <IconButton size="sm" variant="secondary" label={tr('theme.preview_ratio.label')} icon={<ChevronDown12Regular />} />
              </MenuTrigger>
              <MenuPopover className="menu">
                <MenuList>
                  {RATIOS.map(([a, b]) => (
                    <MenuItem key={a + ':' + b} onClick={() => setState({ lock: { theme: t.id, ratio: a / b } })}>{tr('theme.preview_ratio', { ratio: `${a}:${b}` })}</MenuItem>
                  ))}
                </MenuList>
              </MenuPopover>
            </Menu>
          </div>
          {entry && (
            <Menu positioning={{ position: 'below', align: 'end', offset: 4 }}>
              <MenuTrigger disableButtonEnhancement>
                <IconButton size="sm" label={tr('common.more')} icon={<MoreHorizontal16Regular />} />
              </MenuTrigger>
              <MenuPopover className="menu">
                <ThemeMenuItems theme={entry} onPage />
              </MenuPopover>
            </Menu>
          )}
        </div>
      </header>
      <div className="editor">
        <Canvas />
        <Inspector />
      </div>
    </div>
  );
}

// Keys on the theme's page. Ctrl+Z / Ctrl+Y anywhere but in a text box; Esc, Delete and the arrow
// keys act on the picture, never on a control in the panel or the toolbar.
function useKeys() {
  useEffect(() => {
    const nudge = { at: 0, timer: 0, gesture: 0, ox: undefined as number | undefined, oy: undefined as number | undefined };
    const key = (e: KeyboardEvent) => {
      const s = getState();
      if (s.view !== 'theme' || !s.open || s.dialogs.length || s.lock || document.querySelector('[role="menu"], [role="dialog"], [role="listbox"]')) return;
      const target = e.target as HTMLElement;
      const tag = (target.tagName || '').toLowerCase(), type = (target as HTMLInputElement).type;
      const typing = (tag === 'input' && type !== 'range' && type !== 'color' && type !== 'checkbox') || tag === 'textarea' || target.isContentEditable;
      const mod = e.ctrlKey || e.metaKey, k = e.key.toLowerCase();
      if (mod && k === 'z' && !typing) { e.preventDefault(); edit(e.shiftKey ? 'redo' : 'undo'); return; }
      if (mod && k === 'y' && !typing) { e.preventDefault(); edit('redo'); return; }
      if (typing || type === 'range' || target.closest?.('[data-own-keys], aside')) return;
      const sel = s.sel;
      if (e.key === 'Escape') { if (sel.part && sel.inst !== 'wallpaper') select(sel.inst, null); else select(null); return; }
      const inst = instOf(sel.inst);
      if (!inst) return;
      if (e.key === 'Delete' || e.key === 'Backspace') {
        e.preventDefault();
        if (inst.id === 'wallpaper') { if (sel.part) setValue(inst.id, sel.part + '.visible', 'false'); return; }
        if ('base' in inst && inst.base) edit('visible', { instance: inst.id, on: false });
        else { select(null); edit('removeInstance', { instance: inst.id }); }
        return;
      }
      const step = e.shiftKey ? 1 : 0.1;
      const d = ({ ArrowLeft: [-step, 0], ArrowRight: [step, 0], ArrowUp: [0, -step], ArrowDown: [0, step] } as Record<string, number[]>)[e.key];
      if (!d || (multiPanel(inst.id) && !sel.part)) return;  // several panels: the arrows move one at a time
      e.preventDefault();
      const pk = panelKey(sel), now = performance.now();
      if (now - nudge.at > 700) nudge.gesture = newGesture();
      nudge.at = now;
      // From where the panel is shown, and no further than keeps it on its display.
      const pending = pendingValues(inst.id);
      const full = valueOf(inst.id, pk + '.anchor') === 'full';
      const g = pv.width && !full ? geometry(inst.id, pk, 1, { width: pv.width, height: pv.height }, s.screen) : null;
      const lim = g ? offsetLimits(g) : { x0: -50, x1: 50, y0: -50, y1: 50 }, from = g ? shownOffset(g) : null;
      const ox = clamp(roundTo(+(pending[pk + '.offset-x'] ?? nudge.ox ?? from?.ox ?? valueOf(inst.id, pk + '.offset-x')) + d[0], 0.1), lim.x0, lim.x1);
      const oy = clamp(roundTo(+(pending[pk + '.offset-y'] ?? nudge.oy ?? from?.oy ?? valueOf(inst.id, pk + '.offset-y')) + d[1], 0.1), lim.y0, lim.y1);
      nudge.ox = ox;
      nudge.oy = oy;
      const values: Record<string, string> = { [pk + '.offset-x']: String(ox), [pk + '.offset-y']: String(oy) };
      if (inst.id === 'wallpaper' && valueOf(inst.id, pk + '.anchor') === 'full') values[pk + '.anchor'] = 'center';
      liveSet(inst.id, values, nudge.gesture);
      clearTimeout(nudge.timer);
      nudge.timer = window.setTimeout(() => { nudge.ox = nudge.oy = undefined; endGesture(); }, 600);
    };
    document.addEventListener('keydown', key);
    return () => document.removeEventListener('keydown', key);
  }, []);
}
