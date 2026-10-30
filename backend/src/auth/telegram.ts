import { createHmac, timingSafeEqual } from 'node:crypto';

export interface TelegramIdentity {
  userId: number;
  firstName?: string;
  authDate: number;
}

export type InitDataResult =
  | { ok: true; identity: TelegramIdentity }
  | { ok: false; reason: string };

/** Init data older than this is rejected (seconds). */
export const DEFAULT_MAX_AGE_S = 24 * 60 * 60;

/** data-check-string: all fields except `hash`, sorted by key, `key=value` joined by '\n'. */
export function dataCheckString(params: URLSearchParams): string {
  return [...params.entries()]
    .filter(([key]) => key !== 'hash')
    .sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0))
    .map(([key, value]) => `${key}=${value}`)
    .join('\n');
}

/** hash = HMAC_SHA256(data-check-string, key = HMAC_SHA256(bot_token, key = "WebAppData")). */
export function signInitData(params: URLSearchParams, botToken: string): string {
  const secret = createHmac('sha256', 'WebAppData').update(botToken).digest();
  return createHmac('sha256', secret).update(dataCheckString(params)).digest('hex');
}

/**
 * Validates Telegram Mini App init data
 * (https://core.telegram.org/bots/webapps#validating-data-received-via-the-mini-app).
 */
export function validateInitData(
  initData: string,
  botToken: string,
  nowS: number,
  maxAgeS = DEFAULT_MAX_AGE_S,
): InitDataResult {
  const params = new URLSearchParams(initData);
  const hash = params.get('hash');
  if (!hash || !/^[0-9a-f]{64}$/i.test(hash)) return { ok: false, reason: 'missing hash' };

  const expected = Buffer.from(signInitData(params, botToken), 'hex');
  const actual = Buffer.from(hash, 'hex');
  if (expected.length !== actual.length || !timingSafeEqual(expected, actual)) {
    return { ok: false, reason: 'bad signature' };
  }

  const authDate = Number(params.get('auth_date'));
  if (!Number.isFinite(authDate) || authDate <= 0) {
    return { ok: false, reason: 'missing auth_date' };
  }
  if (nowS - authDate > maxAgeS) {
    return { ok: false, reason: 'expired' };
  }

  let user: { id?: unknown; first_name?: unknown };
  try {
    user = JSON.parse(params.get('user') ?? '') as typeof user;
  } catch {
    return { ok: false, reason: 'missing user' };
  }
  if (typeof user.id !== 'number') return { ok: false, reason: 'missing user' };

  return {
    ok: true,
    identity: {
      userId: user.id,
      firstName: typeof user.first_name === 'string' ? user.first_name : undefined,
      authDate,
    },
  };
}
