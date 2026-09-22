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

# ---------------------------------------------------------------------------
# Cross-module compatibility
#
# The stamp above protects a module from its own past. It says nothing about
# the libraries it links, which a separate make invocation built under a
# separate stamp: `make -C ralph` never re-derives shared. Two ways that has
# actually bitten:
#
#   - a module built with gcc linking a libshared.a that cl produced. The
#     diagnostic is "corrupt .drectve", then ld exiting 5 with nothing more
#     to say.
#
#   - an unsanitized link line picking up a library that test-asan left
#     sanitized: undefined __asan_report_load4 and friends, which is what
#     fuelwise/bench hit when its sanitizer job was first wired up.
#
# Set STAMP_REQUIRE_DIRS to the directories whose libraries this module
# links. The comparison itself, and why it is only two properties, is in
# mk/stamp_compat.sh.
#
#     STAMP_REQUIRE_DIRS = ../shared
#     include ../mk/flagstamp.mk
STAMP_REQUIRE_DIRS ?=

STAMP_CC     := $(firstword $(STAMP_FLAGS))
STAMP_FAMILY := $(if $(filter cl cl.exe,$(notdir $(STAMP_CC))),msvc,gnu)
STAMP_SAN    := $(if $(findstring fsanitize,$(STAMP_FLAGS)),yes,no)
STAMP_COMPAT := $(dir $(lastword $(MAKEFILE_LIST)))stamp_compat.sh

# A warning, not an error, and that is a deliberate downgrade. The check runs
# when the makefile is parsed, which is before a module that rebuilds its own
# dependencies has had the chance to: `make -C fuelwise all` has `shared` as a
# prerequisite and would have fixed a stale libshared.a by itself, so stopping
# at parse time broke a build that was going to succeed. Measured, not
# supposed -- it was tried as an error first.
#
# What it is for is turning "corrupt .drectve", "ld returned 5" and undefined
# __asan_report_load4 into a sentence naming the directory to rebuild. A
# warning does that. When the mismatch is real the link fails immediately
# afterwards, now explained; when the module heals itself the warning is
# spurious and nothing is lost.
# Not while cleaning: the check would refuse to let you fix the state it is
# objecting to. Everything else, including a bare `make`, is checked.
STAMP_CLEAN_ONLY := $(if $(MAKECMDGOALS),$(if $(filter-out clean clean-all distclean,$(MAKECMDGOALS)),,yes),)

ifneq ($(STAMP_CLEAN_ONLY),yes)
STAMP_CONFLICTS := $(strip $(foreach d,$(STAMP_REQUIRE_DIRS),$(shell sh $(STAMP_COMPAT) $(d) $(notdir $(BUILD_STAMP)) $(STAMP_FAMILY) $(STAMP_SAN))))
ifneq ($(STAMP_CONFLICTS),)
$(warning $(STAMP_CONFLICTS) The link will fail or misbehave; rebuild it: make -C <dir> clean lib)
endif
endif
