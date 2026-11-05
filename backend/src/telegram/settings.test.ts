import { describe, expect, it } from 'vitest';
import {
  formatQuietHours,
  isQuiet,
  isValidTimeZone,
  localMinutes,
  parseQuietHours,
} from './settings';

// 2027-01-15 08:00 UTC = 09:00 in Berlin (winter time).
const WINTER = 1_800_000_000;
// 2027-07-15 08:00 UTC = 10:00 in Berlin (summer time).
const SUMMER = Date.UTC(2027, 6, 15, 8) / 1000;

describe('quiet hours', () => {
  it('parses ranges and off', () => {
    expect(parseQuietHours('22-7')).toEqual({ ok: true, quiet: { from: 1320, to: 420 } });
    expect(parseQuietHours(' 22:30 – 06:45 ')).toEqual({
      ok: true,
      quiet: { from: 1350, to: 405 },
    });
    expect(parseQuietHours('0-24')).toEqual({ ok: false, error: 'Start and end must differ.' });
    expect(parseQuietHours('Off')).toEqual({ ok: true, quiet: undefined });
    for (const bad of ['', '22', '25-7', '22:60-7', 'a-b', '1-2-3']) {
      expect(parseQuietHours(bad).ok).toBe(false);
    }
  });

  it('follows the time zone, including daylight saving time', () => {
    expect(localMinutes(WINTER, 'Europe/Berlin')).toBe(9 * 60);
    expect(localMinutes(SUMMER, 'Europe/Berlin')).toBe(10 * 60);
    expect(localMinutes(WINTER, 'UTC')).toBe(8 * 60);
  });

  it('handles windows within a day and across midnight', () => {
    const base = { notify: true, timeZone: 'Europe/Berlin' };
    expect(isQuiet({ ...base, quietFrom: 8 * 60, quietTo: 10 * 60 }, WINTER)).toBe(true);
    expect(isQuiet({ ...base, quietFrom: 8 * 60, quietTo: 9 * 60 }, WINTER)).toBe(false);
    expect(isQuiet({ ...base, quietFrom: 22 * 60, quietTo: 9 * 60 + 1 }, WINTER)).toBe(true);
    expect(isQuiet({ ...base, quietFrom: 22 * 60, quietTo: 7 * 60 }, WINTER)).toBe(false);
    expect(isQuiet(base, WINTER)).toBe(false);
    expect(
      isQuiet({ ...base, timeZone: 'Nowhere/Land', quietFrom: 0, quietTo: 1439 }, WINTER),
    ).toBe(false);
  });

  it('formats the setting', () => {
    expect(formatQuietHours({ notify: true, timeZone: 'Europe/Berlin' })).toBe('off');
    expect(
      formatQuietHours({ notify: true, timeZone: 'Europe/Berlin', quietFrom: 1320, quietTo: 420 }),
    ).toBe('22:00–07:00 (Europe/Berlin)');
  });
});

describe('time zones', () => {
  it('accepts IANA names only', () => {
    expect(isValidTimeZone('Europe/Berlin')).toBe(true);
    expect(isValidTimeZone('America/Argentina/Buenos_Aires')).toBe(true);
    expect(isValidTimeZone('Mars/Olympus')).toBe(false);
    expect(isValidTimeZone('')).toBe(false);
    expect(isValidTimeZone('Europe/Berlin; drop')).toBe(false);
  });
});
