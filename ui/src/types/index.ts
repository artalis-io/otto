// FuelWise Type Definitions

export interface Coordinate {
  lat: number;
  lon: number;
}

export interface Station {
  id: number;
  lat: number;
  lon: number;
  price: number;
  name?: string;
}

export interface SnappedStation {
  stationId: number;
  distanceFromStart: number;
  perpDistance: number;
  price: number;
  snapPoint?: Coordinate;
}

export interface RouteSegment {
  start: number;      // start distance in miles
  weight?: number;    // cargo weight in lbs
  mpg: number;        // fuel consumption rate
}

export interface VehicleConfig {
  tankCapacity: number;
  currentFuel: number;
  consumptionMpg: number;
  minimumFuel: number;
  minPurchase?: number;
  stopCost?: number;
}

export interface OptimizeRequest {
  stations: Station[];
  route: [number, number][];  // [lat, lon][]
  segments?: RouteSegment[];
  tankCapacity: number;
  currentFuel: number;
  consumptionMpg: number;
  minimumFuel: number;
  maxDistance?: number;
  minPurchase?: number;
  stopCost?: number;
}

export interface FuelStop {
  stationId: number;
  distanceFromStart?: number;
  gallons: number;
  cost: number;
}

export interface OptimizeResponse {
  status: string;
  routeDistance: number;
  stationsFiltered: number;
  numStops: number;
  totalCost: number;
  grossCost: number;
  remainingFuel: number;
  stops: FuelStop[];
}

export interface FilterResponse {
  count: number;
  stations: SnappedStation[];
}

// App state types
export interface AppState {
  stations: Station[];
  waypoints: Coordinate[];
  route: [number, number][] | null;
  vehicleConfig: VehicleConfig;
  segments: RouteSegment[];
  result: OptimizeResponse | null;
  loading: boolean;
  error: string | null;
}

export type AppAction =
  | { type: 'SET_STATIONS'; payload: Station[] }
  | { type: 'SET_WAYPOINTS'; payload: Coordinate[] }
  | { type: 'SET_ROUTE'; payload: [number, number][] }
  | { type: 'SET_CONFIG'; payload: Partial<VehicleConfig> }
  | { type: 'SET_SEGMENTS'; payload: RouteSegment[] }
  | { type: 'SET_RESULT'; payload: OptimizeResponse }
  | { type: 'SET_LOADING'; payload: boolean }
  | { type: 'SET_ERROR'; payload: string | null }
  | { type: 'CLEAR_RESULT' };
