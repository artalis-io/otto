# Technical Internals

Deep-dive technical documentation for OTTO's core algorithms and architectures.

## Contents

| Document | Component | Description |
|----------|-----------|-------------|
| [security-model.md](security-model.md) | Platform | Role separation, process isolation, hardening |
| [ralph-architecture.md](ralph-architecture.md) | Ralph | LP/MIP solver architecture, data flow, performance |
| [lu-factorization.md](lu-factorization.md) | Ralph | LU decomposition, eta updates, numerical stability |
| [simplex.md](simplex.md) | Ralph | Revised simplex implementation details |
| [clayshards-design.md](clayshards-design.md) | ClayShards | Widget state, focus model, render contract |

## Audience

These documents are for developers working on the internals of OTTO's core engines.
For API usage, see component CLAUDE.md files. For roadmaps, see `docs/roadmaps/`.
