// The panel beside the picture (D40). It shows one level at a time, as the selection says: the
// wallpaper and the list of components; one component; one part of it (组件 / 时钟 / 时间). The
// breadcrumbs at its top go back up. Each level is a few sections that fold, their values in a
// grid of labels and controls.
import { useEffect, useLayoutEffect, useReducer, useRef, useState, type ReactNode } from 'react';
import { Menu, MenuItem, MenuList, MenuPopover, MenuTrigger } from '@fluentui/react-components';
import {
  Add16Regular,
  Battery1016Regular,
  ChevronDown12Regular,
  ChevronRight12Regular,
  Clock16Regular,
  Image16Regular,
  MoreHorizontal16Regular,
  MusicNote216Regular,
  PuzzlePiece16Regular,
  Timer16Regular,
  Warning16Regular,
} from '@fluentui/react-icons';
import { ARRIVE, calm } from '../motion';
import { wallpaperUrl } from '../native';
import { clearValues, edit, liveSet, pv, setValue, setValues, subscribePreview } from '../preview';
import { getState, instOf, openDialog, select, setState, setTab, useStore } from '../store';
import type { Instance, Part, SceneControls } from '../types';
import { Button, Crumbs, cx, IconButton, ResetButton, Switch } from '../ui/controls';
import { useDraft } from './draft';
import { AdjustmentField, ChoiceField, NumberField, SettingField, ToggleField, type View } from './fields';
import { adjustment, centreOf, clamp, geometry, placeCentre } from './geometry';

const COMMON_TEXT = ['font', 'weight', 'size', 'color'];

/* --- sections that fold --- */
// Which sections are open stays as the person left it while the window is open, so going from one
// component to another does not fold and unfold them again.
const openSections = new Map<string, Set<string>>();
function useSections(scope: string, initial: string[]) {
  const [open, setOpen] = useState<Set<string>>(() => openSections.get(scope) ?? new Set(initial));
  return {
    isOpen: (v: string) => open.has(v),
    toggle: (v: string) => {
      const next = new Set(open);
      if (!next.delete(v)) next.add(v);
      openSections.set(scope, next);
      setOpen(next);
    },
  };
}
type Sections = ReturnType<typeof useSections>;

function Section({ value, title, aside, sections, children }: { value: string; title: string; aside?: string; sections?: Sections; children: ReactNode }) {
  const open = sections ? sections.isOpen(value) : true;
  return (
    <section className={cx('sec', !open && 'closed')}>
      <button type="button" className="sec-h" aria-expanded={open} onClick={() => sections?.toggle(value)}>
        <span>{title}</span>
        {aside && <span className="t-cap3 num">{aside}</span>}
        <ChevronDown12Regular />
      </button>
      {open && <div className="sec-b">{children}</div>}
    </section>
  );
}

// A row that opens something: a part, a layer.
function OpenRow({ title, detail, onOpen }: { title: string; detail?: string; onOpen: () => void }) {
  return (
    <div className="item">
      <button type="button" className="go" onClick={onOpen}>
        <span className="tx">
          <span className="ellipsis">{title}</span>
          {detail && <small className="ellipsis">{detail}</small>}
        </span>
        <ChevronRight12Regular className="chev" />
      </button>
    </div>
  );
}

const BUILT_IN_ICONS: Record<string, ReactNode> = {
  clock: <Clock16Regular />,
  battery: <Battery1016Regular />,
  media: <MusicNote216Regular />,
  countdown: <Timer16Regular />,
};

/* --- the panel --- */
export function Inspector() {
  const s = useStore();
  const sel = s.sel;
  const inst = instOf(sel.inst);
  const depth = !inst ? 0 : sel.part ? 2 : inst.id === 'wallpaper' ? 0 : 1;
  const key = `${sel.inst ?? ''}/${sel.part ?? ''}`;
  const content = useRef<HTMLDivElement>(null);
  const shownDepth = useRef(depth);
  const shownKey = useRef(key);
  // Going down a level slides in from the right, going up from the left.
  useLayoutEffect(() => {
    if (key !== shownKey.current && content.current && !calm()) {
      const dx = depth > shownDepth.current ? 16 : depth < shownDepth.current ? -16 : 0;
      content.current.animate([{ opacity: 0, transform: `translateX(${dx}px)` }, { opacity: 1, transform: 'none' }], { duration: 180, easing: ARRIVE });
      content.current.parentElement?.scrollTo({ top: 0 });
    }
    shownDepth.current = depth;
    shownKey.current = key;
  }, [key, depth]);

  let view: ReactNode;
  if (inst && sel.part) view = <PartView inst={inst} partKey={sel.part} />;
  else if (inst && inst.id !== 'wallpaper') view = <InstanceView inst={inst as Instance} />;
  else view = <Overview />;
  return (
    <aside className="inspector" aria-label="调整">
      <div ref={content}>{view}</div>
    </aside>
  );
}

/* --- the wallpaper and the components --- */
const describe = (i: Instance) =>
  [i.missing ? '组件缺失' : i.builtIn ? '内置组件' : i.author ? `作者 ${i.author}` : '导入的组件', i.sources.length ? `显示${i.sources.join('、')}` : null,
    i.modified ? '已修改' : null, i.visible ? null : '已隐藏'].filter(Boolean).join(' · ');

// The list says only what sets a component apart; its page says the rest.
const brief = (i: Instance) =>
  i.missing ? '组件缺失' : [i.visible ? null : '已隐藏', i.modified ? '已修改' : null].filter(Boolean).join(' · ') || (i.builtIn ? '内置组件' : i.author ? `作者 ${i.author}` : '导入的组件');

function Overview() {
  const s = useStore();
  const sections = useSections('overview', ['wallpaper', 'components']);
  const t = s.open!, wp = t.wallpaper, scene = wp.scene;
  const full = t.room <= 0;
  const hoverOn = (id: string, on: boolean) => setState({ hover: on ? { inst: id, pk: null } : getState().hover?.inst === id ? null : getState().hover });
  return (
    <>
      <div className="ihead">
        <div className="top"><h2 className="ititle ellipsis">{t.name}</h2></div>
      </div>
      <Section value="wallpaper" title="壁纸" sections={sections}>
        <div className="wp">
          <img className="pic" src={wallpaperUrl(wp.ref)} alt="" draggable={false} />
          <div className="tx">
            <span className="nm ellipsis">{wp.name}</span>
            <span className="t-cap3 ellipsis num">{wp.meta}</span>
          </div>
          {wp.modified && <ResetButton label="换回主题自带的壁纸" onClick={() => edit('wallpaper', { ref: t.ownWallpaper })} />}
          <Button size="sm" onClick={() => openDialog({ kind: 'wallpaper' })}>更换…</Button>
        </div>
        {((wp.kind === 'video' || wp.kind === 'image') || (scene && scene.settings.length > 0)) && (
          <div className="props">
            {(wp.kind === 'video' || wp.kind === 'image') && (
              <ChoiceField id="wp-fit" label="契合度" detail="画面比例与屏幕不同时怎样显示。" value={t.fit} modified={t.fitModified}
                options={[{ value: 'fill', label: '填充' }, { value: 'fit', label: '适应' }, { value: 'stretch', label: '拉伸' }]}
                write={v => edit('fit', { value: v })} onReset={() => edit('fit', { value: t.ownFit })} />
            )}
            {scene && scene.settings.map(st => <SettingField key={st.id} instId="wallpaper" setting={st} onReset={() => clearValues('wallpaper', [st.id])} />)}
          </div>
        )}
        {wp.audio && (
          <p className="inote">视频的声音在<button type="button" className="link" onClick={() => setTab('settings')}>设置</button>里开关。</p>
        )}
        {scene && scene.static && <p className="inote">此场景在这台设备上使用静止画面。调整会保留在导出的主题中。</p>}
        {scene && scene.parts.length > 0 && (
          <div className="list">
            {scene.parts.map(p => (
              <div key={p.key} className="item">
                <button type="button" className="go" onClick={() => select('wallpaper', p.key)}>
                  <span className="ic"><Image16Regular /></span>
                  <span className="tx">
                    <span className="ellipsis">{p.label}</span>
                    {p.modified && <small>已修改</small>}
                  </span>
                  <ChevronRight12Regular className="chev" />
                </button>
              </div>
            ))}
          </div>
        )}
      </Section>
      <Section value="components" title="组件" aside={`${t.instances.length}`} sections={sections}>
        <div className="list">
          {t.instances.map(i => (
            <div key={i.id} className={cx('item', s.hover?.inst === i.id && 'hover')} onPointerEnter={() => hoverOn(i.id, true)} onPointerLeave={() => hoverOn(i.id, false)}>
              <button type="button" className="go" onClick={() => select(i.id)}>
                <span className="ic">{(i.builtIn && BUILT_IN_ICONS[i.ref]) || <PuzzlePiece16Regular />}</span>
                <span className="tx">
                  <span className="ellipsis">{i.label}</span>
                  <small className="ellipsis">{brief(i)}</small>
                </span>
              </button>
              <Switch checked={i.visible} label={`在锁屏上显示「${i.label}」`} onChange={on => edit('visible', { instance: i.id, on })} />
            </div>
          ))}
        </div>
        <Button size="sm" variant="ghost" className="add" icon={<Add16Regular />} disabled={full} onClick={() => openDialog({ kind: 'addComponent' })}>添加组件</Button>
        {full && <p className="inote">已有 8 个组件，隐藏的也算在内。</p>}
      </Section>
    </>
  );
}

/* --- one component --- */
function InstanceView({ inst }: { inst: Instance }) {
  const sections = useSections('component', ['look', 'layout', 'parts']);
  const parts = (inst.parts || []).filter(p => (inst.panels ?? 1) > 1 || p.key.includes('.'));
  const own = (inst.settings || []).filter(st => !st.folded);
  const keys = [...(inst.settings || []).map(st => st.id), ...(inst.parts || []).flatMap(p => p.adjustments.map(a => a.key))];
  const single = (inst.panels ?? 1) === 1;
  return (
    <>
      <div className="ihead">
        <div className="top">
          <Crumbs label="位置" items={[{ label: '组件', go: () => select(null) }, { label: inst.label }]} />
          <Menu positioning={{ position: 'below', align: 'end', offset: 4 }}>
            <MenuTrigger disableButtonEnhancement>
              <IconButton size="sm" label={`「${inst.label}」的更多操作`} tip="更多" icon={<MoreHorizontal16Regular />} />
            </MenuTrigger>
            <MenuPopover className="menu">
              <MenuList>
                <MenuItem disabled={!inst.modified} onClick={() => clearValues(inst.id, keys)}>恢复这个组件</MenuItem>
                {!inst.base && <MenuItem onClick={() => { select(null); edit('removeInstance', { instance: inst.id }); }}>删除</MenuItem>}
              </MenuList>
            </MenuPopover>
          </Menu>
        </div>
        <div className="sub">
          <span className="t-cap3 ellipsis">{describe(inst)}</span>
          <label className="showsw">
            显示
            <Switch checked={inst.visible} onChange={on => edit('visible', { instance: inst.id, on })} />
          </label>
        </div>
        {!inst.visible && <p className="inote">{inst.base ? '主题自带的组件只能隐藏，不能删除。' : '锁屏上不显示。'}</p>}
      </div>
      {inst.missing ? (
        <div className="ibody">
          <div className="notice"><Warning16Regular /><span>找不到这个组件的文件。</span></div>
        </div>
      ) : (
        // What a component looks like comes first; where it is, mostly set by dragging it on the
        // stage, is folded until opened; the parts that can be set one by one follow.
        <>
          {(single || own.length > 0) && (
            <Section value="look" title="外观" sections={sections}>
              <div className="props">
                {single && <PanelField instId={inst.id} pk="p1" attr="size" />}
                {own.map(st => <SettingField key={st.id} instId={inst.id} setting={st} onReset={() => clearValues(inst.id, [st.id])} />)}
              </div>
            </Section>
          )}
          {single && (
            <Section value="layout" title="排版" sections={sections}>
              <LayoutFields inst={inst} pk="p1" />
            </Section>
          )}
          {single && (
            <Section value="place" title="位置" sections={sections}>
              <PlaceFields instId={inst.id} pk="p1" />
            </Section>
          )}
          {parts.length > 0 && (
            <Section value="parts" title="逐项调整" sections={sections}>
              <div className="list">
                {parts.map(p => <OpenRow key={p.key} title={p.label} detail={p.modified ? '已单独设置' : undefined} onOpen={() => select(inst.id, p.key)} />)}
              </div>
            </Section>
          )}
        </>
      )}
    </>
  );
}

/* --- one part: a text, a panel, its shadow or backdrop; or a layer of a scene --- */
function PartView({ inst, partKey }: { inst: Instance | SceneControls; partKey: string }) {
  const kind = /^p\d+\.l\d+\.t\d+$/.test(partKey) ? 'text' : /^p\d+$/.test(partKey) ? 'panel' : 'other';
  const sections = useSections(kind === 'text' ? 'text' : kind === 'panel' ? (inst.id === 'wallpaper' ? 'layer' : 'panel') : 'part',
    kind === 'text' ? ['common'] : ['look', 'layout', 'place']);
  const pt: Part | undefined = inst.parts?.find(p => p.key === partKey);
  if (!pt) return null;
  const attr = (key: string) => key.split('.').pop() || '';
  const field = (a: Part['adjustments'][number]) => <AdjustmentField key={a.key} instId={inst.id} a={a} onReset={() => clearValues(inst.id, [a.key])} />;
  const crumbs = inst.id === 'wallpaper'
    ? [{ label: '壁纸', go: () => select(null) }, { label: pt.label }]
    : [{ label: '组件', go: () => select(null) }, { label: inst.label, go: () => select(inst.id) }, { label: pt.label }];
  let body: ReactNode;
  if (kind === 'text') {
    const common = pt.adjustments.filter(a => COMMON_TEXT.includes(attr(a.key)) && a.key.startsWith(pt.key + '.'));
    const more = pt.adjustments.filter(a => !common.includes(a));
    body = (
      <>
        <Section value="common" title="常用" sections={sections}><div className="props">{common.map(field)}</div></Section>
        {more.length > 0 && <Section value="more" title="更多" sections={sections}><div className="props">{more.map(field)}</div></Section>}
      </>
    );
  } else if (kind === 'panel') {
    const look = inst.id === 'wallpaper' ? pt.adjustments.filter(a => ['opacity', 'visible'].includes(attr(a.key))) : [];
    body = (
      <>
        <Section value="look" title="外观" sections={sections}>
          <div className="props">
            <PanelField instId={inst.id} pk={pt.key} attr="size" />
            {look.map(field)}
            {inst.id === 'wallpaper' && <PanelField instId={inst.id} pk={pt.key} attr="align" />}
          </div>
        </Section>
        {inst.id !== 'wallpaper' && (
          <Section value="layout" title="排版" sections={sections}>
            <LayoutFields inst={inst as Instance} pk={pt.key} />
          </Section>
        )}
        <Section value="place" title="位置" sections={sections}><PlaceFields instId={inst.id} pk={pt.key} /></Section>
      </>
    );
  } else {
    body = <Section value="look" title="外观" sections={sections}><div className="props">{pt.adjustments.map(field)}</div></Section>;
  }
  return (
    <>
      <div className="ihead">
        <div className="top"><Crumbs label="位置" items={crumbs} /></div>
        {/^(时间|上午\/下午|日期)$/.test(pt.label) && inst.id !== 'wallpaper' && (
          <p className="inote">
            时间制式、日期和语言在<button type="button" className="link" onClick={() => setTab('settings')}>设置</button>里改，对所有主题生效。
          </p>
        )}
      </div>
      {body}
      <div className="ifoot">
        <Button size="sm" disabled={!pt.modified} onClick={() => clearValues(inst.id, pt.adjustments.map(a => a.key))}>{`恢复「${pt.label}」`}</Button>
      </div>
    </>
  );
}

// One of a panel's own adjustments (its size, how its lines align), showing what a resize on the
// stage is about to set while it is under way.
function PanelField({ instId, pk, attr }: { instId: string; pk: string; attr: string }) {
  const draft = useDraft(instId);
  const a = adjustment(instId, pk + '.' + attr);
  if (!a) return null;
  return <AdjustmentField instId={instId} a={{ ...a, value: draft?.[a.key] ?? a.value }} onReset={() => clearValues(instId, [a.key])} />;
}

// How a panel's lines sit together, in one place: how they align, whether a text that may hang
// beside the others (an AM/PM marker beside the numbers) is left out of the alignment or counted
// in it, and how far each line is from the one above. Counting the marker in centres the date
// under the whole first line, and the stage's snap lines then take the panel's full width. The
// same values are also on each text's own page.
function LayoutFields({ inst, pk }: { inst: Instance; pk: string }) {
  const draft = useDraft(inst.id);
  const rows: { label: string; a: Part['adjustments'][number] }[] = [];
  for (const p of inst.parts || []) {
    if (!p.key.startsWith(pk + '.')) continue;
    for (const a of p.adjustments) {
      if (a.key.endsWith('.hang')) rows.push({ label: p.label, a });
      else if (/\.l\d+\.gap$/.test(a.key)) rows.push({ label: `${p.label}行距`, a });
    }
  }
  const align = adjustment(inst.id, pk + '.align');
  return (
    <div className="props">
      {align && <AdjustmentField instId={inst.id} a={{ ...align, value: draft?.[align.key] ?? align.value }} onReset={() => clearValues(inst.id, [align.key])} />}
      {rows.map(({ label, a }) => (
        <AdjustmentField key={a.key} instId={inst.id} a={{ ...a, label }} onReset={() => clearValues(inst.id, [a.key])} />
      ))}
    </div>
  );
}

const POSITION_VIEW = (unit: string): View => ({ factor: 1, digits: 1, unit: () => unit });

// Where a panel is (D60): where the middle of its box is, in percent of its display. The anchor
// follows the position, as it does for a drag, and is never shown; lining a panel up with the
// display or with other panels is what the stage's snap lines are for. The margin from the
// display's edges is under 更多. While the panel is dragged or resized on the stage, the fields
// show where it is going. Every position keeps it on its display.
function PlaceFields({ instId, pk }: { instId: string; pk: string }) {
  const draft = useDraft(instId);
  const [, redraw] = useReducer((x: number) => x + 1, 0);
  useEffect(() => subscribePreview(redraw), []);
  const [more, setMore] = useState(false);
  const anchor = adjustment(instId, pk + '.anchor'), cur = draft?.[pk + '.anchor'] ?? (anchor ? anchor.value : 'top');
  const modified = ['anchor', 'offset-x', 'offset-y'].some(k => adjustment(instId, pk + '.' + k)?.modified);
  const full = cur === 'full';
  const g = pv.width && !full ? geometry(instId, pk, 1, { width: pv.width, height: pv.height }, getState().screen) : null;
  const c = g ? centreOf(g) : null;
  const put = (x: number | null, y: number | null, gesture?: number) => {
    if (!g) return;
    const p = placeCentre(g, x, y);
    const values = { [pk + '.anchor']: p.anchor, [pk + '.offset-x']: String(p.ox), [pk + '.offset-y']: String(p.oy) };
    if (gesture) liveSet(instId, values, gesture);
    else setValues(instId, values);
  };
  const reset = () => clearValues(instId, [pk + '.anchor', pk + '.offset-x', pk + '.offset-y']);
  const margin = adjustment(instId, pk + '.margin');
  return (
    <div className="props">
      {instId === 'wallpaper' && (
        <ToggleField id={`fill-${pk}`} label="铺满画面" value={full}
          write={on => (on ? setValue(instId, pk + '.anchor', 'full') : setValues(instId, { [pk + '.anchor']: 'center', [pk + '.offset-x']: '0', [pk + '.offset-y']: '0' }))} />
      )}
      {c && (
        <>
          <NumberField id={`pos-${instId}-${pk}-x`} label="水平位置" detail="组件中心离屏幕左边多远。" min={c.x0} max={c.x1} step={0.1}
            value={draft?.['#x'] ?? String(clamp(c.x, c.x0, c.x1))} modified={false} view={POSITION_VIEW('% 屏宽')}
            write={(v, gesture) => put(v, null, gesture)} onReset={reset} />
          <NumberField id={`pos-${instId}-${pk}-y`} label="垂直位置" detail="组件中心离屏幕上边多远。" min={c.y0} max={c.y1} step={0.1}
            value={draft?.['#y'] ?? String(clamp(c.y, c.y0, c.y1))} modified={false} view={POSITION_VIEW('% 屏高')}
            write={(v, gesture) => put(null, v, gesture)} onReset={reset} />
        </>
      )}
      {!full && (margin || modified) && (
        <div className="pmore">
          {margin && (
            <button type="button" className="link quietlink" aria-expanded={more} onClick={() => setMore(!more)}>
              {more ? '收起' : '边距…'}
            </button>
          )}
          {modified && <button type="button" className="link" onClick={reset}>恢复位置</button>}
        </div>
      )}
      {margin && !full && more && <AdjustmentField instId={instId} a={margin} onReset={() => clearValues(instId, [margin.key])} />}
    </div>
  );
}
