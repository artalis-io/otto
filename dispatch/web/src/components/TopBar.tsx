import { useState } from 'react';
import { Play, RotateCcw, Download, Loader2, Repeat, GitCompareArrows, Database, History, Settings2, X, ChevronDown, FileText, FileSpreadsheet, FileJson, SlidersHorizontal, CalendarRange } from 'lucide-react';
import { Button } from '@/components/ui/button';
import { Badge } from '@/components/ui/badge';
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select';
import { api } from '@/lib/api';
import { useI18n, useT } from '@/i18n';
import type { DaySummary, Objective, Plan, Scenario } from '@/types';
import type { JobView } from '@/App';

/* Localize the planning-day label from its ISO date (e.g. "kedd, 2026. május 6."
 * in Hungarian) rather than using the backend's fixed English label. */
function fmtDay(iso: string, lang: string): string {
  const d = new Date(`${iso}T00:00:00`);
  if (Number.isNaN(d.getTime())) return iso;
  return d.toLocaleDateString(lang === 'hu' ? 'hu-HU' : 'en-GB', { weekday: 'long', year: 'numeric', month: 'long', day: 'numeric' });
}

/* A tiny popover: a trigger plus an absolutely-positioned panel with a backdrop
 * that closes on outside click. Avoids pulling in a dropdown primitive. */
function Popover({ open, onClose, align = 'right', children }: { open: boolean; onClose: () => void; align?: 'left' | 'right'; children: React.ReactNode }) {
  if (!open) return null;
  return (
    <>
      <div className="fixed inset-0 z-40" onClick={onClose} />
      <div className={`absolute top-full z-50 mt-1.5 ${align === 'right' ? 'right-0' : 'left-0'} min-w-[220px] rounded-md border border-divider bg-card p-2 text-foreground shadow-lg`}>
        {children}
      </div>
    </>
  );
}

export function TopBar({
  days, dayId, onDayChange, scenario, plan, job, solving,
  objective, onObjectiveChange, budgetSec, onBudgetChange, fullBudget, onFullBudgetChange, changesCount, dirty,
  onOptimize, onCancel, onReset, onOpenChanges, showCompare, onCompare, onOpenImport, onOpenHistory, onOpenWeek,
}: {
  days: DaySummary[]; dayId: string; onDayChange: (id: string) => void;
  scenario: Scenario; plan: Plan; job: JobView | null; solving: boolean;
  objective: Objective; onObjectiveChange: (o: Objective) => void;
  budgetSec: number; onBudgetChange: (n: number) => void; fullBudget: boolean; onFullBudgetChange: (b: boolean) => void; changesCount: number; dirty: boolean;
  onOptimize: () => void; onCancel: () => void; onReset: () => void; onOpenChanges: () => void;
  showCompare: boolean; onCompare: () => void; onOpenImport: () => void; onOpenHistory: () => void; onOpenWeek: () => void;
}) {
  const { lang, setLang } = useI18n();
  const t = useT();
  const [settingsOpen, setSettingsOpen] = useState(false);
  const [exportOpen, setExportOpen] = useState(false);
  const isReplan = scenario.kind === 'copy';
  const termOk = plan.provenance.termination === 'OK';

  function exportPlanJson() {
    const blob = new Blob([JSON.stringify(plan, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url; a.download = `otto-plan-${plan.day}-${plan.id}.json`; a.click();
    URL.revokeObjectURL(url);
  }
  function downloadCsv() {
    const a = document.createElement('a');
    a.href = api.exportCsvUrl(plan.id); a.download = ''; a.click();
  }
  function openRouteSheets() { window.open(api.routeSheetUrl(plan.id), '_blank', 'noopener'); }

  return (
    <header className="flex h-14 shrink-0 items-center gap-4 bg-graphite px-4 text-graphite-foreground">
      <div className="flex items-baseline gap-2">
        <span className="text-lg font-bold tracking-tight">OTTO</span>
        <span className="text-sm font-medium text-graphite-muted">{t('brand.sub')}</span>
      </div>

      <div className="mx-1 h-6 w-px bg-white/10" />

      <div className="flex items-center rounded-md border border-white/15 bg-white/5 p-0.5" role="group" aria-label={t('lang.switch')}>
        {(['en', 'hu'] as const).map((l) => (
          <button key={l} type="button" onClick={() => setLang(l)}
            aria-pressed={lang === l}
            className={`rounded px-2 py-0.5 text-[11px] font-medium transition-colors ${lang === l ? 'bg-white/15 text-graphite-foreground' : 'text-graphite-muted hover:text-graphite-foreground'}`}>
            {t(l === 'en' ? 'lang.en' : 'lang.hu')}
          </button>
        ))}
      </div>

      <div className="w-64">
        <Select value={dayId} onValueChange={onDayChange} disabled={solving}>
          <SelectTrigger className="h-8 border-white/15 bg-white/5 text-graphite-foreground">
            <SelectValue placeholder={t('topbar.selectDay')} />
          </SelectTrigger>
          <SelectContent>
            {days.map((d) => (
              <SelectItem key={d.id} value={d.id}>
                {fmtDay(d.isoDate, lang)} · {t('topbar.ordersSuffix', { n: d.orders ?? '—' })}
              </SelectItem>
            ))}
          </SelectContent>
        </Select>
      </div>

      {isReplan ? (
        <Badge variant="warning" title="Editable scenario (baseline preserved)">
          {scenario.label}{changesCount > 0 ? ` · ${t('topbar.editsN', { n: changesCount })}` : ''}
        </Badge>
      ) : (
        <Badge variant={plan.source === 'saved' ? 'outline' : 'default'} title="Plan source / Surge termination" className="border-white/20 text-graphite-foreground">
          {plan.source === 'saved' ? t('topbar.baseline') : t('topbar.live')} · {termOk ? 'OK' : plan.provenance.termination}
        </Badge>
      )}

      <div className="ml-auto flex items-center gap-2">
        <Button variant="graphite" size="sm" className="border border-white/15" onClick={onOpenWeek} title={t('week.title')}>
          <CalendarRange className="h-4 w-4" /> {t('topbar.week')}
        </Button>
        <Button variant="graphite" size="sm" className="border border-white/15" onClick={onOpenHistory} title={t('history.title')}>
          <History className="h-4 w-4" /> {t('topbar.plans')}
        </Button>
        <Button variant="graphite" size="sm" className="border border-white/15" onClick={onOpenImport} title={t('import.title')}>
          <Database className="h-4 w-4" /> {t('topbar.data')}
        </Button>
        {changesCount > 0 && (
          <Button variant="graphite" size="sm" className={`border ${dirty ? 'border-warning/60 text-warning' : 'border-white/15'}`} onClick={onOpenChanges}>
            <SlidersHorizontal className="h-4 w-4" /> {t('topbar.changes')} <span className="tnum rounded bg-white/15 px-1 text-[10px]">{changesCount}</span>
          </Button>
        )}
        {showCompare && (
          <Button variant="graphite" size="sm" className="border border-white/15" onClick={onCompare} disabled={solving}>
            <GitCompareArrows className="h-4 w-4" /> {t('topbar.compare')}
          </Button>
        )}
        {isReplan && (
          <Button variant="graphite" size="sm" className="border border-white/15" onClick={onReset} disabled={solving}>
            <RotateCcw className="h-4 w-4" /> {t('topbar.reset')}
          </Button>
        )}

        {/* Solve settings (objective + time budget) */}
        <div className="relative">
          <Button variant="graphite" size="icon" className="h-8 w-8 border border-white/15" title={t('topbar.settings')} aria-label={t('topbar.settings')}
            onClick={() => setSettingsOpen((o) => !o)} disabled={solving}>
            <Settings2 className="h-4 w-4" />
          </Button>
          <Popover open={settingsOpen} onClose={() => setSettingsOpen(false)}>
            <div className="space-y-3">
              <div>
                <div className="mb-1 text-[11px] font-semibold uppercase tracking-wide text-muted-foreground">{t('topbar.objective')}</div>
                <div className="space-y-1">
                  {(['vehicles', 'distance'] as const).map((o) => (
                    <button key={o} type="button" onClick={() => onObjectiveChange(o)}
                      className={`w-full rounded border px-2 py-1.5 text-left text-xs transition-colors ${objective === o ? 'border-primary bg-primary/10' : 'border-divider hover:bg-accent'}`}>
                      <div className="font-medium">{t(o === 'vehicles' ? 'obj.vehicles' : 'obj.distance')}</div>
                      <div className="text-[10px] text-muted-foreground">{t(o === 'vehicles' ? 'obj.vehiclesHint' : 'obj.distanceHint')}</div>
                    </button>
                  ))}
                </div>
              </div>
              <div>
                <div className="mb-1 flex items-baseline justify-between text-[11px] font-semibold uppercase tracking-wide text-muted-foreground">
                  <span>{t('topbar.budget')}</span><span className="tnum text-foreground">{budgetSec}s</span>
                </div>
                <input type="range" min={15} max={240} step={5} value={budgetSec}
                  onChange={(e) => onBudgetChange(Number(e.target.value))} className="w-full accent-[var(--primary)]" />
              </div>
              <label className="flex cursor-pointer items-start gap-2 text-xs">
                <input type="checkbox" checked={fullBudget} onChange={(e) => onFullBudgetChange(e.target.checked)} className="mt-0.5 accent-[var(--primary)]" />
                <span>
                  <span className="font-medium">{t('topbar.fullBudget')}</span>
                  <span className="block text-[10px] text-muted-foreground">{t('topbar.fullBudgetHint')}</span>
                </span>
              </label>
            </div>
          </Popover>
        </div>

        {solving ? (
          <>
            <Button variant="default" size="sm" disabled className="min-w-[128px]">
              <Loader2 className="h-4 w-4 animate-spin" /> {isReplan ? t('topbar.replanning') : t('topbar.optimizing')} {job ? `${job.elapsedSec.toFixed(0)}s` : ''}
            </Button>
            <Button variant="graphite" size="sm" className="border border-white/15" onClick={onCancel} title={t('topbar.cancel')}>
              <X className="h-4 w-4" /> {t('topbar.cancel')}
            </Button>
          </>
        ) : (
          <Button variant="default" size="sm" onClick={onOptimize} className={`min-w-[128px] ${dirty ? 'ring-2 ring-warning/60' : ''}`}>
            {isReplan ? <Repeat className="h-4 w-4" /> : <Play className="h-4 w-4" />} {isReplan ? t('topbar.replan') : t('topbar.optimize')}
          </Button>
        )}

        {/* Export menu */}
        <div className="relative">
          <Button variant="graphite" size="sm" className="border border-white/15" onClick={() => setExportOpen((o) => !o)} title={t('topbar.export')}>
            <Download className="h-4 w-4" /> {t('topbar.export')} <ChevronDown className="h-3 w-3 opacity-70" />
          </Button>
          <Popover open={exportOpen} onClose={() => setExportOpen(false)}>
            <div className="space-y-0.5">
              <MenuItem icon={<FileText className="h-4 w-4" />} label={t('export.routesheet')} onClick={() => { setExportOpen(false); openRouteSheets(); }} />
              <MenuItem icon={<FileSpreadsheet className="h-4 w-4" />} label={t('export.csv')} onClick={() => { setExportOpen(false); downloadCsv(); }} />
              <MenuItem icon={<FileJson className="h-4 w-4" />} label={t('export.json')} onClick={() => { setExportOpen(false); exportPlanJson(); }} />
            </div>
          </Popover>
        </div>
      </div>
    </header>
  );
}

function MenuItem({ icon, label, onClick }: { icon: React.ReactNode; label: string; onClick: () => void }) {
  return (
    <button type="button" onClick={onClick} className="flex w-full items-center gap-2 rounded px-2 py-1.5 text-left text-xs hover:bg-accent">
      <span className="text-muted-foreground">{icon}</span> {label}
    </button>
  );
}
