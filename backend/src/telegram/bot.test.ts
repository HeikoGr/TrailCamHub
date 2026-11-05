import { describe, expect, it } from 'vitest';
import { createPairing } from '../admin';
import { handleRequest } from '../api';
import type { ApiResponse } from '../http';
import {
  APP_URL,
  appRequest,
  buttonPress,
  CAM_ID,
  HUB_ID,
  hubRequest,
  makeInitData,
  NOW,
  setup,
  textMessage,
  USER_ID,
  webhookRequest,
} from '../test-utils';

const FRIEND = 777;
const body = <T>(response: ApiResponse) => JSON.parse(response.body) as T;

/** The code of a pairing link `https://t.me/TrailCamHubBot?start=<code>`. */
function codeOf(link: string): string {
  const match = /^https:\/\/t\.me\/TrailCamHubBot\?start=([A-Za-z0-9]{12})$/.exec(link);
  if (!match?.[1]) throw new Error(`not a pairing link: ${link}`);
  return match[1];
}

describe('webhook', () => {
  it('accepts only requests with the secret token', async () => {
    const { deps, telegram } = await setup();
    const forged = await handleRequest(webhookRequest(textMessage('/help'), 'wrong'), deps);
    expect(forged.statusCode).toBe(401);
    expect(telegram.messages).toHaveLength(0);

    const ok = await handleRequest(webhookRequest(textMessage('/help')), deps);
    expect(ok.statusCode).toBe(200);
    expect(telegram.lastText()).toContain('/settings');
  });

  it('answers 200 even when handling fails, so Telegram does not retry forever', async () => {
    const { deps } = await setup();
    deps.store.listUserHubIds = async () => {
      throw new Error('table unavailable');
    };
    const response = await handleRequest(webhookRequest(textMessage('/start')), deps);
    expect(response.statusCode).toBe(200);
  });

  it('ignores group chats and other bots', async () => {
    const { deps, telegram } = await setup();
    await handleRequest(
      webhookRequest({
        message: {
          message_id: 1,
          from: { id: USER_ID },
          chat: { id: -100, type: 'group' },
          text: '/help',
        },
      }),
      deps,
    );
    await handleRequest(
      webhookRequest({
        message: {
          message_id: 2,
          from: { id: 5, is_bot: true },
          chat: { id: 5, type: 'private' },
          text: '/help',
        },
      }),
      deps,
    );
    expect(telegram.messages).toHaveLength(0);
  });
});

describe('pairing', () => {
  it('links a friend through /invite: QR code and link, usable once', async () => {
    const { deps, store, telegram } = await setup();
    await handleRequest(webhookRequest(textMessage('/invite')), deps);

    const invitation = telegram.photos.at(-1);
    expect(invitation?.chatId).toBe(USER_ID);
    expect(typeof invitation?.photo).toBe('object');
    const png = (invitation?.photo as { png: Buffer }).png;
    expect(png.subarray(1, 4).toString('ascii')).toBe('PNG');
    const link = /https:\/\/t\.me\/\S+/.exec(invitation?.caption ?? '')?.[0] ?? '';
    const code = codeOf(link);

    await handleRequest(webhookRequest(textMessage(`/start ${code}`, FRIEND)), deps);
    expect(await store.isLinked(FRIEND, HUB_ID)).toBe(true);
    expect(await store.listHubUserIds(HUB_ID)).toEqual([FRIEND, USER_ID]);
    expect(telegram.lastText(FRIEND)).toContain('now linked to the hub "Garden hub"');
    expect(telegram.messages.at(-1)?.options?.buttons).toEqual([
      [{ text: '📱 Open app', web_app: { url: APP_URL } }],
    ]);

    // The friend now sees the hub's cameras in the app.
    const cameras = await handleRequest(
      appRequest('GET', '/v1/cameras', undefined, makeInitData(FRIEND)),
      deps,
    );
    expect(cameras.statusCode).toBe(200);

    // A second person cannot use the same code.
    await handleRequest(webhookRequest(textMessage(`/start ${code}`, 888)), deps);
    expect(await store.isLinked(888, HUB_ID)).toBe(false);
    expect(telegram.lastText(888)).toContain('not valid or has already been used');
  });

  it('refuses expired codes', async () => {
    const { deps, store, telegram } = await setup();
    const result = await createPairing(
      { admin: 'createPairingCode', hubId: HUB_ID, ttlHours: 1 },
      { ...deps, now: () => NOW - 2 * 3600 },
    );
    if (!result.ok) throw new Error(result.error);
    expect(result.link).toBe(`https://t.me/TrailCamHubBot?start=${result.code}`);

    await handleRequest(webhookRequest(textMessage(`/start ${result.code}`, FRIEND)), deps);
    expect(await store.isLinked(FRIEND, HUB_ID)).toBe(false);
    expect(telegram.lastText(FRIEND)).toContain('expired');
  });

  it('rejects malformed codes without touching the store', async () => {
    const { deps, telegram } = await setup();
    await handleRequest(webhookRequest(textMessage('/start ../../etc', FRIEND)), deps);
    expect(telegram.lastText(FRIEND)).toContain('not valid');
  });

  it('greets users without a code', async () => {
    const { deps, telegram } = await setup();
    await handleRequest(webhookRequest(textMessage('/start', FRIEND)), deps);
    expect(telegram.lastText(FRIEND)).toContain('open the link or scan the QR code');
    await handleRequest(webhookRequest(textMessage('/start')), deps);
    expect(telegram.lastText()).toContain('linked to the hub "Garden hub"');
  });

  it('gives the hub short-lived codes to show', async () => {
    const { deps, hubKey } = await setup();
    const response = await handleRequest(hubRequest('/v1/device/pairing-code', hubKey, {}), deps);
    expect(response.statusCode).toBe(201);
    const result = body<{ code: string; link: string; expiresAt: number }>(response);
    expect(codeOf(result.link)).toBe(result.code);
    expect(result.expiresAt).toBe(NOW + 30 * 60);

    const forged = await handleRequest(
      hubRequest('/v1/device/pairing-code', 'garden-hub.wrong', {}),
      deps,
    );
    expect(forged.statusCode).toBe(401);
  });

  it('only invites to hubs the user is linked to', async () => {
    const { deps, telegram } = await setup();
    await handleRequest(webhookRequest(textMessage('/invite', FRIEND)), deps);
    expect(telegram.lastText(FRIEND)).toContain('not linked to any hub');
    expect(telegram.photos).toHaveLength(0);
  });

  it('asks which hub when the user has several', async () => {
    const { deps, store, telegram } = await setup();
    await store.linkUser(USER_ID, 'other-hub');
    await handleRequest(webhookRequest(textMessage('/invite')), deps);
    expect(telegram.messages.at(-1)?.options?.buttons).toEqual([
      [{ text: 'Garden hub', callback_data: 'i:0' }],
      [{ text: 'Other', callback_data: 'i:1' }],
    ]);
    await handleRequest(webhookRequest(buttonPress('i:1')), deps);
    expect(telegram.photos.at(-1)?.caption).toContain('Invitation to the hub "Other"');
  });
});

describe('settings', () => {
  it('turns notifications off and on by command and by button', async () => {
    const { deps, store, telegram } = await setup();
    await handleRequest(webhookRequest(textMessage('/notify off')), deps);
    expect((await store.getUserSettings(USER_ID))?.notify).toBe(false);

    await handleRequest(webhookRequest(textMessage('/settings')), deps);
    expect(telegram.lastText()).toContain('Notifications: off');
    expect(telegram.messages.at(-1)?.options?.buttons?.[0]).toEqual([
      { text: '🔔 Turn notifications on', callback_data: 'n:on' },
    ]);

    await handleRequest(webhookRequest(buttonPress('n:on')), deps);
    expect((await store.getUserSettings(USER_ID))?.notify).toBe(true);
    expect(telegram.answers.at(-1)).toEqual({ id: 'cb-n:on', text: 'Notifications on' });
    expect(telegram.lastText()).toContain('Notifications: on');
  });

  it('sets quiet hours and the time zone', async () => {
    const { deps, store, telegram } = await setup();
    await handleRequest(webhookRequest(textMessage('/quiet 22-6:30')), deps);
    expect(await store.getUserSettings(USER_ID)).toEqual({
      notify: true,
      quietFrom: 22 * 60,
      quietTo: 6 * 60 + 30,
      timeZone: 'Europe/Berlin',
    });
    expect(telegram.lastText()).toContain('22:00–06:30 (Europe/Berlin)');

    await handleRequest(webhookRequest(textMessage('/timezone Mars/Olympus')), deps);
    expect(telegram.lastText()).toContain('Unknown time zone');
    await handleRequest(webhookRequest(textMessage('/timezone America/New_York')), deps);
    expect((await store.getUserSettings(USER_ID))?.timeZone).toBe('America/New_York');

    await handleRequest(webhookRequest(textMessage('/quiet nonsense')), deps);
    expect(telegram.lastText()).toContain('Use a range like 22-7');
    await handleRequest(webhookRequest(textMessage('/quiet off')), deps);
    expect((await store.getUserSettings(USER_ID))?.quietFrom).toBeUndefined();
  });

  it('accepts commands addressed to the bot by name', async () => {
    const { deps, store } = await setup();
    await handleRequest(webhookRequest(textMessage('/notify@TrailCamHubBot off')), deps);
    expect((await store.getUserSettings(USER_ID))?.notify).toBe(false);
  });
});

describe('video button', () => {
  async function withVideo() {
    const ctx = await setup();
    await ctx.store.claimCamera(CAM_ID, 'garden-hub');
    await ctx.store.addMedia(CAM_ID, [
      {
        mediaId: 2035,
        dir: 100,
        num: 35,
        fileType: 2,
        mediaTime: NOW - 600,
        thumbPresent: true,
        videoState: 'none',
      },
    ]);
    return ctx;
  }

  it('requests the video once', async () => {
    const { deps, store, telegram } = await withVideo();
    await handleRequest(webhookRequest(buttonPress(`v:${CAM_ID}:2035`)), deps);
    expect(telegram.answers.at(-1)?.text).toContain('Video requested');
    expect((await store.getMedia(CAM_ID, 2035))?.videoState).toBe('requested');
    expect(await store.listCommands(CAM_ID)).toHaveLength(1);

    await handleRequest(webhookRequest(buttonPress(`v:${CAM_ID}:2035`)), deps);
    expect(telegram.answers.at(-1)?.text).toBe('This video is already requested.');
    expect(await store.listCommands(CAM_ID)).toHaveLength(1);
  });

  it('does nothing for users without access', async () => {
    const { deps, store, telegram } = await withVideo();
    await handleRequest(webhookRequest(buttonPress(`v:${CAM_ID}:2035`, FRIEND)), deps);
    expect(telegram.answers.at(-1)?.text).toBe('Camera not found');
    expect(await store.listCommands(CAM_ID)).toHaveLength(0);
  });
});
