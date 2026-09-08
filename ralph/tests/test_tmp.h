/*
 * test_tmp.h - temp-file paths for Ralph's tests.
 *
 * These tests used to write to literal "/tmp/..." paths. That is not a POSIX
 * detail that Windows merely spells differently: a mingw-built test binary is
 * a native Windows program, so it reads "/tmp/x" as "C:\tmp\x" -- a directory
 * that need not exist, and normally does not.
 *
 * On CI it did exist, because shared/tests/test_fs.c created it earlier in the
 * same job as a side effect of its own mkdirs test. Splitting the Windows job
 * removed that accident and three tests in test_lp.c went red, which is the
 * correct outcome: a suite should not depend on a directory another suite
 * happens to create.
 *
 * sh_pal_temp_dir() answers $TMPDIR or /tmp on POSIX and GetTempPathA on
 * Windows, so these helpers work the same on both.
 */
#ifndef RALPH_TEST_TMP_H
#define RALPH_TEST_TMP_H

#include <stdio.h>
#include <string.h>

#include "sh_pal.h"

/*
 * Write "<temp dir>/<name>" into `buf`. Returns `buf` on success and NULL if
 * the temp directory could not be resolved or the result would not fit --
 * both of which a caller should treat as a failed test rather than ignore.
 */
static inline const char *ralph_tmp_path(char *buf, size_t len,
                                         const char *name)
{
    char dir[260];
    int n;

    if (!buf || len == 0 || !name) return NULL;
    if (sh_pal_temp_dir(dir, sizeof(dir)) != 0) { buf[0] = '\0'; return NULL; }

    n = snprintf(buf, len, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= len) { buf[0] = '\0'; return NULL; }
    return buf;
}

#endif /* RALPH_TEST_TMP_H */
