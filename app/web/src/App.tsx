import { Component, useEffect, type ReactNode } from 'react';
import { FluentProvider, Toast, Toaster, ToastTitle, useId, useToastController } from '@fluentui/react-components';
import { Dialogs } from './dialogs/Dialogs';
import { EditorPage } from './editor/EditorPage';
import { WindowButtons, WindowTitle } from './frame';
import { LibraryPage } from './library/LibraryPage';
import { LockPreview } from './lock/LockPreview';
import { SettingsPage } from './settings/SettingsPage';
import { DiscoverPage } from './steam/DiscoverPage';
import { SetupPage } from './setup/SetupPage';
import { send } from './native';
import { t } from './i18n';
import { goBack, onToast, setTab, useStore, type Tab } from './store';
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
        <h1 className="t-h">{t('app.error.title')}</h1>
        <p className="t-cap selectable">{this.state.failed}</p>
        <Button variant="primary" onClick={() => location.reload()}>{t('app.error.reload')}</Button>
      </div>
    );
  }
}

export function App() {
  const dark = useDark();
  const s = useStore();
  useBack();
  return (
    <FluentProvider theme={dark ? darkTheme : lightTheme}>
      <div className="app">
        <AppBar />
        <Fallback>{s.setup ? <SetupPage /> : !s.lib ? null : <Page />}</Fallback>
      </div>
      <Toasts />
      <Dialogs />
      {s.lock && <LockPreview key={s.lock.theme + '/' + (s.lock.ratio || 0)} theme={s.lock.theme} ratio={s.lock.ratio} />}
    </FluentProvider>
  );
}

// 发现, the Steam Workshop, only in the Steam edition.
const pagesFor = (steam: boolean): [Tab, Parameters<typeof t>[0]][] =>
  [['mine', 'nav.library'], ...(steam ? [['discover', 'nav.discover']] as [Tab, Parameters<typeof t>[0]][] : []), ['settings', 'nav.settings']];

// The app bar stays over every page: the library, a theme's own page under it, and the settings.
// It is also the window's title bar, so it is there from the start and stays when a page fails to
// draw: what is being done in the background, then the window's buttons, end it. The setup wizard
// has no pages to go to.
function AppBar() {
  const s = useStore();
  return (
    <header className="topbar">
      <WindowTitle />
      {!s.setup && (
        <nav className="nav" aria-label={t('nav.label')}>
          {pagesFor(s.env.steam).map(([tab, name]) => (
            <button key={tab} type="button" className={cx(s.tab === tab && 'on')} aria-current={s.tab === tab ? 'page' : undefined} onClick={() => setTab(tab)}>{t(name)}</button>
          ))}
        </nav>
      )}
      <BusyBar />
      <WindowButtons overLock={!!s.lock} />
    </header>
  );
}

// Alt+Left and the mouse's back button go back (store.ts goBack), as in Windows' own apps.
function useBack() {
  useEffect(() => {
    const key = (e: KeyboardEvent) => {
      if ((e.altKey && e.key === 'ArrowLeft') || e.key === 'BrowserBack') {
        e.preventDefault();
        goBack();
      }
    };
    // The back button is button 3; taken on its press too, so that nothing else acts on it.
    const press = (e: MouseEvent) => { if (e.button === 3) e.preventDefault(); };
    const release = (e: MouseEvent) => {
      if (e.button !== 3) return;
      e.preventDefault();
      goBack();
    };
    document.addEventListener('keydown', key);
    document.addEventListener('mousedown', press);
    document.addEventListener('mouseup', release);
    return () => {
      document.removeEventListener('keydown', key);
      document.removeEventListener('mousedown', press);
      document.removeEventListener('mouseup', release);
    };
  }, []);
}

function Page() {
  const s = useStore();
  const themePage = s.tab === 'mine' && s.view === 'theme' && !!s.open;
  if (s.tab === 'discover' && s.env.steam) return <DiscoverPage />;
  return s.tab === 'settings' ? <SettingsPage /> : themePage ? <EditorPage /> : <LibraryPage />;
}

function BusyBar() {
  const { busy } = useStore();
  if (!busy) return null;
  return (
    <div className="busy" role="status">
      <span className="t-cap ellipsis">{busy.label}</span>
      <Progress value={busy.percent} label={busy.label} />
      <span className="t-cap num">{busy.percent}%</span>
      <Button size="sm" onClick={() => send('cancelImport')}>{t('button.cancel')}</Button>
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
