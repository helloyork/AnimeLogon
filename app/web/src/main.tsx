// The settings app's page: what animelogon.exe shows in its window. The native side holds the
// themes and the settings and draws every picture of a lock screen; this page lays them out,
// sends what the person does, and draws its marks over the pictures.
import { createRoot } from 'react-dom/client';
import { App } from './App';
import { exposeForTests } from './compat';
import { start } from './store';

createRoot(document.getElementById('app')!).render(<App />);
exposeForTests();
start();
