import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { mkdirSync, writeFileSync, readFileSync, rmSync, existsSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { randomUUID } from 'node:crypto';
import { config } from '../config.js';
import { CATALOG, type Entity, type FieldType } from './catalog.js';

const pExecFile = promisify(execFile);
const here = dirname(fileURLToPath(import.meta.url));            // dispatch/server/src/data
const SAMPLE_PY = resolve(here, '../../scripts/sample_table.py');
const INGEST_SH = resolve(config.nexusDir, 'scripts/ingest.sh');

function tmpDir(): string { const d = resolve(config.uploadsDir, 'tmp'); mkdirSync(d, { recursive: true }); return d; }

/** A server-side path for an upload (generated name; the client filename is never trusted). */
export function newUploadPath(entity: Entity): { uploadId: string; path: string } {
  const uploadId = `up_${randomUUID().slice(0, 12)}`;
  return { uploadId, path: resolve(tmpDir(), `${uploadId}_${entity}.csv`) };
}
export function uploadPath(uploadId: string, entity: Entity): string {
  return resolve(tmpDir(), `${uploadId}_${entity}.csv`);
}

export interface Sample { headers: string[]; rows: string[][]; totalRows: number; delimiter: string }

/** Headers + sample rows + row count, parsed in Python (not in Node). */
export async function sampleUpload(path: string, limit = 8): Promise<Sample> {
  if (!existsSync(path)) throw new Error('upload not found');
  const { stdout } = await pExecFile('python3', [SAMPLE_PY, path, String(limit)], { timeout: 15_000, maxBuffer: 8 * 1024 * 1024 });
  const o = JSON.parse(stdout) as Sample & { error?: string };
  if (o.error) throw new Error(`sample failed: ${o.error}`);
  return o;
}

export type Mapping = Record<string, number>; // canonical field -> raw column index

/** Generate an nx_schema v2 from a column mapping (direct column->field). */
export function buildSchema(entity: Entity, mapping: Mapping): object {
  const fields = CATALOG[entity];
  const byField = new Map(fields.map((f) => [f.field, f]));
  const columns: object[] = [];
  for (const [field, idx] of Object.entries(mapping)) {
    const f = byField.get(field);
    if (!f || typeof idx !== 'number' || idx < 0) continue;
    columns.push(columnSpec(idx, field, f.type, f.required));
  }
  return {
    nx_schema: 2, version: `onboard-${entity}`, output_type: 'record',
    table_selector: { index: 0 }, skip_rows: 0, columns,
  };
}
function columnSpec(source: number, target: string, type: FieldType, required: boolean): object {
  const base: Record<string, unknown> = { source, target, type };
  if (type === 'string') { base.transforms = ['trim']; if (!required) base.default = ''; }
  else if (type === 'double') { base.precision = 10; base.validate = { min: 0 }; }
  else if (type === 'int') { base.validate = { min: 0 }; }
  else if (type === 'bool') { base.default = 'false'; }
  if (required) base.required = true;
  return base;
}

/** Which required fields are unmapped (empty if the mapping is complete). */
export function missingRequired(entity: Entity, mapping: Mapping): string[] {
  return CATALOG[entity].filter((f) => f.required && !(f.field in mapping)).map((f) => f.field);
}

export interface Preview {
  ok: boolean;                 // reconcile passed (admit-able)
  reconcile: { provenanceOk: boolean; rowsReconciled: number; fieldsVerified: string[]; mismatches: number; mismatchSamples: string[] };
  semantic: { errors: number; warnings: number; info: number; items: { level: string; kind: string; count: number }[] };
  canonical: { count: number; sample: Record<string, unknown>[] };
  error?: string;
}

/** Run the real ingest (nx_pipeline + reconcile gate + semantic checks) on an
 * uploaded file with a generated schema, and summarize the result for review. */
export async function runIngestPreview(path: string, entity: Entity, mapping: Mapping): Promise<Preview> {
  const miss = missingRequired(entity, mapping);
  if (miss.length) return emptyPreview(`map required fields: ${miss.join(', ')}`);
  if (!existsSync(INGEST_SH)) return emptyPreview(`nexus ingest not found at ${INGEST_SH} (build: make -C nexus tools)`);

  const schemaPath = `${path}.schema.json`;
  const outPath = `${path}.canonical.json`;
  writeFileSync(schemaPath, JSON.stringify(buildSchema(entity, mapping)));

  let stdout = '', exitCode = 0;
  try {
    const r = await pExecFile('sh', [INGEST_SH, path, schemaPath, outPath], { timeout: config.ingestTimeoutSec * 1000, maxBuffer: 32 * 1024 * 1024 });
    stdout = r.stdout;
  } catch (e) {
    const err = e as { stdout?: string; stderr?: string; code?: number; killed?: boolean };
    stdout = err.stdout ?? '';
    exitCode = typeof err.code === 'number' ? err.code : 2;
    if (err.killed) { cleanup(schemaPath, outPath); return emptyPreview(`ingest timed out after ${config.ingestTimeoutSec}s`); }
    if (exitCode === 2) { cleanup(schemaPath, outPath); return emptyPreview((err.stderr || 'ingest setup error').trim().slice(0, 300)); }
    // exit 1 = reconcile mismatch: fall through and report it (stdout has the details)
  }

  const reconcile = parseReconcile(stdout);
  const semantic = parseSemantic(stdout);
  let canonical = { count: 0, sample: [] as Record<string, unknown>[] };
  try {
    const raw = JSON.parse(readFileSync(outPath, 'utf8')) as unknown;
    const recs = (Array.isArray(raw) ? raw : (raw as { records?: Record<string, unknown>[] }).records) ?? [];
    canonical = { count: recs.length, sample: recs.slice(0, 8) };
  } catch { /* no output on gate failure */ }

  cleanup(schemaPath, outPath);
  return { ok: exitCode === 0 && reconcile.provenanceOk && reconcile.mismatches === 0, reconcile, semantic, canonical };
}

function cleanup(...paths: string[]): void { for (const p of paths) { try { rmSync(p, { force: true }); } catch { /* ignore */ } } }
function emptyPreview(error: string): Preview {
  return { ok: false, reconcile: { provenanceOk: false, rowsReconciled: 0, fieldsVerified: [], mismatches: 0, mismatchSamples: [] }, semantic: { errors: 0, warnings: 0, info: 0, items: [] }, canonical: { count: 0, sample: [] }, error };
}

export function parseReconcile(stdout: string): Preview['reconcile'] {
  const provenanceOk = /provenance sha256\s*:\s*OK/.test(stdout);
  const rowsReconciled = Number(/rows reconciled\s*:\s*(\d+)/.exec(stdout)?.[1] ?? 0);
  const mismatches = Number(/mismatches\s*:\s*(\d+)/.exec(stdout)?.[1] ?? 0);
  const fieldsStr = /fields verified\s*:\s*\d+\s*\[([^\]]*)\]/.exec(stdout)?.[1] ?? '';
  const fieldsVerified = fieldsStr.split(',').map((s) => s.trim().replace(/^'|'$/g, '')).filter(Boolean);
  const mismatchSamples = (stdout.match(/^\s*MISMATCH .*/gm) ?? []).slice(0, 20).map((s) => s.trim());
  return { provenanceOk, rowsReconciled, fieldsVerified, mismatches, mismatchSamples };
}

export function parseSemantic(stdout: string): Preview['semantic'] {
  const items: { level: string; kind: string; count: number }[] = [];
  let errors = 0, warnings = 0, info = 0;
  for (const m of stdout.matchAll(/\[(ERROR|WARN|INFO)\]\s+([\w-]+)\s+\((\d+)\)/g)) {
    const level = m[1]!, kind = m[2]!, count = Number(m[3]!);
    items.push({ level, kind, count });
    if (level === 'ERROR') errors += count; else if (level === 'WARN') warnings += count; else info += count;
  }
  return { errors, warnings, info, items };
}
