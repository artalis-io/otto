import { useState, useEffect, useMemo, useRef, useCallback } from 'react';
import { MapContainer, TileLayer, useMap, useMapEvents, Polyline, Marker, Popup } from 'react-leaflet';
import L from 'leaflet';
import 'leaflet/dist/leaflet.css';
import './App.css';

// Fix Leaflet default marker icons
import markerIcon2x from 'leaflet/dist/images/marker-icon-2x.png';
import markerIcon from 'leaflet/dist/images/marker-icon.png';
import markerShadow from 'leaflet/dist/images/marker-shadow.png';

// @ts-expect-error - Leaflet Icon default workaround
delete L.Icon.Default.prototype._getIconUrl;
L.Icon.Default.mergeOptions({
  iconUrl: markerIcon,
  iconRetinaUrl: markerIcon2x,
  shadowUrl: markerShadow,
});

// Custom icons for origin/destination
const originIcon = new L.Icon({
  iconUrl: 'data:image/svg+xml,' + encodeURIComponent(`
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="32" height="32">
      <circle cx="12" cy="12" r="10" fill="#4CAF50" stroke="white" stroke-width="2"/>
      <circle cx="12" cy="12" r="4" fill="white"/>
    </svg>
  `),
  iconSize: [32, 32],
  iconAnchor: [16, 16],
  popupAnchor: [0, -16],
});

const destIcon = new L.Icon({
  iconUrl: 'data:image/svg+xml,' + encodeURIComponent(`
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="32" height="32">
      <circle cx="12" cy="12" r="10" fill="#F44336" stroke="white" stroke-width="2"/>
      <rect x="8" y="8" width="8" height="8" fill="white"/>
    </svg>
  `),
  iconSize: [32, 32],
  iconAnchor: [16, 16],
  popupAnchor: [0, -16],
});

// Types
interface Stats {
  total_nodes: number;
  total_ways: number;
  features_indexed: number;
  bbox: {
    min_lat: number;
    min_lon: number;
    max_lat: number;
    max_lon: number;
  };
}

interface TileInfo {
  zoom: number;
  x: number;
  y: number;
}

interface RouteResponse {
  status: string;
  route?: {
    distance: number;
    duration: number;
    profile: string;
    mode: string;
    from: [number, number];
    to: [number, number];
    geometry: string;
  };
  error?: string;
}

type Profile = 'car' | 'truck' | 'bike' | 'foot';
type Mode = 'fastest' | 'shortest';

// Get server URLs from env or default to current origin (for proxied requests)
const TILE_SERVER = import.meta.env.VITE_TILE_SERVER || '';
const ROUTE_SERVER = import.meta.env.VITE_ROUTE_SERVER || '';

// Google Polyline decoder
function decodePolyline(encoded: string): [number, number][] {
  const points: [number, number][] = [];
  let index = 0;
  let lat = 0;
  let lng = 0;

  while (index < encoded.length) {
    let shift = 0;
    let result = 0;
    let byte: number;

    do {
      byte = encoded.charCodeAt(index++) - 63;
      result |= (byte & 0x1f) << shift;
      shift += 5;
    } while (byte >= 0x20);

    const dlat = (result & 1) ? ~(result >> 1) : (result >> 1);
    lat += dlat;

    shift = 0;
    result = 0;

    do {
      byte = encoded.charCodeAt(index++) - 63;
      result |= (byte & 0x1f) << shift;
      shift += 5;
    } while (byte >= 0x20);

    const dlng = (result & 1) ? ~(result >> 1) : (result >> 1);
    lng += dlng;

    points.push([lat / 1e5, lng / 1e5]);
  }

  return points;
}

// Format duration in human readable format
function formatDuration(seconds: number): string {
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  if (hours > 0) {
    return `${hours}h ${minutes}m`;
  }
  return `${minutes} min`;
}

// Format distance in human readable format
function formatDistance(meters: number): string {
  if (meters >= 1000) {
    return `${(meters / 1000).toFixed(1)} km`;
  }
  return `${Math.round(meters)} m`;
}

// Component to fit map to bounds (runs once)
function FitBounds({ bounds }: { bounds: [[number, number], [number, number]] | null }) {
  const map = useMap();
  const hasFit = useRef(false);

  useEffect(() => {
    if (bounds && !hasFit.current) {
      hasFit.current = true;
      map.fitBounds(bounds, { padding: [50, 50] });
    }
  }, [map, bounds]);

  return null;
}

// Component to track tile info
function TileTracker({ onUpdate }: { onUpdate: (info: TileInfo) => void }) {
  const map = useMap();

  useEffect(() => {
    const updateInfo = () => {
      const center = map.getCenter();
      const zoom = map.getZoom();
      const n = Math.pow(2, zoom);
      const x = Math.floor((center.lng + 180) / 360 * n);
      const latRad = center.lat * Math.PI / 180;
      const y = Math.floor((1 - Math.log(Math.tan(latRad) + 1/Math.cos(latRad)) / Math.PI) / 2 * n);
      onUpdate({ zoom, x, y });
    };

    map.on('move', updateInfo);
    map.on('zoom', updateInfo);
    updateInfo();

    return () => {
      map.off('move', updateInfo);
      map.off('zoom', updateInfo);
    };
  }, [map, onUpdate]);

  return null;
}

// Component to handle map clicks for routing
function ClickHandler({
  onOriginSet,
  onDestinationSet,
  hasOrigin
}: {
  onOriginSet: (latlng: [number, number]) => void;
  onDestinationSet: (latlng: [number, number]) => void;
  hasOrigin: boolean;
}) {
  useMapEvents({
    click: (e) => {
      const latlng: [number, number] = [e.latlng.lat, e.latlng.lng];
      if (!hasOrigin) {
        onOriginSet(latlng);
      } else {
        onDestinationSet(latlng);
      }
    },
  });

  return null;
}

type LayerType = 'png' | 'osm';

function App() {
  // Map state
  const [stats, setStats] = useState<Stats | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [layer, setLayer] = useState<LayerType>('png');
  const [tileInfo, setTileInfo] = useState<TileInfo>({ zoom: 10, x: 0, y: 0 });

  // Routing state
  const [origin, setOrigin] = useState<[number, number] | null>(null);
  const [destination, setDestination] = useState<[number, number] | null>(null);
  const [routeGeometry, setRouteGeometry] = useState<[number, number][] | null>(null);
  const [routeInfo, setRouteInfo] = useState<{ distance: number; duration: number } | null>(null);
  const [routeLoading, setRouteLoading] = useState(false);
  const [routeError, setRouteError] = useState<string | null>(null);
  const [profile, setProfile] = useState<Profile>('car');
  const [mode, setMode] = useState<Mode>('fastest');
  const [routeServerAvailable, setRouteServerAvailable] = useState<boolean | null>(null);

  // Fetch stats on mount
  useEffect(() => {
    fetch(`${TILE_SERVER}/api/v1/stats`)
      .then(res => {
        if (!res.ok) throw new Error('Failed to load stats');
        return res.json();
      })
      .then((data: Stats) => {
        setStats(data);
        setLoading(false);
      })
      .catch(err => {
        setError(err.message);
        setLoading(false);
      });

    // Check if route server is available
    fetch(`${ROUTE_SERVER}/api/v1/health`)
      .then(res => {
        setRouteServerAvailable(res.ok);
      })
      .catch(() => {
        setRouteServerAvailable(false);
      });
  }, []);

  // Fetch route when origin and destination are set
  useEffect(() => {
    if (!origin || !destination) {
      setRouteGeometry(null);
      setRouteInfo(null);
      setRouteError(null);
      return;
    }

    setRouteLoading(true);
    setRouteError(null);

    const url = `${ROUTE_SERVER}/api/v1/route?from=${origin[0]},${origin[1]}&to=${destination[0]},${destination[1]}&profile=${profile}&mode=${mode}`;

    fetch(url)
      .then(res => res.json())
      .then((data: RouteResponse) => {
        if (data.error) {
          setRouteError(data.error);
          setRouteGeometry(null);
          setRouteInfo(null);
        } else if (data.route) {
          const decoded = decodePolyline(data.route.geometry);
          setRouteGeometry(decoded);
          setRouteInfo({
            distance: data.route.distance,
            duration: data.route.duration,
          });
        }
        setRouteLoading(false);
      })
      .catch(err => {
        setRouteError(err.message);
        setRouteLoading(false);
      });
  }, [origin, destination, profile, mode]);

  // Clear route
  const clearRoute = useCallback(() => {
    setOrigin(null);
    setDestination(null);
    setRouteGeometry(null);
    setRouteInfo(null);
    setRouteError(null);
  }, []);

  // Calculate initial center and bounds from stats (memoized to prevent re-renders)
  const initialCenter = useMemo<[number, number]>(() =>
    stats?.bbox
      ? [(stats.bbox.min_lat + stats.bbox.max_lat) / 2, (stats.bbox.min_lon + stats.bbox.max_lon) / 2]
      : [0, 0],
    [stats?.bbox]
  );

  const bounds = useMemo<[[number, number], [number, number]] | null>(() =>
    stats?.bbox
      ? [[stats.bbox.min_lat, stats.bbox.min_lon], [stats.bbox.max_lat, stats.bbox.max_lon]]
      : null,
    [stats?.bbox]
  );

  // Show loading state until we have stats
  if (loading) {
    return (
      <div className="app loading-screen">
        <div className="loading-content">
          <h2>Carta Tile Server</h2>
          <p>Loading map data...</p>
        </div>
      </div>
    );
  }

  if (error) {
    return (
      <div className="app loading-screen">
        <div className="loading-content">
          <h2>Carta Tile Server</h2>
          <p className="error-text">Error: {error}</p>
        </div>
      </div>
    );
  }

  return (
    <div className="app">
      <MapContainer
        center={initialCenter}
        zoom={12}
        className="map"
      >
        {layer === 'png' && (
          <TileLayer
            url={`${TILE_SERVER}/tiles/{z}/{x}/{y}.png`}
            maxZoom={18}
            attribution='&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> | Carta'
          />
        )}
        {layer === 'osm' && (
          <TileLayer
            url="https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png"
            maxZoom={19}
            attribution='&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a>'
          />
        )}
        <FitBounds bounds={bounds} />
        <TileTracker onUpdate={setTileInfo} />

        {/* Click handler for routing */}
        {routeServerAvailable && (
          <ClickHandler
            onOriginSet={setOrigin}
            onDestinationSet={setDestination}
            hasOrigin={origin !== null}
          />
        )}

        {/* Origin marker */}
        {origin && (
          <Marker position={origin} icon={originIcon}>
            <Popup>
              <strong>Origin</strong><br />
              {origin[0].toFixed(5)}, {origin[1].toFixed(5)}
            </Popup>
          </Marker>
        )}

        {/* Destination marker */}
        {destination && (
          <Marker position={destination} icon={destIcon}>
            <Popup>
              <strong>Destination</strong><br />
              {destination[0].toFixed(5)}, {destination[1].toFixed(5)}
            </Popup>
          </Marker>
        )}

        {/* Route polyline */}
        {routeGeometry && (
          <Polyline
            positions={routeGeometry}
            pathOptions={{
              color: '#2196F3',
              weight: 5,
              opacity: 0.8,
            }}
          />
        )}
      </MapContainer>

      {/* Info Panel */}
      <div className="info-panel">
        <h2>Carta + Velo</h2>

        {stats && (
          <div className="stats">
            <div className="stat-row">
              <span className="stat-label">Nodes:</span>
              <span className="stat-value">{stats.total_nodes.toLocaleString()}</span>
            </div>
            <div className="stat-row">
              <span className="stat-label">Ways:</span>
              <span className="stat-value">{stats.total_ways.toLocaleString()}</span>
            </div>
            <div className="stat-row">
              <span className="stat-label">Features:</span>
              <span className="stat-value">{stats.features_indexed.toLocaleString()}</span>
            </div>
          </div>
        )}

        <div className="layer-controls">
          <h3>Tile Layer</h3>
          <label className="layer-option">
            <input
              type="radio"
              name="layer"
              value="png"
              checked={layer === 'png'}
              onChange={() => setLayer('png')}
            />
            <span>Carta (PNG)</span>
          </label>
          <label className="layer-option">
            <input
              type="radio"
              name="layer"
              value="osm"
              checked={layer === 'osm'}
              onChange={() => setLayer('osm')}
            />
            <span>OSM (Reference)</span>
          </label>
        </div>
      </div>

      {/* Routing Panel */}
      <div className="routing-panel">
        <h3>Routing</h3>

        {routeServerAvailable === false && (
          <div className="route-unavailable">
            Route server unavailable.<br />
            Start with: <code>make run-velo-api</code>
          </div>
        )}

        {routeServerAvailable && (
          <>
            <div className="route-instructions">
              {!origin && <span>Click map to set origin</span>}
              {origin && !destination && <span>Click map to set destination</span>}
              {origin && destination && <span>Route calculated</span>}
            </div>

            <div className="profile-controls">
              <label className="profile-label">Profile:</label>
              <select
                value={profile}
                onChange={(e) => setProfile(e.target.value as Profile)}
                className="profile-select"
              >
                <option value="car">Car</option>
                <option value="truck">Truck</option>
                <option value="bike">Bike</option>
                <option value="foot">Foot</option>
              </select>
            </div>

            <div className="mode-controls">
              <label className="mode-label">Mode:</label>
              <select
                value={mode}
                onChange={(e) => setMode(e.target.value as Mode)}
                className="mode-select"
              >
                <option value="fastest">Fastest</option>
                <option value="shortest">Shortest</option>
              </select>
            </div>

            {routeLoading && (
              <div className="route-loading">Calculating route...</div>
            )}

            {routeError && (
              <div className="route-error">{routeError}</div>
            )}

            {routeInfo && (
              <div className="route-info">
                <div className="route-stat">
                  <span className="route-stat-label">Distance:</span>
                  <span className="route-stat-value">{formatDistance(routeInfo.distance)}</span>
                </div>
                <div className="route-stat">
                  <span className="route-stat-label">Duration:</span>
                  <span className="route-stat-value">{formatDuration(routeInfo.duration)}</span>
                </div>
              </div>
            )}

            {(origin || destination) && (
              <button className="clear-route-btn" onClick={clearRoute}>
                Clear Route
              </button>
            )}
          </>
        )}
      </div>

      <div className="tile-info">
        Zoom: {tileInfo.zoom} | Tile: {tileInfo.zoom}/{tileInfo.x}/{tileInfo.y}
      </div>
    </div>
  );
}

export default App;
