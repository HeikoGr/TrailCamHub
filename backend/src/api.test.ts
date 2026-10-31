import { describe, expect, it } from 'vitest';
import { handleRequest } from './api';
import type { ApiResponse } from './http';
import type { CameraCheckin, CheckinRequest, CheckinResponse } from './model';
import {
  appRequest,
  CAM_2,
  CAM_ID,
  hubRequest,
  makeInitData,
  NOW,
  setup,
  USER_ID,
} from './test-utils';

const body = <T>(response: ApiResponse) => JSON.parse(response.body) as T;

const garden = {
  camId: CAM_ID,
  model: 'TC100',
  firmware: '2.3.0.3',
  batPercent: 78,
  sdFreeMB: 28412,
  readAt: NOW - 120,
  media: [
    { dir: 100, num: 35, fileType: 2, mediaTime: NOW - 600 },
    { dir: 100, num: 36, fileType: 0, mediaTime: NOW - 300 },
  ],
};

const meadow = {
  camId: CAM_2,
  batPercent: 40,
  media: [{ dir: 100, num: 7, fileType: 0, mediaTime: NOW - 100 }],
};

const checkinBody: CheckinRequest = {
  hubFirmware: '0.5.0',
  hubBatPercent: 91,
  cameras: [garden, meadow],
};

function camera(response: ApiResponse, camId: string): CameraCheckin | undefined {
  return body<CheckinResponse>(response).cameras.find((c) => c.camId === camId);
}

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

describe('hub check-in with several cameras', () => {
  it('rejects an invalid hub key', async () => {
    const { deps } = await setup();
    const res = await handleRequest(
      hubRequest('/v1/device/checkin', `garden-hub.${'x'.repeat(43)}`, checkinBody),
      deps,
    );
    expect(res.statusCode).toBe(401);
  });

  it('creates both cameras, stores telemetry and asks for thumbnails newest first', async () => {
    const { deps, hubKey, store } = await setup();
    const res = await handleRequest(hubRequest('/v1/device/checkin', hubKey, checkinBody), deps);

    expect(res.statusCode).toBe(200);
    const result = body<CheckinResponse>(res);
    expect(result.config.sleepIntervalS).toBe(1800);
    expect(result.cameras.map((c) => c.camId).sort()).toEqual([CAM_2, CAM_ID].sort());
    // New cameras: the hub should send the full media list next time.
    expect(camera(res, CAM_ID)).toMatchObject({ accepted: true, resync: true });

    expect(camera(res, CAM_2)?.thumbUploads.map((t) => t.num)).toEqual([7]);
    expect(camera(res, CAM_ID)?.thumbUploads.map((t) => t.num)).toEqual([36, 35]);
    expect(camera(res, CAM_2)?.thumbUploads[0]?.url).toContain(`/put/${CAM_2}/`);

    expect(await store.getCamera(CAM_ID)).toMatchObject({
      hubId: 'garden-hub',
      batPercent: 78,
      model: 'TC100',
      lastSeen: NOW - 120,
    });
    expect((await store.getCamera(CAM_2))?.lastSeen).toBe(NOW);
    expect(await store.getHub('garden-hub')).toMatchObject({
      firmware: '0.5.0',
      batPercent: 91,
      lastSeen: NOW,
    });
  });

  it('caps thumbnail uploads over all cameras', async () => {
    const { deps, hubKey } = await setup();
    deps.config = { maxThumbUploadsPerCheckin: 2 };
    const res = await handleRequest(hubRequest('/v1/device/checkin', hubKey, checkinBody), deps);
    const all = body<CheckinResponse>(res).cameras.flatMap((c) =>
      c.thumbUploads.map((t) => `${c.camId}:${t.num}`),
    );
    // The two newest recordings, whichever camera they belong to.
    expect(all.sort()).toEqual([`${CAM_2}:7`, `${CAM_ID}:36`].sort());
  });

  it('accepts deltas: known media keep their state, no resync for known cameras', async () => {
    const { deps, hubKey, storage } = await setup();
    await handleRequest(hubRequest('/v1/device/checkin', hubKey, checkinBody), deps);
    storage.uploaded.add(`${CAM_ID}/100_36`);
    await handleRequest(
      hubRequest('/v1/device/ack', hubKey, {
        cameras: [{ camId: CAM_ID, uploadedThumbs: [{ dir: 100, num: 36 }] }],
      }),
      deps,
    );

    // Second check-in reports only the new recording of the first camera.
    const res = await handleRequest(
      hubRequest('/v1/device/checkin', hubKey, {
        cameras: [
          { camId: CAM_ID, media: [{ dir: 100, num: 37, fileType: 0, mediaTime: NOW - 10 }] },
        ],
      }),
      deps,
    );
    expect(camera(res, CAM_ID)).toMatchObject({ accepted: true, resync: false });
    // 37 is new, 35 is still missing, 36 is done.
    expect(camera(res, CAM_ID)?.thumbUploads.map((t) => t.num)).toEqual([37, 35]);
    // The second camera was not read, but its missing thumbnail is still asked for.
    expect(camera(res, CAM_2)?.thumbUploads.map((t) => t.num)).toEqual([7]);
  });

  it('keeps a camera with the hub that reported it first', async () => {
    const { deps, hubKey, otherHubKey, store } = await setup();
    await handleRequest(hubRequest('/v1/device/checkin', hubKey, checkinBody), deps);

    const res = await handleRequest(
      hubRequest('/v1/device/checkin', otherHubKey, {
        cameras: [{ camId: CAM_ID, batPercent: 1, media: [] }],
      }),
      deps,
    );
    expect(camera(res, CAM_ID)).toMatchObject({ accepted: false, thumbUploads: [] });
    expect(await store.getCamera(CAM_ID)).toMatchObject({ hubId: 'garden-hub', batPercent: 78 });

    // Its acknowledgements for that camera are ignored as well.
    const ack = await handleRequest(
      hubRequest('/v1/device/ack', otherHubKey, {
        cameras: [{ camId: CAM_ID, uploadedThumbs: [{ dir: 100, num: 36 }] }],
      }),
      deps,
    );
    expect(body(ack)).toEqual({ thumbsConfirmed: 0, commandsUpdated: 0 });
  });

  it('rejects invalid requests before storing anything', async () => {
    const { deps, hubKey, store } = await setup();
    const bad = (cameras: unknown) =>
      handleRequest(hubRequest('/v1/device/checkin', hubKey, { cameras }), deps);

    expect((await bad([{ camId: '../x' }])).statusCode).toBe(400);
    expect((await bad([{ camId: 'A' }, { camId: 'A' }])).statusCode).toBe(400);
    expect((await bad([garden, { camId: CAM_2, media: [{ dir: 'x' }] }])).statusCode).toBe(400);
    expect((await bad('nope')).statusCode).toBe(400);
    expect(await store.getCamera(CAM_ID)).toBeUndefined();

    const badJson = await handleRequest(
      {
        method: 'POST',
        path: '/v1/device/checkin',
        headers: { authorization: `Bearer ${hubKey}` },
        body: '{',
      },
      deps,
    );
    expect(badJson.statusCode).toBe(400);
  });

  it('ignores implausible readAt values', async () => {
    const { deps, hubKey, store } = await setup();
    await handleRequest(
      hubRequest('/v1/device/checkin', hubKey, {
        cameras: [{ camId: CAM_ID, readAt: NOW + 3600 }],
      }),
      deps,
    );
    expect((await store.getCamera(CAM_ID))?.lastSeen).toBe(NOW);
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

  it('lists all cameras of the linked hubs with hub details', async () => {
    const { deps, hubKey } = await setup();
    await handleRequest(hubRequest('/v1/device/checkin', hubKey, checkinBody), deps);

    const mine = body<Array<Record<string, unknown>>>(
      await handleRequest(appRequest('GET', '/v1/cameras'), deps),
    );
    expect(mine.map((c) => c.camId)).toEqual([CAM_2, CAM_ID]); // most recently read first
    expect(mine[0]).toMatchObject({
      hubId: 'garden-hub',
      hubName: 'Garden hub',
      hubBatPercent: 91,
      batPercent: 40,
    });

    const other = await handleRequest(
      appRequest('GET', '/v1/cameras', undefined, makeInitData(999)),
      deps,
    );
    expect(body(other)).toEqual([]);
  });

  it('shows thumbnails only after S3 confirms the upload', async () => {
    const { deps, hubKey, storage } = await setup();
    await handleRequest(hubRequest('/v1/device/checkin', hubKey, checkinBody), deps);
    storage.uploaded.add(`${CAM_ID}/100_36`); // 100_35 "failed" to upload
    const ack = await handleRequest(
      hubRequest('/v1/device/ack', hubKey, {
        cameras: [
          {
            camId: CAM_ID,
            uploadedThumbs: [
              { dir: 100, num: 36 },
              { dir: 100, num: 35 },
            ],
          },
        ],
      }),
      deps,
    );
    expect(body(ack)).toEqual({ thumbsConfirmed: 1, commandsUpdated: 0 });

    const media = await handleRequest(appRequest('GET', `/v1/cameras/${CAM_ID}/media`), deps);
    expect(body<Array<{ num: number; thumbUrl: string }>>(media)).toEqual([
      expect.objectContaining({ num: 36, thumbUrl: expect.stringContaining('/get/') }),
    ]);
  });

  it("hides other users' cameras behind 404", async () => {
    const { deps, hubKey } = await setup();
    await handleRequest(hubRequest('/v1/device/checkin', hubKey, checkinBody), deps);
    const res = await handleRequest(
      appRequest('GET', `/v1/cameras/${CAM_ID}/media`, undefined, makeInitData(999)),
      deps,
    );
    expect(res.statusCode).toBe(404);
  });

  it('gives every user linked to the hub access', async () => {
    const { deps, hubKey, store } = await setup();
    await handleRequest(hubRequest('/v1/device/checkin', hubKey, checkinBody), deps);
    await store.linkUser(USER_ID + 1, 'garden-hub');
    const res = await handleRequest(
      appRequest('GET', `/v1/cameras/${CAM_2}/media`, undefined, makeInitData(USER_ID + 1)),
      deps,
    );
    expect(res.statusCode).toBe(200);
  });
});

describe('video requests (app → hub → app)', () => {
  async function withMedia() {
    const ctx = await setup();
    await handleRequest(hubRequest('/v1/device/checkin', ctx.hubKey, checkinBody), ctx.deps);
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

  it('delivers the command for the right camera until it is acknowledged', async () => {
    const { deps, hubKey, store } = await withMedia();
    const path = `/v1/cameras/${CAM_ID}/commands`;
    const request = { type: 'requestVideo', dir: 100, num: 35 };

    const created = await handleRequest(appRequest('POST', path, request), deps);
    expect(created.statusCode).toBe(201);
    expect(body(created)).toEqual({ id: 'cmd-1', type: 'requestVideo', state: 'queued' });
    expect(body(await handleRequest(appRequest('POST', path, request), deps))).toMatchObject({
      id: 'cmd-1',
    });

    // Delivered even when the hub reports no camera, re-sent until acknowledged.
    for (const cameras of [[], [meadow]]) {
      const res = await handleRequest(hubRequest('/v1/device/checkin', hubKey, { cameras }), deps);
      expect(camera(res, CAM_ID)?.commands).toEqual([
        { id: 'cmd-1', type: 'requestVideo', dir: 100, num: 35 },
      ]);
      expect(camera(res, CAM_2)?.commands ?? []).toEqual([]);
    }
    expect((await store.getMedia(CAM_ID, 100, 35))?.videoState).toBe('downloading');

    await handleRequest(
      hubRequest('/v1/device/ack', hubKey, {
        cameras: [{ camId: CAM_ID, commands: [{ id: 'cmd-1', state: 'done' }] }],
      }),
      deps,
    );
    expect((await store.getCommand(CAM_ID, 'cmd-1'))?.state).toBe('done');
    expect((await store.getMedia(CAM_ID, 100, 35))?.videoState).toBe('available');

    const after = await handleRequest(
      hubRequest('/v1/device/checkin', hubKey, { cameras: [] }),
      deps,
    );
    expect(camera(after, CAM_ID)?.commands ?? []).toEqual([]);
  });

  it('marks the video as failed when the hub reports an error', async () => {
    const { deps, hubKey, store } = await withMedia();
    await handleRequest(
      appRequest('POST', `/v1/cameras/${CAM_ID}/commands`, {
        type: 'requestVideo',
        dir: 100,
        num: 35,
      }),
      deps,
    );
    await handleRequest(
      hubRequest('/v1/device/ack', hubKey, {
        cameras: [
          { camId: CAM_ID, commands: [{ id: 'cmd-1', state: 'failed', error: 'camera offline' }] },
        ],
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
