#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include "lp_external_oop.h"

#define OOP_DEFAULT_POLL_MS 10
#define OOP_LINE_BUF_SIZE 4096
#define OOP_IO_BUF_SIZE 1024

static double oop_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void oop_sleep_ms(int ms) {
    struct timespec req;
    if (ms <= 0) return;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    (void)nanosleep(&req, NULL);
}

static void oop_result_init(LPExternalOOPRunResult *result) {
    if (!result) return;
    memset(result, 0, sizeof(*result));
    result->exit_code = -1;
}

static int oop_pipe_set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return -1;
    return 0;
}

static int oop_emit_line(const LPExternalOOPRunRequest *req, const char *line) {
    if (!req || !req->on_line) return 0;
    return req->on_line(line, req->line_user_data);
}

static int oop_handle_stream_chunk(const LPExternalOOPRunRequest *req,
                                   const char *chunk,
                                   size_t len,
                                   char *line_buf,
                                   size_t *line_len,
                                   int *cancelled_by_line) {
    for (size_t i = 0; i < len; i++) {
        char c = chunk[i];
        if (c == '\n') {
            line_buf[*line_len] = '\0';
            if (oop_emit_line(req, line_buf) != 0) {
                if (cancelled_by_line) *cancelled_by_line = 1;
            }
            *line_len = 0;
            continue;
        }

        if (*line_len + 1 < OOP_LINE_BUF_SIZE) {
            line_buf[*line_len] = c;
            (*line_len)++;
        }
    }
    return 0;
}

static void oop_flush_line(const LPExternalOOPRunRequest *req,
                           char *line_buf,
                           size_t *line_len,
                           int *cancelled_by_line) {
    if (!line_buf || !line_len || *line_len == 0) return;
    line_buf[*line_len] = '\0';
    if (oop_emit_line(req, line_buf) != 0) {
        if (cancelled_by_line) *cancelled_by_line = 1;
    }
    *line_len = 0;
}

int lp_external_oop_make_tempfile(const char *prefix,
                                  char *path,
                                  size_t path_size) {
    int fd = -1;

    if (!prefix || !path || path_size < 32) return -1;
    snprintf(path, path_size, "/tmp/%sXXXXXX", prefix);
    fd = mkstemp(path);
    if (fd < 0) return -1;
    close(fd);
    return 0;
}

void lp_external_oop_cleanup_file(const char *path) {
    if (!path || path[0] == '\0') return;
    (void)unlink(path);
}

int lp_external_oop_run(const LPExternalOOPRunRequest *req,
                        LPExternalOOPRunResult *result) {
    int pipefd[2] = {-1, -1};
    pid_t pid = -1;
    int poll_ms;
    double start_ms;
    int child_done = 0;
    int child_status = 0;
    int child_killed = 0;
    int cancel_requested = 0;
    int kill_reason_timeout = 0;
    int kill_reason_cancel = 0;
    char io_buf[OOP_IO_BUF_SIZE];
    char line_buf[OOP_LINE_BUF_SIZE];
    size_t line_len = 0;
    int cancelled_by_line = 0;

    if (!req || !req->program || !req->argv || !result) return -1;

    oop_result_init(result);
    poll_ms = req->poll_interval_ms > 0 ? req->poll_interval_ms : OOP_DEFAULT_POLL_MS;
    start_ms = oop_now_ms();

    if (pipe(pipefd) != 0) return -1;
    if (oop_pipe_set_nonblock(pipefd[0]) != 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        (void)dup2(pipefd[1], STDOUT_FILENO);
        (void)dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execvp(req->program, req->argv);
        _exit(127);
    }

    close(pipefd[1]);
    pipefd[1] = -1;

    while (!child_done) {
        int wait_rc;
        ssize_t nread;

        do {
            nread = read(pipefd[0], io_buf, sizeof(io_buf));
            if (nread > 0) {
                (void)oop_handle_stream_chunk(req,
                                              io_buf,
                                              (size_t)nread,
                                              line_buf,
                                              &line_len,
                                              &cancelled_by_line);
            }
        } while (nread > 0);

        if (cancelled_by_line) {
            cancel_requested = 1;
        } else if (req->cancel_cb && req->cancel_cb->should_cancel) {
            if (req->cancel_cb->should_cancel(req->cancel_cb->user_data) != 0) {
                cancel_requested = 1;
            }
        }

        if (!child_killed && req->wall_time_limit_sec > 0.0) {
            double elapsed_sec = (oop_now_ms() - start_ms) / 1000.0;
            if (elapsed_sec > req->wall_time_limit_sec) {
                kill_reason_timeout = 1;
                child_killed = 1;
                (void)kill(pid, SIGKILL);
            }
        }

        if (!child_killed && cancel_requested) {
            kill_reason_cancel = 1;
            child_killed = 1;
            (void)kill(pid, SIGKILL);
        }

        wait_rc = waitpid(pid, &child_status, WNOHANG);
        if (wait_rc == pid) {
            child_done = 1;
            break;
        }
        if (wait_rc < 0 && errno != EINTR) {
            child_done = 1;
            break;
        }

        oop_sleep_ms(poll_ms);
    }

    if (!child_done) {
        int wait_rc;
        do {
            wait_rc = waitpid(pid, &child_status, 0);
        } while (wait_rc < 0 && errno == EINTR);
    }

    {
        ssize_t nread;
        do {
            nread = read(pipefd[0], io_buf, sizeof(io_buf));
            if (nread > 0) {
                (void)oop_handle_stream_chunk(req,
                                              io_buf,
                                              (size_t)nread,
                                              line_buf,
                                              &line_len,
                                              &cancelled_by_line);
            }
        } while (nread > 0);
    }
    oop_flush_line(req, line_buf, &line_len, &cancelled_by_line);

    close(pipefd[0]);
    pipefd[0] = -1;

    result->elapsed_ms = oop_now_ms() - start_ms;
    result->timed_out = kill_reason_timeout ? 1 : 0;
    result->cancelled = kill_reason_cancel ? 1 : 0;

    if (WIFEXITED(child_status)) {
        result->exit_code = WEXITSTATUS(child_status);
        result->signaled = 0;
        result->signal_no = 0;
    } else if (WIFSIGNALED(child_status)) {
        result->exit_code = -1;
        result->signaled = 1;
        result->signal_no = WTERMSIG(child_status);
    } else {
        result->exit_code = -1;
        result->signaled = 0;
        result->signal_no = 0;
    }

    return 0;
}
