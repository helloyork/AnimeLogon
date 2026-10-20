// 设置: what every theme shares, laid out as Windows 11's Settings app does: one card per
// setting, its label and a line about it on the left, its control on the right.
import { useState, type ReactNode } from 'react';
import {
  Body1,
  Button,
  Caption1,
  Dropdown,
  makeStyles,
  mergeClasses,
  Option,
  Slider,
  Subtitle2,
  Switch,
  Text,
  tokens,
} from '@fluentui/react-components';
import { UacShield } from '../icons';
import { send } from '../native';
import { global, perMonitor, themeOf, useStore } from '../store';

const useStyles = makeStyles({
  scroll: { flex: 1, minHeight: 0, overflowY: 'auto' },
  page: {
    display: 'flex',
    flexDirection: 'column',
    gap: tokens.spacingVerticalXXL,
    paddingInline: tokens.spacingHorizontalXXL,
    paddingTop: tokens.spacingVerticalS,
    paddingBottom: tokens.spacingVerticalXXXL,
    maxWidth: '1000px',
  },
  group: { display: 'flex', flexDirection: 'column', gap: tokens.spacingVerticalXXS },
  heading: { marginBottom: tokens.spacingVerticalS },
  row: {
    display: 'flex',
    alignItems: 'center',
    justifyContent: 'space-between',
    flexWrap: 'wrap',
    gap: `${tokens.spacingVerticalS} ${tokens.spacingHorizontalL}`,
    minHeight: '64px',
    boxSizing: 'border-box',
    paddingBlock: tokens.spacingVerticalM,
    paddingInline: tokens.spacingHorizontalL,
    backgroundColor: tokens.colorNeutralBackground1,
    border: `${tokens.strokeWidthThin} solid ${tokens.colorNeutralStroke2}`,
    borderRadius: tokens.borderRadiusMedium,
  },
  sub: { paddingLeft: '48px' },
  text: { display: 'flex', flexDirection: 'column', minWidth: 0, flex: '1 1 240px' },
  quiet: { color: tokens.colorNeutralForeground3 },
  control: { display: 'flex', alignItems: 'center', gap: tokens.spacingHorizontalS },
  dropdown: { minWidth: '220px' },
  range: { display: 'flex', alignItems: 'center', gap: tokens.spacingHorizontalS, width: 'min(100%, 280px)' },
  value: { minWidth: '3em', textAlign: 'right', fontVariantNumeric: 'tabular-nums' },
  rail: { '::before': { backgroundImage: 'none' } },
});

function Row({ title, description, children, sub }: { title: string; description?: string; children?: ReactNode; sub?: boolean }) {
  const styles = useStyles();
  return (
    <div className={mergeClasses(styles.row, sub && styles.sub)}>
      <div className={styles.text}>
        <Body1>{title}</Body1>
        {description && <Caption1 className={styles.quiet}>{description}</Caption1>}
      </div>
      {children && <div className={styles.control}>{children}</div>}
    </div>
  );
}

function Group({ title, children }: { title: string; children: ReactNode }) {
  const styles = useStyles();
  return (
    <section className={styles.group}>
      <Subtitle2 as="h2" className={styles.heading}>{title}</Subtitle2>
      {children}
    </section>
  );
}

// Windows writes 开 / 关 beside a switch.
export function OnOff({ checked, onChange, label }: { checked: boolean; onChange: (on: boolean) => void; label: string }) {
  return <Switch checked={checked} onChange={(_, d) => onChange(d.checked)} label={checked ? '开' : '关'} labelPosition="before" aria-label={label} />;
}

function Choice({ options, value, onChange, label }: { options: [string, string][]; value: string; onChange: (v: string) => void; label: string }) {
  const styles = useStyles();
  const selected = options.find(o => o[0] === value) || options[0];
  return (
    <Dropdown className={styles.dropdown} aria-label={label} value={selected[1]} selectedOptions={[selected[0]]} onOptionSelect={(_, d) => d.optionValue != null && onChange(d.optionValue)}>
      {options.map(([v, l]) => <Option key={v} value={v}>{l}</Option>)}
    </Dropdown>
  );
}

// A volume: moves freely, saved when let go.
function Volume({ value, onDone, label }: { value: number; onDone: (v: number) => void; label: string }) {
  const styles = useStyles();
  const [shown, setShown] = useState<number | null>(null);
  const done = () => {
    if (shown != null && shown !== value) onDone(shown);
    setShown(null);
  };
  return (
    <div className={styles.range}>
      <Slider min={0} max={100} value={shown ?? value} aria-label={label} style={{ flex: 1 }} rail={{ className: styles.rail }}
        onChange={(_, d) => setShown(d.value)} onPointerUp={done} onKeyUp={done} onBlur={done} />
      <Text className={styles.value}>{(shown ?? value) + '%'}</Text>
    </div>
  );
}

export function SettingsPage() {
  const s = useStore();
  const styles = useStyles();
  const set = s.lib!.settings, c = set.clock;
  return (
    <main className={styles.scroll}>
      <div className={styles.page}>
        <Group title="时钟">
          <Row title="现在显示为"><Text>{s.clock}</Text></Row>
          <Row title="时间制式" description="「跟随区域格式」与任务栏一致。">
            <Choice label="时间制式" value={c.hours} onChange={v => global('clock.hours', v)} options={[['auto', '跟随区域格式'], ['12', '12 小时制'], ['24', '24 小时制']]} />
          </Row>
          <Row title="显示上午/下午" description="仅用于 12 小时制。">
            <OnOff label="显示上午/下午" checked={c.ampm} onChange={v => global('clock.ampm', v)} />
          </Row>
          <Row title="日期">
            <Choice label="日期" value={c.date} onChange={v => global('clock.date', v)} options={[['weekday', '星期与月日'], ['long', '完整日期'], ['none', '不显示']]} />
          </Row>
          <Row title="语言" description="时间和日期使用的语言。">
            <Choice label="语言" value={c.language} onChange={v => global('clock.language', v)}
              options={[['', '跟随区域格式'], ['zh-CN', '简体中文'], ['zh-TW', '繁體中文'], ['ja-JP', '日本語'], ['ko-KR', '한국어'], ['en-US', 'English']]} />
          </Row>
        </Group>

        <Group title="组件">
          <Row title="显示组件" description="关闭后所有主题只显示壁纸。">
            <OnOff label="显示组件" checked={set.components} onChange={v => global('components', v)} />
          </Row>
          <Row title="组件显示在" description="壁纸显示在每块屏幕上，组件只显示在这里。">
            <Choice label="组件显示在" value={set.componentDisplays} onChange={v => global('componentDisplays', v)}
              options={[['auto', '密码框所在的屏幕'], ['primary', '主屏幕'], ['all', '所有屏幕']]} />
          </Row>
          <Row title="显示正在播放" description="只在你自己锁屏时显示歌名和歌手，别的账户和开机登录界面上看不到。">
            <OnOff label="显示正在播放" checked={set.media} onChange={v => global('media', v)} />
          </Row>
        </Group>

        <Group title="屏幕">
          <Row title="多显示器" description={`这台电脑有 ${s.env.monitors.length} 块显示器。`}>
            <Choice label="多显示器" value={set.monitorMode} onChange={v => global('monitorMode', v)}
              options={[['duplicate', '每块屏幕相同'], ['span', '横跨所有屏幕'], ['per-monitor', '每块屏幕各自']]} />
          </Row>
          {perMonitor() && s.env.monitors.map(m => (
            <Row key={m.key} sub title={m.name + (m.primary ? '（主）' : '')} description={`${m.width}×${m.height}`}>
              <Choice label={m.name} value={set.screens[m.key] || ''} onChange={v => global('screen.' + m.key, v)}
                options={[['', `跟随当前主题（${themeOf(s.lib!.current)?.name ?? ''}）`], ...s.lib!.themes.map(t => [t.id, t.name] as [string, string])]} />
            </Row>
          ))}
        </Group>

        <Group title="声音">
          <Row title="播放声音" description="在锁屏上播放声音。">
            <OnOff label="播放声音" checked={set.audio.enabled} onChange={v => global('audio.enabled', v)} />
          </Row>
          {set.audio.enabled && (
            <>
              <Row sub title="输出设备">
                <Choice label="输出设备" value={set.audio.device} onChange={v => global('audio.device', v)}
                  options={[['', '自动（跟随系统）'], ...s.env.devices.map(d => [d.id, d.name] as [string, string])]} />
              </Row>
              <Row sub title="总音量"><Volume label="总音量" value={set.audio.volume} onDone={v => global('audio.volume', v)} /></Row>
              <Row sub title="视频的声音" description="播放视频壁纸自带的声音。">
                <OnOff label="视频的声音" checked={set.audio.video} onChange={v => global('audio.video', v)} />
              </Row>
              {set.audio.video && (
                <Row sub title="视频声音的音量"><Volume label="视频声音的音量" value={set.audio.videoVolume} onDone={v => global('audio.videoVolume', v)} /></Row>
              )}
            </>
          )}
        </Group>

        <Group title="系统">
          <Row title="启用 AnimeLogon" description="关闭后锁屏恢复 Windows 默认，主题和设置保留。">
            <UacShield />
            <OnOff label="启用 AnimeLogon" checked={s.env.on} onChange={v => send('switch', { on: v })} />
          </Row>
          <Row title="日志" description="反馈问题时附上。"><Button onClick={() => send('logs')}>打开</Button></Row>
          <Row title="卸载 AnimeLogon" description="移除本程序并还原所有系统更改。">
            <Button icon={<UacShield />} onClick={() => send('uninstall')}>卸载</Button>
          </Row>
          <Row title="关于" description="AnimeLogon 0.1.0 · MIT 许可证" />
        </Group>
      </div>
    </main>
  );
}
