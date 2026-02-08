/* ============================================================================
 * vsh - Vanguard Shell
 * pipeline.c - Pipe chain management
 *
 * Executes pipelines of the form `cmd1 | cmd2 | ... | cmdN` by creating
 * N-1 pipes, forking N children (each wired to the appropriate pipe ends),
 * and waiting for all of them.  The exit status of the pipeline is that of
 * the last command.  If the pipeline is negated (! prefix) the status is
 * inverted.
 *
 * Single-command "pipelines" are optimised by executing in-process so that
 * builtins like `cd` or `export` can modify shell state directly.
 * ============================================================================ */

#include "pipeline.h"
#include "executor.h"
#include "builtins.h"
#include "env.h"
#include "shell.h"
#include "job_control.h"
#include "parser.h"
#include "arena.h"
#include "wildcard.h"

#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Forward declarations ----------------------------------------------- */

static void child_reset_signals(void);
static void exec_pipeline_child(Shell *shell, ASTNode *node);

/* ---- Pipeline execution ------------------------------------------------- */

int pipeline_execute(Shell *shell, PipelineNode *pipeline)
{
    int n = pipeline->count;

    /* ---- Optimisation: single-command pipeline runs in-process ----------- */
    if (n == 1) {
        int status = executor_execute(shell, pipeline->commands[0]);
        if (pipeline->negated)
            status = (status == 0) ? 1 : 0;
        shell->last_status = status;
        return status;
    }

    /* ---- Multi-command pipeline ----------------------------------------- */

    /*
     * Allocate pipe fd pairs.  For N commands we need N-1 pipes.
     * pipes[i] connects command i's stdout to command i+1's stdin.
     *   pipes[i][0] = read end    (stdin of command i+1)
     *   pipes[i][1] = write end   (stdout of command i)
     */
    int (*pipes)[2] = malloc(sizeof(int[2]) * (n - 1));
    if (!pipes) {
        perror("vsh: malloc");
        return 1;
    }

    for (int i = 0; i < n - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("vsh: pipe");
            /* Close any pipes we already opened */
            for (int j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            free(pipes);
            return 1;
        }
    }

    /* Fork all child processes */
    pid_t *pids = malloc(sizeof(pid_t) * n);
    if (!pids) {
        perror("vsh: malloc");
        for (int i = 0; i < n - 1; i++) {
            close(pipes[i][0]);
            close(pipes[i][1]);
        }
        free(pipes);
        return 1;
    }

    pid_t pgid = 0; /* Process group id (set to first child's PID) */

    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("vsh: fork");
            /* Kill any children we already started */
            for (int j = 0; j < i; j++)
                kill(pids[j], SIGTERM);
            for (int j = 0; j < n - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            free(pids);
            free(pipes);
            return 1;
        }

        if (pid == 0) {
            /* ---- Child process ------------------------------------------ */

            /* Set process group */
            setpgid(0, pgid);

            /* Wire up pipe ends */
            if (i > 0) {
                /* stdin from previous pipe's read end */
                if (dup2(pipes[i - 1][0], STDIN_FILENO) < 0) {
                    perror("vsh: dup2");
                    _exit(1);
                }
            }
            if (i < n - 1) {
                /* stdout to current pipe's write end */
                if (dup2(pipes[i][1], STDOUT_FILENO) < 0) {
                    perror("vsh: dup2");
                    _exit(1);
                }
            }

            /* Close all pipe fds in the child -- they've been dup2'd */
            for (int j = 0; j < n - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            child_reset_signals();

            /* Execute the command */
            exec_pipeline_child(shell, pipeline->commands[i]);

            /* Should not reach here */
            _exit(127);
        }

        /* ---- Parent ----------------------------------------------------- */
        pids[i] = pid;
        if (i == 0) {
            pgid = pid; /* First child becomes the process group leader */
        }
        setpgid(pid, pgid);
    }

    /* ---- Parent: close all pipe fds ------------------------------------- */
    for (int i = 0; i < n - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    free(pipes);

    /* ---- Give the pipeline the terminal --------------------------------- */
    if (shell->interactive)
        tcsetpgrp(STDIN_FILENO, pgid);

    /* ---- Register job and wait ------------------------------------------ */
    /*
     * Build a command string for the job table by joining the first
     * argument of each pipeline command.
     */
    Job *job = job_add(shell, pgid, pids, n, "(pipeline)", true);

    int status = job_wait_foreground(shell, job);

    free(pids);

    /* ---- Handle negation ------------------------------------------------ */
    if (pipeline->negated)
        status = (status == 0) ? 1 : 0;

    shell->last_status = status;
    return status;
}

/* ---- Execute a single pipeline stage in a child process ----------------- */

static void exec_pipeline_child(Shell *shell, ASTNode *node)
{
    /*
     * If the node is a simple command, handle it directly so we can apply
     * redirections and call execvp.  For compound nodes (subshells, etc.)
     * we just run executor_execute and exit.
     */
    if (node->type == NODE_COMMAND) {
        CommandNode *cmd = &node->cmd;
        Arena *arena = shell->parse_arena;

        /* Apply command-local assignments */
        for (int i = 0; i < cmd->nassign; i++) {
            char *key   = NULL;
            char *value = NULL;
            if (env_parse_assignment(cmd->assignments[i], &key, &value, arena)) {
                char *exp_val = env_expand(shell, value, arena);
                env_set(shell->env, key, exp_val, true);
            }
        }

        /* Apply redirections */
        if (executor_apply_redirections(cmd->redirs) < 0)
            _exit(1);

        /* Expand arguments */
        int   cap  = cmd->argc > 4 ? cmd->argc * 2 : 8;
        int   argc = 0;
        char **argv = malloc(sizeof(char *) * cap);
        if (!argv)
            _exit(1);

        for (int i = 0; i < cmd->argc; i++) {
            char *expanded = env_expand(shell, cmd->argv[i], arena);
            if (expanded[0] == '~')
                expanded = env_expand_tilde(shell, expanded, arena);

            if (wildcard_has_magic(expanded)) {
                int   glob_count = 0;
                char **matches = wildcard_expand(expanded, arena, &glob_count);
                if (matches && glob_count > 0) {
                    for (int j = 0; j < glob_count; j++) {
                        if (argc >= cap) {
                            cap *= 2;
                            argv = realloc(argv, sizeof(char *) * cap);
                            if (!argv) _exit(1);
                        }
                        argv[argc++] = matches[j];
                    }
                    continue;
                }
            }

            if (argc >= cap) {
                cap *= 2;
                argv = realloc(argv, sizeof(char *) * cap);
                if (!argv) _exit(1);
            }
            argv[argc++] = expanded;
        }

        if (argc >= cap) {
            argv = realloc(argv, sizeof(char *) * (cap + 1));
            if (!argv) _exit(1);
        }
        argv[argc] = NULL;

        if (argc == 0)
            _exit(0);

        /*
         * Even builtins must run as subprocesses in a pipeline (they cannot
         * affect the parent shell's state from a pipe stage).  However we
         * can still execute them directly here since we're already in a
         * forked child.
         */
        if (builtins_is_builtin(argv[0])) {
            int status = builtins_execute(shell, argc, argv);
            _exit(status);
        }

        /* External command */
        char **envp = env_build_envp(shell->env);
        execve(argv[0], argv, envp);
        execvp(argv[0], argv);

        int err = errno;
        fprintf(stderr, "vsh: %s: %s\n", argv[0], strerror(err));
        env_free_envp(envp);
        _exit(err == ENOENT ? 127 : 126);
    }

    /* Non-command node (subshell, compound, etc.) -- just execute and exit */
    int status = executor_execute(shell, node);
    _exit(status);
}

/* ---- Helper: reset signals to defaults in child process ----------------- */

static void child_reset_signals(void)
{
    signal(SIGINT,  SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
}
