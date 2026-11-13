// The Steam Workshop as the page knows it, in the Steam edition only (environment `steam`): Steam's
// state, the discover page's filters and results, the item open beside them, and the items the
// person subscribes to as the library shows them. The native side asks animelogon-steam.exe
// (app/src/host_steam.cpp) and answers with the messages below; this module listens for them
// itself, apart from store.ts, and is read with useSteam.
import { useSyncExternalStore } from 'react';
import { listen, send } from '../native';
import { toast } from '../store';
import { t } from '../i18n';
import { TAGS } from './tags';

// What Steam is, as the helper last said: ok, not_running, missing_dll, no_appid, init_failed, or
// the helper's own helper_failed (it could not be started) and helper_exited.
export interface SteamStatus {
  steam: string;
  message: string;
  loggedOn: boolean;
  ownsApp: boolean;
  appid: number;
}

// An item as Steam's servers describe it (steam/PROTOCOL.md "Item").
export interface WorkshopEntry {
  id: string;
  title: string;
  description: string;
  owner: string;
  preview: string;
  tags: string[];
  descriptors: string[];
  votesUp: number;
  votesDown: number;
  score: number;
  subscriptions: number;
  favorites: number;
  fileSize: number;
  timeCreated: number;
  timeUpdated: number;
  subscribed: boolean;
  installed: boolean;
  previews?: { url: string; type: string; name: string }[];
}

// What the library shows of an item subscribed to (app/src/steam_workshop.h State).
export type ShelfState = 'downloading' | 'notDownloaded' | 'unreadable' | 'notInstalled' | 'installed' | 'update' | 'unsubscribed';

export interface ShelfItem {
  id: string;
  title: string;
  preview: string;
  state: ShelfState;
  problem: string;      // why it is unreadable, could not be downloaded or installed; or empty
  theme: string;        // the theme installed from it, or empty
  subscribed: boolean;
  size: number;
  timeUpdated: number;
  progress?: { done: number; total: number };
  descriptors: string[];
}

export interface Shelf {
  source: 'steam' | 'folders';  // the helper's word, or a look in Steam's download folders without it
  steam: string;
  items: ShelfItem[];
}

export type Sort = 'trend' | 'recent' | 'subscribed' | 'rating';
// A time range of the popular items, in days; 0 for all time.
export type Days = 1 | 7 | 30 | 90 | 365 | 0;

export interface Filters {
  text: string;
  sort: Sort;
  days: Days;
  type: string;        // a tag of TAGS.type, or empty for any
  resolution: string;  // a tag of TAGS.resolution, or empty
  sound: boolean;
  components: boolean;
  mature: boolean;     // items with content descriptors shown too; off until the person turns it on
}

export const DEFAULT_FILTERS: Filters = { text: '', sort: 'trend', days: 7, type: '', resolution: '', sound: false, components: false, mature: false };

export interface Results {
  seq: number;           // the query these are the answer to; 0 before the first
  items: WorkshopEntry[];
  total: number;
  hidden: number;        // left out for their content descriptors, over every page so far
  next: number | null;   // the page to ask for next
  loading: boolean;
  error: { code: string; message: string } | null;
}

export interface SteamState {
  status: SteamStatus | null;
  filters: Filters;
  results: Results;
  chosen: string | null;            // the item open beside the results
  details: Record<string, WorkshopEntry>;
  subscribing: Record<string, boolean>;
  progress: Record<string, { done: number; total: number }>;
  shelf: Shelf | null;
  libraryTab: 'themes' | 'workshop';
  picked: string[];                 // items chosen in the library to install
}

let state: SteamState = {
  status: null,
  filters: DEFAULT_FILTERS,
  results: { seq: 0, items: [], total: 0, hidden: 0, next: null, loading: false, error: null },
  chosen: null,
  details: {},
  subscribing: {},
  progress: {},
  shelf: null,
  libraryTab: 'themes',
  picked: [],
};
const listeners = new Set<() => void>();
const set = (patch: Partial<SteamState>) => {
  state = { ...state, ...patch };
  for (const f of listeners) f();
};
const subscribe = (f: () => void) => {
  listeners.add(f);
  return () => { listeners.delete(f); };
};
export const getSteam = () => state;
export const useSteam = () => useSyncExternalStore(subscribe, getSteam);

// Steam can be used: running, signed in, and the account owns AnimeLogon.
export const steamReady = (s: SteamStatus | null) => !!s && s.steam === 'ok' && s.loggedOn && s.ownsApp;

/* --- asking --- */
export const askStatus = () => send('steamStatus');
export const askShelf = () => send('steamItems');

const PER_PAGE = 50;
let seq = 0;
// Pages fetched one after another because the one before showed nothing (all hidden).
let emptyRun = 0;

function query(page: number) {
  const f = state.filters;
  const required = [f.type, f.resolution, f.sound ? 'Sound' : '', f.components ? 'Components' : ''].filter(Boolean);
  // Popular over all time is the most subscribed: Steam ranks by trend over a number of days only.
  const sort: Sort = f.sort === 'trend' && f.days === 0 ? 'subscribed' : f.sort;
  send('steamQuery', {
    seq: state.results.seq, sort, trendDays: f.days || 7, text: f.text.trim(), requiredTags: required,
    matchAnyTag: false, includeMature: f.mature, page, perPage: PER_PAGE,
  });
}

// The first page for the filters as they are now; what was shown before goes.
export function search() {
  emptyRun = 0;
  set({ results: { seq: ++seq, items: [], total: 0, hidden: 0, next: null, loading: true, error: null } });
  query(1);
}

export function loadMore() {
  const r = state.results;
  if (r.loading || r.next == null || r.error) return;
  set({ results: { ...r, loading: true } });
  query(r.next);
}

let typing = 0;
// New filters search again: at once, or a moment after the last key for the words typed.
export function setFilters(patch: Partial<Filters>) {
  set({ filters: { ...state.filters, ...patch } });
  clearTimeout(typing);
  if ('text' in patch && Object.keys(patch).length === 1) typing = window.setTimeout(search, 350);
  else search();
}
export function clearFilters() {
  set({ filters: { ...DEFAULT_FILTERS, mature: state.filters.mature } });
  search();
}
export const filtered = (f: Filters) => !!(f.text.trim() || f.type || f.resolution || f.sound || f.components);

export function choose(id: string | null) {
  set({ chosen: id });
  if (id) send('steamDetails', { itemId: id });
}

export function subscribeTo(id: string, on: boolean) {
  set({ subscribing: { ...state.subscribing, [id]: true } });
  send('steamSubscribe', { itemId: id, on });
}
export const download = (id: string) => send('steamDownload', { itemId: id });
export const install = (ids: string[]) => {
  if (!ids.length) return;
  set({ picked: state.picked.filter(id => !ids.includes(id)) });
  send('steamInstall', { items: ids });
};
export const openInSteam = (what: 'workshop' | 'item' | 'steam', itemId?: string) => send('steamOpen', { what, itemId });

export const setLibraryTab = (libraryTab: SteamState['libraryTab']) => {
  set({ libraryTab });
  if (libraryTab === 'workshop') askShelf();
};
export function togglePick(id: string) {
  set({ picked: state.picked.includes(id) ? state.picked.filter(x => x !== id) : [...state.picked, id] });
}
export const setPicked = (picked: string[]) => set({ picked });

/* --- what the native side says --- */
type SteamMessage =
  | ({ type: 'steamStatus' } & SteamStatus)
  | { type: 'steamQuery'; seq: number; ok: boolean; error: string; message: string; total?: number; hidden?: number; next?: number | string | null; items?: WorkshopEntry[] }
  | { type: 'steamDetails'; itemId: string; ok: boolean; error: string; message: string; item?: WorkshopEntry }
  | { type: 'steamSubscribe'; itemId: string; on: boolean; ok: boolean; error: string; message: string }
  | { type: 'steamProgress'; id: string; done: number; total: number }
  | { type: 'steamInstalled'; id: string }
  | ({ type: 'steamItems' } & Shelf);

// An item's subscription in what is shown of it.
function marked(id: string, subscribed: boolean): Partial<SteamState> {
  const r = state.results;
  const d = state.details[id];
  return {
    results: { ...r, items: r.items.map(i => (i.id === id ? { ...i, subscribed } : i)) },
    details: d ? { ...state.details, [id]: { ...d, subscribed } } : state.details,
  };
}

function receive(m: SteamMessage) {
  switch (m.type) {
    case 'steamStatus': {
      const { type: _type, ...status } = m;
      const was = steamReady(state.status);
      set({ status });
      // Steam came up: what the page shows is asked for again.
      if (!was && steamReady(status) && state.results.seq) search();
      break;
    }
    case 'steamQuery': {
      const r = state.results;
      if (m.seq !== r.seq) break;
      if (!m.ok) {
        set({ results: { ...r, loading: false, error: { code: m.error, message: m.message } } });
        // Steam may have gone or never been there: the page says which.
        askStatus();
        break;
      }
      const have = new Set(r.items.map(i => i.id));
      const fresh = (m.items ?? []).filter(i => !have.has(i.id));
      const next = typeof m.next === 'number' ? m.next : null;
      set({ results: { ...r, items: [...r.items, ...fresh], total: m.total ?? r.total, hidden: r.hidden + (m.hidden ?? 0), next, loading: false, error: null } });
      // A page whose items were all left out shows nothing: the next one is fetched at once, a few
      // times at most, so that the list does not stop at an empty page.
      if (!fresh.length && next != null && emptyRun < 4) {
        ++emptyRun;
        loadMore();
      } else if (fresh.length) emptyRun = 0;
      break;
    }
    case 'steamDetails':
      if (m.ok && m.item) set({ details: { ...state.details, [m.itemId]: m.item } });
      break;
    case 'steamSubscribe': {
      const subscribing = { ...state.subscribing };
      delete subscribing[m.itemId];
      if (m.ok) set({ subscribing, ...marked(m.itemId, m.on) });
      else {
        set({ subscribing });
        toast(t(m.on ? 'discover.subscribe_failed' : 'discover.unsubscribe_failed', { why: m.message || m.error }));
      }
      break;
    }
    case 'steamProgress':
      set({ progress: { ...state.progress, [m.id]: { done: m.done, total: m.total } } });
      break;
    case 'steamInstalled': {
      const progress = { ...state.progress };
      delete progress[m.id];
      set({ progress });
      break;
    }
    case 'steamItems': {
      const { type: _type, ...shelf } = m;
      // Picked items stay picked while they can still be installed.
      const can = new Set(shelf.items.filter(i => i.state === 'notInstalled' || i.state === 'update').map(i => i.id));
      const progress = { ...state.progress };
      for (const i of shelf.items) if (i.progress) progress[i.id] = i.progress; else if (i.state !== 'downloading') delete progress[i.id];
      set({ shelf, picked: state.picked.filter(id => can.has(id)), progress });
      break;
    }
  }
}
listen(m => receive(m as unknown as SteamMessage));

/* --- what is shown --- */
// The library's word for an item: what the person subscribes to, as installed or not.
export const shelfItem = (id: string) => state.shelf?.items.find(i => i.id === id);

// Every tag group's names, for the filters and for the publish dialog to come.
export const TYPE_TAGS: string[] = TAGS.type;
export const RESOLUTION_TAGS: string[] = TAGS.resolution;
