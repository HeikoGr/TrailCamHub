import type { ApiClient, Camera, CommandResult, MediaItem } from './types';

export class ApiError extends Error {
  constructor(
    readonly status: number,
    message: string,
  ) {
    super(message);
    this.name = 'ApiError';
  }
}

/**
 * Talks to the TrailCamHub backend.
 *
 * Authentication: the signed Telegram init data is sent as
 * `Authorization: tma <initData>`; the backend verifies its HMAC signature
 * with the bot token. The app itself never holds a secret.
 */
export class HttpApiClient implements ApiClient {
  readonly isDemo = false;
  private readonly baseUrl: string;

  constructor(
    baseUrl: string,
    private readonly initData: string,
    private readonly fetchFn: typeof fetch = (...args) => fetch(...args),
  ) {
    this.baseUrl = baseUrl.replace(/\/+$/, '');
  }

  listCameras(): Promise<Camera[]> {
    return this.request<Camera[]>('GET', '/v1/cameras');
  }

  listMedia(camId: string): Promise<MediaItem[]> {
    return this.request<MediaItem[]>('GET', `/v1/cameras/${encodeURIComponent(camId)}/media`);
  }

  requestVideo(camId: string, item: Pick<MediaItem, 'dir' | 'num'>): Promise<CommandResult> {
    return this.request<CommandResult>(
      'POST',
      `/v1/cameras/${encodeURIComponent(camId)}/commands`,
      {
        type: 'requestVideo',
        dir: item.dir,
        num: item.num,
      },
    );
  }

  private async request<T>(method: string, path: string, body?: unknown): Promise<T> {
    const headers: Record<string, string> = { Accept: 'application/json' };
    if (this.initData) {
      headers.Authorization = `tma ${this.initData}`;
    }
    if (body !== undefined) {
      headers['Content-Type'] = 'application/json';
    }
    const response = await this.fetchFn(this.baseUrl + path, {
      method,
      headers,
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    if (!response.ok) {
      throw new ApiError(response.status, `${method} ${path} failed with HTTP ${response.status}`);
    }
    return (await response.json()) as T;
  }
}
