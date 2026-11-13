// The lock screen as it will behave, full window: the still covers the screen at once, a video
// wallpaper starts over it, and the components come in last. A key or a click "wakes" it: all of
// that fades out over a sketch of the Windows sign-in screen. With `ratio`, it is shown as a
// display of that shape would show it, with black bars around it.
import { useEffect, useRef, useState, type CSSProperties } from 'react';
import { SceneFrames, WallpaperVideo } from '../media';
import { calm } from '../motion';
import { layerUrl, lockUrl, ORIGIN } from '../native';
import { motion } from '../preview';
import { setState, shapeOf, themeOf, useStore } from '../store';
import { Button } from '../ui/controls';

// What Windows 11 shows once the lock screen has gone, drawn after its own layout at 1920 x 1080
// and scaled with the display (sizes are in cqh of the frame): the wallpaper blurred and darkened
// as Windows does behind the password box, the account picture, the name, the PIN box and its two
// links, and the network, accessibility and power buttons in the corner. It is a sketch: nothing
// in it can be typed into.
function SignIn({ still }: { still: string }) {
  const { env } = useStore();
  return (
    <div className="lock-signin" aria-hidden="true">
      <img className="lock-blur" src={still} alt="" />
      <div className="lock-box">
        {env.accountPicture ? (
          <img className="lock-avatar" src={ORIGIN + 'account'} alt="" />
        ) : (
          <svg className="lock-avatar" viewBox="0 0 100 100">
            <circle cx="50" cy="50" r="50" fill="rgba(255,255,255,.22)" />
            <circle cx="50" cy="39" r="17" fill="#fff" />
            <path d="M18 84c4-17 17-26 32-26s28 9 32 26a50 50 0 0 1-64 0z" fill="#fff" />
          </svg>
        )}
        <div className="lock-name">{env.user || '用户'}</div>
        <div className="lock-pin"><span>PIN</span></div>
        <div className="lock-link">我忘记了我的 PIN</div>
        <div className="lock-link">登录选项</div>
      </div>
      <div className="lock-tray">
        <span>{''}</span>
        <span>{''}</span>
        <span>{''}</span>
      </div>
    </div>
  );
}

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
        <SignIn still={lockUrl(id, 'still', query)} />

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
      <div className="lock-hint">{woke ? '这里是 Windows 的登录界面（示意）' : '按任意键或单击，模拟唤醒'}</div>
      <Button size="sm" className="lock-exit" onPointerDown={e => e.stopPropagation()} onClick={close}>退出预览 (Esc)</Button>
    </div>
  );
}
