/* ============================================================================
 * vsh - Vanguard Shell
 * builtins/fg_bg.c - Foreground and background job control builtins
 * ============================================================================ */

#include "builtins.h"
#include "shell.h"
#include "job_control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Parse a job specifier from an argument string.
 * Supports:
 *   %N  - job number N
 *   N   - job number N
 *   (none) - most recent job
 *
 * Returns the Job pointer, or NULL on failure (prints error).
 */
static Job *parse_job_arg(Shell *shell, int argc, char **argv) {
    if (argc < 2) {
        /* No argument: use most recent job */
        Job *job = job_most_recent(shell);
        if (!job) {
            fprintf(stderr, "vsh: %s: no current job\n", argv[0]);
            return NULL;
        }
        return job;
    }

    const char *arg = argv[1];
    int job_id;

    /* Skip leading % if present */
    if (arg[0] == '%')
        arg++;

    char *endp;
    long val = strtol(arg, &endp, 10);
    if (*endp != '\0' || val <= 0) {
        fprintf(stderr, "vsh: %s: %s: no such job\n", argv[0], argv[1]);
        return NULL;
    }
    job_id = (int)val;

    Job *job = job_find_by_id(shell, job_id);
    if (!job) {
        fprintf(stderr, "vsh: %s: %%%d: no such job\n", argv[0], job_id);
        return NULL;
    }

    return job;
}

/*
 * fg [%N]
 *
 * Resume a stopped job (or bring a background job) to the foreground.
 * With no argument, operates on the most recent job.
 */
int builtin_fg(Shell *shell, int argc, char **argv) {
    Job *job = parse_job_arg(shell, argc, argv);
    if (!job)
        return 1;

    printf("[%d] %s\n", job->id, job->command);
    return job_continue_foreground(shell, job);
}

/*
 * bg [%N]
 *
 * Resume a stopped job in the background.
 * With no argument, operates on the most recent job.
 */
int builtin_bg(Shell *shell, int argc, char **argv) {
    Job *job = parse_job_arg(shell, argc, argv);
    if (!job)
        return 1;

    printf("[%d] %s &\n", job->id, job->command);
    return job_continue_background(shell, job);
}
