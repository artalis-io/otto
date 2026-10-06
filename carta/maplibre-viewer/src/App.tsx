import { useEffect, useState } from 'react';
import { MapView } from './MapView';
import { RoutePanel } from './RoutePanel';
import type { RoutePlan } from './types';

export default function App() {
  const [plan, setPlan] = useState<RoutePlan | null>(null);
  const [selected, setSelected] = useState<string | null>(null);
  const [err, setErr] = useState<string | null>(null);

  useEffect(() => {
    fetch('/routes/plan.json')
      .then((r) => { if (!r.ok) throw new Error('plan ' + r.status); return r.json(); })
      .then(setPlan)
      .catch((e) => setErr(String(e)));
  }, []);

  if (err) return <div className="fatal">Failed to load plan: {err}</div>;
  if (!plan) return <div className="fatal">Loading plan...</div>;

  return (
    <div className="layout">
      <MapView plan={plan} selected={selected} />
      <RoutePanel plan={plan} selected={selected} onSelect={setSelected} />
    </div>
  );
}
