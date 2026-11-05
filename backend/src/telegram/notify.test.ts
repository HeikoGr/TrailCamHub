import { describe, expect, it } from 'vitest';
import { handleRequest } from '../api';
import type { CheckinRequest } from '../model';
import { APP_URL, CAM_2, CAM_ID, HUB_ID, hubRequest, NOW, setup, USER_ID } from '../test-utils';
import { MAX_MESSAGES_PER_USER, PHOTOS_PER_CAMERA, RECENT_S } from './notify';

const FRIEND = 777;

function media(mediaId: number, fileType: 0 | 2, mediaTime: number) {
  return { mediaId, dir: 100, num: mediaId % 1000, fileType, mediaTime };
}

/** Check-in, "upload" every thumbnail, acknowledge: what a hub cycle does. */
async function cycle(ctx: Awaited<ReturnType<typeof setup>>, request: CheckinRequest) {
  await handleRequest(hubRequest('/v1/device/checkin', ctx.hubKey, request), ctx.deps);
  const cameras = (request.cameras ?? []).map((c) => {
    for (const m of c.media ?? []) ctx.storage.uploaded.add(`${c.camId}/${m.mediaId}`);
    return { camId: c.camId, uploadedThumbs: (c.media ?? []).map((m) => ({ mediaId: m.mediaId })) };
  });
  return handleRequest(hubRequest('/v1/device/ack', ctx.hubKey, { cameras }), ctx.deps);
}

describe('notifications about new recordings', () => {
  it('sends each linked user the new thumbnails with buttons', async () => {
    const ctx = await setup();
    await ctx.store.linkUser(FRIEND, HUB_ID);
    const ack = await cycle(ctx, {
      cameras: [{ camId: CAM_ID, media: [media(2035, 2, NOW - 600), media(2036, 0, NOW - 300)] }],
    });
    expect(ack.statusCode).toBe(200);

    const photos = ctx.telegram.photos;
    expect(photos.map((p) => p.chatId)).toEqual([FRIEND, FRIEND, USER_ID, USER_ID]);
    // Newest first; Telegram downloads the photo from the presigned URL.
    expect(photos[0]?.photo).toBe(`https://media.test/get/${CAM_ID}/2036.jpg`);
    expect(photos[0]?.caption).toBe(`📷 ${CAM_ID} · Garden hub\n15 Jan, 07:55 · photo`);
    expect(photos[0]?.options?.buttons).toEqual([
      [{ text: '📱 Open app', web_app: { url: APP_URL } }],
    ]);
    // Videos get a download button.
    expect(photos[1]?.caption).toContain('video');
    expect(photos[1]?.options?.buttons?.[0]).toEqual([
      { text: '🎬 Download video', callback_data: `v:${CAM_ID}:2035` },
    ]);
  });

  it('announces a recording only once', async () => {
    const ctx = await setup();
    const request = { cameras: [{ camId: CAM_ID, media: [media(2036, 0, NOW - 300)] }] };
    await cycle(ctx, request);
    expect(ctx.telegram.photos).toHaveLength(1);
    // The hub acknowledges the same thumbnail again (e.g. after a lost response).
    await handleRequest(
      hubRequest('/v1/device/ack', ctx.hubKey, {
        cameras: [{ camId: CAM_ID, uploadedThumbs: [{ mediaId: 2036 }] }],
      }),
      ctx.deps,
    );
    expect(ctx.telegram.photos).toHaveLength(1);
  });

  it('skips old recordings, e.g. the backlog of a new camera', async () => {
    const ctx = await setup();
    await cycle(ctx, {
      cameras: [{ camId: CAM_ID, media: [media(1001, 0, NOW - RECENT_S - 60)] }],
    });
    expect(ctx.telegram.photos).toHaveLength(0);
    expect(ctx.telegram.messages).toHaveLength(0);
  });

  it('sums up many recordings and limits the messages per user', async () => {
    const ctx = await setup();
    const many = (base: number) =>
      Array.from({ length: 8 }, (_, i) => media(base + i, 0, NOW - 1000 + i));
    await cycle(ctx, {
      cameras: [
        { camId: CAM_ID, media: many(2000) },
        { camId: CAM_2, media: many(3000) },
        { camId: 'C0FFEE000001', media: many(4000) },
      ],
    });
    const total = ctx.telegram.photos.length + ctx.telegram.messages.length;
    expect(total).toBe(MAX_MESSAGES_PER_USER);
    expect(ctx.telegram.photos[0]?.photo).toBe(`https://media.test/get/${CAM_ID}/2007.jpg`);
    expect(ctx.telegram.messages[0]?.text).toBe(
      `…and ${8 - PHOTOS_PER_CAMERA} more new recordings from ${CAM_ID}.`,
    );
  });

  it('respects notifications off and quiet hours', async () => {
    const ctx = await setup();
    await ctx.store.linkUser(FRIEND, HUB_ID);
    await ctx.store.putUserSettings(USER_ID, { notify: false, timeZone: 'Europe/Berlin' });
    // NOW is 09:00 in Berlin.
    await ctx.store.putUserSettings(FRIEND, {
      notify: true,
      quietFrom: 8 * 60,
      quietTo: 10 * 60,
      timeZone: 'Europe/Berlin',
    });
    await cycle(ctx, { cameras: [{ camId: CAM_ID, media: [media(2036, 0, NOW - 300)] }] });
    expect(ctx.telegram.photos).toHaveLength(1);
    expect(ctx.telegram.photos[0]?.chatId).toBe(FRIEND);
    expect(ctx.telegram.photos[0]?.options?.silent).toBe(true);
  });

  it('keeps going when a user blocked the bot, and never fails the hub', async () => {
    const ctx = await setup();
    await ctx.store.linkUser(FRIEND, HUB_ID);
    ctx.telegram.blocked.add(USER_ID);
    const ack = await cycle(ctx, {
      cameras: [{ camId: CAM_ID, media: [media(2036, 0, NOW - 300)] }],
    });
    expect(ack.statusCode).toBe(200);
    expect(ctx.telegram.photos.map((p) => p.chatId)).toEqual([FRIEND]);

    ctx.deps.store.listHubUserIds = async () => {
      throw new Error('table unavailable');
    };
    const failing = await cycle(ctx, {
      cameras: [{ camId: CAM_ID, media: [media(2037, 0, NOW - 200)] }],
    });
    expect(failing.statusCode).toBe(200);
  });
});
