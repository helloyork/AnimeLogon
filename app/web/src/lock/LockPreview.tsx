// The lock screen as it will behave, full window: the still covers the screen at once, a video
// wallpaper starts over it, and the components come in last. A key or a click "wakes" it: all of
// that fades out over a sketch of the Windows sign-in screen. With `ratio`, it is shown as a
// display of that shape would show it, with black bars around it.
import { useEffect, useRef, useState, type CSSProperties } from 'react';
import { Button, makeStyles, tokens } from '@fluentui/react-components';
import { SceneFrames, WallpaperVideo } from '../media';
import { calm } from '../motion';
import { layerUrl, lockUrl } from '../native';
import { motion } from '../preview';
import { setState, shapeOf, themeOf, useStore } from '../store';

const useStyles = makeStyles({
  root: { position: 'fixed', inset: 0, zIndex: 1000, backgroundColor: '#000', outline: 'none' },
  frame: { position: 'absolute', inset: 0, margin: 'auto', overflow: 'hidden' },
  fill: { position: 'absolute', inset: 0, width: '100%', height: '100%', display: 'block' },
  signin: { position: 'absolute', inset: 0, display: 'grid', placeItems: 'center', overflow: 'hidden' },
  blurred: { position: 'absolute', inset: 0, width: '100%', height: '100%', filter: 'blur(22px) brightness(.62)', transform: 'scale(1.1)' },
  box: { position: 'relative', display: 'grid', justifyItems: 'center', gap: '12px', color: '#fff', textAlign: 'center', paddingInline: '16px' },
  avatar: { width: '104px', height: '104px', borderRadius: '50%', backgroundColor: 'rgba(255,255,255,.16)', display: 'grid', placeItems: 'center', fontSize: '40px', fontWeight: 300 },
  name: { fontSize: '26px', fontWeight: 300 },
  pin: {
    width: '260px', maxWidth: '70vw', padding: '9px 12px', borderRadius: '4px', backgroundColor: 'rgba(255,255,255,.12)',
    borderBottom: '2px solid rgba(255,255,255,.65)', textAlign: 'left', color: 'rgba(255,255,255,.6)', boxSizing: 'border-box',
  },
  note: { color: 'rgba(255,255,255,.6)', fontSize: '12px', marginTop: '10px' },
  hint: { position: 'absolute', left: 0, right: 0, bottom: '28px', textAlign: 'center', color: 'rgba(255,255,255,.75)', fontSize: tokens.fontSizeBase300, textShadow: '0 1px 6px rgba(0,0,0,.6)' },
  exit: { position: 'absolute', top: '12px', right: '16px', zIndex: 3 },
});

export function LockPreview({ theme: id, ratio }: { theme: string; ratio?: number }) {
  const s = useStore();
  const styles = useStyles();
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
    <div ref={root} className={styles.root} role="dialog" aria-label="锁屏预览" tabIndex={-1} onPointerDown={() => setWoke(true)}>
      <div className={styles.frame} style={{ width: `min(100vw, ${(100 * aspect).toFixed(2)}vh)`, height: `min(100vh, ${(100 / aspect).toFixed(2)}vw)` }}>
        <div className={styles.signin}>
          <img className={styles.blurred} src={lockUrl(id, 'still', query)} alt="" />
          <div className={styles.box}>
            <div className={styles.avatar}>A</div>
            <div className={styles.name}>用户</div>
            <div className={styles.pin}>PIN</div>
            <div className={styles.note}>Windows 登录界面（示意）</div>
          </div>
        </div>
        <img className={styles.fill} style={fade(!woke, 0.5)} src={lockUrl(id, 'still', query)} alt="" />
        {video && (
          <WallpaperVideo wallpaper={t.wallpaperRef} fit={t.fit} fade={false} onPlaying={() => setStarted(true)}
            style={{ ...fade(started && !woke, woke ? 0.5 : 0) }} />
        )}
        {scene && (
          <SceneFrames theme={id} width={w} height={ratio ? h : 0} moving={motion() && !t.sceneStatic} onFrame={() => setStarted(true)}
            style={fade(started && !woke, woke ? 0.5 : 0)} />
        )}
        <img className={styles.fill} style={fade(componentsIn && !woke, woke ? 0.5 : 0.4)} onLoad={() => setLoaded(true)}
          src={video || scene ? layerUrl(id, t.revision, w, s.tick, ratio ? h : 0) : lockUrl(id, 'full', query)} alt="" />
      </div>
      <div className={styles.hint}>{woke ? '之后是 Windows 的登录界面' : '按任意键或单击，模拟唤醒'}</div>
      <Button className={styles.exit} onPointerDown={e => e.stopPropagation()} onClick={close}>退出预览 (Esc)</Button>
    </div>
  );
}
