import { test } from 'node:test';
import assert from 'node:assert/strict';

import { vehicleCost, type Tariff } from './cost.js';

const TARIFF: Tariff = {
  currency: 'HUF', source: 'demo',
  perKmDefault: 200, fixedPerVehicle: 30_000,
  perKmByClass: { SEMI: 300 }, fixedByClass: { SEMI: 50_000 },
  subcontractorMultiplier: 1.1,
};

test('vehicleCost: default rates when class is unknown', () => {
  const c = vehicleCost(TARIFF, null, false, 100);
  assert.equal(c.fixed, 30_000);
  assert.equal(c.variable, 200 * 100);
  assert.equal(c.total, 30_000 + 20_000);
});

test('vehicleCost: per-class override applies (substring match)', () => {
  const c = vehicleCost(TARIFF, 'SEMI_TRAILER', false, 100);
  assert.equal(c.fixed, 50_000);
  assert.equal(c.variable, 300 * 100);
});

test('vehicleCost: subcontractor multiplier applies to fixed and variable', () => {
  const c = vehicleCost(TARIFF, null, true, 100);
  assert.equal(c.fixed, Math.round(30_000 * 1.1));
  assert.equal(c.variable, Math.round(200 * 100 * 1.1));
  assert.equal(c.total, c.fixed + c.variable);
});
