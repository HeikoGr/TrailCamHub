import { useCallback, useEffect, useRef, useState } from 'react';
import type { ApiClient } from './api';
import type { Camera, MediaItem } from './api/types';
import { CameraPicker } from './components/CameraPicker';
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
  | { status: 'ready'; cameras: Camera[] };

type MediaLoad =
  | { status: 'idle' | 'loading' }
  | { status: 'error'; message: string }
  | { status: 'ready'; camId: string; items: MediaItem[] };

const unixNow = () => Math.floor(Date.now() / 1000);
const CAMERA_KEY = 'trailcamhub.camera';

// The last viewed camera is a per-device convenience; storage may be blocked.
function rememberedCamera(): string | undefined {
  try {
    return window.localStorage.getItem(CAMERA_KEY) ?? undefined;
  } catch {
    return undefined;
  }
}

function rememberCamera(camId: string): void {
  try {
    window.localStorage.setItem(CAMERA_KEY, camId);
  } catch {
    // Not important enough to report.
  }
}

const message = (error: unknown) => (error instanceof Error ? error.message : String(error));

export function App({ api, webApp, now = unixNow }: Props) {
  const [load, setLoad] = useState<Load>({ status: 'loading' });
  const [camId, setCamId] = useState<string>();
  const [media, setMedia] = useState<MediaLoad>({ status: 'idle' });
  const [selectedKey, setSelectedKey] = useState<string>();
  const [busy, setBusy] = useState(false);
  // Ignores answers for a camera the user has already switched away from.
  const mediaRequest = useRef(0);
  const inTelegram = isInTelegram(webApp);
  const user = getUser(webApp);

  const loadMedia = useCallback(
    async (id: string) => {
      const request = ++mediaRequest.current;
      setMedia({ status: 'loading' });
      try {
        const items = await api.listMedia(id);
        if (request === mediaRequest.current) setMedia({ status: 'ready', camId: id, items });
      } catch (error) {
        if (request === mediaRequest.current)
          setMedia({ status: 'error', message: message(error) });
      }
    },
    [api],
  );

  const refresh = useCallback(async () => {
    setLoad({ status: 'loading' });
    try {
      const cameras = await api.listCameras();
      setLoad({ status: 'ready', cameras });
      const remembered = rememberedCamera();
      const initial = cameras.find((c) => c.camId === remembered) ?? cameras[0];
      setCamId(initial?.camId);
      if (initial) await loadMedia(initial.camId);
    } catch (error) {
      setLoad({ status: 'error', message: message(error) });
    }
  }, [api, loadMedia]);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  const selectCamera = (id: string) => {
    if (id === camId) return;
    setCamId(id);
    setSelectedKey(undefined);
    rememberCamera(id);
    void loadMedia(id);
  };

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

  const camera = load.status === 'ready' ? load.cameras.find((c) => c.camId === camId) : undefined;
  const items = media.status === 'ready' && media.camId === camId ? media.items : undefined;
  const selected = items?.find((m) => mediaKey(m) === selectedKey);

  const requestVideo = async (item: MediaItem) => {
    if (!camera || media.status !== 'ready') return;
    const target = camera.camId;
    setBusy(true);
    try {
      await api.requestVideo(target, item);
      setMedia((current) =>
        current.status === 'ready' && current.camId === target
          ? {
              ...current,
              items: current.items.map((m) =>
                mediaKey(m) === mediaKey(item) ? { ...m, videoState: 'requested' } : m,
              ),
            }
          : current,
      );
      hapticSuccess(webApp);
    } catch (error) {
      notify(`Request failed: ${message(error)}`, webApp);
    } finally {
      setBusy(false);
    }
  };

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

      {load.status === 'ready' && load.cameras.length === 0 && (
        <p className="hint empty">
          No camera yet. Cameras appear here after your TrailCamHub has read them for the first
          time.
        </p>
      )}

      {camera && selected && (
        <MediaDetail
          item={selected}
          busy={busy}
          showBackLink={!inTelegram}
          onBack={goBack}
          onRequestVideo={(item) => void requestVideo(item)}
        />
      )}

      {load.status === 'ready' && camera && !selected && (
        <>
          <CameraPicker cameras={load.cameras} selected={camera.camId} onSelect={selectCamera} />
          <StatusCard camera={camera} now={now()} />
          <h2 className="section-title">Recordings</h2>
          {media.status === 'loading' && <p className="hint">Loading recordings…</p>}
          {media.status === 'error' && (
            <div className="card error" role="alert">
              <p>Could not load recordings: {media.message}</p>
              <button
                type="button"
                className="primary"
                onClick={() => void loadMedia(camera.camId)}
              >
                Try again
              </button>
            </div>
          )}
          {items && <MediaGrid items={items} onSelect={(item) => setSelectedKey(mediaKey(item))} />}
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
