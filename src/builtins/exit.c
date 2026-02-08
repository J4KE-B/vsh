/* ============================================================================
 * vsh - Vanguard Shell
 * builtins/exit.c - Exit the shell
 * ============================================================================ */

#include "builtins.h"
#include "shell.h"
#include "job_control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * exit [N]
 *
 * Exit the shell with status N (default 0).
 * If there are stopped jobs, warn the first time and only exit on a
 * second consecutive attempt.
 */
int builtin_exit(Shell *shell, int argc, char **argv) {
    static bool warned_about_jobs = false;

    /* Check for stopped jobs */
    if (shell->jobs && shell->jobs->head) {
        Job *j = shell->jobs->head;
        bool has_stopped = false;
        while (j) {
            if (j->state == JOB_STOPPED) {
                has_stopped = true;
                break;
            }
            j = j->next;
        }

        if (has_stopped && !warned_about_jobs) {
            fprintf(stderr, "There are stopped jobs.\n");
            warned_about_jobs = true;
            return 1;
        }
    }

    /* Determine exit status */
    int status = 0;
    if (argc > 1) {
        char *endp;
        long val = strtol(argv[1], &endp, 10);
        if (*endp != '\0') {
            fprintf(stderr, "vsh: exit: %s: numeric argument required\n", argv[1]);
            status = 2;
        } else {
            status = (int)(val & 0xff);
        }
    } else {
        status = shell->last_status;
    }

    shell->running = false;
    shell->last_status = status;
    warned_about_jobs = false;

    return status;
}
