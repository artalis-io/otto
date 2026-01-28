import type { OptimizeResponse, Station } from '../types';

interface OptimizationResultProps {
  result: OptimizeResponse;
  stations: Station[];
}

export default function OptimizationResult({ result, stations }: OptimizationResultProps) {
  const getStationName = (id: number) => {
    const station = stations.find(s => s.id === id);
    return station?.name || `Station ${id}`;
  };

  const getStationPrice = (id: number) => {
    const station = stations.find(s => s.id === id);
    return station?.price || 0;
  };

  const isOptimal = result.status.toLowerCase().includes('optimal');

  return (
    <div className={`optimization-result ${isOptimal ? 'success' : 'error'}`}>
      <h3>Optimization Result</h3>

      <div className="result-status">
        <span className={`status-badge ${isOptimal ? 'optimal' : 'failed'}`}>
          {result.status}
        </span>
      </div>

      {isOptimal && (
        <>
          <div className="result-summary">
            <div className="summary-item">
              <span className="label">Route Distance</span>
              <span className="value">{result.routeDistance.toFixed(1)} mi</span>
            </div>
            <div className="summary-item">
              <span className="label">Stations Considered</span>
              <span className="value">{result.stationsFiltered}</span>
            </div>
            <div className="summary-item highlight">
              <span className="label">Total Cost</span>
              <span className="value">${result.totalCost.toFixed(2)}</span>
            </div>
            <div className="summary-item">
              <span className="label">Number of Stops</span>
              <span className="value">{result.numStops}</span>
            </div>
            <div className="summary-item">
              <span className="label">Remaining Fuel</span>
              <span className="value">{result.remainingFuel.toFixed(1)} gal</span>
            </div>
          </div>

          {result.stops.length > 0 && (
            <div className="stops-list">
              <h4>Fuel Stops</h4>
              <table>
                <thead>
                  <tr>
                    <th>#</th>
                    <th>Station</th>
                    <th>Distance</th>
                    <th>Price</th>
                    <th>Gallons</th>
                    <th>Cost</th>
                  </tr>
                </thead>
                <tbody>
                  {result.stops.map((stop, i) => (
                    <tr key={i}>
                      <td>{i + 1}</td>
                      <td>{getStationName(stop.stationId)}</td>
                      <td>
                        {stop.distanceFromStart !== undefined
                          ? `${stop.distanceFromStart.toFixed(1)} mi`
                          : '-'}
                      </td>
                      <td>${getStationPrice(stop.stationId).toFixed(2)}/gal</td>
                      <td>{stop.gallons.toFixed(1)} gal</td>
                      <td>${stop.cost.toFixed(2)}</td>
                    </tr>
                  ))}
                </tbody>
                <tfoot>
                  <tr>
                    <td colSpan={4}></td>
                    <td><strong>Total</strong></td>
                    <td><strong>${result.totalCost.toFixed(2)}</strong></td>
                  </tr>
                </tfoot>
              </table>
            </div>
          )}

          {result.stops.length === 0 && (
            <p className="no-stops">
              No fuel stops needed - current fuel is sufficient for the route.
            </p>
          )}
        </>
      )}
    </div>
  );
}
