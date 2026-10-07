import { createContext, useCallback, useContext, useState, type ReactNode } from 'react';

/* Lightweight i18n: a language context + t() with {name} interpolation. English
 * and Hungarian. Hungarian uses the customer's own domain terms where they exist
 * (e.g. "forduló" for a vehicle trip/round, matching the dataset). Missing keys
 * fall back to English, then to the key itself. */

export type Lang = 'en' | 'hu';
type Dict = Record<string, string>;

const en: Dict = {
  'brand.sub': 'Dispatch',
  'lang.en': 'EN', 'lang.hu': 'HU', 'lang.switch': 'Language',

  'app.loading': 'Loading planning day…',
  'app.failed': 'Failed to load: {e}',

  'topbar.selectDay': 'Select planning day',
  'topbar.ordersSuffix': '{n} orders',
  'topbar.baseline': 'Baseline', 'topbar.live': 'Live',
  'topbar.optimize': 'Optimize', 'topbar.optimizing': 'Optimizing',
  'topbar.replan': 'Replan', 'topbar.replanning': 'Replanning',
  'topbar.compare': 'Compare', 'topbar.reset': 'Baseline',
  'topbar.data': 'Data', 'topbar.plans': 'Plans', 'topbar.export': 'Export',
  'scn.unavailable': '{ref} unavailable', 'scn.edited': '{day} (edited)',

  'kpi.servedTotal': 'Served / Total', 'kpi.vehiclesUsed': 'Vehicles used',
  'kpi.totalDistance': 'Total distance', 'kpi.deliveryStops': 'Delivery stops',
  'kpi.solveTime': 'Solve time', 'kpi.unassignedN': '{n} unassigned',
  'kpi.tripsN': '{n} trips', 'kpi.savedResult': 'saved result', 'kpi.vsBaseline': 'vs baseline',

  'fleet.title': 'Fleet', 'fleet.search': 'Search customer, order, vehicle',
  'fleet.trips': '{n} trips', 'fleet.tripOne': '{n} trip', 'fleet.finish': 'finish {t}',
  'fleet.noMatch': 'No vehicles match.', 'fleet.tripLabel': 'Trip {n}', 'fleet.stopsN': '{n} stops',
  'fleet.markUnavailable': 'Mark unavailable', 'fleet.staged': 'staged', 'fleet.restore': 'Restore',
  'fleet.unassigned': 'Unassigned orders', 'fleet.allAssigned': 'All orders assigned.',

  'timeline.title': 'Timeline', 'timeline.workingDay': '{ref} · working day',
  'timeline.fleetOverview': 'Fleet overview', 'timeline.vehHint': '{n} trips · finish {t}',
  'timeline.selectHint': 'select a vehicle for its day',
  'timeline.service': 'service', 'timeline.wait': 'wait', 'timeline.reload': 'reload', 'timeline.depot': 'depot',

  'inspector.title': 'Inspector',
  'inspector.empty': 'Select a vehicle, trip, stop or unassigned order to inspect.',
  'inspector.unassignedBadge': 'Unassigned',
  'inspector.unassignedReason': 'No reason provided by the solver. This order was not served in the committed plan (e.g. insufficient fleet or an infeasible time window under the current scenario).',
  'inspector.tabDetails': 'Details', 'inspector.tabLoad': 'Load',
  'inspector.trips': 'Trips', 'inspector.distance': 'Distance', 'inspector.finish': 'Finish',
  'inspector.capacity': 'Capacity', 'inspector.window': 'Window', 'inspector.load': 'Load',
  'inspector.reloadAfter': 'Reload after', 'inspector.arrival': 'Arrival', 'inspector.service': 'Service',
  'inspector.timeWindow': 'Time window', 'inspector.wait': 'Wait', 'inspector.lateBy': 'Late by',
  'inspector.weight': 'Weight', 'inspector.pallets': 'Pallets', 'inspector.serviceTime': 'Service time',
  'inspector.travelTo': 'Travel to stop', 'inspector.noViolations': 'No hard-constraint violations',
  'inspector.markUnavailable': 'Mark {ref} unavailable',
  'inspector.stopN': 'stop {n}', 'inspector.tripN': 'trip {n}', 'inspector.minN': '{n} min',
  'inspector.loadNote': 'Aggregate scalar capacity (weight & pallet count). Physical 3L loading feasibility — pallet positions, stackability, unloading access — has not been checked; no loading checker is wired.',

  'compare.title': 'Plan comparison — {label}', 'compare.comparing': 'Comparing…',
  'compare.fewerServed': '{n} fewer order(s) served with the reduced fleet{save}. Distance savings here come at the cost of service, not efficiency.',
  'compare.saveFragment': ', saving {km}',
  'compare.metric': 'Metric', 'compare.baseline': 'Baseline', 'compare.revised': 'Revised', 'compare.delta': 'Δ',
  'compare.servedOrders': 'Served orders', 'compare.unassigned': 'Unassigned', 'compare.vehiclesUsed': 'Vehicles used',
  'compare.trips': 'Trips', 'compare.totalDistance': 'Total distance',
  'compare.newlyUnassigned': 'Newly unassigned ({n})', 'compare.wasVeh': 'was {ref}',
  'compare.nowServed': 'Now served ({n})', 'compare.reassigned': 'Reassigned ({n})',
  'compare.removedVehicles': 'Vehicles no longer used ({n})',

  'import.title': 'Data provenance & import', 'import.loading': 'Loading…',
  'import.nexusAudit': 'Nexus: {a}/{b} accepted · {e} errors · {w} warnings',
  'import.shaMatch': 'Canonical built from this exact source (sha match)',
  'import.scope': '{n} in scope · day 1 {a} · day 2 {b}',
  'import.pipeline': 'Nexus pipeline', 'import.rules': 'Business rules enforced',
  'import.rawCanonical': 'Raw → canonical (sample)', 'import.excelHdr': 'Excel export (Hungarian headers)',
  'import.canonicalRecords': 'Canonical records (typed, validated{sha})', 'import.provHashed': ', provenance-hashed',
  'import.tab.orders': 'orders', 'import.tab.vehicles': 'vehicles', 'import.tab.routes': 'routes',

  'history.title': 'Saved plans', 'history.loading': 'Loading…', 'history.none': 'No saved plans yet.',
  'history.summary': '{served}/{total} served · {veh} vehicles · {km}',
  'history.open': 'Open', 'history.reopen': 'Reopen',

  'narr.title': 'AI summary', 'narr.generate': 'Generate', 'narr.regenerate': 'Regenerate',
  'narr.generating': 'Generating…', 'narr.unavailable': 'Sage narration unavailable (LLM endpoint not reachable).',
  'narr.caption': 'AI-generated by Sage from the plan figures.',

  // Solve settings / cancel / resume
  'topbar.cancel': 'Cancel', 'topbar.settings': 'Solve settings',
  'topbar.objective': 'Objective', 'topbar.budget': 'Time budget',
  'obj.vehicles': 'Fewest vehicles', 'obj.distance': 'Least distance',
  'obj.vehiclesHint': 'Serve all, then the smallest fleet, then distance.',
  'obj.distanceHint': 'Minimize total driving distance (cost-weighted).',
  'topbar.resumed': 'Resumed running solve',
  // Export menu
  'export.json': 'Plan (JSON)', 'export.csv': 'Stops (CSV)', 'export.routesheet': 'Driver route sheets',
  // Overrides
  'topbar.editsN': '{n} edits', 'topbar.changes': 'Changes',
  'edits.pending': '{n} staged change(s) — review & Replan to apply.',
  'changes.title': 'Staged changes', 'changes.none': 'No staged changes.',
  'changes.removed': 'Removed vehicles', 'changes.pinned': 'Pinned orders',
  'changes.forbidden': 'Forbidden', 'changes.constraints': 'Vehicle constraints',
  'changes.discard': 'Discard all', 'changes.applied': 'Reflected in the current plan.',
  'changes.capacity': 'cap {kg} kg / {plt} plt', 'changes.shift': 'shift {a}–{b}',
  'changes.maxTrips': 'max {n} trips', 'changes.maxDistance': 'max {n} km', 'changes.maxDuration': 'max {n} min',
  'inspector.assignment': 'Assignment',
  'inspector.pinHere': 'Pin to {ref}', 'inspector.pinned': 'Pinned to {ref}', 'inspector.unpin': 'Unpin',
  'inspector.moveTo': 'Move to…', 'inspector.forbidHere': 'Forbid {ref}',
  'inspector.forbiddenN': 'Forbidden: {refs}', 'inspector.clearOverrides': 'Clear overrides',
  'inspector.overrideHint': 'Overrides apply on the next Replan.',
  'inspector.subcontractor': 'subcontractor',
  'inspector.driveWait': 'Drive · wait', 'inspector.backToVehicle': 'Back to vehicle',
  'inspector.back': 'Back', 'inspector.forward': 'Forward',
  'inspector.constraints': 'Edit constraints', 'inspector.capKg': 'Capacity kg', 'inspector.capPlt': 'Capacity plt',
  'inspector.shiftStart': 'Shift start', 'inspector.shiftEnd': 'Shift end',
  'inspector.maxTrips': 'Max trips', 'inspector.maxDist': 'Max km',
  'inspector.constraintsHint': 'Blank = dataset default. Applies on Replan.', 'inspector.clearConstraints': 'Reset constraints',
  'inspector.assignByHand': 'Assign by hand', 'inspector.assignTo': 'Assign to vehicle…',
  // Cost
  'kpi.cost': 'Est. cost', 'kpi.demoTariff': 'demo tariff', 'kpi.extTariff': 'tariff',
  'compare.cost': 'Est. cost', 'compare.saved': '{p}% saved',
  'inspector.cost': 'Est. cost',
  // Constraint surfacing (access: tail lift, vehicle size)
  'constraint.tailLift': 'tail lift', 'constraint.maxT': 'max {n}t', 'constraint.tonnage': '{n}t',
  'constraint.tailLiftMiss': '{ref} has no tail lift', 'constraint.sizeMiss': 'Vehicle {t}t exceeds the {max}t access limit',
  'constraint.advisories': 'Access advisories', 'constraint.sizeAdvisory': '{n} oversize assignment(s)',
  'constraint.tailAdvisory': '{n} tail-lift mismatch(es)',
  'fleet.filterTailLift': 'Tail lift', 'fleet.filterAdvisories': 'Has advisories',
  // Week view
  'topbar.week': 'Week', 'week.title': 'Week overview', 'week.loading': 'Loading…',
  'week.orders': 'Orders', 'week.vehicles': 'Vehicles', 'week.trips': 'Trips', 'week.distance': 'Distance', 'week.cost': 'Est. cost',
  'week.perVehicle': 'Per-vehicle utilization', 'week.vehicle': 'Vehicle', 'week.total': 'Total', 'week.bothDays': 'both days',
  // Timeline playback
  'play.play': 'Play', 'play.pause': 'Pause', 'play.stop': 'Stop', 'play.seek': 'Seek', 'play.speed': 'Speed',
};

const hu: Dict = {
  'brand.sub': 'Diszpécser',
  'lang.en': 'EN', 'lang.hu': 'HU', 'lang.switch': 'Nyelv',

  'app.loading': 'Tervezési nap betöltése…',
  'app.failed': 'Betöltés sikertelen: {e}',

  'topbar.selectDay': 'Válassz tervezési napot',
  'topbar.ordersSuffix': '{n} megrendelés',
  'topbar.baseline': 'Alapterv', 'topbar.live': 'Élő',
  'topbar.optimize': 'Optimalizálás', 'topbar.optimizing': 'Optimalizálás',
  'topbar.replan': 'Újratervezés', 'topbar.replanning': 'Újratervezés',
  'topbar.compare': 'Összehasonlítás', 'topbar.reset': 'Alapterv',
  'topbar.data': 'Adatok', 'topbar.plans': 'Tervek', 'topbar.export': 'Exportálás',
  'scn.unavailable': '{ref} nem elérhető', 'scn.edited': '{day} (módosítva)',

  'kpi.servedTotal': 'Kiszolgált / Összes', 'kpi.vehiclesUsed': 'Használt járművek',
  'kpi.totalDistance': 'Összes távolság', 'kpi.deliveryStops': 'Kiszállítási megállók',
  'kpi.solveTime': 'Megoldási idő', 'kpi.unassignedN': '{n} kiosztatlan',
  'kpi.tripsN': '{n} forduló', 'kpi.savedResult': 'mentett eredmény', 'kpi.vsBaseline': 'az alaptervhez',

  'fleet.title': 'Flotta', 'fleet.search': 'Keresés: ügyfél, megrendelés, jármű',
  'fleet.trips': '{n} forduló', 'fleet.tripOne': '{n} forduló', 'fleet.finish': 'vége {t}',
  'fleet.noMatch': 'Nincs találat.', 'fleet.tripLabel': '{n}. forduló', 'fleet.stopsN': '{n} megálló',
  'fleet.markUnavailable': 'Kivonás', 'fleet.staged': 'előkészítve', 'fleet.restore': 'Visszaállítás',
  'fleet.unassigned': 'Kiosztatlan megrendelések', 'fleet.allAssigned': 'Minden megrendelés kiosztva.',

  'timeline.title': 'Idővonal', 'timeline.workingDay': '{ref} · munkanapja',
  'timeline.fleetOverview': 'Flotta áttekintés', 'timeline.vehHint': '{n} forduló · vége {t}',
  'timeline.selectHint': 'válassz járművet a napjához',
  'timeline.service': 'kiszolgálás', 'timeline.wait': 'várakozás', 'timeline.reload': 'újrarakodás', 'timeline.depot': 'telephely',

  'inspector.title': 'Részletek',
  'inspector.empty': 'Válassz járművet, fordulót, megállót vagy kiosztatlan megrendelést a megtekintéshez.',
  'inspector.unassignedBadge': 'Kiosztatlan',
  'inspector.unassignedReason': 'A megoldó nem adott meg okot. Ezt a megrendelést a véglegesített terv nem szolgálta ki (pl. kevés jármű vagy teljesíthetetlen időablak az adott forgatókönyvben).',
  'inspector.tabDetails': 'Részletek', 'inspector.tabLoad': 'Rakomány',
  'inspector.trips': 'Fordulók', 'inspector.distance': 'Távolság', 'inspector.finish': 'Befejezés',
  'inspector.capacity': 'Kapacitás', 'inspector.window': 'Időablak', 'inspector.load': 'Rakomány',
  'inspector.reloadAfter': 'Újrarakodás utána', 'inspector.arrival': 'Érkezés', 'inspector.service': 'Kiszolgálás',
  'inspector.timeWindow': 'Időablak', 'inspector.wait': 'Várakozás', 'inspector.lateBy': 'Késés',
  'inspector.weight': 'Súly', 'inspector.pallets': 'Raklap', 'inspector.serviceTime': 'Kiszolgálási idő',
  'inspector.travelTo': 'Utazás a megállóig', 'inspector.noViolations': 'Nincs kemény feltétel megsértve',
  'inspector.markUnavailable': '{ref} kivonása',
  'inspector.stopN': '{n}. megálló', 'inspector.tripN': '{n}. forduló', 'inspector.minN': '{n} perc',
  'inspector.loadNote': 'Csak összesített skaláris kapacitás (súly és raklapszám). A fizikai 3L rakodhatóság — raklappozíciók, torlaszolhatóság, lerakodási hozzáférés — nincs ellenőrizve; nincs rakodásellenőrző bekötve.',

  'compare.title': 'Terv-összehasonlítás — {label}', 'compare.comparing': 'Összehasonlítás…',
  'compare.fewerServed': '{n} megrendeléssel kevesebbet szolgál ki a csökkentett flotta{save}. Az itteni távolságmegtakarítás a kiszolgálás rovására megy, nem hatékonyság.',
  'compare.saveFragment': ', {km} megtakarítással',
  'compare.metric': 'Mutató', 'compare.baseline': 'Alapterv', 'compare.revised': 'Módosított', 'compare.delta': 'Δ',
  'compare.servedOrders': 'Kiszolgált megrendelések', 'compare.unassigned': 'Kiosztatlan', 'compare.vehiclesUsed': 'Használt járművek',
  'compare.trips': 'Fordulók', 'compare.totalDistance': 'Összes távolság',
  'compare.newlyUnassigned': 'Újonnan kiosztatlan ({n})', 'compare.wasVeh': 'volt: {ref}',
  'compare.nowServed': 'Most kiszolgált ({n})', 'compare.reassigned': 'Átrendezett ({n})',
  'compare.removedVehicles': 'Már nem használt járművek ({n})',

  'import.title': 'Adateredet és import', 'import.loading': 'Betöltés…',
  'import.nexusAudit': 'Nexus: {a}/{b} elfogadva · {e} hiba · {w} figyelmeztetés',
  'import.shaMatch': 'A kanonikus adat pontosan ebből a forrásból készült (sha egyezés)',
  'import.scope': '{n} a hatókörben · 1. nap {a} · 2. nap {b}',
  'import.pipeline': 'Nexus folyamat', 'import.rules': 'Érvényesített üzleti szabályok',
  'import.rawCanonical': 'Nyers → kanonikus (minta)', 'import.excelHdr': 'Excel export (magyar fejlécek)',
  'import.canonicalRecords': 'Kanonikus rekordok (típusos, validált{sha})', 'import.provHashed': ', eredet-hash-elt',
  'import.tab.orders': 'megrendelések', 'import.tab.vehicles': 'járművek', 'import.tab.routes': 'útvonalak',

  'history.title': 'Mentett tervek', 'history.loading': 'Betöltés…', 'history.none': 'Még nincs mentett terv.',
  'history.summary': '{served}/{total} kiszolgálva · {veh} jármű · {km}',
  'history.open': 'Megnyitva', 'history.reopen': 'Újranyitás',

  'narr.title': 'MI összefoglaló', 'narr.generate': 'Generálás', 'narr.regenerate': 'Újragenerálás',
  'narr.generating': 'Generálás…', 'narr.unavailable': 'A Sage narráció nem elérhető (az LLM végpont nem érhető el).',
  'narr.caption': 'A Sage MI által a terv adataiból generálva.',

  // Solve settings / cancel / resume
  'topbar.cancel': 'Mégse', 'topbar.settings': 'Megoldási beállítások',
  'topbar.objective': 'Célfüggvény', 'topbar.budget': 'Időkeret',
  'obj.vehicles': 'Legkevesebb jármű', 'obj.distance': 'Legrövidebb út',
  'obj.vehiclesHint': 'Mindet kiszolgálni, majd a legkisebb flotta, majd távolság.',
  'obj.distanceHint': 'A teljes megtett távolság minimalizálása (költségsúlyozott).',
  'topbar.resumed': 'Futó megoldás folytatva',
  // Export menu
  'export.json': 'Terv (JSON)', 'export.csv': 'Megállók (CSV)', 'export.routesheet': 'Sofőr útvonallapok',
  // Overrides
  'topbar.editsN': '{n} módosítás', 'topbar.changes': 'Módosítások',
  'edits.pending': '{n} előkészített módosítás — nézd át és Újratervezés az érvényesítéshez.',
  'changes.title': 'Előkészített módosítások', 'changes.none': 'Nincs előkészített módosítás.',
  'changes.removed': 'Kivont járművek', 'changes.pinned': 'Rögzített megrendelések',
  'changes.forbidden': 'Tiltva', 'changes.constraints': 'Jármű-korlátok',
  'changes.discard': 'Összes elvetése', 'changes.applied': 'A jelenlegi tervben érvényesítve.',
  'changes.capacity': 'kap. {kg} kg / {plt} rlp', 'changes.shift': 'műszak {a}–{b}',
  'changes.maxTrips': 'max {n} forduló', 'changes.maxDistance': 'max {n} km', 'changes.maxDuration': 'max {n} perc',
  'inspector.assignment': 'Hozzárendelés',
  'inspector.pinHere': 'Rögzítés: {ref}', 'inspector.pinned': 'Rögzítve: {ref}', 'inspector.unpin': 'Feloldás',
  'inspector.moveTo': 'Áthelyezés…', 'inspector.forbidHere': '{ref} tiltása',
  'inspector.forbiddenN': 'Tiltva: {refs}', 'inspector.clearOverrides': 'Módosítások törlése',
  'inspector.overrideHint': 'A módosítások a következő újratervezéskor lépnek életbe.',
  'inspector.subcontractor': 'alvállalkozó',
  'inspector.driveWait': 'Vezetés · várakozás', 'inspector.backToVehicle': 'Vissza a járműhöz',
  'inspector.back': 'Vissza', 'inspector.forward': 'Előre',
  'inspector.constraints': 'Korlátok szerkesztése', 'inspector.capKg': 'Kapacitás kg', 'inspector.capPlt': 'Kapacitás rlp',
  'inspector.shiftStart': 'Műszak kezdete', 'inspector.shiftEnd': 'Műszak vége',
  'inspector.maxTrips': 'Max forduló', 'inspector.maxDist': 'Max km',
  'inspector.constraintsHint': 'Üres = adathalmaz alapértelmezés. Újratervezéskor lép életbe.', 'inspector.clearConstraints': 'Korlátok visszaállítása',
  'inspector.assignByHand': 'Kézi hozzárendelés', 'inspector.assignTo': 'Hozzárendelés járműhöz…',
  // Cost
  'kpi.cost': 'Becsült költség', 'kpi.demoTariff': 'teszt tarifa', 'kpi.extTariff': 'tarifa',
  'compare.cost': 'Becsült költség', 'compare.saved': '{p}% megtakarítás',
  'inspector.cost': 'Becsült költség',
  // Constraint surfacing
  'constraint.tailLift': 'emelőhátfal', 'constraint.maxT': 'max {n}t', 'constraint.tonnage': '{n}t',
  'constraint.tailLiftMiss': '{ref} nincs emelőhátfal', 'constraint.sizeMiss': 'A {t}t jármű meghaladja a {max}t behajtási korlátot',
  'constraint.advisories': 'Behajtási figyelmeztetések', 'constraint.sizeAdvisory': '{n} túlméretes hozzárendelés',
  'constraint.tailAdvisory': '{n} emelőhátfal-eltérés',
  'fleet.filterTailLift': 'Emelőhátfal', 'fleet.filterAdvisories': 'Figyelmeztetéssel',
  // Week view
  'topbar.week': 'Hét', 'week.title': 'Heti áttekintés', 'week.loading': 'Betöltés…',
  'week.orders': 'Megrendelés', 'week.vehicles': 'Járművek', 'week.trips': 'Fordulók', 'week.distance': 'Távolság', 'week.cost': 'Becsült költség',
  'week.perVehicle': 'Járművenkénti kihasználtság', 'week.vehicle': 'Jármű', 'week.total': 'Összes', 'week.bothDays': 'mindkét nap',
  // Timeline playback
  'play.play': 'Lejátszás', 'play.pause': 'Szünet', 'play.stop': 'Leállítás', 'play.seek': 'Keresés', 'play.speed': 'Sebesség',
};

const DICTS: Record<Lang, Dict> = { en, hu };

type TFn = (key: string, params?: Record<string, string | number>) => string;
const Ctx = createContext<{ lang: Lang; setLang: (l: Lang) => void; t: TFn }>({
  lang: 'en', setLang: () => {}, t: (k) => k,
});

export function LangProvider({ children }: { children: ReactNode }) {
  const [lang, setLangState] = useState<Lang>(() => {
    try { const s = localStorage.getItem('otto.lang'); if (s === 'hu' || s === 'en') return s; } catch { /* ignore */ }
    return 'en';
  });
  const setLang = useCallback((l: Lang) => {
    try { localStorage.setItem('otto.lang', l); } catch { /* ignore */ }
    document.documentElement.lang = l;
    setLangState(l);
  }, []);
  const t = useCallback<TFn>((key, params) => {
    let s = DICTS[lang][key] ?? DICTS.en[key] ?? key;
    if (params) for (const [k, v] of Object.entries(params)) s = s.replace(new RegExp(`\\{${k}\\}`, 'g'), String(v));
    return s;
  }, [lang]);
  return <Ctx.Provider value={{ lang, setLang, t }}>{children}</Ctx.Provider>;
}

export const useI18n = () => useContext(Ctx);
export const useT = (): TFn => useContext(Ctx).t;
