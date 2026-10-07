import { test } from 'node:test';
import assert from 'node:assert/strict';

import { buildSchema, missingRequired, parseReconcile, parseSemantic } from './onboard.js';
import { suggestMapping } from './catalog.js';

test('buildSchema: generates a valid nx_schema from a column mapping', () => {
  const s = buildSchema('orders', { order_no: 0, customer: 4, city: 6, weight_kg: 11, pallets: 12 }) as {
    nx_schema: number; columns: { source: number; target: string; type: string; required?: boolean }[];
  };
  assert.equal(s.nx_schema, 2);
  const byTarget = new Map(s.columns.map((c) => [c.target, c]));
  assert.equal(byTarget.get('order_no')!.source, 0);
  assert.equal(byTarget.get('order_no')!.required, true);   // required field flagged
  assert.equal(byTarget.get('weight_kg')!.type, 'double');
  assert.equal(byTarget.get('city')!.type, 'string');
  assert.equal(byTarget.size, 5);
});

test('missingRequired: reports unmapped required fields', () => {
  assert.deepEqual(missingRequired('orders', { order_no: 0 }).sort(), ['city', 'customer']);
  assert.deepEqual(missingRequired('orders', { order_no: 0, customer: 1, city: 2 }), []);
});

test('suggestMapping: auto-maps Hungarian headers to canonical fields', () => {
  const headers = ['Megrendelés száma', '', 'Fuvarszám', 'Szállítási cím azonosító', 'Szállítási cím megnevezés',
    'Kiszállítási cím: irányítószám', 'Kiszállítási cím: település', 'Kiszállítási cím: utca, házszám'];
  const m = suggestMapping('orders', headers);
  assert.equal(m.order_no, 0);
  assert.equal(m.customer, 4);
  assert.equal(m.zip, 5);
  assert.equal(m.city, 6);
  assert.equal(m.street, 7);
});

const RECONCILE_OK = `--- reconciliation gate ---
=== gyermelyi_orders_raw.csv  (schema onboard-orders) ===
  provenance sha256 : OK
  rows reconciled   : 332  (Stage B rejects skipped: 0)
  fields verified   : 3  ['city', 'customer', 'order_no']
  mismatches        : 0
--- semantic sanity (advisory) ---
    [WARN] duplicate-key (1)
    [INFO] zip-shape (16)`;

const RECONCILE_FAIL = `=== x.csv (schema onboard-orders) ===
  provenance sha256 : MISMATCH
  rows reconciled   : 10
  fields verified   : 2  ['a', 'b']
  mismatches        : 2
    MISMATCH row 3 field weight_kg: canonical=5 raw=50
    MISMATCH row 7 field city: canonical=X raw=Y`;

test('parseReconcile: clean gate', () => {
  const r = parseReconcile(RECONCILE_OK);
  assert.equal(r.provenanceOk, true);
  assert.equal(r.rowsReconciled, 332);
  assert.equal(r.mismatches, 0);
  assert.deepEqual(r.fieldsVerified, ['city', 'customer', 'order_no']);
});

test('parseReconcile: failed gate surfaces mismatches', () => {
  const r = parseReconcile(RECONCILE_FAIL);
  assert.equal(r.provenanceOk, false);
  assert.equal(r.mismatches, 2);
  assert.equal(r.mismatchSamples.length, 2);
  assert.ok(r.mismatchSamples[0]!.startsWith('MISMATCH'));
});

test('parseSemantic: counts errors/warnings/info by level', () => {
  const s = parseSemantic(RECONCILE_OK);
  assert.equal(s.errors, 0);
  assert.equal(s.warnings, 1);
  assert.equal(s.info, 16);
  assert.equal(s.items.length, 2);
  assert.deepEqual(s.items[0], { level: 'WARN', kind: 'duplicate-key', count: 1 });
});
