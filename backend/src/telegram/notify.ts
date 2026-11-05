import type { Deps } from '../api';
import type { CameraRecord, HubRecord, MediaRecord } from '../model';
import type { InlineKeyboard } from './api';
import { TelegramError } from './api';
import { openAppRow } from './bot';
import { defaultSettings, isQuiet } from './settings';

export interface NewThumbnail {
  camera: CameraRecord;
  media: MediaRecord;
}

/**
 * Only recordings from the last days are announced, so a hub syncing an old
 * backlog (a new camera, a long time offline) does not flood the chat.
 * mediaTime is the camera's wall clock read as UTC, which can be up to a day
 * off; the window is wide enough for that.
 */
export const RECENT_S = 3 * 24 * 60 * 60;
/** Photos sent per camera and check-in; more are summed up in one message. */
export const PHOTOS_PER_CAMERA = 3;
/** Messages per user and check-in (Telegram allows about one per second and chat). */
export const MAX_MESSAGES_PER_USER = 10;

const timeFormat = new Intl.DateTimeFormat('en-GB', {
  timeZone: 'UTC', // the camera's wall clock, see docs/clock.md
  day: 'numeric',
  month: 'short',
  hour: '2-digit',
  minute: '2-digit',
  hourCycle: 'h23',
});

export function caption(camera: CameraRecord, hub: HubRecord, media: MediaRecord): string {
  return (
    `📷 ${camera.name} · ${hub.name}\n` +
    `${timeFormat.format(new Date(media.mediaTime * 1000))} · ${media.fileType === 2 ? 'video' : 'photo'}`
  );
}

function buttons(camera: CameraRecord, media: MediaRecord, appUrl?: string): InlineKeyboard {
  const rows: InlineKeyboard = [];
  const data = `v:${camera.camId}:${media.mediaId}`;
  // callback_data is limited to 64 bytes; ids are ASCII.
  if (media.fileType === 2 && data.length <= 64) {
    rows.push([{ text: '🎬 Download video', callback_data: data }]);
  }
  return [...rows, ...openAppRow(appUrl)];
}

/**
 * Tells every user linked to the hub about thumbnails that arrived now: one
 * photo per recording (newest first, a few per camera) with buttons to fetch
 * the video and to open the app. Users who turned notifications off get
 * nothing; during their quiet hours messages arrive without sound.
 *
 * Failures are logged and never fail the hub's request. Returns the number of
 * messages sent.
 */
export async function notifyNewThumbnails(
  hub: HubRecord,
  thumbnails: NewThumbnail[],
  deps: Deps,
): Promise<number> {
  const telegram = deps.telegram;
  if (!telegram) return 0;
  const now = deps.now();
  const recent = thumbnails.filter((t) => t.media.mediaTime >= now - RECENT_S);
  if (recent.length === 0) return 0;

  const byCamera = new Map<string, NewThumbnail[]>();
  for (const t of recent) {
    byCamera.set(t.camera.camId, [...(byCamera.get(t.camera.camId) ?? []), t]);
  }
  for (const list of byCamera.values()) {
    list.sort((a, b) => b.media.mediaTime - a.media.mediaTime || b.media.num - a.media.num);
  }

  let sent = 0;
  for (const userId of await deps.store.listHubUserIds(hub.hubId)) {
    const settings =
      (await deps.store.getUserSettings(userId)) ?? defaultSettings(telegram.defaultTimeZone);
    if (!settings.notify) continue;
    const silent = isQuiet(settings, now);
    let budget = MAX_MESSAGES_PER_USER;
    try {
      for (const list of byCamera.values()) {
        const shown = list.slice(0, PHOTOS_PER_CAMERA);
        for (const { camera, media } of shown) {
          if (budget-- <= 0) break;
          const url = await deps.storage.thumbReadUrl(camera.camId, media.mediaId);
          await telegram.api.sendPhoto(userId, url, caption(camera, hub, media), {
            buttons: buttons(camera, media, telegram.appUrl),
            silent,
          });
          sent++;
        }
        const more = list.length - shown.length;
        const camera = list[0]?.camera;
        if (more > 0 && camera && budget-- > 0) {
          await telegram.api.sendMessage(
            userId,
            `…and ${more} more new recording${more === 1 ? '' : 's'} from ${camera.name}.`,
            { buttons: openAppRow(telegram.appUrl), silent },
          );
          sent++;
        }
      }
    } catch (error) {
      // 403: the user blocked the bot or never started it; others: try the next user.
      if (!(error instanceof TelegramError && error.code === 403)) {
        console.error(`Notification to user ${userId} failed`, error);
      }
    }
  }
  return sent;
}
