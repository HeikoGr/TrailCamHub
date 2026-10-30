import { vi } from 'vitest';
import type { TelegramWebApp } from './telegram';

/** Minimal stand-in for window.Telegram.WebApp; empty initData = plain browser. */
export function fakeWebApp(initData = 'query_id=1&hash=abc'): TelegramWebApp {
  return {
    initData,
    initDataUnsafe: initData ? { user: { id: 42, first_name: 'Thomas' } } : {},
    colorScheme: 'light',
    platform: 'ios',
    version: '9.0',
    ready: vi.fn(),
    expand: vi.fn(),
    showAlert: vi.fn(),
    isVersionAtLeast: () => true,
    BackButton: { show: vi.fn(), hide: vi.fn(), onClick: vi.fn(), offClick: vi.fn() },
    HapticFeedback: { impactOccurred: vi.fn(), notificationOccurred: vi.fn() },
  };
}
