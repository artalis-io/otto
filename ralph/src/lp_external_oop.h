#ifndef LP_EXTERNAL_OOP_H
#define LP_EXTERNAL_OOP_H

#include <stddef.h>
#include "ralph_core.h"

typedef int (*LPExternalOOPLineFn)(const char *line, void *user_data);

typedef struct {
    const char *program;
    char *const *argv;
    double wall_time_limit_sec;                 /* <=0 disables wall-clock timeout */
    const RalphLPCancelCallback *cancel_cb;     /* Optional cancellation hook */
    int poll_interval_ms;                       /* <=0 defaults to 10ms */
    LPExternalOOPLineFn on_line;                /* Optional line callback (stdout+stderr merged) */
    void *line_user_data;                       /* Optional line callback user data */
} LPExternalOOPRunRequest;

typedef struct {
    int exit_code;      /* Child exit code when exited normally; -1 otherwise */
    int signaled;       /* 1 if child ended due to signal */
    int signal_no;      /* Signal number when signaled, otherwise 0 */
    int timed_out;      /* 1 when wall timeout enforced */
    int cancelled;      /* 1 when cancel callback requested stop */
    double elapsed_ms;  /* Wall time in ms spent in runner */
} LPExternalOOPRunResult;

int lp_external_oop_run(const LPExternalOOPRunRequest *req,
                        LPExternalOOPRunResult *result);

int lp_external_oop_make_tempfile(const char *prefix,
                                  char *path,
                                  size_t path_size);

void lp_external_oop_cleanup_file(const char *path);

#endif
