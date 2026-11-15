// What end-to-end test scripts driving the page through DevTools read and call on it: the state
// as globals and the actions a person would take. Nothing in the page uses these.
import { liftFor, pv } from './preview';
import { send } from './native';
import { applyFlow, apply, deleteFlow, getState, leaveTheme, openDialog, openTheme, select, setState, setTab, type Tab } from './store';

export function exposeForTests() {
  const w = window as unknown as Record<string, unknown>;
  const getters: Record<string, () => unknown> = {
    L: () => getState().lib,
    T: () => getState().open,
    ENV: () => getState().env,
    UI: () => ({ view: getState().view, tab: getState().tab, sel: getState().sel, screen: getState().screen, noVideo: getState().noVideo }),
    PV: () => pv,
  };
  for (const [name, get] of Object.entries(getters)) Object.defineProperty(w, name, { get, configurable: true });
  Object.assign(w, {
    send,
    openTheme,
    leaveTheme: () => leaveTheme(),
    apply,
    applyFlow,
    deleteFlow,
    setTab: (tab: Tab) => setTab(tab),
    selectSel: (inst: string | null, part?: string | null) => select(inst, part ?? null),
    pickWallpaper: () => openDialog({ kind: 'wallpaper' }),
    lockPreview: (theme: string, ratio?: number) => setState({ lock: { theme, ratio } }),
    // The setup wizard, as an install that left switching on to the person shows it: for looking
    // at it in a sandbox, which otherwise stands for an AnimeLogon that is on. The next environment
    // message from the native side puts `on` back. `others` names other programs drawing the logon
    // screen as on, which a sandbox never lists.
    setupWizard: (on = false, others: string[] = []) =>
      setState({ setup: true, env: { ...getState().env, on, setupPending: !on, others: others.map(name => ({ name, on: true })) } }),
    // Whether a drag of this component would move its own pixels (preview.ts).
    liftReady: (inst: string) => !!liftFor(inst),
    // The old page rebuilt itself on render(); this one follows its state, so there is nothing to do.
    render: () => {},
    fitPreview: () => {},
  });
}
