import { describe, expect, it } from 'vitest';
import { getInitData, getUser, initTelegram, isInTelegram } from './telegram';
import { fakeWebApp } from './test-utils';

describe('telegram bridge', () => {
  it('detects Telegram only with signed init data', () => {
    expect(isInTelegram(undefined)).toBe(false);
    expect(isInTelegram(fakeWebApp(''))).toBe(false);
    expect(isInTelegram(fakeWebApp())).toBe(true);
  });

  it('exposes init data and user only inside Telegram', () => {
    expect(getInitData(fakeWebApp())).toBe('query_id=1&hash=abc');
    expect(getUser(fakeWebApp())?.first_name).toBe('Thomas');
    expect(getInitData(fakeWebApp(''))).toBe('');
    expect(getUser(fakeWebApp(''))).toBeUndefined();
  });

  it('signals readiness and expands inside Telegram', () => {
    const webApp = fakeWebApp();
    initTelegram(webApp);
    expect(webApp.ready).toHaveBeenCalled();
    expect(webApp.expand).toHaveBeenCalled();
  });

  it('does nothing in a plain browser', () => {
    const webApp = fakeWebApp('');
    initTelegram(webApp);
    expect(webApp.ready).not.toHaveBeenCalled();
  });
});
