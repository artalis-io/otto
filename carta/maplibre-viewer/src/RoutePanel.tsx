import type { RoutePlan } from './types';

export function RoutePanel(props: {
  plan: RoutePlan;
  selected: string | null;
  onSelect: (id: string | null) => void;
}) {
  const { plan, selected, onSelect } = props;
  const sel = plan.routes.find((r) => r.id === selected) || null;
  return (
    <aside className="panel">
      <header className="panel-head">
        <h1>Plan viewer</h1>
        <span className="sub">Carta basemap &middot; {plan.routes.length} routes</span>
        {plan.source === 'sample' && (
          <span className="badge-sample" title={plan.note || ''}>SAMPLE DATA</span>
        )}
      </header>

      <ul className="route-list">
        {plan.routes.map((r) => (
          <li
            key={r.id}
            className={'route-item' + (r.id === selected ? ' active' : '')}
            onClick={() => onSelect(r.id === selected ? null : r.id)}
          >
            <span className="swatch" style={{ background: r.color }} />
            <span className="route-label">{r.label}</span>
            <span className="route-meta">{r.distance_km.toFixed(1)} km &middot; {r.stops.length} stops</span>
          </li>
        ))}
      </ul>

      {sel && (
        <div className="detail">
          <h2><span className="swatch" style={{ background: sel.color }} /> {sel.label}</h2>
          <div className="detail-row"><span>Distance</span><b>{sel.distance_km.toFixed(1)} km</b></div>
          <div className="detail-row"><span>Stops</span><b>{sel.stops.length}</b></div>
          <ol className="stop-list">
            <li className="depot">Depot &middot; {plan.depot.name}</li>
            {sel.stops.map((s) => <li key={s.seq}>{s.seq}. {s.name}</li>)}
          </ol>
          <button className="clear" onClick={() => onSelect(null)}>Clear selection</button>
        </div>
      )}
      {!sel && <p className="hint">Select a route to highlight it, fit the map and see its stops.</p>}
    </aside>
  );
}
