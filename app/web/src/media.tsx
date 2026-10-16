// Wallpapers that move play in the page: a video from the wallpaper store, or a scene's frames,
// which the native side renders in its sandbox and hands over one at a time.
import { useEffect, useRef, type CSSProperties } from 'react';
import { calm } from './motion';
import { ORIGIN, send, videoUrl } from './native';
import { setState } from './store';
import type { Bound, Fit } from './types';

const OBJECT_FIT: Record<Fit, CSSProperties['objectFit']> = { fill: 'cover', fit: 'contain', stretch: 'fill' };  // as the logon screen fits a wallpaper

const fill: CSSProperties = { position: 'absolute', inset: 0, width: '100%', height: '100%', display: 'block', pointerEvents: 'none' };

// A video wallpaper, muted and looping. Its first frame comes in softly over the still under it;
// once a video fails to play, the page shows stills from then on.
export function WallpaperVideo({ wallpaper, fit, fade = true, onPlaying, style }: { wallpaper: string; fit: Fit; fade?: boolean; onPlaying?: () => void; style?: CSSProperties }) {
  const ref = useRef<HTMLVideoElement>(null);
  useEffect(() => {
    const v = ref.current;
    if (!v) return;
    v.muted = true;
    v.play().catch(() => {});
  }, [wallpaper]);
  return (
    <video
      ref={ref}
      src={videoUrl(wallpaper)}
      muted
      loop
      playsInline
      preload="auto"
      aria-hidden="true"
      style={{ ...fill, objectFit: OBJECT_FIT[fit] || 'cover', ...style }}
      onPlaying={e => {
        if (fade && !calm()) e.currentTarget.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 150, easing: 'linear' });
        onPlaying?.();
      }}
      onError={() => setState({ noVideo: true })}
    />
  );
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
