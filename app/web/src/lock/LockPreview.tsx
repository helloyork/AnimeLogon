// The lock screen as it will behave, full window: the still covers the screen at once, a video
// wallpaper starts over it, and the components come in last. A key or a click "wakes" it: all of
// that fades out over a sketch of the Windows sign-in screen. With `ratio`, it is shown as a
// display of that shape would show it, with black bars around it.
import { useEffect, useRef, useState, type CSSProperties } from 'react';
import { t as tr } from '../i18n';
import { SceneFrames, SpectrumFrames, WallpaperVideo } from '../media';
import { calm } from '../motion';
import { lockUrl, ORIGIN } from '../native';
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
        <div className="lock-name">{env.user || tr('lock.user')}</div>
        <div className="lock-pin"><span>PIN</span></div>
        <div className="lock-link">{tr('lock.forgot_pin')}</div>
        <div className="lock-link">{tr('lock.options')}</div>
      </div>
      <div className="lock-tray">
        <span>{''}</span>
        <span>{''}</span>
        <span>{''}</span>
      </div>
    </div>
  );
}

// One panel of a component on its way in, as the logon screen brings it: its picture alone over
// nothing, where it is (fractions of the frame), and its entrance as the component says.
interface Entering {
  part: string;
  enter: 'fade' | 'rise' | 'zoom' | 'none';
  delay: number;
  duration: number;
  size: number;  // the panel's em, in percent of the display's height
  live: boolean; // it shows what is playing: drawn again every second
  audio: boolean; // it holds a spectrum: drawn moving with the sound (SpectrumFrames), its picture cut to the panel
  theme: string;
  query: string;  // what its pictures were asked for with
  url: string;
  box: number[];
}

type Panel = Omit<Entering, 'url' | 'box' | 'query' | 'theme'>;
const wait = (ms: number) => new Promise(ok => setTimeout(ok, ms));

// Each visible panel's picture and entrance, fetched and decoded before anything comes in, so that
// none of them is late. Null until then. The pictures show the battery and what is playing as
// they are now, as this person's own lock screen would (host.cpp): what is playing is read once
// the preview opens, so the panels wait for it, for at most three seconds. Panels that show what
// is playing are drawn again every second; all are again as the clock turns (the query changes).
// The pictures shown stay until the new ones are decoded, so nothing blinks.
function useEntering(id: string, query: string) {
  const [parts, setParts] = useState<Entering[] | null>(null);
  const shownFor = useRef(id);
  useEffect(() => {
    let alive = true;
    const urls: string[] = [];
    if (shownFor.current !== id) {
      shownFor.current = id;
      setParts(null);
    }
    const draw = async (e: Panel, q: string): Promise<Entering | null> => {
      try {
        // A panel with a spectrum moves only where the page may move things; otherwise it is one
        // picture like the rest, with the made-up sound previews show.
        const audio = e.audio && motion();
        const r = await fetch(audio ? `${ORIGIN}spectrum/${id}/${e.part}.png?${q}&lock=1` : `${ORIGIN}lock/${id}/part/${e.part}.png?${q}`);
        if (!r.ok) return null;
        const url = URL.createObjectURL(await r.blob());
        urls.push(url);
        const img = new Image();
        img.src = url;
        await img.decode();
        return { ...e, audio, theme: id, query: q, url, box: (r.headers.get('X-Part-Box') || '0,0,1,1').split(',').map(Number) };
      } catch {
        return null;  // a panel that cannot be drawn is left out, as the logon screen leaves it out
      }
    };
    (async () => {
      let list: { ready: boolean; parts: Panel[] } = { ready: false, parts: [] };
      for (let tries = 0; alive && tries < 20; ++tries) {
        list = await fetch(`${ORIGIN}lock/${id}/entrance.json?${query}`).then(r => r.json()).catch(() => ({ ready: true, parts: [] }));
        if (list.ready) break;
        await wait(150);
      }
      const out: Entering[] = [];
      for (const e of list.parts) {
        const d = await draw(e, query);
        if (d) out.push(d);
      }
      if (!alive) return;
      setParts(out);
      const live = list.parts.filter(p => p.live && !(p.audio && motion()));
      for (let n = 1; alive && live.length; ++n) {
        await wait(1000);
        const fresh = new Map<string, Entering>();
        for (const e of live) {
          const d = alive ? await draw(e, `${query}&live=${n}`) : null;
          if (d) fresh.set(e.part, d);
        }
        if (!alive) return;
        setParts(shown => {
          if (!shown) return shown;
          // The pictures replaced are let go a frame after the new ones are in.
          const gone = shown.filter(p => fresh.has(p.part)).map(p => p.url);
          requestAnimationFrame(() => requestAnimationFrame(() => gone.forEach(u => URL.revokeObjectURL(u))));
          return shown.map(p => fresh.get(p.part) ?? p);
        });
      }
    })();
    return () => {
      alive = false;
      urls.forEach(u => URL.revokeObjectURL(u));
    };
  }, [id, query]);
  return parts;
}

// A panel's picture: hidden and, for rise and zoom, set back until `go`; then it comes in after its
// delay, over its duration, as SkinView draws it on the logon screen (opacity eased in and out, the
// motion easing out: 0.35 em up, or from 90 % of its size). When the screen is woken it leaves with
// the rest.
function EnteringPart({ p, go, woke, leave }: { p: Entering; go: boolean; woke: boolean; leave: number }) {
  const still = calm() || p.enter === 'none';
  const away = p.enter === 'rise' ? `translateY(${(0.35 * p.size).toFixed(3)}cqh)` : p.enter === 'zoom' ? 'scale(.9)' : 'none';
  const [x0, y0, x1, y1] = p.box;
  const style: CSSProperties = {
    opacity: go && !woke ? 1 : 0,
    transform: go || still ? 'none' : away,
    transformOrigin: `${(((x0 + x1) / 2) * 100).toFixed(2)}% ${(((y0 + y1) / 2) * 100).toFixed(2)}%`,
    transition: woke
      ? `opacity ${leave}s ease`
      : !go
        ? 'none'
        : still
          ? `opacity 0s linear ${calm() ? 0 : p.delay}s`
          : `opacity ${p.duration}s ease-in-out ${p.delay}s, transform ${p.duration}s cubic-bezier(.33, 1, .68, 1) ${p.delay}s`,
  };
  if (p.audio)
    return (
      <div className="lock-fill" style={style}>
        <SpectrumFrames theme={p.theme} part={p.part} query={`${p.query}&lock=1`} first={{ url: p.url, box: p.box }} />
      </div>
    );
  return <img className="lock-fill" src={p.url} style={style} alt="" />;
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
  const [woke, setWoke] = useState(false);
  const parts = useEntering(id, query);
  // As the logon screen does: the components start coming in once the wallpaper is up (moving,
  // for a video or a scene), each after its own delay. A frame later, so that each picture is
  // first drawn where it starts from.
  const [go, setGo] = useState(false);
  useEffect(() => {
    if (!parts || !started || go) return;
    const frame = requestAnimationFrame(() => requestAnimationFrame(() => setGo(true)));
    return () => cancelAnimationFrame(frame);
  }, [parts, started, go]);
  const close = () => setState({ lock: null });

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
  // Woken, everything over the sign-in screen leaves together as the theme's wake transition says,
  // the way the logon screen moves its windows: fading, growing as it fades, sliding up, or at once.
  const wake = calm() ? 'cut' : t.wake, leave = wake === 'cut' ? 0 : t.wakeSeconds;
  const leaving: CSSProperties = !woke
    ? {}
    : wake === 'slide'
      ? { transform: 'translateY(-100%)', transition: `transform ${leave}s ease-in-out` }
      : wake === 'zoom'
        ? { transform: 'scale(1.08)', opacity: 0, transition: `transform ${leave}s cubic-bezier(.33, 1, .68, 1), opacity ${leave}s ease-in-out` }
        : { opacity: 0, transition: wake === 'cut' ? 'none' : `opacity ${leave}s linear` };
  return (
    <div ref={root} className="lock" role="dialog" aria-label={tr('lock.label')} tabIndex={-1} onPointerDown={() => setWoke(true)}>
      <div className="lock-frame" style={{ width: `min(100vw, ${(100 * aspect).toFixed(2)}vh)`, height: `min(100vh, ${(100 / aspect).toFixed(2)}vw)` }}>
        <SignIn still={lockUrl(id, 'still', query)} />

        <div className="lock-fill" style={leaving}>
          <img className="lock-fill" src={lockUrl(id, 'still', query)} alt="" />
          {video && (
            <WallpaperVideo wallpaper={t.wallpaperRef} fit={t.fit} brightness={t.brightness} fade={false} onPlaying={() => setStarted(true)}
              style={fade(started, 0)} />
          )}
          {scene && (
            <SceneFrames theme={id} width={w} height={ratio ? h : 0} moving={motion() && !t.sceneStatic} onFrame={() => setStarted(true)}
              style={fade(started, 0)} />
          )}
          {parts?.map(p => <EnteringPart key={p.part} p={p} go={go} woke={false} leave={0} />)}
        </div>
      </div>
      <div className="lock-hint">{woke ? tr('lock.hint.awake') : tr('lock.hint.wake')}</div>
      <Button size="sm" className="lock-exit" onPointerDown={e => e.stopPropagation()} onClick={close}>{tr('lock.exit')}</Button>
    </div>
  );
}
