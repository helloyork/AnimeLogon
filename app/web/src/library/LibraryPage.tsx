// 主题库: the theme in use, large, then every theme. A click on a theme puts it in use (D41).
import { useState, type CSSProperties } from 'react';
import {
  Button,
  Caption1,
  Card,
  cardCSSVars,
  CardPreview,
  Link,
  makeStyles,
  Menu,
  MenuDivider,
  MenuItem,
  MenuList,
  MenuPopover,
  MenuTrigger,
  MessageBar,
  MessageBarActions,
  MessageBarBody,
  mergeClasses,
  shorthands,
  Subtitle1,
  Subtitle2,
  Text,
  Title3,
  tokens,
} from '@fluentui/react-components';
import { MoreHorizontalRegular } from '@fluentui/react-icons';
import { UacShield } from '../icons';
import { SceneFrames, WallpaperVideo } from '../media';
import { calm } from '../motion';
import { layerUrl, send, thumbUrl } from '../native';
import { motion } from '../preview';
import {
  applyFlow,
  copyFlow,
  deleteFlow,
  kindName,
  openTheme,
  perMonitor,
  renameFlow,
  resetFlow,
  screenTheme,
  setState,
  setTab,
  shapeOf,
  themeOf,
  usedOn,
  useStore,
} from '../store';
import type { ThemeEntry } from '../types';

const useStyles = makeStyles({
  scroll: { flex: 1, minHeight: 0, overflowY: 'auto' },
  page: {
    display: 'flex',
    flexDirection: 'column',
    gap: tokens.spacingVerticalXXL,
    paddingInline: tokens.spacingHorizontalXXL,
    paddingTop: tokens.spacingVerticalS,
    paddingBottom: tokens.spacingVerticalXXXL,
    maxWidth: '1440px',
  },
  section: { display: 'flex', flexDirection: 'column', gap: tokens.spacingVerticalM },
  heading: { display: 'flex', alignItems: 'baseline', gap: tokens.spacingHorizontalS },
  quiet: { color: tokens.colorNeutralForeground3 },
  current: {
    display: 'grid',
    gridTemplateColumns: 'minmax(0, 480px) minmax(0, 1fr)',
    alignItems: 'center',
    gap: tokens.spacingHorizontalXXL,
    padding: tokens.spacingHorizontalL,
    backgroundColor: tokens.colorNeutralBackground1,
    border: `${tokens.strokeWidthThin} solid ${tokens.colorNeutralStroke2}`,
    borderRadius: tokens.borderRadiusXLarge,
    '@media (max-width: 760px)': { gridTemplateColumns: 'minmax(0, 1fr)' },
  },
  info: { display: 'flex', flexDirection: 'column', gap: tokens.spacingVerticalXS, minWidth: 0 },
  actions: { display: 'flex', gap: tokens.spacingHorizontalS, marginTop: tokens.spacingVerticalM, flexWrap: 'wrap' },
  screens: {
    display: 'grid',
    gridTemplateColumns: 'repeat(auto-fit, minmax(min(100%, 260px), 1fr))',
    gap: tokens.spacingHorizontalL,
    padding: tokens.spacingHorizontalL,
    backgroundColor: tokens.colorNeutralBackground1,
    border: `${tokens.strokeWidthThin} solid ${tokens.colorNeutralStroke2}`,
    borderRadius: tokens.borderRadiusXLarge,
  },
  screen: { display: 'flex', flexDirection: 'column', gap: tokens.spacingVerticalXS, margin: 0 },
  caption: { display: 'flex', justifyContent: 'space-between', gap: tokens.spacingHorizontalS },
  grid: {
    display: 'grid',
    gridTemplateColumns: 'repeat(auto-fill, minmax(min(100%, 220px), 1fr))',
    gap: `${tokens.spacingVerticalXL} ${tokens.spacingHorizontalL}`,
    alignItems: 'start',
  },
  picture: {
    position: 'relative',
    overflow: 'hidden',
    borderRadius: tokens.borderRadiusMedium,
    backgroundColor: '#000',
    aspectRatio: 'var(--arw) / var(--arh)',
  },
  shot: { position: 'absolute', inset: 0, width: '100%', height: '100%', objectFit: 'cover', display: 'block' },
  // A theme card is Fluent's selectable Card, drawn by its stroke like every card here: none of
  // Fluent's card shadows, and the card's own corners.
  card: {
    [cardCSSVars.cardBorderRadiusVar]: tokens.borderRadiusXLarge,
    boxShadow: 'none',
    ':hover': { boxShadow: 'none' },
    '::after': { ...shorthands.borderColor(tokens.colorNeutralStroke2) },
  },
  // Fluent marks a selected card with a neutral stroke, too faint to tell the theme in use at a
  // glance; it takes the brand colour, as the page's other selected items do.
  inUse: {
    '::after': { ...shorthands.borderColor(tokens.colorBrandStroke1), ...shorthands.borderWidth(tokens.strokeWidthThick) },
  },
  tile: {
    position: 'relative',
    overflow: 'hidden',
    backgroundColor: '#000',
    aspectRatio: 'var(--arw) / var(--arh)',
  },
  overlay: {
    position: 'absolute',
    right: tokens.spacingHorizontalS,
    bottom: tokens.spacingVerticalS,
    display: 'flex',
    gap: tokens.spacingHorizontalXS,
    opacity: 0,
    transitionProperty: 'opacity',
    transitionDuration: tokens.durationFaster,
  },
  overlayShown: { opacity: 1 },
  meta: { display: 'flex', flexDirection: 'column', minWidth: 0 },
  ellipsis: { overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' },
});

const shapeVars = (id: string) => {
  const s = shapeOf(id);
  return { '--arw': String(s.width), '--arh': String(s.height) } as CSSProperties;
};

export function LibraryPage() {
  const s = useStore();
  const styles = useStyles();
  const lib = s.lib!;
  return (
    <main className={styles.scroll}>
      <div className={styles.page}>
        <Title3 as="h1">主题库</Title3>
        {!s.env.installed && (
          <MessageBar intent="warning">
            <MessageBarBody>这台电脑上没有安装 AnimeLogon，改动无法保存。</MessageBarBody>
          </MessageBar>
        )}
        {s.env.installed && !s.env.on && (
          <MessageBar intent="info">
            <MessageBarBody>AnimeLogon 已关闭，锁屏显示 Windows 默认界面。</MessageBarBody>
            <MessageBarActions>
              <Button size="small" onClick={() => setTab('settings')}>打开设置</Button>
            </MessageBarActions>
          </MessageBar>
        )}
        <section className={styles.section}>
          <div className={styles.heading}>
            <Subtitle2 as="h2">当前主题</Subtitle2>
            {perMonitor() && <Caption1 className={styles.quiet}>每块屏幕各自使用主题</Caption1>}
          </div>
          {perMonitor() ? <Screens /> : <Current />}
        </section>
        <section className={styles.section}>
          <div className={styles.heading}>
            <Subtitle2 as="h2">我的主题</Subtitle2>
            <Caption1 className={styles.quiet}>{lib.themes.length} 个</Caption1>
          </div>
          <div className={styles.grid}>
            {lib.themes.map(t => <ThemeCard key={t.id} theme={t} />)}
          </div>
        </section>
      </div>
    </main>
  );
}

function Current() {
  const s = useStore();
  const styles = useStyles();
  const t = themeOf(s.lib!.current);
  if (!t) return null;
  const wallpaper = ['image', 'video'].includes(t.wallpaperKind) ? `${t.wallpaper}（${kindName(t.wallpaperKind)}）` : t.wallpaper;
  const showComponents = s.lib!.settings.components;
  return (
    <div className={styles.current}>
      <div className={styles.picture} style={shapeVars(t.id)}>
        <img className={styles.shot} src={thumbUrl(t.id, t.revision, 960, s.tick)} alt="" draggable={false} />
        {t.wallpaperKind === 'video' && motion() && <WallpaperVideo key={t.wallpaperRef} wallpaper={t.wallpaperRef} fit={t.fit} />}
        {t.wallpaperKind === 'scene' && motion() && !t.sceneStatic && <SceneFrames theme={t.id} width={960} moving />}
        {(t.wallpaperKind === 'video' || (t.wallpaperKind === 'scene' && !t.sceneStatic)) && motion() && showComponents && (
          <img className={styles.shot} src={layerUrl(t.id, t.revision, 960, s.tick)} alt="" draggable={false} />
        )}
      </div>
      <div className={styles.info}>
        <Subtitle1 as="h3">{t.name}</Subtitle1>
        <Caption1 className={styles.quiet}>{`${wallpaper} · ${showComponents ? t.summary : '组件已关闭'}`}</Caption1>
        <div className={styles.actions}>
          <Button onClick={() => openTheme(t.id)}>自定义</Button>
          <Button onClick={() => setState({ lock: { theme: t.id } })}>锁屏预览</Button>
        </div>
      </div>
    </div>
  );
}

function Screens() {
  const s = useStore();
  const styles = useStyles();
  const showOn = (primary: boolean) => s.lib!.settings.componentDisplays === 'all' || primary;
  return (
    <div className={styles.screens}>
      {s.env.monitors.map(m => {
        const t = themeOf(screenTheme(m.key));
        if (!t) return null;
        return (
          <figure key={m.key} className={styles.screen}>
            <div className={styles.picture} style={{ '--arw': String(m.width), '--arh': String(m.height) } as CSSProperties}>
              <img className={styles.shot} src={thumbUrl(t.id, t.revision, 640, s.tick)} alt="" draggable={false} />
            </div>
            <figcaption className={styles.caption}>
              <Caption1 className={styles.quiet}>{m.name + (m.primary ? '（主）' : '') + (showOn(m.primary) ? '' : ' · 不显示组件')}</Caption1>
              <Link onClick={() => openTheme(t.id)}>{t.name}</Link>
            </figcaption>
          </figure>
        );
      })}
    </div>
  );
}

export function ThemeMenuItems({ theme, onPage }: { theme: ThemeEntry; onPage?: boolean }) {
  return (
    <MenuList>
      {!onPage && <MenuItem onClick={() => openTheme(theme.id)}>自定义</MenuItem>}
      {!onPage && <MenuItem onClick={() => setState({ lock: { theme: theme.id } })}>锁屏预览</MenuItem>}
      {!onPage && <MenuDivider />}
      <MenuItem icon={<UacShield />} onClick={() => copyFlow(theme.id)}>复制…</MenuItem>
      {!theme.builtIn && <MenuItem icon={<UacShield />} onClick={() => renameFlow(theme.id)}>重命名…</MenuItem>}
      <MenuItem onClick={() => send('export', { theme: theme.id })}>导出主题包…</MenuItem>
      {theme.modifications > 0 && <MenuItem onClick={() => resetFlow(theme.id)}>{`恢复默认（${theme.modifications} 项修改）`}</MenuItem>}
      {!theme.builtIn && <MenuDivider />}
      {!theme.builtIn && <MenuItem icon={<UacShield />} onClick={() => deleteFlow(theme.id)}>删除…</MenuItem>}
    </MenuList>
  );
}

function ThemeCard({ theme: t }: { theme: ThemeEntry }) {
  const s = useStore();
  const styles = useStyles();
  const [hover, setHover] = useState(false);
  const [menuOpen, setMenuOpen] = useState(false);
  const used = usedOn(t.id);
  const moves = hover && motion() && (t.wallpaperKind === 'video' || (t.wallpaperKind === 'scene' && !t.sceneStatic));
  const facts = [kindName(t.wallpaperKind), t.summary, t.modifications ? '已修改' : null, t.author && !t.builtIn ? t.author : null].filter(Boolean).join(' · ');
  // Selecting the card puts the theme in use; it stays selected while the theme is in use, and a
  // click on the buttons over its picture does not select it (Card leaves focusable children alone).
  return (
    <article data-theme={t.id} onPointerEnter={() => setHover(true)} onPointerLeave={() => setHover(false)}>
      <Card
        className={mergeClasses(styles.card, used.length > 0 && styles.inUse)}
        selected={used.length > 0}
        onSelectionChange={e => {
          // Space on the card's checkbox reaches Card twice, as the checkbox's change and as the
          // same click bubbling to the card; only the change is acted on.
          if (e.type === 'click' && e.target instanceof HTMLInputElement) return;
          applyFlow(t.id);
        }}
        checkbox={{ 'aria-label': `套用「${t.name}」` }}
      >
        <CardPreview>
          <div className={styles.tile} style={shapeVars(t.id)}>
            <img className={styles.shot} src={thumbUrl(t.id, t.revision, 480, s.tick)} alt="" draggable={false} />
            {moves && t.wallpaperKind === 'video' && <WallpaperVideo wallpaper={t.wallpaperRef} fit={t.fit} />}
            {moves && t.wallpaperKind === 'scene' && <SceneFrames theme={t.id} width={480} moving />}
            {moves && s.lib!.settings.components && <img className={styles.shot} src={layerUrl(t.id, t.revision, 480, s.tick)} alt="" draggable={false} />}
            <div className={mergeClasses(styles.overlay, (hover || menuOpen || calm()) && styles.overlayShown)} onClick={e => e.stopPropagation()}>
              <Button size="small" onClick={() => openTheme(t.id)}>自定义</Button>
              <Menu open={menuOpen} onOpenChange={(_, d) => setMenuOpen(d.open)}>
                <MenuTrigger disableButtonEnhancement>
                  <Button size="small" icon={<MoreHorizontalRegular />} aria-label={`「${t.name}」的更多操作`} />
                </MenuTrigger>
                <MenuPopover>
                  <ThemeMenuItems theme={t} />
                </MenuPopover>
              </Menu>
            </div>
          </div>
        </CardPreview>
        <div className={styles.meta}>
          <Text className={styles.ellipsis}>{t.name}</Text>
          {/* A card colours its Text with currentColor, so the quiet colour is set on a wrapper. */}
          <div className={mergeClasses(styles.quiet, styles.ellipsis)}>
            <Caption1>{(used.length ? used.join('、') + ' · ' : '') + facts}</Caption1>
          </div>
        </div>
      </Card>
    </article>
  );
}
