import type { Deps } from '../api';
import { requestVideo } from '../commands';
import type { UserSettings } from '../model';
import { createPairingCode, INVITE_TTL_S, pairingLink, redeemPairingCode } from '../pairing';
import { qrPng } from '../qr';
import type { InlineKeyboard, TelegramApi } from './api';
import { defaultSettings, formatQuietHours, isValidTimeZone, parseQuietHours } from './settings';

/** The parts of a Telegram update the bot reads (https://core.telegram.org/bots/api#update). */
interface TgUser {
  id: number;
  is_bot?: boolean;
}

interface TgChat {
  id: number;
  type: string;
}

export interface TelegramUpdate {
  update_id: number;
  message?: { message_id: number; from?: TgUser; chat: TgChat; text?: string };
  callback_query?: { id: string; from: TgUser; data?: string; message?: { chat: TgChat } };
}

const HELP = [
  '/settings – notifications and quiet hours',
  '/notify on|off – messages about new recordings',
  '/quiet 22-7 | off – quiet hours: messages arrive without sound',
  '/timezone Europe/Berlin – time zone of the quiet hours',
  '/invite – link another person to your hub',
  '/help – this list',
].join('\n');

const NEW_LINK_HINT = 'Ask for a new one: anyone linked to the hub can send /invite.';
/** Hubs offered as buttons by /invite. */
const MAX_HUB_BUTTONS = 10;

interface Telegram {
  api: TelegramApi;
  appUrl?: string;
  defaultTimeZone: string;
}

/** "Open app" button for the Mini App, if its URL is configured. */
export function openAppRow(appUrl: string | undefined): InlineKeyboard {
  return appUrl ? [[{ text: '📱 Open app', web_app: { url: appUrl } }]] : [];
}

/** Handles one webhook update. Only private chats are served. */
export async function handleUpdate(update: TelegramUpdate, deps: Deps): Promise<void> {
  const telegram = deps.telegram;
  if (!telegram) return;
  const message = update.message;
  if (message?.from && !message.from.is_bot && message.chat.type === 'private' && message.text) {
    await handleMessage(message.chat.id, message.from.id, message.text, telegram, deps);
    return;
  }
  const query = update.callback_query;
  if (query) {
    const chat = query.message?.chat;
    if (chat && chat.type !== 'private') {
      await telegram.api.answerCallbackQuery(query.id);
      return;
    }
    await handleCallback(
      query.id,
      chat?.id ?? query.from.id,
      query.from.id,
      query.data ?? '',
      telegram,
      deps,
    );
  }
}

async function loadSettings(userId: number, telegram: Telegram, deps: Deps): Promise<UserSettings> {
  return (await deps.store.getUserSettings(userId)) ?? defaultSettings(telegram.defaultTimeZone);
}

async function updateSettings(
  userId: number,
  change: Partial<UserSettings>,
  telegram: Telegram,
  deps: Deps,
): Promise<UserSettings> {
  const settings = { ...(await loadSettings(userId, telegram, deps)), ...change };
  await deps.store.putUserSettings(userId, settings);
  return settings;
}

async function handleMessage(
  chatId: number,
  userId: number,
  text: string,
  telegram: Telegram,
  deps: Deps,
): Promise<void> {
  const match = /^\/([A-Za-z_]+)(?:@\w+)?(?:\s+([\s\S]*))?$/.exec(text.trim());
  const reply = (body: string, buttons?: InlineKeyboard) =>
    telegram.api.sendMessage(chatId, body, buttons?.length ? { buttons } : undefined);
  if (!match) {
    await reply(`I only understand commands.\n\n${HELP}`);
    return;
  }
  const command = (match[1] ?? '').toLowerCase();
  const arg = (match[2] ?? '').trim();

  switch (command) {
    case 'start':
      await start(chatId, userId, arg, telegram, deps);
      return;
    case 'help':
      await reply(HELP, openAppRow(telegram.appUrl));
      return;
    case 'settings':
      await showSettings(chatId, userId, telegram, deps);
      return;
    case 'notify': {
      const value = arg.toLowerCase();
      if (value !== 'on' && value !== 'off') {
        await reply('Use /notify on or /notify off.');
        return;
      }
      await updateSettings(userId, { notify: value === 'on' }, telegram, deps);
      await reply(
        value === 'on'
          ? 'Notifications on: new recordings are sent here.'
          : 'Notifications off. /notify on turns them back on.',
      );
      return;
    }
    case 'quiet': {
      if (!arg) {
        const settings = await loadSettings(userId, telegram, deps);
        await reply(
          `Quiet hours: ${formatQuietHours(settings)}.\nChange them with /quiet 22-7 or turn them off with /quiet off.`,
        );
        return;
      }
      const parsed = parseQuietHours(arg);
      if (!parsed.ok) {
        await reply(parsed.error);
        return;
      }
      const settings = await updateSettings(
        userId,
        { quietFrom: parsed.quiet?.from, quietTo: parsed.quiet?.to },
        telegram,
        deps,
      );
      await reply(
        parsed.quiet
          ? `Quiet hours: ${formatQuietHours(settings)}. Messages in this time arrive without sound.`
          : 'Quiet hours off.',
      );
      return;
    }
    case 'timezone': {
      if (!arg) {
        const settings = await loadSettings(userId, telegram, deps);
        await reply(
          `Time zone: ${settings.timeZone}.\nChange it with e.g. /timezone Europe/Berlin.`,
        );
        return;
      }
      if (!isValidTimeZone(arg)) {
        await reply(`Unknown time zone "${arg}". Use a name like Europe/Berlin.`);
        return;
      }
      await updateSettings(userId, { timeZone: arg }, telegram, deps);
      await reply(`Time zone: ${arg}.`);
      return;
    }
    case 'invite':
      await invite(chatId, userId, telegram, deps);
      return;
    default:
      await reply(`Unknown command.\n\n${HELP}`);
  }
}

async function start(
  chatId: number,
  userId: number,
  code: string,
  telegram: Telegram,
  deps: Deps,
): Promise<void> {
  const send = (text: string) =>
    telegram.api.sendMessage(chatId, text, { buttons: openAppRow(telegram.appUrl) });
  if (code) {
    const result = await redeemPairingCode(deps.store, code, userId, deps.now());
    if (!result.ok) {
      await telegram.api.sendMessage(
        chatId,
        result.reason === 'expired'
          ? `This link has expired. ${NEW_LINK_HINT}`
          : `This link is not valid or has already been used. ${NEW_LINK_HINT}`,
      );
      return;
    }
    await send(
      result.alreadyLinked
        ? `You are already linked to the hub "${result.hub.name}".`
        : `✅ You are now linked to the hub "${result.hub.name}" and see its cameras in the app.\n` +
            'New recordings are sent here; /settings changes that.',
    );
    return;
  }
  const hubIds = await deps.store.listUserHubIds(userId);
  if (hubIds.length === 0) {
    await telegram.api.sendMessage(
      chatId,
      "Hi! This is the TrailCamHub bot. To see a hub's cameras, open the link or scan the " +
        `QR code you got from someone linked to the hub.\n\n${HELP}`,
    );
    return;
  }
  const hub = hubIds.length === 1 ? await deps.store.getHub(hubIds[0] ?? '') : undefined;
  await send(
    `Welcome back! You are linked to ${hub ? `the hub "${hub.name}"` : `${hubIds.length} hubs`}.\n\n${HELP}`,
  );
}

async function showSettings(
  chatId: number,
  userId: number,
  telegram: Telegram,
  deps: Deps,
): Promise<void> {
  const settings = await loadSettings(userId, telegram, deps);
  const hubs = await Promise.all(
    (await deps.store.listUserHubIds(userId)).map(
      async (id) => (await deps.store.getHub(id))?.name ?? id,
    ),
  );
  const text = [
    `Notifications: ${settings.notify ? 'on' : 'off'}`,
    `Quiet hours: ${formatQuietHours(settings)}`,
    `Hubs: ${hubs.length > 0 ? hubs.join(', ') : 'none'}`,
    '',
    'Change the quiet hours with /quiet 22-7, the time zone with /timezone.',
  ].join('\n');
  await telegram.api.sendMessage(chatId, text, {
    buttons: [
      [
        settings.notify
          ? { text: '🔕 Turn notifications off', callback_data: 'n:off' }
          : { text: '🔔 Turn notifications on', callback_data: 'n:on' },
      ],
      ...openAppRow(telegram.appUrl),
    ],
  });
}

async function invite(chatId: number, userId: number, telegram: Telegram, deps: Deps) {
  const hubIds = await deps.store.listUserHubIds(userId);
  if (hubIds.length === 0) {
    await telegram.api.sendMessage(chatId, 'You are not linked to any hub yet.');
    return;
  }
  if (hubIds.length === 1) {
    await sendInvite(chatId, userId, hubIds[0] ?? '', telegram, deps);
    return;
  }
  const buttons: InlineKeyboard = [];
  for (const [index, hubId] of hubIds.slice(0, MAX_HUB_BUTTONS).entries()) {
    const name = (await deps.store.getHub(hubId))?.name ?? hubId;
    buttons.push([{ text: name, callback_data: `i:${index}` }]);
  }
  await telegram.api.sendMessage(chatId, 'Which hub should the invitation be for?', { buttons });
}

/** Sends a one-time pairing link for the hub as a QR code (scan) and as text (forward). */
async function sendInvite(
  chatId: number,
  userId: number,
  hubId: string,
  telegram: Telegram,
  deps: Deps,
): Promise<void> {
  const hub = await deps.store.getHub(hubId);
  if (!hub || !(await deps.store.isLinked(userId, hubId))) {
    await telegram.api.sendMessage(chatId, 'Hub not found.');
    return;
  }
  const pairing = await createPairingCode(deps.store, hubId, INVITE_TTL_S, deps.now(), userId);
  const link = pairingLink(await telegram.api.botUsername(), pairing.code);
  await telegram.api.sendPhoto(
    chatId,
    { png: qrPng(link), filename: 'invitation.png' },
    `Invitation to the hub "${hub.name}"\n\n` +
      `Scan the code with the phone camera, or forward this link:\n${link}\n\n` +
      'It works once and expires in 24 hours. Whoever opens it sees all cameras of the hub.',
  );
}

async function handleCallback(
  queryId: string,
  chatId: number,
  userId: number,
  data: string,
  telegram: Telegram,
  deps: Deps,
): Promise<void> {
  const answer = (text?: string) => telegram.api.answerCallbackQuery(queryId, text);

  if (data === 'n:on' || data === 'n:off') {
    const notify = data === 'n:on';
    await updateSettings(userId, { notify }, telegram, deps);
    await answer(notify ? 'Notifications on' : 'Notifications off');
    await showSettings(chatId, userId, telegram, deps);
    return;
  }

  const inviteMatch = /^i:(\d{1,2})$/.exec(data);
  if (inviteMatch) {
    const hubId = (await deps.store.listUserHubIds(userId))[Number(inviteMatch[1])];
    await answer();
    if (hubId) await sendInvite(chatId, userId, hubId, telegram, deps);
    return;
  }

  const videoMatch = /^v:([A-Za-z0-9._-]{1,64}):(\d{1,10})$/.exec(data);
  if (videoMatch) {
    const camera = await deps.store.getCamera(videoMatch[1] ?? '');
    if (!camera || !(await deps.store.isLinked(userId, camera.hubId))) {
      await answer('Camera not found');
      return;
    }
    const result = await requestVideo(camera, Number(videoMatch[2]), deps);
    if (!result.ok) {
      await answer(result.error);
    } else if (!result.created) {
      await answer('This video is already requested.');
    } else {
      await answer('Video requested. The hub fetches it at its next check-in.');
    }
    return;
  }

  await answer();
}
