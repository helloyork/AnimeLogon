// An item's description as its author wrote it, in Steam's BBCode ([b]bold[/b], [url=...]...[/url],
// [list][*]...[/list]), shown as text with the same emphasis, headings and lists. Links show their
// words only: the page opens nothing but what the program names. Pictures, videos and anything
// unknown are left out or shown as they were typed.
import type { ReactNode } from 'react';

interface Node {
  tag: string;  // '' for the whole text
  children: (Node | string)[];
}

const BLOCKS = new Set(['h1', 'h2', 'h3', 'list', 'olist', 'quote', 'code', 'hr', 'table', 'tr']);
const KNOWN = new Set(['b', 'i', 'u', 's', 'strike', 'spoiler', 'url', 'noparse', 'img', 'previewyoutube', 'td', 'th', '*', ...BLOCKS]);
// Tags with nothing to close.
const SINGLE = new Set(['*', 'hr']);

function parse(text: string): Node {
  const root: Node = { tag: '', children: [] };
  const stack: Node[] = [root];
  const top = () => stack[stack.length - 1];
  const re = /\[(\/?)([a-z0-9*]+)(?:=[^\]]*)?\]/gi;
  let at = 0;
  for (let m = re.exec(text); m; m = re.exec(text)) {
    const tag = m[2].toLowerCase(), closing = m[1] === '/';
    // Inside [noparse], everything up to its end is text.
    if (top().tag === 'noparse' && !(closing && tag === 'noparse')) continue;
    if (!KNOWN.has(tag)) continue;
    if (m.index > at) top().children.push(text.slice(at, m.index));
    at = re.lastIndex;
    if (closing) {
      const i = stack.map(n => n.tag).lastIndexOf(tag);
      if (i > 0) stack.length = i;  // closes it and whatever was left open inside
      // An item of a list ends where the list does.
      else if (tag === 'list' || tag === 'olist') {
        const j = stack.map(n => n.tag).lastIndexOf('*');
        if (j > 0) stack.length = j;
      }
      continue;
    }
    if (tag === '*') {
      // A new item ends the one before.
      const j = stack.map(n => n.tag).lastIndexOf('*');
      const list = Math.max(stack.map(n => n.tag).lastIndexOf('list'), stack.map(n => n.tag).lastIndexOf('olist'));
      if (j > list && j > 0) stack.length = j;
    }
    const node: Node = { tag, children: [] };
    top().children.push(node);
    if (!SINGLE.has(tag) || tag === '*') stack.push(node);
  }
  if (at < text.length) top().children.push(text.slice(at));
  return root;
}

// Line breaks next to a heading, list or quote are the block's own.
const tidy = (s: string, before: Node | string | undefined, after: Node | string | undefined) => {
  let out = s;
  if (before && typeof before !== 'string' && BLOCKS.has(before.tag)) out = out.replace(/^\r?\n/, '');
  if (after && typeof after !== 'string' && BLOCKS.has(after.tag)) out = out.replace(/\r?\n$/, '');
  return out;
};

function render(node: Node, key = 0): ReactNode {
  const kids = node.children.map((c, i) =>
    typeof c === 'string' ? tidy(c, node.children[i - 1], node.children[i + 1]) : render(c, i),
  );
  switch (node.tag) {
    case 'b': return <strong key={key}>{kids}</strong>;
    case 'i': return <em key={key}>{kids}</em>;
    case 'u': return <u key={key}>{kids}</u>;
    case 's': case 'strike': return <s key={key}>{kids}</s>;
    case 'h1': case 'h2': case 'h3': return <strong key={key} className="bb-h">{kids}</strong>;
    case 'list': return <ul key={key}>{kids}</ul>;
    case 'olist': return <ol key={key}>{kids}</ol>;
    case '*': return <li key={key}>{kids}</li>;
    case 'quote': return <blockquote key={key}>{kids}</blockquote>;
    case 'code': return <code key={key}>{kids}</code>;
    case 'hr': return <hr key={key} />;
    case 'img': case 'previewyoutube': return null;
    case 'url': return <span key={key} className="bb-link">{kids}</span>;
    default: return <span key={key}>{kids}</span>;
  }
}

export function BBText({ text, className }: { text: string; className?: string }) {
  return <div className={className}>{render(parse(text.trim()))}</div>;
}
