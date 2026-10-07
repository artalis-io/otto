import { useEffect, useState } from 'react';
import { Upload, CheckCircle2, XCircle, AlertTriangle, Loader2, FileUp } from 'lucide-react';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { ScrollArea } from '@/components/ui/scroll-area';
import { api, type Entity, type CanonicalField, type UploadResult, type OnboardPreview } from '@/lib/api';
import { useT } from '@/i18n';

/* M1 onboarding: upload a raw CSV, map its columns to canonical fields, and run
 * the real Nexus ingest + reconcile gate as a reviewed dry-run. Admit (creating
 * a dataset) arrives in M3. */
export function OnboardPanel() {
  const t = useT();
  const [catalog, setCatalog] = useState<Record<Entity, CanonicalField[]> | null>(null);
  const [entity, setEntity] = useState<Entity>('orders');
  const [upload, setUpload] = useState<UploadResult | null>(null);
  const [mapping, setMapping] = useState<Record<string, number>>({});
  const [preview, setPreview] = useState<OnboardPreview | null>(null);
  const [busy, setBusy] = useState<'upload' | 'preview' | null>(null);
  const [err, setErr] = useState<string | null>(null);

  useEffect(() => { api.onboardCatalog().then(setCatalog).catch((e) => setErr(String(e))); }, []);

  const fields = catalog?.[entity] ?? [];
  const missing = fields.filter((f) => f.required && mapping[f.field] == null).map((f) => f.label);

  async function onFile(file: File) {
    setErr(null); setPreview(null); setBusy('upload');
    try {
      const u = await api.onboardUpload(entity, file);
      setUpload(u); setMapping(u.suggested);
    } catch (e) { setErr(e instanceof Error ? e.message : String(e)); }
    finally { setBusy(null); }
  }
  async function validate() {
    if (!upload) return;
    setErr(null); setBusy('preview');
    try { setPreview(await api.onboardPreview(upload.uploadId, entity, mapping)); }
    catch (e) { setErr(e instanceof Error ? e.message : String(e)); }
    finally { setBusy(null); }
  }

  return (
    <ScrollArea className="max-h-[72vh]">
      <div className="space-y-4 pr-3">
        {err && <p className="rounded border border-destructive/40 bg-destructive/10 px-2 py-1 text-sm text-destructive">{err}</p>}

        {/* 1. Entity + file */}
        <div className="flex flex-wrap items-center gap-2">
          <div className="flex items-center rounded-md border border-divider p-0.5">
            {(['orders', 'vehicles', 'routes'] as const).map((e) => (
              <button key={e} type="button" onClick={() => { setEntity(e); setUpload(null); setPreview(null); setMapping({}); }}
                className={`rounded px-2 py-0.5 text-[11px] font-medium ${entity === e ? 'bg-primary text-primary-foreground' : 'text-muted-foreground hover:bg-accent'}`}>
                {t(`onboard.entity.${e}`)}
              </button>
            ))}
          </div>
          <label className="inline-flex cursor-pointer items-center gap-1.5 rounded-md border border-divider px-2.5 py-1 text-xs font-medium hover:bg-accent">
            {busy === 'upload' ? <Loader2 className="h-3.5 w-3.5 animate-spin" /> : <FileUp className="h-3.5 w-3.5" />} {t('onboard.choose')}
            <input type="file" accept=".csv,text/csv" className="hidden" onChange={(e) => { const f = e.target.files?.[0]; if (f) void onFile(f); e.target.value = ''; }} />
          </label>
          {upload && <span className="tnum text-[11px] text-muted-foreground">{t('onboard.rows', { n: upload.totalRows })}</span>}
        </div>

        {/* 2. Mapping */}
        {upload && (
          <div>
            <div className="mb-1.5 text-xs font-semibold uppercase tracking-wide text-muted-foreground">{t('onboard.map')}</div>
            <div className="overflow-hidden rounded border border-divider">
              <table className="w-full border-collapse text-[11px]">
                <thead className="bg-muted/60">
                  <tr className="text-left text-muted-foreground">
                    <th className="px-2 py-1 font-medium">{t('onboard.field')}</th>
                    <th className="px-2 py-1 font-medium">{t('onboard.column')}</th>
                    <th className="px-2 py-1 font-medium">{t('onboard.sample')}</th>
                  </tr>
                </thead>
                <tbody>
                  {fields.map((f) => {
                    const idx = mapping[f.field];
                    const sample = idx != null ? upload.rows[0]?.[idx] ?? '' : '';
                    const unmappedReq = f.required && idx == null;
                    return (
                      <tr key={f.field} className="border-t border-divider">
                        <td className="px-2 py-1">
                          <span className="font-medium">{f.label}</span>
                          {f.required && <span className={`ml-1 ${unmappedReq ? 'text-destructive' : 'text-muted-foreground'}`}>*</span>}
                          {f.hint && <div className="text-[10px] text-muted-foreground">{f.hint}</div>}
                        </td>
                        <td className="px-2 py-1">
                          <select value={idx ?? ''} onChange={(e) => {
                            const v = e.target.value; setMapping((m) => { const n = { ...m }; if (v === '') delete n[f.field]; else n[f.field] = Number(v); return n; });
                          }} className={`h-6 w-full rounded border bg-card px-1 text-[11px] ${unmappedReq ? 'border-destructive' : 'border-divider'}`}>
                            <option value="">{t('onboard.none')}</option>
                            {upload.headers.map((h, i) => <option key={i} value={i}>{i}: {h || `col ${i}`}</option>)}
                          </select>
                        </td>
                        <td className="tnum truncate px-2 py-1 text-muted-foreground" style={{ maxWidth: 220 }}>{sample}</td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
            <div className="mt-2 flex items-center gap-2">
              <Button size="sm" onClick={() => void validate()} disabled={busy != null || missing.length > 0}>
                {busy === 'preview' ? <Loader2 className="h-4 w-4 animate-spin" /> : <Upload className="h-4 w-4" />} {t('onboard.validate')}
              </Button>
              {missing.length > 0 && <span className="text-[11px] text-destructive">{t('onboard.mapRequired', { f: missing.join(', ') })}</span>}
            </div>
          </div>
        )}

        {/* 3. Preview */}
        {preview && <PreviewPanel preview={preview} />}
      </div>
    </ScrollArea>
  );
}

function PreviewPanel({ preview: p }: { preview: OnboardPreview }) {
  const t = useT();
  if (p.error) return <div className="rounded border border-destructive/40 bg-destructive/10 px-2.5 py-2 text-sm text-destructive">{p.error}</div>;
  const r = p.reconcile, s = p.semantic;
  return (
    <div className="space-y-3 rounded-md border border-divider bg-muted/20 p-3">
      {/* reconcile gate = the admit boundary */}
      <div className="flex flex-wrap items-center gap-2">
        {p.ok ? (
          <Badge variant="default" className="gap-1 bg-primary"><CheckCircle2 className="h-3.5 w-3.5" /> {t('onboard.reconcilePass')}</Badge>
        ) : (
          <Badge variant="destructive" className="gap-1"><XCircle className="h-3.5 w-3.5" /> {t('onboard.reconcileFail')}</Badge>
        )}
        <span className="tnum text-[11px] text-muted-foreground">{t('onboard.rowsReconciled', { n: r.rowsReconciled })} · {t('onboard.fieldsVerified', { n: r.fieldsVerified.length })}{r.mismatches > 0 ? ` · ${r.mismatches} mismatches` : ''}</span>
      </div>
      {r.mismatchSamples.length > 0 && (
        <div className="space-y-0.5 text-[11px] text-destructive">{r.mismatchSamples.slice(0, 6).map((m, i) => <div key={i} className="tnum">{m}</div>)}</div>
      )}

      {/* semantic advisories */}
      <div className="flex flex-wrap items-center gap-1.5 text-[11px]">
        <span className="font-medium text-muted-foreground">{t('onboard.semantic')}:</span>
        {s.errors === 0 && s.warnings === 0 && s.info === 0 && <span className="text-primary">clean</span>}
        {s.items.map((it, i) => (
          <Badge key={i} variant={it.level === 'ERROR' ? 'destructive' : it.level === 'WARN' ? 'warning' : 'outline'} className="gap-1 text-[10px]">
            {it.level !== 'INFO' && <AlertTriangle className="h-3 w-3" />}{it.kind} ({it.count})
          </Badge>
        ))}
      </div>

      {/* canonical sample */}
      {p.canonical.sample.length > 0 && (
        <div>
          <div className="mb-1 text-[11px] font-semibold uppercase tracking-wide text-muted-foreground">{t('onboard.canonical', { n: p.canonical.count })}</div>
          <div className="overflow-x-auto rounded border border-divider">
            <table className="w-full border-collapse text-[10px]">
              <thead className="bg-muted/50"><tr>{Object.keys(p.canonical.sample[0]!).slice(0, 9).map((k) => <th key={k} className="border-b border-divider px-1.5 py-1 text-left font-medium text-muted-foreground">{k}</th>)}</tr></thead>
              <tbody>
                {p.canonical.sample.slice(0, 5).map((rec, ri) => (
                  <tr key={ri} className="odd:bg-card even:bg-muted/20">
                    {Object.keys(p.canonical.sample[0]!).slice(0, 9).map((k) => <td key={k} className="tnum truncate border-b border-divider px-1.5 py-1" style={{ maxWidth: 130 }}>{String(rec[k] ?? '')}</td>)}
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </div>
      )}

      <p className="text-[10px] text-muted-foreground">{p.ok ? t('onboard.admitSoon') : t('onboard.fixToAdmit')}</p>
    </div>
  );
}
