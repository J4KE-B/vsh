/* ============================================================================
 * vsh - Vanguard Shell
 * job_control.c - Job control with process groups and signals
 *
 * Manages background/foreground jobs, process groups, and the SIGCHLD handler.
 * The shell's own process is placed in its own process group and holds the
 * terminal.  Each pipeline gets its own process group; foreground jobs are
 * given the terminal via tcsetpgrp, and background jobs run silently until
 * the shell reports their completion at the next prompt.
 * ============================================================================ */

#include "job_control.h"
#include "shell.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ---- Global shell pointer for signal handlers --------------------------- */

static Shell *g_shell = NULL;

/* ---- Internal helpers --------------------------------------------------- */

static const char *job_state_str(JobState state)
{
    switch (state) {
    case JOB_RUNNING:  return "Running";
    case JOB_STOPPED:  return "Stopped";
    case JOB_DONE:     return "Done";
    case JOB_KILLED:   return "Killed";
    }
    return "Unknown";
}

/* ---- Public API --------------------------------------------------------- */

void job_set_shell(Shell *shell)
{
    g_shell = shell;
}

void job_control_init(Shell *shell)
{
    if (!shell)
        return;

    /* Initialise the job table */
    shell->jobs->head    = NULL;
    shell->jobs->next_id = 1;

    if (!shell->interactive)
        return;

    /* Put the shell in its own process group and take the terminal */
    pid_t shell_pid = getpid();
    shell->shell_pid = shell_pid;

    /* Loop until we are in the foreground process group */
    while (tcgetpgrp(STDIN_FILENO) != (shell_pid = getpgrp()))
        kill(-shell_pid, SIGTTIN);

    /* Put ourselves in our own process group */
    shell_pid = getpid();
    if (setpgid(shell_pid, shell_pid) < 0) {
        perror("vsh: setpgid");
    }

    /* Grab the terminal */
    tcsetpgrp(STDIN_FILENO, shell_pid);

    /* Ignore job-control signals in the shell process itself */
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    /* Set up SIGCHLD handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    /* Store global pointer for the signal handler */
    g_shell = shell;
}

void sigchld_handler(int sig)
{
    (void)sig;

    if (!g_shell)
        return;

    int saved_errno = errno;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        job_update_status(g_shell, pid, status);
    }

    errno = saved_errno;
}

Job *job_add(Shell *shell, pid_t pgid, pid_t *pids, int npids,
             const char *command, bool foreground)
{
    if (!shell || !shell->jobs)
        return NULL;

    Job *job = malloc(sizeof(Job));
    if (!job)
        return NULL;

    job->id = shell->jobs->next_id++;
    job->pgid = pgid;

    /* Copy the PID array */
    job->pids = malloc((size_t)npids * sizeof(pid_t));
    if (!job->pids) {
        free(job);
        return NULL;
    }
    memcpy(job->pids, pids, (size_t)npids * sizeof(pid_t));
    job->npids = npids;

    job->state      = JOB_RUNNING;
    job->command    = strdup(command ? command : "");
    job->notified   = false;
    job->foreground = foreground;

    /* Prepend to the list */
    job->next = shell->jobs->head;
    shell->jobs->head = job;

    return job;
}

void job_remove(Shell *shell, int job_id)
{
    if (!shell || !shell->jobs)
        return;

    Job **pp = &shell->jobs->head;
    while (*pp) {
        Job *j = *pp;
        if (j->id == job_id) {
            *pp = j->next;
            free(j->pids);
            free(j->command);
            free(j);
            return;
        }
        pp = &j->next;
    }
}

Job *job_find_by_id(Shell *shell, int job_id)
{
    if (!shell || !shell->jobs)
        return NULL;

    for (Job *j = shell->jobs->head; j; j = j->next) {
        if (j->id == job_id)
            return j;
    }
    return NULL;
}

Job *job_find_by_pgid(Shell *shell, pid_t pgid)
{
    if (!shell || !shell->jobs)
        return NULL;

    for (Job *j = shell->jobs->head; j; j = j->next) {
        if (j->pgid == pgid)
            return j;
    }
    return NULL;
}

Job *job_find_by_pid(Shell *shell, pid_t pid)
{
    if (!shell || !shell->jobs)
        return NULL;

    for (Job *j = shell->jobs->head; j; j = j->next) {
        for (int i = 0; i < j->npids; i++) {
            if (j->pids[i] == pid)
                return j;
        }
    }
    return NULL;
}

Job *job_most_recent(Shell *shell)
{
    if (!shell || !shell->jobs)
        return NULL;

    Job *best = NULL;
    for (Job *j = shell->jobs->head; j; j = j->next) {
        if (!best || j->id > best->id)
            best = j;
    }
    return best;
}

void job_update_status(Shell *shell, pid_t pid, int status)
{
    if (!shell)
        return;

    Job *job = job_find_by_pid(shell, pid);
    if (!job)
        return;

    if (WIFSTOPPED(status)) {
        job->state = JOB_STOPPED;
        job->notified = false;
    } else if (WIFCONTINUED(status)) {
        job->state = JOB_RUNNING;
        job->notified = false;
    } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
        /* Mark this PID as finished by zeroing it.  When all PIDs are done
         * the job is complete. */
        for (int i = 0; i < job->npids; i++) {
            if (job->pids[i] == pid) {
                job->pids[i] = 0;
                break;
            }
        }

        /* Check if all processes in the pipeline are finished */
        bool all_done = true;
        for (int i = 0; i < job->npids; i++) {
            if (job->pids[i] != 0) {
                all_done = false;
                break;
            }
        }

        if (all_done) {
            if (WIFSIGNALED(status))
                job->state = JOB_KILLED;
            else
                job->state = JOB_DONE;
            job->notified = false;
        }
    }
}

int job_wait_foreground(Shell *shell, Job *job)
{
    if (!shell || !job)
        return -1;

    int status = 0;

    /* Give the terminal to the job's process group */
    if (shell->interactive)
        tcsetpgrp(STDIN_FILENO, job->pgid);

    /* Wait for the job to stop or finish */
    pid_t pid;
    while (job->state == JOB_RUNNING) {
        pid = waitpid(-job->pgid, &status, WUNTRACED);
        if (pid < 0) {
            if (errno == ECHILD)
                break;
            if (errno == EINTR)
                continue;
            break;
        }
        job_update_status(shell, pid, status);
    }

    /* Restore the shell to the foreground */
    if (shell->interactive)
        tcsetpgrp(STDIN_FILENO, shell->shell_pid);

    if (job->state == JOB_STOPPED) {
        fprintf(stderr, "\n[%d]+  Stopped                 %s\n",
                job->id, job->command);
        job->notified = true;
    }

    /* Extract exit code */
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return status;
}

int job_continue_foreground(Shell *shell, Job *job)
{
    if (!shell || !job)
        return -1;

    job->state = JOB_RUNNING;
    job->foreground = true;

    /* Send SIGCONT to the entire process group */
    if (kill(-job->pgid, SIGCONT) < 0) {
        perror("vsh: kill (SIGCONT)");
        return -1;
    }

    return job_wait_foreground(shell, job);
}

int job_continue_background(Shell *shell, Job *job)
{
    if (!shell || !job)
        return -1;

    job->state = JOB_RUNNING;
    job->foreground = false;

    if (kill(-job->pgid, SIGCONT) < 0) {
        perror("vsh: kill (SIGCONT)");
        return -1;
    }

    fprintf(stderr, "[%d] %s &\n", job->id, job->command);
    return 0;
}

void job_check_background(Shell *shell)
{
    if (!shell || !shell->jobs)
        return;

    Job *j = shell->jobs->head;
    while (j) {
        Job *next = j->next;  /* save — we may remove j */

        if ((j->state == JOB_DONE || j->state == JOB_KILLED) && !j->notified) {
            fprintf(stderr, "[%d]   %-24s%s\n",
                    j->id, job_state_str(j->state), j->command);
            job_remove(shell, j->id);
        }

        j = next;
    }
}

void job_list_print(Shell *shell)
{
    if (!shell || !shell->jobs)
        return;

    /* Print from lowest to highest job id.  The list is prepended so the
     * head has the highest id.  We do a simple O(n^2) walk to print in
     * order — job lists are tiny. */
    int max_id = 0;
    for (Job *j = shell->jobs->head; j; j = j->next) {
        if (j->id > max_id)
            max_id = j->id;
    }

    for (int id = 1; id <= max_id; id++) {
        Job *j = job_find_by_id(shell, id);
        if (!j)
            continue;

        /* Mark whether this is the current (+) or previous (-) job */
        char marker = ' ';
        Job *recent = job_most_recent(shell);
        if (j == recent)
            marker = '+';

        fprintf(stdout, "[%d]%c  %-24s%s\n",
                j->id, marker, job_state_str(j->state), j->command);
    }
}

void job_table_destroy(Shell *shell)
{
    if (!shell || !shell->jobs)
        return;

    Job *j = shell->jobs->head;
    while (j) {
        Job *next = j->next;

        /* Kill any still-running jobs */
        if (j->state == JOB_RUNNING || j->state == JOB_STOPPED) {
            kill(-j->pgid, SIGKILL);
            waitpid(-j->pgid, NULL, 0);
        }

        free(j->pids);
        free(j->command);
        free(j);
        j = next;
    }

    shell->jobs->head = NULL;
}
