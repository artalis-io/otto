/*
 * Tests for generic out-of-process external runner utilities.
 *
 * The child processes these tests spawn are this same binary, re-executed with
 * a flag. The alternative -- writing a "#!/bin/sh" script to a temp file, which
 * is what this file used to do -- cannot work on Windows: CreateProcess has no
 * shebang handling, so such a file is not runnable at all, and going through
 * cmd.exe instead would mostly test batch-file quoting rather than the runner.
 *
 * Re-exec gives both platforms identical child behaviour, so the POSIX and
 * Windows runners are held to the same expectations by the same assertions,
 * and it drops the dependency on a shell, on chmod, and on `sleep` timing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
  /* MSVC has no <unistd.h>; <io.h> declares the same POSIX I/O names. */
  #include <io.h>
#else
  #include <unistd.h>
#endif
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

/* access() mode bits; MSVC spells the existence check as a bare 0. */
#ifndef F_OK
  #define F_OK 0
#endif
#include <sys/stat.h>

#include "test_tmp.h"

#include "../src/lp_external_oop.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT_TRUE(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s\n", msg); \
    } \
} while (0)

#define ASSERT_INT_EQ(a, b, msg) do { \
    tests_run++; \
    if ((a) == (b)) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%d != %d)\n", msg, (int)(a), (int)(b)); \
    } \
} while (0)

/* ---------------------------------------------------------------------------
 * Child modes
 * ------------------------------------------------------------------------ */

#define CHILD_FLAG_ECHO  "--child-echo"
#define CHILD_FLAG_SLEEP "--child-sleep"
#define CHILD_FLAG_TICK  "--child-tick"
#define CHILD_FLAG_ARGV  "--child-argv"

/* Long enough that the parent is still polling when it decides to cancel,
 * short enough that a missed kill shows up as a slow test rather than a
 * wedged one. */
#define CHILD_TICK_INTERVAL_MS 50
#define CHILD_SLEEP_MS 2000

static int run_child_mode(int argc, char **argv)
{
    const char *flag = argv[1];

    /* stdout is a pipe here, so it is block-buffered by default and the tick
     * child's output would not reach the parent until it exited -- which is
     * exactly what it never does. */
    setvbuf(stdout, NULL, _IONBF, 0);

    if (strcmp(flag, CHILD_FLAG_ARGV) == 0) {
        /* Echo what actually arrived, bracketed so that leading or trailing
         * whitespace in an argument is visible to the parent. */
        int i;
        for (i = 2; i < argc; i++) printf("[%s]\n", argv[i]);
        return 0;
    }
    if (strcmp(flag, CHILD_FLAG_ECHO) == 0) {
        printf("alpha\n");
        printf("beta\n");
        return 0;
    }
    if (strcmp(flag, CHILD_FLAG_SLEEP) == 0) {
        sh_sleep_ms(CHILD_SLEEP_MS);
        return 0;
    }
    if (strcmp(flag, CHILD_FLAG_TICK) == 0) {
        for (;;) {
            printf("tick\n");
            sh_sleep_ms(CHILD_TICK_INTERVAL_MS);
        }
    }
    fprintf(stderr, "unknown child mode: %s\n", flag);
    return 2;
}

/*
 * Path this binary can be re-executed by.
 *
 * On Windows the module path is authoritative. On POSIX argv[0] is what the
 * Makefile invoked ("./test_lp_external_oop_runner"), which execvp resolves
 * against the working directory the same way the shell just did.
 */
static int self_exe_path(char *buf, size_t size, const char *argv0)
{
#ifdef _WIN32
    (void)argv0;
    if (GetModuleFileNameA(NULL, buf, (DWORD)size) == 0) return -1;
    return 0;
#else
    if (!argv0 || argv0[0] == '\0') return -1;
    if (snprintf(buf, size, "%s", argv0) >= (int)size) return -1;
    return 0;
#endif
}

static char self_exe[1024];

/* ---------------------------------------------------------------------------
 * Callbacks
 * ------------------------------------------------------------------------ */

typedef struct {
    int lines;
    int cancel_on_line;
} RunnerLineState;

static int on_line_count(const char *line, void *user_data) {
    RunnerLineState *st = (RunnerLineState*)user_data;
    if (!st || !line) return 0;
    st->lines++;
    if (st->cancel_on_line > 0 && st->lines >= st->cancel_on_line) return 1;
    return 0;
}

#define COLLECT_MAX_LINES 16
#define COLLECT_MAX_LEN 256

typedef struct {
    char lines[COLLECT_MAX_LINES][COLLECT_MAX_LEN];
    int count;
} RunnerCollectState;

static int on_line_collect(const char *line, void *user_data) {
    RunnerCollectState *st = (RunnerCollectState*)user_data;
    if (!st || !line) return 0;
    if (st->count < COLLECT_MAX_LINES) {
        snprintf(st->lines[st->count], COLLECT_MAX_LEN, "%s", line);
        st->count++;
    }
    return 0;
}

typedef struct {
    int polls;
    int cancel_after;
} RunnerCancelState;

static int should_cancel_poll(void *user_data) {
    RunnerCancelState *st = (RunnerCancelState*)user_data;
    if (!st) return 0;
    st->polls++;
    return st->polls >= st->cancel_after ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------ */

static void test_tempfile_helpers(void) {
    char path[256];
    ASSERT_INT_EQ(lp_external_oop_make_tempfile("ralph_oop_tmp_", path, sizeof(path)), 0,
                  "tempfile: create");
    ASSERT_TRUE(access(path, F_OK) == 0,
                "tempfile: exists after create");
    lp_external_oop_cleanup_file(path);
    ASSERT_TRUE(access(path, F_OK) != 0,
                "tempfile: removed after cleanup");
}

static void test_runner_exit_and_line_capture(void) {
    char *argv[3];
    LPExternalOOPRunRequest req;
    LPExternalOOPRunResult res;
    RunnerLineState st;

    argv[0] = self_exe;
    argv[1] = (char *)CHILD_FLAG_ECHO;
    argv[2] = NULL;

    memset(&st, 0, sizeof(st));
    memset(&req, 0, sizeof(req));
    req.program = self_exe;
    req.argv = argv;
    req.poll_interval_ms = 5;
    req.on_line = on_line_count;
    req.line_user_data = &st;

    ASSERT_INT_EQ(lp_external_oop_run(&req, &res), 0,
                  "runner/lines: run succeeds");
    ASSERT_INT_EQ(res.exit_code, 0,
                  "runner/lines: exit code");
    ASSERT_INT_EQ(res.timed_out, 0,
                  "runner/lines: not timed out");
    ASSERT_INT_EQ(res.cancelled, 0,
                  "runner/lines: not cancelled");
    ASSERT_TRUE(st.lines >= 2,
                "runner/lines: line callback received output");
}

static void test_runner_timeout(void) {
    char *argv[3];
    LPExternalOOPRunRequest req;
    LPExternalOOPRunResult res;

    argv[0] = self_exe;
    argv[1] = (char *)CHILD_FLAG_SLEEP;
    argv[2] = NULL;

    memset(&req, 0, sizeof(req));
    req.program = self_exe;
    req.argv = argv;
    req.poll_interval_ms = 5;
    req.wall_time_limit_sec = 0.05;

    ASSERT_INT_EQ(lp_external_oop_run(&req, &res), 0,
                  "runner/timeout: run succeeds");
    ASSERT_INT_EQ(res.timed_out, 1,
                  "runner/timeout: timeout flag set");
    /* The point of the timeout is that it does not wait out the child. */
    ASSERT_TRUE(res.elapsed_ms < (double)CHILD_SLEEP_MS,
                "runner/timeout: returned before the child would have exited");
}

static void test_runner_cancel_callback_and_line_cancel(void) {
    char *argv[3];
    LPExternalOOPRunRequest req;
    LPExternalOOPRunResult res;
    RunnerCancelState cancel_state;
    RalphLPCancelCallback cb;
    RunnerLineState line_state;

    argv[0] = self_exe;
    argv[1] = (char *)CHILD_FLAG_TICK;
    argv[2] = NULL;

    memset(&cancel_state, 0, sizeof(cancel_state));
    cancel_state.cancel_after = 3;
    cb.should_cancel = should_cancel_poll;
    cb.user_data = &cancel_state;

    memset(&req, 0, sizeof(req));
    req.program = self_exe;
    req.argv = argv;
    req.poll_interval_ms = 5;
    req.cancel_cb = &cb;

    ASSERT_INT_EQ(lp_external_oop_run(&req, &res), 0,
                  "runner/cancel: run with poll-cancel succeeds");
    ASSERT_INT_EQ(res.cancelled, 1,
                  "runner/cancel: poll cancellation set");

    memset(&line_state, 0, sizeof(line_state));
    line_state.cancel_on_line = 1;
    memset(&req, 0, sizeof(req));
    req.program = self_exe;
    req.argv = argv;
    req.poll_interval_ms = 5;
    req.on_line = on_line_count;
    req.line_user_data = &line_state;

    ASSERT_INT_EQ(lp_external_oop_run(&req, &res), 0,
                  "runner/cancel: run with line-cancel succeeds");
    ASSERT_INT_EQ(res.cancelled, 1,
                  "runner/cancel: line cancellation set");
}

/*
 * A program that does not exist is not a runner failure: POSIX forks, execvp
 * fails and the child exits 127. The Windows runner reports the same thing
 * when CreateProcess refuses, so callers need no per-platform branch.
 */
static void test_runner_missing_program(void) {
    char *argv[2];
    LPExternalOOPRunRequest req;
    LPExternalOOPRunResult res;
    const char *missing = "ralph_no_such_program_hopefully";

    argv[0] = (char *)missing;
    argv[1] = NULL;

    memset(&req, 0, sizeof(req));
    req.program = missing;
    req.argv = argv;
    req.poll_interval_ms = 5;

    ASSERT_INT_EQ(lp_external_oop_run(&req, &res), 0,
                  "runner/missing: run reports a result, not a runner error");
    ASSERT_INT_EQ(res.exit_code, 127,
                  "runner/missing: exit code 127");
}

/*
 * Arguments must reach the child unchanged.
 *
 * On POSIX this is nearly free -- execvp is handed the argv array as-is. On
 * Windows the runner has to flatten argv into one string that the child's CRT
 * splits again, and the rules for that are quote- and backslash-sensitive in a
 * way that fails silently: the child simply receives something else. Since the
 * arguments this runner carries in production are Windows file paths, which
 * are made of backslashes, that is the case worth pinning down.
 */
static void test_runner_argv_roundtrip(void) {
    LPExternalOOPRunRequest req;
    LPExternalOOPRunResult res;
    RunnerCollectState st;
    int i;

    static const char *const args[] = {
        "plain",
        "with space",
        "C:\\Users\\Mark\\AppData\\Local\\Temp\\model.mps",
        "trailing\\",
        "quote\"inside",
        "back\\\\slash"
    };
    const int nargs = (int)(sizeof(args) / sizeof(args[0]));
    /* self_exe, the mode flag, every argument, and the NULL terminator. */
    char *argv[2 + sizeof(args) / sizeof(args[0]) + 1];

    argv[0] = self_exe;
    argv[1] = (char *)CHILD_FLAG_ARGV;
    for (i = 0; i < nargs; i++) argv[2 + i] = (char *)args[i];
    argv[2 + nargs] = NULL;

    memset(&st, 0, sizeof(st));
    memset(&req, 0, sizeof(req));
    req.program = self_exe;
    req.argv = argv;
    req.poll_interval_ms = 5;
    req.on_line = on_line_collect;
    req.line_user_data = &st;

    ASSERT_INT_EQ(lp_external_oop_run(&req, &res), 0,
                  "runner/argv: run succeeds");
    ASSERT_INT_EQ(res.exit_code, 0,
                  "runner/argv: child exited cleanly");
    ASSERT_INT_EQ(st.count, nargs,
                  "runner/argv: child saw the same number of arguments");

    for (i = 0; i < nargs && i < st.count; i++) {
        char want[COLLECT_MAX_LEN];
        snprintf(want, sizeof(want), "[%s]", args[i]);
        if (strcmp(want, st.lines[i]) != 0) {
            printf("    arg %d: sent %s, child saw %s\n", i, want, st.lines[i]);
        }
        ASSERT_TRUE(strcmp(want, st.lines[i]) == 0,
                    "runner/argv: argument survived the round trip");
    }
}

int main(int argc, char **argv) {
    if (argc >= 2 && strncmp(argv[1], "--child-", 8) == 0) {
        return run_child_mode(argc, argv);
    }

    printf("=== LP External OOP Runner Tests ===\n");

    if (self_exe_path(self_exe, sizeof(self_exe), argc > 0 ? argv[0] : NULL) != 0) {
        printf("  FAIL: could not determine own executable path\n");
        return 1;
    }

    test_tempfile_helpers();
    test_runner_exit_and_line_capture();
    test_runner_timeout();
    test_runner_cancel_callback_and_line_cancel();
    test_runner_argv_roundtrip();
    test_runner_missing_program();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_run == tests_passed) ? 0 : 1;
}
