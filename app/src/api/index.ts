import { getInitData } from '../telegram';
import { DemoApiClient } from './demo';
import { HttpApiClient } from './http';
import type { ApiClient } from './types';

export type { ApiClient } from './types';

/**
 * Chooses the backend: the real API when `VITE_API_BASE_URL` is set at build
 * time, otherwise the in-memory demo client.
 */
export function createApiClient(
  baseUrl: string | undefined = import.meta.env.VITE_API_BASE_URL,
  initData: string = getInitData(),
): ApiClient {
  const url = baseUrl?.trim();
  return url ? new HttpApiClient(url, initData) : new DemoApiClient();
}
