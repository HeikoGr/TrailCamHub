import type { Camera } from '../api/types';
import { batteryLevel, formatRelative, formatStorage } from '../format';

interface Props {
  camera: Camera;
  now: number;
}

export function StatusCard({ camera, now }: Props) {
  return (
    <section className="card status" aria-label={`Status of ${camera.name}`}>
      <header className="status__header">
        <div>
          <h2 className="status__name">{camera.name}</h2>
          <p className="hint">
            {camera.model} · FW {camera.firmware}
          </p>
        </div>
        <p className="hint status__seen">Seen {formatRelative(camera.lastSeen, now)}</p>
      </header>
      <dl className="metrics">
        <Metric
          label="Camera battery"
          value={`${camera.batPercent} %`}
          level={batteryLevel(camera.batPercent)}
        />
        {camera.hubBatPercent !== undefined && (
          <Metric
            label="Hub battery"
            value={`${camera.hubBatPercent} %`}
            level={batteryLevel(camera.hubBatPercent)}
          />
        )}
        <Metric label="SD free" value={formatStorage(camera.sdFreeMB)} />
      </dl>
    </section>
  );
}

function Metric({ label, value, level }: { label: string; value: string; level?: string }) {
  return (
    <div className="metric" data-level={level}>
      <dt>{label}</dt>
      <dd>{value}</dd>
    </div>
  );
}
