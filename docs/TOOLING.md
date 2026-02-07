# Cross-Functional Tooling Plan

This document tracks cross-functional/cross-cutting concerns for the OTTO platform: memory safety, static analysis, logging, testing infrastructure, and developer tooling.

## Current State

### What Exists ✓
- Basic CI (GitHub Actions) - builds, tests, WASM, Docker
- Unit tests (700+ across modules)
- Debug builds (`make debug`)
- Consistent error enums per module (LC_ERROR_*, VL_ERROR_*, etc.)
- Memory safety guidelines in CLAUDE.md
- Benchmarks per module

### What's Missing ✗
- Memory sanitizers (ASAN, MSAN, UBSAN)
- Static analysis (clang-tidy, cppcheck)
- Code formatting (clang-format)
- Logging framework
- Fuzzing
- Code coverage
- API documentation generation

---

## Phase 1: Memory Safety Tooling

### 1.1 Sanitizer Build Targets

Add to root Makefile and each module's Makefile:

```makefile
# Sanitizer flags
ASAN_FLAGS = -fsanitize=address -fno-omit-frame-pointer -g
UBSAN_FLAGS = -fsanitize=undefined -fno-omit-frame-pointer -g
MSAN_FLAGS = -fsanitize=memory -fno-omit-frame-pointer -g

# Targets
asan: CFLAGS += $(ASAN_FLAGS)
asan: LDFLAGS += $(ASAN_FLAGS)
asan: all

ubsan: CFLAGS += $(UBSAN_FLAGS)
ubsan: LDFLAGS += $(UBSAN_FLAGS)
ubsan: all

sanitize: CFLAGS += $(ASAN_FLAGS) $(UBSAN_FLAGS)
sanitize: LDFLAGS += $(ASAN_FLAGS) $(UBSAN_FLAGS)
sanitize: all
```

**Tasks:**
- [ ] Add sanitizer targets to root Makefile
- [ ] Add sanitizer targets to ralph/Makefile
- [ ] Add sanitizer targets to shared/Makefile
- [ ] Add sanitizer targets to velo/Makefile
- [ ] Add sanitizer targets to carta/Makefile
- [ ] Add sanitizer targets to fuelwise/Makefile
- [ ] Add sanitizer targets to locus/Makefile

### 1.2 CI Integration

Add sanitizer job to `.github/workflows/ci.yml`:

```yaml
test-sanitizers:
  name: Memory Safety (ASAN/UBSAN)
  runs-on: ubuntu-latest
  steps:
    - uses: actions/checkout@v4
    - name: Build with sanitizers
      run: make sanitize
    - name: Run tests with sanitizers
      run: make test
      env:
        ASAN_OPTIONS: detect_leaks=1:abort_on_error=1
        UBSAN_OPTIONS: print_stacktrace=1:abort_on_error=1
```

**Tasks:**
- [ ] Add sanitizer CI job
- [ ] Ensure all tests pass under ASAN
- [ ] Fix any memory leaks detected
- [ ] Fix any undefined behavior detected

### 1.3 Valgrind Integration

```makefile
valgrind: debug
	valgrind --leak-check=full --track-origins=yes --error-exitcode=1 ./test_runner
```

**Tasks:**
- [ ] Add valgrind target to each module
- [ ] Create `make valgrind-all` in root Makefile
- [ ] Optional: Add valgrind CI job (slow, maybe nightly only)

---

## Phase 2: Static Analysis & Formatting

### 2.1 clang-format Configuration

Create `.clang-format` in root:

```yaml
BasedOnStyle: LLVM
IndentWidth: 4
TabWidth: 4
UseTab: Never
ColumnLimit: 100
BreakBeforeBraces: Linux
AllowShortFunctionsOnASingleLine: None
AllowShortIfStatementsOnASingleLine: false
AlwaysBreakAfterReturnType: None
PointerAlignment: Right
SpaceAfterCStyleCast: false
```

**Tasks:**
- [ ] Create .clang-format file
- [ ] Add `make format` target (formats all .c/.h files)
- [ ] Add `make format-check` target (CI - fails if unformatted)
- [ ] Format existing codebase (one-time, separate commit)

### 2.2 clang-tidy Configuration

Create `.clang-tidy` in root:

```yaml
Checks: >
  -*,
  bugprone-*,
  cert-*,
  clang-analyzer-*,
  misc-*,
  modernize-*,
  performance-*,
  portability-*,
  readability-*,
  -readability-magic-numbers,
  -bugprone-easily-swappable-parameters,
  -cert-err33-c

WarningsAsErrors: ''
HeaderFilterRegex: '.*'
```

**Tasks:**
- [ ] Create .clang-tidy file
- [ ] Generate compile_commands.json (`bear make` or cmake)
- [ ] Add `make lint` target
- [ ] Fix critical warnings
- [ ] Add lint job to CI (warnings only initially)

### 2.3 cppcheck Integration

```makefile
cppcheck:
	cppcheck --enable=all --error-exitcode=1 \
	  --suppress=missingIncludeSystem \
	  -I include -I ../shared/include \
	  src/
```

**Tasks:**
- [ ] Add cppcheck target to each module
- [ ] Add cppcheck to CI
- [ ] Create suppressions file for false positives

---

## Phase 3: Logging Framework

### 3.1 Design

Create `shared/include/sh_log.h`:

```c
typedef enum {
    SH_LOG_TRACE = 0,
    SH_LOG_DEBUG = 1,
    SH_LOG_INFO  = 2,
    SH_LOG_WARN  = 3,
    SH_LOG_ERROR = 4,
    SH_LOG_FATAL = 5,
    SH_LOG_OFF   = 6
} SHLogLevel;

/* Configuration */
void sh_log_set_level(SHLogLevel level);
void sh_log_set_file(FILE *fp);
void sh_log_set_callback(void (*cb)(SHLogLevel, const char *, void *), void *ctx);

/* Logging macros */
#define SH_LOG_TRACE(...) sh_log(SH_LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define SH_LOG_DEBUG(...) sh_log(SH_LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define SH_LOG_INFO(...)  sh_log(SH_LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define SH_LOG_WARN(...)  sh_log(SH_LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define SH_LOG_ERROR(...) sh_log(SH_LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define SH_LOG_FATAL(...) sh_log(SH_LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

/* Compile-time disable for release builds */
#ifdef SH_LOG_DISABLE
#define SH_LOG_TRACE(...) ((void)0)
#define SH_LOG_DEBUG(...) ((void)0)
#endif
```

**Tasks:**
- [ ] Design logging API (above is starting point)
- [ ] Implement sh_log.c in shared/
- [ ] Add tests for logging
- [ ] Optionally integrate into one module as pilot
- [ ] Document usage in CLAUDE.md

### 3.2 Features
- [ ] Thread-safe logging (optional, for API servers)
- [ ] JSON output format (optional, for structured logging)
- [ ] Log rotation (optional, for long-running servers)

---

## Phase 4: Fuzzing

### 4.1 Fuzz Targets

Priority targets (parsers that handle untrusted input):

| Target | Module | Input |
|--------|--------|-------|
| PBF parsing | shared | .osm.pbf files |
| Protobuf decoding | shared | Raw protobuf bytes |
| MVT decoding | carta | .mvt tiles |
| JSON parsing | API servers | HTTP request bodies |

### 4.2 Implementation

Create `fuzz/` directory:

```
fuzz/
├── fuzz_pbf.c        # Fuzz PBF parser
├── fuzz_protobuf.c   # Fuzz protobuf decoder
├── fuzz_mvt.c        # Fuzz MVT parser
├── corpus/           # Seed inputs
│   ├── pbf/
│   ├── protobuf/
│   └── mvt/
└── Makefile
```

Example harness (libFuzzer):

```c
#include "sh_pbf.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    SHPBFBlob blob;
    sh_pbf_decompress_blob(data, size, &blob);
    sh_pbf_blob_free(&blob);
    return 0;
}
```

**Tasks:**
- [ ] Create fuzz/ directory structure
- [ ] Implement fuzz_protobuf.c
- [ ] Implement fuzz_pbf.c
- [ ] Implement fuzz_mvt.c
- [ ] Add `make fuzz` target
- [ ] Collect seed corpus from test data
- [ ] Run fuzzing campaign (local or OSS-Fuzz)

---

## Phase 5: Code Coverage

### 5.1 Coverage Build

```makefile
COVERAGE_FLAGS = --coverage -fprofile-arcs -ftest-coverage

coverage: CFLAGS += $(COVERAGE_FLAGS)
coverage: LDFLAGS += $(COVERAGE_FLAGS)
coverage: clean all test
	gcov src/*.c
	lcov --capture --directory . --output-file coverage.info
	genhtml coverage.info --output-directory coverage-report
```

**Tasks:**
- [ ] Add coverage target to each module
- [ ] Add `make coverage-report` to root Makefile
- [ ] Integrate with codecov.io or coveralls
- [ ] Add coverage badge to README
- [ ] Set coverage threshold (e.g., 70%)

---

## Phase 6: Documentation Generation

### 6.1 Doxygen Configuration

Create `Doxyfile` in root:

```
PROJECT_NAME           = "OTTO Platform"
OUTPUT_DIRECTORY       = docs/api
INPUT                  = ralph/include shared/include velo/include carta/include fuelwise/include locus/include
RECURSIVE              = NO
EXTRACT_ALL            = YES
GENERATE_HTML          = YES
GENERATE_LATEX         = NO
```

**Tasks:**
- [ ] Create Doxyfile
- [ ] Add Doxygen comments to key headers
- [ ] Add `make docs` target
- [ ] Host generated docs (GitHub Pages?)

---

## Priority Order

1. **Phase 1: Memory Safety** - Highest impact, catches real bugs
2. **Phase 2: Static Analysis** - Catches bugs before runtime
3. **Phase 5: Code Coverage** - Identifies untested code
4. **Phase 4: Fuzzing** - Critical for parsers handling untrusted input
5. **Phase 3: Logging** - Useful but not urgent
6. **Phase 6: Documentation** - Nice to have

---

## Quick Wins (Can Do Now)

These require minimal setup:

```bash
# 1. Build with sanitizers (just add flags)
make CFLAGS="-fsanitize=address,undefined -g" all test

# 2. Run cppcheck (if installed)
cppcheck --enable=all -I include src/

# 3. Check formatting (if clang-format installed)
find . -name "*.c" -o -name "*.h" | xargs clang-format --dry-run -Werror
```

---

## Tracking

| Phase | Status | Notes |
|-------|--------|-------|
| 1. Memory Safety | Not Started | |
| 2. Static Analysis | Not Started | |
| 3. Logging | Not Started | |
| 4. Fuzzing | Not Started | |
| 5. Code Coverage | Not Started | |
| 6. Documentation | Not Started | |
