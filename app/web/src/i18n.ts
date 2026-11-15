// The page's strings, in the language the native side chose (common/include/animelogon/i18n.h):
// res/locales/<tag>.json, the same files the installer and the native side read. The native side
// writes the language into index.html as it serves it (data-lang), so the first paint is already
// in it, and says it again in every environment message; a change made in the settings is sent to
// the native side, which stores it and answers with the new language.
import zhHans from '../../../res/locales/zh-Hans.json';
import zhHant from '../../../res/locales/zh-Hant.json';
import en from '../../../res/locales/en.json';
import ja from '../../../res/locales/ja.json';

export type Key = keyof typeof zhHans;
export type Lang = 'zh-Hans' | 'zh-Hant' | 'en' | 'ja';

const TABLES: Record<Lang, Partial<Record<string, string>>> = { 'zh-Hans': zhHans, 'zh-Hant': zhHant, en, ja };
// What the page's lang attribute says, so that Chromium picks the Han glyphs of the language.
const LOCALES: Record<Lang, string> = { 'zh-Hans': 'zh-CN', 'zh-Hant': 'zh-TW', en: 'en-US', ja: 'ja-JP' };
// Each language as it calls itself.
export const LANGUAGES: [Lang, string][] = [['zh-Hans', '简体中文'], ['zh-Hant', '繁體中文'], ['en', 'English'], ['ja', '日本語']];

const isLang = (v: unknown): v is Lang => typeof v === 'string' && v in TABLES;

let lang: Lang = isLang(document.documentElement.dataset.lang) ? document.documentElement.dataset.lang : 'zh-Hans';

export const currentLang = () => lang;
export function setLang(next: string) {
  if (!isLang(next) || next === lang) return;
  lang = next;
  document.documentElement.lang = LOCALES[next];
  document.documentElement.dataset.lang = next;
}

type Vars = Record<string, string | number>;
const fill = (s: string, vars?: Vars) => (vars ? s.replace(/\{(\w+)\}/g, (m, k) => (k in vars ? String(vars[k]) : m)) : s);

// The string for `key`, with each {name} replaced. A key the language lacks reads as Simplified
// Chinese, the language the strings are written in first.
export function t(key: Key, vars?: Vars): string {
  return fill(TABLES[lang][key] ?? zhHans[key] ?? key, vars);
}

// A count: `key` for any number, `key.one` for exactly one where the language has it (English).
export function tn(key: Key, n: number, vars?: Vars): string {
  const one = TABLES[lang][key + '.one'];
  return fill(n === 1 && one ? one : t(key), { n, ...vars });
}

// Items in a run, as the language joins them: 时钟、电量 / Clock, Battery.
export const list = (items: string[]) => items.join(t('list.join'));
