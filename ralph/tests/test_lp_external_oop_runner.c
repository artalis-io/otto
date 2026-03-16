/*
 * Tests for generic out-of-process external runner utilities.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

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

static int write_script(const char *body, char *path, size_t path_size) {
    int fd;
    FILE *f;
    if (!body || !path || path_size < 32) return -1;

    snprintf(path, path_size, "/tmp/ralph_oop_runner_XXXXXX");
    fd = mkstemp(path);
    if (fd < 0) return -1;
    f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(path);
        return -1;
    }
    if (fputs(body, f) == EOF) {
        fclose(f);
        unlink(path);
        return -1;
    }
    if (fclose(f) != 0) {
        unlink(path);
        return -1;
    }
    if (chmod(path, 0700) != 0) {
        unlink(path);
        return -1;
    }
    return 0;
}

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
    char script[256];
    char *argv[2];
    LPExternalOOPRunRequest req;
    LPExternalOOPRunResult res;
    RunnerLineState st;
    int rc_script;

    const char *body =
        "#!/bin/sh\n"
        "echo alpha\n"
        "echo beta\n"
        "exit 0\n";

    rc_script = write_script(body, script, sizeof(script));
    ASSERT_INT_EQ(rc_script, 0,
                  "runner/lines: create script");
    if (rc_script != 0) return;

    argv[0] = script;
    argv[1] = NULL;
    memset(&st, 0, sizeof(st));
    memset(&req, 0, sizeof(req));
    req.program = script;
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

    unlink(script);
}

static void test_runner_timeout(void) {
    char script[256];
    char *argv[2];
    LPExternalOOPRunRequest req;
    LPExternalOOPRunResult res;
    const char *body =
        "#!/bin/sh\n"
        "sleep 2\n"
        "exit 0\n";
    int rc_script;

    rc_script = write_script(body, script, sizeof(script));
    ASSERT_INT_EQ(rc_script, 0,
                  "runner/timeout: create script");
    if (rc_script != 0) return;

    argv[0] = script;
    argv[1] = NULL;
    memset(&req, 0, sizeof(req));
    req.program = script;
    req.argv = argv;
    req.poll_interval_ms = 5;
    req.wall_time_limit_sec = 0.05;

    ASSERT_INT_EQ(lp_external_oop_run(&req, &res), 0,
                  "runner/timeout: run succeeds");
    ASSERT_INT_EQ(res.timed_out, 1,
                  "runner/timeout: timeout flag set");

    unlink(script);
}

static void test_runner_cancel_callback_and_line_cancel(void) {
    char script[256];
    char *argv[2];
    LPExternalOOPRunRequest req;
    LPExternalOOPRunResult res;
    RunnerCancelState cancel_state;
    RalphLPCancelCallback cb;
    RunnerLineState line_state;

    const char *body =
        "#!/bin/sh\n"
        "while true; do\n"
        "  echo tick\n"
        "  sleep 1\n"
        "done\n";
    int rc_script;

    rc_script = write_script(body, script, sizeof(script));
    ASSERT_INT_EQ(rc_script, 0,
                  "runner/cancel: create script");
    if (rc_script != 0) return;

    argv[0] = script;
    argv[1] = NULL;

    memset(&cancel_state, 0, sizeof(cancel_state));
    cancel_state.cancel_after = 3;
    cb.should_cancel = should_cancel_poll;
    cb.user_data = &cancel_state;

    memset(&req, 0, sizeof(req));
    req.program = script;
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
    req.program = script;
    req.argv = argv;
    req.poll_interval_ms = 5;
    req.on_line = on_line_count;
    req.line_user_data = &line_state;

    ASSERT_INT_EQ(lp_external_oop_run(&req, &res), 0,
                  "runner/cancel: run with line-cancel succeeds");
    ASSERT_INT_EQ(res.cancelled, 1,
                  "runner/cancel: line cancellation set");

    unlink(script);
}

int main(void) {
    printf("=== LP External OOP Runner Tests ===\n");

    test_tempfile_helpers();
    test_runner_exit_and_line_capture();
    test_runner_timeout();
    test_runner_cancel_callback_and_line_cancel();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_run == tests_passed) ? 0 : 1;
}
