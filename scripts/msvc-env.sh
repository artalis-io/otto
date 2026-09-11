# msvc-env.sh - put an MSVC toolchain into an MSYS2 bash session.
#
#   source scripts/msvc-env.sh
#   make -C ralph CC=cl test
#
# OTTO builds with MSVC through the same Make graph as everything else (see
# mk/toolchain.mk). What that needs is cl.exe, link.exe and lib.exe on PATH plus
# the INCLUDE/LIB/LIBPATH that vcvars64.bat sets. MSYS2 makes it awkward in two
# specific ways, and both have cost real debugging time:
#
#   1. MSYS2's /usr/bin ships its own `link.exe` (a coreutils program). vcvars
#      inherits the calling PATH and prepends to it, so simply adopting the
#      environment it produces still leaves /usr/bin ahead of the linker. The
#      VC tools directory is therefore put in front explicitly, not merged.
#
#   2. MSYS rewrites anything that looks like a POSIX path when handing
#      arguments to a native binary, mangling `/Fo:`, `/std:c11` and every other
#      MSVC flag. MSYS2_ARG_CONV_EXCL turns that off.
#
# Make still needs MSYS's own tools -- the Makefiles call `uname` and `rm`, and
# make wants an `sh` for recipes -- so /usr/bin stays on PATH, just behind.
#
# Safe to source repeatedly; the work is skipped once it has succeeded.

_msvc_env_setup() {
    local vswhere vsroot vcvars dump line key toolsdir

    if [ -n "${OTTO_MSVC_ENV_READY:-}" ]; then
        return 0
    fi

    vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    if [ ! -x "$vswhere" ]; then
        echo "msvc-env.sh: vswhere.exe not found; is Visual Studio installed?" >&2
        return 1
    fi

    vsroot=$("$vswhere" -latest -products '*' \
                 -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
                 -property installationPath 2>/dev/null | tr -d '\r')
    if [ -z "$vsroot" ]; then
        echo "msvc-env.sh: no Visual Studio install with the x64 C++ tools." >&2
        return 1
    fi

    vcvars="$(cygpath -u "$vsroot")/VC/Auxiliary/Build/vcvars64.bat"
    if [ ! -f "$vcvars" ]; then
        echo "msvc-env.sh: vcvars64.bat missing under $vsroot" >&2
        return 1
    fi

    # Capture the environment vcvars produces, via a throwaway batch file so
    # there is no nested quoting to get wrong.
    #
    # Note `cmd.exe /c` with MSYS2_ARG_CONV_EXCL rather than the usual `cmd //c`
    # idiom: //c only collapses to /c when MSYS path conversion is on, and this
    # function needs it off. Left as //c with conversion disabled, cmd never
    # sees a switch at all and blocks waiting for input.
    local tmpbat="${TMPDIR:-/tmp}/otto-vcvars-$$.bat"
    {
        echo '@echo off'
        echo "call \"$(cygpath -w "$vcvars")\" >nul 2>&1"
        echo 'set'
    } > "$tmpbat"
    dump=$(MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' \
               cmd.exe /c "$(cygpath -w "$tmpbat")" 2>/dev/null | tr -d '\r')
    rm -f "$tmpbat"
    if [ -z "$dump" ]; then
        echo "msvc-env.sh: vcvars64.bat produced no environment." >&2
        return 1
    fi

    while IFS= read -r line; do
        key="${line%%=*}"
        case "$key" in
            INCLUDE|LIB|LIBPATH|VCToolsInstallDir)
                export "$key=${line#*=}" ;;
        esac
    done <<EOF
$dump
EOF

    if [ -z "${VCToolsInstallDir:-}" ] || [ -z "${INCLUDE:-}" ]; then
        echo "msvc-env.sh: vcvars did not set VCToolsInstallDir/INCLUDE." >&2
        return 1
    fi

    # Explicitly in front of everything, so MSYS's link.exe cannot win.
    toolsdir="$(cygpath -u "$VCToolsInstallDir")"
    toolsdir="${toolsdir%/}/bin/HostX64/x64"
    if [ ! -x "$toolsdir/cl.exe" ]; then
        echo "msvc-env.sh: no cl.exe under $toolsdir" >&2
        return 1
    fi
    export PATH="$toolsdir:$PATH"

    export MSYS_NO_PATHCONV=1
    export MSYS2_ARG_CONV_EXCL='*'
    export OTTO_MSVC_ENV_READY=1
}

if _msvc_env_setup; then
    for _msvc_tool in cl link lib; do
        case "$(command -v "$_msvc_tool")" in
            /usr/bin/*|/bin/*|/mingw*/*)
                echo "msvc-env.sh: $_msvc_tool resolves to $(command -v "$_msvc_tool"), not MSVC's." >&2
                ;;
        esac
    done
    unset _msvc_tool
else
    echo "msvc-env.sh: setup failed; CC=cl builds will not work." >&2
fi
