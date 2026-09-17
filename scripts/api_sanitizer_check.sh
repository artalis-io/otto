# shellcheck shell=sh
#
# Make sanitizer findings in an API server fatal to its test script.
#
# Sourced by the six api/test_api.sh scripts. It exists because running those
# scripts against a sanitizer build does not, on its own, test anything.
#
# The scripts drive the server over HTTP and judge it by what curl gets back.
# A sanitizer diagnostic goes somewhere else entirely -- the server's stderr --
# and the default for UBSan is to print and keep going. So the server reports
# undefined behaviour, serves a perfectly good response anyway, curl is happy,
# every check passes, and the script exits 0. Two of the six sent that stderr
# to /dev/null; the other four kept a log they printed only when a check had
# already failed, and none of the six ever looked in it.
#
# Adding a sanitizer build without this would have produced a green CI job that
# could not go red, which is worse than no job at all.
#
# Two halves:
#   1. the options below make a finding kill the server, so the checks notice
#   2. api_sanitizer_assert() greps the log at the end, which catches anything
#      that printed without aborting, and anything that happened after the
#      last request

# One log for the whole script run, appended to, so servers that restart
# between test groups (fuelwise, surge) all land in the same place.
API_SAN_LOG="${API_SAN_LOG:-${TMPDIR:-/tmp}/otto_api_server_$$.log}"
export API_SAN_LOG
: > "$API_SAN_LOG"

# abort_on_error so ASan takes the process down rather than trying to continue;
# halt_on_error so UBSan does the same instead of printing and carrying on.
#
# detect_leaks is off deliberately. These scripts stop the server with a signal,
# so everything still reachable at that moment is reported as leaked -- the
# server's routing table, its arenas, the graph it loaded. That is not a finding,
# it is what a kill looks like, and leaving it on would bury the real output.
ASAN_OPTIONS="abort_on_error=1:detect_leaks=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1${UBSAN_OPTIONS:+:$UBSAN_OPTIONS}"
export ASAN_OPTIONS UBSAN_OPTIONS

# Call once at the end, before the script decides its exit status.
# Returns non-zero if the server logged anything a sanitizer produced.
api_sanitizer_assert() {
    _pat='AddressSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|runtime error:|SUMMARY: .*Sanitizer'
    if [ -s "$API_SAN_LOG" ] && grep -qE "$_pat" "$API_SAN_LOG" 2>/dev/null; then
        echo ""
        echo "========================================"
        echo "  SANITIZER FINDING in the server"
        echo "========================================"
        grep -nE "$_pat" "$API_SAN_LOG" | head -20
        echo ""
        echo "--- server log ---"
        tail -60 "$API_SAN_LOG"
        return 1
    fi
    return 0
}

# Where a script should send the server's output. Using this rather than
# /dev/null or a hardcoded path is what lets the check above see anything.
api_sanitizer_log() {
    printf '%s' "$API_SAN_LOG"
}
