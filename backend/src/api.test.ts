import { describe, expect, it } from 'vitest';
import { handleRequest } from './api';
import type { ApiResponse } from './http';
import type { CheckinResponse } from './model';
import { appRequest, CAM_ID, deviceRequest, makeInitData, NOW, setup } from './test-utils';

const body = <T>(response: ApiResponse) => JSON.parse(response.body) as T;

const MEDIA = [
  { dir: 100, num: 35, fileType: 2, mediaTime: NOW - 600 },
  { dir: 100, num: 36, fileType: 0, mediaTime: NOW - 300 },
];

const checkinBody = {
  hubFirmware: '0.4.0',
  hubBatPercent: 91,
  camera: { model: 'TC100', firmware: '2.3.0.3', batPercent: 78, sdFreeMB: 28412 },
  media: MEDIA,
};

describe('routing', () => {
  it('answers the health check without auth', async () => {
    const { deps } = await setup();
    const res = await handleRequest({ method: 'GET', path: '/v1/health', headers: {} }, deps);
    expect(res.statusCode).toBe(200);
  });

  it('returns 404 for unknown paths and 405 for wrong methods', async () => {
    const { deps } = await setup();
    expect((await handleRequest(appRequest('GET', '/v2/x'), deps)).statusCode).toBe(404);
    expect((await handleRequest(appRequest('DELETE', '/v1/cameras'), deps)).statusCode).toBe(405);
  });
});

describe('app API', () => {
  it('requires valid Telegram init data', async () => {
    const { deps } = await setup();
    const noAuth = await handleRequest({ method: 'GET', path: '/v1/cameras', headers: {} }, deps);
    expect(noAuth.statusCode).toBe(401);
    const forged = await handleRequest(
      appRequest('GET', '/v1/cameras', undefined, makeInitData(1, NOW, 'wrong:token')),
      deps,
    );
    expect(forged.statusCode).toBe(401);
  });

  it('lists only the cameras linked to the user', async () => {
    const { deps } = await setup();
    const mine = await handleRequest(appRequest('GET', '/v1/cameras'), deps);
    expect(body<Array<{ camId: string; name: string }>>(mine)).toEqual([
      expect.objectContaining({ camId: CAM_ID, name: 'Garden' }),
    ]);

    const other = await handleRequest(
      appRequest('GET', '/v1/cameras', undefined, makeInitData(999)),
      deps,
    );
    expect(body(other)).toEqual([]);
  });

  it("hides other users' cameras behind 404", async () => {
    const { deps } = await setup();
    const res = await handleRequest(
      appRequest('GET', `/v1/cameras/${CAM_ID}/media`, undefined, makeInitData(999)),
      deps,
    );
    expect(res.statusCode).toBe(404);
  });
});

describe('device check-in → thumbnails → app', () => {
  it('rejects an invalid device key', async () => {
    const { deps } = await setup();
    const res = await handleRequest(
      deviceRequest('/v1/device/checkin', `${CAM_ID}.${'x'.repeat(43)}`, checkinBody),
      deps,
    );
    expect(res.statusCode).toBe(401);
  });

  it('stores telemetry and asks for missing thumbnails, newest first', async () => {
    const { deps, deviceKey, store } = await setup();
    const res = await handleRequest(
      deviceRequest('/v1/device/checkin', deviceKey, checkinBody),
      deps,
    );

    expect(res.statusCode).toBe(200);
    const result = body<CheckinResponse>(res);
    expect(result.thumbUploads.map((t) => `${t.dir}_${t.num}`)).toEqual(['100_36', '100_35']);
    expect(result.thumbUploads[0]?.url).toContain('/put/');
    expect(result.config.sleepIntervalS).toBe(1800);

    const camera = await store.getCamera(CAM_ID);
    expect(camera).toMatchObject({ batPercent: 78, hubBatPercent: 91, lastSeen: NOW });
  });

  it('shows thumbnails in the app only after S3 confirms the upload', async () => {
    const { deps, deviceKey, storage } = await setup();
    await handleRequest(deviceRequest('/v1/device/checkin', deviceKey, checkinBody), deps);

    storage.uploaded.add(`${CAM_ID}/100_36`); // 100_35 "failed" to upload
    const ack = await handleRequest(
      deviceRequest('/v1/device/ack', deviceKey, {
        uploadedThumbs: [
          { dir: 100, num: 36 },
          { dir: 100, num: 35 },
        ],
      }),
      deps,
    );
    expect(body(ack)).toEqual({ thumbsConfirmed: 1, commandsUpdated: 0 });

    const media = await handleRequest(appRequest('GET', `/v1/cameras/${CAM_ID}/media`), deps);
    expect(body<Array<{ num: number; thumbUrl: string }>>(media)).toEqual([
      expect.objectContaining({ num: 36, thumbUrl: expect.stringContaining('/get/') }),
    ]);

    // The missing one is requested again at the next check-in.
    const again = await handleRequest(
      deviceRequest('/v1/device/checkin', deviceKey, checkinBody),
      deps,
    );
    expect(body<CheckinResponse>(again).thumbUploads.map((t) => t.num)).toEqual([35]);
  });
});

describe('video requests (app → device → app)', () => {
  async function withMedia() {
    const ctx = await setup();
    await handleRequest(deviceRequest('/v1/device/checkin', ctx.deviceKey, checkinBody), ctx.deps);
    return ctx;
  }

  it('validates the command', async () => {
    const { deps } = await withMedia();
    const path = `/v1/cameras/${CAM_ID}/commands`;
    const bad = (b: unknown) => handleRequest(appRequest('POST', path, b), deps);
    expect((await bad({ type: 'format' })).statusCode).toBe(400);
    expect((await bad({ type: 'requestVideo', dir: 100 })).statusCode).toBe(400);
    expect((await bad({ type: 'requestVideo', dir: 100, num: 99 })).statusCode).toBe(404);
    expect((await bad({ type: 'requestVideo', dir: 100, num: 36 })).statusCode).toBe(409);
  });

  it('runs the full round trip and is idempotent while pending', async () => {
    const { deps, deviceKey, store } = await withMedia();
    const path = `/v1/cameras/${CAM_ID}/commands`;
    const request = { type: 'requestVideo', dir: 100, num: 35 };

    const created = await handleRequest(appRequest('POST', path, request), deps);
    expect(created.statusCode).toBe(201);
    expect(body(created)).toEqual({ id: 'cmd-1', type: 'requestVideo', state: 'queued' });
    expect((await store.getMedia(CAM_ID, 100, 35))?.videoState).toBe('requested');

    const repeated = await handleRequest(appRequest('POST', path, request), deps);
    expect(body(repeated)).toMatchObject({ id: 'cmd-1' });

    // Delivered at check-in, and re-sent until acknowledged.
    for (let i = 0; i < 2; i++) {
      const checkin = await handleRequest(
        deviceRequest('/v1/device/checkin', deviceKey, checkinBody),
        deps,
      );
      expect(body<CheckinResponse>(checkin).commands).toEqual([
        { id: 'cmd-1', type: 'requestVideo', dir: 100, num: 35 },
      ]);
    }
    expect((await store.getMedia(CAM_ID, 100, 35))?.videoState).toBe('downloading');

    await handleRequest(
      deviceRequest('/v1/device/ack', deviceKey, { commands: [{ id: 'cmd-1', state: 'done' }] }),
      deps,
    );
    expect((await store.getCommand(CAM_ID, 'cmd-1'))?.state).toBe('done');
    expect((await store.getMedia(CAM_ID, 100, 35))?.videoState).toBe('available');

    const after = await handleRequest(
      deviceRequest('/v1/device/checkin', deviceKey, checkinBody),
      deps,
    );
    expect(body<CheckinResponse>(after).commands).toEqual([]);
  });

  it('marks the video as failed when the device reports an error', async () => {
    const { deps, deviceKey, store } = await withMedia();
    await handleRequest(
      appRequest('POST', `/v1/cameras/${CAM_ID}/commands`, {
        type: 'requestVideo',
        dir: 100,
        num: 35,
      }),
      deps,
    );
    await handleRequest(
      deviceRequest('/v1/device/ack', deviceKey, {
        commands: [{ id: 'cmd-1', state: 'failed', error: 'camera offline' }],
      }),
      deps,
    );
    expect(await store.getCommand(CAM_ID, 'cmd-1')).toMatchObject({
      state: 'failed',
      error: 'camera offline',
    });
    expect((await store.getMedia(CAM_ID, 100, 35))?.videoState).toBe('failed');
  });
});

describe('input validation', () => {
  it('rejects invalid media entries and bad JSON', async () => {
    const { deps, deviceKey } = await setup();
    const invalid = await handleRequest(
      deviceRequest('/v1/device/checkin', deviceKey, { media: [{ dir: 'x' }] }),
      deps,
    );
    expect(invalid.statusCode).toBe(400);

    const badJson = await handleRequest(
      {
        method: 'POST',
        path: '/v1/device/checkin',
        headers: { authorization: `Bearer ${deviceKey}` },
        body: '{',
      },
      deps,
    );
    expect(badJson.statusCode).toBe(400);
  });
});
