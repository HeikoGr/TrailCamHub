import { createHmac } from 'node:crypto';

/** One button of an inline keyboard (https://core.telegram.org/bots/api#inlinekeyboardbutton). */
export interface InlineButton {
  text: string;
  /** At most 64 bytes. */
  callback_data?: string;
  url?: string;
  /** Opens the Mini App; works in private chats only. */
  web_app?: { url: string };
}

export type InlineKeyboard = InlineButton[][];

export interface SendOptions {
  buttons?: InlineKeyboard;
  /** Deliver without sound (quiet hours). */
  silent?: boolean;
}

/** A PNG made by the backend, sent as a file upload. */
export interface PhotoUpload {
  png: Buffer;
  filename: string;
}

/** The part of the Telegram Bot API the backend uses. */
export interface TelegramApi {
  /** The bot's username (without @), for t.me links. */
  botUsername(): Promise<string>;
  sendMessage(chatId: number, text: string, options?: SendOptions): Promise<void>;
  /** `photo` is a URL Telegram downloads itself, or an upload. */
  sendPhoto(
    chatId: number,
    photo: string | PhotoUpload,
    caption: string,
    options?: SendOptions,
  ): Promise<void>;
  answerCallbackQuery(callbackQueryId: string, text?: string): Promise<void>;
}

export class TelegramError extends Error {
  constructor(
    /** Telegram's error_code (HTTP status), e.g. 403 when the user blocked the bot. */
    readonly code: number,
    message: string,
    readonly retryAfterS?: number,
  ) {
    super(message);
    this.name = 'TelegramError';
  }
}

/** Longest wait for a rate limit (429) before giving up on a message. */
const MAX_RETRY_WAIT_S = 5;
/** Telegram's limit for message texts. */
const MAX_TEXT_LENGTH = 4096;
const MAX_CAPTION_LENGTH = 1024;

/**
 * Secret Telegram sends in the X-Telegram-Bot-Api-Secret-Token header of every
 * webhook request. Derived from the bot token, so no extra secret has to be
 * stored; the deploy workflow computes the same value for setWebhook:
 * `printf %s "$TOKEN" | openssl dgst -sha256 -hmac trailcamhub-webhook`.
 */
export function webhookSecret(botToken: string): string {
  return createHmac('sha256', 'trailcamhub-webhook').update(botToken).digest('hex');
}

function truncate(text: string, max: number): string {
  return text.length <= max ? text : `${text.slice(0, max - 1)}…`;
}

function markup(options: SendOptions | undefined): Record<string, unknown> {
  return {
    ...(options?.buttons ? { reply_markup: { inline_keyboard: options.buttons } } : {}),
    ...(options?.silent ? { disable_notification: true } : {}),
  };
}

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

/** Calls the Bot API over HTTPS. */
export class HttpTelegramApi implements TelegramApi {
  private username?: Promise<string>;

  constructor(
    private readonly token: () => Promise<string>,
    private readonly fetchFn: typeof fetch = (...args) => fetch(...args),
  ) {}

  botUsername(): Promise<string> {
    this.username ??= this.call<{ username?: string }>('getMe', {})
      .then((me) => {
        if (!me.username) throw new Error('getMe returned no username');
        return me.username;
      })
      .catch((error: unknown) => {
        this.username = undefined;
        throw error;
      });
    return this.username;
  }

  async sendMessage(chatId: number, text: string, options?: SendOptions): Promise<void> {
    await this.call('sendMessage', {
      chat_id: chatId,
      text: truncate(text, MAX_TEXT_LENGTH),
      link_preview_options: { is_disabled: true },
      ...markup(options),
    });
  }

  async sendPhoto(
    chatId: number,
    photo: string | PhotoUpload,
    caption: string,
    options?: SendOptions,
  ): Promise<void> {
    const fields = {
      chat_id: chatId,
      caption: truncate(caption, MAX_CAPTION_LENGTH),
      ...markup(options),
    };
    if (typeof photo === 'string') {
      await this.call('sendPhoto', { ...fields, photo });
      return;
    }
    const form = new FormData();
    for (const [key, value] of Object.entries(fields)) {
      form.set(key, typeof value === 'string' ? value : JSON.stringify(value));
    }
    form.set('photo', new Blob([new Uint8Array(photo.png)], { type: 'image/png' }), photo.filename);
    await this.call('sendPhoto', form);
  }

  async answerCallbackQuery(callbackQueryId: string, text?: string): Promise<void> {
    await this.call('answerCallbackQuery', {
      callback_query_id: callbackQueryId,
      ...(text ? { text } : {}),
    });
  }

  /** One request; waits once for a short rate limit (429 with retry_after). */
  private async call<T>(method: string, body: Record<string, unknown> | FormData): Promise<T> {
    for (let attempt = 0; ; attempt++) {
      try {
        return await this.request<T>(method, body);
      } catch (error) {
        const wait = error instanceof TelegramError ? error.retryAfterS : undefined;
        if (attempt > 0 || wait === undefined || wait > MAX_RETRY_WAIT_S) throw error;
        await sleep(wait * 1000);
      }
    }
  }

  private async request<T>(method: string, body: Record<string, unknown> | FormData): Promise<T> {
    const response = await this.fetchFn(
      `https://api.telegram.org/bot${await this.token()}/${method}`,
      body instanceof FormData
        ? { method: 'POST', body }
        : {
            method: 'POST',
            headers: { 'content-type': 'application/json' },
            body: JSON.stringify(body),
          },
    );
    let result: {
      ok?: boolean;
      result?: T;
      error_code?: number;
      description?: string;
      parameters?: { retry_after?: number };
    };
    try {
      result = (await response.json()) as typeof result;
    } catch {
      throw new TelegramError(response.status, `${method}: HTTP ${response.status}`);
    }
    if (!result.ok) {
      throw new TelegramError(
        result.error_code ?? response.status,
        `${method}: ${result.description ?? `HTTP ${response.status}`}`,
        result.parameters?.retry_after,
      );
    }
    return result.result as T;
  }
}
