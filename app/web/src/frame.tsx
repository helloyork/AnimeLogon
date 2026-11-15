// The window has no caption of Windows' own (app/src/frame.cpp): the app bar is its title bar.
// It shows the window's title, drags the window where nothing in it is pressable (app-region in
// styles/shell.css), and ends with the window's buttons, drawn as Windows 11 draws its own.
import { useEffect, useRef, useSyncExternalStore } from 'react';
import { t } from './i18n';
import { listen, send } from './native';
import type { WindowFrame } from './types';
import { cx } from './ui/controls';

let frame: WindowFrame = { title: 'AnimeLogon', maximised: false, active: true, max: '' };
const listeners = new Set<() => void>();
listen(m => {
  if (m.type !== 'frame') return;
  const { type: _type, ...next } = m;
  frame = next;
  for (const f of listeners) f();
});
const subscribe = (f: () => void) => {
  listeners.add(f);
  return () => { listeners.delete(f); };
};
export const useFrame = () => useSyncExternalStore(subscribe, () => frame);

const NAME = 'AnimeLogon';

// The program's icon at the pixels the display has for 16 CSS pixels: the icon file's own drawing
// for each size (app/src/host.cpp serves it), so that it stays sharp at every scale.
const MARK = [16, 20, 24, 28, 32, 40, 48].map(px => `icon?px=${px} ${px / 16}x`).join(', ');

// The window's title as the taskbar shows it, after the program's icon, as Windows puts both at the
// left end of a title bar; what follows the name (a sandbox's folder) is quieter.
export function WindowTitle() {
  const { title } = useFrame();
  const rest = title.startsWith(NAME) ? title.slice(NAME.length) : '';
  return (
    <span className="brand ellipsis">
      <img className="brand-mark" src="icon?px=16" srcSet={MARK} width={16} height={16} alt="" draggable={false} />
      {NAME}
      {rest && <span className="brand-rest">{rest}</span>}
    </span>
  );
}

// Segoe Fluent Icons, as Windows 11's caption buttons use them.
const GLYPH = { minimise: '', maximise: '', restore: '', close: '' };

// Minimise, maximise or restore, close. The native side takes the pointer over the maximise button
// so that Windows offers Snap Layouts there; it says how to draw it (`max`), and presses it itself.
// `overLock`: the lock screen preview fills the window under them.
export function WindowButtons({ overLock }: { overLock: boolean }) {
  const f = useFrame();
  const maxButton = useRef<HTMLButtonElement>(null);
  useEffect(() => reportButton(maxButton.current), []);
  const command = (action: keyof typeof GLYPH) => send('window', { action });
  const max = f.maximised ? 'restore' : 'maximise';
  return (
    <div className={cx('winctl', !f.active && 'inactive', overLock && 'over-lock')} role="group" aria-label={t('window.label')}>
      <button type="button" tabIndex={-1} title={t('window.minimise')} aria-label={t('window.minimise')} onClick={() => command('minimise')}>
        <span aria-hidden="true">{GLYPH.minimise}</span>
      </button>
      <button ref={maxButton} type="button" tabIndex={-1} className={cx(f.max === 'hover' && 'hover', f.max === 'press' && 'press')}
        title={t(f.maximised ? 'window.restore' : 'window.maximise')} aria-label={t(f.maximised ? 'window.restore' : 'window.maximise')} onClick={() => command(max)}>
        <span aria-hidden="true">{GLYPH[max]}</span>
      </button>
      <button type="button" tabIndex={-1} className="close" title={t('window.close')} aria-label={t('window.close')} onClick={() => command('close')}>
        <span aria-hidden="true">{GLYPH.close}</span>
      </button>
    </div>
  );
}

// Tells the native side where the maximise button is, in device pixels, whenever the window's size
// or scale moves it; and that it is gone when it is.
function reportButton(button: HTMLButtonElement | null) {
  if (!button) return;
  const report = () => {
    const r = button.getBoundingClientRect(), k = devicePixelRatio;
    send('frame', { max: { left: r.left * k, top: r.top * k, right: r.right * k, bottom: r.bottom * k } });
  };
  // A move to a display of another scale may leave the page's own size as it was.
  let scale: MediaQueryList | null = null;
  const watchScale = () => {
    scale?.removeEventListener('change', onScale);
    scale = matchMedia(`(resolution: ${devicePixelRatio}dppx)`);
    scale.addEventListener('change', onScale);
  };
  const onScale = () => { report(); watchScale(); };
  const resized = new ResizeObserver(report);
  resized.observe(button);
  addEventListener('resize', report);
  watchScale();
  report();
  return () => {
    resized.disconnect();
    removeEventListener('resize', report);
    scale?.removeEventListener('change', onScale);
    send('frame', { max: null });
  };
}
