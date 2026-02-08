/* ============================================================================
 * vsh - Vanguard Shell
 * executor.c - Command execution engine
 *
 * Walks the AST produced by the parser and executes each node.  Simple
 * commands are dispatched to builtins or forked as external processes.
 * Compound constructs (if/while/for), logical operators (&&/||), sequences,
 * background jobs, subshells, pipelines, and function definitions are all
 * handled here or delegated to the appropriate subsystem.
 * ============================================================================ */

#include "executor.h"
#include "pipeline.h"
#include "builtins.h"
#include "env.h"
#include "shell.h"
#include "job_control.h"
#include "wildcard.h"
#include "arena.h"
#include "parser.h"

#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Forward declarations ----------------------------------------------- */

static int exec_command(Shell *shell, ASTNode *node);
static int exec_pipeline(Shell *shell, ASTNode *node);
static int exec_and(Shell *shell, ASTNode *node);
static int exec_or(Shell *shell, ASTNode *node);
static int exec_sequence(Shell *shell, ASTNode *node);
static int exec_background(Shell *shell, ASTNode *node);
static int exec_negate(Shell *shell, ASTNode *node);
static int exec_subshell(Shell *shell, ASTNode *node);
static int exec_if(Shell *shell, ASTNode *node);
static int exec_while(Shell *shell, ASTNode *node);
static int exec_for(Shell *shell, ASTNode *node);
static int exec_function(Shell *shell, ASTNode *node);
static int exec_block(Shell *shell, ASTNode *node);
static void child_reset_signals(void);

/* ---- Main dispatcher ---------------------------------------------------- */

int executor_execute(Shell *shell, ASTNode *node)
{
    if (!node)
        return 0;

    int status = 0;

    switch (node->type) {
    case NODE_COMMAND:    status = exec_command(shell, node);    break;
    case NODE_PIPELINE:   status = exec_pipeline(shell, node);   break;
    case NODE_AND:        status = exec_and(shell, node);        break;
    case NODE_OR:         status = exec_or(shell, node);         break;
    case NODE_SEQUENCE:   status = exec_sequence(shell, node);   break;
    case NODE_BACKGROUND: status = exec_background(shell, node); break;
    case NODE_NEGATE:     status = exec_negate(shell, node);     break;
    case NODE_SUBSHELL:   status = exec_subshell(shell, node);   break;
    case NODE_IF:         status = exec_if(shell, node);         break;
    case NODE_WHILE:      status = exec_while(shell, node);      break;
    case NODE_FOR:        status = exec_for(shell, node);        break;
    case NODE_FUNCTION:   status = exec_function(shell, node);   break;
    case NODE_BLOCK:      status = exec_block(shell, node);      break;
    }

    shell->last_status = status;
    return status;
}

/* ---- Simple command execution ------------------------------------------- */

/*
 * Expand a single word: variable expansion, tilde expansion, wildcard
 * expansion.  Appends results to *out_argv / *out_argc (may produce multiple
 * words due to glob expansion).  All strings are arena-allocated.
 */
static void expand_word(Shell *shell, const char *word, Arena *arena,
                        char ***out_argv, int *out_argc, int *out_cap)
{
    /* Variable expansion */
    char *expanded = env_expand(shell, word, arena);

    /* Tilde expansion (only when the word starts with ~) */
    if (expanded[0] == '~')
        expanded = env_expand_tilde(shell, expanded, arena);

    /* Wildcard / glob expansion */
    if (wildcard_has_magic(expanded)) {
        int   glob_count = 0;
        char **matches = wildcard_expand(expanded, arena, &glob_count);
        if (matches && glob_count > 0) {
            for (int i = 0; i < glob_count; i++) {
                if (*out_argc >= *out_cap) {
                    *out_cap *= 2;
                    char **tmp = arena_alloc(arena, sizeof(char *) * (*out_cap));
                    memcpy(tmp, *out_argv, sizeof(char *) * (*out_argc));
                    *out_argv = tmp;
                }
                (*out_argv)[(*out_argc)++] = matches[i];
            }
            return;
        }
        /* No matches -- fall through and use the literal pattern */
    }

    /* Single word -- append it */
    if (*out_argc >= *out_cap) {
        *out_cap *= 2;
        char **tmp = arena_alloc(arena, sizeof(char *) * (*out_cap));
        memcpy(tmp, *out_argv, sizeof(char *) * (*out_argc));
        *out_argv = tmp;
    }
    (*out_argv)[(*out_argc)++] = expanded;
}

int executor_exec_command(Shell *shell, CommandNode *cmd)
{
    Arena *arena = shell->parse_arena;

    /* ---- Handle bare assignments (no command name) ---------------------- */
    if (cmd->argc == 0 && cmd->nassign > 0) {
        for (int i = 0; i < cmd->nassign; i++) {
            char *key   = NULL;
            char *value = NULL;
            if (env_parse_assignment(cmd->assignments[i], &key, &value, arena)) {
                char *exp_val = env_expand(shell, value, arena);
                env_set(shell->env, key, exp_val, false);
            }
        }
        shell->last_status = 0;
        return 0;
    }

    /* ---- Expand all arguments ------------------------------------------- */
    int   cap  = cmd->argc > 4 ? cmd->argc * 2 : 8;
    int   argc = 0;
    char **argv = arena_alloc(arena, sizeof(char *) * cap);

    for (int i = 0; i < cmd->argc; i++)
        expand_word(shell, cmd->argv[i], arena, &argv, &argc, &cap);

    /* Null-terminate the argv array */
    if (argc >= cap) {
        cap++;
        char **tmp = arena_alloc(arena, sizeof(char *) * cap);
        memcpy(tmp, argv, sizeof(char *) * argc);
        argv = tmp;
    }
    argv[argc] = NULL;

    if (argc == 0) {
        shell->last_status = 0;
        return 0;
    }

    /* ---- Builtin? ------------------------------------------------------- */
    if (builtins_is_builtin(argv[0])) {
        /* Apply command-local variable assignments to a temporary env */
        /* (simplified: we skip per-command env overrides for builtins) */
        int status = builtins_execute(shell, argc, argv);
        shell->last_status = status;
        return status;
    }

    /* ---- External command: fork & exec ---------------------------------- */
    pid_t pid = fork();
    if (pid < 0) {
        perror("vsh: fork");
        shell->last_status = 1;
        return 1;
    }

    if (pid == 0) {
        /* ---- Child process ---------------------------------------------- */
        /* New process group */
        setpgid(0, 0);
        if (shell->interactive)
            tcsetpgrp(STDIN_FILENO, getpid());

        child_reset_signals();

        /* Apply command-local variable assignments to environment */
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

        /* Build envp and exec */
        char **envp = env_build_envp(shell->env);
        execve(argv[0], argv, envp);

        /* execve with bare name failed -- try PATH lookup via execvp */
        execvp(argv[0], argv);

        /* If we get here, exec failed */
        int err = errno;
        fprintf(stderr, "vsh: %s: %s\n", argv[0], strerror(err));
        env_free_envp(envp);
        _exit(err == ENOENT ? 127 : 126);
    }

    /* ---- Parent process ------------------------------------------------- */
    setpgid(pid, pid);

    Job *job = job_add(shell, pid, &pid, 1, argv[0], true);
    int status = job_wait_foreground(shell, job);

    shell->last_status = status;
    return status;
}

/* ---- Wrapper for the dispatcher ----------------------------------------- */

static int exec_command(Shell *shell, ASTNode *node)
{
    return executor_exec_command(shell, &node->cmd);
}

/* ---- Pipeline ----------------------------------------------------------- */

static int exec_pipeline(Shell *shell, ASTNode *node)
{
    return pipeline_execute(shell, &node->pipeline);
}

/* ---- Logical AND (&&) --------------------------------------------------- */

static int exec_and(Shell *shell, ASTNode *node)
{
    int status = executor_execute(shell, node->binary.left);
    if (status == 0)
        status = executor_execute(shell, node->binary.right);
    return status;
}

/* ---- Logical OR (||) ---------------------------------------------------- */

static int exec_or(Shell *shell, ASTNode *node)
{
    int status = executor_execute(shell, node->binary.left);
    if (status != 0)
        status = executor_execute(shell, node->binary.right);
    return status;
}

/* ---- Sequence (;) ------------------------------------------------------- */

static int exec_sequence(Shell *shell, ASTNode *node)
{
    executor_execute(shell, node->binary.left);
    return executor_execute(shell, node->binary.right);
}

/* ---- Background (&) ----------------------------------------------------- */

static int exec_background(Shell *shell, ASTNode *node)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("vsh: fork");
        return 1;
    }

    if (pid == 0) {
        /* Child: new process group, execute the node */
        setpgid(0, 0);
        child_reset_signals();
        int status = executor_execute(shell, node->child);
        _exit(status);
    }

    /* Parent: register background job */
    setpgid(pid, pid);
    Job *job = job_add(shell, pid, &pid, 1, "(background)", false);
    if (job)
        fprintf(stderr, "[%d] %d\n", job->id, (int)pid);

    return 0;
}

/* ---- Negate (!) --------------------------------------------------------- */

static int exec_negate(Shell *shell, ASTNode *node)
{
    int status = executor_execute(shell, node->child);
    return status == 0 ? 1 : 0;
}

/* ---- Subshell ( ... ) --------------------------------------------------- */

static int exec_subshell(Shell *shell, ASTNode *node)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("vsh: fork");
        return 1;
    }

    if (pid == 0) {
        /* Child: new process group, execute, then exit */
        setpgid(0, 0);
        if (shell->interactive)
            tcsetpgrp(STDIN_FILENO, getpid());
        child_reset_signals();
        int status = executor_execute(shell, node->child);
        _exit(status);
    }

    /* Parent: wait for the subshell */
    setpgid(pid, pid);
    Job *job = job_add(shell, pid, &pid, 1, "(subshell)", true);
    return job_wait_foreground(shell, job);
}

/* ---- If / elif / else --------------------------------------------------- */

static int exec_if(Shell *shell, ASTNode *node)
{
    IfNode *ifn = &node->if_node;

    int cond = executor_execute(shell, ifn->condition);
    if (cond == 0)
        return executor_execute(shell, ifn->then_body);

    if (ifn->else_body)
        return executor_execute(shell, ifn->else_body);

    return 0;
}

/* ---- While loop --------------------------------------------------------- */

static int exec_while(Shell *shell, ASTNode *node)
{
    WhileNode *wn = &node->while_node;
    int status = 0;

    while (executor_execute(shell, wn->condition) == 0)
        status = executor_execute(shell, wn->body);

    return status;
}

/* ---- For loop ----------------------------------------------------------- */

static int exec_for(Shell *shell, ASTNode *node)
{
    ForNode *fn = &node->for_node;
    Arena   *arena = shell->parse_arena;
    int      status = 0;

    for (int i = 0; i < fn->nwords; i++) {
        /* Expand each word */
        char *word = env_expand(shell, fn->words[i], arena);
        if (word[0] == '~')
            word = env_expand_tilde(shell, word, arena);

        /* Wildcard expansion on the word */
        if (wildcard_has_magic(word)) {
            int   glob_count = 0;
            char **matches = wildcard_expand(word, arena, &glob_count);
            if (matches && glob_count > 0) {
                for (int j = 0; j < glob_count; j++) {
                    env_set(shell->env, fn->varname, matches[j], false);
                    status = executor_execute(shell, fn->body);
                }
                continue;
            }
        }

        env_set(shell->env, fn->varname, word, false);
        status = executor_execute(shell, fn->body);
    }

    return status;
}

/* ---- Function definition ------------------------------------------------ */

static int exec_function(Shell *shell, ASTNode *node)
{
    /*
     * Store the function body AST in the environment as a special variable.
     * The convention is "func:<name>" → pointer-as-string.  This is a
     * simplified approach; a proper implementation would use a dedicated
     * function table.  We store the raw pointer value so we can retrieve it
     * at call time.
     */
    FunctionNode *fn = &node->func;
    char key[256];
    snprintf(key, sizeof(key), "func:%s", fn->name);

    /*
     * Encode the ASTNode pointer as a hex string.  The body AST lives in
     * the parse arena which is only valid for the current top-level command,
     * so in practice functions should be defined via rc files that persist
     * the arena.  For now this is a minimal placeholder implementation.
     */
    char ptr_buf[32];
    snprintf(ptr_buf, sizeof(ptr_buf), "%p", (void *)fn->body);
    env_set(shell->env, key, ptr_buf, false);

    return 0;
}

/* ---- Block { ... } ------------------------------------------------------ */

static int exec_block(Shell *shell, ASTNode *node)
{
    return executor_execute(shell, node->child);
}

/* ---- Redirections ------------------------------------------------------- */

int executor_apply_redirections(Redirection *redirs)
{
    for (Redirection *r = redirs; r; r = r->next) {
        int fd  = r->fd;
        int src = -1;

        switch (r->type) {
        case REDIR_INPUT:
            if (fd < 0) fd = STDIN_FILENO;
            src = open(r->target, O_RDONLY);
            if (src < 0) {
                fprintf(stderr, "vsh: %s: %s\n", r->target, strerror(errno));
                return -1;
            }
            if (dup2(src, fd) < 0) {
                perror("vsh: dup2");
                close(src);
                return -1;
            }
            close(src);
            break;

        case REDIR_OUTPUT:
            if (fd < 0) fd = STDOUT_FILENO;
            src = open(r->target, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (src < 0) {
                fprintf(stderr, "vsh: %s: %s\n", r->target, strerror(errno));
                return -1;
            }
            if (dup2(src, fd) < 0) {
                perror("vsh: dup2");
                close(src);
                return -1;
            }
            close(src);
            break;

        case REDIR_APPEND:
            if (fd < 0) fd = STDOUT_FILENO;
            src = open(r->target, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (src < 0) {
                fprintf(stderr, "vsh: %s: %s\n", r->target, strerror(errno));
                return -1;
            }
            if (dup2(src, fd) < 0) {
                perror("vsh: dup2");
                close(src);
                return -1;
            }
            close(src);
            break;

        case REDIR_DUP_OUT:
            if (fd < 0) fd = STDOUT_FILENO;
            if (dup2(atoi(r->target), fd) < 0) {
                perror("vsh: dup2");
                return -1;
            }
            break;

        case REDIR_DUP_IN:
            if (fd < 0) fd = STDIN_FILENO;
            if (dup2(atoi(r->target), fd) < 0) {
                perror("vsh: dup2");
                return -1;
            }
            break;

        case REDIR_HEREDOC:
            fprintf(stderr, "vsh: heredoc: not yet implemented\n");
            break;
        }
    }

    return 0;
}

/* ---- Placeholder for redirection restore (used in parent after fork) ---- */

void executor_restore_redirections(void)
{
    /*
     * In the current architecture redirections are only applied in child
     * processes (post-fork), so the parent never needs to restore them.
     * This stub exists for future use when builtins apply redirections
     * in-process and need to undo them afterwards.
     */
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
