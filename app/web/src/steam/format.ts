// Numbers, sizes and dates of Workshop items, in the page's language.
const locale = () => document.documentElement.lang || 'zh-CN';

// 25300 as 2.5万 or 25K.
export function count(n: number) {
  try {
    return new Intl.NumberFormat(locale(), { notation: 'compact', maximumFractionDigits: 1 }).format(n);
  } catch {
    return String(n);
  }
}

export function size(bytes: number) {
  if (!bytes) return '';
  const mb = bytes / 1048576;
  if (mb >= 1024) return `${(mb / 1024).toFixed(1)} GB`;
  if (mb >= 1) return `${mb < 10 ? mb.toFixed(1) : Math.round(mb)} MB`;
  return `${Math.max(1, Math.round(bytes / 1024))} KB`;
}

// Seconds since 1970 as a date.
export function date(seconds: number) {
  if (!seconds) return '';
  try {
    return new Date(seconds * 1000).toLocaleDateString(locale(), { year: 'numeric', month: 'short', day: 'numeric' });
  } catch {
    return '';
  }
}

export const percent = (done: number, total: number) => (total > 0 ? Math.min(100, Math.floor((done / total) * 100)) : 0);
