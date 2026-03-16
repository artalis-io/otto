/** Surge TypeScript definitions. */

export interface SurgeConfig {
    max_iterations?: number;
    max_time_seconds?: number;
    seed?: number;
    deterministic?: boolean;
    segment_size?: number;
    q_min?: number;
    q_max?: number;
    lexicographic_objective?: boolean;
    accept_type?: 'sa' | 'rrt' | 'improving';
    adaptive_q?: boolean;
}

export interface Depot {
    x?: number;
    y?: number;
    location_id?: number;
    tw_early?: number;
    tw_late?: number;
    max_simultaneous?: number;
}

export interface Vehicle {
    start_depot_id: number;
    end_depot_id: number;
    shift_early?: number;
    shift_late?: number;
    capacity?: number[];
    qualifications?: number;
    open_end?: boolean;
    max_duration?: number;
    max_tasks?: number;
    max_distance?: number;
    fixed_cost?: number;
    cost_per_distance?: number;
    cost_per_duration?: number;
    cost_per_waiting?: number;
    cost_per_overtime?: number;
    depot_loading_seconds?: number;
    depot_unloading_seconds?: number;
    break_max_work_seconds?: number;
    break_duration_seconds?: number;
    max_total_work_seconds?: number;
    max_trips?: number;
    trip_reload_seconds?: number;
}

export interface SoftTimeWindow {
    early: number;
    late: number;
    early_penalty?: number;
    late_penalty?: number;
}

export interface TimeWindow {
    early: number;
    late: number;
}

export interface Task {
    type: 'pickup' | 'delivery' | 'service';
    x?: number;
    y?: number;
    location_id?: number;
    tw_early?: number;
    tw_late?: number;
    service_seconds?: number;
    demand?: number[];
    soft_time_window?: SoftTimeWindow;
    extra_time_windows?: TimeWindow[];
}

export interface Request {
    delivery_task_id?: number;
    pickup_task_id?: number;
    required_qualifications?: number;
    priority?: number;
    tw_early_hint?: number;
    tw_late_hint?: number;
    zone_hint?: number;
    max_ride_time?: number;
    unassigned_penalty?: number;
    allowed_vehicles?: number[];
    forbidden_vehicles?: number[];
    commodity_id?: number;
    exclusion_groups?: number[];
    setup_class?: number;
}

export interface Location {
    x: number;
    y: number;
}

export interface TravelData {
    distance_matrix?: number[][];
    duration_matrix?: number[][];
}

export interface ZoneData {
    zone_count: number;
    distance_matrix: number[];
}

export interface CommodityData {
    count: number;
    conflicts?: [number, number][];
}

export interface SetupTime {
    from: number;
    to: number;
    seconds: number;
}

export interface SetupTimeData {
    num_classes: number;
    times: SetupTime[];
}

export interface InitialRoute {
    vehicle_id: number;
    request_ids: number[];
}

export interface Problem {
    config?: SurgeConfig;
    dimension_count?: number;
    unassigned_weight?: number;
    demand_sign_convention?: 'pickup_positive_delivery_negative' | 'pickup_negative_delivery_positive';
    locations?: Location[];
    travel?: TravelData;
    depots: Depot[];
    vehicles: Vehicle[];
    tasks: Task[];
    requests: Request[];
    zones?: ZoneData;
    commodities?: CommodityData;
    exclusion_groups?: { count: number };
    setup_times?: SetupTimeData;
    initial_routes?: InitialRoute[];
}

export interface SolutionStop {
    request_id: number;
    task_id: number;
    type: 'pickup' | 'delivery' | 'service';
    arrival: number;
    service_start: number;
    departure: number;
    trip_index: number;
}

export interface SolutionBreak {
    after_stop_index: number;
    start: number;
    duration: number;
}

export interface Route {
    vehicle_id: number;
    distance: number;
    duration: number;
    waiting: number;
    overtime: number;
    tw_penalty: number;
    break_time: number;
    break_count: number;
    total_work: number;
    trip_count: number;
    stops: SolutionStop[];
    breaks?: SolutionBreak[];
}

export interface Stats {
    iterations: number;
    total_cost: number;
    total_distance: number;
    unassigned: number;
    vehicles_used: number;
    total_waiting: number;
    total_overtime: number;
    total_tw_penalty: number;
}

export interface Solution {
    status: 'ok' | 'limit' | 'infeasible' | 'error';
    error?: string;
    stats: Stats;
    routes: Route[];
    unassigned: number[];
}

export interface SurgeAPI {
    solve(problem: Problem): Solution;
    version(): string;
    health(): { status: string; service: string; version: string };
}

export function loadSurge(wasmPath?: string): Promise<SurgeAPI>;
export default loadSurge;
