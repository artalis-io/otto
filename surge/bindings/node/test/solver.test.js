import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { loadSurge } from '../src/index.js';

/** Minimal 1-vehicle, 1-delivery problem. */
function singleDeliveryProblem() {
    return {
        config: { max_iterations: 100, seed: 42, deterministic: true },
        depots: [{ x: 0, y: 0, tw_early: 0, tw_late: 10000 }],
        vehicles: [{
            start_depot_id: 0, end_depot_id: 0,
            shift_early: 0, shift_late: 10000,
            capacity: [100],
        }],
        tasks: [{
            type: 'delivery', x: 10, y: 0,
            tw_early: 0, tw_late: 10000,
            service_seconds: 60, demand: [-10],
        }],
        requests: [{ delivery_task_id: 0 }],
    };
}

/** 2 vehicles, 3 deliveries. */
function multiVehicleProblem() {
    return {
        config: { max_iterations: 200, seed: 42, deterministic: true },
        depots: [{ x: 0, y: 0, tw_early: 0, tw_late: 10000 }],
        vehicles: [
            { start_depot_id: 0, end_depot_id: 0, shift_early: 0, shift_late: 10000, capacity: [100] },
            { start_depot_id: 0, end_depot_id: 0, shift_early: 0, shift_late: 10000, capacity: [100] },
        ],
        tasks: [
            { type: 'delivery', x: 10, y: 10, tw_early: 0, tw_late: 10000, service_seconds: 60, demand: [-10] },
            { type: 'delivery', x: -10, y: -10, tw_early: 0, tw_late: 10000, service_seconds: 60, demand: [-10] },
            { type: 'delivery', x: 20, y: 0, tw_early: 0, tw_late: 10000, service_seconds: 60, demand: [-10] },
        ],
        requests: [
            { delivery_task_id: 0 },
            { delivery_task_id: 1 },
            { delivery_task_id: 2 },
        ],
    };
}

describe('Surge WASM binding', async () => {
    const surge = await loadSurge();

    it('version returns non-empty string', () => {
        const v = surge.version();
        assert.ok(typeof v === 'string' && v.length > 0, `Expected non-empty version, got: "${v}"`);
    });

    it('health returns healthy status', () => {
        const h = surge.health();
        assert.equal(h.status, 'healthy');
    });

    it('solve basic delivery', () => {
        const result = surge.solve(singleDeliveryProblem());
        assert.ok(result.status === 'ok' || result.status === 'limit');
        assert.equal(result.stats.unassigned, 0);
        assert.equal(result.routes.length, 1);
        assert.ok(result.routes[0].stops.length >= 1);
    });

    it('solve shared example (2v, 3d)', () => {
        const result = surge.solve(multiVehicleProblem());
        assert.ok(result.status === 'ok' || result.status === 'limit');
        assert.equal(result.stats.unassigned, 0);
        const totalStops = result.routes.reduce((n, r) => n + r.stops.length, 0);
        assert.equal(totalStops, 3);
    });

    it('throws on invalid input', () => {
        assert.throws(() => surge.solve({ vehicles: [{ start_depot_id: 99 }] }), /error/i);
    });
});
