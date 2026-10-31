/**
 * Thin, typed wrapper around the Telegram Mini App bridge
 * (https://core.telegram.org/bots/webapps).
 *
 * Only the parts the app uses are typed here. Outside Telegram the bridge
 * script still loads, but `initData` is empty — the app then runs as a plain
 * web page (browser preview), which is also the basis for a later PWA.
 */

export interface TelegramUser {
  id: number;
  first_name: string;
  last_name?: string;
  username?: string;
  language_code?: string;
}

interface BackButton {
  show(): void;
  hide(): void;
  onClick(cb: () => void): void;
  offClick(cb: () => void): void;
}

interface HapticFeedback {
  impactOccurred(style: 'light' | 'medium' | 'heavy' | 'rigid' | 'soft'): void;
  notificationOccurred(type: 'error' | 'success' | 'warning'): void;
}

export interface TelegramWebApp {
  initData: string;
  initDataUnsafe: { user?: TelegramUser; start_param?: string };
  colorScheme: 'light' | 'dark';
  platform: string;
  version: string;
  ready(): void;
  expand(): void;
  showAlert(message: string, callback?: () => void): void;
  isVersionAtLeast(version: string): boolean;
  BackButton: BackButton;
  HapticFeedback: HapticFeedback;
}

declare global {
  interface Window {
    Telegram?: { WebApp?: TelegramWebApp };
  }
}

export function getWebApp(): TelegramWebApp | undefined {
  return typeof window === 'undefined' ? undefined : window.Telegram?.WebApp;
}

/** True only when actually opened inside Telegram (signed initData present). */
export function isInTelegram(webApp: TelegramWebApp | undefined = getWebApp()): boolean {
  return Boolean(webApp && webApp.initData.length > 0);
}

/** Raw, signed init data. Sent to the backend, which verifies the signature. */
export function getInitData(webApp: TelegramWebApp | undefined = getWebApp()): string {
  return isInTelegram(webApp) ? (webApp?.initData ?? '') : '';
}

export function getUser(
  webApp: TelegramWebApp | undefined = getWebApp(),
): TelegramUser | undefined {
  return isInTelegram(webApp) ? webApp?.initDataUnsafe.user : undefined;
}

/** Signals Telegram that the app is ready and uses the full available height. */
export function initTelegram(webApp: TelegramWebApp | undefined = getWebApp()): void {
  if (!isInTelegram(webApp) || !webApp) {
    return;
  }
  webApp.ready();
  webApp.expand();
}

/** Shows a native alert inside Telegram, a browser alert otherwise. */
export function notify(message: string, webApp: TelegramWebApp | undefined = getWebApp()): void {
  if (isInTelegram(webApp) && webApp) {
    webApp.showAlert(message);
  } else {
    window.alert(message);
  }
}

export function hapticSuccess(webApp: TelegramWebApp | undefined = getWebApp()): void {
  if (isInTelegram(webApp) && webApp?.isVersionAtLeast('6.1')) {
    webApp.HapticFeedback.notificationOccurred('success');
  }
}
