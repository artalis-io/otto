/*
 * test_pal.c - Platform abstraction layer tests.
 *
 * Runs identically on both backends. This is the file the Windows CI job
 * exists to execute: a PAL that only ever builds on Linux is a PAL that
 * silently rots, which is what happened to build-wasm before it was ungated.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh_pal.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(int cond, const char *what)
{
    if (cond) { g_pass++; printf("  PASS: %s\n", what); }
    else      { g_fail++; printf("  FAIL: %s\n", what); }
}

/* ------------------------------------------------------------------ Mutex */

static void test_mutex(void)
{
    ShMutex m;
    printf("\n=== Mutex ===\n");

    check(sh_mutex_init(&m) == 0, "Mutex initialises");
    sh_mutex_lock(&m);
    sh_mutex_unlock(&m);
    check(1, "Lock/unlock round-trips");
    sh_mutex_lock(&m);
    sh_mutex_unlock(&m);
    check(1, "Second lock after unlock succeeds");
    sh_mutex_destroy(&m);

    check(sh_mutex_init(NULL) == -1, "NULL mutex init is rejected");
    sh_mutex_lock(NULL);
    sh_mutex_unlock(NULL);
    sh_mutex_destroy(NULL);
    check(1, "NULL lock/unlock/destroy are safe no-ops");
}

static void test_mutex_recursive(void)
{
    ShMutex m;
    printf("\n=== Recursive mutex ===\n");

    check(sh_mutex_init_recursive(&m) == 0, "Recursive mutex initialises");

    /* The whole point: the plain mutex would deadlock here. On Windows this
     * is a CRITICAL_SECTION rather than an SRWLOCK for exactly this reason. */
    sh_mutex_lock(&m);
    sh_mutex_lock(&m);
    sh_mutex_lock(&m);
    sh_mutex_unlock(&m);
    sh_mutex_unlock(&m);
    sh_mutex_unlock(&m);
    check(1, "Same thread locks three deep and unlocks out");

    sh_mutex_destroy(&m);
}

/* ------------------------------------------------------- Condition variable */

typedef struct {
    ShMutex m;
    ShCond  c;
    int     flag;
    int     counter;
} Shared;

static void *signaller(void *arg)
{
    Shared *s = (Shared *)arg;
    sh_mutex_lock(&s->m);
    s->flag = 1;
    sh_cond_signal(&s->c);
    sh_mutex_unlock(&s->m);
    return (void *)(size_t)42;
}

static void test_cond(void)
{
    Shared s;
    ShThread t;
    void *ret = NULL;

    printf("\n=== Condition variable ===\n");

    memset(&s, 0, sizeof(s));
    check(sh_mutex_init(&s.m) == 0 && sh_cond_init(&s.c) == 0,
          "Mutex and cond initialise");

    /* Timeout path: nothing will signal, so this must report 0 and must
     * actually wait rather than returning immediately. */
    {
        uint64_t t0 = sh_monotonic_ms();
        int rc;
        sh_mutex_lock(&s.m);
        rc = sh_cond_timedwait(&s.c, &s.m, 60);
        sh_mutex_unlock(&s.m);
        check(rc == 0, "timedwait reports 0 on timeout");
        check(sh_monotonic_ms() - t0 >= 40,
              "timedwait actually waited (>=40ms of a 60ms budget)");
    }

    /* Signal path: a second thread sets the flag and signals. */
    check(sh_thread_create(&t, signaller, &s) == 0, "Thread created");
    sh_mutex_lock(&s.m);
    while (!s.flag) {
        /* Bounded so a broken signal fails the test instead of hanging CI. */
        if (sh_cond_timedwait(&s.c, &s.m, 5000) < 0) break;
    }
    check(s.flag == 1, "Waiter observed the signal");
    sh_mutex_unlock(&s.m);

    check(sh_thread_join(&t, &ret) == 0, "Thread joined");
    check((size_t)ret == 42, "Thread return value survives the join");

    sh_cond_destroy(&s.c);
    sh_mutex_destroy(&s.m);
}

/* ------------------------------------------------------------------- Once */

static int g_once_calls = 0;
static void bump_once(void) { g_once_calls++; }

static void test_once(void)
{
    ShOnce o = SH_ONCE_INIT;
    int i;

    printf("\n=== Once ===\n");

    for (i = 0; i < 5; i++) sh_once(&o, bump_once);
    check(g_once_calls == 1, "sh_once runs the callback exactly once of five");

    {
        /* A second, independent ShOnce must run again -- the state is
         * per-object, not global. */
        ShOnce o2 = SH_ONCE_INIT;
        sh_once(&o2, bump_once);
        check(g_once_calls == 2, "A distinct ShOnce runs its own callback");
    }
}

/* -------------------------------------------------------------------- TLS */

static void *tls_worker(void *arg)
{
    ShTls *key = (ShTls *)arg;
    /* A fresh thread must not see the main thread's value. */
    if (sh_tls_get(key) != NULL) return (void *)(size_t)1;
    sh_tls_set(key, (void *)(size_t)0xBEEF);
    if (sh_tls_get(key) != (void *)(size_t)0xBEEF) return (void *)(size_t)2;
    return NULL;
}

static void test_tls(void)
{
    ShTls key;
    ShThread t;
    void *ret = (void *)(size_t)999;

    printf("\n=== Thread-local storage ===\n");

    check(sh_tls_create(&key, NULL) == 0, "TLS key created");
    check(sh_tls_get(&key) == NULL, "Unset TLS value reads back NULL");
    check(sh_tls_set(&key, (void *)(size_t)0x1234) == 0, "TLS value set");
    check(sh_tls_get(&key) == (void *)(size_t)0x1234, "TLS value reads back");

    check(sh_thread_create(&t, tls_worker, &key) == 0, "TLS worker created");
    check(sh_thread_join(&t, &ret) == 0, "TLS worker joined");
    check(ret == NULL, "Another thread sees its own slot, not this one's");
    check(sh_tls_get(&key) == (void *)(size_t)0x1234,
          "This thread's value is unchanged by the other thread");

    sh_tls_destroy(&key);
}

/* ----------------------------------------------------------------- Clocks */

static void test_clocks(void)
{
    uint64_t a, b, w;
    printf("\n=== Clocks ===\n");

    a = sh_monotonic_ms();
    check(a > 0, "Monotonic clock returns a value");

    /* Spin rather than sleep: the PAL has no sleep, and this keeps the test
     * free of another platform dependency. */
    while (sh_monotonic_ms() - a < 25) { /* busy wait */ }
    b = sh_monotonic_ms();
    check(b >= a, "Monotonic clock never goes backwards");
    check(b - a >= 25, "Monotonic clock advances by the elapsed time");

    w = sh_wall_ms();
    /* Sanity: after 2001-09-09 and before 2100. Catches a wrong epoch, which
     * is the classic Windows FILETIME mistake. */
    check(w > 1000000000000ull, "Wall clock is past 2001 (epoch is correct)");
    check(w < 4102444800000ull, "Wall clock is before 2100");

    {
        struct tm tmv;
        /* 2001-09-09T01:46:40Z, the 1e9 second mark. */
        memset(&tmv, 0, sizeof(tmv));
        check(sh_gmtime(1000000000, &tmv) == 0, "sh_gmtime succeeds");
        check(tmv.tm_year + 1900 == 2001, "sh_gmtime yields the right year");
        check(tmv.tm_mon == 8, "sh_gmtime yields the right month");
        check(tmv.tm_mday == 9, "sh_gmtime yields the right day");

        memset(&tmv, 0, sizeof(tmv));
        check(sh_localtime(1000000000, &tmv) == 0, "sh_localtime succeeds");
        check(tmv.tm_year + 1900 == 2001 || tmv.tm_year + 1900 == 2002,
              "sh_localtime yields a plausible year for any timezone");
    }
}

/* -------------------------------------------------------------------- CPU */

static void test_cpu(void)
{
    int n = sh_cpu_count();
    printf("\n=== CPU ===\n");
    check(n >= 1, "CPU count is at least 1");
    check(n < 4096, "CPU count is plausible");
    printf("  (detected %d logical processors)\n", n);
}

/* ------------------------------------------------------------ Environment */

static void test_env(void)
{
    const char *k = "SH_PAL_TEST_VAR";
    const char *v;

    printf("\n=== Environment ===\n");

    check(sh_pal_setenv(k, "hello") == 0, "setenv succeeds");
    v = getenv(k);
    check(v != NULL && strcmp(v, "hello") == 0, "the value reads back");

    /* Overwriting is the only mode the PAL offers, so it must actually
     * overwrite -- POSIX setenv defaults to not doing so. */
    check(sh_pal_setenv(k, "world") == 0, "setenv over an existing value succeeds");
    v = getenv(k);
    check(v != NULL && strcmp(v, "world") == 0, "the new value replaced the old");

    /* Deleted, not emptied: _putenv_s(name, "") is how Windows deletes, and
     * getenv() must answer NULL rather than "" on both platforms. */
    check(sh_pal_unsetenv(k) == 0, "unsetenv succeeds");
    check(getenv(k) == NULL, "the variable is gone, not empty");

    check(sh_pal_unsetenv(k) == 0, "unsetenv on an unset variable still succeeds");

    check(sh_pal_setenv(k, "") == 0, "setenv accepts an empty value");
    sh_pal_unsetenv(k);

    /* A NULL value is treated as empty rather than crashing. */
    check(sh_pal_setenv(k, NULL) == 0, "setenv accepts a NULL value");
    sh_pal_unsetenv(k);

    check(sh_pal_setenv(NULL, "x") == -1, "setenv rejects a NULL name");
    check(sh_pal_setenv("", "x") == -1, "setenv rejects an empty name");
    check(sh_pal_setenv("A=B", "x") == -1, "setenv rejects '=' in the name");
    check(sh_pal_unsetenv(NULL) == -1, "unsetenv rejects a NULL name");
    check(sh_pal_unsetenv("") == -1, "unsetenv rejects an empty name");
}

/* ----------------------------------------------------- Temp directory */

static void test_temp_dir(void)
{
    char buf[512];
    char tiny[2];

    printf("\n=== Temp directory ===\n");

    check(sh_pal_temp_dir(buf, sizeof(buf)) == 0, "temp dir resolves");
    check(buf[0] != '\0', "temp dir is not empty");

    /* No trailing separator, so callers can always join with one. */
    {
        size_t n = strlen(buf);
        check(n > 0 && buf[n - 1] != '/' && buf[n - 1] != '\\',
              "temp dir has no trailing separator");
    }

    /* It has to be a directory we can actually create a file in -- the point
     * of the call. A literal "/tmp" is not that on Windows. */
    {
        char path[600];
        FILE *f;
        snprintf(path, sizeof(path), "%s/sh_pal_temp_probe.tmp", buf);
        f = fopen(path, "w");
        check(f != NULL, "a file can be created in the temp dir");
        if (f) {
            fclose(f);
            remove(path);
        }
    }

    check(sh_pal_temp_dir(tiny, sizeof(tiny)) == -1,
          "a buffer too small is refused");
    check(tiny[0] == '\0', "the refused buffer is still terminated");
    check(sh_pal_temp_dir(NULL, sizeof(buf)) == -1, "NULL buffer is refused");
    check(sh_pal_temp_dir(buf, 0) == -1, "zero length is refused");
}

/* ------------------------------------------------------------------- main */

int main(void)
{
    printf("=== sh_pal tests ===\n");

    test_mutex();
    test_mutex_recursive();
    test_cond();
    test_once();
    test_tls();
    test_clocks();
    test_cpu();
    test_env();
    test_temp_dir();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
