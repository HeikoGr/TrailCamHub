/**
 * Domain types and the API v1 wire format.
 *
 * App-facing shapes (`Camera`, `MediaItem`, `CommandResult`) mirror
 * app/src/api/types.ts. Device-facing shapes describe the hub's check-in.
 */

export type FileType = 0 | 2;
export type VideoState = 'none' | 'requested' | 'downloading' | 'available' | 'failed';
export type CommandType = 'requestVideo';
export type CommandState = 'queued' | 'delivered' | 'done' | 'failed';

export interface CameraRecord {
  camId: string;
  name: string;
  /** SHA-256 (hex) of the device key secret; the secret itself is never stored. */
  keyHash: string;
  model?: string;
  firmware?: string;
  hubFirmware?: string;
  batPercent?: number;
  sdFreeMB?: number;
  hubBatPercent?: number;
  /** Last check-in, Unix seconds; 0 = never. */
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

// ---- Device API ------------------------------------------------------------

export interface CheckinRequest {
  /** Firmware version of the TrailCamHub module. */
  hubFirmware?: string;
  hubBatPercent?: number;
  camera?: {
    model?: string;
    firmware?: string;
    batPercent?: number;
    sdFreeMB?: number;
  };
  /** The camera's media list (cmdId 768), any order. */
  media?: Array<{ dir: number; num: number; fileType: number; mediaTime: number }>;
}

export interface CheckinResponse {
  serverTime: number;
  /** Thumbnails the backend is missing: PUT the JPEG to `url`, then report it via /ack. */
  thumbUploads: Array<{ dir: number; num: number; url: string }>;
  commands: Array<{ id: string; type: CommandType; dir: number; num: number }>;
  config: { sleepIntervalS: number };
}

export interface AckRequest {
  uploadedThumbs?: Array<{ dir: number; num: number }>;
  commands?: Array<{ id: string; state: 'done' | 'failed'; error?: string }>;
}

export function mediaKey(item: { dir: number; num: number }): string {
  return `${item.dir}_${item.num}`;
}
