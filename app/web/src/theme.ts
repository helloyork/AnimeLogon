// Fluent's components (menus, lists of options, tooltips, dialogs, toasts, the colour picker) are
// drawn with the page's own colours, corners and type: their tokens name the values in
// styles/tokens.css, so both follow the Windows theme together. Fluent's light or dark theme
// still gives the tokens the page has no value of its own for.
import { useSyncExternalStore } from 'react';
import { webDarkTheme, webLightTheme, type Theme } from '@fluentui/react-components';

const v = (name: string) => `var(--${name})`;

const page: Partial<Theme> = {
  fontFamilyBase: v('font'),
  fontFamilyMonospace: v('font-mono'),

  colorNeutralForeground1: v('text-1'),
  colorNeutralForeground1Hover: v('text-1'),
  colorNeutralForeground1Pressed: v('text-1'),
  colorNeutralForeground1Selected: v('text-1'),
  colorNeutralForeground2: v('text-2'),
  colorNeutralForeground2Hover: v('text-1'),
  colorNeutralForeground2Pressed: v('text-1'),
  colorNeutralForeground2Selected: v('text-1'),
  colorNeutralForeground3: v('text-3'),
  colorNeutralForeground3Hover: v('text-2'),
  colorNeutralForeground3Pressed: v('text-2'),
  colorNeutralForeground3Selected: v('text-2'),
  colorNeutralForeground4: v('text-3'),

  colorNeutralBackground1: v('surface-2'),
  colorNeutralBackground1Hover: v('surface-3'),
  colorNeutralBackground1Pressed: v('surface-hover'),
  colorNeutralBackground1Selected: v('surface-3'),
  colorNeutralBackground2: v('surface-1'),
  colorNeutralBackground3: v('surface-3'),
  colorNeutralBackground4: v('bg'),
  colorSubtleBackgroundHover: v('surface-3'),
  colorSubtleBackgroundPressed: v('surface-hover'),
  colorSubtleBackgroundSelected: v('surface-3'),

  colorNeutralStroke1: v('border-strong'),
  colorNeutralStroke1Hover: v('text-3'),
  colorNeutralStroke1Pressed: v('text-3'),
  colorNeutralStroke1Selected: v('border-strong'),
  colorNeutralStroke2: v('border'),
  colorNeutralStroke3: v('border'),
  colorNeutralStrokeAccessible: v('border-strong'),
  colorNeutralStrokeAccessibleHover: v('text-3'),
  colorNeutralStrokeAccessiblePressed: v('text-3'),
  colorStrokeFocus2: v('accent'),

  colorBrandBackground: v('accent'),
  colorBrandBackgroundHover: v('accent-hover'),
  colorBrandBackgroundPressed: v('accent-press'),
  colorBrandBackgroundSelected: v('accent'),
  colorCompoundBrandBackground: v('accent'),
  colorCompoundBrandBackgroundHover: v('accent-hover'),
  colorCompoundBrandBackgroundPressed: v('accent-press'),
  colorCompoundBrandStroke: v('accent'),
  colorCompoundBrandStrokeHover: v('accent-hover'),
  colorCompoundBrandStrokePressed: v('accent-press'),
  colorBrandStroke1: v('accent'),
  colorBrandForeground1: v('accent'),
  colorBrandForeground2: v('accent'),
  colorCompoundBrandForeground1: v('accent'),
  colorCompoundBrandForeground1Hover: v('accent-hover'),
  colorCompoundBrandForeground1Pressed: v('accent-press'),
  colorBrandForegroundLink: v('accent'),
  colorBrandForegroundLinkHover: v('accent-hover'),
  colorBrandForegroundLinkPressed: v('accent-press'),
  colorNeutralForegroundOnBrand: v('on-accent'),

  colorBackgroundOverlay: v('scrim'),

  borderRadiusSmall: v('r-xs'),
  borderRadiusMedium: v('r-sm'),
  borderRadiusLarge: v('r-md'),
  borderRadiusXLarge: v('r-lg'),

  shadow4: v('shadow-pop'),
  shadow8: v('shadow-pop'),
  shadow16: v('shadow-pop'),
  shadow28: v('shadow-pop'),
  shadow64: v('shadow-dialog'),
};

export const lightTheme: Theme = { ...webLightTheme, ...page };
export const darkTheme: Theme = { ...webDarkTheme, ...page };

const scheme = matchMedia('(prefers-color-scheme: dark)');
const subscribe = (f: () => void) => {
  scheme.addEventListener('change', f);
  return () => scheme.removeEventListener('change', f);
};
export const useDark = () => useSyncExternalStore(subscribe, () => scheme.matches);
