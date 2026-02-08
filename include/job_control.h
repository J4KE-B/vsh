/* ============================================================================
 * vsh - Vanguard Shell
 * job_control.h - Job control and signal management
 * ============================================================================ */

#ifndef VSH_JOB_CONTROL_H
#define VSH_JOB_CONTROL_H

#include "shell.h"
#include <sys/types.h>
#include <stdbool.h>

/* Initialize job control (take control of terminal) */
void job_control_init(Shell *shell);

/* Add a new job to the job table */
Job *job_add(Shell *shell, pid_t pgid, pid_t *pids, int npids,
             const char *command, bool foreground);

/* Remove a job from the table */
void job_remove(Shell *shell, int job_id);

/* Find a job by job ID */
Job *job_find_by_id(Shell *shell, int job_id);

/* Find a job by process group ID */
Job *job_find_by_pgid(Shell *shell, pid_t pgid);

/* Find a job by any PID in its process list */
Job *job_find_by_pid(Shell *shell, pid_t pid);

/* Get the most recent job */
Job *job_most_recent(Shell *shell);

/* Update job status (call after waitpid) */
void job_update_status(Shell *shell, pid_t pid, int status);

/* Wait for a foreground job to complete or stop */
int job_wait_foreground(Shell *shell, Job *job);

/* Continue a stopped job in foreground */
int job_continue_foreground(Shell *shell, Job *job);

/* Continue a stopped job in background */
int job_continue_background(Shell *shell, Job *job);

/* Check for completed/stopped background jobs and notify user */
void job_check_background(Shell *shell);

/* Print job list */
void job_list_print(Shell *shell);

/* Free all jobs */
void job_table_destroy(Shell *shell);

/* SIGCHLD handler (updates job statuses) */
void sigchld_handler(int sig);

/* Set up the global shell pointer for signal handlers */
void job_set_shell(Shell *shell);

#endif /* VSH_JOB_CONTROL_H */
