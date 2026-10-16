import { Component, useEffect, type ReactNode } from 'react';
import {
  Button,
  FluentProvider,
  makeStyles,
  ProgressBar,
  Tab,
  TabList,
  Text,
  Toast,
  Toaster,
  ToastTitle,
  tokens,
  useId,
  useToastController,
} from '@fluentui/react-components';
import { Dialogs } from './dialogs/Dialogs';
import { EditorPage } from './editor/EditorPage';
import { UacShield } from './icons';
import { LibraryPage } from './library/LibraryPage';
import { LockPreview } from './lock/LockPreview';
import { send } from './native';
import { SettingsPage } from './settings/SettingsPage';
import { onToast, openDialog, setTab, useStore, type Tab as TabName } from './store';
import { darkTheme, lightTheme, useDark } from './theme';

const useStyles = makeStyles({
  root: {
    height: '100%',
    display: 'flex',
    flexDirection: 'column',
    backgroundColor: tokens.colorNeutralBackground2,
    color: tokens.colorNeutralForeground1,
  },
  bar: {
    display: 'flex',
    alignItems: 'center',
    gap: tokens.spacingHorizontalM,
    minHeight: '48px',
    paddingInline: tokens.spacingHorizontalXXL,
    flex: 'none',
  },
  brand: { marginRight: tokens.spacingHorizontalS },
  spacer: { flex: 1 },
  busy: {
    display: 'flex',
    alignItems: 'center',
    gap: tokens.spacingHorizontalM,
    paddingInline: tokens.spacingHorizontalXXL,
    paddingBlock: tokens.spacingVerticalS,
  },
  busyBar: { maxWidth: '320px' },
});

// Something on the page failed to draw: say so and offer to start the page again, rather than
// leave the window empty.
class Fallback extends Component<{ children: ReactNode }, { failed: string | null }> {
  state = { failed: null as string | null };
  static getDerivedStateFromError(e: unknown) {
    return { failed: String((e as Error)?.message ?? e) };
  }
  render() {
    if (!this.state.failed) return this.props.children;
    return (
      <div style={{ padding: tokens.spacingHorizontalXXL, display: 'flex', flexDirection: 'column', gap: tokens.spacingVerticalM, alignItems: 'flex-start' }}>
        <Text weight="semibold">页面出现问题</Text>
        <Text className="selectable" size={200}>{this.state.failed}</Text>
        <Button appearance="primary" onClick={() => location.reload()}>重新载入</Button>
      </div>
    );
  }
}

export function App() {
  const dark = useDark();
  const s = useStore();
  const styles = useStyles();
  return (
    <FluentProvider theme={dark ? darkTheme : lightTheme}>
      <div className={styles.root}>
        <Fallback>{!s.lib ? null : s.view === 'theme' && s.open ? <EditorPage /> : <LibraryShell />}</Fallback>
      </div>
      <Toasts />
      <Dialogs />
      {s.lock && <LockPreview key={s.lock.theme + '/' + (s.lock.ratio || 0)} theme={s.lock.theme} ratio={s.lock.ratio} />}
    </FluentProvider>
  );
}

// The app bar over the library and the settings: the two pages, and the two things to do from either.
function LibraryShell() {
  const s = useStore();
  const styles = useStyles();
  return (
    <>
      <header className={styles.bar}>
        <Text weight="semibold" className={styles.brand}>AnimeLogon</Text>
        <TabList selectedValue={s.tab} onTabSelect={(_, d) => setTab(d.value as TabName)} aria-label="页面">
          <Tab value="mine">主题库</Tab>
          <Tab value="settings">设置</Tab>
        </TabList>
        <div className={styles.spacer} />
        <Button icon={<UacShield />} onClick={() => send('import')}>导入…</Button>
        <Button appearance="primary" onClick={() => openDialog({ kind: 'newTheme' })}>新建主题</Button>
      </header>
      <BusyBar />
      {s.tab === 'settings' ? <SettingsPage /> : <LibraryPage />}
    </>
  );
}

export function BusyBar() {
  const { busy } = useStore();
  const styles = useStyles();
  if (!busy) return null;
  return (
    <div className={styles.busy} role="status">
      <Text size={200}>{busy.label}</Text>
      <ProgressBar className={styles.busyBar} value={busy.percent / 100} thickness="medium" />
      <Text size={200}>{busy.percent}%</Text>
    </div>
  );
}

function Toasts() {
  const toasterId = useId('toaster');
  const { dispatchToast } = useToastController(toasterId);
  useEffect(
    () =>
      onToast(text =>
        dispatchToast(
          <Toast>
            <ToastTitle>{text}</ToastTitle>
          </Toast>,
          { timeout: 3000 },
        ),
      ),
    [dispatchToast],
  );
  return <Toaster toasterId={toasterId} position="bottom" />;
}
