// Telling displays apart: a small drawing of how the displays are arranged, as Windows' display
// settings draw them, with one of them filled in (or every one, for `all`). Beside a display's name
// and where it is (store.ts placeOf), it shows which one is meant when two have the same name and
// the same size.
import { useStore } from './store';

export function DisplaySketch({ highlight }: { highlight: string }) {
  const { env } = useStore();
  const ms = env.monitors;
  if (ms.length < 2) return null;
  const left = Math.min(...ms.map(m => m.left)), top = Math.min(...ms.map(m => m.top));
  const width = Math.max(...ms.map(m => m.left + m.width)) - left, height = Math.max(...ms.map(m => m.top + m.height)) - top;
  // At most 40 x 24, in the wall's proportions; each display a little inside its place, so that
  // displays side by side stay apart.
  const k = Math.min(40 / width, 24 / height), w = width * k, h = height * k;
  return (
    <svg className="sketch" width={Math.ceil(w) + 2} height={Math.ceil(h) + 2} viewBox={`-1 -1 ${Math.ceil(w) + 2} ${Math.ceil(h) + 2}`} aria-hidden="true">
      {ms.map(m => {
        const on = highlight === 'all' || m.key === highlight;
        return (
          <rect key={m.key} x={(m.left - left) * k + 1} y={(m.top - top) * k + 1} width={Math.max(2, m.width * k - 2)} height={Math.max(2, m.height * k - 2)}
            rx={1.5} className={on ? 'on' : undefined} />
        );
      })}
    </svg>
  );
}
