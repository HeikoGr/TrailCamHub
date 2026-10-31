/** Minimal HTTP abstractions so the API logic stays independent of Lambda. */

export interface ApiRequest {
  method: string;
  path: string;
  /** Lower-case header names. */
  headers: Record<string, string | undefined>;
  body?: string;
}

export interface ApiResponse {
  statusCode: number;
  headers: Record<string, string>;
  body: string;
}

export class HttpError extends Error {
  constructor(
    readonly statusCode: number,
    message: string,
  ) {
    super(message);
    this.name = 'HttpError';
  }
}

export function json(statusCode: number, body: unknown): ApiResponse {
  return {
    statusCode,
    headers: { 'content-type': 'application/json', 'cache-control': 'no-store' },
    body: JSON.stringify(body),
  };
}

export function parseJsonBody<T>(request: ApiRequest): T {
  if (!request.body) throw new HttpError(400, 'Missing request body');
  try {
    return JSON.parse(request.body) as T;
  } catch {
    throw new HttpError(400, 'Request body is not valid JSON');
  }
}

/** Matches `/v1/cameras/:camId/media` style patterns; returns params or undefined. */
export function matchPath(pattern: string, path: string): Record<string, string> | undefined {
  const want = pattern.split('/');
  const got = path.replace(/\/+$/, '').split('/');
  if (want.length !== got.length) return undefined;
  const params: Record<string, string> = {};
  for (let i = 0; i < want.length; i++) {
    const w = want[i] ?? '';
    const g = got[i] ?? '';
    if (w.startsWith(':')) {
      if (!g) return undefined;
      params[w.slice(1)] = decodeURIComponent(g);
    } else if (w !== g) {
      return undefined;
    }
  }
  return params;
}
