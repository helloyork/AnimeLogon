// The theme library: the theme in use, large, then every theme. A click on a theme puts it in use (D41).
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
  workshopFlow,
} from '../store';
import { list, t as tr } from '../i18n';
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
  const others = list(s.env.others.filter(o => o.on).map(o => o.name));
  return (
    <main className="scroll">
      <div className="page">
        <div className="pagehead">
          <h1 className="t-title">{tr('library.title')}</h1>
          <div className="actions">
            <Button shield onClick={() => send('import')}>{tr('library.import')}</Button>
            {s.env.wallpaperEngine && <Button onClick={() => workshopFlow(false)}>{tr('library.import_we')}</Button>}
            <Button icon={<Add16Regular />} onClick={() => openDialog({ kind: 'newTheme' })}>{tr('library.new')}</Button>
          </div>
        </div>
        {!s.env.installed && (
          <div className="notice" role="status">
            <Warning16Regular />
            <span>{tr('library.not_installed')}</span>
          </div>
        )}
        {s.env.installed && !s.env.on && (
          <div className="notice" role="status">
            <Warning16Regular />
            <span>{others ? tr('library.off_others', { names: others }) : tr('library.off')}</span>
            <Button size="sm" shield onClick={switchOn}>{tr('library.turn_on')}</Button>
          </div>
        )}
        {s.env.installed && s.env.on && others && (
          <div className="notice" role="status">
            <Warning16Regular />
            <span>{tr('library.both_on', { names: others })}</span>
            <Button size="sm" shield onClick={() => send('switch', { on: true })}>{tr('library.turn_off_others', { names: others })}</Button>
          </div>
        )}
        {perMonitor() ? <Screens /> : <Current />}
        <div className="sechead">
          <h2 className="t-h">{tr('library.mine')}</h2>
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
    <section className="hero" aria-label={tr('library.current')}>
      <div className="shot" style={shapeVars(t.id)}>
        <Picture t={t} width={1280} playing />
      </div>
      <div className="info">
        <span className="t-cap3">{tr('library.current')}</span>
        <h2 className="name ellipsis">{t.name}</h2>
        <dl className="facts">
          <dt>{tr('library.fact.wallpaper')}</dt>
          <dd>
            <div className="ellipsis">{t.wallpaper}</div>
            {meta && <div className="t-cap ellipsis num">{meta}</div>}
          </dd>
          <dt>{tr('library.fact.components')}</dt>
          <dd className="ellipsis">{showComponents ? t.summary : tr('library.fact.components_off')}</dd>
          {t.author && !t.builtIn && (
            <>
              <dt>{tr('library.fact.author')}</dt>
              <dd className="ellipsis">{t.author}</dd>
            </>
          )}
        </dl>
        <div className="acts">
          <Button variant="primary" icon={<Edit16Regular />} onClick={() => openTheme(t.id)}>{tr('theme.customize')}</Button>
          <Button icon={<Play16Regular />} onClick={() => setState({ lock: { theme: t.id } })}>{tr('theme.lock_preview')}</Button>
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
    <section aria-label={tr('library.current')}>
      <div className="sechead first">
        <h2 className="t-h">{tr('library.current')}</h2>
      </div>
      <div className="screens">
        {s.env.monitors.map(m => {
          const t = themeOf(screenTheme(m.key));
          if (!t) return null;
          return (
            <figure key={m.key} className="screen">
              <button type="button" className="shot" style={{ '--arw': String(m.width), '--arh': String(m.height) } as CSSProperties}
                aria-label={tr('theme.customize_named', { name: t.name })} onClick={() => openTheme(t.id)}>
                <img src={thumbUrl(t.id, t.revision, 640, s.tick)} alt="" draggable={false} />
              </button>
              <figcaption>
                <span className="ellipsis">{t.name}</span>
                <span className="t-cap3 ellipsis">{(m.primary ? tr('monitor.primary', { name: m.name }) : m.name) + (showOn(m.primary) ? '' : ' · ' + tr('library.no_components'))}</span>
              </figcaption>
            </figure>
          );
        })}
      </div>
    </section>
  );
}

// Copy, Rename and Delete carry the shield of an administrator's action; `hasIcons` keeps that
// column on every item, so all the names start at the same place.
export function ThemeMenuItems({ theme, onPage }: { theme: ThemeEntry; onPage?: boolean }) {
  return (
    <MenuList hasIcons>
      {!onPage && <MenuItem onClick={() => openTheme(theme.id)}>{tr('theme.customize')}</MenuItem>}
      {!onPage && <MenuItem onClick={() => setState({ lock: { theme: theme.id } })}>{tr('theme.lock_preview')}</MenuItem>}
      {!onPage && <MenuDivider />}
      <MenuItem icon={<UacShield />} onClick={() => copyFlow(theme.id)}>{tr('theme.copy')}</MenuItem>
      {!theme.builtIn && <MenuItem icon={<UacShield />} onClick={() => renameFlow(theme.id)}>{tr('theme.rename')}</MenuItem>}
      <MenuItem onClick={() => send('export', { theme: theme.id })}>{tr('theme.export')}</MenuItem>
      {theme.modifications > 0 && <MenuItem onClick={() => resetFlow(theme.id)}>{tr('theme.reset')}</MenuItem>}
      {!theme.builtIn && <MenuDivider />}
      {!theme.builtIn && <MenuItem icon={<UacShield />} onClick={() => deleteFlow(theme.id)}>{tr('theme.delete')}</MenuItem>}
    </MenuList>
  );
}

// A theme: its picture and name are one button that puts it in use; Customize and its menu sit over
// the picture's corner, beside that button rather than inside it.
function ThemeCard({ theme: t }: { theme: ThemeEntry }) {
  const [hover, setHover] = useState(false);
  const [menuOpen, setMenuOpen] = useState(false);
  const used = usedOn(t.id);
  // What it is, in a few words; that it is in use shows in its outline.
  const facts = [kindName(t.wallpaperKind), t.summary, t.author && !t.builtIn ? t.author : null].filter(Boolean).join(' · ');
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
          <Button size="sm" onClick={() => openTheme(t.id)}>{tr('theme.customize')}</Button>
          <Menu open={menuOpen} onOpenChange={(_, d) => setMenuOpen(d.open)} positioning={{ position: 'below', align: 'end', offset: 4 }}>
            <MenuTrigger disableButtonEnhancement>
              <IconButton size="sm" variant="secondary" label={tr('theme.more_named', { name: t.name })} tip={tr('common.more')} icon={<MoreHorizontal16Regular />} />
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
