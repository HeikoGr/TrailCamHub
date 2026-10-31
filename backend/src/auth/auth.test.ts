import { describe, expect, it } from 'vitest';
import { BOT_TOKEN, makeInitData, NOW } from '../test-utils';
import { generateDeviceKey, parseDeviceKey, verifySecret } from './device';
import { dataCheckString, validateInitData } from './telegram';

describe('Telegram init data', () => {
  it('builds the data-check-string sorted and without hash', () => {
    const params = new URLSearchParams('b=2&hash=ff&a=1');
    expect(dataCheckString(params)).toBe('a=1\nb=2');
  });

  it('accepts correctly signed init data', () => {
    const result = validateInitData(makeInitData(77), BOT_TOKEN, NOW);
    expect(result).toEqual({
      ok: true,
      identity: { userId: 77, firstName: 'Thomas', authDate: NOW - 60 },
    });
  });

  it('rejects data signed with another bot token', () => {
    const result = validateInitData(makeInitData(77, NOW - 60, '999:other'), BOT_TOKEN, NOW);
    expect(result).toEqual({ ok: false, reason: 'bad signature' });
  });

  it('rejects tampered fields', () => {
    const tampered = makeInitData(77).replace(/%22id%22%3A77/, '%22id%22%3A78');
    expect(tampered).not.toBe(makeInitData(77));
    expect(validateInitData(tampered, BOT_TOKEN, NOW)).toMatchObject({ ok: false });
  });

  it('rejects expired init data', () => {
    const old = makeInitData(77, NOW - 2 * 86_400);
    expect(validateInitData(old, BOT_TOKEN, NOW)).toEqual({ ok: false, reason: 'expired' });
  });

  it('rejects data without hash', () => {
    expect(validateInitData('auth_date=1', BOT_TOKEN, NOW)).toEqual({
      ok: false,
      reason: 'missing hash',
    });
  });
});

describe('device keys', () => {
  it('generates keys that verify against their hash', () => {
    const { key, keyHash } = generateDeviceKey('CAM1');
    const parsed = parseDeviceKey(key);
    expect(parsed?.camId).toBe('CAM1');
    expect(verifySecret(parsed?.secret ?? '', keyHash)).toBe(true);
  });

  it('rejects a wrong secret', () => {
    const { keyHash } = generateDeviceKey('CAM1');
    expect(verifySecret('x'.repeat(43), keyHash)).toBe(false);
  });

  it.each(['', 'CAM1', 'CAM1.', '.secretsecretsecret', 'CAM 1.secretsecretsecret', 'C.short'])(
    'rejects malformed key %j',
    (key) => {
      expect(parseDeviceKey(key)).toBeUndefined();
    },
  );
});
