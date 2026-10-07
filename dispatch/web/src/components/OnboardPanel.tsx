import { useCallback, useEffect, useMemo, useState } from 'react';
import { Upload, CheckCircle2, XCircle, AlertTriangle, Loader2, FileUp, MapPin, Trash2, Database } from 'lucide-react';
import Map, { Layer, Source } from 'react-map-gl/maplibre';
import type { FeatureCollection } from 'geojson';
import maplibregl from 'maplibre-gl';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { ScrollArea } from '@/components/ui/scroll-area';
import { cartaStyle } from '@/map/cartaStyle';
import { Input } from '@/components/ui/input';
import { api, pollJob, type Entity, type CanonicalField, type UploadResult, type OnboardPreview, type GeocodeResult, type GeoTier, type AdmitResult, type DatasetSummary } from '@/lib/api';
import { useT } from '@/i18n';

const TIER_COLOR: Record<GeoTier, string> = { GREEN: '#16a34a', YELLOW: '#ca8a04', APPROX: '#ea580c', RED: '#dc2626' };

/* M1 onboarding: upload a raw CSV, map its columns to canonical fields, and run
 * the real Nexus ingest + reconcile gate as a reviewed dry-run. Admit (creating
 * a dataset) arrives in M3. */
export function OnboardPanel({ onAdmitted, onDatasetsChanged }: { onAdmitted?: (firstDayId: string) => void; onDatasetsChanged?: () => void }) {
  const t = useT();
  const [catalog, setCatalog] = useState<Record<Entity, CanonicalField[]> | null>(null);
  const [datasets, setDatasets] = useState<DatasetSummary[] | null>(null);
  const refreshDatasets = useCallback(() => { void api.onboardDatasets().then(setDatasets).catch(() => setDatasets([])); }, []);
  useEffect(() => { refreshDatasets(); }, [refreshDatasets]);
  const [entity, setEntity] = useState<Entity>('orders');
  const [upload, setUpload] = useState<UploadResult | null>(null);
  const [mapping, setMapping] = useState<Record<string, number>>({});
  const [preview, setPreview] = useState<OnboardPreview | null>(null);
  const [geo, setGeo] = useState<GeocodeResult | null>(null);
  const [admit, setAdmit] = useState<AdmitResult | null>(null);
  const [admitStage, setAdmitStage] = useState<string | null>(null);
  const [label, setLabel] = useState('');
  const [busy, setBusy] = useState<'upload' | 'preview' | 'geocode' | 'admit' | null>(null);
  const [err, setErr] = useState<string | null>(null);

  useEffect(() => { api.onboardCatalog().then(setCatalog).catch((e) => setErr(String(e))); }, []);

  const fields = catalog?.[entity] ?? [];
  const missing = fields.filter((f) => f.required && mapping[f.field] == null).map((f) => f.label);

  async function onFile(file: File) {
    setErr(null); setPreview(null); setGeo(null); setBusy('upload');
    try {
      const u = await api.onboardUpload(entity, file);
      setUpload(u); setMapping(u.suggested);
    } catch (e) { setErr(e instanceof Error ? e.message : String(e)); }
    finally { setBusy(null); }
  }
  async function validate() {
    if (!upload) return;
    setErr(null); setGeo(null); setBusy('preview');
    try { setPreview(await api.onboardPreview(upload.uploadId, entity, mapping)); }
    catch (e) { setErr(e instanceof Error ? e.message : String(e)); }
    finally { setBusy(null); }
  }
  async function geocode() {
    if (!upload) return;
    setErr(null); setAdmit(null); setBusy('geocode');
    try { setGeo(await api.onboardGeocode(upload.uploadId, mapping)); }
    catch (e) { setErr(e instanceof Error ? e.message : String(e)); }
    finally { setBusy(null); }
  }
  async function doAdmit() {
    if (!upload) return;
    setErr(null); setBusy('admit'); setAdmitStage('queued');
    try {
      const { jobId } = await api.onboardAdmit(upload.uploadId, mapping, label || `Upload ${new Date().toLocaleDateString()}`);
      const final = await pollJob(jobId, (j) => setAdmitStage(j.stage ?? null));
      if (final.status !== 'completed') { setErr(final.error ?? 'admit failed'); return; }
      const r = final.result as AdmitResult;
      setAdmit(r);
      if (r.ok) { refreshDatasets(); onDatasetsChanged?.(); if (r.days?.length) onAdmitted?.(r.days[0]!.dayId); }
      else setErr(r.error ?? 'admit failed');
    } catch (e) { setErr(e instanceof Error ? e.message : String(e)); }
    finally { setBusy(null); setAdmitStage(null); }
  }
  async function deleteDataset(id: string) {
    try { await api.onboardDeleteDataset(id); refreshDatasets(); onDatasetsChanged?.(); }
    catch (e) { setErr(e instanceof Error ? e.message : String(e)); }
  }

  return (
    <ScrollArea className="max-h-[72vh]">
      <div className="space-y-4 pr-3">
        {err && <p className="rounded border border-destructive/40 bg-destructive/10 px-2 py-1 text-sm text-destructive">{err}</p>}

        {/* Admitted datasets (manage/delete) */}
        {datasets && datasets.length > 0 && (
          <div className="rounded-md border border-divider bg-muted/20 p-2.5">
            <div className="mb-1.5 flex items-center gap-1.5 text-[11px] font-semibold uppercase tracking-wide text-muted-foreground"><Database className="h-3.5 w-3.5" /> {t('onboard.datasets')}</div>
            <div className="space-y-1">
              {datasets.map((ds) => (
                <div key={ds.id} className="flex items-center gap-2 rounded border border-divider bg-card px-2 py-1 text-[11px]">
                  <span className="min-w-0 flex-1 truncate font-medium">{ds.label}</span>
                  <span className="tnum shrink-0 text-muted-foreground">{t('onboard.dsSummary', { d: ds.days.length, n: ds.totalOrders })}</span>
                  <button type="button" onClick={() => void deleteDataset(ds.id)} aria-label={t('onboard.delete')} title={t('onboard.delete')}
                    className="shrink-0 rounded p-0.5 text-muted-foreground hover:bg-destructive/10 hover:text-destructive">
                    <Trash2 className="h-3.5 w-3.5" />
                  </button>
                </div>
              ))}
            </div>
          </div>
        )}

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

        {/* 4. Geocode (orders, after the gate passes) */}
        {preview?.ok && entity === 'orders' && (
          <div className="space-y-2">
            <Button size="sm" variant="outline" onClick={() => void geocode()} disabled={busy != null}>
              {busy === 'geocode' ? <Loader2 className="h-4 w-4 animate-spin" /> : <MapPin className="h-4 w-4" />} {t('onboard.geocode')}
            </Button>
            {geo && <GeocodePanel geo={geo} />}

            {/* 5. Admit -> dataset becomes selectable days */}
            {geo?.ok && (
              <div className="space-y-2 rounded-md border border-divider bg-muted/20 p-3">
                <div className="text-[11px] font-semibold uppercase tracking-wide text-muted-foreground">{t('onboard.admitTitle')}</div>
                <div className="flex items-center gap-2">
                  <Input value={label} onChange={(e) => setLabel(e.target.value)} placeholder={t('onboard.datasetName')} className="h-8 flex-1" disabled={busy != null} />
                  <Button size="sm" onClick={() => void doAdmit()} disabled={busy != null || mapping.delivery_date == null}>
                    {busy === 'admit' ? <Loader2 className="h-4 w-4 animate-spin" /> : <Upload className="h-4 w-4" />} {t('onboard.admit')}
                  </Button>
                </div>
                {mapping.delivery_date == null && <p className="text-[11px] text-destructive">{t('onboard.needDate')}</p>}
                {busy === 'admit' ? (
                  <p className="flex items-center gap-1.5 text-[11px] text-foreground"><Loader2 className="h-3 w-3 animate-spin" /> {t('onboard.admitWorking')} · {t(`onboard.stage.${admitStage ?? 'queued'}`)}</p>
                ) : (
                  <p className="text-[10px] text-muted-foreground">{t('onboard.admitNote')}</p>
                )}
                {admit?.ok && (
                  <div className="rounded border border-primary/30 bg-primary/5 px-2 py-1.5 text-[11px] text-primary">
                    {t('onboard.admitDone', { r: admit.routable ?? 0, x: admit.excluded ?? 0, d: admit.days?.length ?? 0 })}
                  </div>
                )}
              </div>
            )}
          </div>
        )}
      </div>
    </ScrollArea>
  );
}

function GeocodePanel({ geo }: { geo: GeocodeResult }) {
  const t = useT();
  if (geo.error) return <div className="rounded border border-destructive/40 bg-destructive/10 px-2.5 py-2 text-sm text-destructive">{geo.error}</div>;
  return (
    <div className="space-y-2 rounded-md border border-divider bg-muted/20 p-3">
      <div className="flex flex-wrap items-center gap-2 text-[11px]">
        <Badge variant="outline">{t(geo.mode === 'online' ? 'onboard.modeOnline' : 'onboard.modeOffline')}</Badge>
        <span className="tnum text-muted-foreground">{t('onboard.resolved', { r: geo.resolved, n: geo.total })}{geo.unresolved > 0 ? ` · ${t('onboard.unresolved', { n: geo.unresolved })}` : ''}</span>
        <span className="ml-auto flex items-center gap-1.5">
          {(['GREEN', 'YELLOW', 'APPROX', 'RED'] as const).map((tier) => geo.byTier[tier] > 0 && (
            <span key={tier} className="inline-flex items-center gap-1 tnum"><span className="h-2.5 w-2.5 rounded-full" style={{ backgroundColor: TIER_COLOR[tier] }} />{geo.byTier[tier]}</span>
          ))}
        </span>
      </div>
      {geo.points.length > 0 && <GeoMiniMap points={geo.points} />}
    </div>
  );
}

const transformRequest = (url: string) => ({ url: url.startsWith('/') ? window.location.origin + url : url });
function GeoMiniMap({ points }: { points: GeocodeResult['points'] }) {
  const style = useMemo(() => cartaStyle(), []);
  const fc = useMemo<FeatureCollection>(() => ({
    type: 'FeatureCollection',
    features: points.map((p) => ({ type: 'Feature', properties: { color: TIER_COLOR[p.tier] }, geometry: { type: 'Point', coordinates: [p.lon, p.lat] } })),
  }), [points]);
  const bounds = useMemo(() => {
    const b = new maplibregl.LngLatBounds();
    for (const p of points) b.extend([p.lon, p.lat]);
    return b;
  }, [points]);
  return (
    <div className="h-56 overflow-hidden rounded border border-divider">
      <Map initialViewState={{ bounds, fitBoundsOptions: { padding: 24 } }} mapStyle={style} transformRequest={transformRequest}
        attributionControl={false} dragRotate={false} style={{ width: '100%', height: '100%' }}>
        <Source id="geo" type="geojson" data={fc}>
          <Layer id="geo-pts" type="circle" paint={{ 'circle-radius': 3.5, 'circle-color': ['get', 'color'], 'circle-stroke-color': '#fff', 'circle-stroke-width': 0.6, 'circle-opacity': 0.9 }} />
        </Source>
      </Map>
    </div>
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
