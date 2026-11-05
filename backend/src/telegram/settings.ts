import type { UserSettings } from '../model';

export function defaultSettings(timeZone: string): UserSettings {
  return { notify: true, timeZone };
}

export function isValidTimeZone(timeZone: string): boolean {
  if (!/^[A-Za-z0-9_+/-]{1,64}$/.test(timeZone)) return false;
  try {
    new Intl.DateTimeFormat('en-GB', { timeZone });
    return true;
  } catch {
    return false;
  }
}

export type QuietHoursInput =
  | { ok: true; quiet: { from: number; to: number } | undefined }
  | { ok: false; error: string };

function parseTime(text: string): number | undefined {
  const match = /^(\d{1,2})(?::(\d{2}))?$/.exec(text.trim());
  if (!match) return undefined;
  const hours = Number(match[1]);
  const minutes = Number(match[2] ?? 0);
  if (hours > 24 || minutes > 59 || (hours === 24 && minutes > 0)) return undefined;
  return (hours % 24) * 60 + minutes;
}

/** Parses "22-7", "22:30-06:45" or "off". */
export function parseQuietHours(text: string): QuietHoursInput {
  const value = text.trim().toLowerCase();
  if (value === 'off' || value === 'none') return { ok: true, quiet: undefined };
  const parts = value.split(/\s*[-–]\s*/);
  const from = parts.length === 2 ? parseTime(parts[0] ?? '') : undefined;
  const to = parts.length === 2 ? parseTime(parts[1] ?? '') : undefined;
  if (from === undefined || to === undefined) {
    return { ok: false, error: 'Use a range like 22-7 or 22:30-06:45, or off.' };
  }
  if (from === to) return { ok: false, error: 'Start and end must differ.' };
  return { ok: true, quiet: { from, to } };
}

const pad = (n: number) => String(n).padStart(2, '0');
const clock = (minutes: number) => `${pad(Math.floor(minutes / 60))}:${pad(minutes % 60)}`;

export function formatQuietHours(settings: UserSettings): string {
  const { quietFrom, quietTo } = settings;
  if (quietFrom === undefined || quietTo === undefined || quietFrom === quietTo) return 'off';
  return `${clock(quietFrom)}–${clock(quietTo)} (${settings.timeZone})`;
}

/** Minutes after midnight at `nowS` in the time zone. */
export function localMinutes(nowS: number, timeZone: string): number {
  const parts = new Intl.DateTimeFormat('en-GB', {
    timeZone,
    hour: '2-digit',
    minute: '2-digit',
    hourCycle: 'h23',
  }).formatToParts(new Date(nowS * 1000));
  const value = (type: string) => Number(parts.find((p) => p.type === type)?.value ?? 0);
  return value('hour') * 60 + value('minute');
}

/** True while the user's quiet hours are on: messages then arrive without sound. */
export function isQuiet(settings: UserSettings, nowS: number): boolean {
  const { quietFrom, quietTo } = settings;
  if (quietFrom === undefined || quietTo === undefined || quietFrom === quietTo) return false;
  let minutes: number;
  try {
    minutes = localMinutes(nowS, settings.timeZone);
  } catch {
    return false; // unknown time zone: rather a sound than a lost hint
  }
  return quietFrom < quietTo
    ? minutes >= quietFrom && minutes < quietTo
    : minutes >= quietFrom || minutes < quietTo;
}
