/* ============================================================================
 * vsh - Vanguard Shell
 * builtins/jobs.c - Job listing builtin
 * ============================================================================ */

#include "builtins.h"
#include "shell.h"
#include "job_control.h"
#include <stdio.h>

/*
 * jobs
 *
 * List all active jobs and their statuses.
 */
int builtin_jobs(Shell *shell, int argc, char **argv) {
    (void)argc;
    (void)argv;

    job_list_print(shell);
    return 0;
}
