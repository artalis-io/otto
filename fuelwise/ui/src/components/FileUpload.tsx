import { useRef } from 'react';
import Papa from 'papaparse';
import type { Station, Coordinate } from '../types';

interface FileUploadProps {
  onStationsLoaded: (stations: Station[]) => void;
  onWaypointsLoaded: (waypoints: Coordinate[]) => void;
}

export default function FileUpload({ onStationsLoaded, onWaypointsLoaded }: FileUploadProps) {
  const stationsInputRef = useRef<HTMLInputElement>(null);
  const waypointsInputRef = useRef<HTMLInputElement>(null);

  const parseStationsCSV = (file: File) => {
    Papa.parse(file, {
      header: true,
      skipEmptyLines: true,
      complete: (results) => {
        const stations: Station[] = [];
        let idCounter = 1;

        for (const row of results.data as Record<string, string>[]) {
          // Try different column name variations
          const lat = parseFloat(row.lat || row.latitude || row.Lat || row.Latitude || '');
          const lon = parseFloat(row.lon || row.lng || row.longitude || row.Lon || row.Lng || row.Longitude || '');
          const price = parseFloat(row.price || row.Price || row.price_per_gallon || '');
          const id = parseInt(row.id || row.ID || row.station_id || '') || idCounter++;
          const name = row.name || row.Name || row.station_name || undefined;

          if (!isNaN(lat) && !isNaN(lon) && !isNaN(price)) {
            stations.push({ id, lat, lon, price, name });
          }
        }

        if (stations.length === 0) {
          alert('No valid stations found. CSV should have columns: lat, lon, price (and optionally id, name)');
          return;
        }

        onStationsLoaded(stations);
      },
      error: (error) => {
        alert(`Failed to parse CSV: ${error.message}`);
      },
    });
  };

  const parseWaypointsCSV = (file: File) => {
    Papa.parse(file, {
      header: true,
      skipEmptyLines: true,
      complete: (results) => {
        const waypoints: Coordinate[] = [];

        for (const row of results.data as Record<string, string>[]) {
          const lat = parseFloat(row.lat || row.latitude || row.Lat || row.Latitude || '');
          const lon = parseFloat(row.lon || row.lng || row.longitude || row.Lon || row.Lng || row.Longitude || '');

          if (!isNaN(lat) && !isNaN(lon)) {
            waypoints.push({ lat, lon });
          }
        }

        if (waypoints.length === 0) {
          alert('No valid waypoints found. CSV should have columns: lat, lon');
          return;
        }

        onWaypointsLoaded(waypoints);
      },
      error: (error) => {
        alert(`Failed to parse CSV: ${error.message}`);
      },
    });
  };

  return (
    <div className="file-upload">
      <h3>Load Data</h3>

      <div className="upload-section">
        <label>Fuel Stations (CSV)</label>
        <p className="help-text">Columns: lat, lon, price, [id], [name]</p>
        <input
          ref={stationsInputRef}
          type="file"
          accept=".csv"
          onChange={(e) => {
            const file = e.target.files?.[0];
            if (file) parseStationsCSV(file);
          }}
        />
        <button
          className="btn-secondary"
          onClick={() => stationsInputRef.current?.click()}
        >
          Upload Stations
        </button>
      </div>

      <div className="upload-section">
        <label>Waypoints (CSV)</label>
        <p className="help-text">Columns: lat, lon</p>
        <input
          ref={waypointsInputRef}
          type="file"
          accept=".csv"
          onChange={(e) => {
            const file = e.target.files?.[0];
            if (file) parseWaypointsCSV(file);
          }}
        />
        <button
          className="btn-secondary"
          onClick={() => waypointsInputRef.current?.click()}
        >
          Upload Waypoints
        </button>
      </div>

      <div className="upload-section">
        <label>Or click on the map to add waypoints</label>
      </div>
    </div>
  );
}
