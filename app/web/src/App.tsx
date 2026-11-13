import { Component, useEffect, type ReactNode } from 'react';
import { FluentProvider, Toast, Toaster, ToastTitle, useId, useToastController } from '@fluentui/react-components';
import { Dialogs } from './dialogs/Dialogs';
import { EditorPage } from './editor/EditorPage';
import { LibraryPage } from './library/LibraryPage';
import { LockPreview } from './lock/LockPreview';
import { SettingsPage } from './settings/SettingsPage';
import { onToast, setTab, useStore, type Tab } from './store';
import { darkTheme, lightTheme, useDark } from './theme';
import { Button, cx, Progress } from './ui/controls';

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
      <div className="fallback">
        <h1 className="t-h">页面出现问题</h1>
        <p className="t-cap selectable">{this.state.failed}</p>
        <Button variant="primary" onClick={() => location.reload()}>重新载入</Button>
      </div>
    );
  }
}

export function App() {
  const dark = useDark();
  const s = useStore();
  return (
    <FluentProvider theme={dark ? darkTheme : lightTheme}>
      <div className="app">
        <Fallback>{!s.lib ? null : <Shell />}</Fallback>
      </div>
      <Toasts />
      <Dialogs />
      {s.lock && <LockPreview key={s.lock.theme + '/' + (s.lock.ratio || 0)} theme={s.lock.theme} ratio={s.lock.ratio} />}
    </FluentProvider>
  );
}

const PAGES: [Tab, string][] = [['mine', '主题库'], ['settings', '设置']];

// The app bar stays over every page (D59): the library, a theme's own page under it, and the
// settings. What is being done in the background shows at its end.
function Shell() {
  const s = useStore();
  const themePage = s.tab === 'mine' && s.view === 'theme' && !!s.open;
  return (
    <>
      <header className="topbar">
        <span className="brand">AnimeLogon</span>
        <nav className="nav" aria-label="页面">
          {PAGES.map(([tab, name]) => (
            <button key={tab} type="button" className={cx(s.tab === tab && 'on')} aria-current={s.tab === tab ? 'page' : undefined} onClick={() => setTab(tab)}>{name}</button>
          ))}
        </nav>
        <BusyBar />
      </header>
      {s.tab === 'settings' ? <SettingsPage /> : themePage ? <EditorPage /> : <LibraryPage />}
    </>
  );
}

function BusyBar() {
  const { busy } = useStore();
  if (!busy) return null;
  return (
    <div className="busy" role="status">
      <span className="t-cap ellipsis">{busy.label}</span>
      <Progress value={busy.percent} label={busy.label} />
      <span className="t-cap num">{busy.percent}%</span>
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
          <Toast className="toast">
            <ToastTitle>{text}</ToastTitle>
          </Toast>,
          { timeout: 3000 },
        ),
      ),
    [dispatchToast],
  );
  return <Toaster toasterId={toasterId} position="bottom" offset={{ vertical: 24 }} />;
}
