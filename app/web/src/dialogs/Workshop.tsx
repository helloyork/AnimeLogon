// Wallpaper Engine's video wallpapers on this computer (app/src/workshop.h), to look at before they
// are brought in: as new themes, any number at once, or one as the open theme's wallpaper. A
// wallpaper plays while it is pointed at, and the one chosen last plays large beside the list, in
// the shape of the main display, filling it as the logon screen fills it by default.
import { useState, type CSSProperties } from 'react';
import { Dialog, DialogActions, DialogBody, DialogContent, DialogSurface, DialogTitle } from '@fluentui/react-components';
import { Checkmark12Filled, Search16Regular } from '@fluentui/react-icons';
import { calm } from '../motion';
import { send } from '../native';
import { closeDialog, mainMonitor, useStore, type Dialog as DialogState } from '../store';
import type { WorkshopItem } from '../types';
import { Button, cx } from '../ui/controls';

const fileUrl = (w: WorkshopItem, file: string) => `${w.base}${encodeURIComponent(w.dir)}/${encodeURIComponent(file)}`;

function sizeText(bytes: number) {
  const mb = bytes / 1048576;
  if (mb >= 1024) return `${(mb / 1024).toFixed(1)} GB`;
  return `${mb < 10 ? mb.toFixed(1) : Math.round(mb)} MB`;
}

const sourceText = (w: WorkshopItem) => (w.source === 'mine' ? '我的壁纸' : '创意工坊');

function duration(seconds: number) {
  const s = Math.max(0, Math.round(seconds));
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
}

// Videos this page could not play: their pictures stand in for them from then on.
const unplayable = new Set<string>();

export function WorkshopDialog({ d }: { d: Extract<DialogState, { kind: 'workshop' }> }) {
  const s = useStore();
  const found = s.workshop;
  const [query, setQuery] = useState('');
  const [chosen, setChosen] = useState<string[]>([]);
  const [shown, setShown] = useState<string | null>(null);
  const close = () => closeDialog(d.id);

  const items = found?.items ?? [];
  const byId = new Map(items.map(w => [w.id, w]));
  const words = query.trim().toLowerCase();
  const visible = words ? items.filter(w => w.title.toLowerCase().includes(words)) : items;
  // A new look may have lost some: only what is still there counts.
  const picked = chosen.filter(id => byId.has(id));
  const current = (shown && byId.get(shown)) || visible[0] || null;
  const bytes = picked.reduce((n, id) => n + (byId.get(id)?.bytes ?? 0), 0);
  const allPicked = visible.length > 0 && visible.every(w => picked.includes(w.id));

  const bring = (ids: string[]) => {
    if (!ids.length) return;
    close();
    send('importWorkshop', { items: ids, wallpaper: d.wallpaper });
  };
  const choose = (w: WorkshopItem) => {
    setShown(w.id);
    if (d.wallpaper) setChosen([w.id]);
    else setChosen(c => (c.includes(w.id) ? c.filter(x => x !== w.id) : [...c, w.id]));
  };
  const chooseAll = () => {
    const ids = new Set(visible.map(w => w.id));
    setChosen(c => (allPicked ? c.filter(id => !ids.has(id)) : [...c.filter(id => !ids.has(id)), ...ids]));
  };

  let summary: string;
  if (d.wallpaper) summary = picked.length ? `将「${byId.get(picked[0])!.title}」用作这个主题的壁纸。` : '选择一个视频用作这个主题的壁纸。';
  else summary = picked.length ? `已选择 ${picked.length} 个，共 ${sizeText(bytes)}。每个成为一个带时钟的新主题。` : '选择要导入的视频，每个成为一个带时钟的新主题。';

  return (
    <Dialog open onOpenChange={(_, data) => !data.open && close()}>
      <DialogSurface className="dialog we-dialog">
        <DialogBody>
          <DialogTitle>{d.wallpaper ? '从 Wallpaper Engine 选择壁纸' : '从 Wallpaper Engine 导入'}</DialogTitle>
          <DialogContent>
            {!found ? (
              <div className="we-empty" role="status"><p className="t-cap">正在查找 Wallpaper Engine 的壁纸…</p></div>
            ) : found.folders === 0 ? (
              <div className="we-empty">
                <p>这台电脑上没有找到 Wallpaper Engine 的壁纸。</p>
                <p className="t-cap">AnimeLogon 在 Steam 的库文件夹里查找 Wallpaper Engine 从创意工坊下载的壁纸，以及在 Wallpaper Engine 里创建的壁纸。</p>
              </div>
            ) : items.length === 0 ? (
              <div className="we-empty">
                <p>Wallpaper Engine 里没有视频壁纸。</p>
                {found.others > 0 && <p className="t-cap">{`另有 ${found.others} 个场景、网页或应用程序壁纸，这些类型不能导入。`}</p>}
              </div>
            ) : (
              <div className="we">
                <div className="we-bar">
                  <label className="field sm we-search">
                    <Search16Regular />
                    <input type="search" aria-label="按名称搜索" placeholder="按名称搜索" value={query} spellCheck={false}
                      onChange={e => setQuery(e.currentTarget.value)} />
                  </label>
                  <span className="t-cap num">
                    {(words ? `${visible.length} / ${items.length}` : `${items.length}`) + ' 个视频壁纸' +
                      (found.others ? `，另有 ${found.others} 个其他类型的壁纸不能导入` : '')}
                  </span>
                  {!d.wallpaper && visible.length > 0 && (
                    <Button size="sm" className="we-all" onClick={chooseAll}>{allPicked ? '全部取消' : '全选'}</Button>
                  )}
                </div>
                <div className="we-main">
                  <div className="we-list" role="listbox" aria-label="视频壁纸" aria-multiselectable={!d.wallpaper}>
                    {visible.map(w => (
                      <Card key={w.id} w={w} chosen={picked.includes(w.id)} multi={!d.wallpaper}
                        onChoose={() => choose(w)} onBring={d.wallpaper ? () => bring([w.id]) : undefined} />
                    ))}
                    {visible.length === 0 && <p className="t-cap3">{`没有名称包含「${query.trim()}」的视频壁纸。`}</p>}
                  </div>
                  <aside className="we-detail" aria-label="预览">
                    {current && <Detail key={current.id} w={current} />}
                  </aside>
                </div>
              </div>
            )}
          </DialogContent>
          <DialogActions position="start">
            {found && items.length > 0 && <span className="t-cap we-sum">{summary}</span>}
          </DialogActions>
          <DialogActions>
            <Button onClick={close}>取消</Button>
            <Button variant="primary" shield disabled={!picked.length} onClick={() => bring(picked)}>
              {d.wallpaper ? '导入并使用' : picked.length > 1 ? `导入 ${picked.length} 个` : '导入'}
            </Button>
          </DialogActions>
        </DialogBody>
      </DialogSurface>
    </Dialog>
  );
}

// One wallpaper in the list: its picture, playing while pointed at; a click chooses it (or, with
// several allowed, chooses it or takes it out again).
function Card({ w, chosen, multi, onChoose, onBring }: { w: WorkshopItem; chosen: boolean; multi: boolean; onChoose: () => void; onBring?: () => void }) {
  const [hover, setHover] = useState(false);
  const [, redraw] = useState(0);
  const play = hover && !calm() && !unplayable.has(w.id);
  return (
    <button type="button" role="option" aria-selected={chosen} className={cx('we-card', chosen && 'on')} title={w.title}
      onClick={onChoose} onDoubleClick={onBring} onPointerEnter={() => setHover(true)} onPointerLeave={() => setHover(false)}>
      <span className="we-pic">
        {w.preview && <img src={fileUrl(w, w.preview)} alt="" loading="lazy" decoding="async" draggable={false} />}
        {play && (
          <video src={fileUrl(w, w.video)} muted loop autoPlay playsInline preload="auto" aria-hidden="true"
            onError={() => { unplayable.add(w.id); redraw(n => n + 1); }} />
        )}
        {multi && <span className="we-check" aria-hidden="true">{chosen && <Checkmark12Filled />}</span>}
      </span>
      <span className="ellipsis">{w.title}</span>
      <span className="t-cap3 ellipsis num">{`${sourceText(w)} · ${sizeText(w.bytes)}`}</span>
    </button>
  );
}

// The wallpaper chosen last, large: playing as the main display would show it, and what it is.
function Detail({ w }: { w: WorkshopItem }) {
  const [failed, setFailed] = useState(unplayable.has(w.id));
  const [shape, setShape] = useState<{ width: number; height: number; seconds: number } | null>(null);
  const m = mainMonitor();
  const play = !calm() && !failed;
  return (
    <>
      <div className="we-big" style={{ aspectRatio: `${m.width} / ${m.height}` } as CSSProperties}>
        {w.preview && <img src={fileUrl(w, w.preview)} alt="" draggable={false} />}
        {play && (
          <video src={fileUrl(w, w.video)} muted loop autoPlay playsInline preload="auto" aria-hidden="true"
            onLoadedMetadata={e => {
              const v = e.currentTarget;
              setShape({ width: v.videoWidth, height: v.videoHeight, seconds: v.duration });
            }}
            onError={() => { unplayable.add(w.id); setFailed(true); }} />
        )}
      </div>
      <h3 className="we-title selectable">{w.title}</h3>
      <dl className="facts">
        <dt>来源</dt>
        <dd>{w.source === 'mine' ? '在 Wallpaper Engine 里创建' : 'Steam 创意工坊'}</dd>
        {shape && shape.width > 0 && (
          <>
            <dt>画面</dt>
            <dd className="num">{`${shape.width}×${shape.height}` + (isFinite(shape.seconds) ? ` · ${duration(shape.seconds)}` : '')}</dd>
          </>
        )}
        <dt>文件</dt>
        <dd className="ellipsis selectable" title={w.video}>{w.video}</dd>
        <dt>大小</dt>
        <dd className="num">{sizeText(w.bytes)}</dd>
      </dl>
      {failed && <p className="t-cap3">这个视频不能在这里播放。导入时会重新编码，能否导入以那时的结果为准。</p>}
    </>
  );
}
