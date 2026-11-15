// Settings: what every theme shares. Settings come in groups, each group one card whose rows are its
// settings: the name and a line about it on the left, the control on the right. A setting's
// sub-settings follow it in the same card, indented.
import { useEffect, useId, useState, type ReactNode } from 'react';
import { UacShield } from '../icons';
import { LANGUAGES, list, t, tn, type Key } from '../i18n';
import { calm } from '../motion';
import { send } from '../native';
import { DisplaySketch } from '../displays';
import { displayName, global, openLink, perMonitor, placeOf, setLanguage, setState, switchOn, themeOf, useStore } from '../store';
import { Button, cx, Select, Slider, Switch, type Choice } from '../ui/controls';

// The control in a row is named by the row's title. `anchor` is what the icon in the system tray
// may ask to have pointed out (store.ts `reveal`).
// `lead` goes before the title: the sketch of which display a row is about.
function Row({ title, description, children, sub, dim, anchor, lead }: { title: string; description?: string; children?: (labelId: string) => ReactNode; sub?: boolean; dim?: boolean; anchor?: string; lead?: ReactNode }) {
  const id = useId();
  return (
    <div className={cx('row', sub && 'sub', dim && 'dim')} data-anchor={anchor}>
      {lead}
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
// 1 to 10 themes in the icon's menu, in the language shown.
const trayCounts = (): Choice[] => Array.from({ length: 10 }, (_, i) => ({ value: String(i + 1), label: tn('settings.tray.count_n', i + 1) }));

// Scrolls to the row, puts the focus on its control and marks the row for a moment: an outline
// in the accent colour that fades, or that stays and goes at once when animations are off.
// Element.animate leaves the row's classes to React and takes no CSS variables.
function pointOut(what: string) {
  const row = document.querySelector<HTMLElement>(`[data-anchor="${what}"]`);
  if (!row) return;
  row.scrollIntoView({ block: 'center', behavior: calm() ? 'auto' : 'smooth' });
  row.querySelector<HTMLElement>('input, button, select')?.focus({ preventScroll: true });
  const accent = getComputedStyle(document.documentElement).getPropertyValue('--accent').trim() || '#2f6fe4';
  row.animate(
    [
      { boxShadow: `inset 0 0 0 2px ${accent}`, backgroundColor: `color-mix(in srgb, ${accent} 12%, transparent)` },
      { boxShadow: 'inset 0 0 0 2px transparent', backgroundColor: 'transparent' },
    ],
    { duration: 2000, easing: calm() ? 'step-end' : 'cubic-bezier(.33, 0, .67, 1)' },
  );
}

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
  const others = list(s.env.others.filter(o => o.on).map(o => o.name));
  // Pointed out once: coming back to the page later does not do it again.
  const reveal = s.reveal;
  useEffect(() => {
    if (!reveal) return;
    pointOut(reveal.what);
    setState({ reveal: null });
  }, [reveal]);
  return (
    <main className="scroll">
      <div className="page narrow">
        <div className="pagehead">
          <h1 className="t-title">{t('settings.title')}</h1>
        </div>

        {/* The switch for the whole program comes first. */}
        <Group>
          <Row title={t('settings.enable')} description={!s.env.on && others ? t('settings.enable.others', { names: others }) : t('settings.enable.detail')} anchor="switch">
            {() => (
              <>
                <UacShield />
                <Switch label={t('settings.enable')} checked={s.env.on} onChange={v => (v ? switchOn() : send('switch', { on: false }))} />
              </>
            )}
          </Row>
          {/* Both on: one of them has to go. */}
          {s.env.on && others && (
            <Row sub title={t('settings.others_on', { names: others })} description={t('settings.others_on.detail')}>
              {() => <Button size="sm" shield onClick={() => send('switch', { on: true })}>{t('library.turn_off_others', { names: others })}</Button>}
            </Row>
          )}
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
          {/* Each display by its name, where it is beside the main one and its resolution, with a
              sketch of the arrangement that marks it: what tells two of the same model apart. */}
          {perMonitor() && s.env.monitors.map(m => (
            <Row key={m.key} sub lead={<DisplaySketch highlight={m.key} />} title={m.name} description={`${placeOf(m)} · ${m.width}×${m.height}`}>
              {() => (
                <Select label={displayName(m)} value={set.screens[m.key] || ''} onChange={v => global('screen.' + m.key, v)}
                  options={[{ value: '', label: t('settings.screen.follow', { name: themeOf(s.lib!.current)?.name ?? '' }) },
                    ...s.lib!.themes.map(th => ({ value: th.id, label: th.name }))]} />
              )}
            </Row>
          ))}
          {/* What the displays other than the one with the sign-in screen show on the lock screen: the
              wallpaper alone, or a clone that draws the components there too, laid out for each
              display's own size. Only a choice where there is more than one display. settings.ini
              may still say `primary` (an older choice); it shows here as the wallpaper-only one. */}
          {several && (
            <Row title={t('settings.component_displays')} description={t('settings.component_displays.detail')}>
              {() => (
                <Select label={t('settings.component_displays')} value={set.componentDisplays === 'all' ? 'all' : 'auto'} onChange={v => global('componentDisplays', v)}
                  options={choices([['auto', 'settings.component_displays.auto'], ['all', 'settings.component_displays.all']])} />
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

        <Group title={t('settings.tray')}>
          <Row title={t('settings.tray.show')} description={t('settings.tray.show.detail')}>
            {() => <Switch label={t('settings.tray.show')} checked={set.tray} onChange={v => global('tray', v)} />}
          </Row>
          {set.tray && (
            <Row sub title={t('settings.tray.count')}>
              {() => <Select label={t('settings.tray.count')} value={String(set.trayRecent)} onChange={v => global('trayRecent', v)} options={trayCounts()} />}
            </Row>
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
