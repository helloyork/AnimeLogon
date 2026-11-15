// The values a drag or resize on the stage would set, while the pointer is still down. Nothing is
// written until it is let go; the panel's fields show these meanwhile, so the numbers move with
// the component.
import { useSyncExternalStore } from 'react';

let draft: { inst: string; values: Record<string, string> } | null = null;
const listeners = new Set<() => void>();

export function setDraft(next: { inst: string; values: Record<string, string> } | null) {
  if (draft === next) return;
  draft = next;
  listeners.forEach(f => f());
}

const subscribe = (f: () => void) => {
  listeners.add(f);
  return () => { listeners.delete(f); };
};

export function useDraft(inst: string): Record<string, string> | null {
  const d = useSyncExternalStore(subscribe, () => draft);
  return d && d.inst === inst ? d.values : null;
}
