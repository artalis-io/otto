# Build-flag stamp
#
# Every build variant in this tree writes to the same paths: objects sit next
# to their sources as src/foo.o, and the archive is lib<module>.a at the module
# root. Release, debug, sanitizer, gcc and cl all land there. Make's only test
# for staleness is timestamp-against-source, so switching variants invalidates
# nothing -- the objects exist, no source is newer, and they are reused as
# though they had been built the way you are now asking for.
#
# Three instances of that in one day:
#
#   - `shared test-asan` is `clean test`, so it rebuilt libshared.a with
#     /Zi /Od, -DDEBUG and ASan. The `make -C shared lib` after it called that
#     archive current. Six servers linked it, and the three backed by solvers
#     answered {"status":"error","iterations":0}. It looked like a solver bug
#     in a debug build, and was written up as one.
#
#   - A worktree built first with gcc and then with cl linked MinGW objects
#     into an MSVC link and failed on __stack_chk_fail, with no hint of why.
#     CLAUDE.md tells the reader to run `make clean` when switching compilers;
#     that instruction exists because the build cannot notice for itself.
#
#   - The same contamination produced a reproduction that sent an entire
#     investigation after a solver that was working correctly.
#
# The fix records the flags an invocation will compile with, and discards
# anything built with different ones.
#
# WHY AT PARSE TIME. The obvious shape -- a stamp file with a rule, and objects
# depending on it -- does not work. Make takes its out-of-date snapshot before
# running any recipe, so a rule that rewrites the stamp cannot retroactively
# invalidate objects make has already decided are current. Measured: the rule
# ran, the stamp came out newer than the object, and the object was not
# rebuilt. A variant that deleted the objects instead was worse -- make
# reported success having built nothing, because the files vanished after the
# snapshot was taken.
#
# So the comparison happens in $(shell ...) during parsing, before the snapshot
# exists. That means the flags must be known without target-specific variables,
# which are not resolved until a target is being built. DEBUG_GOALS names the
# goals that compile with DEBUG_CFLAGS, and the effective flags are derived
# from $(MAKECMDGOALS).
#
# Include this AFTER CFLAGS, DEBUG_CFLAGS, OBJS and LIB_FILE are defined, and
# set DEBUG_GOALS to the targets that assign CFLAGS = $(DEBUG_CFLAGS).
#
#     DEBUG_GOALS = debug test-asan
#     include ../mk/flagstamp.mk
#
# STAMP_ARTIFACTS defaults to the objects and the archive. A module with other
# outputs that embed the flags can extend it before including.

BUILD_STAMP ?= .build-flags
DEBUG_GOALS ?=
STAMP_ARTIFACTS ?= $(OBJS) $(LIB_FILE)

# The flags this invocation will actually compile with. `make` with no goal
# builds the default one, which is never a debug goal.
STAMP_FLAGS := $(strip $(CC) \
    $(if $(filter $(DEBUG_GOALS),$(MAKECMDGOALS)),$(DEBUG_CFLAGS),$(CFLAGS)))

# Discard anything built with different flags, before make looks at the tree.
# Printed to stderr rather than stdout so it cannot be mistaken for build
# output being parsed by something.
#
# `clean` is deliberately not special-cased: it deletes these files anyway, and
# leaving the stamp matching the release flags afterwards is correct.
$(shell \
  if [ "$$(cat $(BUILD_STAMP) 2>/dev/null)" != "$(STAMP_FLAGS)" ]; then \
      if [ -f $(BUILD_STAMP) ]; then \
          echo "build flags changed; discarding objects built the old way" >&2; \
      fi; \
      rm -f $(STAMP_ARTIFACTS); \
      printf '%s' '$(STAMP_FLAGS)' > $(BUILD_STAMP); \
  fi)
