# OTTO toolchain abstraction
#
# The one place in the tree where compilers differ. Every module Makefile
# includes this and composes its flags from the normalized variables below;
# nothing else should ever test which compiler is in use.
#
#     make              GCC or Clang, exactly as before
#     make CC=cl        MSVC, from a Visual Studio Developer shell
#
# The GNU branch reproduces the flags the module Makefiles carried inline
# before this file existed, so `make -n` output is unchanged there. Anything
# added here for MSVC is a deliberate, documented mapping -- flags are not
# translated where the semantics do not survive the trip.

# ---------------------------------------------------------------------------
# Toolchain detection
#
# Matched on the exact program name, never a substring: "clang" contains "cl".
# ---------------------------------------------------------------------------
CC ?= gcc
CC_NAME := $(notdir $(CC))

ifeq ($(CC_NAME),cl)
  TOOLCHAIN := msvc
else ifeq ($(CC_NAME),cl.exe)
  TOOLCHAIN := msvc
else
  TOOLCHAIN := gnu
endif

UNAME_S := $(shell uname -s)

# A caution for module Makefiles: never name a variable LIB, INCLUDE, LINK or
# CL. Those are MSVC's environment contract, and make re-exports any variable
# that also exists in the environment -- so a makefile LIB silently replaces
# the linker's search path for every recipe.

# A trailing space that survives make's variable stripping, for the compilers
# whose output flag takes one.
EMPTY :=
SPACE := $(EMPTY) $(EMPTY)

ifeq ($(TOOLCHAIN),msvc)

# ---------------------------------------------------------------------------
# MSVC
# ---------------------------------------------------------------------------

# /experimental:c11atomics is how MSVC exposes C11 <stdatomic.h>; without it
# the header itself hard-errors. sh_workqueue.c, ralph/detect.c and ralph/lap.c
# need it. Microsoft ships no stable alternative, so the flag is here, named,
# rather than scattered.
CC_STD    := /std:c11 /experimental:c11atomics
# For modules that pin no standard on GCC because they rely on GNU
# extensions in the compiler's default mode. MSVC has no such default,
# so the standard has to be named explicitly there and nowhere else.
CC_STD_BASELINE := /std:c11 /experimental:c11atomics
# For modules that ask for C99 on GCC. MSVC has no C99 mode at all -- its
# choices are C11 and C17 -- so this is the nearest thing that exists rather
# than a translation. Nothing that uses it depends on C99-specific semantics.
CC_STD_C99 := /std:c11
# For modules that deliberately build at -O2 rather than the -O3 default.
# /O2 is MSVC's optimise-for-speed setting and is what CC_OPT uses too, so on
# this compiler the distinction does not arise.
CC_OPT_O2 := /O2
CC_WARN   := /W3
# Deliberately empty. MSVC /W3 diagnoses things GCC does not -- signed/unsigned
# narrowing and size_t truncation most of all -- so /WX would not mean "the same
# bar as -Werror", it would mean "a second, different bar". Raising that bar
# is a per-module warning-cleanup job, not a toolchain one.
CC_WERROR :=

# Treating a header as a system header, so its warnings do not count against
# the including module. /external:W0 needs /external:I to take effect.
CC_SYSINC := /external:W0 /external:I$(SPACE)
CC_OPT    := /O2

# -march=native has no portable MSVC equivalent. /arch:AVX2 is not the same
# promise -- it is a hard floor that produces binaries the target may not be
# able to run -- so nothing is emitted rather than something different.
CC_ARCH   :=

# Floating point. FP_MODE=precise is the default and the supported setting;
# `make CC=cl FP_MODE=fast` exists so the two can be compared on real solver
# output. See docs/roadmaps/infrastructure.md for the evidence behind this.
#
# Note this is NOT a translation of the GNU side. GCC builds Ralph with
# -ffast-math -fno-finite-math-only: aggressive FP, NaN and Inf still honoured.
# MSVC's /fp:fast has no such carve-out -- it assumes NaN and Inf do not occur,
# which in a simplex is precisely the assumption that fails.
FP_MODE ?= precise
ifeq ($(FP_MODE),fast)
  CC_FP_MODE := /fp:fast
else
  CC_FP_MODE := /fp:precise
endif

# The GNU spellings have no MSVC counterpart beyond the mode above: /fp: is the
# whole dial, so asking for fast math and for finite-math to stay on cannot be
# expressed separately. Nothing is emitted rather than something different.
CC_FP_FASTMATH       :=
CC_FP_KEEP_NONFINITE :=

# MSVC auto-vectorises at /O2; there is no separate switch to ask for it.
CC_VECTORIZE :=

# M_PI: MSVC gates the math constants behind _USE_MATH_DEFINES, where glibc
# exposes them under _GNU_SOURCE. Same intent, different spelling.
# _CRT_SECURE_NO_WARNINGS: MSVC deprecates snprintf/strncpy in favour of its
# own _s variants. OTTO uses the standard ones deliberately.
# _GNU_SOURCE has no MSVC counterpart; the modules that pass CC_DEFS get nothing.
CC_DEFS   :=

# Needed by every module, whether or not it pins _GNU_SOURCE on GNU.
# _CRT_NONSTDC_NO_WARNINGS: <io.h> declares the POSIX spellings (close, read,
# unlink) and then deprecates them in favour of the underscore forms. The
# POSIX spellings are what the sources use and what every other platform has.
CC_PORT_DEFS := /D_USE_MATH_DEFINES /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_WARNINGS

# POSIX names MSVC spells differently. Each is a straight rename with identical
# semantics and argument order -- not a reimplementation:
#
#   strcasecmp  -> _stricmp
#   strncasecmp -> _strnicmp
#   strtok_r    -> strtok_s
#
# Done here rather than with an #include in each of the ~7 files that call them,
# so a build concern stays in the build layer and the sources stay POSIX-spelled.
# Without these MSVC treats the calls as implicit declarations returning int,
# which truncates strtok_r's pointer on a 64-bit build -- a crash, not a warning.
CC_PORT_DEFS += /Dstrcasecmp=_stricmp /Dstrncasecmp=_strnicmp /Dstrtok_r=strtok_s

# popen/pclose are the same functions under MSVC, spelled with a leading
# underscore because they are not ISO C.
CC_PORT_DEFS += /Dpopen=_popen /Dpclose=_pclose

# /GS is MSVC's stack cookie (the -fstack-protector-strong analogue) and
# /guard:cf its control-flow guard. There is no _FORTIFY_SOURCE equivalent and
# ASLR (/DYNAMICBASE) is already the linker default, so -fPIE has no counterpart
# to emit.
# Position-independent code. Windows binaries are relocatable by construction,
# so there is nothing to ask for.
CC_PIE    :=
CC_HARDEN := /GS /guard:cf

# OpenMP is deliberately OFF for MSVC.
#
# 29 of Ralph's 33 pragmas are `omp simd`, which is OpenMP 4.0. MSVC's /openmp
# is 2.0 and has no simd construct; /openmp:llvm rejects a loop index declared
# in the for-init in C. Enabling either would mean rewriting loops to suit a
# compiler rather than the algorithm.
#
# With OpenMP off, MSVC ignores the simd pragmas silently -- verified: zero
# warnings, identical results -- and auto-vectorises under /O2 anyway. The three
# genuine `parallel` constructs in lap.c degrade to serial, which is a defined
# OpenMP property and is already covered by Ralph's "parallel disabled" tests.
CC_OMP      :=
CC_OMP_SIMD :=
LD_OMP      :=

CC_DEBUG_OPT := /Zi /Od
CC_SANITIZE  :=
LD_SANITIZE  :=

LD_MATH   :=
LD_THREAD :=
LD_PIE    :=
LD_RELRO  :=
# bcrypt: sh_pal_random_bytes. ws2_32: the StatsD sink in sh_metrics.
LD_PLATFORM := bcrypt.lib ws2_32.lib

# MSVC gives an executable a 1 MB stack; Linux gives 8 MB and MinGW 2 MB. Code
# written against the larger default overflows on entry, before its first
# statement runs, so it dies with no output at all and an exit code that says
# nothing -- ralph-benchmark holds a 200-entry array of 4 KB paths, ~840 KB, in
# one frame. Match the Linux default rather than leave that trap set.
LD_STACK  := /F8388608

# cl accepts -c, -I and -D, but its output flags are unlike anyone else's and
# -o is deprecated (D9035). These carry the whole difference.
# Header dependency tracking. MSVC's /showIncludes emits a different format
# that would need a parser to turn into make rules; not worth it for this
# pass, so MSVC builds simply do not track headers. `make clean` after a
# header change is the workaround, and CI always builds clean.
CC_DEPFLAGS :=

OBJ_OUT := /Fo:
EXE_OUT := /Fe:
AR_CMD  := lib /nologo /OUT:

# $(call link_lib,<dir>,<name>) -- naming a static library on the link line.
# MSVC has no -L/-l: it takes the archive as a plain input file, and reads the
# lib*.a that lib.exe produced without needing a .lib extension.
link_lib = $(1)/lib$(2).a

else

# ---------------------------------------------------------------------------
# GCC / Clang -- byte-for-byte what the module Makefiles used to spell inline
# ---------------------------------------------------------------------------

CC_STD    := -std=c11
CC_STD_BASELINE :=
CC_STD_C99 := -std=c99
CC_OPT_O2 := -O2
CC_WARN   := -Wall -Wextra
CC_WERROR := -Werror
CC_SYSINC := -isystem$(SPACE)
CC_OPT    := -O3
CC_ARCH   := -march=native
# Two knobs, not one: Ralph wants both, Velo wants only the first.
CC_FP_MODE           :=
CC_FP_FASTMATH       := -ffast-math
CC_FP_KEEP_NONFINITE := -fno-finite-math-only
CC_VECTORIZE         := -ftree-vectorize
CC_DEFS   := -D_GNU_SOURCE
CC_PORT_DEFS :=
CC_PIE    := -fPIE
CC_HARDEN := -fstack-protector-strong -D_FORTIFY_SOURCE=2 $(CC_PIE) -fno-common

CC_DEBUG_OPT := -g -O0
CC_SANITIZE  := -fsanitize=address,undefined -fno-omit-frame-pointer
LD_SANITIZE  := -fsanitize=address,undefined

LD_MATH   := -lm
LD_THREAD := -lpthread
LD_STACK  :=

ifeq ($(UNAME_S),Darwin)
  # macOS: Homebrew libomp with clang. -pie is implicit here.
  OMP_PREFIX := $(shell brew --prefix libomp 2>/dev/null || echo "/opt/homebrew/opt/libomp")
  CC_OMP      := -Xclang -fopenmp -I$(OMP_PREFIX)/include
  CC_OMP_SIMD :=
  LD_OMP      := -L$(OMP_PREFIX)/lib -lomp
  LD_PIE    :=
  LD_RELRO  :=
  LD_PLATFORM :=
else ifneq (,$(findstring MINGW,$(UNAME_S)))
  # Windows/MinGW: -z relro/now are ELF-only and MinGW ld rejects them outright.
  # bcrypt and ws2_32 come from libshared's use of sh_pal.
  CC_OMP      := -fopenmp
  # Velo has always passed this on Windows too -- its old Makefile keyed the
  # SIMD half on "not macOS", not on Linux specifically.
  CC_OMP_SIMD := -fopenmp-simd
  LD_OMP      := -fopenmp
  LD_PIE    :=
  LD_RELRO  :=
  LD_PLATFORM := -lbcrypt -lws2_32
else
  CC_OMP      := -fopenmp
  # Velo asks for the SIMD half explicitly; Ralph does not.
  CC_OMP_SIMD := -fopenmp-simd
  LD_OMP      := -fopenmp
  # shared links with -pie; ralph historically did not. Kept as two knobs
  # so each module reproduces exactly what it linked with before.
  LD_PIE    := -pie
  LD_RELRO  := -Wl,-z,relro,-z,now
  LD_PLATFORM :=
endif

CC_DEPFLAGS := -MMD -MP

OBJ_OUT := -o$(SPACE)
EXE_OUT := -o$(SPACE)
AR_CMD  := ar rcs$(SPACE)

link_lib = -L$(1) -l$(2)

endif
