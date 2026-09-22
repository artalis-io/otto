#!/bin/sh
# Is a linked module's build stamp compatible with ours?
#
# Called from mk/flagstamp.mk once per entry in STAMP_REQUIRE_DIRS. It lives in
# a script rather than inside $(shell ...) because the check needs a case
# statement and command substitution, and make counts parentheses inside its own
# functions -- `cl.exe)` silently closes $(shell early, which is a confusing way
# to lose an afternoon.
#
# Prints one line if the dependency is incompatible; nothing if it is fine, or
# has not been built yet. Never fails: make decides what to do with the output.
#
#   $1  dependency directory      $3  our toolchain family (gnu|msvc)
#   $2  stamp filename            $4  whether we are sanitized (yes|no)
set -u
stamp="$1/$2"
[ -f "$stamp" ] || exit 0

cc=$(awk '{print $1; exit}' "$stamp")
case "${cc##*/}" in
    cl|cl.exe) fam=msvc ;;
    *)         fam=gnu ;;
esac

san=no
grep -q -- -fsanitize "$stamp" && san=yes

# Only two properties, deliberately not the whole flag set. Optimisation and
# warning flags differ between modules by design; comparing the compiler string
# itself would fail `cc` against `gcc`, which are one toolchain.
#
# The sanitizer comparison is asymmetric on purpose. A sanitized module linking
# a plain library is normal and works -- every module test-asan does it, because
# the link line carries the runtime. The reverse does not link at all.
if [ "$fam" != "$3" ]; then
    echo "toolchain-mismatch: $1 was built with $fam, this module is $3."
elif [ "$san" = yes ] && [ "$4" = no ]; then
    echo "toolchain-mismatch: $1 is sanitized, this module is not."
fi
exit 0
