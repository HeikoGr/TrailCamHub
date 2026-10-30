import { useCallback, useEffect, useState } from 'react';
import type { ApiClient } from './api';
import type { Camera, MediaItem } from './api/types';
import { MediaDetail } from './components/MediaDetail';
import { MediaGrid } from './components/MediaGrid';
import { StatusCard } from './components/StatusCard';
import { mediaKey } from './format';
import { getUser, hapticSuccess, isInTelegram, notify, type TelegramWebApp } from './telegram';

interface Props {
  api: ApiClient;
  webApp?: TelegramWebApp;
  now?: () => number;
}

type Load =
  | { status: 'loading' }
  | { status: 'error'; message: string }
  | { status: 'ready'; camera: Camera | undefined; media: MediaItem[] };

const unixNow = () => Math.floor(Date.now() / 1000);

export function App({ api, webApp, now = unixNow }: Props) {
  const [load, setLoad] = useState<Load>({ status: 'loading' });
  const [selectedKey, setSelectedKey] = useState<string>();
  const [busy, setBusy] = useState(false);
  const inTelegram = isInTelegram(webApp);
  const user = getUser(webApp);

  const refresh = useCallback(async () => {
    setLoad({ status: 'loading' });
    try {
      const cameras = await api.listCameras();
      const camera = cameras[0];
      const media = camera ? await api.listMedia(camera.camId) : [];
      setLoad({ status: 'ready', camera, media });
    } catch (error) {
      setLoad({ status: 'error', message: error instanceof Error ? error.message : String(error) });
    }
  }, [api]);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  const goBack = useCallback(() => setSelectedKey(undefined), []);

  // Inside Telegram, use the native back button in the header.
  useEffect(() => {
    if (!inTelegram || !webApp || selectedKey === undefined) {
      return;
    }
    webApp.BackButton.show();
    webApp.BackButton.onClick(goBack);
    return () => {
      webApp.BackButton.offClick(goBack);
      webApp.BackButton.hide();
    };
  }, [inTelegram, webApp, selectedKey, goBack]);

  const requestVideo = async (item: MediaItem) => {
    if (load.status !== 'ready' || !load.camera) return;
    setBusy(true);
    try {
      await api.requestVideo(load.camera.camId, item);
      setLoad({
        ...load,
        media: load.media.map((m) =>
          mediaKey(m) === mediaKey(item) ? { ...m, videoState: 'requested' } : m,
        ),
      });
      hapticSuccess(webApp);
    } catch (error) {
      notify(`Request failed: ${error instanceof Error ? error.message : String(error)}`, webApp);
    } finally {
      setBusy(false);
    }
  };

  const selected =
    load.status === 'ready' ? load.media.find((m) => mediaKey(m) === selectedKey) : undefined;

  return (
    <div className="app">
      <header className="topbar">
        <h1>TrailCamHub</h1>
        {user && <p className="hint">Hi {user.first_name}</p>}
      </header>

      {api.isDemo && (
        <p className="banner" role="note">
          Demo mode: showing sample data, no backend connected.
        </p>
      )}

      {load.status === 'loading' && <p className="hint">Loading…</p>}

      {load.status === 'error' && (
        <div className="card error" role="alert">
          <p>Could not load data: {load.message}</p>
          <button type="button" className="primary" onClick={() => void refresh()}>
            Try again
          </button>
        </div>
      )}

      {load.status === 'ready' && !load.camera && (
        <p className="hint empty">
          No camera linked yet. Scan the QR code on your TrailCamHub to add one.
        </p>
      )}

      {load.status === 'ready' && load.camera && selected && (
        <MediaDetail
          item={selected}
          busy={busy}
          showBackLink={!inTelegram}
          onBack={goBack}
          onRequestVideo={(item) => void requestVideo(item)}
        />
      )}

      {load.status === 'ready' && load.camera && !selected && (
        <>
          <StatusCard camera={load.camera} now={now()} />
          <h2 className="section-title">Recordings</h2>
          <MediaGrid items={load.media} onSelect={(item) => setSelectedKey(mediaKey(item))} />
        </>
      )}

      <footer className="hint footer">
        {import.meta.env.VITE_APP_VERSION
          ? `Version ${import.meta.env.VITE_APP_VERSION}`
          : 'Development build'}
      </footer>
    </div>
  );
}
