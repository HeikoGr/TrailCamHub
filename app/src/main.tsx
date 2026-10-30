import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { createApiClient } from './api';
import { App } from './App';
import { getWebApp, initTelegram } from './telegram';
import './styles.css';

const webApp = getWebApp();
initTelegram(webApp);

const root = document.getElementById('root');
if (!root) {
  throw new Error('Missing #root element');
}

createRoot(root).render(
  <StrictMode>
    <App api={createApiClient()} webApp={webApp} />
  </StrictMode>,
);
