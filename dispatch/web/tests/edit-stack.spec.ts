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

  test('drag-to-reassign stages a pin onto the drop-target vehicle', async ({ page }) => {
    await loadApp(page);
    // Pick a real order + a different target vehicle from the loaded plan, then
    // dispatch a native drop onto the target card (HTML5 DnD, deterministic).
    const picked = await page.evaluate(async () => {
      const days = await (await fetch('/api/days')).json();
      const d1 = days.find((d: { id: string }) => d.id === 'day1') ?? days[0];
      const plan = await (await fetch(`/api/plans/${d1.baselinePlanId}`)).json();
      const v0 = plan.vehicles[0], v1 = plan.vehicles[1];
      return { order: v0.trips[0].stops[0].orderNo as string, target: v1.ref as string };
    });
    const dropped = await page.evaluate(({ order, target, MIME }) => {
      const fleet = document.querySelectorAll('aside')[0];
      const refSpan = [...fleet!.querySelectorAll('span')].find((s) => s.textContent?.trim() === target);
      const card = refSpan?.closest('div.rounded-md');
      if (!card) return false;
      const dt = new DataTransfer();
      dt.setData(MIME, order);
      card.dispatchEvent(new DragEvent('dragover', { bubbles: true, cancelable: true, dataTransfer: dt }));
      card.dispatchEvent(new DragEvent('drop', { bubbles: true, cancelable: true, dataTransfer: dt }));
      return true;
    }, { ...picked, MIME: 'application/x-otto-order' });
    expect(dropped).toBe(true);

    // The pin must stage (not auto-solve) and show the target vehicle.
    await expect(page.getByText('Optimizing')).toHaveCount(0);
    await expect(page.getByText(/staged change/)).toBeVisible();
    await page.getByRole('button', { name: /Changes/ }).click();
    const dialog = page.getByRole('dialog');
    await expect(dialog.getByText(picked.target)).toBeVisible();
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
