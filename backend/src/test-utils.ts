import { generateDeviceKey } from './auth/device';
import { signInitData } from './auth/telegram';
import type { Deps } from './api';
import type { ApiRequest } from './http';
import type { MediaStorage } from './storage';
import { MemoryStore } from './store/memory';

export const BOT_TOKEN = '123456:TEST-token-for-unit-tests';
export const NOW = 1_800_000_000;
export const USER_ID = 4242;
export const CAM_ID = 'F4C15606ED3B';

/** Builds init data the way Telegram does, signed with BOT_TOKEN. */
export function makeInitData(userId = USER_ID, authDate = NOW - 60, botToken = BOT_TOKEN): string {
  const params = new URLSearchParams({
    query_id: 'AAH_test',
    user: JSON.stringify({ id: userId, first_name: 'Thomas' }),
    auth_date: String(authDate),
  });
  params.set('hash', signInitData(params, botToken));
  return params.toString();
}

export class FakeStorage implements MediaStorage {
  readonly uploaded = new Set<string>();
  async thumbReadUrl(camId: string, dir: number, num: number) {
    return `https://media.test/get/${camId}/${dir}_${num}.jpg`;
  }
  async thumbUploadUrl(camId: string, dir: number, num: number) {
    return `https://media.test/put/${camId}/${dir}_${num}.jpg`;
  }
  async thumbExists(camId: string, dir: number, num: number) {
    return this.uploaded.has(`${camId}/${dir}_${num}`);
  }
}

export async function setup() {
  const store = new MemoryStore();
  const storage = new FakeStorage();
  const { key, keyHash } = generateDeviceKey(CAM_ID);
  await store.putCamera({ camId: CAM_ID, name: 'Garden', keyHash, lastSeen: 0 });
  await store.linkUser(USER_ID, CAM_ID);
  let nextId = 1;
  const deps: Deps = {
    store,
    storage,
    botToken: async () => BOT_TOKEN,
    now: () => NOW,
    newId: () => `cmd-${nextId++}`,
  };
  return { store, storage, deps, deviceKey: key };
}

export function appRequest(
  method: string,
  path: string,
  body?: unknown,
  initData = makeInitData(),
): ApiRequest {
  return {
    method,
    path,
    headers: { authorization: `tma ${initData}` },
    body: body === undefined ? undefined : JSON.stringify(body),
  };
}

export function deviceRequest(path: string, deviceKey: string, body: unknown): ApiRequest {
  return {
    method: 'POST',
    path,
    headers: { authorization: `Bearer ${deviceKey}` },
    body: JSON.stringify(body),
  };
}
