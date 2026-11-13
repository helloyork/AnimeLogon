// The setup wizard: all the window shows after an install that left switching AnimeLogon on to the
// person (install.exe --setup-in-app, which Steam's install script runs). Until AnimeLogon is
// switched on here the logon screen stays as Windows has it, so the wizard comes back each time the
// window opens. Its first page says what switching on changes and switches on, with an
// administrator's consent; its last says what was done and leads on to the library.
import { useEffect, useState } from 'react';
import { LANGUAGES, list, t } from '../i18n';
import { send } from '../native';
import { othersOn, setLanguage, setState, useStore } from '../store';
import { Button, Select } from '../ui/controls';

export function SetupPage() {
  const s = useStore();
  // Waiting on the native side: the administrator's consent, then switching on. Whatever comes of
  // it, it answers with the environment again, which ends the wait.
  const [asked, setAsked] = useState(false);
  useEffect(() => setAsked(false), [s.env]);
  // What switching on switched off with it, named on the last page: by then they are off.
  const [stopped, setStopped] = useState('');
  const others = list(othersOn());
  const enable = () => {
    setStopped(others);
    setAsked(true);
    send('switch', { on: true });
  };
  return (
    <main className="scroll">
      <div className="page narrow setup">
        {s.env.on ? <Done stopped={stopped} /> : <Welcome others={others} asked={asked} enable={enable} />}
      </div>
    </main>
  );
}

function Welcome({ others, asked, enable }: { others: string; asked: boolean; enable: () => void }) {
  const s = useStore();
  return (
    <>
      <div className="pagehead">
        <h1 className="t-title">{t('firstrun.title')}</h1>
        {/* As the installer's first page offers it: the language to read the rest in. */}
        <div className="actions">
          <span className="t-cap" aria-hidden="true">{t('settings.language')}</span>
          <Select size="sm" label={t('settings.language')} value={s.env.langChoice} onChange={setLanguage}
            options={[{ value: '', label: t('settings.language.system') }, ...LANGUAGES.map(([value, label]) => ({ value, label }))]} />
        </div>
      </div>
      <p className="setup-intro">{t('setup.welcome.intro')}</p>
      <section className="group">
        <h2 className="t-group">{t('firstrun.changes')}</h2>
        <ul className="card setup-changes">
          <li>{t('setup.welcome.change.lock_screen')}</li>
          <li>{t('setup.welcome.change.spotlight')}</li>
          {others && <li>{t('setup.welcome.change.others', { names: others })}</li>}
          <li>{t('setup.welcome.change.undo')}</li>
        </ul>
      </section>
      <div className="setup-foot">
        <p className="t-cap">{t('firstrun.until')}</p>
        <Button variant="primary" shield disabled={asked} onClick={enable}>{t('settings.enable')}</Button>
      </div>
    </>
  );
}

function Done({ stopped }: { stopped: string }) {
  return (
    <>
      <div className="pagehead">
        <h1 className="t-title">{t('firstrun.done.title')}</h1>
      </div>
      <p className="setup-intro">{t('setup.done.subtitle')}</p>
      <section className="group">
        <div className="card">
          {stopped && (
            <div className="row">
              <div className="lbl">
                <div>{t('setup.done.others', { name: stopped })}</div>
                <div className="t-cap">{t('setup.done.others.detail')}</div>
              </div>
            </div>
          )}
          <div className="row">
            <div className="lbl">
              <div>{t('setup.done.pause.label')}</div>
              <div className="t-cap">{t('setup.done.pause.value')}</div>
            </div>
          </div>
        </div>
      </section>
      <div className="setup-foot">
        <Button variant="primary" autoFocus onClick={() => setState({ setup: false, tab: 'mine', view: 'library' })}>{t('firstrun.done.start')}</Button>
      </div>
    </>
  );
}
