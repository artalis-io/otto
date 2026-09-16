# shellcheck shell=bash
#
# Guard against MSYS2 argument path conversion being switched off.
#
# scripts/msvc-env.sh exports MSYS2_ARG_CONV_EXCL='*' so cl.exe receives its
# /-prefixed options (/Fo:, /std:c11) unmangled. That switch is global rather
# than scoped to the compiler: it also stops MSYS2 rewriting POSIX paths into
# Windows ones when handing arguments to any native program. A harness that
# passes a native binary an absolute path built from `pwd` -- /c/Users/... --
# is then handing it something Windows cannot open.
#
# This has been diagnosed the slow way twice. The NETLIB gate reported all 84
# problems as "Problem file not found" while the same binary solved them fine
# when given a relative path, and `curl -o /dev/null` exited 23 writing to a
# literal unwritable path. Neither failure mentions paths, and neither has
# anything to do with the compiler, so both cost a session to track down.
#
# Usage, from a script that hands absolute paths to a native binary:
#
#     . "$SCRIPT_DIR/../../scripts/msys-path-guard.sh"
#     otto_guard_msys_path_conv "The NETLIB gate"
#
# Set OTTO_SKIP_PATH_CONV_GUARD=1 to proceed anyway.

otto_guard_msys_path_conv() {
    what="${1:-This script}"

    [ -n "${OTTO_SKIP_PATH_CONV_GUARD:-}" ] && return 0
    [ -z "${MSYS2_ARG_CONV_EXCL:-}" ] && return 0

    # Only MSYS2/Cygwin shells convert arguments in the first place; everywhere
    # else the variable is inert and means nothing.
    case "$(uname -s 2>/dev/null)" in
        MSYS*|MINGW*|CYGWIN*) ;;
        *) return 0 ;;
    esac

    # '*' excludes every argument, i.e. conversion is off entirely. A narrower
    # exclusion list is someone deliberately exempting specific arguments, and
    # is theirs to get right.
    case "$MSYS2_ARG_CONV_EXCL" in
        '*') ;;
        *) return 0 ;;
    esac

    cat >&2 <<EOF
ERROR: MSYS2_ARG_CONV_EXCL='*' is set in this shell, which switches off
       MSYS2's POSIX-to-Windows path conversion for every native program it
       launches.

       $what
       hands absolute POSIX paths (/c/...) to a native binary, and Windows
       cannot open those. Left to run, it would fail on every single input
       with a "file not found" that says nothing about paths.

       This is almost certainly because scripts/msvc-env.sh was sourced here.
       Sourcing it is correct for compiling and linking and wrong for running
       a harness: build in one step, run in a step that does not source it.
       Running needs no compiler.

       Set OTTO_SKIP_PATH_CONV_GUARD=1 to proceed anyway.
EOF
    return 1
}
