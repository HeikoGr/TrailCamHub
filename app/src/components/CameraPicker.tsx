import type { Camera } from '../api/types';
import { batteryLevel } from '../format';

interface Props {
  cameras: Camera[];
  selected: string;
  onSelect(camId: string): void;
}

/** Row of camera chips; shown only when there is more than one camera. */
export function CameraPicker({ cameras, selected, onSelect }: Props) {
  if (cameras.length < 2) {
    return null;
  }
  return (
    <nav className="picker" aria-label="Cameras">
      {cameras.map((camera) => (
        <button
          key={camera.camId}
          type="button"
          className="picker__chip"
          aria-pressed={camera.camId === selected}
          onClick={() => onSelect(camera.camId)}
        >
          <span className="picker__name">{camera.name}</span>
          <span className="picker__battery" data-level={batteryLevel(camera.batPercent)}>
            {camera.batPercent} %
          </span>
        </button>
      ))}
    </nav>
  );
}
