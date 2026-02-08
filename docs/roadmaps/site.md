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

**Status:** Complete
**Effort:** 3-4 hours

### Final Coverage

All practical endpoints now have WASM demos:

| Module | Endpoints | With Demos | Notes |
|--------|-----------|------------|-------|
| Carta | 4 | 4 | png, mvt, ascii, tilejson |
| Velo | 5 | 3 | route, health, stats (POST route uses same logic as GET) |
| Locus | 6 | 5 | search, autocomplete, reverse, health, stats |
| FuelWise | 5 | 3 | solve, health, stats |

### Completed

- [x] Add `/api/v1/solve` demo to FuelWise (core LP optimization)
- [x] Add MVT demo for Carta (shows tile size info)
- [x] Add ASCII demo for Carta (renders map as text art)
- [x] Update `api-config.json` with all button mappings
- [x] Add handler functions for all new demos

### Not Adding (By Design)

| Endpoint | Reason |
|----------|--------|
| FuelWise filter/optimize | Require route polyline (complex input) |
| POST /api/v1/route | Same as GET, just different input method |
| /metrics | Prometheus format, not useful for browser demo |

---

## Phase 4: Simplify Build Pipeline

**Status:** In Progress (Partial)
**Effort:** 4-6 hours (reduced scope)

### Current Flow

```
C headers (@api annotations)
       |
api-config.json (module config, button mappings)
       |
build-api-docs.py (parser + generator)
       |
api-template.html (Jinja-like template)
       |
js/handlers/*.js (4 handler files)
       |
site/api.html (output)
```

### Implemented Improvements

1. **Added `@demo_fetch` annotation** - For simple fetch endpoints, specify the path directly:
   ```c
   * @demo json
   * @demo_fetch /api/v1/health
   ```

2. **Added `@demo_handler` annotation** - For custom handlers:
   ```c
   * @demo image
   * @demo_handler generateCartaTile
   ```

3. **Added HandlerGenerator class** - Infrastructure for auto-generating simple handlers

### Kept As-Is (Working Well)

- `api-config.json` for module-level config (ports, WASM factory names)
- Custom handlers in `js/handlers/*.js` for complex demos (tile generation, routing)
- Template-based HTML generation

### Why Not Full Automation

The original plan to move ALL config into C headers was reconsidered:

| Concern | Decision |
|---------|----------|
| Mixing JS config in C headers | Keep separate - cleaner separation of concerns |
| Complex handler logic | Keep in JS files - easier to debug/modify |
| Module-level config | Keep in JSON - rarely changes, easy to edit |
| Simple fetch handlers | Can auto-generate, but manual is ~5 lines each |

### Future Enhancements (Optional)

If maintenance burden grows:
- Generate `api-handlers.js` from `@demo_fetch` annotations
- Remove redundant button mappings from `api-config.json`
- Add `@module` annotations for module-level config

---

## Phase 5: UI/UX Polish

**Status:** Complete
**Effort:** 2-3 hours

### Implemented

#### Copy Buttons (Done)
- Added "Copy" button to all code blocks (examples, responses, demo output)
- Appears on hover, copies text content to clipboard
- Visual feedback: button shows "Copied!" with green highlight
- Mobile: always visible (no hover state)

#### Editable Demo Inputs (Already Present)
- Velo route demo: editable from/to coordinates, profile, mode selects
- Carta tile demo: editable z/x/y coordinates
- No additional work needed

### Not Implementing (Low Value/High Effort)

| Feature | Reason |
|---------|--------|
| Response format toggle | Complex UI for minimal benefit; users can copy and format externally |
| Side-by-side layout | Would require significant HTML restructuring; current flow is clear |
| TypeScript code gen | Niche use case; cURL example sufficient |

### CSS/JS Added

```css
.copy-btn {
    position: absolute;
    top: 8px;
    right: 8px;
    opacity: 0;
    transition: opacity 0.15s;
}
.code-block:hover .copy-btn { opacity: 1; }
.copy-btn.copied { background: var(--success); }
```

```javascript
function copyToClipboard(text, btn) {
    navigator.clipboard.writeText(text).then(() => {
        btn.textContent = 'Copied!';
        btn.classList.add('copied');
        // Reset after 1.5s
    });
}
```

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
