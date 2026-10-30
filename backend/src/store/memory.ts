import type { CameraRecord, CommandRecord, MediaRecord, VideoState } from '../model';
import { mediaKey } from '../model';
import type { Store, Telemetry } from './types';

/** In-memory store for tests and local experiments. */
export class MemoryStore implements Store {
  private readonly cameras = new Map<string, CameraRecord>();
  private readonly links = new Map<number, Set<string>>();
  private readonly media = new Map<string, Map<string, MediaRecord>>();
  private readonly commands = new Map<string, Map<string, CommandRecord>>();

  async putCamera(camera: CameraRecord): Promise<void> {
    this.cameras.set(camera.camId, { ...camera });
  }

  async getCamera(camId: string): Promise<CameraRecord | undefined> {
    const camera = this.cameras.get(camId);
    return camera && { ...camera };
  }

  async updateTelemetry(camId: string, telemetry: Telemetry, lastSeen: number): Promise<void> {
    const camera = this.cameras.get(camId);
    if (!camera) throw new Error(`Unknown camera ${camId}`);
    const defined = Object.fromEntries(
      Object.entries(telemetry).filter(([, value]) => value !== undefined),
    );
    this.cameras.set(camId, { ...camera, ...defined, lastSeen });
  }

  async linkUser(userId: number, camId: string): Promise<void> {
    const set = this.links.get(userId) ?? new Set<string>();
    set.add(camId);
    this.links.set(userId, set);
  }

  async listUserCameraIds(userId: number): Promise<string[]> {
    return [...(this.links.get(userId) ?? [])].sort();
  }

  async isLinked(userId: number, camId: string): Promise<boolean> {
    return this.links.get(userId)?.has(camId) ?? false;
  }

  async listMedia(camId: string): Promise<MediaRecord[]> {
    return [...(this.media.get(camId)?.values() ?? [])].map((m) => ({ ...m }));
  }

  async getMedia(camId: string, dir: number, num: number): Promise<MediaRecord | undefined> {
    const item = this.media.get(camId)?.get(mediaKey({ dir, num }));
    return item && { ...item };
  }

  async addMedia(camId: string, items: MediaRecord[]): Promise<void> {
    const map = this.media.get(camId) ?? new Map<string, MediaRecord>();
    for (const item of items) {
      const key = mediaKey(item);
      if (!map.has(key)) map.set(key, { ...item });
    }
    this.media.set(camId, map);
  }

  async markThumbPresent(camId: string, dir: number, num: number): Promise<void> {
    const item = this.media.get(camId)?.get(mediaKey({ dir, num }));
    if (item) item.thumbPresent = true;
  }

  async setVideoState(camId: string, dir: number, num: number, state: VideoState): Promise<void> {
    const item = this.media.get(camId)?.get(mediaKey({ dir, num }));
    if (item) item.videoState = state;
  }

  async putCommand(camId: string, command: CommandRecord): Promise<void> {
    const map = this.commands.get(camId) ?? new Map<string, CommandRecord>();
    map.set(command.id, { ...command });
    this.commands.set(camId, map);
  }

  async getCommand(camId: string, id: string): Promise<CommandRecord | undefined> {
    const command = this.commands.get(camId)?.get(id);
    return command && { ...command };
  }

  async listCommands(camId: string): Promise<CommandRecord[]> {
    return [...(this.commands.get(camId)?.values() ?? [])]
      .map((c) => ({ ...c }))
      .sort((a, b) => a.created - b.created);
  }
}
