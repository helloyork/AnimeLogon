// Every dialog the page opens, from the store's list: the newest is on top. Fluent's Dialog keeps
// the focus inside, closes on Esc and animates itself in and out; what is in it is the page's own.
import { useEffect, useRef, useState, type ReactNode } from 'react';
import { t as tr, tn } from '../i18n';
import { Dialog, DialogActions, DialogBody, DialogContent, DialogSurface, DialogTitle } from '@fluentui/react-components';
import { FolderOpen16Regular, Video16Regular } from '@fluentui/react-icons';
import { UacShield } from '../icons';
import { send, wallpaperUrl } from '../native';
import { edit } from '../preview';
import { apply, closeDialog, screenTheme, themeOf, useStore, workshopFlow, type Dialog as DialogState } from '../store';
import { Button, cx, TextInput } from '../ui/controls';
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
    case 'workshop':
      return <WorkshopDialog d={d} />;
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
        {s.env.monitors.map(m => (
          <button key={m.key} type="button" className="choice" onClick={() => pick(m.key)}>
            <span className="ellipsis">{m.primary ? tr('monitor.primary', { name: m.name }) : m.name}</span>
            <span className="t-cap3 ellipsis">{`${m.width}×${m.height} · ` + tr('dialog.apply.now', { name: themeOf(screenTheme(m.key))?.name ?? '' })}</span>
          </button>
        ))}
        <button type="button" className="choice" onClick={() => pick('all')}>
          <span>{tr('monitor.all')}</span>
          <span className="t-cap3">{tr('dialog.apply.all.detail')}</span>
        </button>
      </div>
    </Frame>
  );
}

// The wallpapers already here, the chosen one outlined and focused first; the last cells bring in
// a new one from a file or, where there is Wallpaper Engine, from its videos.
function WallpaperGrid({ current, onPick, onImport, importLabel, onWorkshop, focusCurrent }: { current: string; onPick: (id: string) => void; onImport: () => void; importLabel: string; onWorkshop: () => void; focusCurrent?: boolean }) {
  const s = useStore();
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
      <button type="button" className="pick" onClick={onImport}>
        <span className="pic browse"><FolderOpen16Regular /></span>
        <span className="ellipsis shielded"><UacShield />{importLabel}</span>
        <span className="t-cap3 ellipsis">{tr('dialog.wallpaper.file.detail')}</span>
      </button>
      {s.env.wallpaperEngine && (
        <button type="button" className="pick" onClick={onWorkshop}>
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
          <WallpaperGrid current={wallpaper} onPick={setWallpaper} importLabel={tr('dialog.new.import')} onImport={() => { closeDialog(d.id); send('import'); }}
            onWorkshop={() => { closeDialog(d.id); workshopFlow(false); }} />
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
        onImport={() => { closeDialog(d.id); send('importWallpaper'); }} onWorkshop={() => { closeDialog(d.id); workshopFlow(true); }} />
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
          <button type="button" className="choice" onClick={() => { closeDialog(d.id); send('importComponent', { add: true }); }}>
            <span>{tr('dialog.import_file')}</span>
            <span className="t-cap3">{tr('dialog.add.xml')}</span>
          </button>
        </div>
        <p className="t-cap3">{tn('dialog.add.room', s.open?.room ?? 0)}</p>
      </div>
    </Frame>
  );
}
