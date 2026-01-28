/**
 * FuelWise WASM JavaScript Wrapper
 *
 * Provides a high-level API for the FuelWise WebAssembly module.
 *
 * Usage:
 *   import { loadFuelWise } from './fuelwise-wrapper.js';
 *   const fw = await loadFuelWise();
 *   const result = fw.optimize(stations, route, config);
 */

let wasmModule = null;

/**
 * Load and initialize the FuelWise WASM module.
 * @returns {Promise<FuelWiseAPI>}
 */
export async function loadFuelWise() {
    if (wasmModule) {
        return createAPI(wasmModule);
    }

    // Dynamic import of the Emscripten-generated module
    const FuelWise = (await import('./build/fuelwise.js')).default;
    wasmModule = await FuelWise();
    return createAPI(wasmModule);
}

/**
 * Create the high-level API wrapper
 */
function createAPI(module) {
    const api = {
        /**
         * Get library version
         */
        version() {
            const ptr = module._wasm_version();
            return module.UTF8ToString(ptr);
        },

        /**
         * Calculate distance between two coordinates (miles)
         */
        distance(lat1, lon1, lat2, lon2) {
            return module._wasm_haversine(lat1, lon1, lat2, lon2);
        },

        /**
         * Calculate polyline length (miles)
         * @param {Array<[number, number]>} points - Array of [lat, lon] pairs
         */
        polylineLength(points) {
            const ptr = allocateF64Array(points.flat());
            const length = module._wasm_polyline_length(ptr, points.length);
            module._wasm_free(ptr);
            return length;
        },

        /**
         * Filter stations to those near a route
         * @param {Array<{id, lat, lon, price}>} stations
         * @param {Array<[number, number]>} route - Array of [lat, lon] pairs
         * @param {number} maxDistance - Maximum distance from route (miles)
         * @returns {Array<{stationId, distanceFromStart, perpDistance, price}>}
         */
        filterStations(stations, route, maxDistance = 5) {
            // Flatten stations: [lat, lon, price, id] x n
            const stationsFlat = [];
            for (const s of stations) {
                stationsFlat.push(s.lat, s.lon, s.price, s.id);
            }

            const stationsPtr = allocateF64Array(stationsFlat);
            const routePtr = allocateF64Array(route.flat());

            // Result buffer - 4 values per station
            const maxResults = stations.length;
            const resultPtr = module._wasm_malloc(maxResults * 4 * 8);

            const count = module._wasm_filter_stations(
                stationsPtr, stations.length,
                routePtr, route.length,
                maxDistance,
                resultPtr, maxResults * 4
            );

            const result = [];
            for (let i = 0; i < count; i++) {
                result.push({
                    stationId: module.HEAPF64[resultPtr / 8 + i * 4 + 0],
                    distanceFromStart: module.HEAPF64[resultPtr / 8 + i * 4 + 1],
                    perpDistance: module.HEAPF64[resultPtr / 8 + i * 4 + 2],
                    price: module.HEAPF64[resultPtr / 8 + i * 4 + 3]
                });
            }

            module._wasm_free(stationsPtr);
            module._wasm_free(routePtr);
            module._wasm_free(resultPtr);

            return result;
        },

        /**
         * Solve a refueling problem (stations already snapped to route)
         * @param {Array<{id, distance, price}>} stations - Sorted by distance
         * @param {Object} config
         * @param {number} config.totalDistance
         * @param {number} config.tankCapacity
         * @param {number} config.currentFuel
         * @param {number} config.consumptionMpg
         * @param {number} config.minimumFuel
         */
        solve(stations, config) {
            // Flatten: [id, distance, price] x n
            const stationsFlat = [];
            for (const s of stations) {
                stationsFlat.push(s.id, s.distance, s.price);
            }

            const stationsPtr = allocateF64Array(stationsFlat);
            const purchasesPtr = module._wasm_malloc(stations.length * 8);
            const metaPtr = module._wasm_malloc(4 * 8);

            module._wasm_solve_simple(
                stationsPtr, stations.length,
                config.totalDistance,
                config.tankCapacity,
                config.currentFuel,
                config.consumptionMpg,
                config.minimumFuel,
                purchasesPtr,
                metaPtr
            );

            const status = module.HEAPF64[metaPtr / 8 + 0];
            const numStops = module.HEAPF64[metaPtr / 8 + 1];
            const totalCost = module.HEAPF64[metaPtr / 8 + 2];
            const remainingFuel = module.HEAPF64[metaPtr / 8 + 3];

            const stops = [];
            for (let i = 0; i < stations.length; i++) {
                const gallons = module.HEAPF64[purchasesPtr / 8 + i];
                if (gallons > 0.001) {
                    stops.push({
                        stationId: stations[i].id,
                        distance: stations[i].distance,
                        gallons,
                        cost: gallons * stations[i].price
                    });
                }
            }

            module._wasm_free(stationsPtr);
            module._wasm_free(purchasesPtr);
            module._wasm_free(metaPtr);

            return {
                status: ['optimal', 'infeasible', 'unbounded', 'error'][status] || 'error',
                numStops,
                totalCost,
                remainingFuel,
                stops
            };
        },

        /**
         * Solve with variable consumption (piecewise segments)
         * @param {Array<{id, distance, price}>} stations
         * @param {Array<{startDistance, weight, mpg}>} segments
         * @param {Object} config
         */
        solveWithSegments(stations, segments, config) {
            // Flatten stations: [id, distance, price] x n
            const stationsFlat = [];
            for (const s of stations) {
                stationsFlat.push(s.id, s.distance, s.price);
            }

            // Flatten segments: [start_dist, weight, mpg] x n
            const segmentsFlat = [];
            for (const seg of segments) {
                segmentsFlat.push(seg.startDistance, seg.weight || 0, seg.mpg);
            }

            const stationsPtr = allocateF64Array(stationsFlat);
            const segmentsPtr = allocateF64Array(segmentsFlat);
            const purchasesPtr = module._wasm_malloc(stations.length * 8);
            const metaPtr = module._wasm_malloc(4 * 8);

            module._wasm_solve_segments(
                stationsPtr, stations.length,
                segmentsPtr, segments.length,
                config.totalDistance,
                config.tankCapacity,
                config.currentFuel,
                config.minimumFuel,
                config.minPurchase || 0,
                config.stopCost || 0,
                purchasesPtr,
                metaPtr
            );

            const status = module.HEAPF64[metaPtr / 8 + 0];
            const numStops = module.HEAPF64[metaPtr / 8 + 1];
            const totalCost = module.HEAPF64[metaPtr / 8 + 2];
            const remainingFuel = module.HEAPF64[metaPtr / 8 + 3];

            const stops = [];
            for (let i = 0; i < stations.length; i++) {
                const gallons = module.HEAPF64[purchasesPtr / 8 + i];
                if (gallons > 0.001) {
                    stops.push({
                        stationId: stations[i].id,
                        distance: stations[i].distance,
                        gallons,
                        cost: gallons * stations[i].price
                    });
                }
            }

            module._wasm_free(stationsPtr);
            module._wasm_free(segmentsPtr);
            module._wasm_free(purchasesPtr);
            module._wasm_free(metaPtr);

            return {
                status: ['optimal', 'infeasible', 'unbounded', 'error'][status] || 'error',
                numStops,
                totalCost,
                remainingFuel,
                stops
            };
        },

        /**
         * Full optimization pipeline: filter stations + solve
         * @param {Array<{id, lat, lon, price}>} stations
         * @param {Array<[number, number]>} route
         * @param {Object} config
         */
        optimize(stations, route, config) {
            // Flatten stations: [lat, lon, price, id] x n
            const stationsFlat = [];
            for (const s of stations) {
                stationsFlat.push(s.lat, s.lon, s.price, s.id);
            }

            // Flatten segments if provided
            let segmentsPtr = 0;
            let numSegments = 0;
            if (config.segments && config.segments.length > 0) {
                const segmentsFlat = [];
                for (const seg of config.segments) {
                    segmentsFlat.push(seg.startDistance, seg.weight || 0, seg.mpg);
                }
                segmentsPtr = allocateF64Array(segmentsFlat);
                numSegments = config.segments.length;
            }

            const stationsPtr = allocateF64Array(stationsFlat);
            const routePtr = allocateF64Array(route.flat());
            const purchasesPtr = module._wasm_malloc(stations.length * 8);
            const metaPtr = module._wasm_malloc(6 * 8);
            const stationIdsPtr = module._wasm_malloc(stations.length * 4);

            module._wasm_optimize_route(
                stationsPtr, stations.length,
                routePtr, route.length,
                segmentsPtr, numSegments,
                config.tankCapacity,
                config.currentFuel,
                config.consumptionMpg || 6.5,
                config.minimumFuel || 0,
                config.maxFilterDistance || 5,
                purchasesPtr,
                metaPtr,
                stationIdsPtr
            );

            const status = module.HEAPF64[metaPtr / 8 + 0];
            const numFiltered = module.HEAPF64[metaPtr / 8 + 1];
            const numStops = module.HEAPF64[metaPtr / 8 + 2];
            const totalCost = module.HEAPF64[metaPtr / 8 + 3];
            const remainingFuel = module.HEAPF64[metaPtr / 8 + 4];
            const routeDistance = module.HEAPF64[metaPtr / 8 + 5];

            const stops = [];
            for (let i = 0; i < numFiltered; i++) {
                const gallons = module.HEAPF64[purchasesPtr / 8 + i];
                const stationId = module.HEAP32[stationIdsPtr / 4 + i];
                if (gallons > 0.001) {
                    stops.push({ stationId, gallons });
                }
            }

            module._wasm_free(stationsPtr);
            module._wasm_free(routePtr);
            module._wasm_free(purchasesPtr);
            module._wasm_free(metaPtr);
            module._wasm_free(stationIdsPtr);
            if (segmentsPtr) module._wasm_free(segmentsPtr);

            return {
                status: ['optimal', 'infeasible', 'unbounded', 'error'][status] || 'error',
                routeDistance,
                stationsFiltered: numFiltered,
                numStops,
                totalCost,
                remainingFuel,
                stops
            };
        }
    };

    // Helper to allocate a Float64Array in WASM memory
    function allocateF64Array(arr) {
        const ptr = module._wasm_malloc(arr.length * 8);
        for (let i = 0; i < arr.length; i++) {
            module.HEAPF64[ptr / 8 + i] = arr[i];
        }
        return ptr;
    }

    return api;
}

export default loadFuelWise;
