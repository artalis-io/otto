// FuelWise REST API Client

import type { Station, OptimizeRequest, OptimizeResponse, FilterResponse } from '../types';

const API_BASE = import.meta.env.VITE_API_URL || 'http://localhost:8080';

async function apiCall<T>(endpoint: string, options?: RequestInit): Promise<T> {
  const response = await fetch(`${API_BASE}${endpoint}`, {
    ...options,
    headers: {
      'Content-Type': 'application/json',
      ...options?.headers,
    },
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ error: 'Request failed' }));
    throw new Error(error.error || `HTTP ${response.status}`);
  }

  return response.json();
}

export async function healthCheck(): Promise<{ status: string; version: string }> {
  return apiCall('/api/v1/health');
}

export async function filterStations(
  stations: Station[],
  route: [number, number][],
  maxDistance: number = 5
): Promise<FilterResponse> {
  return apiCall('/api/v1/filter', {
    method: 'POST',
    body: JSON.stringify({
      stations: stations.map(s => ({
        id: s.id,
        lat: s.lat,
        lon: s.lon,
        price: s.price,
      })),
      route,
      max_distance: maxDistance,
    }),
  });
}

export async function optimize(request: OptimizeRequest): Promise<OptimizeResponse> {
  const body: Record<string, unknown> = {
    stations: request.stations.map(s => ({
      id: s.id,
      lat: s.lat,
      lon: s.lon,
      price: s.price,
    })),
    route: request.route,
    tank_capacity: request.tankCapacity,
    current_fuel: request.currentFuel,
    consumption_mpg: request.consumptionMpg,
    minimum_fuel: request.minimumFuel,
    max_distance: request.maxDistance || 5,
  };

  if (request.segments && request.segments.length > 0) {
    body.segments = request.segments.map(s => ({
      start: s.start,
      weight: s.weight || 0,
      mpg: s.mpg,
    }));
  }

  if (request.minPurchase) body.min_purchase = request.minPurchase;
  if (request.stopCost) body.stop_cost = request.stopCost;
  if (request.remainingFuelValue) body.remaining_fuel_value = request.remainingFuelValue;

  const response = await apiCall<{
    status: string;
    route_distance: number;
    stations_filtered: number;
    num_stops: number;
    total_cost: number;
    gross_cost: number;
    remaining_fuel: number;
    stops: Array<{
      station_id: number;
      distance_from_start?: number;
      gallons: number;
      cost: number;
    }>;
  }>('/api/v1/optimize', {
    method: 'POST',
    body: JSON.stringify(body),
  });

  return {
    status: response.status,
    routeDistance: response.route_distance,
    stationsFiltered: response.stations_filtered,
    numStops: response.num_stops,
    totalCost: response.total_cost,
    grossCost: response.gross_cost,
    remainingFuel: response.remaining_fuel,
    stops: response.stops.map(s => ({
      stationId: s.station_id,
      distanceFromStart: s.distance_from_start,
      gallons: s.gallons,
      cost: s.cost,
    })),
  };
}

// Routing service abstraction (uses OSRM demo server by default)
const OSRM_URL = 'https://router.project-osrm.org';

export async function getRoute(
  waypoints: { lat: number; lon: number }[]
): Promise<{ route: [number, number][]; distance: number; duration: number }> {
  if (waypoints.length < 2) {
    throw new Error('At least 2 waypoints required');
  }

  const coords = waypoints.map(w => `${w.lon},${w.lat}`).join(';');
  const url = `${OSRM_URL}/route/v1/driving/${coords}?overview=full&geometries=geojson`;

  const response = await fetch(url);
  if (!response.ok) {
    throw new Error('Routing request failed');
  }

  const data = await response.json();
  if (data.code !== 'Ok' || !data.routes?.length) {
    throw new Error(data.message || 'No route found');
  }

  const route = data.routes[0];
  const coordinates: [number, number][] = route.geometry.coordinates.map(
    (c: [number, number]) => [c[1], c[0]] // GeoJSON is [lon, lat], we want [lat, lon]
  );

  return {
    route: coordinates,
    distance: route.distance / 1609.34, // meters to miles
    duration: route.duration / 60, // seconds to minutes
  };
}
