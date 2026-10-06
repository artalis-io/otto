import { Play, Save, Download } from 'lucide-react';
import { Button } from '@/components/ui/button';
import { Badge } from '@/components/ui/badge';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import type { Plan } from '@/types';

/*
 * Graphite top bar: wordmark, day/scenario selector (placeholder), plan-status
 * chip, and primary/secondary actions. Actions are inert in the scaffold.
 */
export function TopBar({ plan }: { plan: Plan }) {
  const termOk = plan.provenance.termination === 'OK';
  return (
    <header className="flex h-14 shrink-0 items-center gap-4 bg-graphite px-4 text-graphite-foreground">
      <div className="flex items-center gap-2">
        <span className="text-lg font-bold tracking-tight">OTTO</span>
        <span className="text-sm font-medium text-graphite-muted">Dispatch</span>
      </div>

      <div className="mx-2 h-6 w-px bg-white/10" />

      <div className="w-56">
        <Select defaultValue={plan.day}>
          <SelectTrigger className="h-8 border-white/15 bg-white/5 text-graphite-foreground">
            <SelectValue placeholder="Select day" />
          </SelectTrigger>
          <SelectContent>
            <SelectItem value="2026-05-06">Day 1 — 2026-05-06 (baseline)</SelectItem>
            <SelectItem value="2026-05-07">Day 2 — 2026-05-07 (baseline)</SelectItem>
          </SelectContent>
        </Select>
      </div>

      <Badge
        variant={termOk ? 'default' : 'warning'}
        className="tnum"
        title="Surge termination status"
      >
        {termOk ? 'Plan OK' : 'Plan LIMIT'}
      </Badge>

      <div className="ml-auto flex items-center gap-2">
        <Button variant="default" size="sm">
          <Play className="h-4 w-4" />
          Optimize
        </Button>
        <Button variant="graphite" size="sm" className="border border-white/15">
          <Save className="h-4 w-4" />
          Save
        </Button>
        <Button variant="graphite" size="sm" className="border border-white/15">
          <Download className="h-4 w-4" />
          Export
        </Button>
      </div>
    </header>
  );
}
