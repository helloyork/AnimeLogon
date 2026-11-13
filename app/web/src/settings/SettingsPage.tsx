// 设置: what every theme shares. Settings come in groups, each group one card whose rows are its
// settings: the name and a line about it on the left, the control on the right. A setting's
// sub-settings follow it in the same card, indented.
import { useId, useState, type ReactNode } from 'react';
import { UacShield } from '../icons';
import { send } from '../native';
import { global, perMonitor, themeOf, useStore } from '../store';
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

const choices = (pairs: [string, string][]): Choice[] => pairs.map(([value, label]) => ({ value, label }));

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
          <h1 className="t-title">设置</h1>
        </div>

        {/* The switch for the whole program comes first. */}
        <Group>
          <Row title="启用 AnimeLogon" description="关闭后锁屏恢复 Windows 默认，主题和设置保留。">
            {() => (
              <>
                <UacShield />
                <Switch label="启用 AnimeLogon" checked={s.env.on} onChange={v => send('switch', { on: v })} />
              </>
            )}
          </Row>
        </Group>

        <Group title="时钟">
          <Row title="现在显示为">{() => <span className="value num selectable">{s.clock}</span>}</Row>
          <Row title="时间制式" description="「跟随区域格式」与任务栏一致。">
            {() => <Select label="时间制式" value={c.hours} onChange={v => global('clock.hours', v)} options={choices([['auto', '跟随区域格式'], ['12', '12 小时制'], ['24', '24 小时制']])} />}
          </Row>
          <Row sub dim={c.hours === '24'} title="显示上午/下午" description={c.hours === '24' ? '仅用于 12 小时制。' : undefined}>
            {() => <Switch label="显示上午/下午" checked={c.ampm} disabled={c.hours === '24'} onChange={v => global('clock.ampm', v)} />}
          </Row>
          <Row title="日期">
            {() => <Select label="日期" value={c.date} onChange={v => global('clock.date', v)} options={choices([['weekday', '星期与月日'], ['long', '完整日期'], ['none', '不显示']])} />}
          </Row>
          <Row title="语言" description="时间和日期使用的语言。">
            {() => (
              <Select label="语言" value={c.language} onChange={v => global('clock.language', v)}
                options={choices([['', '跟随区域格式'], ['zh-CN', '简体中文'], ['zh-TW', '繁體中文'], ['ja-JP', '日本語'], ['ko-KR', '한국어'], ['en-US', 'English']])} />
            )}
          </Row>
        </Group>

        <Group title="组件">
          <Row title="显示组件" description="关闭后所有主题只显示壁纸。">
            {() => <Switch label="显示组件" checked={set.components} onChange={v => global('components', v)} />}
          </Row>
          <Row title="显示正在播放" description="只在你自己锁屏时显示歌名和歌手，别的账户和开机登录界面上看不到。">
            {() => <Switch label="显示正在播放" checked={set.media} onChange={v => global('media', v)} />}
          </Row>
        </Group>

        <Group title="屏幕">
          <Row title="多显示器" dim={!several} description={`这台电脑有 ${s.env.monitors.length} 块显示器。`}>
            {() => (
              <Select label="多显示器" value={set.monitorMode} disabled={!several} onChange={v => global('monitorMode', v)}
                options={choices([['duplicate', '每块屏幕相同'], ['span', '横跨所有屏幕'], ['per-monitor', '每块屏幕各自']])} />
            )}
          </Row>
          {perMonitor() && s.env.monitors.map(m => (
            <Row key={m.key} sub title={m.name + (m.primary ? '（主）' : '')} description={`${m.width}×${m.height}`}>
              {() => (
                <Select label={m.name} value={set.screens[m.key] || ''} onChange={v => global('screen.' + m.key, v)}
                  options={[{ value: '', label: `跟随当前主题（${themeOf(s.lib!.current)?.name ?? ''}）` }, ...s.lib!.themes.map(t => ({ value: t.id, label: t.name }))]} />
              )}
            </Row>
          ))}
          {/* Only a choice where there is more than one display to choose from. */}
          {several && (
            <Row title="组件显示在" description="壁纸显示在每块屏幕上，组件只显示在这里。">
              {() => (
                <Select label="组件显示在" value={set.componentDisplays} onChange={v => global('componentDisplays', v)}
                  options={choices([['auto', '密码框所在的屏幕'], ['primary', '主屏幕'], ['all', '所有屏幕']])} />
              )}
            </Row>
          )}
          {several && (
            <Row title="唤醒后其他屏幕">
              {() => (
                <Select label="唤醒后其他屏幕" value={set.otherDisplays} onChange={v => global('otherDisplays', v)}
                  options={choices([['blur', '模糊的壁纸'], ['picture', '壁纸'], ['black', '黑屏']])} />
              )}
            </Row>
          )}
        </Group>

        <Group title="声音">
          <Row title="播放声音">
            {() => <Switch label="播放声音" checked={set.audio.enabled} onChange={v => global('audio.enabled', v)} />}
          </Row>
          {set.audio.enabled && (
            <>
              <Row sub title="输出设备">
                {() => (
                  <Select label="输出设备" value={set.audio.device} onChange={v => global('audio.device', v)}
                    options={[{ value: '', label: '自动（跟随系统）' }, ...s.env.devices.map(d => ({ value: d.id, label: d.name }))]} />
                )}
              </Row>
              <Row sub title="总音量">{id => <Volume labelledBy={id} value={set.audio.volume} onDone={v => global('audio.volume', v)} />}</Row>
              <Row sub title="视频的声音" description="播放视频壁纸自带的声音。">
            {() => <Switch label="视频的声音" checked={set.audio.video} onChange={v => global('audio.video', v)} />}
              </Row>
              {set.audio.video && (
                <Row sub title="视频声音的音量">{id => <Volume labelledBy={id} value={set.audio.videoVolume} onDone={v => global('audio.videoVolume', v)} />}</Row>
              )}
            </>
          )}
        </Group>

        <Group title="系统">
          <Row title="日志" description="反馈问题时附上。">{() => <Button size="sm" onClick={() => send('logs')}>打开</Button>}</Row>
          <Row title="卸载 AnimeLogon" description="移除本程序并还原所有系统更改。">
            {() => <Button size="sm" shield danger onClick={() => send('uninstall')}>卸载</Button>}
          </Row>
        </Group>

        <Group title="关于">
          <Row title="AnimeLogon" description={`版本 ${s.env.version} · MIT 许可证`} />
        </Group>
      </div>
    </main>
  );
}
