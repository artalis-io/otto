/* Display helpers. Surge times are seconds-from-midnight; show HH:MM. */

export function hhmm(secFromMidnight: number): string {
  const s = Math.max(0, Math.round(secFromMidnight));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}`;
}

export function km(value: number, digits = 0): string {
  return `${value.toLocaleString('en-US', {
    minimumFractionDigits: digits,
    maximumFractionDigits: digits,
  })} km`;
}

export function seconds(value: number, digits = 1): string {
  return `${value.toFixed(digits)} s`;
}

export function pct(value: number): string {
  return `${Math.round(value * 100)}%`;
}

/** Fraction in [0,1], clamped, for capacity bars. */
export function ratio(used: number, cap: number): number {
  if (cap <= 0) return 0;
  return Math.min(1, Math.max(0, used / cap));
}
