import { randomUUID } from 'node:crypto';
import type { Deps } from './api';
import type { CameraRecord, CommandRecord } from './model';

export type VideoRequest =
  | { ok: true; created: boolean; command: CommandRecord }
  | { ok: false; status: 404 | 409; error: string };

/**
 * Queues a requestVideo command for the hub, from the app or a bot button.
 * Re-requesting a pending video returns the existing command (idempotent).
 * The caller has checked that the user may see the camera.
 */
export async function requestVideo(
  camera: CameraRecord,
  mediaId: number,
  deps: Deps,
): Promise<VideoRequest> {
  const media = await deps.store.getMedia(camera.camId, mediaId);
  if (!media) return { ok: false, status: 404, error: 'Recording not found' };
  if (media.fileType !== 2) return { ok: false, status: 409, error: 'Recording is not a video' };

  const pending = (await deps.store.listCommands(camera.camId)).find(
    (c) =>
      c.type === 'requestVideo' &&
      c.mediaId === mediaId &&
      (c.state === 'queued' || c.state === 'delivered'),
  );
  if (pending) return { ok: true, created: false, command: pending };

  const now = deps.now();
  const command: CommandRecord = {
    id: (deps.newId ?? randomUUID)(),
    type: 'requestVideo',
    mediaId,
    state: 'queued',
    created: now,
    updated: now,
  };
  await deps.store.putCommand(camera.camId, command);
  await deps.store.setVideoState(camera.camId, mediaId, 'requested');
  return { ok: true, created: true, command };
}
