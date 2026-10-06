export interface RouteStop { name: string; lon: number; lat: number; seq: number; }
export interface DispatchRoute {
  id: string;
  label: string;
  color: string;
  distance_km: number;
  stops: RouteStop[];
  geometry: GeoJSON.LineString;   // road-following path
}
export interface RoutePlan {
  source: 'velo' | 'sample';      // provenance; 'sample' is clearly flagged in the UI
  note?: string;
  depot: { name: string; lon: number; lat: number };
  routes: DispatchRoute[];
}
