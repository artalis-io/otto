import { useState, useEffect, useMemo, useRef } from 'react';
import { MapContainer, TileLayer, useMap } from 'react-leaflet';
import 'leaflet/dist/leaflet.css';
import './App.css';

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

// Get tile server URL from env or default to current origin
const TILE_SERVER = import.meta.env.VITE_TILE_SERVER || '';

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

type LayerType = 'png' | 'osm';

function App() {
  const [stats, setStats] = useState<Stats | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [layer, setLayer] = useState<LayerType>('png');
  const [tileInfo, setTileInfo] = useState<TileInfo>({ zoom: 10, x: 0, y: 0 });

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
      </MapContainer>

      <div className="info-panel">
        <h2>Carta Tile Server</h2>

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

      <div className="tile-info">
        Zoom: {tileInfo.zoom} | Tile: {tileInfo.zoom}/{tileInfo.x}/{tileInfo.y}
      </div>
    </div>
  );
}

export default App;
