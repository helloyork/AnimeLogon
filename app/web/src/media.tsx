// Wallpapers that move play in the page: a video from the wallpaper store, or a scene's frames,
// which the native side renders in its sandbox and hands over one at a time.
import { useEffect, useId, useRef, useState, type CSSProperties } from 'react';
import { calm } from './motion';
import { ORIGIN, send, videoUrl } from './native';
import { setState } from './store';
import type { Bound, Fit } from './types';

const OBJECT_FIT: Record<Fit, CSSProperties['objectFit']> = { fill: 'cover', fit: 'contain', stretch: 'fill' };  // as the logon screen fits a wallpaper

const fill: CSSProperties = { position: 'absolute', inset: 0, width: '100%', height: '100%', display: 'block', pointerEvents: 'none' };

// A video wallpaper, muted and looping, at the theme's `brightness` and `blur` as the logon screen
// draws it (the pictures around it, drawn natively, already are). Its first frame comes in softly over the still under it;
// once a video fails to play, the page shows stills from then on. `playing` fires again each time
// the loop jumps back to the start (and after any stall), so the fade belongs to the first start
// of each video only: replayed at the seam, it showed the still through for a moment.
export function WallpaperVideo({ wallpaper, fit, brightness = 1, blur = 0, fade = true, onPlaying, style }: { wallpaper: string; fit: Fit; brightness?: number; blur?: number; fade?: boolean; onPlaying?: () => void; style?: CSSProperties }) {
  const ref = useRef<HTMLVideoElement>(null);
  const faded = useRef('');
  const filterId = 'blur' + useId().replace(/[^a-zA-Z0-9]/g, '');
  // The blur's standard deviation is a share of the height of the display the element stands for.
  const [height, setHeight] = useState(0);
  useEffect(() => {
    const v = ref.current;
    if (!v || !blur) return;
    const watch = new ResizeObserver(() => setHeight(v.clientHeight));
    watch.observe(v);
    setHeight(v.clientHeight);
    return () => watch.disconnect();
  }, [blur]);
  const sigma = blur > 0 ? (blur / 100) * height : 0;
  // Filling the element, the edges are held as the logon screen holds them: an SVG blur whose
  // colours are divided by their coverage, so nothing fades towards the edges. With bars (fit), the
  // picture is blurred over the black around it, as there.
  const filters = [
    sigma > 0 ? (fit === 'fit' ? `blur(${sigma.toFixed(2)}px)` : `url(#${filterId})`) : '',
    brightness < 1 ? `brightness(${brightness})` : '',
  ].filter(Boolean).join(' ');
  useEffect(() => {
    const v = ref.current;
    if (!v) return;
    v.muted = true;
    v.play().catch(() => {});
  }, [wallpaper]);
  return (
    <>
    {sigma > 0 && fit !== 'fit' && (
      <svg width="0" height="0" aria-hidden="true" style={{ position: 'absolute' }}>
        <filter id={filterId} x="0" y="0" width="1" height="1" colorInterpolationFilters="sRGB">
          <feGaussianBlur stdDeviation={sigma.toFixed(2)} edgeMode="duplicate" />
          <feComponentTransfer><feFuncA type="table" tableValues="1 1" /></feComponentTransfer>
        </filter>
      </svg>
    )}
    <video
      ref={ref}
      src={videoUrl(wallpaper)}
      muted
      loop
      playsInline
      preload="auto"
      aria-hidden="true"
      style={{ ...fill, objectFit: OBJECT_FIT[fit] || 'cover', ...(filters ? { filter: filters } : {}), ...style }}
      onPlaying={e => {
        if (faded.current !== wallpaper) {
          faded.current = wallpaper;
          if (fade && !calm()) e.currentTarget.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 150, easing: 'linear' });
        }
        onPlaying?.();
      }}
      onError={() => setState({ noVideo: true })}
    />
    </>
  );
}

// A panel that holds a spectrum, moving with what this computer plays: the native side draws it
// as it is each moment, cut to what it can touch (livespectrum.h), and this lays each picture where
// the panel is, over a parent that covers the whole display. About thirty pictures a second while
// the page is shown. `query` gives the display's size (w=&h=) and, for the lock-screen preview,
// lock=1. `first` is a picture already fetched, shown until the next.
export function SpectrumFrames({ theme, part, query, first }: { theme: string; part: string; query: string; first?: { url: string; box: number[] } }) {
  const img = useRef<HTMLImageElement>(null);
  const [box, setBox] = useState<number[] | null>(first?.box ?? null);
  useEffect(() => {
    let active = true, timer = 0, url = '', attempts = 0, shownBox = (first?.box ?? []).join(',');
    const next = async () => {
      if (!active) return;
      if (document.hidden) {
        timer = window.setTimeout(next, 500);
        return;
      }
      const asked = performance.now();
      try {
        const response = await fetch(`${ORIGIN}spectrum/${theme}/${part}.png?${query}`, { cache: 'no-store' });
        if (!active) return;
        if (response.ok) {
          const blob = await response.blob();
          if (!active) return;
          const fresh = URL.createObjectURL(blob), old = url;
          url = fresh;
          const where = response.headers.get('X-Part-Box') || '';
          if (where && where !== shownBox) {
            shownBox = where;
            setBox(where.split(',').map(Number));
          }
          if (img.current) img.current.src = fresh;
          if (old) requestAnimationFrame(() => URL.revokeObjectURL(old));
          attempts = 0;
        } else ++attempts;
      } catch {
        ++attempts;
      }
      // One picture every 33 ms, counting the time this one took.
      if (active && attempts < 100) timer = window.setTimeout(next, attempts ? 250 : Math.max(0, 33 - (performance.now() - asked)));
    };
    timer = window.setTimeout(next, 0);
    return () => {
      active = false;
      clearTimeout(timer);
      if (url) URL.revokeObjectURL(url);
    };
  }, [theme, part, query]);
  if (!box) return <img ref={img} alt="" draggable={false} style={{ display: 'none' }} />;
  const [x0, y0, x1, y1] = box;
  const at: CSSProperties = {
    position: 'absolute', left: `${x0 * 100}%`, top: `${y0 * 100}%`, width: `${(x1 - x0) * 100}%`, height: `${(y1 - y0) * 100}%`,
    display: 'block', pointerEvents: 'none',
  };
  return <img ref={img} alt="" draggable={false} src={first?.url} style={at} />;
}

// A scene's frames at `width` x `height` (0: the theme's own shape). With `moving`, frames follow
// one another; otherwise one frame is shown, and fetched again whenever `refresh` changes.
export function SceneFrames({ theme, width, height = 0, moving, refresh = 0, onBounds, onFrame, style }: {
  theme: string;
  width: number;
  height?: number;
  moving: boolean;
  refresh?: number;
  onBounds?: (bounds: Bound[]) => void;
  onFrame?: () => void;
  style?: CSSProperties;
}) {
  const img = useRef<HTMLImageElement>(null);
  const kick = useRef<() => void>(() => {});
  const callbacks = useRef({ onBounds, onFrame });
  callbacks.current = { onBounds, onFrame };
  const size = `w=${width}${height ? '&h=' + height : ''}`;

  useEffect(() => {
    let active = true, timer = 0, url = '', attempts = 0, serial = 0;
    const started = performance.now();
    const next = async () => {
      if (!active) return;
      if (document.hidden) {
        timer = window.setTimeout(next, 500);
        return;
      }
      const ticket = ++serial, time = moving ? performance.now() - started : 0;
      try {
        const response = await fetch(`${ORIGIN}scene/${theme}.bmp?${size}&t=${time.toFixed(1)}&motion=${moving ? 1 : 0}`, { cache: 'no-store' });
        if (!active || ticket !== serial) return;
        if (response.ok) {
          const blob = await response.blob();
          if (!active || ticket !== serial) return;
          const fresh = URL.createObjectURL(blob), old = url;
          url = fresh;
          if (img.current) {
            img.current.src = fresh;
            img.current.decode().then(() => callbacks.current.onFrame?.(), () => {});
          }
          if (old) URL.revokeObjectURL(old);
          const rectangles = response.headers.get('X-Scene-Bounds');
          if (rectangles) callbacks.current.onBounds?.(JSON.parse(rectangles));
          attempts = 0;
          if (!moving) return;
        } else if (++attempts > 100) return;
      } catch {
        if (++attempts > 100) return;
      }
      if (active) timer = window.setTimeout(next, attempts ? 100 : 33);
    };
    kick.current = () => {
      clearTimeout(timer);
      timer = window.setTimeout(next, 0);
    };
    timer = window.setTimeout(next, 0);
    return () => {
      active = false;
      clearTimeout(timer);
      if (url) URL.revokeObjectURL(url);
      send('sceneStop', { theme, width, height });
    };
  }, [theme, size, moving]);

  useEffect(() => {
    if (refresh) kick.current();
  }, [refresh]);

  return <img ref={img} alt="" draggable={false} src={`${ORIGIN}lock/${theme}/still.bmp?${size}`} style={{ ...fill, ...style }} />;
}
