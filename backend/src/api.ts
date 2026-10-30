import { randomUUID } from 'node:crypto';
import { parseDeviceKey, verifySecret } from './auth/device';
import { validateInitData } from './auth/telegram';
import {
  HttpError,
  json,
  matchPath,
  parseJsonBody,
  type ApiRequest,
  type ApiResponse,
} from './http';
import type {
  AckRequest,
  Camera,
  CameraRecord,
  CheckinRequest,
  CheckinResponse,
  CommandRecord,
  CommandResult,
  MediaItem,
  MediaRecord,
} from './model';
import { mediaKey } from './model';
import type { MediaStorage } from './storage';
import type { Store } from './store/types';

export interface Deps {
  store: Store;
  storage: MediaStorage;
  /** Resolved lazily so requests that don't need it never touch SSM. */
  botToken: () => Promise<string>;
  now: () => number;
  newId?: () => string;
  config?: { sleepIntervalS?: number; maxThumbUploadsPerCheckin?: number };
}

const DEFAULT_SLEEP_INTERVAL_S = 30 * 60;
const DEFAULT_MAX_THUMB_UPLOADS = 20;
const MAX_MEDIA_PER_CHECKIN = 2_000;

type Handler = (
  req: ApiRequest,
  params: Record<string, string>,
  deps: Deps,
) => Promise<ApiResponse>;

const routes: Array<{ method: string; pattern: string; handler: Handler }> = [
  { method: 'GET', pattern: '/v1/health', handler: async () => json(200, { ok: true }) },
  { method: 'GET', pattern: '/v1/cameras', handler: listCameras },
  { method: 'GET', pattern: '/v1/cameras/:camId/media', handler: listMedia },
  { method: 'POST', pattern: '/v1/cameras/:camId/commands', handler: createCommand },
  { method: 'POST', pattern: '/v1/device/checkin', handler: checkin },
  { method: 'POST', pattern: '/v1/device/ack', handler: ack },
];

export async function handleRequest(req: ApiRequest, deps: Deps): Promise<ApiResponse> {
  try {
    let pathMatched = false;
    for (const route of routes) {
      const params = matchPath(route.pattern, req.path);
      if (!params) continue;
      pathMatched = true;
      if (route.method === req.method) return await route.handler(req, params, deps);
    }
    throw new HttpError(pathMatched ? 405 : 404, pathMatched ? 'Method not allowed' : 'Not found');
  } catch (error) {
    if (error instanceof HttpError) return json(error.statusCode, { error: error.message });
    console.error('Unhandled error', error);
    return json(500, { error: 'Internal error' });
  }
}

// ---- Authentication ----------------------------------------------------------

function bearer(req: ApiRequest, scheme: string): string {
  const header = req.headers.authorization ?? '';
  const prefix = `${scheme} `;
  if (!header.startsWith(prefix)) throw new HttpError(401, `Expected Authorization: ${scheme} …`);
  return header.slice(prefix.length).trim();
}

async function authUser(req: ApiRequest, deps: Deps): Promise<number> {
  const result = validateInitData(bearer(req, 'tma'), await deps.botToken(), deps.now());
  if (!result.ok) throw new HttpError(401, `Invalid Telegram init data (${result.reason})`);
  return result.identity.userId;
}

async function authUserForCamera(
  req: ApiRequest,
  camId: string,
  deps: Deps,
): Promise<CameraRecord> {
  const userId = await authUser(req, deps);
  const camera = (await deps.store.isLinked(userId, camId))
    ? await deps.store.getCamera(camId)
    : undefined;
  // Same answer for "unknown" and "not yours", so camera ids cannot be probed.
  if (!camera) throw new HttpError(404, 'Camera not found');
  return camera;
}

async function authDevice(req: ApiRequest, deps: Deps): Promise<CameraRecord> {
  const parsed = parseDeviceKey(bearer(req, 'Bearer'));
  const camera = parsed && (await deps.store.getCamera(parsed.camId));
  if (!parsed || !camera || !verifySecret(parsed.secret, camera.keyHash)) {
    throw new HttpError(401, 'Invalid device key');
  }
  return camera;
}

// ---- App endpoints -------------------------------------------------------------

function toCamera(record: CameraRecord): Camera {
  return {
    camId: record.camId,
    name: record.name,
    model: record.model ?? 'unknown',
    firmware: record.firmware ?? 'unknown',
    batPercent: record.batPercent ?? 0,
    sdFreeMB: record.sdFreeMB ?? 0,
    hubBatPercent: record.hubBatPercent,
    lastSeen: record.lastSeen,
  };
}

async function listCameras(req: ApiRequest, _p: Record<string, string>, deps: Deps) {
  const userId = await authUser(req, deps);
  const ids = await deps.store.listUserCameraIds(userId);
  const records = await Promise.all(ids.map((id) => deps.store.getCamera(id)));
  return json(200, records.filter((r): r is CameraRecord => r !== undefined).map(toCamera));
}

async function listMedia(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  const camera = await authUserForCamera(req, params.camId ?? '', deps);
  const media = (await deps.store.listMedia(camera.camId))
    .filter((m) => m.thumbPresent)
    .sort((a, b) => b.mediaTime - a.mediaTime || b.num - a.num);
  const items: MediaItem[] = await Promise.all(
    media.map(async (m) => ({
      dir: m.dir,
      num: m.num,
      fileType: m.fileType,
      mediaTime: m.mediaTime,
      videoState: m.videoState,
      thumbUrl: await deps.storage.thumbReadUrl(camera.camId, m.dir, m.num),
    })),
  );
  return json(200, items);
}

async function createCommand(req: ApiRequest, params: Record<string, string>, deps: Deps) {
  const camera = await authUserForCamera(req, params.camId ?? '', deps);
  const body = parseJsonBody<{ type?: unknown; dir?: unknown; num?: unknown }>(req);
  if (body.type !== 'requestVideo') throw new HttpError(400, 'Unsupported command type');
  if (!Number.isInteger(body.dir) || !Number.isInteger(body.num)) {
    throw new HttpError(400, 'dir and num must be integers');
  }
  const dir = body.dir as number;
  const num = body.num as number;

  const media = await deps.store.getMedia(camera.camId, dir, num);
  if (!media) throw new HttpError(404, 'Recording not found');
  if (media.fileType !== 2) throw new HttpError(409, 'Recording is not a video');

  // Re-requesting a pending video returns the existing command (idempotent).
  const pending = (await deps.store.listCommands(camera.camId)).find(
    (c) =>
      c.type === 'requestVideo' &&
      c.dir === dir &&
      c.num === num &&
      (c.state === 'queued' || c.state === 'delivered'),
  );
  if (pending) return json(200, commandResult(pending));

  const now = deps.now();
  const command: CommandRecord = {
    id: (deps.newId ?? randomUUID)(),
    type: 'requestVideo',
    dir,
    num,
    state: 'queued',
    created: now,
    updated: now,
  };
  await deps.store.putCommand(camera.camId, command);
  await deps.store.setVideoState(camera.camId, dir, num, 'requested');
  return json(201, commandResult(command));
}

function commandResult(c: CommandRecord): CommandResult {
  return { id: c.id, type: c.type, state: c.state };
}

// ---- Device endpoints ----------------------------------------------------------

function optionalNumber(value: unknown): number | undefined {
  return typeof value === 'number' && Number.isFinite(value) ? value : undefined;
}

function optionalString(value: unknown): string | undefined {
  return typeof value === 'string' && value.length <= 64 ? value : undefined;
}

function sanitizeMedia(raw: CheckinRequest['media']): MediaRecord[] {
  if (raw === undefined) return [];
  if (!Array.isArray(raw)) throw new HttpError(400, 'media must be an array');
  if (raw.length > MAX_MEDIA_PER_CHECKIN) throw new HttpError(413, 'Too many media entries');
  return raw.map((m, i) => {
    const ok =
      Number.isInteger(m?.dir) &&
      Number.isInteger(m?.num) &&
      (m?.fileType === 0 || m?.fileType === 2) &&
      Number.isInteger(m?.mediaTime);
    if (!ok) throw new HttpError(400, `Invalid media entry at index ${i}`);
    return {
      dir: m.dir,
      num: m.num,
      fileType: m.fileType as 0 | 2,
      mediaTime: m.mediaTime,
      thumbPresent: false,
      videoState: 'none',
    };
  });
}

async function checkin(req: ApiRequest, _p: Record<string, string>, deps: Deps) {
  const camera = await authDevice(req, deps);
  const body = parseJsonBody<CheckinRequest>(req);
  const now = deps.now();

  await deps.store.updateTelemetry(
    camera.camId,
    {
      hubFirmware: optionalString(body.hubFirmware),
      hubBatPercent: optionalNumber(body.hubBatPercent),
      model: optionalString(body.camera?.model),
      firmware: optionalString(body.camera?.firmware),
      batPercent: optionalNumber(body.camera?.batPercent),
      sdFreeMB: optionalNumber(body.camera?.sdFreeMB),
    },
    now,
  );

  // Only new recordings are written; existing ones keep their flags.
  const reported = sanitizeMedia(body.media);
  const known = new Map((await deps.store.listMedia(camera.camId)).map((m) => [mediaKey(m), m]));
  const fresh = reported.filter((m) => !known.has(mediaKey(m)));
  await deps.store.addMedia(camera.camId, fresh);

  // Newest first, so the latest pictures reach the phone first.
  const maxUploads = deps.config?.maxThumbUploadsPerCheckin ?? DEFAULT_MAX_THUMB_UPLOADS;
  const missing = reported
    .filter((m) => !(known.get(mediaKey(m))?.thumbPresent ?? false))
    .sort((a, b) => b.mediaTime - a.mediaTime || b.num - a.num)
    .slice(0, maxUploads);
  const thumbUploads = await Promise.all(
    missing.map(async (m) => ({
      dir: m.dir,
      num: m.num,
      url: await deps.storage.thumbUploadUrl(camera.camId, m.dir, m.num),
    })),
  );

  // Hand out open commands; a delivered command is re-sent until it is acknowledged.
  const open = (await deps.store.listCommands(camera.camId)).filter(
    (c) => c.state === 'queued' || c.state === 'delivered',
  );
  for (const command of open.filter((c) => c.state === 'queued')) {
    await deps.store.putCommand(camera.camId, { ...command, state: 'delivered', updated: now });
    await deps.store.setVideoState(camera.camId, command.dir, command.num, 'downloading');
  }

  const response: CheckinResponse = {
    serverTime: now,
    thumbUploads,
    commands: open.map((c) => ({ id: c.id, type: c.type, dir: c.dir, num: c.num })),
    config: { sleepIntervalS: deps.config?.sleepIntervalS ?? DEFAULT_SLEEP_INTERVAL_S },
  };
  return json(200, response);
}

async function ack(req: ApiRequest, _p: Record<string, string>, deps: Deps) {
  const camera = await authDevice(req, deps);
  const body = parseJsonBody<AckRequest>(req);
  const now = deps.now();

  let thumbsConfirmed = 0;
  for (const t of Array.isArray(body.uploadedThumbs) ? body.uploadedThumbs : []) {
    if (!Number.isInteger(t?.dir) || !Number.isInteger(t?.num)) continue;
    // Trust S3, not the device: only mark thumbnails that really arrived.
    if (await deps.storage.thumbExists(camera.camId, t.dir, t.num)) {
      await deps.store.markThumbPresent(camera.camId, t.dir, t.num);
      thumbsConfirmed++;
    }
  }

  let commandsUpdated = 0;
  for (const c of Array.isArray(body.commands) ? body.commands : []) {
    if (typeof c?.id !== 'string' || (c.state !== 'done' && c.state !== 'failed')) continue;
    const command = await deps.store.getCommand(camera.camId, c.id);
    if (!command || command.state === 'done' || command.state === 'failed') continue;
    await deps.store.putCommand(camera.camId, {
      ...command,
      state: c.state,
      updated: now,
      error: c.state === 'failed' ? optionalString(c.error) : undefined,
    });
    if (command.type === 'requestVideo') {
      await deps.store.setVideoState(
        camera.camId,
        command.dir,
        command.num,
        c.state === 'done' ? 'available' : 'failed',
      );
    }
    commandsUpdated++;
  }

  return json(200, { thumbsConfirmed, commandsUpdated });
}
