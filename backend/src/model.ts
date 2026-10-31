/**
 * Domain types and the API v1 wire format.
 *
 * A **hub** is a TrailCamHub module; it authenticates with its own key and
 * reports for one or more **cameras**. Cameras are created automatically the
 * first time a hub reports them and then belong to that hub. Users are linked
 * to hubs and see all cameras of their hubs.
 *
 * App-facing shapes (`Camera`, `MediaItem`, `CommandResult`) mirror
 * app/src/api/types.ts.
 */

export type FileType = 0 | 2;
export type VideoState = 'none' | 'requested' | 'downloading' | 'available' | 'failed';
export type CommandType = 'requestVideo';
export type CommandState = 'queued' | 'delivered' | 'done' | 'failed';

export interface HubRecord {
  hubId: string;
  name: string;
  /** SHA-256 (hex) of the hub key secret; the secret itself is never stored. */
  keyHash: string;
  firmware?: string;
  batPercent?: number;
  /** Last check-in, Unix seconds; 0 = never. */
  lastSeen: number;
}

export interface CameraRecord {
  camId: string;
  hubId: string;
  name: string;
  model?: string;
  firmware?: string;
  batPercent?: number;
  sdFreeMB?: number;
  /** When the hub last read the camera, Unix seconds. */
  lastSeen: number;
}

export interface MediaRecord {
  dir: number;
  num: number;
  fileType: FileType;
  mediaTime: number;
  thumbPresent: boolean;
  videoState: VideoState;
}

export interface CommandRecord {
  id: string;
  type: CommandType;
  dir: number;
  num: number;
  state: CommandState;
  created: number;
  updated: number;
  error?: string;
}

// ---- App API ---------------------------------------------------------------

export interface Camera {
  camId: string;
  name: string;
  hubId: string;
  hubName: string;
  model: string;
  firmware: string;
  batPercent: number;
  sdFreeMB: number;
  hubBatPercent?: number;
  lastSeen: number;
}

export interface MediaItem {
  dir: number;
  num: number;
  fileType: FileType;
  mediaTime: number;
  thumbUrl: string;
  videoState: VideoState;
}

export interface CommandResult {
  id: string;
  type: CommandType;
  state: CommandState;
}

// ---- Hub (device) API ------------------------------------------------------

export interface MediaEntry {
  dir: number;
  num: number;
  fileType: number;
  mediaTime: number;
}

export interface CameraReport {
  camId: string;
  /** When the hub read the camera (hub clock, Unix seconds); optional. */
  readAt?: number;
  model?: string;
  firmware?: string;
  batPercent?: number;
  sdFreeMB?: number;
  /**
   * Media not reported before (delta). The backend only adds entries, so
   * sending an item again is harmless.
   */
  media?: MediaEntry[];
}

export interface CheckinRequest {
  hubFirmware?: string;
  hubBatPercent?: number;
  cameras?: CameraReport[];
}

export interface CameraCheckin {
  camId: string;
  /** False if the camera belongs to another hub; nothing was stored for it. */
  accepted: boolean;
  /**
   * The backend has no media for this camera yet (new camera): the hub should
   * report its full media list at the next check-in.
   */
  resync: boolean;
  /** Missing thumbnails: PUT the JPEG to `url`, then report it via /ack. */
  thumbUploads: Array<{ dir: number; num: number; url: string }>;
  commands: Array<{ id: string; type: CommandType; dir: number; num: number }>;
}

export interface CheckinResponse {
  serverTime: number;
  cameras: CameraCheckin[];
  config: { sleepIntervalS: number };
}

export interface CameraAck {
  camId: string;
  uploadedThumbs?: Array<{ dir: number; num: number }>;
  commands?: Array<{ id: string; state: 'done' | 'failed'; error?: string }>;
}

export interface AckRequest {
  cameras?: CameraAck[];
}

export function mediaKey(item: { dir: number; num: number }): string {
  return `${item.dir}_${item.num}`;
}

/** Ids of hubs and cameras: safe as S3 key and URL path segments. */
export const ID_PATTERN = /^[A-Za-z0-9_-][A-Za-z0-9._-]{0,63}$/;
