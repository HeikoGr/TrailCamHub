import { describe, expect, it } from 'vitest';
import { buildDemoMedia, DemoApiClient } from './demo';

describe('buildDemoMedia', () => {
  it('returns photo/video pairs, newest first', () => {
    const media = buildDemoMedia(1_000_000, 4);
    expect(media.map((m) => m.fileType)).toEqual([0, 2, 0, 2]);
    expect(media.map((m) => m.num)).toEqual([36, 35, 34, 33]);
    const times = media.map((m) => m.mediaTime);
    expect([...times].sort((a, b) => b - a)).toEqual(times);
  });

  it('uses inline SVG thumbnails', () => {
    expect(buildDemoMedia(0, 1)[0]?.thumbUrl).toMatch(/^data:image\/svg\+xml/);
  });
});

describe('DemoApiClient', () => {
  const client = () => new DemoApiClient(() => 1_000_000, 0);

  it('lists one demo camera', async () => {
    const cameras = await client().listCameras();
    expect(cameras).toHaveLength(1);
    expect(cameras[0]?.model).toBe('TC100');
  });

  it('marks a requested video and keeps it after reloading the list', async () => {
    const api = client();
    const [camera] = await api.listCameras();
    const camId = camera?.camId ?? '';
    const video = (await api.listMedia(camId)).find(
      (m) => m.fileType === 2 && m.videoState === 'none',
    );
    expect(video).toBeDefined();

    const result = await api.requestVideo(camId, video!);

    expect(result).toMatchObject({ type: 'requestVideo', state: 'queued' });
    const reloaded = (await api.listMedia(camId)).find((m) => m.num === video!.num);
    expect(reloaded?.videoState).toBe('requested');
  });

  it('rejects unknown media', async () => {
    await expect(client().requestVideo('nope', { dir: 1, num: 1 })).rejects.toThrow(
      'Unknown media',
    );
  });
});
