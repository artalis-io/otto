import { test, expect } from '@playwright/test';
import { homedir } from 'node:os';
import { join } from 'node:path';
import { existsSync } from 'node:fs';
import { loadApp, expectNoErrors } from './helpers';

const RAW = join(homedir(), 'artalis.io/data/gyermelyi/raw/gyermelyi_orders_raw.csv');

test.describe('data onboarding (M1)', () => {
  test.skip(!existsSync(RAW), 'raw dataset not present');

  test('upload a CSV, auto-map columns, pass the reconcile gate', async ({ page }) => {
    test.setTimeout(90_000);
    await loadApp(page);
    await page.getByRole('button', { name: 'Data' }).click();
    await page.getByRole('tab', { name: 'Upload' }).click();
    await expect(page.getByText('Choose CSV')).toBeVisible();

    await page.locator('input[type=file]').setInputFiles(RAW);
    await expect(page.getByText('Map columns')).toBeVisible();
    await expect(page.getByText(/\d+ rows/).first()).toBeVisible();

    // auto-suggest should have mapped the required fields (order_no, customer, city)
    const mapped = await page.locator('table select').evaluateAll((els) => els.filter((e) => (e as HTMLSelectElement).value !== '').length);
    expect(mapped).toBeGreaterThanOrEqual(3);

    await page.getByRole('button', { name: 'Validate' }).click();
    await expect(page.getByText('Reconcile passed')).toBeVisible({ timeout: 60_000 });
    await expect(page.getByText(/Canonical records/)).toBeVisible();
    expectNoErrors(page);
  });

  test('geocode the mapped orders and preview tiers on a map', async ({ page }) => {
    test.setTimeout(120_000);
    await loadApp(page);
    await page.getByRole('button', { name: 'Data' }).click();
    await page.getByRole('tab', { name: 'Upload' }).click();
    await page.locator('input[type=file]').setInputFiles(RAW);
    await expect(page.getByText('Map columns')).toBeVisible();
    await page.getByRole('button', { name: 'Validate' }).click();
    await expect(page.getByText('Reconcile passed')).toBeVisible({ timeout: 60_000 });

    await page.getByRole('button', { name: 'Geocode addresses' }).click();
    await expect(page.getByText(/\d+\/\d+ resolved/)).toBeVisible({ timeout: 60_000 });
    await expect(page.getByRole('dialog').locator('canvas.maplibregl-canvas')).toBeVisible();
    expectNoErrors(page);
  });
});
