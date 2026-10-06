import { Play, RotateCcw, Download, Loader2, Repeat, GitCompareArrows, Database, History } from 'lucide-react';
import { Button } from '@/components/ui/button';
import { Badge } from '@/components/ui/badge';
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select';
import { useI18n, useT } from '@/i18n';
import type { DaySummary, Plan, Scenario } from '@/types';
import type { JobView } from '@/App';

/* Localize the planning-day label from its ISO date (e.g. "kedd, 2026. május 6."
 * in Hungarian) rather than using the backend's fixed English label. */
function fmtDay(iso: string, lang: string): string {
  const d = new Date(`${iso}T00:00:00`);
  if (Number.isNaN(d.getTime())) return iso;
  return d.toLocaleDateString(lang === 'hu' ? 'hu-HU' : 'en-GB', { weekday: 'long', year: 'numeric', month: 'long', day: 'numeric' });
}

/* Graphite top bar: wordmark, planning-day selector, plan-status chip, and the
 * primary Optimize/Replan action (with genuine elapsed while solving). */
export function TopBar({
  days, dayId, onDayChange, scenario, plan, job, solving, onOptimize, onReset, showCompare, onCompare, onOpenImport, onOpenHistory,
}: {
  days: DaySummary[]; dayId: string; onDayChange: (id: string) => void;
  scenario: Scenario; plan: Plan; job: JobView | null; solving: boolean;
  onOptimize: () => void; onReset: () => void; showCompare: boolean; onCompare: () => void; onOpenImport: () => void; onOpenHistory: () => void;
}) {
  const { lang, setLang } = useI18n();
  const t = useT();
  const isReplan = scenario.kind === 'copy';
  const termOk = plan.provenance.termination === 'OK';

  function exportPlan() {
    const blob = new Blob([JSON.stringify(plan, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url; a.download = `otto-plan-${plan.day}-${plan.id}.json`; a.click();
    URL.revokeObjectURL(url);
  }

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
        <Badge variant="warning" title="Editable scenario (baseline preserved)">{scenario.label}</Badge>
      ) : (
        <Badge variant={plan.source === 'saved' ? 'outline' : 'default'} title="Plan source / Surge termination" className="border-white/20 text-graphite-foreground">
          {plan.source === 'saved' ? t('topbar.baseline') : t('topbar.live')} · {termOk ? 'OK' : plan.provenance.termination}
        </Badge>
      )}

      <div className="ml-auto flex items-center gap-2">
        <Button variant="graphite" size="sm" className="border border-white/15" onClick={onOpenHistory} title={t('history.title')}>
          <History className="h-4 w-4" /> {t('topbar.plans')}
        </Button>
        <Button variant="graphite" size="sm" className="border border-white/15" onClick={onOpenImport} title={t('import.title')}>
          <Database className="h-4 w-4" /> {t('topbar.data')}
        </Button>
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
        <Button variant="default" size="sm" onClick={onOptimize} disabled={solving} className="min-w-[128px]">
          {solving ? (
            <><Loader2 className="h-4 w-4 animate-spin" /> {isReplan ? t('topbar.replanning') : t('topbar.optimizing')} {job ? `${job.elapsedSec.toFixed(0)}s` : ''}</>
          ) : (
            <>{isReplan ? <Repeat className="h-4 w-4" /> : <Play className="h-4 w-4" />} {isReplan ? t('topbar.replan') : t('topbar.optimize')}</>
          )}
        </Button>
        <Button variant="graphite" size="sm" className="border border-white/15" onClick={exportPlan} title={t('topbar.export')}>
          <Download className="h-4 w-4" /> {t('topbar.export')}
        </Button>
      </div>
    </header>
  );
}
