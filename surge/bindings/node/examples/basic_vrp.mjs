#!/usr/bin/env node
/**
 * Basic VRP example — 2 vehicles, 3 deliveries.
 *
 * Run from surge/node/:
 *   npm run build-wasm   # first time only
 *   node examples/basic_vrp.mjs
 */

import { loadSurge } from '../src/index.js';

const surge = await loadSurge();

console.log(`Surge version: ${surge.version()}`);
console.log('Health:', surge.health());
console.log();

const problem = {
    config: {
        max_iterations: 500,
        seed: 42,
        deterministic: true,
    },
    depots: [
        { x: 0, y: 0, tw_early: 0, tw_late: 86400 },
    ],
    vehicles: [
        {
            start_depot_id: 0, end_depot_id: 0,
            shift_early: 0, shift_late: 86400,
            capacity: [100],
        },
        {
            start_depot_id: 0, end_depot_id: 0,
            shift_early: 0, shift_late: 86400,
            capacity: [100],
        },
    ],
    tasks: [
        {
            type: 'delivery', x: 10, y: 10,
            tw_early: 0, tw_late: 86400,
            service_seconds: 300, demand: [-20],
        },
        {
            type: 'delivery', x: -15, y: 5,
            tw_early: 0, tw_late: 86400,
            service_seconds: 300, demand: [-30],
        },
        {
            type: 'delivery', x: 20, y: -10,
            tw_early: 0, tw_late: 86400,
            service_seconds: 300, demand: [-25],
        },
    ],
    requests: [
        { delivery_task_id: 0 },
        { delivery_task_id: 1 },
        { delivery_task_id: 2 },
    ],
};

const result = surge.solve(problem);

console.log(`Status: ${result.status}`);
console.log(`Vehicles used: ${result.stats.vehicles_used}`);
console.log(`Total distance: ${result.stats.total_distance.toFixed(2)}`);
console.log(`Unassigned: ${result.stats.unassigned}`);
console.log();

for (const route of result.routes) {
    console.log(`Route (vehicle ${route.vehicle_id}):`);
    console.log(`  Distance: ${route.distance.toFixed(2)}`);
    console.log(`  Duration: ${route.duration.toFixed(2)}`);
    for (const stop of route.stops) {
        console.log(`  Stop: request=${stop.request_id} type=${stop.type} arrive=${stop.arrival.toFixed(1)} depart=${stop.departure.toFixed(1)}`);
    }
    console.log();
}
