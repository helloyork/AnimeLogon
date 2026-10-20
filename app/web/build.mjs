// Bundles the settings app's page into one app.js beside index.html and app.css, the three files
// animelogon.exe carries as resources (app/src/web.rc.in). CMake runs it with the output
// directory in the build tree:
//   node build.mjs --out <dir>
// The bundle keeps the libraries' license comments at its end.
import { build } from 'esbuild';
import { copyFileSync, mkdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const at = process.argv.indexOf('--out');
if (at < 0 || !process.argv[at + 1]) {
  console.error('usage: node build.mjs --out <dir>');
  process.exit(2);
}
const out = resolve(process.argv[at + 1]);
mkdirSync(out, { recursive: true });

await build({
  entryPoints: [join(here, 'src', 'main.tsx')],
  outfile: join(out, 'app.js'),
  bundle: true,
  minify: true,
  format: 'iife',
  // WebView2 is evergreen Chromium.
  target: 'chrome120',
  jsx: 'automatic',
  charset: 'utf8',
  legalComments: 'eof',
  define: { 'process.env.NODE_ENV': '"production"' },
  logLevel: 'warning',
});
for (const file of ['index.html', 'app.css']) copyFileSync(join(here, file), join(out, file));
