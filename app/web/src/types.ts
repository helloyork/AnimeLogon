// What the native side (app/src/host.cpp, library.cpp) sends the page, as it writes it.

export interface Monitor {
  key: string;
  name: string;   // Display 1, Display 2…, in the page's language
  model: string;
  left: number;
  top: number;
  width: number;
  height: number;
  primary: boolean;
}

export interface Environment {
  on: boolean;         // AnimeLogon is turned on (service and Windows' lock screen off)
  installed: boolean;
  // Installed without being switched on: the page is its setup wizard until it is (setup/SetupPage.tsx).
  setupPending: boolean;
  // Other programs that draw the logon screen; switching AnimeLogon on switches them off.
  others: { name: string; on: boolean }[];
  version: string;     // this program's version, as its version resource gives it
  user: string;        // the name Windows' sign-in screen greets this person by
  accountPicture: boolean;  // they have an account picture, served as `account`
  wallpaperEngine: boolean; // Wallpaper Engine keeps wallpapers on this computer (app/src/workshop.h)
  steam: boolean;           // the Steam edition, with the Steam Workshop (app/src/host_steam.cpp)
  // The file types each kind of import takes, as extensions without the dot: the ones the file
  // dialog for that kind lists (app/src/importable.h).
  importable: Record<ImportKind, string[]>;
  lang: string;        // the language the page is shown in: zh-Hans, zh-Hant, en or ja (i18n.ts)
  langChoice: string;  // the person's own choice of it, or empty when it follows Windows
  monitors: Monitor[];
  fonts: { name: string; stored: string }[];
  devices: { id: string; name: string }[];
}

// What a file brought in as a new theme can be; the `import` message names one.
export type ImportKind = 'video' | 'picture' | 'package';

export type MonitorMode = 'duplicate' | 'span' | 'per-monitor';

export interface Settings {
  theme: string;
  monitorMode: MonitorMode;
  screens: Record<string, string>;  // monitor key -> theme id, in per-monitor mode
  components: boolean;
  componentDisplays: 'auto' | 'primary' | 'all';
  otherDisplays: 'blur' | 'picture' | 'black';  // what the other displays show from the wake until sign-in
  media: boolean;
  tray: boolean;       // the icon in the system tray (app/src/tray.h)
  trayRecent: number;  // how many recently used themes its menu offers
  clock: { hours: 'auto' | '12' | '24'; ampm: boolean; date: 'weekday' | 'long' | 'none'; language: string };
  audio: { enabled: boolean; device: string; volume: number; video: boolean; videoVolume: number };
}

export type WallpaperKind = 'scene' | 'video' | 'image' | 'builtin' | 'none' | 'missing';

export interface ThemeEntry {
  id: string;
  name: string;
  author: string;
  builtIn: boolean;
  wallpaper: string;        // its name
  wallpaperKind: WallpaperKind;
  wallpaperRef: string;
  fit: Fit;
  wake: Wake;
  wakeSeconds: number;
  sceneStatic: boolean;
  summary: string;          // the visible components, or a word saying there are none
  modifications: number;
  revision: number;
}

export interface WallpaperEntry {
  id: string;
  name: string;
  kind: WallpaperKind;
  meta: string;
}

export interface ComponentEntry {
  id: string;
  name: string;
  author: string;
  builtIn: boolean;
}

export interface Library {
  current: string;
  settings: Settings;
  themes: ThemeEntry[];
  wallpapers: WallpaperEntry[];
  components: ComponentEntry[];
}

export type Fit = 'fill' | 'fit' | 'stretch';
// How the lock screen gives way to the sign-in screen when woken.
export type Wake = 'fade' | 'zoom' | 'slide' | 'cut';

export interface Option {
  value: string;
  label: string;
}

// A setting of a component (or of a scene wallpaper), changed for the whole of it.
export interface Setting {
  id: string;
  kind: 'toggle' | 'number' | 'color' | 'font' | 'date' | 'choice';
  label: string;
  detail: string;
  value: string;
  modified: boolean;
  folded?: boolean;   // shown with a part's adjustments rather than with the component's style
  min: number;
  max: number;
  step: number;
  options: Option[];
}

// One value of one part: p1.anchor, p1.l1.t1.size…
export interface Adjustment {
  key: string;
  label: string;
  detail: string;
  control: 'slider' | 'choice' | 'color' | 'font';
  min: number;
  max: number;
  step: number;
  value: string;
  modified: boolean;
  options: Option[];
}

// A panel (p1), or a text, shadow or backdrop in one (p1.l1.t1).
export interface Part {
  key: string;
  label: string;
  modified: boolean;
  adjustments: Adjustment[];
}

export interface Instance {
  id: string;
  ref: string;
  label: string;
  base: boolean;      // the theme's own, which can only be hidden
  visible: boolean;
  missing: boolean;
  modified: boolean;
  builtIn: boolean;
  author: string;
  sources: string[];  // what it shows besides the clock: the battery, what is playing, a countdown
  panels?: number;
  settings?: Setting[];
  parts?: Part[];
}

// A scene wallpaper's controls, shaped like an instance so the same panels can show them.
export interface SceneControls {
  id: 'wallpaper';
  label: string;
  static: boolean;
  panels: number;
  settings: Setting[];
  parts: Part[];
}

export interface OpenTheme {
  id: string;
  name: string;
  author: string;
  builtIn: boolean;
  modifications: number;
  canUndo: boolean;
  canRedo: boolean;
  revision: number;
  wallpaper: { ref: string; name: string; kind: WallpaperKind; meta: string; modified: boolean; audio: boolean; scene?: SceneControls };
  fit: Fit;
  fitModified: boolean;
  ownWallpaper: string;
  ownFit: Fit;
  wake: Wake;
  wakeModified: boolean;
  wakeSeconds: number;
  wakeSecondsModified: boolean;
  ownWake: Wake;
  ownWakeSeconds: number;
  room: number;       // how many more components fit
  instances: Instance[];
}

// Where a panel or text landed in the preview, in its pixels.
export interface Bound {
  instance: string;
  key: string;
  screen: number;
  left: number;
  top: number;
  right: number;
  bottom: number;
  // A panel's points held at a left, centre and right anchor, then at a top, middle and bottom
  // one: where the logon screen puts the anchor's point, which hanging texts and the last
  // line's baseline keep from being the box's edge or middle.
  hold?: number[];
}

export interface ScreenRect {
  left: number;
  top: number;
  right: number;
  bottom: number;
  components: boolean;
}

export type FromNative =
  | ({ type: 'environment' } & Environment)
  | { type: 'state'; clock: string; library: Library; open: OpenTheme | null }
  | { type: 'preview'; rev: number; seq: number; width: number; height: number; layer: boolean; bounds: Bound[]; screens: ScreenRect[] }
  | { type: 'toast'; text: string }
  | { type: 'problem'; text: string }
  | { type: 'busy'; label: string | null; percent?: number }
  | { type: 'added'; instance: string }
  | { type: 'navigate'; theme: string }
  | { type: 'show'; what: 'switch' | 'lock' }
  | ({ type: 'workshop' } & Workshop)
  | ({ type: 'frame' } & WindowFrame);

// A video wallpaper in Wallpaper Engine. Its files are under `base`, a host name the native side
// maps to the folder it is in, in the directory `dir`.
export interface WorkshopItem {
  id: string;
  title: string;
  base: string;
  dir: string;
  video: string;
  preview: string;  // a picture, or empty
  bytes: number;
  source: 'workshop' | 'mine';  // downloaded from the Steam Workshop, or made in Wallpaper Engine
}

// What a look for Wallpaper Engine's wallpapers found.
export interface Workshop {
  folders: number;  // folders of wallpapers found; 0: no Wallpaper Engine
  others: number;   // wallpapers of other types, which cannot be imported
  items: WorkshopItem[];
}

// The window the page is the title bar of (app/src/frame.cpp): its title, whether it is maximised
// and active, and how its maximise button is, which the native side takes the pointer over.
export interface WindowFrame {
  title: string;
  maximised: boolean;
  active: boolean;
  max: '' | 'hover' | 'press';
}
