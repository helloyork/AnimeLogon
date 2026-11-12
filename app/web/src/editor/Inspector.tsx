// The panel beside the picture, like a settings page (D40). It shows one level at a time, as the
// selection says: the wallpaper and the list of components; one component; one part of it
// (组件 › 时钟 › 时间). The breadcrumb goes back up.
import { Fragment, useEffect, useLayoutEffect, useReducer, useRef, useState, type ReactNode } from 'react';
import {
  Accordion,
  AccordionHeader,
  AccordionItem,
  AccordionPanel,
  Breadcrumb,
  BreadcrumbButton,
  BreadcrumbDivider,
  BreadcrumbItem,
  Button,
  Caption1,
  Divider,
  Link,
  makeStyles,
  Menu,
  MenuItem,
  MenuList,
  MenuPopover,
  MenuTrigger,
  MessageBar,
  MessageBarBody,
  mergeClasses,
  Subtitle2,
  Switch,
  Text,
  ToggleButton,
  Tooltip,
  tokens,
} from '@fluentui/react-components';
import { AddRegular, ArrowResetRegular, ChevronDownRegular, ChevronRightRegular, ChevronUpRegular, MoreHorizontalRegular } from '@fluentui/react-icons';
import { ARRIVE, calm } from '../motion';
import { wallpaperUrl } from '../native';
import { clearValues, edit, liveSet, pv, setValue, setValues, subscribePreview } from '../preview';
import { getState, instOf, openDialog, select, setState, setTab, useStore } from '../store';
import type { Instance, Part, SceneControls } from '../types';
import { useDraft } from './draft';
import { adjustment, centreOf, clamp, geometry, placeCentre } from './geometry';
import { AdjustmentField, ChoiceField, NumberField, SettingField, type View } from './fields';

const COMMON_TEXT = ['font', 'weight', 'size', 'color'];

const useStyles = makeStyles({
  panel: {
    width: '360px',
    flex: 'none',
    display: 'flex',
    flexDirection: 'column',
    minHeight: 0,
    backgroundColor: tokens.colorNeutralBackground1,
    borderLeft: `${tokens.strokeWidthThin} solid ${tokens.colorNeutralStroke2}`,
  },
  scroll: { flex: 1, minHeight: 0, overflowY: 'auto', overflowX: 'hidden' },
  body: { display: 'flex', flexDirection: 'column', gap: tokens.spacingVerticalL, padding: tokens.spacingHorizontalL },
  section: { display: 'flex', flexDirection: 'column', gap: tokens.spacingVerticalS },
  heading: { display: 'flex', alignItems: 'baseline', gap: tokens.spacingHorizontalS },
  quiet: { color: tokens.colorNeutralForeground3 },
  wallpaper: { display: 'grid', gridTemplateColumns: '88px minmax(0, 1fr) auto', alignItems: 'center', gap: tokens.spacingHorizontalM },
  shot: { width: '88px', aspectRatio: '16 / 9', objectFit: 'cover', borderRadius: tokens.borderRadiusMedium, backgroundColor: '#000', display: 'block' },
  info: { display: 'flex', flexDirection: 'column', minWidth: 0 },
  ellipsis: { overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' },
  rows: { display: 'flex', flexDirection: 'column', marginInline: `calc(-1 * ${tokens.spacingHorizontalS})` },
  row: {
    display: 'flex',
    alignItems: 'center',
    gap: tokens.spacingHorizontalS,
    minHeight: '48px',
    paddingInline: tokens.spacingHorizontalS,
    borderRadius: tokens.borderRadiusMedium,
    cursor: 'pointer',
    ':hover': { backgroundColor: tokens.colorSubtleBackgroundHover },
    ':active': { backgroundColor: tokens.colorSubtleBackgroundPressed },
  },
  rowHover: { backgroundColor: tokens.colorSubtleBackgroundHover },
  rowText: { flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column', paddingBlock: tokens.spacingVerticalS },
  chevron: { color: tokens.colorNeutralForeground3, flex: 'none' },
  top: { display: 'flex', flexDirection: 'column', gap: tokens.spacingVerticalXS, padding: `${tokens.spacingVerticalM} ${tokens.spacingHorizontalL} 0` },
  titleRow: { display: 'flex', alignItems: 'center', gap: tokens.spacingHorizontalS },
  foot: { display: 'flex', gap: tokens.spacingHorizontalS, paddingTop: tokens.spacingVerticalS },
  accordionPanel: { marginInline: 0, paddingBottom: tokens.spacingVerticalS },
  place: { display: 'flex', flexDirection: 'column', gap: tokens.spacingVerticalXS, paddingBlock: tokens.spacingVerticalS },
  more: { alignSelf: 'flex-start', paddingInline: 0, minWidth: 0 },
  crumb: { fontSize: tokens.fontSizeBase500, lineHeight: tokens.lineHeightBase500, color: tokens.colorNeutralForeground2 },
  crumbNow: { fontSize: tokens.fontSizeBase500, lineHeight: tokens.lineHeightBase500, fontWeight: tokens.fontWeightSemibold },
});

/* --- shared pieces --- */
function Section({ title, aside, children }: { title: string; aside?: string; children: ReactNode }) {
  const styles = useStyles();
  return (
    <section className={styles.section}>
      <div className={styles.heading}>
        <Subtitle2 as="h2">{title}</Subtitle2>
        {aside && <Caption1 className={styles.quiet}>{aside}</Caption1>}
      </div>
      {children}
    </section>
  );
}

// The panel's title: where in it the person is, larger than its groups (Windows' settings).
function Crumbs({ items }: { items: { label: string; go?: () => void }[] }) {
  const styles = useStyles();
  return (
    <Breadcrumb aria-label="位置" size="large">
      {items.map((it, i) => (
        <Fragment key={i}>
          {i > 0 && <BreadcrumbDivider />}
          <BreadcrumbItem>
            <BreadcrumbButton className={it.go ? styles.crumb : styles.crumbNow} current={!it.go} onClick={it.go}>{it.label}</BreadcrumbButton>
          </BreadcrumbItem>
        </Fragment>
      ))}
    </Breadcrumb>
  );
}

// A row that opens something: a component, a part.
function OpenRow({ title, detail, onOpen, aside, hovered, onHover }: { title: string; detail?: string; onOpen: () => void; aside?: ReactNode; hovered?: boolean; onHover?: (on: boolean) => void }) {
  const styles = useStyles();
  return (
    <div className={mergeClasses(styles.row, hovered && styles.rowHover)} role="button" tabIndex={0} onClick={onOpen}
      onKeyDown={e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); onOpen(); } }}
      onPointerEnter={() => onHover?.(true)} onPointerLeave={() => onHover?.(false)}>
      <div className={styles.rowText}>
        <Text className={styles.ellipsis}>{title}</Text>
        {detail && <Caption1 className={mergeClasses(styles.quiet, styles.ellipsis)}>{detail}</Caption1>}
      </div>
      {aside}
      <ChevronRightRegular className={styles.chevron} />
    </div>
  );
}

function Group({ value, title, children }: { value: string; title: string; children: ReactNode }) {
  const styles = useStyles();
  return (
    <AccordionItem value={value}>
      <AccordionHeader expandIconPosition="end" size="large">{title}</AccordionHeader>
      <AccordionPanel className={styles.accordionPanel}>{children}</AccordionPanel>
    </AccordionItem>
  );
}

// Which groups are open stays as the person left it while the window is open, so going from one
// component to another does not fold and unfold them again.
const openGroups = new Map<string, string[]>();
function Groups({ scope, initial, children }: { scope: string; initial: string[]; children: ReactNode }) {
  const [open, setOpen] = useState<string[]>(() => openGroups.get(scope) ?? initial);
  return (
    <Accordion multiple collapsible openItems={open}
      onToggle={(_, d) => { const next = d.openItems as string[]; openGroups.set(scope, next); setOpen(next); }}>
      {children}
    </Accordion>
  );
}

/* --- the panel --- */
export function Inspector() {
  const s = useStore();
  const styles = useStyles();
  const sel = s.sel;
  const inst = instOf(sel.inst);
  const depth = !inst ? 0 : sel.part ? 2 : inst.id === 'wallpaper' ? 0 : 1;
  const key = `${sel.inst ?? ''}/${sel.part ?? ''}`;
  const content = useRef<HTMLDivElement>(null);
  const shownDepth = useRef(depth);
  const shownKey = useRef(key);
  // Going down a level slides in from the right, going up from the left (Windows' drill-in).
  useLayoutEffect(() => {
    if (key !== shownKey.current && content.current && !calm()) {
      const dx = depth > shownDepth.current ? 24 : depth < shownDepth.current ? -24 : 0;
      content.current.animate([{ opacity: 0, transform: `translateX(${dx}px)` }, { opacity: 1, transform: 'none' }], { duration: 200, easing: ARRIVE });
      content.current.parentElement?.scrollTo({ top: 0 });
    }
    shownDepth.current = depth;
    shownKey.current = key;
  }, [key, depth]);

  let view: ReactNode;
  if (inst && sel.inst === 'wallpaper' && sel.part) view = <PartView inst={inst} partKey={sel.part} />;
  else if (inst && inst.id !== 'wallpaper' && sel.part) view = <PartView inst={inst} partKey={sel.part} />;
  else if (inst && inst.id !== 'wallpaper') view = <InstanceView inst={inst as Instance} />;
  else view = <Overview />;
  return (
    <aside className={styles.panel} aria-label="调整">
      <div className={styles.scroll}>
        <div ref={content}>{view}</div>
      </div>
    </aside>
  );
}

/* --- the wallpaper and the components --- */
function Overview() {
  const s = useStore();
  const styles = useStyles();
  const t = s.open!, wp = t.wallpaper, scene = wp.scene;
  const full = t.room <= 0;
  const hoverOn = (id: string, on: boolean) => setState({ hover: on ? { inst: id, pk: null } : getState().hover?.inst === id ? null : getState().hover });
  return (
    <div className={styles.body}>
      <Section title="壁纸">
        <div className={styles.wallpaper}>
          <img className={styles.shot} src={wallpaperUrl(wp.ref)} alt="" draggable={false} />
          <div className={styles.info}>
            <Text className={styles.ellipsis}>{wp.name}</Text>
            <Caption1 className={mergeClasses(styles.quiet, styles.ellipsis)}>{wp.meta}</Caption1>
          </div>
          <div className={styles.titleRow}>
            {wp.modified && (
              <Tooltip content="换回主题自带的壁纸" relationship="label">
                <Button appearance="subtle" icon={<ArrowResetRegular />} onClick={() => edit('wallpaper', { ref: t.ownWallpaper })} />
              </Tooltip>
            )}
            <Button onClick={() => openDialog({ kind: 'wallpaper' })}>更换…</Button>
          </div>
        </div>
        {(wp.kind === 'video' || wp.kind === 'image') && (
          <ChoiceField id="wp-fit" label="契合度" detail="画面比例与屏幕不同时怎样显示。" value={t.fit} modified={t.fitModified}
            options={[{ value: 'fill', label: '填充' }, { value: 'fit', label: '适应' }, { value: 'stretch', label: '拉伸' }]}
            write={v => edit('fit', { value: v })} onReset={() => edit('fit', { value: t.ownFit })} />
        )}
        {wp.audio && (
          <Caption1 className={styles.quiet}>
            视频的声音在<Link inline onClick={() => setTab('settings')}>设置</Link>里开关。
          </Caption1>
        )}
        {scene && (
          <>
            {scene.settings.map(st => <SettingField key={st.id} instId="wallpaper" setting={st} onReset={() => clearValues('wallpaper', [st.id])} />)}
            {scene.static && <Caption1 className={styles.quiet}>此场景在这台设备上使用静止画面。调整会保留在导出的主题中。</Caption1>}
            {scene.parts.length > 0 && (
              <div className={styles.rows}>
                {scene.parts.map(p => <OpenRow key={p.key} title={p.label} detail={p.modified ? '已修改' : undefined} onOpen={() => select('wallpaper', p.key)} />)}
              </div>
            )}
          </>
        )}
      </Section>
      <Divider />
      <Section title="组件" aside={`${t.instances.length} 个`}>
        <div className={styles.rows}>
          {t.instances.map(i => (
            <OpenRow key={i.id} title={i.label} detail={describe(i)} onOpen={() => select(i.id)}
              hovered={s.hover?.inst === i.id} onHover={on => hoverOn(i.id, on)}
              aside={<span onClick={e => e.stopPropagation()}><Switch checked={i.visible} aria-label={`在锁屏上显示「${i.label}」`} onChange={(_, d) => edit('visible', { instance: i.id, on: d.checked })} /></span>} />
          ))}
        </div>
        <div className={styles.foot}>
          <Button icon={<AddRegular />} disabled={full} onClick={() => openDialog({ kind: 'addComponent' })}>添加组件</Button>
        </div>
        {full && <Caption1 className={styles.quiet}>已有 8 个组件，隐藏的也算在内。</Caption1>}
      </Section>
    </div>
  );
}

const describe = (i: Instance) =>
  [i.missing ? '组件缺失' : i.builtIn ? '内置组件' : i.author ? `作者 ${i.author}` : '导入的组件', i.sources.length ? `显示${i.sources.join('、')}` : null,
    i.modified ? '已修改' : null, i.visible ? null : '已隐藏'].filter(Boolean).join(' · ');

/* --- one component --- */
function InstanceView({ inst }: { inst: Instance }) {
  const styles = useStyles();
  const parts = (inst.parts || []).filter(p => (inst.panels ?? 1) > 1 || p.key.includes('.'));
  const own = (inst.settings || []).filter(st => !st.folded);
  const keys = [...(inst.settings || []).map(st => st.id), ...(inst.parts || []).flatMap(p => p.adjustments.map(a => a.key))];
  const single = (inst.panels ?? 1) === 1;
  return (
    <>
      <div className={styles.top}>
        <Crumbs items={[{ label: '组件', go: () => select(null) }, { label: inst.label }]} />
        <div className={styles.titleRow}>
          <Caption1 className={mergeClasses(styles.quiet, styles.ellipsis)} style={{ flex: 1 }}>{describe(inst)}</Caption1>
          <Switch checked={inst.visible} label="显示" labelPosition="before" onChange={(_, d) => edit('visible', { instance: inst.id, on: d.checked })} />
          <Menu>
            <MenuTrigger disableButtonEnhancement>
              <Button appearance="subtle" icon={<MoreHorizontalRegular />} aria-label={`「${inst.label}」的更多操作`} />
            </MenuTrigger>
            <MenuPopover>
              <MenuList>
                <MenuItem disabled={!inst.modified} onClick={() => clearValues(inst.id, keys)}>恢复这个组件</MenuItem>
                {!inst.base && <MenuItem onClick={() => { select(null); edit('removeInstance', { instance: inst.id }); }}>删除</MenuItem>}
              </MenuList>
            </MenuPopover>
          </Menu>
        </div>
      </div>
      <div className={styles.body}>
        {inst.missing ? (
          <MessageBar intent="warning"><MessageBarBody>找不到这个组件的文件。</MessageBarBody></MessageBar>
        ) : (
          // What a component looks like comes first; where it is, mostly set by dragging it on the
          // stage, is folded until opened; the parts that can be set one by one follow.
          <Groups scope="component" initial={['look', 'parts']}>
            {(single || own.length > 0) && (
              <Group value="look" title="外观">
                {single && <PanelField instId={inst.id} pk="p1" attr="size" />}
                {own.map(st => <SettingField key={st.id} instId={inst.id} setting={st} onReset={() => clearValues(inst.id, [st.id])} />)}
                {single && <PanelField instId={inst.id} pk="p1" attr="align" />}
              </Group>
            )}
            {single && <Group value="place" title="位置"><PlaceFields instId={inst.id} pk="p1" /></Group>}
            {parts.length > 0 && (
              <Group value="parts" title="逐项调整">
                <div className={styles.rows}>
                  {parts.map(p => <OpenRow key={p.key} title={p.label} detail={p.modified ? '已单独设置' : undefined} onOpen={() => select(inst.id, p.key)} />)}
                </div>
              </Group>
            )}
          </Groups>
        )}
        {!inst.visible && <Caption1 className={styles.quiet}>{inst.base ? '已隐藏。主题自带的组件只能隐藏，不能删除。' : '已隐藏，锁屏上不显示。'}</Caption1>}
      </div>
    </>
  );
}

/* --- one part: a text, a panel, its shadow or backdrop; or a layer of a scene --- */
function PartView({ inst, partKey }: { inst: Instance | SceneControls; partKey: string }) {
  const styles = useStyles();
  const pt: Part | undefined = inst.parts?.find(p => p.key === partKey);
  if (!pt) return null;
  const attr = (key: string) => key.split('.').pop() || '';
  const field = (a: Part['adjustments'][number]) => <AdjustmentField key={a.key} instId={inst.id} a={a} onReset={() => clearValues(inst.id, [a.key])} />;
  const crumbs = inst.id === 'wallpaper'
    ? [{ label: '壁纸', go: () => select(null) }, { label: pt.label }]
    : [{ label: '组件', go: () => select(null) }, { label: inst.label, go: () => select(inst.id) }, { label: pt.label }];
  let body: ReactNode;
  if (/^p\d+\.l\d+\.t\d+$/.test(pt.key)) {
    const common = pt.adjustments.filter(a => COMMON_TEXT.includes(attr(a.key)) && a.key.startsWith(pt.key + '.'));
    const more = pt.adjustments.filter(a => !common.includes(a));
    body = (
      <>
        {/^(时间|上午\/下午|日期)$/.test(pt.label) && (
          <Caption1 className={styles.quiet}>
            时间制式、日期和语言在<Link inline onClick={() => setTab('settings')}>设置 › 时钟</Link>里改，对所有主题生效。
          </Caption1>
        )}
        <Accordion multiple collapsible defaultOpenItems={['common']}>
          <Group value="common" title="常用">{common.map(field)}</Group>
          {more.length > 0 && <Group value="more" title="更多">{more.map(field)}</Group>}
        </Accordion>
      </>
    );
  } else if (/^p\d+$/.test(pt.key)) {
    const look = inst.id === 'wallpaper' ? pt.adjustments.filter(a => ['opacity', 'visible'].includes(attr(a.key))) : [];
    body = (
      <Groups scope={inst.id === 'wallpaper' ? 'layer' : 'panel'} initial={['look', 'place']}>
        <Group value="look" title="外观">
          <PanelField instId={inst.id} pk={pt.key} attr="size" />
          {look.map(field)}
          <PanelField instId={inst.id} pk={pt.key} attr="align" />
        </Group>
        <Group value="place" title="位置"><PlaceFields instId={inst.id} pk={pt.key} /></Group>
      </Groups>
    );
  } else {
    body = (
      <Accordion multiple collapsible defaultOpenItems={['look']}>
        <Group value="look" title="外观">{pt.adjustments.map(field)}</Group>
      </Accordion>
    );
  }
  return (
    <>
      <div className={styles.top}><Crumbs items={crumbs} /></div>
      <div className={styles.body}>
        {body}
        <div className={styles.foot}>
          <Button disabled={!pt.modified} onClick={() => clearValues(inst.id, pt.adjustments.map(a => a.key))}>{`恢复「${pt.label}」`}</Button>
        </div>
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

const POSITION_VIEW = (unit: string): View => ({ factor: 1, digits: 1, unit: () => unit });

// Where a panel is (D60): where the middle of its box is, in percent of its display. The anchor
// follows the position, as it does for a drag, and is never shown; lining a panel up with the
// display or with other panels is what the stage's snap lines are for. The margin from the
// display's edges is under 更多. While the panel is dragged or resized on the stage, the fields
// show where it is going. Every position keeps it on its display.
function PlaceFields({ instId, pk }: { instId: string; pk: string }) {
  const styles = useStyles();
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
    <>
      {instId === 'wallpaper' && (
        <div className={styles.place}>
          <ToggleButton size="small" className={styles.more} checked={full} onClick={() => (full ? setValues(instId, { [pk + '.anchor']: 'center', [pk + '.offset-x']: '0', [pk + '.offset-y']: '0' }) : setValue(instId, pk + '.anchor', 'full'))}>铺满画面</ToggleButton>
        </div>
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
      {!full && (
        <div className={styles.titleRow}>
          {margin && (
            <Button appearance="transparent" size="small" className={styles.more} icon={more ? <ChevronUpRegular /> : <ChevronDownRegular />} iconPosition="after"
              onClick={() => setMore(!more)}>更多</Button>
          )}
          <div style={{ flex: 1 }} />
          {modified && <Button appearance="subtle" size="small" icon={<ArrowResetRegular />} onClick={reset}>恢复位置</Button>}
        </div>
      )}
      {margin && !full && more && <AdjustmentField instId={instId} a={margin} onReset={() => clearValues(instId, [margin.key])} />}
    </>
  );
}
