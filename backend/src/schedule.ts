/**
 * Camera read plans: time windows in the hub's local time, each with an
 * interval in minutes, and `*` for the rest of the day, e.g.
 * `05:00-09:00=30,17:00-22:00=30,*=120`. Mirrors the firmware's parser
 * (components/schedule/schedule_plan.cpp) so a plan set in the app is
 * checked before it reaches the hub, and stored in the same normalized form
 * the hub reports back.
 */

export const MAX_WINDOWS = 8;
export const MAX_INTERVAL_MIN = 1440;
export const MAX_PLAN_LENGTH = 200;
/** Check-in intervals that can be set from the app (seconds). */
export const MIN_CHECKIN_INTERVAL_S = 5 * 60;
export const MAX_CHECKIN_INTERVAL_S = 24 * 60 * 60;

export interface PlanWindow {
  /** Minutes of the day; `end` is exclusive, may be 1440 and may be < start. */
  start: number;
  end: number;
  intervalMin: number;
}

export interface Plan {
  windows: PlanWindow[];
  defaultMin: number;
}

export type PlanResult = { ok: true; plan: Plan } | { ok: false; error: string };

function parseClock(text: string): number | undefined {
  const match = /^(\d\d):(\d\d)$/.exec(text);
  if (!match) return undefined;
  const hours = Number(match[1]);
  const minutes = Number(match[2]);
  if (minutes > 59 || hours > 24 || (hours === 24 && minutes !== 0)) return undefined;
  return hours * 60 + minutes;
}

export function parsePlan(text: string): PlanResult {
  const plan: Plan = { windows: [], defaultMin: 0 };
  let haveDefault = false;
  for (const entry of text.split(/[,; \t]+/).filter((e) => e !== '')) {
    if (entry === 'off') continue;
    const equals = entry.indexOf('=');
    if (equals < 0) return { ok: false, error: `entry '${entry}' has no '=MIN'` };
    const range = entry.slice(0, equals);
    const minutesText = entry.slice(equals + 1);
    const minutes = /^\d{1,5}$/.test(minutesText) ? Number(minutesText) : NaN;
    if (!(minutes >= 0 && minutes <= MAX_INTERVAL_MIN)) {
      return { ok: false, error: `interval in '${entry}' must be 0-1440 minutes` };
    }
    if (range === '*') {
      if (haveDefault) return { ok: false, error: "'*' is given twice" };
      haveDefault = true;
      plan.defaultMin = minutes;
      continue;
    }
    const dash = range.indexOf('-');
    const start = dash < 0 ? undefined : parseClock(range.slice(0, dash));
    const end = dash < 0 ? undefined : parseClock(range.slice(dash + 1));
    if (start === undefined || end === undefined || start === 1440) {
      return { ok: false, error: `window '${range}' must look like HH:MM-HH:MM` };
    }
    if (start === end) return { ok: false, error: `window '${range}' is empty` };
    if (plan.windows.length >= MAX_WINDOWS) return { ok: false, error: 'at most 8 windows' };
    plan.windows.push({ start, end, intervalMin: minutes });
  }
  return { ok: true, plan };
}

function clockText(minutes: number): string {
  const pad = (n: number) => String(n).padStart(2, '0');
  return `${pad(Math.floor(minutes / 60))}:${pad(minutes % 60)}`;
}

export function planReads(plan: Plan): boolean {
  return plan.defaultMin > 0 || plan.windows.some((w) => w.intervalMin > 0);
}

/** The plan in the form the firmware prints ("off" for no reads). */
export function formatPlan(plan: Plan): string {
  if (!planReads(plan)) return 'off';
  const parts = plan.windows.map(
    (w) => `${clockText(w.start)}-${clockText(w.end)}=${w.intervalMin}`,
  );
  parts.push(`*=${plan.defaultMin}`);
  return parts.join(',');
}
