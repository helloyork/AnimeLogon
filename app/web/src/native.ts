// The page talks to animelogon.exe through WebView2's message channel; pictures and the page's
// own files come from a host name only the program answers (host.cpp OnResource).
import type { FromNative } from './types';

export const ORIGIN = 'https://animelogon.invalid/';
export const MEDIA = 'https://wallpapers.animelogon.invalid/';  // the wallpaper store, for playing videos

interface WebView {
  postMessage(message: unknown): void;
  addEventListener(type: 'message', listener: (e: { data: FromNative }) => void): void;
}
const webview: WebView | undefined = (window as unknown as { chrome?: { webview?: WebView } }).chrome?.webview;

export function send(op: string, args?: Record<string, unknown>) {
  webview?.postMessage({ op, ...args });
}

export function listen(handler: (m: FromNative) => void) {
  webview?.addEventListener('message', e => handler(e.data));
}

export const videoUrl = (ref: string) => `${MEDIA}${ref}/video.mp4`;
export const thumbUrl = (id: string, revision: number, width: number, tick: number) => `${ORIGIN}thumb/${id}.bmp?w=${width}&r=${revision}.${tick}`;
export const layerUrl = (id: string, revision: number, width: number, tick: number, height = 0) =>
  `${ORIGIN}layer/${id}.png?w=${width}${height ? '&h=' + height : ''}&r=${revision}.${tick}`;
export const wallpaperUrl = (ref: string) => `${ORIGIN}wallpaper/${ref}.bmp`;
export const lockUrl = (id: string, which: 'still' | 'full', query: string) => `${ORIGIN}lock/${id}/${which}.bmp?${query}`;

// Every response says no-store, so a picture shown twice would be drawn twice. Pictures that are
// swapped in only once decoded are fetched once and kept as object URLs.
export async function fetchPicture(url: string): Promise<string | null> {
  try {
    const response = await fetch(url, { cache: 'no-store' });
    if (!response.ok) return null;
    const objectUrl = URL.createObjectURL(await response.blob());
    const image = new Image();
    image.src = objectUrl;
    try {
      await image.decode();
    } catch {
      URL.revokeObjectURL(objectUrl);
      return null;
    }
    return objectUrl;
  } catch {
    return null;
  }
}
