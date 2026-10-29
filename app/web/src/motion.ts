// Windows' "Animation effects" off reaches the page as prefers-reduced-motion. Fluent's own
// components follow it; whatever the page animates itself checks this first.
const reduce = matchMedia('(prefers-reduced-motion: reduce)');
export const calm = () => reduce.matches;
// The preference can change while the window is open (store.ts redraws the page when it does).
export const onCalmChange = (f: () => void) => reduce.addEventListener('change', f);

// Fluent's curve for things arriving (tokens.curveDecelerateMax), spelled out: Element.animate
// takes no CSS variables.
export const ARRIVE = 'cubic-bezier(0, 0, 0, 1)';
