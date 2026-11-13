// Settings: what every theme shares. Settings come in groups, each group one card whose rows are its
// settings: the name and a line about it on the left, the control on the right. A setting's
// sub-settings follow it in the same card, indented.
import { useId, useState, type ReactNode } from 'react';
import { UacShield } from '../icons';
import { LANGUAGES, t, tn, type Key } from '../i18n';
import { send } from '../native';
import { global, openLink, perMonitor, setLanguage, themeOf, useStore } from '../store';
import { Button, cx, Select, Slider, Switch, type Choice } from '../ui/controls';

// The control in a row is named by the row's title.
function Row({ title, description, children, sub, dim }: { title: string; description?: string; children?: (labelId: string) => ReactNode; sub?: boolean; dim?: boolean }) {
  const id = useId();
  return (
    <div className={cx('row', sub && 'sub', dim && 'dim')}>
      <div className="lbl">
        <div id={id}>{title}</div>
        {description && <div className="t-cap">{description}</div>}
      </div>
      {children && <div className="ctl">{children(id)}</div>}
    </div>
  );
}

function Group({ title, children }: { title?: string; children: ReactNode }) {
  return (
    <section className="group">
      {title && <h2 className="t-group">{title}</h2>}
      <div className="card">{children}</div>
    </section>
  );
}

const choices = (pairs: [string, Key][]): Choice[] => pairs.map(([value, key]) => ({ value, label: t(key) }));

// A volume: moves freely, saved when let go.
function Volume({ value, onDone, labelledBy }: { value: number; onDone: (v: number) => void; labelledBy: string }) {
  const [shown, setShown] = useState<number | null>(null);
  const done = () => {
    if (shown != null && shown !== value) onDone(shown);
    setShown(null);
  };
  return (
    <div className="range" aria-labelledby={labelledBy} role="group">
      <Slider min={0} max={100} step={1} value={shown ?? value} onChange={setShown} onDone={done} />
      <span className="value num">{shown ?? value}</span>
    </div>
  );
}

export function SettingsPage() {
  const s = useStore();
  const set = s.lib!.settings, c = set.clock;
  const several = s.env.monitors.length > 1;
  return (
    <main className="scroll">
      <div className="page narrow">
        <div className="pagehead">
          <h1 className="t-title">{t('settings.title')}</h1>
        </div>

        {/* The switch for the whole program comes first. */}
        <Group>
          <Row title={t('settings.enable')} description={t('settings.enable.detail')}>
            {() => (
              <>
                <UacShield />
                <Switch label={t('settings.enable')} checked={s.env.on} onChange={v => send('switch', { on: v })} />
              </>
            )}
          </Row>
        </Group>

        <Group title={t('settings.general')}>
          {/* Each language in its own name, so that it can be found from any of the others. */}
          <Row title={t('settings.language')}>
            {() => (
              <Select label={t('settings.language')} value={s.env.langChoice} onChange={setLanguage}
                options={[{ value: '', label: t('settings.language.system') }, ...LANGUAGES.map(([value, label]) => ({ value, label }))]} />
            )}
          </Row>
        </Group>

        <Group title={t('settings.clock')}>
          <Row title={t('settings.clock.now')}>{() => <span className="value num selectable">{s.clock}</span>}</Row>
          <Row title={t('settings.clock.hours')} description={t('settings.clock.hours.detail')}>
            {() => (
              <Select label={t('settings.clock.hours')} value={c.hours} onChange={v => global('clock.hours', v)}
                options={choices([['auto', 'settings.clock.hours.auto'], ['12', 'settings.clock.hours.12'], ['24', 'settings.clock.hours.24']])} />
            )}
          </Row>
          <Row sub dim={c.hours === '24'} title={t('settings.clock.ampm')} description={c.hours === '24' ? t('settings.clock.ampm.only12') : undefined}>
            {() => <Switch label={t('settings.clock.ampm')} checked={c.ampm} disabled={c.hours === '24'} onChange={v => global('clock.ampm', v)} />}
          </Row>
          <Row title={t('settings.clock.date')}>
            {() => (
              <Select label={t('settings.clock.date')} value={c.date} onChange={v => global('clock.date', v)}
                options={choices([['weekday', 'settings.clock.date.weekday'], ['long', 'settings.clock.date.long'], ['none', 'settings.clock.date.none']])} />
            )}
          </Row>
          <Row title={t('settings.clock.language')} description={t('settings.clock.language.detail')}>
            {() => (
              <Select label={t('settings.clock.language')} value={c.language} onChange={v => global('clock.language', v)}
                options={[{ value: '', label: t('settings.clock.language.auto') },
                  ...[['zh-CN', '简体中文'], ['zh-TW', '繁體中文'], ['ja-JP', '日本語'], ['ko-KR', '한국어'], ['en-US', 'English']].map(([value, label]) => ({ value, label }))]} />
            )}
          </Row>
        </Group>

        <Group title={t('settings.components')}>
          <Row title={t('settings.components.show')} description={t('settings.components.show.detail')}>
            {() => <Switch label={t('settings.components.show')} checked={set.components} onChange={v => global('components', v)} />}
          </Row>
          <Row title={t('settings.media')} description={t('settings.media.detail')}>
            {() => <Switch label={t('settings.media')} checked={set.media} onChange={v => global('media', v)} />}
          </Row>
        </Group>

        <Group title={t('settings.displays')}>
          <Row title={t('settings.monitors')} dim={!several} description={tn('settings.monitors.count', s.env.monitors.length)}>
            {() => (
              <Select label={t('settings.monitors')} value={set.monitorMode} disabled={!several} onChange={v => global('monitorMode', v)}
                options={choices([['duplicate', 'settings.monitors.duplicate'], ['span', 'settings.monitors.span'], ['per-monitor', 'settings.monitors.per']])} />
            )}
          </Row>
          {perMonitor() && s.env.monitors.map(m => (
            <Row key={m.key} sub title={m.primary ? t('monitor.primary', { name: m.name }) : m.name} description={`${m.width}×${m.height}`}>
              {() => (
                <Select label={m.name} value={set.screens[m.key] || ''} onChange={v => global('screen.' + m.key, v)}
                  options={[{ value: '', label: t('settings.screen.follow', { name: themeOf(s.lib!.current)?.name ?? '' }) },
                    ...s.lib!.themes.map(th => ({ value: th.id, label: th.name }))]} />
              )}
            </Row>
          ))}
          {/* Only a choice where there is more than one display to choose from. */}
          {several && (
            <Row title={t('settings.component_displays')} description={t('settings.component_displays.detail')}>
              {() => (
                <Select label={t('settings.component_displays')} value={set.componentDisplays} onChange={v => global('componentDisplays', v)}
                  options={choices([['auto', 'settings.component_displays.auto'], ['primary', 'settings.component_displays.primary'], ['all', 'settings.component_displays.all']])} />
              )}
            </Row>
          )}
          {several && (
            <Row title={t('settings.other_displays')} description={t('settings.other_displays.detail')}>
              {() => (
                <Select label={t('settings.other_displays')} value={set.otherDisplays} onChange={v => global('otherDisplays', v)}
                  options={choices([['blur', 'settings.other_displays.blur'], ['picture', 'settings.other_displays.picture'], ['black', 'settings.other_displays.black']])} />
              )}
            </Row>
          )}
        </Group>

        <Group title={t('settings.sound')}>
          <Row title={t('settings.sound.play')}>
            {() => <Switch label={t('settings.sound.play')} checked={set.audio.enabled} onChange={v => global('audio.enabled', v)} />}
          </Row>
          {set.audio.enabled && (
            <>
              <Row sub title={t('settings.sound.device')}>
                {() => (
                  <Select label={t('settings.sound.device')} value={set.audio.device} onChange={v => global('audio.device', v)}
                    options={[{ value: '', label: t('settings.sound.device.auto') }, ...s.env.devices.map(d => ({ value: d.id, label: d.name }))]} />
                )}
              </Row>
              <Row sub title={t('settings.sound.volume')}>{id => <Volume labelledBy={id} value={set.audio.volume} onDone={v => global('audio.volume', v)} />}</Row>
              <Row sub title={t('settings.sound.video')} description={t('settings.sound.video.detail')}>
                {() => <Switch label={t('settings.sound.video')} checked={set.audio.video} onChange={v => global('audio.video', v)} />}
              </Row>
              {set.audio.video && (
                <Row sub title={t('settings.sound.video_volume')}>{id => <Volume labelledBy={id} value={set.audio.videoVolume} onDone={v => global('audio.videoVolume', v)} />}</Row>
              )}
            </>
          )}
        </Group>

        <Group title={t('settings.system')}>
          <Row title={t('settings.logs')} description={t('settings.logs.detail')}>{() => <Button size="sm" onClick={() => send('logs')}>{t('settings.logs.open')}</Button>}</Row>
          <Row title={t('settings.uninstall')} description={t('settings.uninstall.detail')}>
            {() => <Button size="sm" shield danger onClick={() => send('uninstall')}>{t('settings.uninstall.button')}</Button>}
          </Row>
        </Group>

        <Group title={t('settings.about')}>
          <Row title="AnimeLogon" description={t('settings.about.version', { version: s.env.version })}>
            {() => <Button size="sm" onClick={() => openLink('releases')}>{t('settings.about.updates')}</Button>}
          </Row>
          {/* Where the project is, where to say what is wrong, and the licences. */}
          <nav className="row links" aria-label={t('settings.about.links')}>
            <button type="button" className="link" onClick={() => openLink('home')}>{t('settings.about.home')}</button>
            <button type="button" className="link" onClick={() => openLink('issues')}>{t('settings.about.issues')}</button>
            <button type="button" className="link" onClick={() => openLink('license')}>{t('settings.about.license')}</button>
            <button type="button" className="link" onClick={() => openLink('notices')}>{t('settings.about.notices')}</button>
          </nav>
        </Group>
      </div>
    </main>
  );
}
