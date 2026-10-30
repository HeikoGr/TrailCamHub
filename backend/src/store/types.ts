import type { CameraRecord, CommandRecord, MediaRecord, VideoState } from '../model';

export type Telemetry = Partial<
  Pick<
    CameraRecord,
    'model' | 'firmware' | 'hubFirmware' | 'batPercent' | 'sdFreeMB' | 'hubBatPercent'
  >
>;

/** Persistence boundary; implemented by DynamoDB in AWS and in memory for tests. */
export interface Store {
  putCamera(camera: CameraRecord): Promise<void>;
  getCamera(camId: string): Promise<CameraRecord | undefined>;
  updateTelemetry(camId: string, telemetry: Telemetry, lastSeen: number): Promise<void>;

  linkUser(userId: number, camId: string): Promise<void>;
  listUserCameraIds(userId: number): Promise<string[]>;
  isLinked(userId: number, camId: string): Promise<boolean>;

  listMedia(camId: string): Promise<MediaRecord[]>;
  getMedia(camId: string, dir: number, num: number): Promise<MediaRecord | undefined>;
  /** Inserts new media; never overwrites existing records (their flags survive). */
  addMedia(camId: string, items: MediaRecord[]): Promise<void>;
  markThumbPresent(camId: string, dir: number, num: number): Promise<void>;
  setVideoState(camId: string, dir: number, num: number, state: VideoState): Promise<void>;

  putCommand(camId: string, command: CommandRecord): Promise<void>;
  getCommand(camId: string, id: string): Promise<CommandRecord | undefined>;
  listCommands(camId: string): Promise<CommandRecord[]>;
}
