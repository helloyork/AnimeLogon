// The library's 创意工坊 tab (D62): the Workshop items the person subscribes to, each as it is on this
// computer. Not installed yet, an item is chosen with its box and installed with any others chosen
// on one administrator's approval; installed, it is a theme like any other, which a click puts on
// the logon screen. An update, once Steam has downloaded it, is installed the same way and takes
// the old version's place; an item no longer subscribed to stays a theme until it is deleted.
import { useEffect, useState, type CSSProperties } from 'react';
import { Menu, MenuDivider, MenuItem, MenuPopover, MenuTrigger } from '@fluentui/react-components';
import { ArrowClockwise16Regular, Checkmark12Filled, Image24Regular, MoreHorizontal16Regular, Warning16Regular } from '@fluentui/react-icons';
import { list, t, tn, type Key } from '../i18n';
import { ThemeMenuItems, ThemePicture } from '../library/LibraryPage';
import { applyFlow, openDialog, openTheme, perMonitor, setTab, shapeOf, themeOf, usedOn, useStore } from '../store';
import { Button, cx, IconButton } from '../ui/controls';
import { percent } from './format';
import { askShelf, install, openInSteam, setPicked, togglePick, useSteam, type ShelfItem } from './state';

const installable = (i: ShelfItem) => i.state === 'notInstalled' || i.state === 'update';

export function WorkshopShelf() {
  const s = useSteam();
  useEffect(() => askShelf(), []);
  const shelf = s.shelf;
  if (!shelf) return <p className="t-cap shelf-note" role="status">{t('shelf.loading')}</p>;
  const can = shelf.items.filter(installable);
  const picked = s.picked.filter(id => can.some(i => i.id === id));
  const allPicked = can.length > 0 && can.every(i => picked.includes(i.id));

  // What is to be installed, and which of them replace a version installed before, asked once.
  const confirm = () => {
    const chosen = shelf.items.filter(i => picked.includes(i.id));
    const updates = chosen.filter(i => i.state === 'update').length;
    openDialog({
      kind: 'confirm',
      title: chosen.length === 1 ? t('dialog.steam_install.title', { name: chosen[0].title || chosen[0].id }) : t('dialog.steam_install.title_n', { n: chosen.length }),
      body: [
        list(chosen.map(i => i.title || i.id)),
        ...(updates ? [tn('dialog.steam_install.updates', updates)] : []),
      ],
      action: t('dialog.steam_install.action'),
      elevated: true,
      run: () => install(chosen.map(i => i.id)),
    });
  };

  return (
    <section className="shelf" aria-label={t('library.tab.workshop')}>
      {shelf.source === 'folders' && (
        <div className="notice" role="status">
          <Warning16Regular />
          <span>{t('shelf.offline')}</span>
          <Button size="sm" icon={<ArrowClockwise16Regular />} onClick={askShelf}>{t('discover.retry')}</Button>
        </div>
      )}
      {shelf.items.length === 0 ? (
        <div className="shelf-empty">
          <p>{t('shelf.empty')}</p>
          <Button onClick={() => setTab('discover')}>{t('shelf.browse')}</Button>
        </div>
      ) : (
        <>
          <div className="shelf-bar">
            <span className="t-cap num">{tn('shelf.count', shelf.items.filter(i => i.subscribed).length) + (can.length ? ' · ' + tn('shelf.ready', can.length) : '')}</span>
            <div className="actions">
              {can.length > 0 && (
                <Button size="sm" onClick={() => setPicked(allPicked ? [] : can.map(i => i.id))}>{t(allPicked ? 'shelf.pick_none' : 'shelf.pick_all')}</Button>
              )}
              <Button size="sm" variant="primary" shield disabled={!picked.length} onClick={confirm}>
                {picked.length > 1 ? t('shelf.install_n', { n: picked.length }) : t('shelf.install')}
              </Button>
            </div>
          </div>
          <div className="grid">
            {shelf.items.map(i => <ShelfCard key={i.id} item={i} picked={picked.includes(i.id)} />)}
          </div>
        </>
      )}
    </section>
  );
}

// The words under an item's name: what it is on this computer.
function stateText(i: ShelfItem, progress?: { done: number; total: number }) {
  if (i.state === 'downloading') return progress && progress.total ? t('shelf.state.downloading_percent', { percent: percent(progress.done, progress.total) }) : t('shelf.state.downloading');
  return t(`shelf.state.${i.state}` as Key);
}

function ShelfCard({ item: i, picked }: { item: ShelfItem; picked: boolean }) {
  useStore();  // the library's themes, which installed items are
  const steam = useSteam();
  const [hover, setHover] = useState(false);
  const [menuOpen, setMenuOpen] = useState(false);
  const theme = i.theme ? themeOf(i.theme) : undefined;
  const used = theme ? usedOn(theme.id) : [];
  const progress = steam.progress[i.id] ?? i.progress;
  const shape = (theme ? shapeOf(theme.id) : shapeOf('')) as { width: number; height: number };
  const vars = { '--arw': String(shape.width), '--arh': String(shape.height) } as CSSProperties;
  const words = stateText(i, progress);
  const picture = theme ? (
    <ThemePicture t={theme} width={480} playing={hover} />
  ) : i.preview ? (
    <img src={i.preview} alt="" loading="lazy" decoding="async" draggable={false} referrerPolicy="no-referrer" />
  ) : (
    <span className="dpic-none" aria-hidden="true"><Image24Regular /></span>
  );
  const meta = (
    <div className="meta">
      <div className="nm ellipsis">{i.title || theme?.name || i.id}</div>
      <div className={cx('t-cap3 ellipsis', i.problem && 'shelf-problem')} title={i.problem || undefined}>
        {i.problem ? `${words} · ${i.problem}` : words}
      </div>
    </div>
  );
  return (
    <article className={cx('tcard', used.length > 0 && 'use', picked && 'picked')} data-item={i.id} style={vars}
      onPointerEnter={() => setHover(true)} onPointerLeave={() => setHover(false)}>
      {theme ? (
        // Installed: a theme like the others, put on the logon screen by a click.
        <button type="button" className="hit" aria-pressed={used.length > 0}
          onClick={() => { if (used.length && !perMonitor()) return; applyFlow(theme.id); }}>
          <div className="thumb">{picture}</div>
          {meta}
        </button>
      ) : installable(i) ? (
        // Not installed: a click chooses it, to install with the others chosen.
        <button type="button" className="hit" role="checkbox" aria-checked={picked} onClick={() => togglePick(i.id)}>
          <div className="thumb">{picture}</div>
          {meta}
        </button>
      ) : (
        <div className="hit still">
          <div className="thumb">{picture}</div>
          {meta}
        </div>
      )}
      {installable(i) && (
        <div className="over top">
          {theme ? (
            <button type="button" className={cx('pick', picked && 'on')} role="checkbox" aria-checked={picked}
              aria-label={t('shelf.pick_update', { name: i.title || theme.name })} onClick={() => togglePick(i.id)}>
              {picked && <Checkmark12Filled />}
            </button>
          ) : (
            <span className={cx('pick', picked && 'on')} aria-hidden="true">{picked && <Checkmark12Filled />}</span>
          )}
        </div>
      )}
      {theme && (
        <div className="over">
          <div className={cx('hov', menuOpen && 'shown')}>
            <Button size="sm" onClick={() => openTheme(theme.id)}>{t('theme.customize')}</Button>
            <Menu open={menuOpen} onOpenChange={(_, d) => setMenuOpen(d.open)} positioning={{ position: 'below', align: 'end', offset: 4 }}>
              <MenuTrigger disableButtonEnhancement>
                <IconButton size="sm" variant="secondary" label={t('theme.more_named', { name: theme.name })} tip={t('common.more')} icon={<MoreHorizontal16Regular />} />
              </MenuTrigger>
              <MenuPopover className="menu">
                <ThemeMenuItems theme={theme} extra={<>
                  <MenuDivider />
                  <MenuItem onClick={() => openInSteam('item', i.id)}>{t('discover.view_in_steam')}</MenuItem>
                </>} />
              </MenuPopover>
            </Menu>
          </div>
        </div>
      )}
    </article>
  );
}
