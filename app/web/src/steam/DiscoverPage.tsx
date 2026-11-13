// 发现: the Steam Workshop inside the app, in the Steam edition only. It works the way Wallpaper
// Engine's own Workshop browser does: a search and a sort at the top (popular over a time range,
// newest, most subscribed, top rated), a bar of filters (type, resolution, sound, components, and
// mature content, hidden until the person shows it), a grid of previews that loads more as it is
// scrolled, and the item chosen open beside it, where it is subscribed to, downloaded and installed
// into the library. A small button opens the Workshop in Steam itself.
import { useEffect, useRef, useState, type ReactNode } from 'react';
import { ArrowClockwise16Regular, Checkmark12Filled, Dismiss16Regular, Image24Regular, Open16Regular, Search16Regular, Warning16Regular } from '@fluentui/react-icons';
import { t, tn, type Key } from '../i18n';
import { setTab } from '../store';
import { Button, cx, IconButton, Progress, Select, Switch } from '../ui/controls';
import { BBText } from './bbcode';
import { count, date, percent, size } from './format';
import {
  askShelf,
  askStatus,
  choose,
  clearFilters,
  download,
  filtered,
  install,
  loadMore,
  openInSteam,
  RESOLUTION_TAGS,
  search,
  setFilters,
  setLibraryTab,
  steamReady,
  subscribeTo,
  TYPE_TAGS,
  useSteam,
  type Days,
  type Filters,
  type Sort,
  type SteamStatus,
  type WorkshopEntry,
} from './state';
import { descriptorLabel, tagLabel } from './tags';

const SORTS: Sort[] = ['trend', 'recent', 'subscribed', 'rating'];
const DAYS: Days[] = [1, 7, 30, 90, 365, 0];

export function DiscoverPage() {
  const s = useSteam();
  const ready = steamReady(s.status);

  // The helper starts when the page is first shown; what it last showed stays.
  useEffect(() => {
    askStatus();
    askShelf();
  }, []);
  useEffect(() => {
    if (ready && !s.results.seq) search();
  }, [ready, s.results.seq]);
  // While Steam cannot be used, it is asked again now and then, so the page follows Steam starting.
  useEffect(() => {
    if (!s.status || ready) return;
    const timer = window.setInterval(askStatus, 5000);
    return () => window.clearInterval(timer);
  }, [s.status, ready]);

  const chosen = s.chosen ? s.details[s.chosen] ?? s.results.items.find(i => i.id === s.chosen) ?? null : null;
  return (
    <main className="discover">
      <Toolbar filters={s.filters} disabled={!ready} />
      <div className={cx('disc-body', chosen && 'with-detail')}>
        <div className="disc-results scroll" aria-busy={s.results.loading || undefined}>
          {!s.status ? <Placeholders /> : !ready ? <SteamProblem status={s.status} /> : <Grid />}
        </div>
        {chosen && <Detail key={chosen.id} item={chosen} />}
      </div>
    </main>
  );
}

function Toolbar({ filters: f, disabled }: { filters: Filters; disabled: boolean }) {
  const [text, setText] = useState(f.text);
  useEffect(() => setText(f.text), [f.text]);
  const opt = <T extends string | number>(values: T[], key: (v: T) => Key) => values.map(v => ({ value: String(v), label: t(key(v)) }));
  return (
    <div className="disc-head">
      <div className="disc-row">
        <label className="field sm disc-search">
          <Search16Regular />
          <input type="search" aria-label={t('discover.search')} placeholder={t('discover.search')} value={text} spellCheck={false} disabled={disabled}
            onChange={e => { setText(e.currentTarget.value); setFilters({ text: e.currentTarget.value }); }} />
        </label>
        <Select size="sm" label={t('discover.sort')} className="disc-sort" disabled={disabled} value={f.sort}
          options={opt(SORTS, v => `discover.sort.${v}` as Key)} onChange={v => setFilters({ sort: v as Sort })} />
        {f.sort === 'trend' && (
          <Select size="sm" label={t('discover.days')} className="disc-days" disabled={disabled} value={String(f.days)}
            options={opt(DAYS, v => `discover.days.${v}` as Key)} onChange={v => setFilters({ days: +v as Days })} />
        )}
        <Button size="sm" variant="ghost" className="disc-steam" icon={<Open16Regular />} onClick={() => openInSteam('workshop')}>{t('discover.open_workshop')}</Button>
      </div>
      <div className="disc-row disc-filters" role="group" aria-label={t('discover.filters')}>
        <Select size="sm" label={t('discover.type')} className="disc-type" disabled={disabled} value={f.type}
          options={[{ value: '', label: t('discover.type.any') }, ...TYPE_TAGS.map(v => ({ value: v, label: tagLabel(v) }))]}
          onChange={v => setFilters({ type: v })} />
        <Select size="sm" label={t('discover.resolution')} className="disc-res" disabled={disabled} value={f.resolution}
          options={[{ value: '', label: t('discover.resolution.any') }, ...RESOLUTION_TAGS.map(v => ({ value: v, label: tagLabel(v) }))]}
          onChange={v => setFilters({ resolution: v })} />
        <Toggle on={f.sound} disabled={disabled} onChange={sound => setFilters({ sound })}>{tagLabel('Sound')}</Toggle>
        <Toggle on={f.components} disabled={disabled} onChange={components => setFilters({ components })}>{tagLabel('Components')}</Toggle>
        <span className="disc-sep" aria-hidden="true" />
        <label className="disc-mature">
          <Switch checked={f.mature} disabled={disabled} labelledBy="disc-mature" onChange={mature => setFilters({ mature })} />
          <span className="disc-mature-text">
            <span id="disc-mature">{t('discover.mature')}</span>
            <span className="t-cap3">{t('discover.mature.detail')}</span>
          </span>
        </label>
      </div>
    </div>
  );
}

// A filter that is on or off, as a button that stays pressed.
function Toggle({ on, onChange, disabled, children }: { on: boolean; onChange: (on: boolean) => void; disabled?: boolean; children: string }) {
  return (
    <Button size="sm" className="toggle" aria-pressed={on} disabled={disabled} icon={on ? <Checkmark12Filled /> : undefined} onClick={() => onChange(!on)}>
      {children}
    </Button>
  );
}

// Why Steam cannot be used, and what to do.
function SteamProblem({ status }: { status: SteamStatus }) {
  const which = status.steam !== 'ok' ? status.steam : !status.loggedOn ? 'not_logged_on' : 'not_owned';
  const known = ['not_running', 'missing_dll', 'no_appid', 'init_failed', 'helper_failed', 'helper_exited', 'not_logged_on', 'not_owned'];
  const key = known.includes(which) ? which : 'init_failed';
  return (
    <State title={t(`steam.state.${key}` as Key)} detail={t(`steam.state.${key}.detail` as Key)}>
      {key === 'not_running' && <Button onClick={() => openInSteam('steam')}>{t('discover.start_steam')}</Button>}
      <Button icon={<ArrowClockwise16Regular />} onClick={askStatus}>{t('discover.retry')}</Button>
    </State>
  );
}

function State({ title, detail, children }: { title: string; detail?: string; children?: ReactNode }) {
  return (
    <div className="disc-state" role="status">
      <h2 className="t-h">{title}</h2>
      {detail && <p className="t-cap selectable">{detail}</p>}
      {children && <div className="disc-state-acts">{children}</div>}
    </div>
  );
}

function Placeholders() {
  return (
    <div className="disc-grid" aria-hidden="true">
      {Array.from({ length: 12 }, (_, i) => (
        <div key={i} className="dcard ghost">
          <span className="dpic" />
          <span className="dline" />
        </div>
      ))}
    </div>
  );
}

function Grid() {
  const s = useSteam();
  const r = s.results;
  const end = useRef<HTMLDivElement>(null);
  // Scrolled to the end, the next page comes.
  useEffect(() => {
    const el = end.current;
    if (!el) return;
    const seen = new IntersectionObserver(entries => entries.some(e => e.isIntersecting) && loadMore(), { rootMargin: '400px' });
    seen.observe(el);
    return () => seen.disconnect();
  }, [r.seq, r.items.length, r.next]);

  if (r.error && !r.items.length) {
    return (
      <State title={t('discover.error')} detail={r.error.message || r.error.code}>
        <Button icon={<ArrowClockwise16Regular />} onClick={search}>{t('discover.retry')}</Button>
      </State>
    );
  }
  if (!r.items.length && r.loading) return <Placeholders />;
  if (!r.items.length) {
    return (
      <State title={t('discover.empty')} detail={r.hidden ? tn('discover.hidden', r.hidden) : filtered(s.filters) ? t('discover.empty.detail') : undefined}>
        {filtered(s.filters) && <Button onClick={clearFilters}>{t('discover.clear')}</Button>}
      </State>
    );
  }
  return (
    <>
      <div className="disc-count t-cap num">
        {tn('discover.count', r.total)}
        {r.hidden > 0 && <span>{' · ' + tn('discover.hidden', r.hidden)}</span>}
      </div>
      <div className="disc-grid" role="list" aria-label={t('discover.results')}>
        {r.items.map(i => <Card key={i.id} item={i} chosen={s.chosen === i.id} />)}
      </div>
      <div ref={end} className="disc-end">
        {r.loading && <span className="t-cap">{t('discover.loading')}</span>}
        {!r.loading && r.error && (
          <>
            <span className="t-cap">{r.error.message || t('discover.error')}</span>
            <Button size="sm" onClick={loadMore}>{t('discover.retry')}</Button>
          </>
        )}
        {!r.loading && !r.error && r.next != null && <Button size="sm" onClick={loadMore}>{t('discover.more')}</Button>}
      </div>
    </>
  );
}

// A preview from Steam's servers; without one, or when it cannot be fetched, a plain frame.
function Preview({ url, alt = '' }: { url: string; alt?: string }) {
  const [failed, setFailed] = useState(false);
  if (!url || failed) return <span className="dpic-none" aria-hidden="true"><Image24Regular /></span>;
  return <img src={url} alt={alt} loading="lazy" decoding="async" draggable={false} referrerPolicy="no-referrer" onError={() => setFailed(true)} />;
}

function Card({ item, chosen }: { item: WorkshopEntry; chosen: boolean }) {
  const s = useSteam();
  const shelf = s.shelf?.items.find(i => i.id === item.id);
  const subscribed = shelf ? shelf.subscribed : item.subscribed;
  const meta = [subscribed ? t('discover.subscribed') : null, t('discover.subscribers_n', { n: count(item.subscriptions) })].filter(Boolean).join(' · ');
  return (
    <div role="listitem" className="dcard-cell">
      <button type="button" className={cx('dcard', chosen && 'on')} data-item={item.id} aria-pressed={chosen} title={item.title} onClick={() => choose(chosen ? null : item.id)}>
        <span className="dpic">
          <Preview url={item.preview} />
          {item.descriptors.length > 0 && <span className="dbadge">{t('discover.mature_short')}</span>}
        </span>
        <span className="dname ellipsis">{item.title}</span>
        <span className="t-cap3 ellipsis num">{meta}</span>
      </button>
    </div>
  );
}

// The item chosen, beside the results: large, what it is, and what can be done with it here.
function Detail({ item }: { item: WorkshopEntry }) {
  const s = useSteam();
  const shelf = s.shelf?.items.find(i => i.id === item.id);
  const subscribed = shelf ? shelf.subscribed : item.subscribed;
  const busy = !!s.subscribing[item.id];
  const progress = s.progress[item.id] ?? shelf?.progress;
  const votes = item.votesUp + item.votesDown;
  const pane = useRef<HTMLElement>(null);
  // Opened, the keyboard is in it; closed, it goes back to the item's card.
  useEffect(() => pane.current?.querySelector<HTMLButtonElement>('.dd-top button')?.focus({ preventScroll: true }), []);
  const shut = () => {
    choose(null);
    document.querySelector<HTMLButtonElement>(`.dcard[data-item="${item.id}"]`)?.focus({ preventScroll: true });
  };
  return (
    <aside ref={pane} className="disc-detail" aria-label={t('discover.detail')}
      onKeyDown={e => { if (e.key === 'Escape') { e.stopPropagation(); shut(); } }}>
      <div className="dd-scroll">
        <div className="dd-top">
          <IconButton size="sm" label={t('discover.close')} icon={<Dismiss16Regular />} onClick={shut} />
        </div>
        <div className="dd-pic"><Preview url={item.preview} /></div>
        <h2 className="dd-title selectable">{item.title}</h2>
        <p className="t-cap ellipsis selectable" title={item.owner}>{t('discover.by', { author: item.owner || t('discover.author') })}</p>
        <Actions item={item} subscribed={subscribed} busy={busy} progress={progress} />
        <button type="button" className="link dd-steam" onClick={() => openInSteam('item', item.id)}>{t('discover.view_in_steam')}</button>
        <dl className="facts dd-facts">
          {votes > 0 && (
            <>
              <dt>{t('discover.rating')}</dt>
              <dd className="num">{t('discover.rating.value', { percent: Math.round(item.score * 100), votes: count(votes) })}</dd>
            </>
          )}
          <dt>{t('discover.subscribers')}</dt>
          <dd className="num">{count(item.subscriptions)}</dd>
          {item.fileSize > 0 && (
            <>
              <dt>{t('discover.size')}</dt>
              <dd className="num">{size(item.fileSize)}</dd>
            </>
          )}
          {item.timeUpdated > 0 && (
            <>
              <dt>{t('discover.updated')}</dt>
              <dd className="num">{date(item.timeUpdated)}</dd>
            </>
          )}
          {item.tags.length > 0 && (
            <>
              <dt>{t('discover.tags')}</dt>
              <dd>{item.tags.map(tagLabel).join(t('list.join'))}</dd>
            </>
          )}
          {item.descriptors.length > 0 && (
            <>
              <dt>{t('discover.descriptors')}</dt>
              <dd>{item.descriptors.map(descriptorLabel).join(t('list.join'))}</dd>
            </>
          )}
        </dl>
        {item.description.trim() ? <BBText className="dd-text selectable" text={item.description} /> : <p className="t-cap3">{t('discover.no_description')}</p>}
      </div>
    </aside>
  );
}

// Subscribe first; once subscribed, its files come down by themselves, then it goes into the
// library with an administrator's approval.
function Actions({ item, subscribed, busy, progress }: { item: WorkshopEntry; subscribed: boolean; busy: boolean; progress?: { done: number; total: number } }) {
  const s = useSteam();
  const shelf = s.shelf?.items.find(i => i.id === item.id);
  if (!subscribed) {
    return (
      <div className="dd-acts">
        <Button variant="primary" disabled={busy} onClick={() => subscribeTo(item.id, true)}>{t('discover.subscribe')}</Button>
      </div>
    );
  }
  const state = shelf?.state;
  let main: ReactNode = null;
  if (progress || state === 'downloading') {
    const p = progress ? percent(progress.done, progress.total) : 0;
    main = (
      <div className="dd-progress">
        <Progress value={p} label={t('discover.downloading', { percent: p })} />
        <span className="t-cap num">{t('discover.downloading', { percent: p })}</span>
      </div>
    );
  } else if (state === 'notInstalled' || state === 'update') {
    main = <Button variant="primary" shield onClick={() => install([item.id])}>{t(state === 'update' ? 'discover.install_update' : 'discover.install')}</Button>;
  } else if (state === 'installed') {
    main = (
      <>
        <span className="t-cap">{t('discover.installed')}</span>
        <Button size="sm" onClick={() => { setLibraryTab('workshop'); setTab('mine'); }}>{t('discover.show_in_library')}</Button>
      </>
    );
  } else if (state === 'notDownloaded') {
    main = <Button onClick={() => download(item.id)}>{t('discover.download')}</Button>;
  }
  return (
    <>
      <div className="dd-acts">
        {main}
        <Button disabled={busy} onClick={() => subscribeTo(item.id, false)}>{t('discover.unsubscribe')}</Button>
      </div>
      {shelf?.problem && (
        <p className="dd-problem t-cap selectable"><Warning16Regular />{shelf.problem}</p>
      )}
    </>
  );
}
