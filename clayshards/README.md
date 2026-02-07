# OTTO UI System

Immediate-mode UI components built on Clay layout, designed for C11 and WebAssembly.

## Components

| Component | Location | Description |
|-----------|----------|-------------|
| **clay-shards** | `clay-shards/` | Core C library - immediate mode components |
| **clay-shards-webgl** | `clay-shards-webgl/` | WebGL renderer for browsers |
| **clay-shards-tui** | `clay-shards-tui/` | Terminal renderer (ANSI escape sequences) |
| **clay-shards-tui-webgl** | `clay-shards-tui-webgl/` | TUI WebGL renderer with CRT effects |
| **clay-shards-demo** | `clay-shards-demo/` | Example map viewer application |

## Architecture

```
┌─────────────────────────────────────────┐
│         Application Code                │
│  if (cs_button(...).clicked) { ... }    │
├─────────────────────────────────────────┤
│            ClayShards                   │
│  • Immediate mode API                   │
│  • Focus/hover/click handling           │
│  • Thread-local state (TLS)             │
│  • Custom allocator support             │
├─────────────────────────────────────────┤
│            Clay Layout                  │
│  • Declarative element tree             │
│  • Layout computation                   │
├─────────────────────────────────────────┤
│            Renderer                     │
│  • WebGL (browser)                      │
│  • TUI (terminal ANSI)                  │
│  • TUI WebGL (browser with CRT effects) │
│  • SDL/raylib/sokol (native) [planned]  │
└─────────────────────────────────────────┘
```

## Features

### Thread Safety
- All global state uses thread-local storage (`CS_THREAD_LOCAL`)
- Each thread gets isolated UI state (focus, widget state, errors)
- Call `cs_init()` once per thread

### Custom Allocators
- Replace malloc/realloc/free with your own functions
- Useful for arena allocators or debugging

```c
CsAllocator arena = {
    .alloc = my_alloc,
    .realloc = my_realloc,
    .free = my_free,
    .user_data = &my_arena
};
cs_set_allocator(&arena);
cs_init();
```

### Error Tracking
- Silent failures are recorded for debugging
- `cs_get_last_error()` - get last error code
- `cs_get_error_count()` - count errors since last clear
- `cs_clear_errors()` - reset error state

### Map Component
- Web Mercator projection utilities
- Douglas-Peucker polyline simplification (iterative, stack-safe)
- Multi-instance support with per-map state
- Overlay system (markers, polylines) with hit testing

## Building

```bash
cd clay-shards && make       # Build core library
cd clay-shards && make test  # Run tests

cd clay-shards-tui && make       # Build TUI renderer
cd clay-shards-tui && make test  # Run TUI tests
```

## Documentation

- [ClayShards API](clay-shards/CLAUDE.md) - Full API reference
- [TUI Renderer](clay-shards-tui/CLAUDE.md) - Terminal renderer documentation
- [TUI WebGL Renderer](clay-shards-tui-webgl/CLAUDE.md) - Browser TUI with CRT effects
- [Design Principles](clay-shards/MANIFESTO.md) - Architecture philosophy
- [Compliance Review](clay-shards/REVIEW.md) - Code quality assessment
- [Contributing](clay-shards/CONTRIBUTING.md) - Adding new widgets

## Renderers

| Backend | Target | Status |
|---------|--------|--------|
| `clay-shards-webgl` | Browsers | Active |
| `clay-shards-tui` | Terminal (ANSI) | Active |
| `clay-shards-tui-webgl` | Browser TUI with CRT effects | Active |
| `clay-shards-sdl` | Desktop/Mobile | Planned |
| `clay-shards-raylib` | Games | Planned |
| `clay-shards-sokol` | Minimal deps | Planned |
