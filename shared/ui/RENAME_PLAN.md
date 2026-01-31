# ClayShards Rename Plan

Rename from `cc_` (Clay Components) to `cs_` (ClayShards) prefix and reorganize directories.

---

## Directory Renames

| Old | New | Reason |
|-----|-----|--------|
| `clay-components/` | `clay-shards/` | Library is now called ClayShards |
| `cc-map-demo/` | `clay-shards-demo/` | Consistent naming |
| `clay-renderer-webgl/` | `clay-shards-webgl/` | Contains ClayShards-specific code (cursor, keyboard, focus) |

**Note on clay-renderer-webgl:** This directory contains both:
- Pure Clay rendering (renderer.js, shaders.js, font.js)
- ClayShards-specific code (keyboard.js, text-cursor.js, render-loop.js with cursor)

Since the two are tightly coupled (render loop needs cursor, keyboard needs focus), keeping them together as `clay-shards-webgl` makes sense. The "webgl" suffix indicates the rendering target.

---

## Prefix Changes

| Old | New | Scope |
|-----|-----|-------|
| `cc_` | `cs_` | Functions |
| `CC_` | `CS_` | Macros |
| `Cc` | `Cs` | Types (CcButtonResult → CsButtonResult) |

---

## File Renames (clay-shards/)

### Headers (include/)

| Old | New |
|-----|-----|
| `cc_common.h` | `cs_common.h` |
| `cc_button.h` | `cs_button.h` |
| `cc_input.h` | `cs_input.h` |
| `cc_map.h` | `cs_map.h` |
| `cc_clay.h` | `cs_clay.h` |
| `cc_immediate.h` | `cs_immediate.h` |
| `clay_components.h` | `clay_shards.h` |

### Sources (src/)

| Old | New |
|-----|-----|
| `cc_common.c` | `cs_common.c` |
| `cc_button.c` | `cs_button.c` |
| `cc_input.c` | `cs_input.c` |
| `cc_map.c` | `cs_map.c` |
| `cc_clay.c` | `cs_clay.c` |
| `cc_immediate.c` | `cs_immediate.c` |
| `cc_internal.h` | `cs_internal.h` |

### Tests

| Old | New |
|-----|-----|
| `test_immediate.c` | `test_clay_shards.c` |

---

## Symbol Renames (non-exhaustive)

### Core API
- `cc_init` → `cs_init`
- `cc_frame_begin` → `cs_frame_begin`
- `cc_frame_end` → `cs_frame_end`
- `cc_hash_id` → `cs_hash_id`
- `CC_ID()` → `CS_ID()`
- `cc_focus` → `cs_focus`
- `cc_blur` → `cs_blur`
- `cc_focused_id` → `cs_focused_id`

### Types
- `CcState` → `CsState`
- `CcWidgetState` → `CsWidgetState`
- `CcButtonResult` → `CsButtonResult`
- `CcButtonStyle` → `CsButtonStyle`
- `CcInputResult` → `CsInputResult`
- `CcInputStyle` → `CsInputStyle`
- `CcMapResult` → `CsMapResult`
- `CcMapStyle` → `CsMapStyle`
- `CcAlign` → `CsAlign`
- `CcMargin` → `CsMargin`

### Constants
- `CC_WIDGET_STATE_CAPACITY` → `CS_WIDGET_STATE_CAPACITY`
- `CC_MAX_FOCUSABLES` → `CS_MAX_FOCUSABLES`
- `CC_CURSOR_BLINK_PERIOD` → `CS_CURSOR_BLINK_PERIOD`
- `CC_COLOR_*` → `CS_COLOR_*`
- `CC_BTN_*` → `CS_BTN_*`
- `CC_ALIGN_*` → `CS_ALIGN_*`

### Clay Integration
- `cc_clay_init` → `cs_clay_init`
- `cc_clay_begin_frame` → `cs_clay_begin_frame`
- `cc_clay_end_frame` → `cs_clay_end_frame`
- `cc_clay_cmd_*` → `cs_clay_cmd_*`
- `cc_clay_set_pointer` → `cs_clay_set_pointer`
- `CcClayConfig` → `CsClayConfig`

### Components
- `cc_button` → `cs_button`
- `cc_button_simple` → `cs_button_simple`
- `cc_input` → `cs_input`
- `cc_input_simple` → `cs_input_simple`
- `cc_map` → `cs_map`
- `cc_map_*` → `cs_map_*`

---

## JavaScript Updates (clay-shards-webgl/)

### WASM Export References

All `wasm.cc_*` calls become `wasm.cs_*`:
- `wasm.cc_focused_id` → `wasm.cs_focused_id`
- `wasm.cc_cursor_pos` → `wasm.cs_cursor_pos`
- `wasm.cc_key_down` → `wasm.cs_key_down`
- etc.

### Files to Update
- `keyboard.js` - WASM function calls
- `text-cursor.js` - WASM function calls
- `render-loop.js` - WASM function calls
- `index.js` - No changes (just re-exports)

---

## Demo Updates (clay-shards-demo/)

### WASM Exports (Makefile)
All `_cc_*` exports become `_cs_*`

### JavaScript (map.js)
All `wasm.cc_*` calls become `wasm.cs_*`

### C Source (map_ui.c)
- Include paths: `cc_*.h` → `cs_*.h`
- Function calls: `cc_*` → `cs_*`
- Macro calls: `CC_ID` → `CS_ID`

---

## Documentation Updates

### Files to Update
- `MANIFESTO.md` - Example code snippets
- `DESIGN.md` - All code examples and references
- `CONTRIBUTING.md` - All code examples
- `README.md` - Hero example and API references
- `CLAUDE.md` files - API documentation

---

## Implementation Order

1. **Commit this plan**
2. **Rename directories** (git mv)
3. **Rename C files** (git mv)
4. **Find/replace in C sources** (cc_ → cs_, CC_ → CS_, Cc → Cs)
5. **Update Makefiles** (file references)
6. **Find/replace in JS sources** (cc_ → cs_)
7. **Update WASM export lists** (Makefile)
8. **Update documentation** (all .md files)
9. **Run tests** (verify nothing broke)
10. **Rebuild WASM** (verify compilation)
11. **Test in browser** (verify runtime)

---

## Risks

1. **Broken imports** - External code referencing old names will break
2. **WASM export mismatch** - JS calling old names, C exporting new names
3. **Documentation drift** - Missed code snippets in docs

**Mitigation:** Thorough grep for `cc_`, `CC_`, `Cc` after rename to catch stragglers.

---

## Not Changing

- Clay library itself (vendor/clay/) - not ours to rename
- CLAY_ macros - these are Clay's, not ClayShards'
- Clay_* types - same reason
