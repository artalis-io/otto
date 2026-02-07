# ClayShards Manifesto

For the full OTTO design philosophy including ClayShards principles, see [docs/MANIFESTO.md](../../docs/MANIFESTO.md).

This file summarizes the key ClayShards-specific principles.

---

## Core Principles

1. **Immediate mode** - UI rebuilt each frame, no retained widget tree
2. **Layout is declarative; components are imperative** - Clay handles layout, ClayShards handles interaction
3. **Stable identity via string hash** - `CS_ID("name")` for deterministic widget state
4. **Renderer-agnostic** - Same code runs on WebGL, TUI, framebuffer
5. **State ownership** - App owns business state, ClayShards owns UI state

## The ClayShards Promise

- **The same UI code runs across targets.**
- Your UI will run in **WASM/WebGL** and **embedded** without rewriting.
- Your business logic remains in **C**.
- Your renderer remains **replaceable** because it consumes render commands.

## Non-Goals

ClayShards is **not**: a DOM, a retained-mode framework, a React clone, or a CSS engine.

## See Also

- [CLAUDE.md](CLAUDE.md) - API reference and usage
- [DESIGN.md](DESIGN.md) - Architecture details
- [docs/MANIFESTO.md](../../docs/MANIFESTO.md) - Full design philosophy
