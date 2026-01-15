import type { VehicleConfig, RouteSegment } from '../types';

interface RouteConfigProps {
  config: VehicleConfig;
  segments: RouteSegment[];
  onConfigChange: (config: Partial<VehicleConfig>) => void;
  onSegmentsChange: (segments: RouteSegment[]) => void;
}

export default function RouteConfig({
  config,
  segments,
  onConfigChange,
  onSegmentsChange,
}: RouteConfigProps) {
  const addSegment = () => {
    const lastEnd = segments.length > 0 ? segments[segments.length - 1].start + 100 : 0;
    onSegmentsChange([...segments, { start: lastEnd, mpg: config.consumptionMpg, weight: 0 }]);
  };

  const updateSegment = (index: number, updates: Partial<RouteSegment>) => {
    const newSegments = [...segments];
    newSegments[index] = { ...newSegments[index], ...updates };
    onSegmentsChange(newSegments);
  };

  const removeSegment = (index: number) => {
    onSegmentsChange(segments.filter((_, i) => i !== index));
  };

  return (
    <div className="route-config">
      <h3>Vehicle Configuration</h3>

      <div className="config-grid">
        <div className="config-item">
          <label>Tank Capacity (gal)</label>
          <input
            type="number"
            value={config.tankCapacity}
            onChange={(e) => onConfigChange({ tankCapacity: parseFloat(e.target.value) || 0 })}
            min={0}
            step={10}
          />
        </div>

        <div className="config-item">
          <label>Current Fuel (gal)</label>
          <input
            type="number"
            value={config.currentFuel}
            onChange={(e) => onConfigChange({ currentFuel: parseFloat(e.target.value) || 0 })}
            min={0}
            max={config.tankCapacity}
            step={5}
          />
        </div>

        <div className="config-item">
          <label>Base MPG</label>
          <input
            type="number"
            value={config.consumptionMpg}
            onChange={(e) => onConfigChange({ consumptionMpg: parseFloat(e.target.value) || 1 })}
            min={1}
            step={0.5}
          />
        </div>

        <div className="config-item">
          <label>Minimum Fuel (gal)</label>
          <input
            type="number"
            value={config.minimumFuel}
            onChange={(e) => onConfigChange({ minimumFuel: parseFloat(e.target.value) || 0 })}
            min={0}
            step={5}
          />
        </div>

        <div className="config-item">
          <label>Min Purchase (gal)</label>
          <input
            type="number"
            value={config.minPurchase || 0}
            onChange={(e) => onConfigChange({ minPurchase: parseFloat(e.target.value) || 0 })}
            min={0}
            step={5}
          />
          <span className="help-text">0 = no minimum</span>
        </div>

        <div className="config-item">
          <label>Stop Cost ($)</label>
          <input
            type="number"
            value={config.stopCost || 0}
            onChange={(e) => onConfigChange({ stopCost: parseFloat(e.target.value) || 0 })}
            min={0}
            step={5}
          />
          <span className="help-text">Fixed cost per stop</span>
        </div>
      </div>

      <div className="segments-section">
        <div className="segments-header">
          <h4>Variable Consumption (Optional)</h4>
          <button className="btn-small" onClick={addSegment}>+ Add Segment</button>
        </div>

        {segments.length === 0 ? (
          <p className="help-text">
            Using constant {config.consumptionMpg} MPG. Add segments for variable consumption
            (e.g., different cargo weights along the route).
          </p>
        ) : (
          <div className="segments-list">
            {segments.map((seg, i) => (
              <div key={i} className="segment-row">
                <div className="segment-field">
                  <label>Start (mi)</label>
                  <input
                    type="number"
                    value={seg.start}
                    onChange={(e) => updateSegment(i, { start: parseFloat(e.target.value) || 0 })}
                    min={0}
                    step={10}
                  />
                </div>
                <div className="segment-field">
                  <label>Weight (lbs)</label>
                  <input
                    type="number"
                    value={seg.weight || 0}
                    onChange={(e) => updateSegment(i, { weight: parseFloat(e.target.value) || 0 })}
                    min={0}
                    step={1000}
                  />
                </div>
                <div className="segment-field">
                  <label>MPG</label>
                  <input
                    type="number"
                    value={seg.mpg}
                    onChange={(e) => updateSegment(i, { mpg: parseFloat(e.target.value) || 1 })}
                    min={1}
                    step={0.5}
                  />
                </div>
                <button
                  className="btn-remove"
                  onClick={() => removeSegment(i)}
                  title="Remove segment"
                >
                  &times;
                </button>
              </div>
            ))}
          </div>
        )}
      </div>
    </div>
  );
}
