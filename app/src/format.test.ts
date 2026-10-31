import { describe, expect, it } from 'vitest';
import {
  batteryLevel,
  canRequestVideo,
  formatRelative,
  formatStorage,
  mediaKey,
  videoStateLabel,
} from './format';

describe('mediaKey', () => {
  it('matches the firmware thumbnail naming <dir>_<num>', () => {
    expect(mediaKey({ dir: 100, num: 36 })).toBe('100_36');
  });
});

describe('batteryLevel', () => {
  it.each([
    [100, 'ok'],
    [26, 'ok'],
    [25, 'low'],
    [11, 'low'],
    [10, 'critical'],
    [0, 'critical'],
  ] as const)('%i %% is %s', (percent, level) => {
    expect(batteryLevel(percent)).toBe(level);
  });
});

describe('formatStorage', () => {
  it('uses MB below 1 GB', () => {
    expect(formatStorage(512)).toBe('512 MB');
  });
  it('uses GB with one decimal from 1 GB', () => {
    expect(formatStorage(28_412)).toBe('27.7 GB');
  });
});

describe('formatRelative', () => {
  const now = 1_000_000;
  it.each([
    [now, 'just now'],
    [now - 59, 'just now'],
    [now - 17 * 60, '17 min ago'],
    [now - 3 * 3_600, '3 h ago'],
    [now - 86_400, '1 day ago'],
    [now - 3 * 86_400, '3 days ago'],
    [now + 120, 'just now'],
  ])('%i → %s', (ts, expected) => {
    expect(formatRelative(ts, now)).toBe(expected);
  });
});

describe('canRequestVideo', () => {
  it('allows videos that are not downloaded or failed', () => {
    expect(canRequestVideo({ fileType: 2, videoState: 'none' })).toBe(true);
    expect(canRequestVideo({ fileType: 2, videoState: 'failed' })).toBe(true);
  });
  it('rejects photos and videos already on their way', () => {
    expect(canRequestVideo({ fileType: 0, videoState: 'none' })).toBe(false);
    expect(canRequestVideo({ fileType: 2, videoState: 'requested' })).toBe(false);
    expect(canRequestVideo({ fileType: 2, videoState: 'available' })).toBe(false);
  });
});

describe('videoStateLabel', () => {
  it('explains that requests are served at the next wake-up', () => {
    expect(videoStateLabel('requested')).toContain('next wake-up');
  });
});
