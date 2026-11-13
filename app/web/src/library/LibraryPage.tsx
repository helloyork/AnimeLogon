// 主题库: the theme in use, large, then every theme. A click on a theme puts it in use (D41).
import { useState, type CSSProperties } from 'react';
import { Menu, MenuDivider, MenuItem, MenuList, MenuPopover, MenuTrigger } from '@fluentui/react-components';
import { Add16Regular, Edit16Regular, MoreHorizontal16Regular, Play16Regular, Warning16Regular } from '@fluentui/react-icons';
import { UacShield } from '../icons';
import { SceneFrames, WallpaperVideo } from '../media';
import { layerUrl, send, thumbUrl } from '../native';
import { motion } from '../preview';
import {
  applyFlow,
  copyFlow,
  deleteFlow,
  kindName,
  openDialog,
  openTheme,
  perMonitor,
  renameFlow,
  resetFlow,
  screenTheme,
  setState,
  shapeOf,
  switchOn,
  themeOf,
  usedOn,
  useStore,
} from '../store';
import type { ThemeEntry } from '../types';
import { Button, cx, IconButton } from '../ui/controls';

const shapeVars = (id: string) => {
  const s = shapeOf(id);
  return { '--arw': String(s.width), '--arh': String(s.height) } as CSSProperties;
};

const moving = (t: ThemeEntry) => t.wallpaperKind === 'video' || (t.wallpaperKind === 'scene' && !t.sceneStatic);

export function LibraryPage() {
  const s = useStore();
  const lib = s.lib!;
  const others = s.env.others.filter(o => o.on).map(o => o.name).join('、');
  return (
    <main className="scroll">
      <div className="page">
        <div className="pagehead">
          <h1 className="t-title">主题库</h1>
          <span className="t-cap num">{`${lib.themes.length} 个主题`}</span>
          <div className="actions">
            <Button shield onClick={() => send('import')}>导入…</Button>
            <Button icon={<Add16Regular />} onClick={() => openDialog({ kind: 'newTheme' })}>新建主题</Button>
          </div>
        </div>
        {!s.env.installed && (
          <div className="notice" role="status">
            <Warning16Regular />
            <span>这台电脑上没有安装 AnimeLogon，改动无法保存。</span>
          </div>
        )}
        {s.env.installed && !s.env.on && (
          <div className="notice" role="status">
            <Warning16Regular />
            <span>{others ? `AnimeLogon 已关闭，锁屏由 ${others} 显示。` : 'AnimeLogon 已关闭，锁屏显示的是 Windows 默认画面。'}</span>
            <Button size="sm" shield onClick={switchOn}>启用</Button>
          </div>
        )}
        {s.env.installed && s.env.on && others && (
          <div className="notice" role="status">
            <Warning16Regular />
            <span>{`${others} 也已启用。两者同时启用时会争用登录界面。`}</span>
            <Button size="sm" shield onClick={() => send('switch', { on: true })}>{`停用 ${others}`}</Button>
          </div>
        )}
        {perMonitor() ? <Screens /> : <Current />}
        <div className="sechead">
          <h2 className="t-h">我的主题</h2>
        </div>
        <div className="grid">
          {lib.themes.map(t => <ThemeCard key={t.id} theme={t} />)}
        </div>
      </div>
    </main>
  );
}

// What the shown picture is made of: the still, then over it the wallpaper playing and the
// components drawn alone, when the wallpaper moves and motion is wanted.
function Picture({ t, width, playing }: { t: ThemeEntry; width: number; playing: boolean }) {
  const s = useStore();
  const play = playing && motion() && moving(t);
  return (
    <>
      <img src={thumbUrl(t.id, t.revision, width, s.tick)} alt="" draggable={false} />
      {play && t.wallpaperKind === 'video' && <WallpaperVideo key={t.wallpaperRef} wallpaper={t.wallpaperRef} fit={t.fit} />}
      {play && t.wallpaperKind === 'scene' && <SceneFrames theme={t.id} width={width} moving />}
      {play && s.lib!.settings.components && <img src={layerUrl(t.id, t.revision, width, s.tick)} alt="" draggable={false} />}
    </>
  );
}

function Current() {
  const s = useStore();
  const t = themeOf(s.lib!.current);
  if (!t) return null;
  const meta = s.lib!.wallpapers.find(w => w.id === t.wallpaperRef)?.meta;
  const showComponents = s.lib!.settings.components;
  return (
    <section className="hero" aria-label="当前主题">
      <div className="shot" style={shapeVars(t.id)}>
        <Picture t={t} width={1280} playing />
      </div>
      <div className="info">
        <span className="t-cap3">当前主题</span>
        <h2 className="name ellipsis">{t.name}</h2>
        <dl className="facts">
          <dt>壁纸</dt>
          <dd>
            <div className="ellipsis">{t.wallpaper}</div>
            {meta && <div className="t-cap ellipsis num">{meta}</div>}
          </dd>
          <dt>组件</dt>
          <dd className="ellipsis">{showComponents ? t.summary || '无' : '已在设置里关闭'}</dd>
          {t.modifications > 0 && (
            <>
              <dt>修改</dt>
              <dd className="num">{`改过 ${t.modifications} 项`}</dd>
            </>
          )}
          {t.author && !t.builtIn && (
            <>
              <dt>作者</dt>
              <dd className="ellipsis">{t.author}</dd>
            </>
          )}
        </dl>
        <div className="acts">
          <Button variant="primary" icon={<Edit16Regular />} onClick={() => openTheme(t.id)}>自定义</Button>
          <Button icon={<Play16Regular />} onClick={() => setState({ lock: { theme: t.id } })}>锁屏预览</Button>
        </div>
      </div>
    </section>
  );
}

// A theme for each display: each display's picture, with the theme it shows.
function Screens() {
  const s = useStore();
  const showOn = (primary: boolean) => s.lib!.settings.componentDisplays === 'all' || primary;
  return (
    <section aria-label="当前主题">
      <div className="sechead first">
        <h2 className="t-h">当前主题</h2>
        <span className="t-cap">每块屏幕各自使用主题</span>
      </div>
      <div className="screens">
        {s.env.monitors.map(m => {
          const t = themeOf(screenTheme(m.key));
          if (!t) return null;
          return (
            <figure key={m.key} className="screen">
              <button type="button" className="shot" style={{ '--arw': String(m.width), '--arh': String(m.height) } as CSSProperties}
                aria-label={`自定义「${t.name}」`} onClick={() => openTheme(t.id)}>
                <img src={thumbUrl(t.id, t.revision, 640, s.tick)} alt="" draggable={false} />
              </button>
              <figcaption>
                <span className="ellipsis">{t.name}</span>
                <span className="t-cap3 ellipsis">{m.name + (m.primary ? '（主）' : '') + (showOn(m.primary) ? '' : ' · 不显示组件')}</span>
              </figcaption>
            </figure>
          );
        })}
      </div>
    </section>
  );
}

export function ThemeMenuItems({ theme, onPage }: { theme: ThemeEntry; onPage?: boolean }) {
  return (
    <MenuList>
      {!onPage && <MenuItem onClick={() => openTheme(theme.id)}>自定义</MenuItem>}
      {!onPage && <MenuItem onClick={() => setState({ lock: { theme: theme.id } })}>锁屏预览</MenuItem>}
      {!onPage && <MenuDivider />}
      <MenuItem icon={<UacShield />} onClick={() => copyFlow(theme.id)}>复制…</MenuItem>
      {!theme.builtIn && <MenuItem icon={<UacShield />} onClick={() => renameFlow(theme.id)}>重命名…</MenuItem>}
      <MenuItem onClick={() => send('export', { theme: theme.id })}>导出主题包…</MenuItem>
      {theme.modifications > 0 && <MenuItem onClick={() => resetFlow(theme.id)}>{`恢复默认（${theme.modifications} 项修改）`}</MenuItem>}
      {!theme.builtIn && <MenuDivider />}
      {!theme.builtIn && <MenuItem icon={<UacShield />} onClick={() => deleteFlow(theme.id)}>删除…</MenuItem>}
    </MenuList>
  );
}

// A theme: its picture and name are one button that puts it in use; 自定义 and its menu sit over
// the picture's corner, beside that button rather than inside it.
function ThemeCard({ theme: t }: { theme: ThemeEntry }) {
  const [hover, setHover] = useState(false);
  const [menuOpen, setMenuOpen] = useState(false);
  const used = usedOn(t.id);
  const facts = [...used, kindName(t.wallpaperKind), t.summary, t.modifications ? '已修改' : null, t.author && !t.builtIn ? t.author : null].filter(Boolean).join(' · ');
  return (
    <article className={cx('tcard', used.length > 0 && 'use')} data-theme={t.id} style={shapeVars(t.id)}
      onPointerEnter={() => setHover(true)} onPointerLeave={() => setHover(false)}>
      <button type="button" className="hit" aria-pressed={used.length > 0}
        onClick={() => {
          // The theme in use is already applied; with a theme for each display, choosing it
          // again still asks which display to put it on.
          if (used.length && !perMonitor()) return;
          applyFlow(t.id);
        }}>
        <div className="thumb">
          <Picture t={t} width={480} playing={hover} />
        </div>
        <div className="meta">
          <div className="nm ellipsis">{t.name}</div>
          <div className="t-cap3 ellipsis">{facts}</div>
        </div>
      </button>
      <div className="over">
        <div className={cx('hov', menuOpen && 'shown')}>
          <Button size="sm" onClick={() => openTheme(t.id)}>自定义</Button>
          <Menu open={menuOpen} onOpenChange={(_, d) => setMenuOpen(d.open)} positioning={{ position: 'below', align: 'end', offset: 4 }}>
            <MenuTrigger disableButtonEnhancement>
              <IconButton size="sm" variant="secondary" label={`「${t.name}」的更多操作`} tip="更多" icon={<MoreHorizontal16Regular />} />
            </MenuTrigger>
            <MenuPopover className="menu">
              <ThemeMenuItems theme={t} />
            </MenuPopover>
          </Menu>
        </div>
      </div>
    </article>
  );
}
