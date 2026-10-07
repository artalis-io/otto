import { test, expect } from '@playwright/test';
import { loadApp, expectNoErrors, selectVehicle } from './helpers';

test.describe('staged edit stack', () => {
  test('mark-unavailable stacks (no auto-solve) and the Changes dialog lists + undoes it', async ({ page }) => {
    await loadApp(page);
    await selectVehicle(page, 'RIC-124');
    await page.getByRole('button', { name: /Mark RIC-124 unavailable/ }).click();

    // it must NOT have started a solve
    await expect(page.getByText('Optimizing')).toHaveCount(0);
    // staged indicators
    await expect(page.getByText(/staged change/)).toBeVisible();
    await expect(page.getByRole('button', { name: /Changes/ })).toBeVisible();

    // open the Changes dialog -> RIC-124 under Removed vehicles
    await page.getByRole('button', { name: /Changes/ }).click();
    const dialog = page.getByRole('dialog');
    await expect(dialog.getByText('Removed vehicles')).toBeVisible();
    await expect(dialog.getByText('RIC-124')).toBeVisible();

    // undo it
    await dialog.getByRole('button', { name: 'Undo' }).first().click();
    await expect(dialog.getByText('No staged changes.')).toBeVisible();
    expectNoErrors(page);
  });

  test('vehicle constraint edit stacks', async ({ page }) => {
    await loadApp(page);
    await selectVehicle(page, 'RIX-419');
    await page.getByRole('button', { name: /Edit constraints/ }).click();
    const capKg = page.locator('input[type=number]').first();
    await capKg.fill('9000');
    await capKg.blur();
    await expect(page.getByRole('button', { name: /Changes/ })).toBeVisible();
    await page.getByRole('button', { name: /Changes/ }).click();
    const dialog = page.getByRole('dialog');
    await expect(dialog.getByText('Vehicle constraints')).toBeVisible();
    await expect(dialog.getByText(/9000/)).toBeVisible();
    expectNoErrors(page);
  });
});
