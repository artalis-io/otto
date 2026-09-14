#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #undef near
  #undef far
  #include <io.h>
#else
  #include <unistd.h>
  #include <sys/wait.h>
#endif

#include "sh_pal.h"
#include "lp_external_oop.h"

#define OOP_DEFAULT_POLL_MS 10
#define OOP_LINE_BUF_SIZE 4096
#define OOP_IO_BUF_SIZE 1024

/*
 * Most read chunks to take before returning to the poll loop.
 *
 * The timeout, cancellation and exit checks all live *after* the drain, so a
 * child that writes faster than we read would keep the drain fed and starve
 * them -- cancel latency unbounded, which is the same shape as the basis
 * repair loop that let a one-second limit run for forty-five seconds.
 *
 * Bounding it costs at most one extra poll interval per drain and caps
 * throughput at this many KiB per interval, which is far above anything a
 * solver's progress output produces. 0 means drain until empty, which is what
 * the final pass after the child exits wants.
 */
#define OOP_DRAIN_CHUNKS_PER_POLL 256

static double oop_now_ms(void) {
    return (double)sh_monotonic_ns() / 1.0e6;
}

static void oop_sleep_ms(int ms) {
    if (ms <= 0) return;
    sh_sleep_ms((unsigned)ms);
}

static void oop_result_init(LPExternalOOPRunResult *result) {
    if (!result) return;
    memset(result, 0, sizeof(*result));
    result->exit_code = -1;
}

#ifndef _WIN32
/* Used only by the POSIX runner below. Windows anonymous pipes have no
 * O_NONBLOCK equivalent; the Windows runner polls with PeekNamedPipe
 * instead, reading only what is already buffered. */
static int oop_pipe_set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return -1;
    return 0;
}
#endif

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
            /*
             * Drop a trailing CR. A child using text-mode stdio on Windows
             * writes CRLF, so without this every line callback there would see
             * a stray '\r' that the same child never produces on POSIX. The
             * callbacks are shared across platforms, so the lines they are
             * handed have to be too.
             */
            if (*line_len > 0 && line_buf[*line_len - 1] == '\r') (*line_len)--;
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
    if (*line_len > 0 && line_buf[*line_len - 1] == '\r') (*line_len)--;
    line_buf[*line_len] = '\0';
    if (oop_emit_line(req, line_buf) != 0) {
        if (cancelled_by_line) *cancelled_by_line = 1;
    }
    *line_len = 0;
}

int lp_external_oop_make_tempfile(const char *prefix,
                                  char *path,
                                  size_t path_size) {
#ifdef _WIN32
    char dir[MAX_PATH];
    char name[MAX_PATH];

    if (!prefix || !path || path_size < 32) return -1;

    /* There is no /tmp on Windows. GetTempFileName also creates the file, so
     * the result matches mkstemp's: the path exists and is ours. */
    if (GetTempPathA((DWORD)sizeof(dir), dir) == 0) return -1;
    if (GetTempFileNameA(dir, prefix, 0, name) == 0) return -1;
    if (strlen(name) >= path_size) {
        (void)DeleteFileA(name);
        return -1;
    }
    snprintf(path, path_size, "%s", name);
    return 0;
#else
    int fd = -1;

    if (!prefix || !path || path_size < 32) return -1;
    snprintf(path, path_size, "/tmp/%sXXXXXX", prefix);
    fd = mkstemp(path);
    if (fd < 0) return -1;
    close(fd);
    return 0;
#endif
}

void lp_external_oop_cleanup_file(const char *path) {
    if (!path || path[0] == '\0') return;
    (void)unlink(path);
}

/*
 * Run a child process and stream its merged stdout/stderr, with a wall-clock
 * timeout and a cancellation hook.
 *
 * Two implementations follow. POSIX is fork + execvp over a non-blocking pipe.
 * Windows is CreateProcess over an anonymous pipe polled with PeekNamedPipe,
 * which is the closest available stand-in for O_NONBLOCK: ask how many bytes
 * are buffered, then read exactly that many so ReadFile never blocks.
 *
 * Both report the same three things the callers actually read -- exit_code,
 * timed_out and cancelled. The `signaled` and `signal_no` fields have no
 * Windows analogue and stay zero there; nothing in the tree reads them.
 */
#ifdef _WIN32

/* Exit code handed to TerminateProcess. Chosen as 128+SIGKILL for anyone
 * reading a process dump; the result struct reports -1 either way, matching
 * what POSIX returns for a signaled child. */
#define OOP_WIN_KILL_EXIT_CODE 137

/* Append one character to the command line under construction, leaving room
 * for the terminating NUL. */
static int oop_win_put(char *cmd, size_t cmd_size, size_t *n, char c) {
    if (*n + 1 >= cmd_size) return -1;
    cmd[*n] = c;
    (*n)++;
    return 0;
}

/*
 * Append one argument to a Windows command line, quoted so that the child
 * splits it back into the same argument.
 *
 * execvp takes a real argv array. CreateProcess takes one string and the child
 * parses it again, so every argument has to survive a round trip through the
 * CommandLineToArgvW rules: a run of N backslashes immediately before a quote
 * becomes 2N+1 backslashes, a run at the end of a quoted argument becomes 2N,
 * and backslashes anywhere else are literal. Getting this wrong is silent --
 * the child simply receives different arguments -- and every caller here
 * passes Windows paths, which are made of backslashes, so it has to be right.
 *
 * Returns 0, or -1 if the argument would not fit.
 */
static int oop_win_append_arg(char *cmd, size_t cmd_size, size_t *n,
                              const char *arg) {
    const char *p;
    size_t backslashes = 0;
    int need_quotes = 0;

    if (arg[0] == '\0') {
        need_quotes = 1;
    } else {
        for (p = arg; *p; p++) {
            if (*p == ' ' || *p == '\t' || *p == '"') { need_quotes = 1; break; }
        }
    }

    if (*n > 0 && oop_win_put(cmd, cmd_size, n, ' ') != 0) return -1;
    if (need_quotes && oop_win_put(cmd, cmd_size, n, '"') != 0) return -1;

    for (p = arg; *p; p++) {
        if (*p == '\\') {
            backslashes++;
            continue;
        }
        if (*p == '"') {
            size_t k = backslashes * 2 + 1;
            while (k-- > 0) {
                if (oop_win_put(cmd, cmd_size, n, '\\') != 0) return -1;
            }
            backslashes = 0;
            if (oop_win_put(cmd, cmd_size, n, '"') != 0) return -1;
            continue;
        }
        /* An ordinary character: any backslashes before it stay literal. */
        while (backslashes > 0) {
            if (oop_win_put(cmd, cmd_size, n, '\\') != 0) return -1;
            backslashes--;
        }
        if (oop_win_put(cmd, cmd_size, n, *p) != 0) return -1;
    }

    if (need_quotes) {
        /* Double a trailing run, which would otherwise escape the closing
         * quote instead of standing for itself. */
        backslashes *= 2;
    }
    while (backslashes > 0) {
        if (oop_win_put(cmd, cmd_size, n, '\\') != 0) return -1;
        backslashes--;
    }
    if (need_quotes && oop_win_put(cmd, cmd_size, n, '"') != 0) return -1;

    cmd[*n] = '\0';
    return 0;
}

/* Drain whatever the pipe currently holds without blocking. Sets *eof when the
 * child has closed its end. Returns 0, or -1 if the pipe broke. */
static int oop_win_drain(const LPExternalOOPRunRequest *req,
                         HANDLE read_h,
                         char *io_buf,
                         size_t io_buf_size,
                         char *line_buf,
                         size_t *line_len,
                         int *cancelled_by_line,
                         int *eof,
                         unsigned max_chunks) {
    unsigned taken = 0;

    for (;;) {
        DWORD avail = 0;
        DWORD got = 0;

        if (max_chunks != 0 && taken >= max_chunks) return 0;
        taken++;

        if (!PeekNamedPipe(read_h, NULL, 0, NULL, &avail, NULL)) {
            /* ERROR_BROKEN_PIPE: the child closed or exited. Not an error. */
            *eof = 1;
            return 0;
        }
        if (avail == 0) return 0;
        if (avail > (DWORD)io_buf_size) avail = (DWORD)io_buf_size;

        if (!ReadFile(read_h, io_buf, avail, &got, NULL) || got == 0) {
            *eof = 1;
            return 0;
        }
        (void)oop_handle_stream_chunk(req, io_buf, (size_t)got,
                                      line_buf, line_len, cancelled_by_line);
    }
}

int lp_external_oop_run(const LPExternalOOPRunRequest *req,
                        LPExternalOOPRunResult *result) {
    HANDLE read_h = INVALID_HANDLE_VALUE;
    HANDLE write_h = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char *cmd = NULL;
    size_t cmd_size = 1;
    size_t cmd_len = 0;
    int poll_ms;
    double start_ms;
    int child_done = 0;
    int child_killed = 0;
    int cancel_requested = 0;
    int kill_reason_timeout = 0;
    int kill_reason_cancel = 0;
    int cancelled_by_line = 0;
    int pipe_eof = 0;
    char io_buf[OOP_IO_BUF_SIZE];
    char line_buf[OOP_LINE_BUF_SIZE];
    size_t line_len = 0;
    DWORD code = 0;
    int i;

    if (!req || !req->program || !req->argv || !result) return -1;

    oop_result_init(result);
    poll_ms = req->poll_interval_ms > 0 ? req->poll_interval_ms : OOP_DEFAULT_POLL_MS;
    start_ms = oop_now_ms();

    /*
     * Build the command line.
     *
     * The first token is req->program rather than argv[0]. CreateProcess
     * resolves the executable from that token when lpApplicationName is NULL,
     * including the PATH search that execvp does, and Windows offers no way to
     * set the child's argv[0] independently without giving that search up.
     * Every caller in this tree passes the same string for both.
     *
     * Worst case per argument is every character doubling into an escaped
     * backslash, plus two quotes and a separating space.
     */
    cmd_size += 2 * strlen(req->program) + 3;
    for (i = 0; req->argv[i]; i++) cmd_size += 2 * strlen(req->argv[i]) + 3;

    cmd = (char *)malloc(cmd_size);
    if (!cmd) return -1;
    cmd[0] = '\0';

    if (oop_win_append_arg(cmd, cmd_size, &cmd_len, req->program) != 0) {
        free(cmd);
        return -1;
    }
    if (req->argv[0]) {
        for (i = 1; req->argv[i]; i++) {
            if (oop_win_append_arg(cmd, cmd_size, &cmd_len, req->argv[i]) != 0) {
                free(cmd);
                return -1;
            }
        }
    }

    /* The write end is inheritable so the child gets it as stdout/stderr; the
     * read end must not be, or the pipe would never report EOF because the
     * child would be holding a copy of it. */
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&read_h, &write_h, &sa, 0)) {
        free(cmd);
        return -1;
    }
    if (!SetHandleInformation(read_h, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(read_h);
        CloseHandle(write_h);
        free(cmd);
        return -1;
    }

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_h;
    si.hStdError = write_h;   /* Merged, like the POSIX pair of dup2 calls. */
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        /*
         * On POSIX a missing or unrunnable program is not a runner failure:
         * fork succeeds, execvp fails and the child exits 127. Report the same
         * thing so callers need no per-platform branch.
         */
        CloseHandle(read_h);
        CloseHandle(write_h);
        free(cmd);
        result->exit_code = 127;
        result->elapsed_ms = oop_now_ms() - start_ms;
        return 0;
    }

    /* The parent's copy must go, or the pipe never reaches EOF. */
    CloseHandle(write_h);
    write_h = INVALID_HANDLE_VALUE;

    while (!child_done) {
        if (!pipe_eof) {
            (void)oop_win_drain(req, read_h, io_buf, sizeof(io_buf),
                                line_buf, &line_len, &cancelled_by_line,
                                &pipe_eof, OOP_DRAIN_CHUNKS_PER_POLL);
        }

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
                (void)TerminateProcess(pi.hProcess, OOP_WIN_KILL_EXIT_CODE);
            }
        }

        if (!child_killed && cancel_requested) {
            kill_reason_cancel = 1;
            child_killed = 1;
            (void)TerminateProcess(pi.hProcess, OOP_WIN_KILL_EXIT_CODE);
        }

        /* TerminateProcess is asynchronous, so this may take one more turn
         * round the loop to observe. */
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            child_done = 1;
            break;
        }

        oop_sleep_ms(poll_ms);
    }

    /* The child has exited, so anything it wrote is already in the pipe
     * buffer; one more drain collects it. */
    if (!pipe_eof) {
        (void)oop_win_drain(req, read_h, io_buf, sizeof(io_buf),
                            line_buf, &line_len, &cancelled_by_line,
                            &pipe_eof, 0);
    }
    oop_flush_line(req, line_buf, &line_len, &cancelled_by_line);

    if (!GetExitCodeProcess(pi.hProcess, &code)) code = (DWORD)-1;

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(read_h);
    free(cmd);

    result->elapsed_ms = oop_now_ms() - start_ms;
    result->timed_out = kill_reason_timeout ? 1 : 0;
    result->cancelled = kill_reason_cancel ? 1 : 0;
    result->signaled = 0;
    result->signal_no = 0;
    /* A killed child reports -1, which is what POSIX gives for a signaled one.
     * Why it was killed is in timed_out and cancelled. */
    result->exit_code = child_killed ? -1 : (int)code;

    return 0;
}

#else
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
        unsigned taken = 0;

        /* Bounded for the same reason as the Windows drain: the timeout
         * and cancel checks are below this loop, and a chatty child would
         * otherwise keep it fed and starve them. */
        do {
            if (taken++ >= OOP_DRAIN_CHUNKS_PER_POLL) break;
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

#endif /* _WIN32 */
