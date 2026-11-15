// Bundles the settings app's page into app.js and app.css (the styles src/main.tsx imports) beside
// index.html, the three files animelogon.exe carries as resources (app/src/web.rc.in). CMake runs it with the output
// directory in the build tree:
//   node build.mjs --out <dir>
// The bundle keeps the libraries' license comments at its end. Beside it, npm-licenses.txt holds the
// name, version and licence text of every npm package that went into app.js, read from esbuild's
// list of what it bundled; install.exe installs it as licenses\npm.txt.
import { build } from 'esbuild';
import { copyFileSync, existsSync, mkdirSync, readdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

// Packages that ship no licence file, with the copyright line of the licence their package.json
// names, as their own repository states it.
const kStatedCopyright = {
  // github.com/microsoft/fluentui-system-icons, LICENSE.
  '@fluentui/react-icons': 'Copyright (c) 2020 Microsoft Corporation',
};

const kMitText = `MIT License

{copyright}

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
`;

const here = dirname(fileURLToPath(import.meta.url));
const at = process.argv.indexOf('--out');
if (at < 0 || !process.argv[at + 1]) {
  console.error('usage: node build.mjs --out <dir>');
  process.exit(2);
}
const out = resolve(process.argv[at + 1]);
mkdirSync(out, { recursive: true });

const result = await build({
  entryPoints: [join(here, 'src', 'main.tsx')],
  outfile: join(out, 'app.js'),
  absWorkingDir: here,
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
  metafile: true,
});
copyFileSync(join(here, 'index.html'), join(out, 'index.html'));
writeFileSync(join(out, 'npm-licenses.txt'), notices(packagesIn(result.metafile)));

// The packages app.js was made from: each node_modules folder an input file of it lies in.
function packagesIn(metafile) {
  const output = Object.keys(metafile.outputs).find((key) => key.endsWith('app.js'));
  const found = new Map();
  for (const input of Object.keys(metafile.outputs[output].inputs)) {
    const path = input.split('\\').join('/');
    const i = path.lastIndexOf('node_modules/');
    if (i < 0) continue;
    const parts = path.slice(i + 'node_modules/'.length).split('/');
    const name = parts[0].startsWith('@') ? `${parts[0]}/${parts[1]}` : parts[0];
    const dir = join(here, path.slice(0, i), 'node_modules', name);
    if (!found.has(dir)) found.set(dir, name);
  }
  // npm may install one version of a package in several places; it is listed once.
  const packages = new Map();
  for (const p of [...found.keys()].map(readPackage)) {
    const key = `${p.name}@${p.version}`;
    if (!packages.has(key)) packages.set(key, p);
  }
  return [...packages.values()].sort((a, b) => a.name.localeCompare(b.name) || a.version.localeCompare(b.version));
}

// A package's name, version, licence and the texts that go with it: its licence file and any
// notice file. One with neither, and no stated copyright above, stops the build, so that nothing
// is bundled without its licence.
function readPackage(dir) {
  const p = JSON.parse(readFileSync(join(dir, 'package.json'), 'utf8'));
  const license = typeof p.license === 'string' ? p.license : (p.license && p.license.type) || '';
  const files = existsSync(dir) ? readdirSync(dir) : [];
  const pick = (pattern) => files.filter((f) => pattern.test(f)).sort();
  const texts = [...pick(/^(licen[cs]e|copying)(\.(md|txt))?$/i), ...pick(/^notice(\.(md|txt))?$/i)].map((f) =>
    readFileSync(join(dir, f), 'utf8').trim(),
  );
  if (!texts.length) {
    const copyright = kStatedCopyright[p.name];
    if (!copyright || license !== 'MIT') {
      throw new Error(`${p.name} ${p.version} (${dir}) ships no licence file: add its licence to build.mjs before bundling it.`);
    }
    texts.push(kMitText.replace('{copyright}', copyright).trim());
  }
  return { name: p.name, version: p.version, license: license || 'see below', texts };
}

function notices(packages) {
  const rule = '='.repeat(78);
  const lines = [
    'AnimeLogon - third-party notices: npm packages',
    '',
    'The settings app (animelogon.exe) includes the following packages from the npm registry,',
    'bundled into its page. Each is listed with its version, its licence and the licence text',
    'it is distributed with.',
    '',
    ...packages.map((p) => `  ${p.name} ${p.version} (${p.license})`),
  ];
  for (const p of packages) {
    lines.push('', rule, `${p.name} ${p.version}`, `License: ${p.license}`, rule, '');
    lines.push(p.texts.join('\n\n'));
  }
  return lines.join('\n').split(/\r?\n/).join('\r\n') + '\r\n';
}
