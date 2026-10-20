// A theme's own page (D40): a toolbar, the lock screen on the left, the panel of adjustments on
// the right. Changes save themselves; undo and redo go back and forth through them.
import { useEffect } from 'react';
import {
  Button,
  Caption1,
  makeStyles,
  Menu,
  MenuPopover,
  MenuTrigger,
  Subtitle2,
  Toolbar,
  ToolbarButton,
  ToolbarDivider,
  Tooltip,
  tokens,
} from '@fluentui/react-components';
import { ArrowLeftRegular, ArrowRedoRegular, ArrowUndoRegular, MoreHorizontalRegular } from '@fluentui/react-icons';
import { BusyBar } from '../App';
import { ThemeMenuItems } from '../library/LibraryPage';
import { edit, endGesture, liveSet, newGesture, pendingValues, setValue } from '../preview';
import { applyFlow, getState, instOf, leaveTheme, multiPanel, panelKey, select, setState, themeOf, usedOn, useStore } from '../store';
import { Canvas } from './Canvas';
import { clamp, roundTo, valueOf } from './geometry';
import { Inspector } from './Inspector';

const useStyles = makeStyles({
  page: { flex: 1, minHeight: 0, display: 'flex', flexDirection: 'column' },
  bar: {
    display: 'flex',
    alignItems: 'center',
    gap: tokens.spacingHorizontalS,
    minHeight: '48px',
    paddingInline: tokens.spacingHorizontalM,
    backgroundColor: tokens.colorNeutralBackground1,
    borderBottom: `${tokens.strokeWidthThin} solid ${tokens.colorNeutralStroke2}`,
    flex: 'none',
  },
  name: { overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', minWidth: 0 },
  status: { color: tokens.colorNeutralForeground3, whiteSpace: 'nowrap' },
  spacer: { flex: 1 },
  body: { flex: 1, minHeight: 0, display: 'flex' },
});

export function EditorPage() {
  const s = useStore();
  const styles = useStyles();
  const t = s.open!;
  const entry = themeOf(t.id);
  const used = usedOn(t.id);
  useKeys();
  return (
    <div className={styles.page}>
      <header className={styles.bar} data-own-keys>
        <Tooltip content="返回主题库" relationship="label">
          <Button appearance="subtle" icon={<ArrowLeftRegular />} onClick={() => leaveTheme()} />
        </Tooltip>
        <Subtitle2 as="h1" className={styles.name}>{t.name}</Subtitle2>
        <Caption1 className={styles.status}>{used.length ? (used[0] === '使用中' ? '使用中' : `用于${used.join('、')}`) : t.modifications ? '已修改' : ''}</Caption1>
        <div className={styles.spacer} />
        <Toolbar aria-label="编辑" size="medium">
          <Tooltip content="撤销 (Ctrl+Z)" relationship="label">
            <ToolbarButton icon={<ArrowUndoRegular />} disabled={!t.canUndo} onClick={() => edit('undo')} />
          </Tooltip>
          <Tooltip content="重做 (Ctrl+Y)" relationship="label">
            <ToolbarButton icon={<ArrowRedoRegular />} disabled={!t.canRedo} onClick={() => edit('redo')} />
          </Tooltip>
          <ToolbarDivider />
          <Button onClick={() => setState({ lock: { theme: t.id } })}>锁屏预览</Button>
          {!used.length && <Button appearance="primary" onClick={() => applyFlow(t.id)} style={{ marginLeft: tokens.spacingHorizontalS }}>套用</Button>}
          {entry && (
            <Menu>
              <MenuTrigger disableButtonEnhancement>
                <Tooltip content="更多" relationship="label">
                  <ToolbarButton icon={<MoreHorizontalRegular />} />
                </Tooltip>
              </MenuTrigger>
              <MenuPopover>
                <ThemeMenuItems theme={entry} onPage />
              </MenuPopover>
            </Menu>
          )}
        </Toolbar>
      </header>
      <BusyBar />
      <div className={styles.body}>
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
      const pending = pendingValues(inst.id);
      const ox = clamp(roundTo(+(pending[pk + '.offset-x'] ?? nudge.ox ?? valueOf(inst.id, pk + '.offset-x')) + d[0], 0.1), -50, 50);
      const oy = clamp(roundTo(+(pending[pk + '.offset-y'] ?? nudge.oy ?? valueOf(inst.id, pk + '.offset-y')) + d[1], 0.1), -50, 50);
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
