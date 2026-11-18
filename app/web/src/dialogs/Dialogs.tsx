// Every dialog the page opens, from the store's list: the newest is on top. Fluent's Dialog keeps
// the focus inside, closes on Esc and animates itself in and out; what is in it is the page's own.
import { useEffect, useRef, useState, type ReactNode } from 'react';
import { t as tr, tn } from '../i18n';
import { Dialog, DialogActions, DialogBody, DialogContent, DialogSurface, DialogTitle, useRestoreFocusTarget } from '@fluentui/react-components';
import { Checkmark12Filled, FolderOpen16Regular, Video16Regular } from '@fluentui/react-icons';
import { UacShield } from '../icons';
import { send, thumbUrl, wallpaperUrl } from '../native';
import { edit } from '../preview';
import { DisplaySketch } from '../displays';
import { addToRotation, apply, closeDialog, importFlow, placeOf, screenTheme, themeOf, useStore, workshopFlow, type Dialog as DialogState } from '../store';
import { Button, cx, TextInput } from '../ui/controls';
import { ImportDialog } from './Import';
import { WorkshopDialog } from './Workshop';

export function Dialogs() {
  const { dialogs } = useStore();
  return (
    <>
      {dialogs.map(d => <One key={d.id} d={d} />)}
    </>
  );
}

function Frame({ d, title, children, actions, wide }: { d: DialogState; title: string; children: ReactNode; actions?: ReactNode; wide?: boolean }) {
  return (
    <Dialog open onOpenChange={(_, data) => !data.open && closeDialog(d.id)}>
      <DialogSurface className="dialog" style={{ maxWidth: wide ? '720px' : '440px' }}>
        <DialogBody>
          <DialogTitle>{title}</DialogTitle>
          <DialogContent>{children}</DialogContent>
          {actions && <DialogActions>{actions}</DialogActions>}
        </DialogBody>
      </DialogSurface>
    </Dialog>
  );
}

function One({ d }: { d: DialogState }) {
  const close = () => closeDialog(d.id);
  switch (d.kind) {
    case 'problem':
      return (
        <Frame d={d} title={tr('dialog.problem.title')} actions={<Button variant="primary" onClick={close}>{tr('button.ok')}</Button>}>
          <p className="selectable dlg-lines">{d.text}</p>
        </Frame>
      );
    case 'confirm':
      return (
        <Frame d={d} title={d.title} actions={<>
          <Button onClick={close}>{tr('button.cancel')}</Button>
          <Button variant="primary" shield={d.elevated} onClick={() => { close(); d.run(); }}>{d.action}</Button>
        </>}>
          <div className="dlg-text">
            {d.body.map((l, i) => <p key={i} className={i ? 'quiet' : undefined}>{l}</p>)}
          </div>
        </Frame>
      );
    case 'name':
      return <NameDialog d={d} />;
    case 'monitor':
      return <MonitorDialog d={d} />;
    case 'newTheme':
      return <NewThemeDialog d={d} />;
    case 'wallpaper':
      return <WallpaperDialog d={d} />;
    case 'addComponent':
      return <AddComponentDialog d={d} />;
    case 'rotation':
      return <RotationDialog d={d} />;
    case 'copyComponents':
      return <CopyComponentsDialog d={d} />;
    case 'workshop':
      return <WorkshopDialog d={d} />;
    case 'import':
      return <ImportDialog d={d} />;
  }
}

// A label over its control.
function Labelled({ label, htmlFor, hint, children }: { label: string; htmlFor?: string; hint?: string; children: ReactNode }) {
  return (
    <div className="dlg-field">
      <div className="dlg-label">
        <label htmlFor={htmlFor}>{label}</label>
        {hint && <span className="t-cap3">{hint}</span>}
      </div>
      {children}
    </div>
  );
}

function NameDialog({ d }: { d: Extract<DialogState, { kind: 'name' }> }) {
  const [value, setValue] = useState(d.value);
  const done = () => {
    const v = value.trim();
    if (!v) return;
    closeDialog(d.id);
    d.run(v);
  };
  return (
    <Frame d={d} title={d.title} actions={<>
      <Button onClick={() => closeDialog(d.id)}>{tr('button.cancel')}</Button>
      <Button variant="primary" shield disabled={!value.trim()} onClick={done}>{d.action}</Button>
    </>}>
      <Labelled label={tr('dialog.name')} htmlFor="dlg-name">
        <TextInput id="dlg-name" value={d.value} maxLength={40} autoFocus live={setValue} onEnter={done} />
      </Labelled>
    </Frame>
  );
}

function MonitorDialog({ d }: { d: Extract<DialogState, { kind: 'monitor' }> }) {
  const s = useStore();
  const t = themeOf(d.theme);
  const pick = (monitor: string) => {
    closeDialog(d.id);
    apply(d.theme, monitor);
  };
  return (
    <Frame d={d} title={tr('dialog.apply.title', { name: t?.name ?? '' })} wide actions={<Button onClick={() => closeDialog(d.id)}>{tr('button.cancel')}</Button>}>
      <div className="choices">
        {/* Each display named, placed beside the main one and marked in a sketch of the arrangement. */}
        {s.env.monitors.map(m => (
          <button key={m.key} type="button" className="choice" onClick={() => pick(m.key)}>
            <DisplaySketch highlight={m.key} />
            <span className="ellipsis">{m.name}</span>
            <span className="t-cap3 ellipsis">{`${placeOf(m)} · ${m.width}×${m.height}`}</span>
            <span className="t-cap3 ellipsis">{tr('dialog.apply.now', { name: themeOf(screenTheme(m.key))?.name ?? '' })}</span>
          </button>
        ))}
        <button type="button" className="choice" onClick={() => pick('all')}>
          <DisplaySketch highlight="all" />
          <span>{tr('monitor.all')}</span>
          <span className="t-cap3">{tr('dialog.apply.all.detail')}</span>
        </button>
      </div>
    </Frame>
  );
}

// The wallpapers already here, the chosen one outlined and focused first; the last cells bring in
// a new one. The import cell goes straight to a file dialog and the administrator's approval when
// it carries the `shield`, and otherwise first asks what to import; where there is Wallpaper
// Engine and `onWorkshop` is given, a last cell offers its videos. One import runs at a time: while
// one does (the busy bar is up), the cells that bring in a new one are not offered.
function WallpaperGrid({ current, onPick, onImport, importLabel, importDetail, shield, onWorkshop, focusCurrent }: {
  current: string;
  onPick: (id: string) => void;
  onImport: () => void;
  importLabel: string;
  importDetail: string;
  shield?: boolean;
  onWorkshop?: () => void;
  focusCurrent?: boolean;
}) {
  const s = useStore();
  // Closing what the import cell opened over this dialog puts the focus back on that cell.
  const restoreFocus = useRestoreFocusTarget();
  // The dialog puts the focus on its first control as it opens; where the grid is the choice, it
  // goes to the wallpaper chosen now instead, once the dialog has done so.
  const chosen = useRef<HTMLButtonElement>(null);
  useEffect(() => {
    if (!focusCurrent) return;
    const timer = window.setTimeout(() => chosen.current?.focus(), 60);
    return () => clearTimeout(timer);
  }, [focusCurrent]);
  return (
    <div className="pictures" role="listbox" aria-label={tr('inspector.wallpaper')}>
      {s.lib!.wallpapers.map(w => (
        <button key={w.id} ref={current === w.id ? chosen : undefined} type="button" className={cx('pick', current === w.id && 'on')} role="option" aria-selected={current === w.id} onClick={() => onPick(w.id)}>
          <img className="pic" src={wallpaperUrl(w.id)} alt="" draggable={false} />
          <span className="ellipsis">{w.name}</span>
          <span className="t-cap3 ellipsis">{w.meta}</span>
        </button>
      ))}
      <button type="button" className="pick" {...restoreFocus} disabled={!!s.busy} onClick={onImport}>
        <span className="pic browse"><FolderOpen16Regular /></span>
        <span className="ellipsis shielded">{shield && <UacShield />}{importLabel}</span>
        <span className="t-cap3 ellipsis">{importDetail}</span>
      </button>
      {onWorkshop && s.env.wallpaperEngine && (
        <button type="button" className="pick" disabled={!!s.busy} onClick={onWorkshop}>
          <span className="pic browse"><Video16Regular /></span>
          <span className="ellipsis">Wallpaper Engine…</span>
          <span className="t-cap3 ellipsis">{tr('dialog.wallpaper.we.detail')}</span>
        </button>
      )}
    </div>
  );
}

function NewThemeDialog({ d }: { d: Extract<DialogState, { kind: 'newTheme' }> }) {
  const [name, setName] = useState(() => tr('dialog.new.default_name'));
  const [wallpaper, setWallpaper] = useState('default');
  const create = () => {
    closeDialog(d.id);
    send('newTheme', { name: name.trim() || tr('dialog.new.default_name'), wallpaper });
  };
  return (
    <Frame d={d} title={tr('dialog.new.title')} wide actions={<>
      <Button onClick={() => closeDialog(d.id)}>{tr('button.cancel')}</Button>
      <Button variant="primary" shield onClick={create}>{tr('dialog.new.action')}</Button>
    </>}>
      <div className="dlg-stack">
        <Labelled label={tr('dialog.name')} htmlFor="dlg-new-name">
          <TextInput id="dlg-new-name" value={tr('dialog.new.default_name')} maxLength={40} autoFocus live={setName} />
        </Labelled>
        <Labelled label={tr('inspector.wallpaper')} hint={tr('dialog.new.hint')}>
          {/* What to import is asked as on the library's Import, Wallpaper Engine included; this
              dialog stays under that question until a kind is chosen. */}
          <WallpaperGrid current={wallpaper} onPick={setWallpaper} importLabel={tr('dialog.new.import')} importDetail={tr('dialog.new.import.detail')}
            onImport={() => importFlow(d.id)} />
        </Labelled>
      </div>
    </Frame>
  );
}

function WallpaperDialog({ d }: { d: Extract<DialogState, { kind: 'wallpaper' }> }) {
  const s = useStore();
  return (
    <Frame d={d} title={tr('dialog.wallpaper.title')} wide actions={<Button onClick={() => closeDialog(d.id)}>{tr('button.cancel')}</Button>}>
      <WallpaperGrid current={s.open?.wallpaper.ref ?? ''} focusCurrent onPick={ref => { closeDialog(d.id); edit('wallpaper', { ref }); }} importLabel={tr('dialog.import_file')}
        importDetail={tr('dialog.wallpaper.file.detail')} shield onImport={() => { closeDialog(d.id); send('importWallpaper'); }}
        onWorkshop={() => { closeDialog(d.id); workshopFlow(true); }} />
    </Frame>
  );
}

function AddComponentDialog({ d }: { d: Extract<DialogState, { kind: 'addComponent' }> }) {
  const s = useStore();
  const add = (component: string) => {
    closeDialog(d.id);
    edit('add', { component });
  };
  return (
    <Frame d={d} title={tr('inspector.add')} wide actions={<Button onClick={() => closeDialog(d.id)}>{tr('button.cancel')}</Button>}>
      <div className="dlg-stack">
        <div className="choices">
          {s.lib!.components.map(c => (
            <button key={c.id} type="button" className="choice" onClick={() => add(c.id)}>
              <span className="ellipsis">{c.name}</span>
              <span className="t-cap3 ellipsis">{c.builtIn ? tr('dialog.add.builtin') : c.author ? tr('inspector.by', { author: c.author }) : tr('dialog.add.imported')}</span>
            </button>
          ))}
          {/* A file dialog, then the administrator's approval to install the component. */}
          <button type="button" className="choice" disabled={!!s.busy} onClick={() => { closeDialog(d.id); send('importComponent', { add: true }); }}>
            <span className="shielded"><UacShield />{tr('dialog.import_file')}</span>
            <span className="t-cap3">{tr('dialog.add.xml')}</span>
          </button>
        </div>
        <p className="t-cap3">{tn('dialog.add.room', s.open?.room ?? 0)}</p>
      </div>
    </Frame>
  );
}

// Themes added to the end of the rotation, chosen by their pictures, any number at once.
function RotationDialog({ d }: { d: Extract<DialogState, { kind: 'rotation' }> }) {
  const s = useStore();
  const [chosen, setChosen] = useState<string[]>([]);
  const r = s.lib!.settings.rotation;
  const others = s.lib!.themes.filter(t => !r.themes.includes(t.id));
  const picked = chosen.filter(id => others.some(t => t.id === id));
  const all = others.length > 0 && picked.length === others.length;
  const toggle = (id: string) => setChosen(c => (c.includes(id) ? c.filter(x => x !== id) : [...c, id]));
  const run = () => {
    if (!picked.length) return;
    closeDialog(d.id);
    // In the order they were chosen.
    addToRotation(picked);
  };
  return (
    <Frame d={d} title={tr('dialog.rotation.title')} wide actions={<>
      <Button onClick={() => closeDialog(d.id)}>{tr('button.cancel')}</Button>
      <Button variant="primary" disabled={!picked.length} onClick={run}>{tr('dialog.rotation.action')}</Button>
    </>}>
      <div className="dlg-stack">
        <div className="dlg-text">
          <p>{tr(others.length ? 'dialog.rotation.body' : 'dialog.rotation.all_in')}</p>
        </div>
        {others.length > 0 && (
          <div className="pictures" role="listbox" aria-multiselectable="true" aria-label={tr('dialog.rotation.title')}>
            {others.map(t => {
              const on = picked.includes(t.id);
              return (
                <button key={t.id} type="button" className={cx('pick', on && 'on')} role="option" aria-selected={on} onClick={() => toggle(t.id)}>
                  <span className="picbox">
                    <img className="pic" src={thumbUrl(t.id, t.revision, 296, s.tick)} alt="" draggable={false} />
                    <span className="tick" aria-hidden="true">{on && <Checkmark12Filled />}</span>
                  </span>
                  <span className="ellipsis">{t.name}</span>
                  <span className="t-cap3 ellipsis">{t.summary}</span>
                </button>
              );
            })}
          </div>
        )}
        {others.length > 0 && (
          <div className="dlg-foot">
            <span className="t-cap3">{picked.length ? tn('dialog.copy_components.picked', picked.length) : tr('dialog.copy_components.none')}</span>
            <Button size="sm" onClick={() => setChosen(all ? [] : others.map(t => t.id))}>{tr(all ? 'we.pick_none' : 'we.pick_all')}</Button>
          </div>
        )}
      </div>
    </Frame>
  );
}

// The open theme's components, given to other themes chosen by their pictures, any number at once.
// The components the chosen themes had are replaced; their wallpapers stay.
function CopyComponentsDialog({ d }: { d: Extract<DialogState, { kind: 'copyComponents' }> }) {
  const s = useStore();
  const [chosen, setChosen] = useState<string[]>([]);
  const open = s.open;
  const others = s.lib!.themes.filter(t => t.id !== open?.id);
  // A theme removed while the dialog is open no longer counts.
  const picked = chosen.filter(id => others.some(t => t.id === id));
  const all = others.length > 0 && picked.length === others.length;
  const toggle = (id: string) => setChosen(c => (c.includes(id) ? c.filter(x => x !== id) : [...c, id]));
  const run = () => {
    if (!picked.length) return;
    closeDialog(d.id);
    send('copyComponents', { themes: picked });
  };
  if (!open) return null;
  return (
    <Frame d={d} title={tr('dialog.copy_components.title')} wide actions={<>
      <Button onClick={() => closeDialog(d.id)}>{tr('button.cancel')}</Button>
      <Button variant="primary" disabled={!picked.length} onClick={run}>{tr('dialog.copy_components.action')}</Button>
    </>}>
      <div className="dlg-stack">
        <div className="dlg-text">
          <p>{tr('dialog.copy_components.body', { name: open.name })}</p>
          <p className="quiet">{tr('dialog.copy_components.later', { name: open.name })}</p>
        </div>
        <div className="pictures" role="listbox" aria-multiselectable="true" aria-label={tr('dialog.copy_components.title')}>
          {others.map(t => {
            const on = picked.includes(t.id);
            return (
              <button key={t.id} type="button" className={cx('pick', on && 'on')} role="option" aria-selected={on} onClick={() => toggle(t.id)}>
                <span className="picbox">
                  <img className="pic" src={thumbUrl(t.id, t.revision, 296, s.tick)} alt="" draggable={false} />
                  <span className="tick" aria-hidden="true">{on && <Checkmark12Filled />}</span>
                </span>
                <span className="ellipsis">{t.name}</span>
                <span className="t-cap3 ellipsis">{t.summary}</span>
              </button>
            );
          })}
        </div>
        <div className="dlg-foot">
          <span className="t-cap3">{picked.length ? tn('dialog.copy_components.picked', picked.length) : tr('dialog.copy_components.none')}</span>
          <Button size="sm" onClick={() => setChosen(all ? [] : others.map(t => t.id))}>{tr(all ? 'we.pick_none' : 'we.pick_all')}</Button>
        </div>
      </div>
    </Frame>
  );
}
