import { useEffect } from 'react';
import { MapContainer, TileLayer, Marker, Popup, Polyline, useMap, CircleMarker } from 'react-leaflet';
import L from 'leaflet';
import 'leaflet/dist/leaflet.css';
import type { Station, Coordinate, FuelStop } from '../types';

// Fix Leaflet default marker icons
import markerIcon2x from 'leaflet/dist/images/marker-icon-2x.png';
import markerIcon from 'leaflet/dist/images/marker-icon.png';
import markerShadow from 'leaflet/dist/images/marker-shadow.png';

delete (L.Icon.Default.prototype as any)._getIconUrl;
L.Icon.Default.mergeOptions({
  iconUrl: markerIcon,
  iconRetinaUrl: markerIcon2x,
  shadowUrl: markerShadow,
});

// Custom icons
const stationIcon = new L.Icon({
  iconUrl: 'data:image/svg+xml,' + encodeURIComponent(`
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="24" height="24">
      <circle cx="12" cy="12" r="10" fill="#3b82f6" stroke="white" stroke-width="2"/>
      <text x="12" y="16" text-anchor="middle" fill="white" font-size="12" font-weight="bold">$</text>
    </svg>
  `),
  iconSize: [24, 24],
  iconAnchor: [12, 12],
  popupAnchor: [0, -12],
});

const selectedStationIcon = new L.Icon({
  iconUrl: 'data:image/svg+xml,' + encodeURIComponent(`
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" width="32" height="32">
      <circle cx="16" cy="16" r="14" fill="#22c55e" stroke="white" stroke-width="2"/>
      <text x="16" y="21" text-anchor="middle" fill="white" font-size="14" font-weight="bold">$</text>
    </svg>
  `),
  iconSize: [32, 32],
  iconAnchor: [16, 16],
  popupAnchor: [0, -16],
});

const waypointIcon = new L.Icon({
  iconUrl: 'data:image/svg+xml,' + encodeURIComponent(`
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="24" height="24">
      <circle cx="12" cy="12" r="8" fill="#ef4444" stroke="white" stroke-width="2"/>
    </svg>
  `),
  iconSize: [24, 24],
  iconAnchor: [12, 12],
  popupAnchor: [0, -12],
});

interface MapViewProps {
  stations: Station[];
  waypoints: Coordinate[];
  route: [number, number][] | null;
  stops: FuelStop[];
  onMapClick?: (coord: Coordinate) => void;
}

function FitBounds({ bounds }: { bounds: L.LatLngBoundsExpression | null }) {
  const map = useMap();
  useEffect(() => {
    if (bounds) {
      map.fitBounds(bounds, { padding: [50, 50] });
    }
  }, [map, bounds]);
  return null;
}

function MapClickHandler({ onClick }: { onClick?: (coord: Coordinate) => void }) {
  const map = useMap();
  useEffect(() => {
    if (!onClick) return;
    const handler = (e: L.LeafletMouseEvent) => {
      onClick({ lat: e.latlng.lat, lon: e.latlng.lng });
    };
    map.on('click', handler);
    return () => { map.off('click', handler); };
  }, [map, onClick]);
  return null;
}

export default function MapView({ stations, waypoints, route, stops, onMapClick }: MapViewProps) {
  // Calculate bounds
  const allPoints: [number, number][] = [
    ...stations.map(s => [s.lat, s.lon] as [number, number]),
    ...waypoints.map(w => [w.lat, w.lon] as [number, number]),
    ...(route || []),
  ];

  const bounds = allPoints.length > 0
    ? L.latLngBounds(allPoints.map(p => L.latLng(p[0], p[1])))
    : null;

  // Get station IDs that are stops
  const stopIds = new Set(stops.map(s => s.stationId));

  return (
    <MapContainer
      center={[39.8283, -98.5795]} // Center of US
      zoom={4}
      style={{ height: '100%', width: '100%' }}
    >
      <TileLayer
        attribution='&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a>'
        url="https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png"
      />

      <FitBounds bounds={bounds} />
      <MapClickHandler onClick={onMapClick} />

      {/* Route polyline */}
      {route && route.length > 0 && (
        <Polyline
          positions={route}
          pathOptions={{ color: '#3b82f6', weight: 4, opacity: 0.8 }}
        />
      )}

      {/* Waypoints */}
      {waypoints.map((wp, i) => (
        <Marker key={`wp-${i}`} position={[wp.lat, wp.lon]} icon={waypointIcon}>
          <Popup>
            <strong>Waypoint {i + 1}</strong><br />
            {wp.lat.toFixed(4)}, {wp.lon.toFixed(4)}
          </Popup>
        </Marker>
      ))}

      {/* Stations */}
      {stations.map(station => {
        const isStop = stopIds.has(station.id);
        const stop = stops.find(s => s.stationId === station.id);
        return (
          <Marker
            key={`station-${station.id}`}
            position={[station.lat, station.lon]}
            icon={isStop ? selectedStationIcon : stationIcon}
          >
            <Popup>
              <strong>{station.name || `Station ${station.id}`}</strong><br />
              Price: ${station.price.toFixed(2)}/gal<br />
              {isStop && stop && (
                <>
                  <hr style={{ margin: '8px 0' }} />
                  <span style={{ color: '#22c55e', fontWeight: 'bold' }}>
                    Stop: {stop.gallons.toFixed(1)} gal<br />
                    Cost: ${stop.cost.toFixed(2)}
                  </span>
                </>
              )}
            </Popup>
          </Marker>
        );
      })}

      {/* Stop sequence indicators */}
      {stops.map((stop, i) => {
        const station = stations.find(s => s.id === stop.stationId);
        if (!station) return null;
        return (
          <CircleMarker
            key={`stop-${i}`}
            center={[station.lat, station.lon]}
            radius={20}
            pathOptions={{
              color: '#22c55e',
              fillColor: '#22c55e',
              fillOpacity: 0.2,
              weight: 2,
            }}
          />
        );
      })}
    </MapContainer>
  );
}
