// Checks the translations in res/locales against each other and against the code:
//   - every file is a flat JSON object of strings;
//   - every language has exactly the keys Simplified Chinese has (the language strings are written
//     in first), and each string the same {placeholders};
//   - every key the C++ asks for (Tr("...")) and the page asks for (t('...')) is in the files.
// Run by `npm run check` in app/web, which CI runs. Exits 1 with a list when anything is off.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const dir = path.join(root, 'res/locales');
const source = 'zh-Hans';
const problems = [];

const tables = {};
for (const file of fs.readdirSync(dir).filter(f => f.endsWith('.json'))) {
  const lang = file.slice(0, -5);
  let table;
  try {
    table = JSON.parse(fs.readFileSync(path.join(dir, file), 'utf8').replace(/^﻿/, ''));
  } catch (e) {
    problems.push(`${file}: not JSON (${e.message})`);
    continue;
  }
  if (!table || typeof table !== 'object' || Array.isArray(table)) {
    problems.push(`${file}: not an object`);
    continue;
  }
  for (const [k, v] of Object.entries(table)) if (typeof v !== 'string') problems.push(`${file}: ${k} is not a string`);
  tables[lang] = table;
}

const placeholders = s => [...s.matchAll(/\{(\w+)\}/g)].map(m => m[1]).sort().join(',');
const base = tables[source] || {};
for (const [lang, table] of Object.entries(tables)) {
  if (lang === source) continue;
  for (const k of Object.keys(base)) {
    if (!(k in table)) problems.push(`${lang}: missing ${k}`);
    else if (placeholders(table[k]) !== placeholders(base[k])) problems.push(`${lang}: ${k} has {${placeholders(table[k])}}, ${source} has {${placeholders(base[k])}}`);
  }
  for (const k of Object.keys(table)) if (!(k in base)) problems.push(`${lang}: ${k} is not in ${source}`);
}

// The keys the code asks for by name. Keys built at run time (a prefix and a variable part) are
// written in the code as a prefix ending in a dot and are checked by their own tests.
const walk = (d, exts, out = []) => {
  for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    if (e.name === 'node_modules' || e.name === 'build' || e.name.startsWith('.')) continue;
    const p = path.join(d, e.name);
    if (e.isDirectory()) walk(p, exts, out);
    else if (exts.some(x => e.name.endsWith(x))) out.push(p);
  }
  return out;
};
const used = new Map();
for (const f of walk(root, ['.cpp', '.h'])) {
  for (const m of fs.readFileSync(f, 'utf8').matchAll(/\bTr\(\s*"([a-z0-9_.]+)"/g)) used.set(m[1], f);
}
for (const f of walk(path.join(root, 'app/web/src'), ['.ts', '.tsx'])) {
  for (const m of fs.readFileSync(f, 'utf8').matchAll(/\bt\(\s*'([a-z0-9_.]+)'/g)) used.set(m[1], f);
}
for (const [k, f] of used) if (!k.endsWith('.') && !(k in base)) problems.push(`${path.relative(root, f)}: asks for ${k}, which ${source}.json lacks`);

if (problems.length) {
  console.error(problems.join('\n'));
  console.error(`${problems.length} problem(s) in the translations`);
  process.exit(1);
}
console.log(`translations: ${Object.keys(tables).length} languages, ${Object.keys(base).length} keys each, ${used.size} used by name`);
