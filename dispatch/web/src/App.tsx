import { useEffect, useState } from 'react';
import { motion, useReducedMotion } from 'motion/react';
import { PanelLeftClose, PanelLeftOpen, PanelRightClose, PanelRightOpen } from 'lucide-react';
import { TooltipProvider } from '@/components/ui/tooltip';
import { Button } from '@/components/ui/button';
import { TopBar } from '@/components/TopBar';
import { KpiStrip } from '@/components/KpiStrip';
import { FleetPanel } from '@/components/FleetPanel';
import { Inspector } from '@/components/Inspector';
import { Timeline } from '@/components/Timeline';
import { MapView } from '@/map/MapView';
import type { Plan, Selection } from '@/types';

/*
 * OTTO Dispatch workspace — static scaffold. One desktop-first workspace:
 * graphite top bar, KPI strip, collapsible left fleet panel, center map, bottom
 * timeline, right inspector. Loads public/sample-plan.json; no live backend yet.
 * Panels collapse; selection is a single lifted piece of state.
 */
export default function App() {
  const [plan, setPlan] = useState<Plan | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [selection, setSelection] = useState<Selection>(null);
  const [leftOpen, setLeftOpen] = useState(true);
  const [rightOpen, setRightOpen] = useState(true);
  // Gate motion on the OS setting: no entrance animation when reduced.
  const reduceMotion = useReducedMotion();

  useEffect(() => {
    // Scaffold data source: the committed sample plan. Swap for GET
    // /api/plans/:id when the backend is wired.
    fetch('/sample-plan.json')
      .then((r) => {
        if (!r.ok) throw new Error(`sample-plan.json: ${r.status}`);
        return r.json();
      })
      .then((data: Plan) => setPlan(data))
      .catch((e: unknown) => setError(e instanceof Error ? e.message : String(e)));
  }, []);

  if (error) {
    return (
      <div className="flex h-full items-center justify-center text-sm text-destructive">
        Failed to load sample plan: {error}
      </div>
    );
  }
  if (!plan) {
    return (
      <div className="flex h-full items-center justify-center text-sm text-muted-foreground">
        Loading sample plan…
      </div>
    );
  }

  return (
    <TooltipProvider delayDuration={200}>
      <div className="flex h-full flex-col overflow-hidden">
        <TopBar plan={plan} />
        <KpiStrip plan={plan} />

        {/* Sample-data banner: this skeleton is driven by a hand-authored plan. */}
        <motion.div
          initial={reduceMotion ? false : { opacity: 0, y: -6 }}
          animate={{ opacity: 1, y: 0 }}
          transition={{ duration: 0.25 }}
          className="flex shrink-0 items-center gap-2 border-b border-divider bg-warning/10 px-4 py-1 text-[11px] text-warning"
        >
          <span className="font-semibold uppercase tracking-wide">Sample</span>
          <span className="text-foreground/70">
            Hand-authored plan near Gyermely/Budapest. Geometry is faked, not road-following. No
            backend connected.
          </span>
        </motion.div>

        {/* Center region: left panel | map+timeline | right inspector */}
        <div className="flex min-h-0 flex-1">
          {leftOpen ? (
            <aside className="w-[300px] shrink-0 border-r border-divider">
              <FleetPanel plan={plan} selection={selection} onSelect={setSelection} />
            </aside>
          ) : null}

          <main className="flex min-w-0 flex-1 flex-col">
            <div className="relative min-h-0 flex-1">
              {/* Panel toggles float over the map's top-left. */}
              <div className="absolute left-3 top-3 z-10 flex gap-1.5">
                <Button
                  variant="graphite"
                  size="icon"
                  className="h-8 w-8 opacity-90"
                  aria-label={leftOpen ? 'Collapse fleet panel' : 'Open fleet panel'}
                  onClick={() => setLeftOpen((o) => !o)}
                >
                  {leftOpen ? <PanelLeftClose className="h-4 w-4" /> : <PanelLeftOpen className="h-4 w-4" />}
                </Button>
                <Button
                  variant="graphite"
                  size="icon"
                  className="h-8 w-8 opacity-90"
                  aria-label={rightOpen ? 'Collapse inspector' : 'Open inspector'}
                  onClick={() => setRightOpen((o) => !o)}
                >
                  {rightOpen ? <PanelRightClose className="h-4 w-4" /> : <PanelRightOpen className="h-4 w-4" />}
                </Button>
              </div>
              <MapView plan={plan} />
            </div>
            <div className="h-44 shrink-0 border-t border-divider">
              <Timeline plan={plan} />
            </div>
          </main>

          {rightOpen ? (
            <aside className="w-[320px] shrink-0 border-l border-divider">
              <Inspector plan={plan} selection={selection} />
            </aside>
          ) : null}
        </div>
      </div>
    </TooltipProvider>
  );
}
