import { describe, expect, it } from 'vitest';
import { HttpTelegramApi, TelegramError, webhookSecret } from './api';

type Call = { url: string; init: RequestInit };

function fakeFetch(...responses: unknown[]) {
  const calls: Call[] = [];
  const fetchFn = (async (url: string, init: RequestInit) => {
    calls.push({ url, init });
    const next = responses.shift() ?? { ok: true, result: true };
    return new Response(JSON.stringify(next), { status: 200 });
  }) as unknown as typeof fetch;
  return { calls, fetchFn };
}

const TOKEN = '123:abc';

describe('HttpTelegramApi', () => {
  it('sends JSON with buttons and silent delivery', async () => {
    const { calls, fetchFn } = fakeFetch();
    const api = new HttpTelegramApi(async () => TOKEN, fetchFn);
    await api.sendMessage(42, 'hello', {
      buttons: [[{ text: 'Go', callback_data: 'n:on' }]],
      silent: true,
    });
    expect(calls[0]?.url).toBe('https://api.telegram.org/bot123:abc/sendMessage');
    expect(JSON.parse(String(calls[0]?.init.body))).toEqual({
      chat_id: 42,
      text: 'hello',
      link_preview_options: { is_disabled: true },
      reply_markup: { inline_keyboard: [[{ text: 'Go', callback_data: 'n:on' }]] },
      disable_notification: true,
    });
  });

  it('uploads PNGs as multipart form data', async () => {
    const { calls, fetchFn } = fakeFetch();
    const api = new HttpTelegramApi(async () => TOKEN, fetchFn);
    await api.sendPhoto(42, { png: Buffer.from([1, 2, 3]), filename: 'qr.png' }, 'Scan me');
    const form = calls[0]?.init.body as FormData;
    expect(form.get('chat_id')).toBe('42');
    expect(form.get('caption')).toBe('Scan me');
    expect((form.get('photo') as File).name).toBe('qr.png');
  });

  it('caches the bot username', async () => {
    const { calls, fetchFn } = fakeFetch({ ok: true, result: { username: 'TrailCamHubBot' } });
    const api = new HttpTelegramApi(async () => TOKEN, fetchFn);
    expect(await api.botUsername()).toBe('TrailCamHubBot');
    expect(await api.botUsername()).toBe('TrailCamHubBot');
    expect(calls).toHaveLength(1);
  });

  it('waits once for a short rate limit and reports other errors', async () => {
    const { calls, fetchFn } = fakeFetch(
      {
        ok: false,
        error_code: 429,
        description: 'Too Many Requests',
        parameters: { retry_after: 0 },
      },
      { ok: true, result: true },
      { ok: false, error_code: 403, description: 'Forbidden: bot was blocked by the user' },
    );
    const api = new HttpTelegramApi(async () => TOKEN, fetchFn);
    await api.answerCallbackQuery('q1');
    expect(calls).toHaveLength(2);

    const error = await api.sendMessage(42, 'x').catch((e: unknown) => e);
    expect(error).toBeInstanceOf(TelegramError);
    expect((error as TelegramError).code).toBe(403);
  });
});

describe('webhookSecret', () => {
  it('matches the value the deploy workflow computes with openssl', () => {
    // printf %s '123456:ABC-def_ghi' | openssl dgst -sha256 -hmac trailcamhub-webhook -r
    expect(webhookSecret('123456:ABC-def_ghi')).toBe(
      'c7284407b23447e69ec1705e183d48531ee48bd5ce00a3ae8a1755a5d00e2779',
    );
  });
});
