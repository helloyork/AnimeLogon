// What the native side (app/src/host.cpp, library.cpp) sends the page, as it writes it.

export interface Monitor {
  key: string;
  name: string;   // 显示器 1, 显示器 2…
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
  monitors: Monitor[];
  fonts: { name: string; stored: string }[];
  devices: { id: string; name: string }[];
}

export type MonitorMode = 'duplicate' | 'span' | 'per-monitor';

export interface Settings {
  theme: string;
  monitorMode: MonitorMode;
  screens: Record<string, string>;  // monitor key -> theme id, in per-monitor mode
  components: boolean;
  componentDisplays: 'auto' | 'primary' | 'all';
  media: boolean;
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
  sceneStatic: boolean;
  summary: string;          // the visible components, or 只有壁纸
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
  sources: string[];  // 电量, 正在播放, 倒计时
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
  | { type: 'navigate'; theme: string };
