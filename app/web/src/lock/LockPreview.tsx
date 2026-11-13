// The lock screen as it will behave, full window: the still covers the screen at once, a video
// wallpaper starts over it, and the components come in last. A key or a click "wakes" it: all of
// that fades out over a sketch of the Windows sign-in screen. With `ratio`, it is shown as a
// display of that shape would show it, with black bars around it.
import { useEffect, useRef, useState, type CSSProperties } from 'react';
import { SceneFrames, WallpaperVideo } from '../media';
import { calm } from '../motion';
import { layerUrl, lockUrl } from '../native';
import { motion } from '../preview';
import { setState, shapeOf, themeOf, useStore } from '../store';
import { Button } from '../ui/controls';

export function LockPreview({ theme: id, ratio }: { theme: string; ratio?: number }) {
  const s = useStore();
  const root = useRef<HTMLDivElement>(null);
  const t = themeOf(id);
  const shape = shapeOf(id);
  const w = Math.min(2560, Math.round(screen.width * (window.devicePixelRatio || 1)));
  const aspect = ratio || shape.width / shape.height, h = Math.round(w / aspect);
  const query = `w=${w}${ratio ? '&h=' + h : ''}&r=${t?.revision ?? 0}.${s.tick}`;
  const video = !!t && t.wallpaperKind === 'video' && motion();
  const scene = !!t && t.wallpaperKind === 'scene';
  const [started, setStarted] = useState(!(video || scene));
  const [loaded, setLoaded] = useState(false);
  const [componentsIn, setComponentsIn] = useState(false);
  const [woke, setWoke] = useState(false);
  const close = () => setState({ lock: null });

  // As the logon screen does: the components come in a moment after the wallpaper is moving.
  useEffect(() => {
    if (!loaded || !started || woke) return;
    const timer = window.setTimeout(() => setComponentsIn(true), 300);
    return () => clearTimeout(timer);
  }, [loaded, started, woke]);

  useEffect(() => {
    root.current?.focus();
    if (!calm()) root.current?.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 150, easing: 'linear' });
    const key = (e: KeyboardEvent) => {
      e.stopPropagation();
      e.preventDefault();
      if (e.key === 'Escape') close();
      else setWoke(true);
    };
    document.addEventListener('keydown', key, true);
    return () => document.removeEventListener('keydown', key, true);
  }, []);

  if (!t) return null;
  const fade = (on: boolean, seconds: number): CSSProperties => ({ opacity: on ? 1 : 0, transition: calm() ? 'none' : `opacity ${seconds}s ease` });
  return (
    <div ref={root} className="lock" role="dialog" aria-label="锁屏预览" tabIndex={-1} onPointerDown={() => setWoke(true)}>
      <div className="lock-frame" style={{ width: `min(100vw, ${(100 * aspect).toFixed(2)}vh)`, height: `min(100vh, ${(100 / aspect).toFixed(2)}vw)` }}>
        <div className="lock-signin">
          <img className="lock-blur" src={lockUrl(id, 'still', query)} alt="" />
          <div className="lock-box">
            <div className="lock-avatar">A</div>
            <div className="lock-name">用户</div>
            <div className="lock-pin">PIN</div>
            <div className="lock-note">Windows 登录界面（示意）</div>
          </div>
        </div>
        <img className="lock-fill" style={fade(!woke, 0.5)} src={lockUrl(id, 'still', query)} alt="" />
        {video && (
          <WallpaperVideo wallpaper={t.wallpaperRef} fit={t.fit} fade={false} onPlaying={() => setStarted(true)}
            style={{ ...fade(started && !woke, woke ? 0.5 : 0) }} />
        )}
        {scene && (
          <SceneFrames theme={id} width={w} height={ratio ? h : 0} moving={motion() && !t.sceneStatic} onFrame={() => setStarted(true)}
            style={fade(started && !woke, woke ? 0.5 : 0)} />
        )}
        <img className="lock-fill" style={fade(componentsIn && !woke, woke ? 0.5 : 0.4)} onLoad={() => setLoaded(true)}
          src={video || scene ? layerUrl(id, t.revision, w, s.tick, ratio ? h : 0) : lockUrl(id, 'full', query)} alt="" />
      </div>
      <div className="lock-hint">{woke ? '之后是 Windows 的登录界面' : '按任意键或单击，模拟唤醒'}</div>
      <Button size="sm" className="lock-exit" onPointerDown={e => e.stopPropagation()} onClick={close}>退出预览 (Esc)</Button>
    </div>
  );
}
