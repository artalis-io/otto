# Site & API Documentation Roadmap

Landing page (`site/index.html`) and API documentation (`site/api.html`) improvements.

## Current State

- **index.html**: CRT terminal aesthetic, typing animations, module showcase
- **api.html**: Generated from C header `@api` annotations via `build-api-docs.py`
- **WASM demos**: Embedded Monaco data, "Try in Browser" buttons for some endpoints

## Known Issues

| Issue | Impact | Status |
|-------|--------|--------|
| CRT typing breaks on re-click | Garbled output, poor UX | **Phase 1** |
| Example JSONs outdated | Misleading documentation | Phase 2 |
| WASM demos missing for many endpoints | Incomplete interactive docs | Phase 3 |
| Build pipeline too complex | Hard to maintain | Phase 4 |

---

## Phase 1: Fix CRT Typing Bug

**Status:** Complete
**Effort:** 1-2 hours

### Problem

`typeAnimateContent()` in `site/js/typing-animation.js` doesn't cancel existing animations before starting a new one. Clicking "Try" twice causes multiple animations to run in parallel, producing garbled output.

### Solution

1. Track active animation IDs per block using a Map
2. Increment animation ID on each new animation
3. Check animation ID before each character render - abort if superseded

### Implementation

```javascript
const activeAnimations = new Map();
let animationIdCounter = 0;

function typeAnimateContent(block, html) {
    // Cancel any existing animation on this block
    const thisAnimationId = ++animationIdCounter;
    activeAnimations.set(block, thisAnimationId);

    // ... setup ...

    function typeNext() {
        // Abort if a newer animation started
        if (activeAnimations.get(block) !== thisAnimationId) {
            return;
        }
        // ... render character ...
    }
}
```

### Files

- `site/js/typing-animation.js` - Added animation ID tracking

---

## Phase 2: Sync Examples with Reality

**Status:** Complete
**Effort:** 2-3 hours

### Problem

`@response_json` annotations in C headers were:
1. Sometimes invalid JSON (parser bug with nested objects)
2. Outdated compared to actual implementation (e.g., TileJSON 2.2.0 vs 3.0.0)

### Solution

#### Part A: JSON Syntax Validation (Implemented)
Added `--validate` flag to `build-api-docs.py` that validates all `@response_json` annotations are syntactically valid JSON.

```bash
python3 scripts/build-api-docs.py --validate
```

#### Part B: Fixed Parser Bug
The parser was stopping at the first `}` instead of tracking nested braces/brackets. Fixed by counting `{}`/`[]` depth.

#### Part C: Fixed Known Discrepancies
- `carta/include/ct_api.h`: Updated TileJSON from 2.2.0 to 3.0.0, added missing fields

### Future Enhancement
Option B: `@response_from_wasm` that fetches real response at build time (not implemented yet).

### Files

- `scripts/build-api-docs.py` - Added `--validate` flag, fixed brace parsing
- `carta/include/ct_api.h` - Updated TileJSON response

---

## Phase 3: Enable All WASM Demos

**Status:** In Progress
**Effort:** 3-4 hours

### Current Coverage

| Module | Endpoints | WASM Demos | Missing |
|--------|-----------|------------|---------|
| Carta | 4 | 2 | mvt, ascii |
| Velo | 5 | 3 | nearest, snap |
| Locus | 6 | 5 | batch |
| FuelWise | 5 | 3 | filter, optimize |

### Completed

- [x] Add `/api/v1/solve` demo to FuelWise (core LP optimization)
- [x] Update `api-config.json` button mapping for solve
- [x] Add `solveFuelWiseProblem()` handler with sample problem

### Remaining Tasks

1. Add missing Velo demos (nearest, snap)
2. Add MVT demo for Carta (hex dump or parsed view)
3. Add batch demo for Locus (if worthwhile for small Monaco data)
4. Consider filter/optimize demos for FuelWise (require route polyline)

### Auto-generate Button Wiring

Instead of manual config, derive from annotations:

```c
/*@api
 * @demo json
 * @demo_button optimize-try-btn
 * @demo_handler runOptimize
 */
```

---

## Phase 4: Simplify Build Pipeline

**Status:** Planned
**Effort:** 4-6 hours

### Current Flow (Too Many Files)

```
C headers (@api annotations)
       |
api-config.json (manual module config, button mappings)
       |
build-api-docs.py (parser + generator)
       |
api-template.html (Jinja-like template)
       |
js/handlers/*.js (manual, 4 files)
       |
site/api.html (output)
```

### Proposed Flow

```
C headers (ALL metadata in annotations)
       |
build-api-docs.py (single script)
       |
site/api.html + site/js/api-handlers.js (both generated)
```

### New Annotation Schema

File-level module metadata:

```c
/*@module
 * @name Carta Tile Server
 * @icon 🗺️
 * @port 8081
 * @description Serves vector tiles (MVT), raster tiles (PNG)...
 * @wasm_factory CartaAPIDemo
 * @wasm_class CartaDemo
 */
```

Endpoint with demo config:

```c
/*@api
 * GET /tiles/{z}/{x}/{y}.png
 * Raster tile (PNG)
 *
 * @path z:int:required Zoom level (0-18)
 * @returns image/png
 *
 * @demo type:image
 * @demo input:z:number:14:0:18 "Zoom level"
 * @demo input:x:number:8529 "Tile X"
 * @demo input:y:number:5974 "Tile Y"
 * @demo call:getTileDataURL(z,x,y)
 */
```

### Generated Handler Code

`build-api-docs.py` generates `site/js/api-handlers.js`:

```javascript
// AUTO-GENERATED - DO NOT EDIT
const ApiHandlers = {
    'carta-tiles-png': {
        type: 'image',
        inputs: [
            {id: 'z', type: 'number', default: 14, min: 0, max: 18, label: 'Zoom level'},
            {id: 'x', type: 'number', default: 8529, label: 'Tile X'},
            {id: 'y', type: 'number', default: 5974, label: 'Tile Y'}
        ],
        async run(demo, inputs) {
            return demo.getTileDataURL(inputs.z, inputs.x, inputs.y);
        }
    },
    // ... other handlers
};
```

### Benefits

- Single source of truth (C headers)
- No manual JS handler maintenance
- Fewer config files
- Easier to add new endpoints

---

## Phase 5: UI/UX Polish

**Status:** Planned
**Effort:** 2-3 hours

### Response Format Toggle

Add tabs for different views:

```
[Raw JSON] [Formatted] [cURL] [TypeScript]
```

### Editable Demo Inputs

Replace static values with editable fields:

```html
<div class="demo-inputs">
    <label>z: <input type="number" value="14" min="0" max="18"></label>
    <label>x: <input type="number" value="8529"></label>
    <label>y: <input type="number" value="5974"></label>
</div>
```

### Request/Response Layout

Side-by-side view:

```
+---------------------+----------------------+
| REQUEST             | RESPONSE             |
| GET /tiles/14/...   | { "tilejson": ... }  |
+---------------------+----------------------+
```

### Copy Buttons

One-click copy for:
- cURL command
- Response JSON
- TypeScript fetch code

---

## Priority Matrix

| Phase | Effort | Value | Priority |
|-------|--------|-------|----------|
| 1. Fix typing bug | 1-2h | High (broken UX) | **P0** |
| 2. Sync examples | 2-3h | High (wrong docs) | **P1** |
| 3. Enable all demos | 3-4h | Medium | **P2** |
| 4. Simplify pipeline | 4-6h | High (maintainability) | **P2** |
| 5. UI/UX polish | 2-3h | Medium | **P3** |

---

## Related Files

| File | Purpose |
|------|---------|
| `site/api-template.html` | HTML template with Jinja-like syntax |
| `site/api-config.json` | Module config, button mappings |
| `scripts/build-api-docs.py` | Generator script |
| `site/js/typing-animation.js` | CRT typing effect |
| `site/js/handlers/*.js` | Per-module WASM demo handlers |
| `*/include/*_api.h` | C headers with `@api` annotations |
