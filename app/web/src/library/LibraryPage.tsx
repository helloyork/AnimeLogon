// The theme library: the theme in use, large, then every theme. A click on a theme puts it in use.
import { useEffect, useState, type CSSProperties, type ReactNode } from 'react';
import { Menu, MenuDivider, MenuItem, MenuList, MenuPopover, MenuTrigger, useRestoreFocusTarget } from '@fluentui/react-components';
import { Add16Regular, Dismiss12Regular, Edit16Regular, MoreHorizontal16Regular, Play16Regular, Warning16Regular } from '@fluentui/react-icons';
import { UacShield } from '../icons';
import { SceneFrames, WallpaperVideo } from '../media';
import { layerUrl, send, thumbUrl } from '../native';
import { motion } from '../preview';
import {
  addToRotation,
  applyFlow,
  copyFlow,
  deleteFlow,
  displayName,
  global,
  importFlow,
  inRotation,
  kindName,
  openDialog,
  openTheme,
  perMonitor,
  placeOf,
  removeFromRotation,
  renameFlow,
  resetFlow,
  screenTheme,
  setRotation,
  setState,
  shapeOf,
  switchOn,
  themeOf,
  usedOn,
  useStore,
} from '../store';
import { list, t as tr, tn } from '../i18n';
import type { Rotation as RotationSettings, ThemeEntry } from '../types';
import { Button, cx, IconButton, Select, Switch, type Choice } from '../ui/controls';
import { setLibraryTab, useSteam } from '../steam/state';
import { WorkshopShelf } from '../steam/WorkshopShelf';

const shapeVars = (id: string) => {
  const s = shapeOf(id);
  return { '--arw': String(s.width), '--arh': String(s.height) } as CSSProperties;
};

const moving = (t: ThemeEntry) => t.wallpaperKind === 'video' || (t.wallpaperKind === 'scene' && !t.sceneStatic);

export function LibraryPage() {
  const s = useStore();
  const restoreFocus = useRestoreFocusTarget();
  const lib = s.lib!;
  const others = list(s.env.others.filter(o => o.on).map(o => o.name));
  return (
    <main className="scroll">
      <div className="page">
        <div className="pagehead">
          <h1 className="t-title">{tr('library.title')}</h1>
          <div className="actions">
            {/* Asks what to import first; the administrator's approval comes after that choice. The
                focus comes back here when the question is closed. One import runs at a time: not
                offered while one does (the busy bar is up). */}
            <Button {...restoreFocus} disabled={!!s.busy} onClick={() => importFlow()}>{tr('library.import')}</Button>
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
        {s.env.installed && s.env.library.away && (
          <div className="notice" role="status">
            <Warning16Regular />
            <span>{tr('library.away', { dir: s.env.library.dir })}</span>
          </div>
        )}
        {perMonitor() ? <Screens /> : <Current />}
        <Rotation />
        {s.env.steam ? <WithWorkshop /> : (
          <>
            <div className="sechead">
              <h2 className="t-h">{tr('library.mine')}</h2>
            </div>
            <div className="grid">
              {lib.themes.map(t => <ThemeCard key={t.id} theme={t} />)}
            </div>
          </>
        )}
      </div>
    </main>
  );
}

// In the Steam edition the themes share the section with the Workshop's items subscribed to:
// two tabs, the themes first.
function WithWorkshop() {
  const s = useStore();
  const steam = useSteam();
  const tabs: ['themes' | 'workshop', string][] = [['themes', tr('library.mine')], ['workshop', tr('library.tab.workshop')]];
  return (
    <>
      <div className="sechead">
        <div className="subtabs" role="tablist" aria-label={tr('library.sections')}>
          {tabs.map(([tab, name]) => (
            <button key={tab} type="button" role="tab" id={'lib-tab-' + tab} aria-selected={steam.libraryTab === tab} aria-controls={'lib-panel-' + tab}
              tabIndex={steam.libraryTab === tab ? 0 : -1} className={cx(steam.libraryTab === tab && 'on')} onClick={() => setLibraryTab(tab)}
              onKeyDown={e => {
                if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight') return;
                e.preventDefault();
                const other = tab === 'themes' ? 'workshop' : 'themes';
                setLibraryTab(other);
                document.getElementById('lib-tab-' + other)?.focus();
              }}>
              {name}
            </button>
          ))}
        </div>
      </div>
      <div role="tabpanel" id={'lib-panel-' + steam.libraryTab} aria-labelledby={'lib-tab-' + steam.libraryTab}>
        {steam.libraryTab === 'workshop' ? <WorkshopShelf /> : (
          <div className="grid">
            {s.lib!.themes.map(t => <ThemeCard key={t.id} theme={t} />)}
          </div>
        )}
      </div>
    </>
  );
}

// What the shown picture is made of: the still, then over it the wallpaper playing and the
// components drawn alone, when the wallpaper moves and motion is wanted.
export function ThemePicture({ t, width, playing }: { t: ThemeEntry; width: number; playing: boolean }) {
  const s = useStore();
  const play = playing && motion() && moving(t);
  return (
    <>
      <img src={thumbUrl(t.id, t.revision, width, s.tick)} alt="" draggable={false} />
      {play && t.wallpaperKind === 'video' && <WallpaperVideo key={t.wallpaperRef} wallpaper={t.wallpaperRef} fit={t.fit} brightness={t.brightness} blur={t.blur} />}
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
  const rotating = s.lib!.settings.rotation.on;
  return (
    <section className="hero" aria-label={tr('library.current')}>
      <div className="shot" style={shapeVars(t.id)}>
        <ThemePicture t={t} width={1280} playing />
      </div>
      <div className="info">
        <span className="t-cap3">{tr(rotating ? 'library.current_rotating' : 'library.current')}</span>
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
                <span className="t-cap3 ellipsis">{displayName(m) + ' · ' + placeOf(m) + (showOn(m.primary) ? '' : ' · ' + tr('library.no_components'))}</span>
              </figcaption>
            </figure>
          );
        })}
      </div>
    </section>
  );
}

// When the rotation moves on: every few minutes or hours, every day, or after each lock.
const MINUTES = [5, 10, 15, 30, 60, 120, 180, 360, 720, 1440];
function whenChoices(r: RotationSettings): Choice[] {
  const label = (m: number) =>
    m === 1440 ? tr('library.rotation.every_day') : m % 60 === 0 ? tn('library.rotation.every_hours', m / 60) : tn('library.rotation.every_minutes', m);
  const minutes = MINUTES.includes(r.minutes) ? MINUTES : [...MINUTES, r.minutes].sort((a, b) => a - b);  // one set by hand
  return [...minutes.map(m => ({ value: String(m), label: label(m) })), { value: 'lock', label: tr('library.rotation.every_lock') }];
}

// What the rotation will do next, in a line.
function rotationStatus(r: RotationSettings, count: number, now: number) {
  if (count < 2) return tr('library.rotation.need_two');
  if (!r.on) return tr('library.rotation.off');
  if (r.trigger === 'lock') return tr('library.rotation.next_lock');
  if (!r.next) return tr('library.rotation.note');
  const minutes = Math.ceil((r.next - now) / 60000);
  return minutes <= 1 ? tr('library.rotation.next_soon') : tn('library.rotation.next_in', minutes);
}

// The themes the lock screen moves through: a switch, the themes in their order (dragged to
// reorder, the one shown marked), when it moves on and in which order. The theme in use is the
// one it has got to (library.current).
function Rotation() {
  const s = useStore();
  const r = s.lib!.settings.rotation;
  const themes = r.themes.map(themeOf).filter((t): t is ThemeEntry => !!t);
  const [now, setNow] = useState(() => Date.now());
  const [dragging, setDragging] = useState<string | null>(null);
  useEffect(() => {
    const timer = window.setInterval(() => setNow(Date.now()), 20000);
    return () => window.clearInterval(timer);
  }, []);
  const moveTo = (id: string, before: string | null) => {
    if (id === before) return;
    const ids = r.themes.filter(t => t !== id);
    const at = before ? ids.indexOf(before) : ids.length;
    ids.splice(at < 0 ? ids.length : at, 0, id);
    if (ids.join(',') !== r.themes.join(',')) setRotation(ids);
  };
  return (
    <section aria-labelledby="rotation-title">
      <div className="sechead rothead">
        <h2 className="t-h" id="rotation-title">{tr('library.rotation')}</h2>
        <Switch checked={r.on} disabled={!r.on && themes.length < 2} labelledBy="rotation-title" onChange={on => global('rotation.on', on)} />
      </div>
      <div className={cx('rotation', !r.on && 'off')}>
        {!themes.length && <p className="t-cap rotempty">{tr('library.rotation.empty')}</p>}
        <div className="rotlist" role="list" aria-label={tr('library.rotation.list')}>
          {themes.map((t, i) => (
            <div key={t.id} role="listitem" className={cx('rotitem', r.on && t.id === s.lib!.current && 'now', dragging === t.id && 'dragging')}
              draggable onDragStart={e => { setDragging(t.id); e.dataTransfer.effectAllowed = 'move'; e.dataTransfer.setData('text/plain', t.id); }}
              onDragEnd={() => setDragging(null)}
              onDragOver={e => { if (dragging) { e.preventDefault(); e.dataTransfer.dropEffect = 'move'; } }}
              onDrop={e => { e.preventDefault(); if (dragging) moveTo(dragging, t.id); setDragging(null); }}>
              <div className="thumb" style={shapeVars(t.id)}>
                <img src={thumbUrl(t.id, t.revision, 296, s.tick)} alt="" draggable={false} />
              </div>
              <div className="rotmeta">
                <span className="num t-cap3">{i + 1}</span>
                <span className="ellipsis">{t.name}</span>
                <IconButton size="xs" quiet className="rm" label={tr('library.rotation.remove_named', { name: t.name })} icon={<Dismiss12Regular />}
                  onClick={() => removeFromRotation(t.id)} />
              </div>
            </div>
          ))}
          <button type="button" className="rotadd" disabled={themes.length >= s.lib!.themes.length}
            onDragOver={e => { if (dragging) e.preventDefault(); }} onDrop={e => { e.preventDefault(); if (dragging) moveTo(dragging, null); setDragging(null); }}
            onClick={() => openDialog({ kind: 'rotation' })}>
            <Add16Regular />
            <span>{tr('library.rotation.add')}</span>
          </button>
        </div>
        <div className="rotctl">
          <label className="rotfield">
            <span className="t-cap">{tr('library.rotation.when')}</span>
            <Select size="sm" label={tr('library.rotation.when')} options={whenChoices(r)} value={r.trigger === 'lock' ? 'lock' : String(r.minutes)}
              onChange={v => {
                if (v === 'lock') global('rotation.trigger', 'lock');
                else {
                  global('rotation.minutes', v);
                  if (r.trigger !== 'interval') global('rotation.trigger', 'interval');
                }
              }} />
          </label>
          <label className="rotfield">
            <span className="t-cap">{tr('library.rotation.order')}</span>
            <Select size="sm" label={tr('library.rotation.order')} value={r.order} onChange={v => global('rotation.order', v)}
              options={[{ value: 'sequential', label: tr('library.rotation.order.sequential') }, { value: 'shuffle', label: tr('library.rotation.order.shuffle') }]} />
          </label>
          <span className="t-cap3 rotstatus" role="status">{rotationStatus(r, themes.length, now)}</span>
        </div>
      </div>
    </section>
  );
}

// Copy, Rename and Delete carry the shield of an administrator's action; `hasIcons` keeps that
// column on every item, so all the names start at the same place. `extra` comes last.
export function ThemeMenuItems({ theme, onPage, extra }: { theme: ThemeEntry; onPage?: boolean; extra?: ReactNode }) {
  // One export or import at a time: not offered while the busy bar is up.
  const { busy } = useStore();
  return (
    <MenuList hasIcons>
      {!onPage && <MenuItem onClick={() => openTheme(theme.id)}>{tr('theme.customize')}</MenuItem>}
      {!onPage && <MenuItem onClick={() => setState({ lock: { theme: theme.id } })}>{tr('theme.lock_preview')}</MenuItem>}
      {!onPage && (inRotation(theme.id)
        ? <MenuItem onClick={() => removeFromRotation(theme.id)}>{tr('theme.rotation_remove')}</MenuItem>
        : <MenuItem onClick={() => addToRotation([theme.id])}>{tr('theme.rotation_add')}</MenuItem>)}
      {!onPage && <MenuDivider />}
      <MenuItem icon={<UacShield />} onClick={() => copyFlow(theme.id)}>{tr('theme.copy')}</MenuItem>
      {!theme.builtIn && <MenuItem icon={<UacShield />} onClick={() => renameFlow(theme.id)}>{tr('theme.rename')}</MenuItem>}
      <MenuItem disabled={!!busy} onClick={() => send('export', { theme: theme.id })}>{tr('theme.export')}</MenuItem>
      {theme.modifications > 0 && <MenuItem onClick={() => resetFlow(theme.id)}>{tr('theme.reset')}</MenuItem>}
      {!theme.builtIn && <MenuDivider />}
      {!theme.builtIn && <MenuItem icon={<UacShield />} onClick={() => deleteFlow(theme.id)}>{tr('theme.delete')}</MenuItem>}
      {extra}
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
          <ThemePicture t={t} width={480} playing={hover} />
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
