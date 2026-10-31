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
  CameraCheckin,
  CameraRecord,
  CameraReport,
  CheckinRequest,
  CheckinResponse,
  CommandRecord,
  CommandResult,
  HubRecord,
  MediaItem,
  MediaRecord,
} from './model';
import { ID_PATTERN } from './model';
import type { MediaStorage } from './storage';
import type { Store } from './store/types';

export interface Deps {
  store: Store;
  storage: MediaStorage;
  /** Resolved lazily so requests that don't need it never touch SSM. */
  botToken: () => Promise<string>;
  now: () => number;
  newId?: () => string;
  config?: {
    sleepIntervalS?: number;
    /** Thumbnail upload URLs handed out per check-in, over all cameras. */
    maxThumbUploadsPerCheckin?: number;
  };
}

const DEFAULT_SLEEP_INTERVAL_S = 30 * 60;
const DEFAULT_MAX_THUMB_UPLOADS = 30;
const MAX_CAMERAS_PER_CHECKIN = 16;
const MAX_MEDIA_PER_CHECKIN = 2_000;
/** A hub clock may lag; readAt values further in the future are ignored. */
const MAX_CLOCK_SKEW_S = 5 * 60;
const MAX_READ_AGE_S = 30 * 24 * 60 * 60;

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
  const camera = await deps.store.getCamera(camId);
  // Same answer for "unknown" and "not yours", so camera ids cannot be probed.
  if (!camera || !(await deps.store.isLinked(userId, camera.hubId))) {
    throw new HttpError(404, 'Camera not found');
  }
  return camera;
}

async function authHub(req: ApiRequest, deps: Deps): Promise<HubRecord> {
  const parsed = parseDeviceKey(bearer(req, 'Bearer'));
  const hub = parsed && (await deps.store.getHub(parsed.id));
  if (!parsed || !hub || !verifySecret(parsed.secret, hub.keyHash)) {
    throw new HttpError(401, 'Invalid hub key');
  }
  return hub;
}

// ---- App endpoints -------------------------------------------------------------

function toCamera(record: CameraRecord, hub: HubRecord | undefined): Camera {
  return {
    camId: record.camId,
    name: record.name,
    hubId: record.hubId,
    hubName: hub?.name ?? record.hubId,
    model: record.model ?? 'unknown',
    firmware: record.firmware ?? 'unknown',
    batPercent: record.batPercent ?? 0,
    sdFreeMB: record.sdFreeMB ?? 0,
    hubBatPercent: hub?.batPercent,
    lastSeen: record.lastSeen,
  };
}

async function listCameras(req: ApiRequest, _p: Record<string, string>, deps: Deps) {
  const userId = await authUser(req, deps);
  const cameras: Camera[] = [];
  for (const hubId of await deps.store.listUserHubIds(userId)) {
    const hub = await deps.store.getHub(hubId);
    const ids = await deps.store.listHubCameraIds(hubId);
    const records = await Promise.all(ids.map((id) => deps.store.getCamera(id)));
    for (const record of records) {
      // A camera listed under a hub it no longer belongs to is skipped.
      if (record && record.hubId === hubId) cameras.push(toCamera(record, hub));
    }
  }
  // Most recently read first.
  cameras.sort((a, b) => b.lastSeen - a.lastSeen || a.name.localeCompare(b.name));
  return json(200, cameras);
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

// ---- Hub endpoints -------------------------------------------------------------

function optionalNumber(value: unknown): number | undefined {
  return typeof value === 'number' && Number.isFinite(value) ? value : undefined;
}

function optionalString(value: unknown): string | undefined {
  return typeof value === 'string' && value.length <= 64 ? value : undefined;
}

function sanitizeMedia(raw: CameraReport['media'], camId: string): MediaRecord[] {
  if (raw === undefined) return [];
  if (!Array.isArray(raw)) throw new HttpError(400, `media of ${camId} must be an array`);
  return raw.map((m, i) => {
    const ok =
      Number.isInteger(m?.dir) &&
      Number.isInteger(m?.num) &&
      (m?.fileType === 0 || m?.fileType === 2) &&
      Number.isInteger(m?.mediaTime);
    if (!ok) throw new HttpError(400, `Invalid media entry ${i} of camera ${camId}`);
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

/** Validates the whole request before anything is stored. */
function parseCameraReports(
  body: CheckinRequest,
): Array<CameraReport & { records: MediaRecord[] }> {
  const raw = body.cameras ?? [];
  if (!Array.isArray(raw)) throw new HttpError(400, 'cameras must be an array');
  if (raw.length > MAX_CAMERAS_PER_CHECKIN) throw new HttpError(413, 'Too many cameras');
  const seen = new Set<string>();
  let mediaCount = 0;
  const reports = raw.map((report, i) => {
    const camId = report?.camId;
    if (typeof camId !== 'string' || !ID_PATTERN.test(camId)) {
      throw new HttpError(400, `Invalid camId at index ${i}`);
    }
    if (seen.has(camId)) throw new HttpError(400, `Camera ${camId} reported twice`);
    seen.add(camId);
    const records = sanitizeMedia(report.media, camId);
    mediaCount += records.length;
    return { ...report, camId, records };
  });
  if (mediaCount > MAX_MEDIA_PER_CHECKIN) throw new HttpError(413, 'Too many media entries');
  return reports;
}

/** When the hub read the camera: its own timestamp if plausible, else now. */
function readTime(readAt: unknown, now: number): number {
  const t = optionalNumber(readAt);
  return t !== undefined && t <= now + MAX_CLOCK_SKEW_S && t >= now - MAX_READ_AGE_S ? t : now;
}

async function checkin(req: ApiRequest, _p: Record<string, string>, deps: Deps) {
  const hub = await authHub(req, deps);
  const body = parseJsonBody<CheckinRequest>(req);
  const reports = parseCameraReports(body);
  const now = deps.now();

  await deps.store.updateHub(
    hub.hubId,
    {
      firmware: optionalString(body.hubFirmware),
      batPercent: optionalNumber(body.hubBatPercent),
    },
    now,
  );

  const results = new Map<string, CameraCheckin>();
  for (const report of reports) {
    const { camera, created } = await deps.store.claimCamera(report.camId, hub.hubId);
    if (camera.hubId !== hub.hubId) {
      // Cameras stay with the hub that reported them first.
      results.set(report.camId, emptyResult(report.camId, false, false));
      continue;
    }
    await deps.store.updateCamera(
      camera.camId,
      {
        model: optionalString(report.model),
        firmware: optionalString(report.firmware),
        batPercent: optionalNumber(report.batPercent),
        sdFreeMB: optionalNumber(report.sdFreeMB),
      },
      readTime(report.readAt, now),
    );
    // Only new recordings are written; existing ones keep their flags.
    await deps.store.addMedia(camera.camId, report.records);
    // A new camera may have been reported with a partial (delta) list.
    results.set(camera.camId, emptyResult(camera.camId, true, created));
  }

  // Every camera of the hub can have work: missing thumbnails and commands
  // also for cameras the hub did not read in this cycle.
  const work: Array<{ camId: string; dir: number; num: number; mediaTime: number }> = [];
  for (const camId of await deps.store.listHubCameraIds(hub.hubId)) {
    const camera = await deps.store.getCamera(camId);
    if (!camera || camera.hubId !== hub.hubId) continue;
    const result = results.get(camId) ?? emptyResult(camId, true, false);

    for (const m of await deps.store.listMedia(camId)) {
      if (!m.thumbPresent) work.push({ camId, dir: m.dir, num: m.num, mediaTime: m.mediaTime });
    }

    // Hand out open commands; a delivered command is re-sent until acknowledged.
    const open = (await deps.store.listCommands(camId)).filter(
      (c) => c.state === 'queued' || c.state === 'delivered',
    );
    for (const command of open.filter((c) => c.state === 'queued')) {
      await deps.store.putCommand(camId, { ...command, state: 'delivered', updated: now });
      await deps.store.setVideoState(camId, command.dir, command.num, 'downloading');
    }
    result.commands = open.map((c) => ({ id: c.id, type: c.type, dir: c.dir, num: c.num }));
    results.set(camId, result);
  }

  // Newest thumbnails first over all cameras, so the latest pictures reach
  // the phone first.
  const maxUploads = deps.config?.maxThumbUploadsPerCheckin ?? DEFAULT_MAX_THUMB_UPLOADS;
  work.sort((a, b) => b.mediaTime - a.mediaTime || b.num - a.num);
  for (const m of work.slice(0, maxUploads)) {
    results.get(m.camId)?.thumbUploads.push({
      dir: m.dir,
      num: m.num,
      url: await deps.storage.thumbUploadUrl(m.camId, m.dir, m.num),
    });
  }

  const response: CheckinResponse = {
    serverTime: now,
    // Reported cameras plus every other camera that has something to do.
    cameras: [...results.values()].filter(
      (r) =>
        reports.some((report) => report.camId === r.camId) ||
        r.thumbUploads.length > 0 ||
        r.commands.length > 0,
    ),
    config: { sleepIntervalS: deps.config?.sleepIntervalS ?? DEFAULT_SLEEP_INTERVAL_S },
  };
  return json(200, response);
}

function emptyResult(camId: string, accepted: boolean, resync: boolean): CameraCheckin {
  return { camId, accepted, resync, thumbUploads: [], commands: [] };
}

async function ack(req: ApiRequest, _p: Record<string, string>, deps: Deps) {
  const hub = await authHub(req, deps);
  const body = parseJsonBody<AckRequest>(req);
  const now = deps.now();

  let thumbsConfirmed = 0;
  let commandsUpdated = 0;
  for (const entry of Array.isArray(body.cameras) ? body.cameras : []) {
    if (typeof entry?.camId !== 'string') continue;
    const camera = await deps.store.getCamera(entry.camId);
    // Acknowledgements for cameras of other hubs are ignored.
    if (!camera || camera.hubId !== hub.hubId) continue;

    for (const t of Array.isArray(entry.uploadedThumbs) ? entry.uploadedThumbs : []) {
      if (!Number.isInteger(t?.dir) || !Number.isInteger(t?.num)) continue;
      // Trust S3, not the device: only mark thumbnails that really arrived.
      if (await deps.storage.thumbExists(camera.camId, t.dir, t.num)) {
        await deps.store.markThumbPresent(camera.camId, t.dir, t.num);
        thumbsConfirmed++;
      }
    }

    for (const c of Array.isArray(entry.commands) ? entry.commands : []) {
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
  }

  return json(200, { thumbsConfirmed, commandsUpdated });
}
