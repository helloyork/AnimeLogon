// Fluent's web themes, light or dark as Windows is, with Windows' own fonts: Segoe UI Variable
// for Latin text and Microsoft YaHei UI for Chinese. Nothing else is changed (design-system §2).
import { useSyncExternalStore } from 'react';
import { webDarkTheme, webLightTheme, type Theme } from '@fluentui/react-components';

const fontFamilyBase = '"Segoe UI Variable Text", "Segoe UI", "Microsoft YaHei UI", system-ui, sans-serif';
export const lightTheme: Theme = { ...webLightTheme, fontFamilyBase };
export const darkTheme: Theme = { ...webDarkTheme, fontFamilyBase };

const scheme = matchMedia('(prefers-color-scheme: dark)');
const subscribe = (f: () => void) => {
  scheme.addEventListener('change', f);
  return () => scheme.removeEventListener('change', f);
};
export const useDark = () => useSyncExternalStore(subscribe, () => scheme.matches);
