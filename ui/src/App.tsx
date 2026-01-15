import { useState, useCallback } from 'react';
import MapView from './components/MapView';
import FileUpload from './components/FileUpload';
import RouteConfig from './components/RouteConfig';
import OptimizationResult from './components/OptimizationResult';
import { optimize, getRoute } from './services/api';
import type { Station, Coordinate, VehicleConfig, RouteSegment, OptimizeResponse } from './types';
import './App.css';

const defaultConfig: VehicleConfig = {
  tankCapacity: 150,
  currentFuel: 50,
  consumptionMpg: 6.5,
  minimumFuel: 20,
  minPurchase: 0,
  stopCost: 0,
};

function App() {
  const [stations, setStations] = useState<Station[]>([]);
  const [waypoints, setWaypoints] = useState<Coordinate[]>([]);
  const [route, setRoute] = useState<[number, number][] | null>(null);
  const [config, setConfig] = useState<VehicleConfig>(defaultConfig);
  const [segments, setSegments] = useState<RouteSegment[]>([]);
  const [result, setResult] = useState<OptimizeResponse | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [routeDistance, setRouteDistance] = useState<number | null>(null);

  const handleMapClick = useCallback((coord: Coordinate) => {
    setWaypoints(prev => [...prev, coord]);
    setRoute(null);
    setResult(null);
  }, []);

  const handleGetRoute = async () => {
    if (waypoints.length < 2) {
      setError('Need at least 2 waypoints to get a route');
      return;
    }

    setLoading(true);
    setError(null);

    try {
      const routeData = await getRoute(waypoints);
      setRoute(routeData.route);
      setRouteDistance(routeData.distance);
      setResult(null);
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Failed to get route');
    } finally {
      setLoading(false);
    }
  };

  const handleOptimize = async () => {
    if (!route || route.length < 2) {
      setError('Get a route first');
      return;
    }

    if (stations.length === 0) {
      setError('Load stations first');
      return;
    }

    setLoading(true);
    setError(null);

    try {
      const response = await optimize({
        stations,
        route,
        segments: segments.length > 0 ? segments : undefined,
        tankCapacity: config.tankCapacity,
        currentFuel: config.currentFuel,
        consumptionMpg: config.consumptionMpg,
        minimumFuel: config.minimumFuel,
        minPurchase: config.minPurchase,
        stopCost: config.stopCost,
        maxDistance: 10,
      });
      setResult(response);
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Optimization failed');
    } finally {
      setLoading(false);
    }
  };

  const handleClearWaypoints = () => {
    setWaypoints([]);
    setRoute(null);
    setResult(null);
    setRouteDistance(null);
  };

  const handleConfigChange = (updates: Partial<VehicleConfig>) => {
    setConfig(prev => ({ ...prev, ...updates }));
  };

  return (
    <div className="app">
      <header className="app-header">
        <h1>FuelWise</h1>
        <span className="subtitle">Truck Refueling Optimization</span>
      </header>

      <div className="app-content">
        <aside className="sidebar">
          <FileUpload
            onStationsLoaded={setStations}
            onWaypointsLoaded={(wps) => {
              setWaypoints(wps);
              setRoute(null);
              setResult(null);
            }}
          />

          <div className="status-panel">
            <h3>Status</h3>
            <div className="status-item">
              <span>Stations:</span>
              <strong>{stations.length}</strong>
            </div>
            <div className="status-item">
              <span>Waypoints:</span>
              <strong>{waypoints.length}</strong>
              {waypoints.length > 0 && (
                <button className="btn-link" onClick={handleClearWaypoints}>
                  Clear
                </button>
              )}
            </div>
            {routeDistance !== null && (
              <div className="status-item">
                <span>Route:</span>
                <strong>{routeDistance.toFixed(1)} mi</strong>
              </div>
            )}
          </div>

          <RouteConfig
            config={config}
            segments={segments}
            onConfigChange={handleConfigChange}
            onSegmentsChange={setSegments}
          />

          <div className="action-buttons">
            <button
              className="btn-primary"
              onClick={handleGetRoute}
              disabled={waypoints.length < 2 || loading}
            >
              {loading ? 'Loading...' : 'Get Route'}
            </button>
            <button
              className="btn-success"
              onClick={handleOptimize}
              disabled={!route || stations.length === 0 || loading}
            >
              {loading ? 'Optimizing...' : 'Optimize'}
            </button>
          </div>

          {error && (
            <div className="error-message">
              {error}
            </div>
          )}

          {result && (
            <OptimizationResult result={result} stations={stations} />
          )}
        </aside>

        <main className="map-container">
          <MapView
            stations={stations}
            waypoints={waypoints}
            route={route}
            stops={result?.stops || []}
            onMapClick={handleMapClick}
          />
        </main>
      </div>
    </div>
  );
}

export default App;
