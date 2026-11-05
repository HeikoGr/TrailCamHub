import { randomInt } from 'node:crypto';
import type { HubRecord, PairingCode } from './model';
import type { Store } from './store/types';

/**
 * Pairing links a Telegram user to a hub without registering them by hand:
 * someone who already has access (an admin, a linked user with /invite, or
 * the hub itself) creates a one-time code, shared as a link or QR code
 * `https://t.me/<bot>?start=<code>`. Opening it sends `/start <code>` to the
 * bot, which redeems the code and links the user.
 */

const ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789';
const CODE_LENGTH = 12; // about 70 bits

/** Telegram's start parameter allows A-Z a-z 0-9 _ - (up to 64 characters). */
export const PAIRING_CODE_PATTERN = /^[A-Za-z0-9]{12}$/;

/** Codes made with /invite: valid for a day. */
export const INVITE_TTL_S = 24 * 60 * 60;
/** Codes the hub shows (display, web UI): short, they are made on demand. */
export const HUB_CODE_TTL_S = 30 * 60;
/** Codes made by an admin (register-hub --pair): a week, they may travel by mail. */
export const ADMIN_CODE_TTL_S = 7 * 24 * 60 * 60;

export function newPairingCode(): string {
  let code = '';
  for (let i = 0; i < CODE_LENGTH; i++) code += ALPHABET[randomInt(ALPHABET.length)];
  return code;
}

export async function createPairingCode(
  store: Store,
  hubId: string,
  ttlS: number,
  now: number,
  createdBy?: number,
): Promise<PairingCode> {
  const pairing: PairingCode = { code: newPairingCode(), hubId, expiresAt: now + ttlS, createdBy };
  await store.putPairingCode(pairing);
  return pairing;
}

export function pairingLink(botUsername: string, code: string): string {
  return `https://t.me/${botUsername}?start=${code}`;
}

export type RedeemResult =
  | { ok: true; hub: HubRecord; alreadyLinked: boolean }
  | { ok: false; reason: 'invalid' | 'expired' };

/** Links the user to the code's hub. A code works once, also when it fails as expired. */
export async function redeemPairingCode(
  store: Store,
  code: string,
  userId: number,
  now: number,
): Promise<RedeemResult> {
  if (!PAIRING_CODE_PATTERN.test(code)) return { ok: false, reason: 'invalid' };
  const pairing = await store.takePairingCode(code);
  if (!pairing) return { ok: false, reason: 'invalid' };
  if (pairing.expiresAt < now) return { ok: false, reason: 'expired' };
  const hub = await store.getHub(pairing.hubId);
  if (!hub) return { ok: false, reason: 'invalid' };
  const alreadyLinked = await store.isLinked(userId, hub.hubId);
  if (!alreadyLinked) await store.linkUser(userId, hub.hubId);
  return { ok: true, hub, alreadyLinked };
}
