import { describe, expect, it } from 'vitest';
import { formatPlan, parsePlan } from './schedule';

function normalized(text: string): string {
  const result = parsePlan(text);
  if (!result.ok) throw new Error(result.error);
  return formatPlan(result.plan);
}

describe('camera read plans', () => {
  it('parses and normalizes plans like the firmware', () => {
    expect(normalized('05:00-09:00=30, 17:00-22:00=30 *=120')).toBe(
      '05:00-09:00=30,17:00-22:00=30,*=120',
    );
    expect(normalized('*=120')).toBe('*=120');
    expect(normalized('20:00-24:00=15')).toBe('20:00-24:00=15,*=0');
    expect(normalized('22:00-06:00=0,*=60')).toBe('22:00-06:00=0,*=60');
    expect(normalized('off')).toBe('off');
    expect(normalized('')).toBe('off');
    expect(normalized('*=0')).toBe('off');
  });

  it('rejects invalid plans with a reason', () => {
    for (const text of [
      '*',
      '*=abc',
      '*=1441',
      '*=-5',
      '5:00-9:00=30',
      '05:00=30',
      '05:00-05:00=30',
      '25:00-26:00=10',
      '05:60-06:00=10',
      '24:00-01:00=10',
      '*=10,*=20',
      Array.from({ length: 9 }, (_, i) => `0${i}:00-0${i}:30=1`).join(','),
    ]) {
      const result = parsePlan(text);
      expect(result.ok, text).toBe(false);
      if (!result.ok) expect(result.error).not.toBe('');
    }
  });
});
