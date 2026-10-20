// Every dialog the page opens, from the store's list: the newest is on top. Fluent's Dialog
// animates itself in and out.
import { useState, type ReactNode } from 'react';
import {
  Body1,
  Button,
  Caption1,
  Dialog,
  DialogActions,
  DialogBody,
  DialogContent,
  DialogSurface,
  DialogTitle,
  Field,
  Input,
  makeStyles,
  mergeClasses,
  Text,
  tokens,
} from '@fluentui/react-components';
import { UacShield } from '../icons';
import { send, wallpaperUrl } from '../native';
import { edit } from '../preview';
import { apply, closeDialog, screenTheme, themeOf, useStore, type Dialog as DialogState } from '../store';

const useStyles = makeStyles({
  content: { display: 'flex', flexDirection: 'column', gap: tokens.spacingVerticalM },
  quiet: { color: tokens.colorNeutralForeground3 },
  choices: { display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(min(100%, 220px), 1fr))', gap: tokens.spacingHorizontalS },
  choice: {
    display: 'flex',
    flexDirection: 'column',
    alignItems: 'flex-start',
    gap: tokens.spacingVerticalXXS,
    textAlign: 'left',
    padding: `${tokens.spacingVerticalM} ${tokens.spacingHorizontalM}`,
    backgroundColor: tokens.colorNeutralBackground1,
    color: tokens.colorNeutralForeground1,
    border: `${tokens.strokeWidthThin} solid ${tokens.colorNeutralStroke1}`,
    borderRadius: tokens.borderRadiusMedium,
    cursor: 'pointer',
    fontFamily: 'inherit',
    ':hover': { backgroundColor: tokens.colorNeutralBackground1Hover, border: `${tokens.strokeWidthThin} solid ${tokens.colorNeutralStroke1Hover}` },
    ':active': { backgroundColor: tokens.colorNeutralBackground1Pressed },
  },
  pictures: { display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(min(100%, 150px), 1fr))', gap: tokens.spacingHorizontalM },
  pick: {
    display: 'flex',
    flexDirection: 'column',
    gap: tokens.spacingVerticalXS,
    padding: 0,
    border: 'none',
    background: 'none',
    textAlign: 'left',
    cursor: 'pointer',
    color: tokens.colorNeutralForeground1,
    fontFamily: 'inherit',
    minWidth: 0,
  },
  pickShot: {
    width: '100%',
    aspectRatio: '16 / 9',
    objectFit: 'cover',
    display: 'block',
    borderRadius: tokens.borderRadiusMedium,
    outline: `${tokens.strokeWidthThin} solid ${tokens.colorNeutralStroke2}`,
    backgroundColor: '#000',
  },
  picked: { outline: `${tokens.strokeWidthThick} solid ${tokens.colorBrandStroke1}`, outlineOffset: '2px' },
  ellipsis: { overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' },
});

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
      <DialogSurface style={{ maxWidth: wide ? '720px' : '480px' }}>
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
        <Frame d={d} title="出现问题" actions={<Button appearance="primary" onClick={close}>确定</Button>}>
          <Body1 className="selectable">{d.text}</Body1>
        </Frame>
      );
    case 'confirm':
      return (
        <Frame d={d} title={d.title} actions={<>
          <Button onClick={close}>取消</Button>
          <Button appearance="primary" icon={d.elevated ? <UacShield /> : undefined} onClick={() => { close(); d.run(); }}>{d.action}</Button>
        </>}>
          <Paragraphs lines={d.body} />
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
  }
}

function Paragraphs({ lines }: { lines: string[] }) {
  const styles = useStyles();
  return (
    <div className={styles.content}>
      {lines.map((l, i) => <Body1 key={i} className={i ? styles.quiet : undefined}>{l}</Body1>)}
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
      <Button onClick={() => closeDialog(d.id)}>取消</Button>
      <Button appearance="primary" icon={<UacShield />} disabled={!value.trim()} onClick={done}>{d.action}</Button>
    </>}>
      <Field label="名称">
        <Input value={value} maxLength={40} autoFocus onFocus={e => e.currentTarget.select()} onChange={(_, data) => setValue(data.value)} onKeyDown={e => e.key === 'Enter' && done()} />
      </Field>
    </Frame>
  );
}

function MonitorDialog({ d }: { d: Extract<DialogState, { kind: 'monitor' }> }) {
  const s = useStore();
  const styles = useStyles();
  const t = themeOf(d.theme);
  const pick = (monitor: string) => {
    closeDialog(d.id);
    apply(d.theme, monitor);
  };
  return (
    <Frame d={d} title={`套用「${t?.name ?? ''}」`}>
      <div className={styles.choices}>
        {s.env.monitors.map(m => (
          <button key={m.key} className={styles.choice} onClick={() => pick(m.key)}>
            <Text weight="semibold">{m.name + (m.primary ? '（主）' : '')}</Text>
            <Caption1 className={styles.quiet}>{`${m.width}×${m.height} · 现在是「${themeOf(screenTheme(m.key))?.name ?? ''}」`}</Caption1>
          </button>
        ))}
        <button className={styles.choice} onClick={() => pick('all')}>
          <Text weight="semibold">所有屏幕</Text>
          <Caption1 className={styles.quiet}>每块屏幕都用这个主题</Caption1>
        </button>
      </div>
    </Frame>
  );
}

function WallpaperGrid({ current, onPick }: { current: string; onPick: (id: string) => void }) {
  const s = useStore();
  const styles = useStyles();
  return (
    <div className={styles.pictures} role="listbox" aria-label="壁纸">
      {s.lib!.wallpapers.map(w => (
        <button key={w.id} className={styles.pick} role="option" aria-selected={current === w.id} onClick={() => onPick(w.id)}>
          <img className={mergeClasses(styles.pickShot, current === w.id && styles.picked)} src={wallpaperUrl(w.id)} alt="" draggable={false} />
          <Text className={styles.ellipsis}>{w.name}</Text>
          <Caption1 className={mergeClasses(styles.quiet, styles.ellipsis)}>{w.meta}</Caption1>
        </button>
      ))}
    </div>
  );
}

function NewThemeDialog({ d }: { d: Extract<DialogState, { kind: 'newTheme' }> }) {
  const styles = useStyles();
  const [name, setName] = useState('新主题');
  const [wallpaper, setWallpaper] = useState('default');
  return (
    <Frame d={d} title="新建主题" wide actions={<>
      <Button onClick={() => closeDialog(d.id)}>取消</Button>
      <Button appearance="primary" icon={<UacShield />} onClick={() => { closeDialog(d.id); send('newTheme', { name: name.trim() || '新主题', wallpaper }); }}>创建</Button>
    </>}>
      <div className={styles.content}>
        <Field label="名称">
          <Input value={name} maxLength={40} autoFocus onFocus={e => e.currentTarget.select()} onChange={(_, data) => setName(data.value)} />
        </Field>
        <Field label="壁纸" hint="新主题带一个时钟。要用新的图片或视频，请用「导入…」。">
          <WallpaperGrid current={wallpaper} onPick={setWallpaper} />
        </Field>
      </div>
    </Frame>
  );
}

function WallpaperDialog({ d }: { d: Extract<DialogState, { kind: 'wallpaper' }> }) {
  const s = useStore();
  return (
    <Frame d={d} title="更换壁纸" wide actions={<>
      <Button icon={<UacShield />} onClick={() => { closeDialog(d.id); send('importWallpaper'); }}>从文件导入…</Button>
      <Button onClick={() => closeDialog(d.id)}>取消</Button>
    </>}>
      <WallpaperGrid current={s.open?.wallpaper.ref ?? ''} onPick={ref => { closeDialog(d.id); edit('wallpaper', { ref }); }} />
    </Frame>
  );
}

function AddComponentDialog({ d }: { d: Extract<DialogState, { kind: 'addComponent' }> }) {
  const s = useStore();
  const styles = useStyles();
  const add = (component: string) => {
    closeDialog(d.id);
    edit('add', { component });
  };
  return (
    <Frame d={d} title="添加组件" wide actions={<Button onClick={() => closeDialog(d.id)}>取消</Button>}>
      <div className={styles.content}>
        <div className={styles.choices}>
          {s.lib!.components.map(c => (
            <button key={c.id} className={styles.choice} onClick={() => add(c.id)}>
              <Text weight="semibold">{c.name}</Text>
              <Caption1 className={styles.quiet}>{c.builtIn ? '内置组件' : c.author ? `作者 ${c.author}` : '导入的组件'}</Caption1>
            </button>
          ))}
          <button className={styles.choice} onClick={() => { closeDialog(d.id); send('importComponent', { add: true }); }}>
            <Text weight="semibold">从文件导入…</Text>
            <Caption1 className={styles.quiet}>一个 .xml 组件文件</Caption1>
          </button>
        </div>
        <Caption1 className={styles.quiet}>{`这个主题还能添加 ${s.open?.room ?? 0} 个组件。`}</Caption1>
      </div>
    </Frame>
  );
}
