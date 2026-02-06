---
name: site-update
description: Update and validate the OTTO landing page. Ensures test counts, modules, personas, and HTML best practices are correct.
user-invocable: true
---

# Site Update Skill

Validates and updates the OTTO landing page (`site/index.html`) to ensure accuracy and quality.

**Target:** $ARGUMENTS (or full audit if empty)

## Usage

```
/site-update              # Full audit and report
/site-update --fix        # Audit and apply fixes
/site-update tests        # Update test counts only
/site-update modules      # Verify all modules listed
/site-update personas     # Verify persona sections
```

## Audit Categories

### 1. Test Count Accuracy (Critical)

Test counts must match actual codebase. Run tests and update:

**How to get current counts:**
```bash
# Run all tests and count
make test 2>&1 | grep -E "passed|tests"

# Or check MEMORY.md for documented counts
cat ~/.claude/projects/-Users-mark-Desktop-work-artalis-io-otto/memory/MEMORY.md
```

**Expected counts (as of Feb 2026):**
| Module | Tests |
|--------|-------|
| shared | 206 |
| ralph (base) | 73 |
| ralph LAP | 358 |
| ralph detect | 194 |
| fuelwise | 33 |
| velo | 51 |
| carta | 110 |
| locus | 52 |
| **Total** | **1077+** |

**Update locations in `site/index.html`:**
- Hero badge: `<span class="badge green">1000+ tests</span>`
- Developer section: "1000+ tests" references
- Card metadata: individual module test counts

### 2. Module Completeness (Critical)

ALL modules must be listed - both implemented and planned.

**Implemented modules (must have cards in #developers):**
| Module | Description | Test Count |
|--------|-------------|------------|
| Ralph | LP/MIP solver | 600+ tests (combined) |
| Velo | OSM routing | 51 tests |
| Carta | Map tiles (MVT/PNG) | 110 tests |
| Locus | Geocoding | 52 tests |
| FuelWise | Refuel optimization | 33 tests |
| Shared | Geo/proto/rate limiting | 206 tests |
| ClayShards | Immediate-mode UI | (in clayshards/) |

**Planned modules (must have cards with `planned` badge):**
| Module | Full Name | Description |
|--------|-----------|-------------|
| HoSE | Hours of Service Engine | FMCSA/EU HoS rules |
| Tempo | Time-window Event Management Policy Orchestrator | Time windows, facility hours |
| Arbor | Algorithmic Recursive Branching and Optimization Runtime | State-space search |
| Sigma | Selection and Integration for Global Multi-assignment Allocation | Fleet MIP |
| Pulse | Plan Utilization and Live State Estimator | PTA, execution tracking |
| Fuse | Fleet Unified Signal and Estimation | Signal fusion, geofencing |
| Forge | Flexible Orchestration and Runtime for General Execution | Async job queue |
| Apex | Asynchronous Pre-computation Execution | Tile pyramid, cache warming |
| Nexus | Normalized External Unified Snapshots | TMS/ELD integration |
| Iris | Intelligent Request Interpretation System | NL/LLM interface |
| Quota | Quote Underwriting and Tariff Optimization Algorithm | Pricing/rates |
| Atlas | Allocation and Tactical Lane Analysis System | Network design |

**Architecture diagram must include:**
- UI layer
- Fleet layer (HoSE, Tempo, Arbor, Sigma, Pulse) - marked planned
- State layer (Fuse) - marked planned
- Network layer (Atlas, Quota) - marked planned
- Domain layer (FuelWise, Velo, Carta, Locus)
- Core layer (Ralph, Shared)
- Vendor layer (miniz, mongoose, Clay)

### 3. Persona Sections (Critical)

All personas must exist and be navigable with zero JavaScript.

**Required personas:**
| ID | Title | Target Audience | Must Include |
|----|-------|-----------------|--------------|
| `#developers` | For Developers & Contributors | OSS contributors, engineers | Quick start, architecture, Why C, benchmarks, GitHub CTA |
| `#executives` | For Executives | CEO, CTO, CFO | ROI stats, no lock-in, license model, TCO, EV/AI ready |
| `#operations` | For Dispatchers & Planners | Dispatchers, planners, owner-operators | Daily tools, trusted reality, geofencing, ETAs, demo CTA |
| `#partners` | For Partners | Fuel cards, OEMs, TMS, brokers | Integration opportunities, partner exchange |

**Navigation requirements:**
- Header nav links: `<a href="#developers">`, `<a href="#executives">`, `<a href="#operations">`
- Persona selector cards with anchor links
- CSS `scroll-behavior: smooth` (no JS)
- Deep links must work: `ottofleet.io/#executives`

**Executive section must include ROI metrics:**
- Fleet utilization target (+25%)
- Fuel cost reduction (-15%)
- HoS compliance (100%)
- Optimization latency (<100ms)

### 4. HTML Best Practices (High)

**Performance:**
- [ ] Single file (no external CSS/JS except email obfuscation)
- [ ] Inline CSS in `<style>` tag
- [ ] No external fonts (uses system fonts)
- [ ] Inline SVG favicon (data URI)
- [ ] Minimal JS (only email obfuscation IIFE)

**Accessibility:**
- [ ] Skip link: `<a href="#main-content" class="skip-link">`
- [ ] Semantic HTML: `<header>`, `<main>`, `<nav>`, `<section>`, `<footer>`
- [ ] ARIA labels on navigation: `aria-label="Footer navigation"`
- [ ] Proper heading hierarchy (h1 > h2 > h3)
- [ ] Alt text on images (if any added)

**SEO:**
- [ ] `<title>` with brand and description
- [ ] `<meta name="description">` with value prop
- [ ] `<link rel="canonical">`
- [ ] Open Graph tags (`og:title`, `og:description`, etc.)
- [ ] Twitter Card tags
- [ ] Structured data (JSON-LD SoftwareApplication)

**Responsive:**
- [ ] `<meta name="viewport">`
- [ ] CSS media queries for mobile
- [ ] Grid layouts with `auto-fit`

### 5. Content Accuracy (Medium)

**Verify claims match reality:**
- [ ] "Zero dependencies" - vendored libs only
- [ ] "MIT License" - core engines are MIT
- [ ] "WASM-first" - all compile to WASM
- [ ] Performance numbers match benchmarks
- [ ] Pricing ($29/mo pro) matches current offering

**Links must be valid:**
- [ ] GitHub links point to correct repo
- [ ] Documentation links work
- [ ] Contact email obfuscation works

## Audit Procedure

When `/site-update` is invoked:

1. **Read current site:**
   ```
   Read site/index.html
   ```

2. **Get current test counts:**
   ```bash
   make test 2>&1 | tail -20
   # Or read from MEMORY.md
   ```

3. **Compare modules:**
   - Extract module cards from HTML
   - Compare against TODO_FEATURES.md and CLAUDE.md
   - Flag missing modules

4. **Verify personas:**
   - Check all 4 persona sections exist
   - Verify anchor IDs match nav links
   - Ensure executives have ROI stats

5. **Check HTML quality:**
   - Validate structure
   - Check for common issues

6. **Generate report:**
   ```markdown
   ## Site Audit Report

   **Date:** YYYY-MM-DD
   **Issues Found:** N

   ### Test Counts
   - Current: X tests shown
   - Actual: Y tests
   - Status: [OK/NEEDS UPDATE]

   ### Modules
   - Implemented: X/Y shown
   - Planned: X/Y shown
   - Missing: [list]

   ### Personas
   - #developers: [OK/MISSING]
   - #executives: [OK/MISSING] (ROI: [OK/MISSING])
   - #operations: [OK/MISSING]
   - #partners: [OK/MISSING]

   ### HTML Quality
   - Performance: [OK/issues]
   - Accessibility: [OK/issues]
   - SEO: [OK/issues]

   ### Recommendations
   1. ...
   2. ...
   ```

## Fix Mode (--fix)

When `--fix` is specified:

1. Update test count badges and text
2. Add missing module cards
3. Fix HTML issues
4. Show diff of changes
5. Do NOT commit (user decides)

**Auto-fixable:**
- Test count numbers
- Missing module cards (using standard card template)
- Broken anchor links

**NOT auto-fixable (require manual review):**
- Content rewrites
- New persona sections
- Layout changes

## Card Templates

### Implemented Module Card
```html
<div class="card">
    <div class="card-header">
        <span class="card-name">ModuleName</span>
        <span class="card-lang">C</span>
    </div>
    <div class="card-desc">Short description of what it does.</div>
    <div class="card-meta">XX tests &middot; XK loc &middot; performance</div>
</div>
```

### Planned Module Card
```html
<div class="card planned">
    <div class="card-header">
        <span class="card-name">ModuleName</span>
        <span class="card-lang">C</span>
        <span class="badge-planned">planned</span>
    </div>
    <div class="card-desc"><strong>M</strong>odule <strong>N</strong>ame <strong>E</strong>xpansion. Description.</div>
</div>
```

## Improvement Suggestions

When auditing, also suggest improvements:

1. **Content gaps:**
   - Missing use cases
   - Unclear value propositions
   - Missing CTAs

2. **UX improvements:**
   - Navigation clarity
   - Mobile experience
   - Section length balance

3. **Technical improvements:**
   - Image optimization (if images added)
   - CSS optimization
   - Load time improvements

4. **Competitive positioning:**
   - Comparison tables
   - Case studies section
   - Testimonials placeholder

## Questions to Ask User

Before making major changes, clarify:

1. "Should planned modules show estimated timelines?"
2. "Should pricing be shown prominently or hidden?"
3. "Are there new modules not in TODO_FEATURES.md?"
4. "Should the demo CTA link to a working demo?"

## Example Invocations

```bash
# Full audit
/site-update

# Just update test counts
/site-update tests --fix

# Verify modules are complete
/site-update modules

# Check personas and fix
/site-update personas --fix
```
