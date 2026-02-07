# ClayShards Renderer Testing Plan

## Overview

Strategy for making all ClayShards renderers testable with Claude Code feedback loops for end-to-end UI regression analysis.

## 1. TUI Renderer (C) - Already Testable

```c
// Headless mode renders to buffer
CSTUIBuffer buf = cs_tui_create_buffer(80, 24);
cs_tui_render_headless(&ctx, &buf);

// Compare against reference
assert(memcmp(buf.chars, expected_chars, 80*24) == 0);
assert(memcmp(buf.colors, expected_colors, 80*24*3) == 0);
```

**What to add:**
- `cs_tui_buffer_to_string()` - dump buffer as text for snapshots
- `cs_tui_buffer_diff()` - report character/color differences
- Snapshot files in `tests/snapshots/tui/*.txt`

## 2. TUI-WebGL Renderer (JS + WebGL)

**Layer separation:**

| Layer | Test Method |
|-------|-------------|
| WASM→JS interop | Node.js unit tests (mock canvas) |
| Character grid logic | Headless TUI buffer comparison |
| WebGL CRT effects | Headless WebGL (see below) |
| Final composite | Screenshot comparison |

**Headless WebGL options:**

```javascript
// Option A: headless-gl (pure Node, no browser)
const gl = require('gl')(512, 512);
const renderer = new TUIWebGLRenderer(gl);
renderer.render(tuiBuffer);
const pixels = new Uint8Array(512 * 512 * 4);
gl.readPixels(0, 0, 512, 512, gl.RGBA, gl.UNSIGNED_BYTE, pixels);

// Option B: Playwright/Puppeteer (real browser, headless)
const browser = await playwright.chromium.launch({ headless: true });
const page = await browser.newPage();
await page.goto('file://test-harness.html');
await page.screenshot({ path: 'output.png' });
```

**Recommended: Playwright** - real WebGL, runs in CI, Claude can read screenshots.

## 3. Pure WebGL Renderer (JS)

Same approach as TUI-WebGL but without TUI layer:

```javascript
// test-harness.js
import { WebGLRenderer } from './renderer.js';

const canvas = document.getElementById('test-canvas');
const renderer = new WebGLRenderer(canvas);

// Render test scene
renderer.beginFrame();
cs_button("Test Button", 100, 50);
cs_label("Hello World", 100, 100);
renderer.endFrame();

// Screenshot captured by Playwright
```

## 4. Claude Code Feedback Loop

**The key insight:** Claude can read PNG images via the Read tool. Create a test harness that:

1. Renders component → PNG
2. Compares against reference PNG
3. Outputs text diff Claude can parse

**Directory structure:**
```
clayshards/tests/
├── visual/
│   ├── run-visual-tests.sh      # Claude runs this
│   ├── harness.html             # Test page
│   ├── harness.js               # Render test scenes
│   ├── snapshots/
│   │   ├── button-default.png   # Reference images
│   │   ├── button-hover.png
│   │   └── tui-grid-80x24.png
│   └── output/
│       ├── button-default.png   # Current render
│       └── diff-button-default.png  # Visual diff
```

**Test script for Claude:**

```bash
#!/bin/bash
# run-visual-tests.sh - Claude invokes this

# Start test server
npx playwright test --reporter=json > results.json

# Generate diff images and text report
node generate-diff-report.js > visual-diff.txt

# Output summary for Claude
echo "=== VISUAL TEST RESULTS ==="
cat visual-diff.txt
echo ""
echo "Diff images saved to: tests/visual/output/"
echo "Use Read tool to view any PNG files"
```

**Diff report format (Claude-readable):**

```
=== VISUAL TEST RESULTS ===

PASS: button-default (0.00% diff)
PASS: button-hover (0.02% diff, below threshold)
FAIL: tui-grid-80x24 (3.45% diff)
  - Region (120,45)-(180,60): Character mismatch
  - Diff image: tests/visual/output/diff-tui-grid-80x24.png

Summary: 2/3 passed, 1 failed
```

**Claude workflow:**
1. Claude runs: `./run-visual-tests.sh`
2. Reads text output, sees failure
3. Reads diff PNG to see visual difference
4. Reads reference PNG and current PNG
5. Identifies issue in renderer code
6. Fixes code
7. Re-runs tests

## 5. Implementation Priority

| Phase | Task | Effort |
|-------|------|--------|
| **1** | TUI snapshot tests (C) | 2 hrs |
| **2** | Playwright harness setup | 4 hrs |
| **3** | TUI-WebGL screenshot tests | 2 hrs |
| **4** | Pure WebGL screenshot tests | 2 hrs |
| **5** | Diff image generation | 2 hrs |
| **6** | Claude-readable report format | 1 hr |

**Total: ~13 hours**

## 6. Regression Test Workflow

```bash
# Update snapshots (after intentional changes)
./run-visual-tests.sh --update-snapshots

# CI check (fails on any diff > threshold)
./run-visual-tests.sh --ci --threshold=0.1

# Claude debugging session
./run-visual-tests.sh  # Claude reads output + images
```

## 7. Quick Win - TUI Text Snapshots

Add this to TUI renderer (30 min):

```c
// cs_tui.h
int cs_tui_buffer_save_txt(const CSTUIBuffer *buf, const char *path);
int cs_tui_buffer_diff(const CSTUIBuffer *a, const CSTUIBuffer *b,
                       char *diff_out, size_t diff_size);
```

Then Claude can:
```bash
./test_tui_headless > /tmp/tui_output.txt
diff /tmp/tui_output.txt tests/snapshots/expected.txt
```

This gives an immediate feedback loop for TUI without any browser infrastructure.

## 8. Tools and Dependencies

| Tool | Purpose | Install |
|------|---------|---------|
| Playwright | Headless browser testing | `npm install -D @playwright/test` |
| pixelmatch | Image diff library | `npm install -D pixelmatch` |
| pngjs | PNG read/write in Node | `npm install -D pngjs` |

## 9. CI Integration

```yaml
# .github/workflows/visual-tests.yml
visual-tests:
  runs-on: ubuntu-latest
  steps:
    - uses: actions/checkout@v4
    - name: Install Playwright
      run: npx playwright install --with-deps chromium
    - name: Run visual tests
      run: ./clayshards/tests/visual/run-visual-tests.sh --ci
    - name: Upload diff artifacts
      if: failure()
      uses: actions/upload-artifact@v4
      with:
        name: visual-diffs
        path: clayshards/tests/visual/output/
```
